// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 Japanese-run scanner over decrypted YBN bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace frat::yuris {

inline bool is_cp932_lead(std::uint8_t b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}
inline bool is_cp932_trail(std::uint8_t b) {
    return (b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC);
}
inline bool is_ascii_printable(std::uint8_t b) {
    return (b >= 0x20 && b < 0x7F) || b == 0x09 || b == 0x0A || b == 0x0D;
}

// Wider than extract::has_japanese — also accepts U+3000-303F and U+FF00-FFEF.
bool has_japanese_run(const std::string& utf8);

// Drop ASCII edges around the JP core. Excludes U+3000-303F so leading 「
// is trimmed — only split_into_messages pieces can start with it.
std::string trim_ascii_edges(const std::string& utf8);

// Strip only the listed chars; U+3000 survives (the engine renders it).
std::string strip_chars(const std::string& s, const std::string& chars);

// Maximal CP932 runs with at least one double-byte character.
// On decode failure the whole run is skipped — no single-byte retry.
std::vector<std::pair<std::size_t, std::string>> scan_cp932_jp(
    const Bytes& data, std::size_t min_run = 4, std::size_t max_run = 16384);

}  // namespace frat::yuris
