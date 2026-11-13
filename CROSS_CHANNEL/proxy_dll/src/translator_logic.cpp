// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace translator_logic {

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

// Lookup with leading-decorator fallback.
//
// The TSV is built from the extract step's control-prefix strip, which walks
// past any leading char that isn't 「」, a Japanese character, …, ―,
// or ～. The engine still emits the un-stripped form at runtime
// ("A定食は…", "※男心を…"), so a literal m.find() misses. Mirror the
// same strip here as a fallback: skip leading non-kept bytes and retry.
namespace {

std::size_t NonKeptCharByteLen(const std::string& jp, std::size_t i) {
    const unsigned char b0 = static_cast<unsigned char>(jp[i]);
    if (b0 < 0x80) return 1;

    if (!IsCp932LeadByte(b0)) return 0;
    if (i + 1 >= jp.size()) return 0;
    const unsigned char b1 = static_cast<unsigned char>(jp[i + 1]);

    if (b0 == 0x81) {
        switch (b1) {
            case 0x40: case 0x41: case 0x42:
            case 0x45: case 0x5B: case 0x5C:
            case 0x60: case 0x63:
            case 0x6B: case 0x6C:
            case 0x75: case 0x76: case 0x77: case 0x78:
            case 0x79: case 0x7A:
                return 0;
            default:
                return 2;
        }
    }
    if (b0 == 0x82) {
        if ((b1 >= 0x4F && b1 <= 0x58) ||
            (b1 >= 0x60 && b1 <= 0x79) ||
            (b1 >= 0x81 && b1 <= 0x9A)) {
            return 2;
        }
        return 0;
    }
    return 0;
}

}  // namespace

const std::string* LookupWithStrippedPrefix(const TranslationMap& m,
                                            const std::string& jp) {
    const auto it = m.find(jp);
    if (it != m.end()) return &it->second;

    constexpr std::size_t kMaxSkipBytes = 16;
    std::size_t i = 0;
    while (i < jp.size() && i < kMaxSkipBytes) {
        const std::size_t step = NonKeptCharByteLen(jp, i);
        if (step == 0) break;
        i += step;
    }
    if (i == 0 || i >= jp.size()) return nullptr;

    const auto it2 = m.find(jp.substr(i));
    if (it2 != m.end()) return &it2->second;
    return nullptr;
}

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
        const std::size_t jpLen = jp.size();

        unsigned char* it = begin;
        while (it + jpLen < end) {
            unsigned char* found = std::search(it, end, jpData, jpData + jpLen);
            if (found == end) break;

            unsigned char* afterJp = found + jpLen;
            const bool validEnd = (afterJp < end) && (*afterJp == 0);
            const bool validStart = (found == begin) || (*(found - 1) == 0);
            if (validEnd && validStart) {
                std::size_t slotLen = jpLen;
                if (en.size() > jpLen) {
                    const std::size_t need = en.size() - jpLen;
                    unsigned char* scan = found + jpLen + 1;
                    std::size_t avail = 0;
                    while (avail < need && scan + avail < end &&
                           *(scan + avail) == 0) {
                        ++avail;
                    }
                    if (avail >= need) {
                        slotLen = jpLen + 1 + avail;
                    }
                }

                std::string chosen;
                if (en.size() <= slotLen) {
                    chosen = en;
                }
                if (!chosen.empty()) {
                    std::memcpy(found, chosen.data(), chosen.size());
                    if (chosen.size() < slotLen) {
                        std::memset(found + chosen.size(), 0x20,
                                    slotLen - chosen.size());
                    }
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

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
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
        size_t eol = i;
        while (eol < len && data[eol] != '\n') ++eol;
        size_t line_end = eol;
        if (line_end > i && data[line_end - 1] == '\r') --line_end;

        size_t tab = i;
        while (tab < line_end && data[tab] != '\t') ++tab;

        if (tab < line_end) {
            std::string jp(data + i, tab - i);
            std::string en(data + tab + 1, line_end - tab - 1);
            UnescapeInPlace(jp);
            UnescapeInPlace(en);
            if (!jp.empty() && !en.empty()) {
                const std::string key = Utf8ToCp932(jp);
                out[key] = en;
                ++added;
            }
        }

        i = eol + 1;
    }
    return added;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out) {
    if (path.empty()) {
        return false;
    }
    std::ifstream f;
    try {
        f.open(std::filesystem::u8path(path), std::ios::binary);
    } catch (const std::exception&) {
        return false;
    }
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string buf = ss.str();
    const int n = ParseTsvBuffer(buf.data(), buf.size(), out);
    return n > 0;
}

bool IsCp932LeadByte(unsigned char b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

bool HasJPLeadByte(const char* data, size_t len) {
    for (size_t i = 0; i + 1 < len; ++i) {
        const unsigned char b = static_cast<unsigned char>(data[i]);
        if (IsCp932LeadByte(b)) return true;
    }
    return false;
}

std::string Utf8ToCp932(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                       static_cast<int>(utf8.size()),
                                       nullptr, 0);
    if (wn <= 0) return utf8;
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    const int w_written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                              static_cast<int>(utf8.size()),
                                              &w[0], wn);
    if (w_written != wn) return utf8;
    const int bn = WideCharToMultiByte(932, 0, w.data(), wn, nullptr, 0,
                                       "?", nullptr);
    if (bn <= 0) return utf8;
    std::string out(static_cast<std::size_t>(bn), '\0');
    const int b_written = WideCharToMultiByte(932, 0, w.data(), wn, &out[0], bn,
                                              "?", nullptr);
    if (b_written != bn) return utf8;
    return out;
#else
    return utf8;
#endif
}

std::string Cp932ToUtf8(const std::string& cp932) {
#ifdef _WIN32
    if (cp932.empty()) return {};
    const int wn = MultiByteToWideChar(932, 0, cp932.data(),
                                       static_cast<int>(cp932.size()),
                                       nullptr, 0);
    if (wn <= 0) return cp932;
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    const int w_written = MultiByteToWideChar(932, 0, cp932.data(),
                                              static_cast<int>(cp932.size()),
                                              &w[0], wn);
    if (w_written != wn) return cp932;
    const int bn = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), wn, nullptr, 0,
                                       nullptr, nullptr);
    if (bn <= 0) return cp932;
    std::string out(static_cast<std::size_t>(bn), '\0');
    const int b_written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), wn, &out[0], bn,
                                              nullptr, nullptr);
    if (b_written != bn) return cp932;
    return out;
#else
    return cp932;
#endif
}

}  // namespace translator_logic
