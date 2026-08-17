// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "yuris/cp932_scan.h"

namespace frat::yuris {

bool has_japanese_run(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x3000 && cp <= 0x303F))
            return true;
    }
    return false;
}

namespace {
bool jpish(char32_t cp) {
    return (cp >= 0x3040 && cp <= 0x9FFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}
}  // namespace

std::string trim_ascii_edges(const std::string& utf8) {
    const std::vector<char32_t> cps = utf8_decode(utf8);
    std::size_t start = 0;
    bool found = false;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        if (jpish(cps[i])) { start = i; found = true; break; }
    }
    if (!found) return {};
    std::size_t end = cps.size();
    for (std::size_t i = cps.size(); i-- > start;) {
        if (jpish(cps[i])) { end = i + 1; break; }
    }
    return utf8_encode(std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(start),
                                             cps.begin() + static_cast<std::ptrdiff_t>(end)));
}

std::string strip_chars(const std::string& s, const std::string& chars) {
    const auto in_set = [&chars](char c) { return chars.find(c) != std::string::npos; };
    std::size_t b = 0, e = s.size();
    while (b < e && in_set(s[b])) ++b;
    while (e > b && in_set(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::pair<std::size_t, std::string>> scan_cp932_jp(
    const Bytes& data, std::size_t min_run, std::size_t max_run) {
    std::vector<std::pair<std::size_t, std::string>> out;
    const std::size_t n = data.size();
    std::size_t i = 0;
    while (i < n) {
        const std::size_t start = i;
        bool has_db = false;
        while (i < n) {
            const std::uint8_t b = data[i];
            if (is_cp932_lead(b) && i + 1 < n && is_cp932_trail(data[i + 1])) {
                i += 2;
                has_db = true;
            } else if (is_ascii_printable(b)) {
                ++i;
            } else {
                break;
            }
        }
        const std::size_t length = i - start;
        if (has_db && min_run <= length && length <= max_run) {
            auto decoded = cp932_to_utf8_strict(data.data() + start, length);
            if (!decoded) {
                if (i == start) ++i;
                continue;
            }
            static const std::string kEdgeChars(" \t\r\n\0", 5);
            const std::string text = strip_chars(trim_ascii_edges(*decoded), kEdgeChars);
            if (!text.empty() && has_japanese_run(text)) out.emplace_back(start, text);
        }
        if (i == start) ++i;
    }
    return out;
}

}  // namespace frat::yuris
