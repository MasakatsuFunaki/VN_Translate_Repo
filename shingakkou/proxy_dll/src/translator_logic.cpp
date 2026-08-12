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
    // Behavior must match the pre-extraction loop byte-for-byte:
    //   {TAB} -> \t      5 chars consumed
    //   {CR}  -> \r      4 chars consumed
    //   {LF}  -> \n      4 chars consumed
    //   anything else    copied verbatim (so "{NOPE}" stays literal,
    //                    which is important -- a handful of TSV
    //                    entries contain curly braces on purpose).
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{') {
            if (s.compare(i, 5, "{TAB}") == 0) { out += '\t'; i += 4; continue; }
            if (s.compare(i, 4, "{CR}")  == 0) { out += '\r'; i += 3; continue; }
            if (s.compare(i, 4, "{LF}")  == 0) { out += '\n'; i += 3; continue; }
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

int ParseTsvBuffer(const char* data, std::size_t len, Utf8TranslationMap& out)
{
    int count = 0;
    std::size_t i = 0;
    while (i < len) {
        const std::size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        const std::size_t line_end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) i++;

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

// --- Big-endian pack/unpack ---------------------------------------------

std::uint16_t ReadBE16(const unsigned char* p)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
         static_cast<std::uint16_t>(p[1]));
}

void WriteBE16(unsigned char* p, std::uint16_t v)
{
    p[0] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[1] = static_cast<unsigned char>(v        & 0xFF);
}

std::uint32_t ReadBE24(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) <<  8) |
            static_cast<std::uint32_t>(p[2]);
}

void WriteBE24(unsigned char* p, std::uint32_t v)
{
    p[0] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[1] = static_cast<unsigned char>((v >>  8) & 0xFF);
    p[2] = static_cast<unsigned char>( v        & 0xFF);
}

// --- Cumulative-delta offset fixup --------------------------------------

int ApplyCumulativeDelta(std::uint32_t offset,
                         const std::vector<CumulativeDelta>& table)
{
    // Iterate front-to-back, tracking the last entry whose threshold
    // is <= offset. Same loop body as the original code in
    // PatchDecryptedData so behavior is unchanged: offsets before the
    // first threshold get delta 0 (because the loop breaks before
    // assigning adj), offsets at/past a threshold pick up that
    // cum_delta.
    int adj = 0;
    for (const auto& c : table) {
        if (offset < c.threshold) break;
        adj = c.cum_delta;
    }
    return adj;
}

// --- Tail offset-table location -----------------------------------------

OffsetTable FindOffsetTable(const unsigned char* data, std::size_t dataSize)
{
    if (dataSize <= 4 || data[dataSize - 1] != 0xFF)
        return {0, 0};

    // The final byte is the 0xFF terminator, so the last 3-byte entry
    // begins at dataSize - 4. Walk backwards while each entry stays a
    // valid, ascending offset that points at an 'FF 01 80' text-record
    // marker. `next` is the value of the entry we just accepted; earlier
    // entries must not exceed it (the table ascends towards the tail).
    std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(dataSize) - 4;
    std::uint32_t next = static_cast<std::uint32_t>(dataSize);
    std::ptrdiff_t start = -1;

    while (pos >= 0) {
        const std::uint32_t v = ReadBE24(data + pos);
        if (v == 0 || v >= dataSize) break;          // out of range
        if (v > next) break;                          // not ascending
        if (v + 5 >= dataSize ||                      // marker must fit
            data[v + 3] != 0xFF ||
            data[v + 4] != 0x01 ||
            data[v + 5] != 0x80) break;               // not a text record
        next = v;
        start = pos;
        pos -= 3;
    }

    if (start < 0) return {0, 0};

    const int count = static_cast<int>(
        (static_cast<std::ptrdiff_t>(dataSize) - 1 - start) / 3);
    return {static_cast<std::size_t>(start), count};
}

// --- Choice-destination table relocation ---------------------------------

int RelocateChoiceTables(const unsigned char* data, std::size_t dataSize,
                         unsigned char* newData, std::size_t newDataSize,
                         const std::vector<CumulativeDelta>& cumulative)
{
    // A choice destination must land on a code-like boundary: either an
    // '03 0d xx' statement preamble or a '00 3f xx' expression token. These
    // are the only two target shapes the real tables use; requiring it of
    // every entry rejects coincidental 2A 10 FF 00 byte runs.
    auto codeLike = [&](std::uint32_t v) -> bool {
        if (v < 0x10 || v + 6 > dataSize) return false;
        return (data[v] == 0x03 && data[v + 1] == 0x0d) ||
               (data[v] == 0x00 && data[v + 1] == 0x3f);
    };

    int relocated = 0;
    if (dataSize < 5) return 0;

    for (std::size_t i = 0; i + 5 <= dataSize; i++) {
        if (data[i]     != 0x2A || data[i + 1] != 0x10 ||
            data[i + 2] != 0xFF || data[i + 3] != 0x00)
            continue;

        const unsigned count = data[i + 4];
        if (count == 0 || count > 16) continue;
        if (i + 5 + static_cast<std::size_t>(count) * 3 > dataSize) continue;

        // Validate every entry before touching the buffer (all-or-nothing).
        bool ok = true;
        for (unsigned e = 0; e < count; e++) {
            if (!codeLike(ReadBE24(data + i + 5 + e * 3))) { ok = false; break; }
        }
        if (!ok) continue;

        // The table body is copied verbatim, so it sits at i shifted by the
        // cumulative delta of all replacements before it; each entry value is
        // shifted by the cumulative delta at the value it points to.
        const std::size_t newTablePos = static_cast<std::size_t>(
            static_cast<int>(i) + ApplyCumulativeDelta(
                static_cast<std::uint32_t>(i), cumulative));

        for (unsigned e = 0; e < count; e++) {
            const std::size_t newEntryPos = newTablePos + 5 + e * 3;
            if (newEntryPos + 3 > newDataSize) break;
            const std::uint32_t val = ReadBE24(data + i + 5 + e * 3);
            const std::uint32_t nv = static_cast<std::uint32_t>(
                static_cast<int>(val) + ApplyCumulativeDelta(val, cumulative));
            WriteBE24(newData + newEntryPos, nv);
            relocated++;
        }
    }
    return relocated;
}

// --- Message-window line-spacing patch ------------------------------------

int PatchMessageLineSpacing(unsigned char* data, std::size_t dataSize,
                            unsigned char newSpacing)
{
    // var[0x2D1] = 30 :  var-ref token (3F 02 D1), imm8 push (0D 1E),
    // store op (40). See translator_logic.h for why this byte sequence
    // is unique to the system scripts' spacing assignment.
    static const unsigned char kPattern[6] = {0x3F, 0x02, 0xD1, 0x0D, 0x1E, 0x40};

    int patched = 0;
    if (dataSize < sizeof(kPattern)) return 0;

    for (std::size_t i = 0; i + sizeof(kPattern) <= dataSize; i++) {
        bool match = true;
        for (std::size_t k = 0; k < sizeof(kPattern); k++) {
            if (data[i + k] != kPattern[k]) { match = false; break; }
        }
        if (match) {
            data[i + 4] = newSpacing;   // the imm8 operand (was 0x1E = 30)
            patched++;
        }
    }
    return patched;
}

// --- Message text-surface height patch -------------------------------------

int PatchTextSurfaceHeight(unsigned char* data, std::size_t dataSize,
                           unsigned char newLineCapacity)
{
    // op15 create surface 130: 15 | imm16 0x82 | x=0 | y=0 |
    // w = imm16 546 + 4 | h = imm8 26 * 4 (each arg FF-terminated).
    // See translator_logic.h; occurs exactly once game-wide (Bootup
    // entry 0).
    static const unsigned char kPattern[19] = {
        0x15, 0x0E, 0x00, 0x82, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
        0x0E, 0x02, 0x22, 0x04, 0x60, 0xFF, 0x0D, 0x1A, 0x04, 0x68};

    int patched = 0;
    if (dataSize < sizeof(kPattern)) return 0;

    for (std::size_t i = 0; i + sizeof(kPattern) <= dataSize; i++) {
        bool match = true;
        for (std::size_t k = 0; k < sizeof(kPattern); k++) {
            if (data[i + k] != kPattern[k]) { match = false; break; }
        }
        if (match) {
            data[i + 17] = newLineCapacity;  // the multiplier (was 4 rows)
            patched++;
        }
    }
    return patched;
}

// --- English word wrap for the message window -------------------------------

int EngineCharWidthPx(wchar_t c)
{
    // Mirror of the engine's per-char measure (FUN_00402860, fixed-pitch
    // path) with the proportionalizer patches applied: halfwidth chars
    // measure 8px (patch #2), everything else the full cell 21px.
    // Halfwidth = code < 0xFF excluding 0xB1/0xD7/0xF7 (the +-/x/÷ signs),
    // plus the halfwidth-katakana block FF61..FF9F.
    const unsigned int o = static_cast<unsigned int>(c);
    if ((o < 0xFF && o != 0xB1 && o != 0xD7 && o != 0xF7) ||
        (o >= 0xFF61 && o <= 0xFF9F))
        return 8;
    return 21;
}

int WordWrapMessage(std::wstring& text, int budgetPx)
{
    // Greedy word wrap: walk the string with the engine's own width
    // metric; when a char would overflow the budget, replace the LAST
    // space of the current line with '\r' (the engine's hard-break char,
    // consumed by the 0x6A mode-1 wrap). One-for-one char swap: the
    // string length never changes, so replacement byte sizes and all
    // offset relocation downstream are unaffected.
    int breaks = 0;
    int lineWidth = 0;            // width of the current line so far
    std::ptrdiff_t lastSpace = -1;// index of the latest space in this line
    int widthAfterSpace = 0;      // width accumulated after that space

    for (std::size_t i = 0; i < text.size(); i++) {
        const wchar_t c = text[i];
        if (c == L'\r') {         // pre-existing hard break
            lineWidth = 0; lastSpace = -1; widthAfterSpace = 0;
            continue;
        }
        const int w = EngineCharWidthPx(c);
        if (c == L' ') {
            lineWidth += w;
            lastSpace = static_cast<std::ptrdiff_t>(i);
            widthAfterSpace = 0;
            continue;
        }
        if (lineWidth + w > budgetPx) {
            if (lastSpace >= 0) {
                text[static_cast<std::size_t>(lastSpace)] = L'\r';
                breaks++;
                lineWidth = widthAfterSpace + w;
                lastSpace = -1; widthAfterSpace = 0;
            } else {
                // Word longer than a whole line: leave it to the engine's
                // char-granularity break (it resets the line to this char).
                lineWidth = w;
            }
        } else {
            lineWidth += w;
        }
        if (lastSpace >= 0) widthAfterSpace += w;
    }
    return breaks;
}

// --- Message-window background-refresh band patch ---------------------------

int PatchMessageWindowRefreshTop(unsigned char* data, std::size_t dataSize,
                                 unsigned char newTopOffset)
{
    // op16 blit: window background (layer 131) re-stamped over the text
    // area at the start of every message. Args, each FF-terminated:
    //   src layer 131 | src x = var[0x2D2] | src y = var[0x2D3] + imm8 36 |
    //   w = 546+4 | h = imm8 86 + 4 | dst layer 1 | dst x = var[0x2D2] |
    //   dst y = var[0x2D3] + imm8 36 | ...
    // See translator_logic.h for why the 36 must follow the line spacing.
    static const unsigned char kPattern[40] = {
        0x16, 0x0E, 0x00, 0x83, 0xFF,                    // op16, src layer 131
        0x3F, 0x02, 0xD2, 0xFF,                          // src x = var[0x2D2]
        0x3F, 0x02, 0xD3, 0x0D, 0x24, 0x60, 0xFF,        // src y = var[0x2D3]+36
        0x0E, 0x02, 0x22, 0x04, 0x60, 0xFF,              // w = 546+4
        0x0D, 0x56, 0x04, 0x60, 0xFF,                    // h = 86+4
        0x01, 0xFF,                                      // dst layer 1
        0x3F, 0x02, 0xD2, 0xFF,                          // dst x = var[0x2D2]
        0x3F, 0x02, 0xD3, 0x0D, 0x24, 0x60, 0xFF};       // dst y = var[0x2D3]+36

    int patched = 0;
    if (dataSize < sizeof(kPattern)) return 0;

    // Keep the bottom edge at +126 (= 36 + 86 + 4): height imm8 grows by
    // exactly what the top moved up.
    const unsigned char newHeight =
        static_cast<unsigned char>(122 - newTopOffset);

    for (std::size_t i = 0; i + sizeof(kPattern) <= dataSize; i++) {
        bool match = true;
        for (std::size_t k = 0; k < sizeof(kPattern); k++) {
            if (data[i + k] != kPattern[k]) { match = false; break; }
        }
        if (match) {
            data[i + 13] = newTopOffset;  // src y imm8 (was 36)
            data[i + 23] = newHeight;     // height imm8 (was 86; expr adds 4)
            data[i + 37] = newTopOffset;  // dst y imm8 (was 36)
            patched++;
        }
    }
    return patched;
}

}  // namespace translator_logic
