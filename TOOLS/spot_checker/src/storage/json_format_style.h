// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spot_checker {

struct JsonFormatStyle {
    int indent_spaces = 2;
    bool use_crlf = false;
    bool trailing_newline = true;
};

// Inspects `raw_json` to detect indentation width (e.g. 1, 2, or 4 spaces on
// the first indented line, or 0 for compact single-line JSON), CRLF vs LF line
// endings, and whether `raw_json` ends with a newline.
JsonFormatStyle detect_json_format_style(std::string_view raw_json) noexcept;

// Insertion-order-preserving `jp -> en` string dictionary for
// `translation_cache_*.json` files so spot-check updates never reshuffle diffs.
class OrderedStringMap {
public:
    OrderedStringMap() = default;

    bool empty() const noexcept { return keys_.empty(); }
    std::size_t size() const noexcept { return keys_.size(); }

    bool contains(std::string_view key) const;
    const std::string* find(std::string_view key) const;

    // Inserts `(key, value)` at the back if `key` is new, or updates `value`
    // in-place preserving `key`'s original position.
    void set(std::string key, std::string value);

    const std::vector<std::string>& keys() const noexcept { return keys_; }

private:
    std::vector<std::string> keys_;
    std::unordered_map<std::string, std::string> values_;
};

// Parses a top-level JSON object of `string -> string` pairs while preserving
// exact key order from `raw_json`. Returns `ErrorCode::CorruptJson` (naming
// `source_label`) if `raw_json` is malformed or its root is not a JSON object.
Result<OrderedStringMap> parse_ordered_json_string_map(
    std::string_view raw_json,
    std::string_view source_label = "<json>");

// Serializes `map` in exact `keys()` order formatted according to `style`.
std::string serialize_ordered_json_string_map(const OrderedStringMap& map,
                                              const JsonFormatStyle& style);

}  // namespace spot_checker
