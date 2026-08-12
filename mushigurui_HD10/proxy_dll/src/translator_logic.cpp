// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <utility>

namespace translator_logic {

void UnescapeInPlace(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'r')  { out += '\r'; i++; continue; }
            if (c == 'n')  { out += '\n'; i++; continue; }
            if (c == 't')  { out += '\t'; i++; continue; }
            if (c == '\\') { out += '\\'; i++; continue; }
        }
        out += s[i];
    }
    s = std::move(out);
}

std::size_t Utf8BomLen(const char* data, std::size_t len)
{
    if (len >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        return 3;
    }
    return 0;
}

int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out)
{
    int count = 0;
    std::size_t i = 0;
    while (i < len) {
        const std::size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        const std::size_t line_end = i;
        // Skip the line terminator(s).
        while (i < len && (data[i] == '\r' || data[i] == '\n')) i++;

        // Find the tab inside the line.
        std::size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') tab++;
        if (tab == line_start || tab >= line_end) continue;

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + 1, line_end - (tab + 1));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        if (jp.empty() || en.empty()) continue;
        out[jp] = en;   // last duplicate wins (matches prior behavior)
        count++;
    }
    return count;
}

}  // namespace translator_logic
