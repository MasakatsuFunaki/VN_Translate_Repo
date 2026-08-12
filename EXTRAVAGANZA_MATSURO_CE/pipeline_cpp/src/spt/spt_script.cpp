// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "spt_script.h"

#include <algorithm>
#include <cstring>

namespace exm::spt {

namespace {

// Codepoint count -- every length test in classify() counts characters, not
// bytes: a 6-character Japanese name plate is 18 bytes in UTF-8, so a byte
// test would classify it as narrative.
std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

bool all_ascii(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size())
        if (utf8_next(utf8, i) > 0x7F) return false;
    return true;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Three-byte-or-shorter strings that are really opcode operands, not text.
bool is_bytecode(std::size_t byte_len, const std::string& text) {
    if (byte_len > 3) return false;
    static const char* const NOISE[] = {"ff", "UU", "VU", "33", "DD",
                                        "22", "f",  "U",  "V",  "D"};
    for (const char* n : NOISE)
        if (text == n) return true;
    return false;
}

}  // namespace

Bytes decrypt(const Bytes& data) {
    Bytes out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) out[i] = static_cast<std::uint8_t>(data[i] ^ 0xFF);
    return out;
}

bool is_valid(const Bytes& dec) {
    return dec.size() >= 0x44 && std::memcmp(dec.data() + 0x30, "SPTHEADER0", 10) == 0;
}

std::uint32_t header_string_count(const Bytes& dec) {
    if (dec.size() < 0x1C) return 0;
    return static_cast<std::uint32_t>(dec[0x18]) |
           (static_cast<std::uint32_t>(dec[0x19]) << 8) |
           (static_cast<std::uint32_t>(dec[0x1A]) << 16) |
           (static_cast<std::uint32_t>(dec[0x1B]) << 24);
}

bool has_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= 0x3040 && cp <= 0x309F) ||   // Hiragana
            (cp >= 0x30A0 && cp <= 0x30FF) ||   // Katakana
            (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
            (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Extension A
            (cp >= 0xFF00 && cp <= 0xFFEF) ||   // Fullwidth forms
            (cp >= 0x3000 && cp <= 0x303F))     // CJK Symbols
            return true;
    }
    return false;
}

std::string classify(const std::string& raw) {
    const std::string text = trim(raw);
    if (text.empty()) return "empty";

    const std::size_t crlf = text.find("\r\n");
    if (crlf != std::string::npos) {
        // First segment short and unquoted -> a speaker plate, so dialogue.
        const std::string first = text.substr(0, crlf);
        if (char_len(first) <= 10 && first.find("\xE3\x80\x8C") == std::string::npos)  // 「
            return "dialogue";
        return "narrative";
    }
    if (text.find("\xE3\x80\x8C") != std::string::npos &&     // 「
        text.find("\xE3\x80\x8D") != std::string::npos)       // 」
        return "dialogue";
    if (starts_with(text, "\xE2\x98\x85") ||   // ★
        starts_with(text, "\xE2\x96\xA0") ||   // ■
        starts_with(text, "\xE2\x97\x8F"))     // ●
        return "menu";
    if (all_ascii(text)) return "label";
    if (char_len(text) <= 6 && has_japanese(text)) return "name";
    if (has_japanese(text)) return "narrative";
    return "other";
}

std::vector<SptString> extract_strings(const Bytes& dec, std::size_t skip_offset) {
    std::vector<SptString> out;
    const std::size_t fsize = dec.size();
    std::size_t pos = skip_offset;

    while (pos < fsize) {
        const auto* nul = static_cast<const std::uint8_t*>(
            std::memchr(dec.data() + pos, 0, fsize - pos));
        if (!nul) break;
        const std::size_t end = static_cast<std::size_t>(nul - dec.data());
        if (end == pos) {
            ++pos;
            continue;
        }

        const std::size_t byte_len = end - pos;
        // Strict decode only: an undecodable run is opcode data, not text, and
        // is skipped rather than salvaged.
        if (auto text = cp932_to_utf8_strict(dec.data() + pos, byte_len)) {
            const bool jp = has_japanese(*text);
            if (!is_bytecode(byte_len, *text) && (jp || byte_len >= 4)) {
                SptString s;
                s.offset = static_cast<std::uint32_t>(pos);
                s.byte_len = static_cast<std::uint32_t>(byte_len);
                s.text = *text;
                s.has_jp = jp;
                out.push_back(std::move(s));
            }
        }

        // SPT text blocks are 4-byte aligned; skip the padding so it doesn't
        // get absorbed as prefix garbage on the next string.
        pos = ((end + 1 + 3) / 4) * 4;
    }
    return out;
}

}  // namespace exm::spt
