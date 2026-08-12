// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace translator_logic {

// --- Choice-text helpers --------------------------------------------

bool IsChoiceLikeJp(const std::string& jp) {
    return jp.size() >= 10 && jp.size() <= 14;
}

std::string FitToSlot(const std::string& en, std::size_t slot) {
    if (en.size() <= slot) return en;
    if (slot == 0) return std::string();
    for (std::size_t i = slot; i > 0; --i) {
        if (en[i - 1] == ' ') return en.substr(0, i - 1);
    }
    return en.substr(0, slot);
}

// --- Lookup with leading-decorator fallback --------------------------
//
// The TSV is built from the extract step's control-prefix strip, which walks
// past ANY leading char that isn't 「」, a "real Japanese" char, …, ―,
// or ～. The engine still emits the un-stripped form at runtime
// ("A定食は…", "※男心を…"), so a literal m.find() misses. Mirror the
// same strip here as a fallback: skip leading non-kept bytes and retry.
//
// Field bugs that this catches (each grounded in an actual proxy_log
// MISS line):
//   * `81 A6 92 6A 90 53 82 F0 98 68 …`  →  `※男心を鷲…`  (annotation)
//   * `41 92 E8 90 48 82 CD 8E 4F 8E 9E …`  →  `A定食は三時…`  (lettered)

namespace {

// Returns the byte-length to skip if jp[i..] begins a NON-KEPT char per
// strip_control_prefix, or 0 if the char is kept (literal lookup is
// final).  We enumerate the strippable set conservatively — anything
// not explicitly listed is treated as kept, so we never strip a real
// Japanese char by accident (worst case: a new decorator surfaces as
// one PRE-LOOP MISS log line and gets added here).
std::size_t NonKeptCharByteLen(const std::string& jp, std::size_t i) {
    unsigned char b0 = static_cast<unsigned char>(jp[i]);
    if (b0 < 0x80) return 1;                         // ASCII → strip 1 byte

    if (!IsCp932LeadByte(b0)) return 0;
    if (i + 1 >= jp.size())   return 0;
    unsigned char b1 = static_cast<unsigned char>(jp[i + 1]);

    if (b0 == 0x81) {
        // The 0x81 page mixes CJK punctuation (kept) and ASCII-equivalent
        // punctuation / symbols (NOT kept).  Kept subset:
        //   0x40 U+3000 idspace   0x41 U+3001 、   0x42 U+3002 。
        //   0x45 U+30FB ・         0x5B U+30FC ー   0x5C U+2015 ―
        //   0x60 U+FF5E ～         0x63 U+2026 …
        //   0x6B U+3014 〔         0x6C U+3015 〕
        //   0x75 U+300C 「         0x76 U+300D 」
        //   0x77 U+300E 『         0x78 U+300F 』
        //   0x79 U+3010 【         0x7A U+3011 】
        // Strip everything else (※ A6, ★ A8, fullwidth ASCII puncts ! ? : ; etc.).
        switch (b1) {
            case 0x40: case 0x41: case 0x42:
            case 0x45: case 0x5B: case 0x5C:
            case 0x60: case 0x63:
            case 0x6B: case 0x6C:
            case 0x75: case 0x76: case 0x77: case 0x78:
            case 0x79: case 0x7A:
                return 0;                            // kept
            default:
                return 2;                            // strip
        }
    }
    if (b0 == 0x82) {
        // The 0x82 page mixes fullwidth digits/Latin (NOT kept) with
        // hiragana (kept).  Strip the Latin sub-ranges only.
        //   0x4F-0x58 fullwidth digits ０-９
        //   0x60-0x79 fullwidth uppercase Ａ-Ｚ
        //   0x81-0x9A fullwidth lowercase ａ-ｚ
        if ((b1 >= 0x4F && b1 <= 0x58) ||
            (b1 >= 0x60 && b1 <= 0x79) ||
            (b1 >= 0x81 && b1 <= 0x9A))
            return 2;
        return 0;                                    // hiragana etc. → kept
    }
    return 0;                                        // 0x83+ kana/kanji → kept
}

}  // namespace

const std::string* LookupWithStrippedPrefix(const TranslationMap& m,
                                             const std::string& jp) {
    auto it = m.find(jp);
    if (it != m.end()) return &it->second;

    // Walk past leading non-kept chars and try ONE retry once we land on
    // a kept char. Defensive byte cap so a long miss never turns into an
    // O(n) scan.
    constexpr std::size_t kMaxSkipBytes = 16;
    std::size_t i = 0;
    while (i < jp.size() && i < kMaxSkipBytes) {
        std::size_t step = NonKeptCharByteLen(jp, i);
        if (step == 0) break;
        i += step;
    }
    if (i == 0 || i >= jp.size()) return nullptr;

    auto it2 = m.find(jp.substr(i));
    if (it2 != m.end()) return &it2->second;
    return nullptr;
}

// --- In-place patcher ------------------------------------------------

PatchStats PatchTranslationsInPlace(unsigned char* begin, unsigned char* end,
                                     const TranslationMap& translations) {
    PatchStats s;
    if (!begin || !end || end <= begin) return s;

    for (const auto& kv : translations) {
        const std::string& jp = kv.first;
        const std::string& en = kv.second;
        if (jp.empty()) continue;

        const unsigned char* jpData =
            reinterpret_cast<const unsigned char*>(jp.data());
        std::size_t jpLen = jp.size();

        unsigned char* it = begin;
        while (it + jpLen < end) {
            unsigned char* found = std::search(it, end, jpData, jpData + jpLen);
            if (found == end) break;

            unsigned char* afterJp = found + jpLen;
            bool validEnd   = (afterJp < end)  && (*afterJp == 0);
            bool validStart = (found == begin) || (*(found - 1) == 0);
            if (validEnd && validStart) {
                std::size_t slotLen = jpLen;
                if (en.size() > jpLen) {
                    // Steal trailing null padding to grow the writable slot.
                    std::size_t need = en.size() - jpLen;
                    unsigned char* scan = found + jpLen + 1;
                    std::size_t avail = 0;
                    while (avail < need && scan + avail < end &&
                           *(scan + avail) == 0)
                        ++avail;
                    if (avail >= need)
                        slotLen = jpLen + 1 + avail;
                }

                // Decide what bytes to write:
                //   - EN fits the slot → write EN as-is
                //   - EN too long      → skip; the render-time hooks
                //                         handle it (dialogue text hook
                //                         redirects to a DLL buffer; the
                //                         choice entry hook builds its
                //                         own replacement buffer with
                //                         the full EN). Truncating in
                //                         place corrupts the JP "key" the
                //                         dialogue hook looks up later
                //                         and produces visible truncated
                //                         EN on screen ("The wind"
                //                         instead of "The wind blew.").
                std::string chosen;
                if (en.size() <= slotLen) {
                    chosen = en;
                }
                if (!chosen.empty()) {
                    std::memcpy(found, chosen.data(), chosen.size());
                    if (chosen.size() < slotLen)
                        std::memset(found + chosen.size(), 0x20,
                                    slotLen - chosen.size());
                    found[slotLen] = 0;
                    ++s.hits;
                } else {
                    ++s.too_long;
                }
            }
            it = found + 1;
        }
    }
    return s;
}

// --- TSV parsing -----------------------------------------------------

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case 'n':  out += '\n'; ++i; continue;
                case 'r':  out += '\r'; ++i; continue;
                case 't':  out += '\t'; ++i; continue;
                case '\\': out += '\\'; ++i; continue;
            }
        }
        out += c;
    }
    s.swap(out);
}

int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out) {
    int added = 0;
    size_t i = 0;
    while (i < len) {
        // Find end-of-line
        size_t eol = i;
        while (eol < len && data[eol] != '\n') ++eol;
        size_t line_end = eol;
        // Trim trailing \r
        if (line_end > i && data[line_end - 1] == '\r') --line_end;

        // Find tab separator
        size_t tab = i;
        while (tab < line_end && data[tab] != '\t') ++tab;

        if (tab < line_end) {
            std::string jp(data + i, tab - i);
            std::string en(data + tab + 1, line_end - tab - 1);
            UnescapeInPlace(jp);
            UnescapeInPlace(en);
            if (!jp.empty() && !en.empty()) {
                std::string key = Utf8ToCp932(jp);
                out[key] = en;
                ++added;
            }
        }

        i = eol + 1;
    }
    return added;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string buf = ss.str();
    int n = ParseTsvBuffer(buf.data(), buf.size(), out);
    return n > 0;
}

// --- CP932 helpers ---------------------------------------------------

bool IsCp932LeadByte(unsigned char b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

bool HasJPLeadByte(const char* data, size_t len) {
    for (size_t i = 0; i + 1 < len; ++i) {
        unsigned char b = static_cast<unsigned char>(data[i]);
        if (IsCp932LeadByte(b)) return true;
    }
    return false;
}

std::string Utf8ToCp932(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                  static_cast<int>(utf8.size()),
                                  nullptr, 0);
    if (wn <= 0) return utf8;
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                        static_cast<int>(utf8.size()),
                        &w[0], wn);
    int bn = WideCharToMultiByte(932, 0, w.data(), wn, nullptr, 0,
                                  "?", nullptr);
    if (bn <= 0) return utf8;
    std::string out(bn, '\0');
    WideCharToMultiByte(932, 0, w.data(), wn, &out[0], bn,
                        "?", nullptr);
    return out;
#else
    // gtest fixtures on Linux: identity. Tests must use raw CP932 input.
    return utf8;
#endif
}

std::string Cp932ToUtf8(const std::string& cp932) {
#ifdef _WIN32
    if (cp932.empty()) return {};
    int wn = MultiByteToWideChar(932, 0, cp932.data(),
                                  static_cast<int>(cp932.size()),
                                  nullptr, 0);
    if (wn <= 0) return cp932;
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(932, 0, cp932.data(),
                        static_cast<int>(cp932.size()),
                        &w[0], wn);
    int bn = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0,
                                  nullptr, nullptr);
    if (bn <= 0) return cp932;
    std::string out(bn, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], bn,
                        nullptr, nullptr);
    return out;
#else
    return cp932;
#endif
}

}  // namespace translator_logic
