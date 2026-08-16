// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace translator_logic {

// --- String helpers ------------------------------------------------------

void UnescapeInPlace(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
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

bool StripLeadingControlNewline(std::string& s)
{
    if (s.size() >= 2 && s[0] == '\\' && s[1] == 'n') {
        s.replace(0, 2, " ");
        return true;
    }
    return false;
}

size_t WordWrapForEngine(std::string& s, size_t line_chars, size_t start_col)
{
    std::string out;
    out.reserve(s.size() + 64);
    size_t col = start_col;
    size_t i   = 0;

    while (i < s.size()) {
        // Hard break control code `\n` (backslash + 'n'): pass through,
        // reset column so the next word starts fresh.
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\\';
            out += 'n';
            i += 2;
            col = 0;
            continue;
        }

        // Space run: defer emission until we know the next word fits.
        // If the next word would overflow, replace the spaces with a
        // `\n` break instead (no trailing space before the break).
        if (s[i] == ' ') {
            size_t sp = 0;
            while (i + sp < s.size() && s[i + sp] == ' ') ++sp;

            // Measure the word that follows the spaces.
            size_t wstart = i + sp;
            size_t wend   = wstart;
            while (wend < s.size() && s[wend] != ' ' &&
                   !(s[wend] == '\\' && wend + 1 < s.size() && s[wend + 1] == 'n')) {
                ++wend;
            }
            size_t wlen = wend - wstart;

            // Wrap if the next word doesn't fit, but only when we're
            // actually mid-line (col > 0 -- don't wrap at line start).
            // start_col >= line_chars: prior MESSAGE already overflowed
            // the line, so break before our first word too.
            if (wlen > 0 && col > 0 && col + sp + wlen > line_chars) {
                out += '\\';
                out += 'n';
                col = 0;
                i  += sp;   // skip the spaces (no trailing space after break)
            } else {
                for (size_t k = 0; k < sp; ++k) out += ' ';
                col += sp;
                i   += sp;
            }
            continue;
        }

        // Word token: collect and emit. If we are at the very start of
        // `s` and `start_col` already exceeds `line_chars`, the first
        // word would land on an overflowed line. We can't insert a
        // break before the word without a preceding space (the engine
        // would render the resulting `\n` against the prior MESSAGE's
        // last character, which is fine), so emit `\n` first.
        if (col > line_chars && col == start_col && i == 0) {
            out += '\\';
            out += 'n';
            col = 0;
        }

        size_t wstart = i;
        while (i < s.size() && s[i] != ' ' &&
               !(s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n')) {
            ++i;
        }
        out.append(s, wstart, i - wstart);
        col += i - wstart;
    }

    s = std::move(out);
    return col;
}

size_t Utf8SafeTruncate(const char* s, size_t max_bytes)
{
    if (!s) return 0;
    size_t i = 0;
    while (i < max_bytes && s[i]) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t cl = 1;
        if      ((c & 0x80) == 0x00) cl = 1;  // ASCII
        else if ((c & 0xE0) == 0xC0) cl = 2;  // 2-byte
        else if ((c & 0xF0) == 0xE0) cl = 3;  // 3-byte
        else if ((c & 0xF8) == 0xF0) cl = 4;  // 4-byte
        // else: malformed continuation byte. Treat as single byte so
        // we don't loop; the copy routine will just truncate here.
        if (i + cl > max_bytes) break;
        i += cl;
    }
    return i;
}

// --- TSV loading ---------------------------------------------------------

int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out)
{
    if (!data || len == 0) return 0;

    // Skip UTF-8 BOM if present.
    size_t start = 0;
    if (len >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        start = 3;
    }

    int count = 0;
    size_t i = start;
    while (i < len) {
        size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        size_t line_end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) i++;

        size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') tab++;
        if (tab == line_start || tab >= line_end) continue;

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + 1, line_end - (tab + 1));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        // NOTE: leading `\n` (backslash + 'n') is intentionally
        // PRESERVED on the EN value. It marks the entry as a
        // continuation of the previous MESSAGE in the same textbox so
        // the runtime can carry its running column counter across the
        // MESSAGE boundary and wrap correctly. The runtime
        // (TranslateTextInner) is responsible for softening the marker
        // to a space before handing the text off to the engine.
        if (jp.empty() || en.empty()) continue;
        out[jp] = en;
        count++;
    }
    return count;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    std::vector<char> buf(static_cast<size_t>(sz));
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n == 0) return false;
    int added = ParseTsvBuffer(buf.data(), n, out);
    return added > 0;
}

// --- Lookup --------------------------------------------------------------

const std::string* FindTranslation(const TranslationMap& map,
                                   const std::string& src)
{
    auto it = map.find(src);
    return (it != map.end()) ? &it->second : nullptr;
}

}  // namespace translator_logic
