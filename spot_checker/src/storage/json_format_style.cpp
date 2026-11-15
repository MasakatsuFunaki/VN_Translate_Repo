// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "storage/json_format_style.h"

#include "common/utf8_text.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace spot_checker {
namespace {

namespace bj = boost::json;

constexpr std::size_t kFormatSampleBytes = 16384;
constexpr int kDefaultIndentSpaces = 2;
constexpr int kMinDetectedIndent = 1;
constexpr int kMaxDetectedIndent = 8;
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

std::string_view strip_utf8_bom(std::string_view text) noexcept {
    if (text.size() >= kUtf8Bom.size() &&
        text.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        text.remove_prefix(kUtf8Bom.size());
    }
    return text;
}

}  // namespace

JsonFormatStyle detect_json_format_style(std::string_view raw_json) noexcept {
    JsonFormatStyle style;
    if (raw_json.empty()) {
        return style;
    }

    style.trailing_newline = (raw_json.back() == '\n');

    const std::string_view sample =
        raw_json.substr(0, std::min(raw_json.size(), kFormatSampleBytes));
    style.use_crlf = (sample.find("\r\n") != std::string_view::npos);

    const std::string_view trimmed = trim_ascii_view( strip_utf8_bom(raw_json) );
    if (trimmed.find('\n') == std::string_view::npos) {
        if (trimmed != "{}" && trimmed != "[]") {
            style.indent_spaces = 0;
        }
        return style;
    }

    style.indent_spaces = kDefaultIndentSpaces;
    std::size_t pos = 0;
    while (pos < sample.size()) {
        const std::size_t nl = sample.find('\n', pos);
        if (nl == std::string_view::npos) {
            break;
        }
        std::size_t idx = nl + 1;
        int spaces = 0;
        while (idx < sample.size() && sample[idx] == ' ') {
            ++spaces;
            ++idx;
        }
        if (idx < sample.size() && sample[idx] != '\r' && sample[idx] != '\n' &&
            spaces >= kMinDetectedIndent && spaces <= kMaxDetectedIndent) {
            style.indent_spaces = spaces;
            break;
        }
        pos = nl + 1;
    }
    return style;
}

bool OrderedStringMap::contains(std::string_view key) const {
    return find(key) != nullptr;
}

const std::string* OrderedStringMap::find(std::string_view key) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return nullptr;
    }
    return &it->second;
}

void OrderedStringMap::set(std::string key, std::string value) {
    const auto [it, inserted] =
        values_.insert_or_assign(key, std::move(value));
    (void)it;
    if (inserted) {
        keys_.push_back(std::move(key));
    }
}

Result<OrderedStringMap> parse_ordered_json_string_map(
    std::string_view raw_json,
    std::string_view source_label) {
    const std::string_view body = strip_utf8_bom(raw_json);
    boost::system::error_code ec;
    const bj::value parsed =
        bj::parse(bj::string_view(body.data(), body.size()), ec);
    if (ec) {
        return Result<OrderedStringMap>::failure(
            ErrorCode::CorruptJson,
            "invalid JSON in " + std::string(source_label) + ": " +
                ec.message());
    }
    if (!parsed.is_object()) {
        return Result<OrderedStringMap>::failure(
            ErrorCode::CorruptJson,
            "JSON root is not an object in " + std::string(source_label));
    }

    OrderedStringMap map;
    for (const auto& kv : parsed.as_object()) {
        if (!kv.value().is_string()) {
            return Result<OrderedStringMap>::failure(
                ErrorCode::CorruptJson,
                "non-string value for key '" + std::string(kv.key()) +
                    "' in " + std::string(source_label));
        }
        map.set(std::string(kv.key()), std::string(kv.value().as_string()));
    }
    return Result<OrderedStringMap>::success(std::move(map));
}

std::string serialize_ordered_json_string_map(const OrderedStringMap& map,
                                              const JsonFormatStyle& style) {
    const std::string_view nl = style.use_crlf ? "\r\n" : "\n";
    if (map.empty()) {
        std::string out = "{}";
        if (style.trailing_newline) {
            out.append(nl.data(), nl.size());
        }
        return out;
    }

    const bool compact = (style.indent_spaces <= 0);
    const std::string pad =
        compact ? std::string{}
                : std::string(static_cast<std::size_t>(style.indent_spaces), ' ');

    std::string out = "{";
    if (!compact) {
        out.append(nl.data(), nl.size());
    }

    const auto& keys = map.keys();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) {
            out += ',';
            if (!compact) {
                out.append(nl.data(), nl.size());
            }
        }
        const std::string& key = keys[i];
        const std::string* val = map.find(key);
        if (!compact) {
            out += pad;
        }
        out += bj::serialize(bj::value(key));
        out += compact ? ":" : ": ";
        out += bj::serialize(bj::value(val != nullptr ? *val : std::string{}));
    }

    if (!compact) {
        out.append(nl.data(), nl.size());
    }
    out += '}';
    if (style.trailing_newline) {
        out.append(nl.data(), nl.size());
    }
    return out;
}

}  // namespace spot_checker
