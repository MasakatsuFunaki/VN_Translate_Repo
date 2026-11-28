// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "storage/script_repository.h"

#include "common/utf8_text.h"
#include "domain/vn_line_syntax.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
constexpr std::string_view kScriptOutputDir = "script_output";
constexpr std::string_view kTranslatedJsonFile = "translated_text.json";
constexpr std::string_view kTonePrimaryFile = "TONE_OF_NOVEL.md";
constexpr std::string_view kToneFallbackFile = "TONE.md";
constexpr std::string_view kCachePrefix = "translation_cache_";
constexpr std::string_view kJsonSuffix = ".json";

constexpr unsigned char kMinPrintableByte = 0x20U;
constexpr unsigned char kAsciiLimitByte = 0x80U;
constexpr char32_t kPuaFirst = 0xE000U;
constexpr char32_t kPuaLast = 0xF8FFU;
constexpr char32_t kHalfwidthCjkPunctFirst = 0xFF61U;
constexpr char32_t kHalfwidthKatakanaLast = 0xFF9FU;

constexpr std::size_t kMaxStandaloneSpeakerCodepoints = 20;
constexpr std::size_t kMaxCrlfSpeakerBytes = 35;
constexpr std::size_t kMaxCrlfSpeakerCodepoints = 20;
constexpr std::size_t kMaxInlineSpeakerBytes = 45;
constexpr std::size_t kMaxInlineSpeakerCodepoints = 25;
constexpr std::size_t kUtf8CornerBracketBytes = 3;
constexpr std::string_view kSoftWrapEscape = "\\n";
constexpr std::string_view kPageContinueEscape = "\\@";
constexpr std::string_view kNarrationSpeaker = "NARRATION";

std::string_view strip_utf8_bom(std::string_view text) noexcept {
    if (text.size() >= kUtf8Bom.size() &&
        text.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        text.remove_prefix(kUtf8Bom.size());
    }
    return text;
}

std::string path_to_utf8(const fs::path& path) {
    return wide_to_utf8(path.wstring());
}

std::string json_string_or_empty(const bj::object& obj, const char* key) {
    const auto* val = obj.if_contains(key);
    if (val != nullptr && val->is_string()) {
        return std::string(val->as_string());
    }
    return {};
}

std::pair<std::string, std::string> extract_jp_field(const bj::object& obj) {
    std::string fallback_key;
    for (const char* key : {"text", "content", "jp"}) {
        const auto* val = obj.if_contains(key);
        if (val != nullptr && val->is_string()) {
            std::string str(val->as_string());
            if (!str.empty()) {
                return {key, std::move(str)};
            }
            if (fallback_key.empty()) {
                fallback_key = key;
            }
        }
    }
    if (fallback_key.empty()) {
        fallback_key = "text";
    }
    return {std::move(fallback_key), {}};
}

std::string extract_en_field(const bj::object& obj) {
    for (const char* key : {"translated", "translation", "en"}) {
        const auto* val = obj.if_contains(key);
        if (val != nullptr && val->is_string()) {
            std::string str(val->as_string());
            if (!str.empty()) {
                return str;
            }
        }
    }
    return {};
}

const bj::object* resolve_scripts_map(const bj::object& root_obj) noexcept {
    const auto* scripts_val = root_obj.if_contains("scripts");
    if (scripts_val != nullptr && scripts_val->is_object()) {
        const auto& inner = scripts_val->as_object();
        const auto* s = inner.if_contains("strings");
        const auto* l = inner.if_contains("lines");
        const bool is_single_script =
            (s != nullptr && s->is_array()) || (l != nullptr && l->is_array());
        if (!is_single_script) {
            return &inner;
        }
    }
    return &root_obj;
}

bj::object* resolve_scripts_map_mut(bj::object& root_obj) noexcept {
    auto* scripts_val = root_obj.if_contains("scripts");
    if (scripts_val != nullptr && scripts_val->is_object()) {
        auto& inner = scripts_val->as_object();
        const auto* s = inner.if_contains("strings");
        const auto* l = inner.if_contains("lines");
        const bool is_single_script =
            (s != nullptr && s->is_array()) || (l != nullptr && l->is_array());
        if (!is_single_script) {
            return &inner;
        }
    }
    return &root_obj;
}

std::pair<std::string, const bj::array*> resolve_entry_array(
    const bj::value& script_val) noexcept {
    if (script_val.is_array()) {
        return {"", &script_val.as_array()};
    }
    if (!script_val.is_object()) {
        return {{}, nullptr};
    }
    const auto& obj = script_val.as_object();
    if (const auto* s = obj.if_contains("strings");
        s != nullptr && s->is_array()) {
        return {"strings", &s->as_array()};
    }
    if (const auto* l = obj.if_contains("lines");
        l != nullptr && l->is_array()) {
        return {"lines", &l->as_array()};
    }
    return {{}, nullptr};
}

bj::array* resolve_entry_array_mut(bj::value& script_val) noexcept {
    if (script_val.is_array()) {
        return &script_val.as_array();
    }
    if (!script_val.is_object()) {
        return nullptr;
    }
    auto& obj = script_val.as_object();
    if (auto* s = obj.if_contains("strings"); s != nullptr && s->is_array()) {
        return &s->as_array();
    }
    if (auto* l = obj.if_contains("lines"); l != nullptr && l->is_array()) {
        return &l->as_array();
    }
    return nullptr;
}

bool has_binary_or_pua_noise(std::string_view text) noexcept {
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < kMinPrintableByte && ch != '\r' && ch != '\n' && ch != '\t') {
            return true;
        }
    }
    std::size_t idx = 0;
    while (idx < text.size()) {
        const char32_t cp = utf8_next(text, idx);
        if (cp >= kPuaFirst && cp <= kPuaLast) {
            return true;
        }
    }
    return false;
}

bool has_non_ascii_bytes(std::string_view text) noexcept {
    for (const char ch : text) {
        if (static_cast<unsigned char>(ch) >= kAsciiLimitByte) {
            return true;
        }
    }
    return false;
}

bool is_excluded_entry_type(std::string_view type) noexcept {
    return type == "COMMAND" || type == "INPUT" || type == "other" ||
           type == "label" || type == "name" || type == "NAME";
}

bool is_standalone_speaker_tag(std::string_view jp,
                               std::string_view en) noexcept {
    if (has_binary_or_pua_noise(jp) || has_binary_or_pua_noise(en)) {
        return false;
    }
    if (jp.find('\r') != std::string_view::npos ||
        jp.find('\n') != std::string_view::npos) {
        return false;
    }

    std::size_t idx = 0;
    while (idx < jp.size()) {
        const char32_t cp = utf8_next(jp, idx);
        if (cp >= kHalfwidthCjkPunctFirst && cp <= kHalfwidthKatakanaLast) {
            return false;
        }
    }

    const std::string cand =
        trim_unicode((!en.empty() && en.front() != '$') ? en : jp);
    if (cand.empty() || cand.front() == '$' ||
        utf8_codepoint_count(cand) > kMaxStandaloneSpeakerCodepoints) {
        return false;
    }
    if (cand.rfind("「", 0) == 0 || cand.rfind("『", 0) == 0 ||
        cand.rfind("（", 0) == 0 || cand.rfind("―", 0) == 0 ||
        cand.find("。") != std::string::npos ||
        cand.find("、") != std::string::npos) {
        return false;
    }
    for (const char ch : cand) {
        if (ch == '"' || ch == '\'' || ch == '.' || ch == ',' || ch == ';' ||
            ch == ':' || ch == '!' || ch == '[' || ch == '-') {
            return false;
        }
    }
    return true;
}

std::string extract_crlf_dialogue_speaker(std::string_view text) {
    std::size_t nl_pos = text.find("\r\n");
    std::size_t sep_len = 2;
    if (nl_pos == std::string_view::npos) {
        nl_pos = text.find('\n');
        sep_len = 1;
    }
    if (nl_pos == std::string_view::npos || nl_pos == 0 ||
        nl_pos > kMaxCrlfSpeakerBytes) {
        return {};
    }

    const std::string_view rest = trim_ascii_view(text.substr(nl_pos + sep_len));
    if (rest.empty()) {
        return {};
    }
    const char r0 = rest.front();
    const bool valid_body_open =
        (r0 == '"' || r0 == '\'' || r0 == '(' || r0 == '*') ||
        rest.rfind("「", 0) == 0 || rest.rfind("『", 0) == 0 ||
        rest.rfind("（", 0) == 0;
    if (!valid_body_open) {
        return {};
    }

    const std::string prefix = trim_ascii(text.substr(0, nl_pos));
    if (prefix.empty() ||
        utf8_codepoint_count(prefix) > kMaxCrlfSpeakerCodepoints) {
        return {};
    }
    if (prefix.rfind("「", 0) == 0 || prefix.rfind("『", 0) == 0 ||
        prefix.rfind("　", 0) == 0 || prefix.rfind("（", 0) == 0) {
        return {};
    }
    for (const char ch : prefix) {
        if (ch == '"' || ch == '\'' || ch == '.' || ch == ',' || ch == ';' ||
            ch == ':' || ch == '!' || ch == '?' || ch == '[' || ch == '(') {
            return {};
        }
    }
    return prefix;
}

std::string extract_inline_kaku_speaker(std::string_view text) {
    const std::size_t pos = text.find("「");
    if (pos == std::string_view::npos || pos == 0 ||
        pos > kMaxInlineSpeakerBytes) {
        return {};
    }

    std::string_view rest = trim_ascii_view(text.substr(pos));
    if (rest.size() >= kPageContinueEscape.size() &&
        rest.substr(rest.size() - kPageContinueEscape.size()) ==
            kPageContinueEscape) {
        rest = trim_ascii_view(
            rest.substr(0, rest.size() - kPageContinueEscape.size()));
    }
    if (rest.find("」") != std::string_view::npos &&
        (rest.size() < kUtf8CornerBracketBytes ||
         rest.substr(rest.size() - kUtf8CornerBracketBytes) != "」")) {
        return {};
    }

    std::string_view raw_prefix = trim_ascii_view(text.substr(0, pos));
    if (raw_prefix.rfind("／", 0) == 0) {
        raw_prefix = trim_ascii_view(raw_prefix.substr(kUtf8CornerBracketBytes));
    }
    if (raw_prefix.empty() ||
        utf8_codepoint_count(raw_prefix) > kMaxInlineSpeakerCodepoints) {
        return {};
    }
    if (raw_prefix.rfind("「", 0) == 0 || raw_prefix.rfind("『", 0) == 0 ||
        raw_prefix.rfind("（", 0) == 0 || raw_prefix.rfind("　", 0) == 0) {
        return {};
    }
    if (raw_prefix.find("。") != std::string_view::npos ||
        raw_prefix.find("、") != std::string_view::npos ||
        raw_prefix.find("！") != std::string_view::npos ||
        raw_prefix.find("？") != std::string_view::npos) {
        return {};
    }
    for (const char ch : raw_prefix) {
        if (ch == '"' || ch == '\'' || ch == '.' || ch == ',' || ch == ';' ||
            ch == ':' || ch == '!' || ch == '?' || ch == '[' || ch == '\r' ||
            ch == '\n') {
            return {};
        }
    }
    return std::string(raw_prefix);
}

std::string resolve_speaker_with_fallback(std::string_view raw_speaker,
                                          const ToneGlossary& glossary,
                                          std::string_view active_speaker) {
    const std::string resolved = glossary.resolve_speaker(raw_speaker);
    if (resolved != raw_speaker ||
        (!has_japanese(resolved) && !has_non_ascii_bytes(resolved))) {
        return resolved;
    }
    if (!active_speaker.empty() && active_speaker != kNarrationSpeaker &&
        !has_japanese(active_speaker) && !has_non_ascii_bytes(active_speaker)) {
        return std::string(active_speaker);
    }
    return resolved;
}

void harvest_corpus_speaker_pairs(const bj::object& scripts_map,
                                  ToneGlossary& glossary) {
    for (const auto& kv : scripts_map) {
        const auto [unused_key, arr_ptr] = resolve_entry_array(kv.value());
        (void)unused_key;
        if (arr_ptr == nullptr) {
            continue;
        }
        for (const auto& entry_val : *arr_ptr) {
            if (!entry_val.is_object()) {
                continue;
            }
            const auto& entry_obj = entry_val.as_object();
            const std::string spk_jp =
                trim_unicode(json_string_or_empty(entry_obj, "speaker"));
            const std::string spk_en =
                trim_unicode(json_string_or_empty(entry_obj, "speaker_en"));
            if (!spk_jp.empty() && !spk_en.empty() &&
                has_real_japanese(spk_jp) && !has_japanese(spk_en)) {
                glossary.merge_speaker_pair(spk_jp, spk_en);
            }

            const std::string type = json_string_or_empty(entry_obj, "type");
            if (type == "NAME" || type == "name") {
                const auto [unused_tk, jp_text] = extract_jp_field(entry_obj);
                (void)unused_tk;
                const std::string en_text =
                    trim_unicode(extract_en_field(entry_obj));
                const std::string jp_clean = trim_unicode(jp_text);
                if (!jp_clean.empty() && !en_text.empty() &&
                    jp_clean != en_text &&
                    is_standalone_speaker_tag(jp_clean, en_text) &&
                    has_real_japanese(jp_clean) && !has_japanese(en_text)) {
                    glossary.merge_speaker_pair(jp_clean, en_text);
                }
            }
        }
    }
}

bool ends_with_page_continue(std::string_view text) noexcept {
    const std::string_view t = trim_ascii_view(text);
    return t.size() >= kPageContinueEscape.size() &&
           t.substr(t.size() - kPageContinueEscape.size()) ==
               kPageContinueEscape;
}

bool opens_japanese_or_ascii_dialogue(std::string_view jp_text,
                                      std::string_view en_text) noexcept {
    std::string_view t_jp = trim_ascii_view(jp_text);
    while (t_jp.rfind(kSoftWrapEscape, 0) == 0) {
        t_jp = trim_ascii_view(t_jp.substr(kSoftWrapEscape.size()));
    }
    if (t_jp.rfind("「", 0) == 0 || t_jp.rfind("『", 0) == 0 ||
        t_jp.rfind("（", 0) == 0) {
        return true;
    }
    std::string_view t_en = trim_ascii_view(en_text);
    while (t_en.rfind(kSoftWrapEscape, 0) == 0) {
        t_en = trim_ascii_view(t_en.substr(kSoftWrapEscape.size()));
    }
    return !t_en.empty() && (t_en.front() == '"' || t_en.front() == '(');
}

bool closes_japanese_dialogue(std::string_view jp_text) noexcept {
    return jp_text.find("」") != std::string_view::npos ||
           jp_text.find("』") != std::string_view::npos ||
           jp_text.find("）") != std::string_view::npos;
}

void pretty_json_impl(const bj::value& val,
                      std::string& out,
                      int indent_spaces,
                      int depth,
                      std::string_view nl) {
    const std::string pad(
        static_cast<std::size_t>(indent_spaces * depth), ' ');
    const std::string child_pad(
        static_cast<std::size_t>(indent_spaces * (depth + 1)), ' ');

    switch (val.kind()) {
    case bj::kind::object: {
        const auto& obj = val.get_object();
        if (obj.empty()) {
            out += "{}";
            return;
        }
        out += '{';
        out.append(nl.data(), nl.size());
        bool first = true;
        for (const auto& kv : obj) {
            if (!first) {
                out += ',';
                out.append(nl.data(), nl.size());
            }
            first = false;
            out += child_pad;
            out += bj::serialize(bj::value(kv.key()));
            out += ": ";
            pretty_json_impl(kv.value(), out, indent_spaces, depth + 1, nl);
        }
        out.append(nl.data(), nl.size());
        out += pad;
        out += '}';
        return;
    }
    case bj::kind::array: {
        const auto& arr = val.get_array();
        if (arr.empty()) {
            out += "[]";
            return;
        }
        out += '[';
        out.append(nl.data(), nl.size());
        bool first = true;
        for (const auto& elem : arr) {
            if (!first) {
                out += ',';
                out.append(nl.data(), nl.size());
            }
            first = false;
            out += child_pad;
            pretty_json_impl(elem, out, indent_spaces, depth + 1, nl);
        }
        out.append(nl.data(), nl.size());
        out += pad;
        out += ']';
        return;
    }
    default:
        out += bj::serialize(val);
        return;
    }
}

std::string serialize_json_with_style(const bj::value& val,
                                      const JsonFormatStyle& style) {
    const std::string_view nl = style.use_crlf ? "\r\n" : "\n";
    std::string out;
    if (style.indent_spaces <= 0) {
        out = bj::serialize(val);
    } else {
        pretty_json_impl(val, out, style.indent_spaces, 0, nl);
    }
    if (style.trailing_newline) {
        out.append(nl.data(), nl.size());
    }
    return out;
}

bool update_entry_translation_fields(bj::object& entry_obj,
                                     const std::string& improved_en) {
    bool modified = false;
    const auto* tr = entry_obj.if_contains("translated");
    if (tr == nullptr || !tr->is_string() || tr->as_string() != improved_en) {
        entry_obj["translated"] = improved_en;
        modified = true;
    }
    for (const char* mirror_key : {"translation", "en"}) {
        if (auto* mirror_val = entry_obj.if_contains(mirror_key);
            mirror_val != nullptr) {
            if (!mirror_val->is_string() ||
                mirror_val->as_string() != improved_en) {
                *mirror_val = improved_en;
                modified = true;
            }
        }
    }
    return modified;
}

bool is_cache_json_filename(std::string_view filename) noexcept {
    return filename.size() > (kCachePrefix.size() + kJsonSuffix.size()) &&
           filename.substr(0, kCachePrefix.size()) == kCachePrefix &&
           filename.substr(filename.size() - kJsonSuffix.size()) == kJsonSuffix;
}

struct PendingFileCommit {
    fs::path path;
    std::string pre_image;
    std::string updated_content;
};

void rollback_commits(IFileStore& store,
                      const std::vector<PendingFileCommit>& commits,
                      std::size_t count) {
    for (std::size_t idx = count; idx > 0; --idx) {
        const auto& item = commits[idx - 1];
        (void)store.write_text_atomic(item.path, item.pre_image);
    }
}

}  // namespace

ScriptRepository::ScriptRepository(IFileStore& store, ICryptSync& crypt) noexcept
    : store_(store), crypt_(crypt) {}

Result<LoadedCorpus> ScriptRepository::parse_corpus_memory(
    std::string_view translated_json_utf8,
    std::string_view tone_markdown_utf8) {
    const JsonFormatStyle style = detect_json_format_style(translated_json_utf8);
    const std::string_view body = strip_utf8_bom(translated_json_utf8);

    boost::system::error_code ec;
    const bj::value root =
        bj::parse(bj::string_view(body.data(), body.size()), ec);
    if (ec) {
        return Result<LoadedCorpus>::failure(
            ErrorCode::CorruptJson,
            "invalid JSON in translated_text.json: " + ec.message());
    }
    if (!root.is_object()) {
        return Result<LoadedCorpus>::failure(
            ErrorCode::CorruptJson,
            "translated_text.json root must be a JSON object");
    }

    LoadedCorpus corpus;
    corpus.translated_json_style = style;
    corpus.glossary = ToneGlossary::parse_markdown(tone_markdown_utf8);

    const bj::object* scripts_map = resolve_scripts_map(root.as_object());
    harvest_corpus_speaker_pairs(*scripts_map, corpus.glossary);

    for (const auto& kv : *scripts_map) {
        const std::string script_name(kv.key());
        const auto [array_key, arr_ptr] = resolve_entry_array(kv.value());
        if (arr_ptr == nullptr) {
            continue;
        }

        std::string active_speaker(kNarrationSpeaker);
        bool in_dialogue = false;

        for (std::size_t i = 0; i < arr_ptr->size(); ++i) {
            const bj::value& elem = (*arr_ptr)[i];
            if (!elem.is_object()) {
                continue;
            }
            const auto& entry_obj = elem.as_object();
            const std::string type = json_string_or_empty(entry_obj, "type");
            auto [text_key, jp_text] = extract_jp_field(entry_obj);
            const std::string en_text = extract_en_field(entry_obj);

            if (type == "NAME") {
                if (is_standalone_speaker_tag(jp_text, en_text)) {
                    const std::string raw_name =
                        trim_unicode(!en_text.empty() ? en_text : jp_text);
                    active_speaker = corpus.glossary.resolve_speaker(raw_name);
                } else {
                    active_speaker = std::string(kNarrationSpeaker);
                }
                in_dialogue = false;
                continue;
            }

            if (type == "name") {
                if (is_standalone_speaker_tag(jp_text, en_text)) {
                    const std::string raw_name =
                        trim_unicode(!en_text.empty() ? en_text : jp_text);
                    active_speaker = corpus.glossary.resolve_speaker(raw_name);
                    in_dialogue = false;
                    continue;
                }
                if (en_text == jp_text || !has_real_japanese(jp_text) ||
                    has_binary_or_pua_noise(jp_text)) {
                    continue;
                }
            }

            if (type == "COMMAND") {
                const std::string_view cmd_trimmed = trim_ascii_view(jp_text);
                if (cmd_trimmed == "fw 0" || cmd_trimmed.rfind("fw 0 ", 0) == 0) {
                    active_speaker = std::string(kNarrationSpeaker);
                    in_dialogue = false;
                }
                continue;
            }

            if (type == "other" &&
                trim_unicode(jp_text).rfind("＊", 0) == 0) {
                active_speaker = std::string(kNarrationSpeaker);
                in_dialogue = false;
                continue;
            }

            if (is_excluded_entry_type(type) && type != "name") {
                continue;
            }

            if (const auto* hj = entry_obj.if_contains("has_jp");
                hj != nullptr && hj->is_bool() && !hj->as_bool()) {
                continue;
            }
            if (!has_japanese(jp_text) || en_text.empty()) {
                continue;
            }
            if (has_binary_or_pua_noise(jp_text) ||
                (jp_text == en_text &&
                 is_engine_asset_or_command_token(jp_text, en_text))) {
                continue;
            }

            const auto* raw_spk_val = entry_obj.if_contains("speaker");
            const bool explicit_null_speaker =
                (raw_spk_val != nullptr && raw_spk_val->is_null());
            const std::string explicit_spk =
                trim_unicode(json_string_or_empty(entry_obj, "speaker"));
            const std::string explicit_spk_en =
                trim_unicode(json_string_or_empty(entry_obj, "speaker_en"));

            if (type == "narration" || type == "narrative" || type == "menu" ||
                explicit_null_speaker) {
                active_speaker = std::string(kNarrationSpeaker);
                in_dialogue = false;
            }

            std::string speaker;
            if (!explicit_spk_en.empty()) {
                speaker = corpus.glossary.resolve_speaker(explicit_spk_en);
            } else if (!explicit_spk.empty()) {
                speaker = resolve_speaker_with_fallback(
                    explicit_spk, corpus.glossary, active_speaker);
            } else if (type == "narration" || type == "narrative" ||
                       type == "menu" || explicit_null_speaker) {
                speaker = std::string(kNarrationSpeaker);
            } else {
                std::string inline_spk = extract_crlf_dialogue_speaker(en_text);
                if (inline_spk.empty()) {
                    inline_spk = extract_inline_kaku_speaker(en_text);
                }
                if (!inline_spk.empty()) {
                    speaker = resolve_speaker_with_fallback(
                        inline_spk, corpus.glossary, active_speaker);
                } else {
                    std::string jp_inline =
                        extract_crlf_dialogue_speaker(jp_text);
                    if (jp_inline.empty()) {
                        jp_inline = extract_inline_kaku_speaker(jp_text);
                    }
                    if (!jp_inline.empty()) {
                        speaker = resolve_speaker_with_fallback(
                            jp_inline, corpus.glossary, active_speaker);
                    }
                }

                if (speaker.empty()) {
                    if (active_speaker != kNarrationSpeaker) {
                        const bool opens =
                            opens_japanese_or_ascii_dialogue(jp_text, en_text);
                        if (opens) {
                            in_dialogue = true;
                        }
                        if (in_dialogue || type == "dialogue") {
                            speaker = active_speaker;
                            const bool page_cont =
                                ends_with_page_continue(jp_text) ||
                                ends_with_page_continue(en_text);
                            const bool closes =
                                closes_japanese_dialogue(jp_text);
                            if (!page_cont && (closes || !opens)) {
                                in_dialogue = false;
                            }
                        } else {
                            active_speaker = std::string(kNarrationSpeaker);
                            speaker = std::string(kNarrationSpeaker);
                        }
                    } else {
                        speaker = std::string(kNarrationSpeaker);
                    }
                }
            }

            ScriptEntry se;
            se.global_index = corpus.entries.size();
            se.script_name = script_name;
            se.array_key = array_key;
            se.entry_index = i;
            se.type = type;
            se.text_key = std::move(text_key);
            se.speaker = std::move(speaker);
            se.jp_text = std::move(jp_text);
            se.current_en = std::move(en_text);
            corpus.entries.push_back(std::move(se));
        }
    }

    return Result<LoadedCorpus>::success(std::move(corpus));
}

Result<std::string> ScriptRepository::apply_improvements_memory(
    std::string_view translated_json_utf8,
    const std::vector<Improvement>& improvements,
    std::size_t* out_script_entries_updated) {
    if (out_script_entries_updated != nullptr) {
        *out_script_entries_updated = 0;
    }

    const JsonFormatStyle style = detect_json_format_style(translated_json_utf8);
    const std::string_view body = strip_utf8_bom(translated_json_utf8);

    boost::system::error_code ec;
    bj::value root = bj::parse(bj::string_view(body.data(), body.size()), ec);
    if (ec) {
        return Result<std::string>::failure(
            ErrorCode::CorruptJson,
            "invalid JSON in translated_text.json: " + ec.message());
    }
    if (!root.is_object()) {
        return Result<std::string>::failure(
            ErrorCode::CorruptJson,
            "translated_text.json root must be a JSON object");
    }

    if (improvements.empty()) {
        return Result<std::string>::success(std::string(translated_json_utf8));
    }

    bj::object* scripts_map = resolve_scripts_map_mut(root.as_object());

    struct DuplicateRule {
        std::string old_en;
        std::string improved_en;
    };
    std::unordered_map<std::string, std::vector<DuplicateRule>> dup_rules;
    dup_rules.reserve(improvements.size());

    std::size_t updated_count = 0;
    for (const Improvement& imp : improvements) {
        if (imp.improved_en.empty()) {
            return Result<std::string>::failure(
                ErrorCode::InvalidArgument,
                "improvement has empty improved_en for script '" +
                    imp.script_name + "'");
        }
        auto* script_val = scripts_map->if_contains(imp.script_name);
        if (script_val == nullptr) {
            return Result<std::string>::failure(
                ErrorCode::InvalidArgument,
                "target script not found in translated_text.json: " +
                    imp.script_name);
        }
        bj::array* arr = resolve_entry_array_mut(*script_val);
        if (arr == nullptr || imp.entry_index >= arr->size() ||
            !(*arr)[imp.entry_index].is_object()) {
            return Result<std::string>::failure(
                ErrorCode::InvalidArgument,
                "target entry_index out of range for " + imp.script_name + "[" +
                    std::to_string(imp.entry_index) + "]");
        }

        auto& entry_obj = (*arr)[imp.entry_index].as_object();
        if (update_entry_translation_fields(entry_obj, imp.improved_en)) {
            ++updated_count;
        }
        if (!imp.jp_text.empty()) {
            dup_rules[imp.jp_text].push_back(
                DuplicateRule{imp.old_en, imp.improved_en});
        }
    }

    if (!dup_rules.empty()) {
        for (auto& kv : *scripts_map) {
            bj::array* arr = resolve_entry_array_mut(kv.value());
            if (arr == nullptr) {
                continue;
            }
            for (auto& elem : *arr) {
                if (!elem.is_object()) {
                    continue;
                }
                auto& entry_obj = elem.as_object();
                const auto [unused_tk, jp] = extract_jp_field(entry_obj);
                (void)unused_tk;
                if (jp.empty()) {
                    continue;
                }
                const auto rule_it = dup_rules.find(jp);
                if (rule_it == dup_rules.end()) {
                    continue;
                }
                const std::string cur_en = extract_en_field(entry_obj);
                for (const DuplicateRule& rule : rule_it->second) {
                    if (cur_en == rule.old_en || cur_en == jp ||
                        cur_en == rule.improved_en) {
                        if (update_entry_translation_fields(entry_obj,
                                                            rule.improved_en)) {
                            ++updated_count;
                        }
                        break;
                    }
                }
            }
        }
    }

    if (out_script_entries_updated != nullptr) {
        *out_script_entries_updated = updated_count;
    }
    if (updated_count == 0) {
        return Result<std::string>::success(std::string(translated_json_utf8));
    }
    return Result<std::string>::success(serialize_json_with_style(root, style));
}

Result<LoadedCorpus> ScriptRepository::load_from_game_dir(
    const std::filesystem::path& game_root) const {
    if (game_root.empty()) {
        return Result<LoadedCorpus>::failure(
            ErrorCode::InvalidArgument, "game_root path is empty");
    }

    const Status dec_status = crypt_.decrypt_if_needed(game_root);
    if (!dec_status.is_ok()) {
        return Result<LoadedCorpus>::failure(dec_status);
    }

    const fs::path translated_path =
        game_root / kScriptOutputDir / kTranslatedJsonFile;
    if (!store_.exists(translated_path)) {
        return Result<LoadedCorpus>::failure(
            ErrorCode::IoError,
            "translated_text.json not found at " +
                path_to_utf8(translated_path));
    }

    const auto json_res = store_.read_text(translated_path);
    if (!json_res.is_ok()) {
        return Result<LoadedCorpus>::failure(json_res.status());
    }

    std::string tone_md;
    const fs::path primary_tone = game_root / kTonePrimaryFile;
    const fs::path fallback_tone = game_root / kToneFallbackFile;
    if (store_.exists(primary_tone)) {
        const auto tone_res = store_.read_text(primary_tone);
        if (!tone_res.is_ok()) {
            return Result<LoadedCorpus>::failure(tone_res.status());
        }
        tone_md = tone_res.value();
    } else if (store_.exists(fallback_tone)) {
        const auto tone_res = store_.read_text(fallback_tone);
        if (!tone_res.is_ok()) {
            return Result<LoadedCorpus>::failure(tone_res.status());
        }
        tone_md = tone_res.value();
    }

    return parse_corpus_memory(json_res.value(), tone_md);
}

Result<SyncCounts> ScriptRepository::apply_improvements_to_artifacts(
    const std::filesystem::path& game_root,
    const std::vector<Improvement>& improvements) const {
    if (game_root.empty()) {
        return Result<SyncCounts>::failure(
            ErrorCode::InvalidArgument, "game_root path is empty");
    }
    if (improvements.empty()) {
        return Result<SyncCounts>::success({});
    }

    const fs::path script_out_dir = game_root / kScriptOutputDir;
    const fs::path translated_path = script_out_dir / kTranslatedJsonFile;
    if (!store_.exists(translated_path)) {
        return Result<SyncCounts>::failure(
            ErrorCode::IoError,
            "translated_text.json not found at " +
                path_to_utf8(translated_path));
    }

    const auto orig_translated_res = store_.read_text(translated_path);
    if (!orig_translated_res.is_ok()) {
        return Result<SyncCounts>::failure(orig_translated_res.status());
    }

    SyncCounts counts;
    const auto new_translated_res = apply_improvements_memory(
        orig_translated_res.value(), improvements,
        &counts.script_entries_updated);
    if (!new_translated_res.is_ok()) {
        return Result<SyncCounts>::failure(new_translated_res.status());
    }

    std::vector<PendingFileCommit> commits;
    if (counts.script_entries_updated > 0) {
        commits.push_back(PendingFileCommit{
            translated_path,
            orig_translated_res.value(),
            new_translated_res.value(),
        });
    }

    const std::vector<fs::path> regular_files =
        store_.list_regular_files(script_out_dir);
    for (const fs::path& file_path : regular_files) {
        const std::string fname = path_to_utf8(file_path.filename());
        if (!is_cache_json_filename(fname)) {
            continue;
        }

        const auto raw_cache_res = store_.read_text(file_path);
        if (!raw_cache_res.is_ok()) {
            return Result<SyncCounts>::failure(raw_cache_res.status());
        }
        const JsonFormatStyle cache_style =
            detect_json_format_style(raw_cache_res.value());
        auto map_res =
            parse_ordered_json_string_map(raw_cache_res.value(), fname);
        if (!map_res.is_ok()) {
            return Result<SyncCounts>::failure(map_res.status());
        }

        OrderedStringMap& cache_map = map_res.value();
        std::size_t file_updates = 0;
        for (const Improvement& imp : improvements) {
            if (!imp.jp_text.empty()) {
                if (const std::string* cur = cache_map.find(imp.jp_text);
                    cur != nullptr && *cur != imp.improved_en) {
                    cache_map.set(imp.jp_text, imp.improved_en);
                    ++file_updates;
                }
            }

            const std::string stripped_jp =
                strip_inline_control_wrappers(imp.jp_text);
            if (!stripped_jp.empty() && stripped_jp != imp.jp_text) {
                if (const std::string* cur = cache_map.find(stripped_jp);
                    cur != nullptr && *cur != imp.improved_en &&
                    (*cur == imp.old_en ||
                     *cur == strip_speaker_prefix_body(imp.old_en))) {
                    cache_map.set(stripped_jp, imp.improved_en);
                    ++file_updates;
                }
            }
        }

        if (file_updates > 0) {
            counts.cache_entries_updated += file_updates;
            commits.push_back(PendingFileCommit{
                file_path,
                raw_cache_res.value(),
                serialize_ordered_json_string_map(cache_map, cache_style),
            });
        }
    }

    if (commits.empty()) {
        return Result<SyncCounts>::success(counts);
    }

    for (std::size_t i = 0; i < commits.size(); ++i) {
        const Status ws = store_.write_text_atomic(commits[i].path,
                                                   commits[i].updated_content);
        if (!ws.is_ok()) {
            rollback_commits(store_, commits, i);
            return Result<SyncCounts>::failure(ws);
        }
    }

    const Status enc_status = crypt_.encrypt_if_tracked(game_root);
    if (!enc_status.is_ok()) {
        rollback_commits(store_, commits, commits.size());
        return Result<SyncCounts>::failure(enc_status);
    }

    return Result<SyncCounts>::success(counts);
}

}  // namespace spot_checker
