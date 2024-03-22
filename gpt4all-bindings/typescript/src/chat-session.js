const { DEFAULT_PROMPT_CONTEXT } = require("./config");
const { prepareMessagesForIngest } = require("./util");

class ChatSession {
    model;
    modelName;
    /**
     * @type {unknown[]}
     */
    messages;
    /**
     * @type {string}
     */
    systemPrompt;
    /**
     * @type {import('./gpt4all').LLModelPromptContext}
     */
    promptContext;
    /**
     * @type {boolean}
     */
    initialized;

    constructor(model, opts = {}) {
        const { messages, systemPrompt, ...otherOpts } = opts;
        this.model = model;
        this.modelName = model.llm.name();
        this.messages = messages ?? [];
        this.systemPrompt = systemPrompt ?? model.config.systemPrompt;
        this.initialized = false;
        this.promptContext = {
            ...DEFAULT_PROMPT_CONTEXT,
            ...otherOpts,
            nPast: 0,
        };
    }

    async initialize() {
        if (this.model.activeChatSession !== this) {
            this.model.activeChatSession = this;
        }

        let tokensIngested = 0;

        // ingest system prompt

        if (this.systemPrompt) {
            const res = await this.model.generate(this.systemPrompt, {
                promptTemplate: "%1",
                nPredict: 0,
                special: true,
                nBatch: this.promptContext.nBatch,
                // verbose: true,
            });
            tokensIngested += res.tokensIngested;
            this.promptContext.nPast = res.nPast;
        }

        // ingest initial messages
        if (this.messages.length > 0) {
            tokensIngested += await this.ingestMessages(this.messages);
        }

        this.initialized = true;

        return tokensIngested;
    }

    async ingestMessages(messages) {
        const turns = prepareMessagesForIngest(messages);

        // send the message pairs to the model
        let tokensIngested = 0;

        for (const turn of turns) {
            const res = await this.model.generate(turn.user, {
                ...this.promptContext,
                fakeReply: turn.assistant,
            });
            tokensIngested += res.tokensIngested;
            this.promptContext.nPast = res.nPast;
        }
        return tokensIngested;
    }

    async generate(input, options = DEFAULT_PROMPT_CONTEXT, callback) {
        if (this.model.activeChatSession !== this) {
            throw new Error(
                "Chat session is not active. Create a new chat session or call initialize to continue."
            );
        }
        let tokensIngested = 0;

        if (!this.initialized) {
            tokensIngested += await this.initialize();
        }

        let prompt = input;

        if (Array.isArray(input)) {
            // assuming input is a messages array
            // -> tailing user message will be used as the final prompt. its optional.
            // -> all system messages will be ignored.
            // -> all other messages will be ingested with fakeReply
            // -> user/assistant messages will be pushed into the messages array

            let tailingUserMessage = "";
            let messagesToIngest = input;

            const lastMessage = input[input.length - 1];
            if (lastMessage.role === "user") {
                tailingUserMessage = lastMessage.content;
                messagesToIngest = input.slice(0, input.length - 1);
            }

            if (messagesToIngest.length > 0) {
                tokensIngested += await this.ingestMessages(messagesToIngest);
                this.messages.push(...messagesToIngest);
            }

            if (tailingUserMessage) {
                prompt = tailingUserMessage;
            } else {
                return {
                    text: "",
                    nPast: this.promptContext.nPast,
                    tokensIngested,
                    tokensGenerated: 0,
                };
            }
        }

        const response = await this.model.generate(
            prompt,
            {
                ...this.promptContext,
                ...options,
            },
            callback
        );

        this.promptContext.nPast = response.nPast;
        response.tokensIngested += tokensIngested;

        this.messages.push({
            role: "user",
            content: prompt,
        });
        this.messages.push({
            role: "assistant",
            content: response.text,
        });

        return response;
    }
}

module.exports = {
    ChatSession,
};
