#include "llmodel.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ranges = std::ranges;
namespace views  = std::ranges::views;

void LLModel::prompt(
    std::string_view        prompt,
    const PromptCallback   &promptCallback,
    const ResponseCallback &responseCallback,
    const PromptContext    &promptCtx,
    bool                    allowContextShift
) {
    if (!isModelLoaded())
        throw std::invalid_argument("Attempted to prompt an unloaded model.");
    if (!supportsCompletion())
        throw std::invalid_argument("Not a text completion model.");
    if (!promptCtx.n_batch)
        throw std::invalid_argument("Batch size cannot be zero.");
    if (!promptCtx.n_predict)
        return; // nothing requested

    auto embd_inp = tokenize(prompt);
    if (embd_inp.empty())
        throw std::invalid_argument("Prompt tokenized to zero tokens.");

    if (auto res = decodePrompt(promptCallback, allowContextShift, promptCtx, embd_inp))
        generateResponse(responseCallback, allowContextShift, promptCtx, /*n_past*/ *res);
}

auto LLModel::decodePrompt(
    const PromptCallback     &promptCallback,
    bool                      allowContextShift,
    const PromptContext      &promptCtx,
    const std::vector<Token> &embd_inp
) -> std::optional<int32_t>
{
    (void)allowContextShift;
    assert(!embd_inp.empty());

    // FIXME(Adam): We should find a way to bubble these strings to the UI level to allow for translation
    if (auto limit = contextLength() - promptCtx.n_min_predict; int32_t(embd_inp.size()) > limit) {
        std::ostringstream ss;
        ss << "Your message was too long and could not be processed (" << embd_inp.size() << " > " << limit
           << "). Please try again with something shorter.";
        throw std::runtime_error(ss.str());
    }

    int32_t n_batch = std::min(promptCtx.n_batch, LLMODEL_MAX_PROMPT_BATCH);

    // Find the greatest n_past where the beginning of embd_inp matches the end of the token cache, starting at the
    // requested n_past.
    // This is used to skip unnecessary work when the prompt shares a common prefix with the previous result.
    auto embd_inp_start = computeModelInputPosition(embd_inp);
    auto nPast = int32_t(embd_inp_start - embd_inp.begin());

    // always decode up to a full batch before generating, even if cached
    nPast -= std::min(n_batch, nPast);

    setModelInputPosition(nPast);

    // execute the callback even for skipped tokens
    if (!promptCallback(embd_inp | views::take(nPast), true))
        return std::nullopt;

    // process the prompt in batches
    for (int32_t i = nPast; i < embd_inp.size();) {
        auto batch_end = std::min(i + n_batch, int32_t(embd_inp.size()));
        std::span<const Token> batch(embd_inp.begin() + i, embd_inp.begin() + batch_end);

        // Check if the context has run out...
        if (nPast + int32_t(batch.size()) > contextLength()) {
            assert(allowContextShift);
            shiftContext(promptCtx, &nPast);
            assert(nPast + int32_t(batch.size()) <= contextLength());
        }

        if (!evalTokens(nPast, batch))
            throw std::runtime_error("An internal error was encountered during prompt processing.");

        for (auto &tok : batch) {
            appendInputToken(tok);
            nPast++;
            if (!promptCallback({ &tok, 1 }, false))
                return std::nullopt;
        }
        i = batch_end;
    }

    return nPast;
}

/*
 * If string s overlaps with the string key such that some prefix of the key is at the end
 * of the string, return the position in s where the first match starts. Otherwise, return
 * std::string::npos. Examples:
 * s = "bfo",  key = "foo" -> 1
 * s = "fooa", key = "foo" -> npos
 */
static std::string::size_type stringsOverlap(const std::string &s, const std::string &key)
{
    if (s.empty() || key.empty())
        throw std::invalid_argument("arguments to stringsOverlap must not be empty");

    for (int start = std::max(0, int(s.size()) - int(key.size())); start < s.size(); start++) {
        if (s.compare(start, s.size(), key, 0, s.size() - start) == 0)
            return start;
    }
    return std::string::npos;
}

void LLModel::generateResponse(
    const ResponseCallback &responseCallback,
    bool                    allowContextShift,
    const PromptContext    &promptCtx,
    int32_t                 nPast
) {
    assert(allowContextShift || nPast < contextLength());

    static const char *stopSequences[] {
        "### Instruction", "### Prompt", "### Response", "### Human", "### Assistant", "### Context",
    };

    initSampler(promptCtx);

    std::string cachedResponse;
    std::vector<Token> cachedTokens;
    int n_predicted = 0;

    // Predict next tokens
    for (bool stop = false; !stop;) {
        // Sample next token
        std::optional<Token> new_tok = sampleToken();
        std::string new_piece = tokenToString(new_tok.value());
        cachedTokens.push_back(new_tok.value());
        cachedResponse += new_piece;

        auto accept = [this, &promptCtx, &new_tok, &nPast, allowContextShift] {
            // Shift context if out of space
            if (nPast >= contextLength()) {
                (void)allowContextShift;
                assert(allowContextShift);
                shiftContext(promptCtx, &nPast);
                assert(nPast < contextLength());
            }

            // Accept the token
            Token tok = std::exchange(new_tok, std::nullopt).value();
            if (!evalTokens(nPast, { &tok, 1 }))
                throw std::runtime_error("An internal error was encountered during response generation.");

            appendInputToken(tok);
            nPast++;
        };

        // Check for EOS
        auto lengthLimit = std::string::npos;
        for (const auto token : endTokens()) {
            if (new_tok == token) {
                stop = true;
                lengthLimit = cachedResponse.size() - new_piece.size();
            }
        }

        if (lengthLimit != std::string::npos) {
            // EOS matched
        } else if (!isSpecialToken(new_tok.value())) {
            // Check if the response contains a stop sequence
            for (const auto &p : stopSequences) {
                auto match = cachedResponse.find(p);
                if (match != std::string::npos) stop = true;
                lengthLimit = std::min(lengthLimit, match);
                if (match == 0) break;
            }

            // Check if the response matches the start of a stop sequence
            if (lengthLimit == std::string::npos) {
                for (const auto &p : stopSequences) {
                    auto match = stringsOverlap(cachedResponse, p);
                    lengthLimit = std::min(lengthLimit, match);
                    if (match == 0) break;
                }
            }
        } else if (ranges::find(stopSequences, new_piece) < std::end(stopSequences)) {
            // Special tokens must exactly match a stop sequence
            stop = true;
            lengthLimit = cachedResponse.size() - new_piece.size();
        }

        // Optionally stop if the context will run out
        if (!allowContextShift && nPast + cachedTokens.size() >= contextLength()) {
            std::cerr << "LLModel Warning: Not enough space, n_past=" << nPast << ", n_ctx=" << contextLength() << "\n";
            stop = true;
        }

        // Empty the cache, up to the length limit
        std::string::size_type responseLength = 0;
        while (!cachedTokens.empty()) {
            Token tok = cachedTokens.front();
            std::string piece = tokenToString(tok);

            // Stop if the piece (or part of it) does not fit within the length limit
            if (responseLength + (stop ? 1 : piece.size()) > lengthLimit)
                break;

            // Remove token from cache
            assert(cachedResponse.starts_with(piece));
            cachedTokens.erase(cachedTokens.begin(), cachedTokens.begin() + 1);
            cachedResponse.erase(cachedResponse.begin(), cachedResponse.begin() + piece.size());

            // Accept the token, if needed (not cached)
            if (cachedTokens.empty() && new_tok)
                accept();

            // Send the token
            if (!responseCallback(tok, piece) || ++n_predicted >= promptCtx.n_predict) {
                stop = true;
                break;
            }

            // FIXME(jared): we could avoid printing partial stop sequences if we didn't have to
            // output token IDs and could cache a partial token for the next prompt call
            responseLength += piece.size();
        }
        assert(cachedTokens.empty() == cachedResponse.empty());

        // Accept the token, if needed (in cache)
        if (new_tok) {
            assert(!cachedTokens.empty() && cachedTokens.back() == new_tok);
            if (stop) {
                cachedTokens.pop_back();
            } else {
                accept();
            }
        }
    }

    if (inputLength() < cachedTokens.size()) {
        /* This is theoretically possible if the longest stop sequence is greater than
         * n_ctx * contextErase tokens. */
        throw std::runtime_error("shifted too much context, can't go back");
    }

#ifndef NDEBUG
    auto inp = inputTokens();
    auto discard_start = inp.end() - cachedTokens.size();
    assert(std::equal(discard_start, inp.end(), cachedTokens.begin()));
#endif
}

void LLModel::embed(
    std::span<const std::string> texts, float *embeddings, std::optional<std::string> prefix, int dimensionality,
    size_t *tokenCount, bool doMean, bool atlas, EmbedCancelCallback *cancelCb
) {
    (void)texts;
    (void)embeddings;
    (void)prefix;
    (void)dimensionality;
    (void)tokenCount;
    (void)doMean;
    (void)atlas;
    (void)cancelCb;
    throw std::logic_error(std::string(implementation().modelType()) + " does not support embeddings");
}

void LLModel::embed(
    std::span<const std::string> texts, float *embeddings, bool isRetrieval, int dimensionality, size_t *tokenCount,
    bool doMean, bool atlas
) {
    (void)texts;
    (void)embeddings;
    (void)isRetrieval;
    (void)dimensionality;
    (void)tokenCount;
    (void)doMean;
    (void)atlas;
    throw std::logic_error(std::string(implementation().modelType()) + " does not support embeddings");
}
