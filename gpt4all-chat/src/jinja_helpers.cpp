#include "jinja_helpers.h"

#include "utils.h"

#include <fmt/format.h>

#include <QString>
#include <QUrl>
#include <QtGlobal>

#include <memory>
#include <vector>

using namespace std::literals::string_view_literals;


JinjaResultInfo::~JinjaResultInfo() = default;

const JinjaFieldMap<ResultInfo> JinjaResultInfo::s_fields = {
    { "collection", [](auto &s) { return s.collection.toStdString(); } },
    { "path",       [](auto &s) { return s.path      .toStdString(); } },
    { "file",       [](auto &s) { return s.file      .toStdString(); } },
    { "title",      [](auto &s) { return s.title     .toStdString(); } },
    { "author",     [](auto &s) { return s.author    .toStdString(); } },
    { "date",       [](auto &s) { return s.date      .toStdString(); } },
    { "text",       [](auto &s) { return s.text      .toStdString(); } },
    { "page",       [](auto &s) { return s.page;                     } },
    { "fileUri",    [](auto &s) { return s.fileUri() .toStdString(); } },
};

JinjaPromptAttachment::~JinjaPromptAttachment() = default;

const JinjaFieldMap<PromptAttachment> JinjaPromptAttachment::s_fields = {
    { "url",              [](auto &s) { return s.url.toString()    .toStdString(); } },
    { "file",             [](auto &s) { return s.file()            .toStdString(); } },
    { "processedContent", [](auto &s) { return s.processedContent().toStdString(); } },
};

std::vector<std::string> JinjaMessage::GetKeys() const
{
    std::vector<std::string> result;
    auto &keys = this->keys();
    result.reserve(keys.size());
    result.assign(keys.begin(), keys.end());
    return result;
}

auto JinjaMessage::keys() const -> const std::unordered_set<std::string_view> &
{
    static const std::unordered_set<std::string_view> baseKeys
        { "role", "content" };
    static const std::unordered_set<std::string_view> userKeys
        { "role", "content", "sources", "prompt_attachments" };
    switch (m_item->type()) {
        using enum ChatItem::Type;
    case System:
    case Response:
        return baseKeys;
    case Prompt:
        return userKeys;
    }
    Q_UNREACHABLE();
}

bool operator==(const JinjaMessage &a, const JinjaMessage &b)
{
    if (a.m_item == b.m_item)
        return true;
    const auto &[ia, ib] = std::tie(*a.m_item, *b.m_item);
    auto type = ia.type();
    if (type != ib.type() || ia.value != ib.value)
        return false;

    switch (type) {
        using enum ChatItem::Type;
    case System:
    case Response:
        return true;
    case Prompt:
        return ia.sources == ib.sources && ia.promptAttachments == ib.promptAttachments;
    }
    Q_UNREACHABLE();
}

const JinjaFieldMap<ChatItem> JinjaMessage::s_fields = {
    { "role", [](auto &i) {
        switch (i.type()) {
            using enum ChatItem::Type;
            case System:   return "system"sv;
            case Prompt:   return "user"sv;
            case Response: return "assistant"sv;
        }
        Q_UNREACHABLE();
    } },
    { "content", [](auto &i) { return i.value.toStdString(); } },
    { "sources", [](auto &i) {
        auto sources = i.sources | views::transform([](auto &r) {
            return jinja2::GenericMap([map = std::make_shared<JinjaResultInfo>(r)] { return map.get(); });
        });
        return jinja2::ValuesList(sources.begin(), sources.end());
    } },
    { "prompt_attachments", [](auto &i) {
        auto attachments = i.promptAttachments | views::transform([](auto &pa) {
            return jinja2::GenericMap([map = std::make_shared<JinjaPromptAttachment>(pa)] { return map.get(); });
        });
        return jinja2::ValuesList(attachments.begin(), attachments.end());
    } },
};
