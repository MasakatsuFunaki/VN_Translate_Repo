// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "translator_logic.h"

namespace translator_logic {

namespace {

constexpr std::size_t kUtf8BomByteLen = 3;
constexpr unsigned char kUtf8BomByte0 = 0xEF;
constexpr unsigned char kUtf8BomByte1 = 0xBB;
constexpr unsigned char kUtf8BomByte2 = 0xBF;

}  // namespace

std::size_t Utf8BomLen(const char* buf, std::size_t len) {
    if (buf && len >= kUtf8BomByteLen &&
        static_cast<unsigned char>(buf[0]) == kUtf8BomByte0 &&
        static_cast<unsigned char>(buf[1]) == kUtf8BomByte1 &&
        static_cast<unsigned char>(buf[2]) == kUtf8BomByte2) {
        return kUtf8BomByteLen;
    }
    return 0;
}

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[i + 1]) {
        case 'r':
            out += '\r';
            ++i;
            break;
        case 'n':
            out += '\n';
            ++i;
            break;
        case 't':
            out += '\t';
            ++i;
            break;
        case '\\':
            out += '\\';
            ++i;
            break;
        default:
            out += s[i];
            break;
        }
    }
    s.swap(out);
}

int ParseTsvBuffer(const char* buf, std::size_t len, TranslationMap& out) {
    if (!buf || len == 0) return 0;

    int count = 0;
    std::size_t lineStart = 0;

    for (std::size_t i = 0; i <= len; ++i) {
        if (i != len && buf[i] != '\n') continue;

        std::size_t lineEnd = i;
        if (lineEnd > lineStart && buf[lineEnd - 1] == '\r') {
            --lineEnd;
        }

        const std::size_t curStart = lineStart;
        lineStart = i + 1;
        if (lineEnd <= curStart) continue;

        std::size_t tabPos = std::string::npos;
        for (std::size_t j = curStart; j < lineEnd; ++j) {
            if (buf[j] == '\t') {
                tabPos = j;
                break;
            }
        }
        if (tabPos == std::string::npos || tabPos <= curStart || tabPos + 1 >= lineEnd) {
            continue;
        }

        std::string jp(buf + curStart, tabPos - curStart);
        std::string en(buf + tabPos + 1, lineEnd - (tabPos + 1));

        UnescapeInPlace(jp);
        UnescapeInPlace(en);

        out[std::move(jp)] = std::move(en);
        ++count;
    }

    return count;
}

}  // namespace translator_logic
