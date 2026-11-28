// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Private C++20 ASCII view/case/redaction utilities shared across `vn_llm_core`
// and provider adapters. Never installed or exposed in `include/vn/llm/`.
#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>

namespace vn::llm::detail {

inline constexpr std::string_view kRedactedSecretPlaceholder = "<redacted>";

[[nodiscard]] constexpr bool is_ascii_whitespace(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

[[nodiscard]] constexpr char to_lower_ascii_char(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] constexpr char to_upper_ascii_char(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
}

[[nodiscard]] inline std::string_view trim_ascii_view(
    std::string_view text) noexcept {
    const auto first_it = std::ranges::find_if_not(text, is_ascii_whitespace);
    if (first_it == text.end()) {
        return {};
    }
    const auto last_rev_it = std::ranges::find_if_not(
        text | std::views::reverse, is_ascii_whitespace);
    const auto first_offset =
        static_cast<std::size_t>(std::distance(text.begin(), first_it));
    const auto last_offset =
        static_cast<std::size_t>(std::distance(text.begin(), last_rev_it.base()));
    return text.substr(first_offset, last_offset - first_offset);
}

[[nodiscard]] inline std::string to_lower_ascii(std::string_view text) {
    std::string out(text.size(), '\0');
    std::ranges::transform(text, out.begin(), to_lower_ascii_char);
    return out;
}

[[nodiscard]] inline std::string to_upper_ascii(std::string_view text) {
    std::string out(text.size(), '\0');
    std::ranges::transform(text, out.begin(), to_upper_ascii_char);
    return out;
}

[[nodiscard]] inline bool equals_ascii_ci(std::string_view lhs,
                                          std::string_view rhs) noexcept {
    return std::ranges::equal(lhs, rhs, {},
                              to_lower_ascii_char, to_lower_ascii_char);
}

[[nodiscard]] inline bool contains_ascii_ci(std::string_view haystack,
                                            std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    return !std::ranges::search(haystack, needle, {},
                                to_lower_ascii_char, to_lower_ascii_char)
                .empty();
}

[[nodiscard]] inline std::string redact_secret(
    std::string text,
    std::string_view secret,
    std::string_view placeholder = kRedactedSecretPlaceholder) {
    if (secret.empty() || text.empty()) {
        return text;
    }
    std::size_t pos = 0;
    while ((pos = text.find(secret, pos)) != std::string::npos) {
        text.replace(pos, secret.size(), placeholder);
        pos += placeholder.size();
    }
    return text;
}

}  // namespace vn::llm::detail
