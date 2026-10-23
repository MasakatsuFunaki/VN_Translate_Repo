// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace tr {

namespace {

void unescape(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            switch (n) {
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'n':  out.push_back('\n'); break;
                case '\\': out.push_back('\\'); break;
                default:   out.push_back('\\'); out.push_back(n); break;
            }
        } else {
            out.push_back(c);
        }
    }
    s.swap(out);
}

}  // namespace

size_t load_tsv(std::string_view tsv,
                std::unordered_map<std::string, std::string>& out) {
    size_t inserted = 0;
    size_t i = 0;
    while (i < tsv.size()) {
        size_t end = tsv.find('\n', i);
        if (end == std::string_view::npos) end = tsv.size();
        std::string_view line = tsv.substr(i, end - i);
        i = end + 1;

        // Strip optional \r at end
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty()) continue;

        size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;

        std::string jp(line.substr(0, tab));
        std::string en(line.substr(tab + 1));
        unescape(jp);
        unescape(en);

        if (jp.empty() || en.empty()) continue;
        if (out.emplace(std::move(jp), std::move(en)).second)
            ++inserted;
    }
    return inserted;
}

size_t load_tsv_file(const std::string& path,
                     std::unordered_map<std::string, std::string>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return SIZE_MAX;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string body = ss.str();
    return load_tsv(body, out);
}

bool lookup(const std::unordered_map<std::string, std::string>& table,
            std::string_view jp, std::string& out) {
    auto it = table.find(std::string(jp));
    if (it == table.end()) return false;
    out = it->second;
    return true;
}

bool has_japanese_utf8(std::string_view s) {
    // Walk codepoints; flag if any falls in Hiragana / Katakana / CJK.
    for (size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        unsigned cp = 0;
        size_t len = 1;
        if (b < 0x80) {
            cp = b; len = 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((b & 0x1F) << 6) | (s[i+1] & 0x3F);
            len = 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((b & 0x0F) << 12)
               | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[i+2]) & 0x3F);
            len = 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((b & 0x07) << 18)
               | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 12)
               | ((static_cast<unsigned char>(s[i+2]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[i+3]) & 0x3F);
            len = 4;
        } else {
            return false;  // invalid UTF-8 — bail conservatively
        }
        if ((cp >= 0x3040 && cp <= 0x309F) ||   // Hiragana
            (cp >= 0x30A0 && cp <= 0x30FF) ||   // Katakana
            (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified
            (cp >= 0x3400 && cp <= 0x4DBF))     // CJK Ext A
            return true;
        i += len;
    }
    return false;
}

}  // namespace tr
