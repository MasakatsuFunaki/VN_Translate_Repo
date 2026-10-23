// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"
#include "translator_logic.h"
#include "log.h"

#include <windows.h>
#include <unordered_map>
#include <string>
#include <mutex>
#include <atomic>

namespace {

std::unordered_map<std::string, std::string> g_table;
std::once_flag g_init_flag;

std::atomic<int> g_lookup_total{0};
std::atomic<int> g_lookup_hit_exact{0};
std::atomic<int> g_lookup_hit_norm{0};
std::atomic<int> g_lookup_miss{0};

std::string get_dll_dir() {
    HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&get_dll_dir), &me);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(me, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? "." : s.substr(0, pos);
}

std::string utf8_from_cp932(const char* in) {
    if (!in || !*in) return {};
    int wlen = MultiByteToWideChar(932, 0, in, -1, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w(static_cast<size_t>(wlen) - 1, L'\0');
    MultiByteToWideChar(932, 0, in, -1, w.data(), wlen);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return {};
    std::string out(static_cast<size_t>(u8len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), u8len,
                        nullptr, nullptr);
    return out;
}

// Engine wrappers the bytecode-extracted strings sometimes lack and
// sometimes embed. Stripping leading 　 (U+3000) and surrounding 「...」
// before retrying the lookup lets the same TSV match buffers handed to
// the renderer in either form. UTF-8 byte sequences:
//   　   E3 80 80
//   「   E3 80 8C
//   」   E3 80 8D
std::string normalize_utf8_key(const std::string& s) {
    auto starts = [&](size_t i, unsigned a, unsigned b, unsigned c) {
        return s.size() >= i + 3
            && (unsigned char)s[i]   == a
            && (unsigned char)s[i+1] == b
            && (unsigned char)s[i+2] == c;
    };
    size_t lo = 0, hi = s.size();
    while (starts(lo, 0xE3, 0x80, 0x80)) lo += 3;
    if (starts(lo, 0xE3, 0x80, 0x8C) && hi >= 3
        && (unsigned char)s[hi-3] == 0xE3
        && (unsigned char)s[hi-2] == 0x80
        && (unsigned char)s[hi-1] == 0x8D) {
        lo += 3;
        hi -= 3;
        while (lo < hi && starts(lo, 0xE3, 0x80, 0x80)) lo += 3;
    }
    if (lo >= hi) return {};
    return s.substr(lo, hi - lo);
}

void load_table_once() {
    std::call_once(g_init_flag, []() {
        std::string path = get_dll_dir() + "\\translations.tsv";
        size_t n = tr::load_tsv_file(path, g_table);
        if (n == SIZE_MAX) {
            LOG("translator: FAILED to open %s", path.c_str());
        } else {
            LOG("translator: loaded %zu entries from %s", n, path.c_str());
            // Sample 5 entries so the log makes it obvious the table is
            // populated and the keys are readable JP.
            int sampled = 0;
            for (const auto& kv : g_table) {
                LOG("translator: sample [%s] -> [%s]",
                    kv.first.c_str(), kv.second.c_str());
                if (++sampled >= 5) break;
            }
        }
    });
}

const std::string* find_with_normalize(const std::string& utf8_key) {
    auto it = g_table.find(utf8_key);
    if (it != g_table.end()) {
        g_lookup_hit_exact++;
        return &it->second;
    }
    std::string norm = normalize_utf8_key(utf8_key);
    if (!norm.empty() && norm.size() != utf8_key.size()) {
        it = g_table.find(norm);
        if (it != g_table.end()) {
            g_lookup_hit_norm++;
            return &it->second;
        }
    }
    g_lookup_miss++;
    int miss = g_lookup_miss.load();
    if (miss <= 100 || (miss % 50) == 0) {
        if (norm.empty() || norm.size() == utf8_key.size()) {
            LOG("translator: MISS #%d [%s]", miss, utf8_key.c_str());
        } else {
            LOG("translator: MISS #%d [%s] (norm tried: [%s])",
                miss, utf8_key.c_str(), norm.c_str());
        }
    }
    return nullptr;
}

}  // namespace

namespace tr_runtime {

void init() { load_table_once(); }

void log_summary() {
    LOG("translator: summary total=%d exact=%d norm=%d miss=%d",
        g_lookup_total.load(), g_lookup_hit_exact.load(),
        g_lookup_hit_norm.load(), g_lookup_miss.load());
}

const char* translate_jp_to_en_utf8(const char* jp_utf8) {
    if (!jp_utf8 || !*jp_utf8) return nullptr;
    load_table_once();
    g_lookup_total++;
    const std::string key(jp_utf8);
    const std::string* en = find_with_normalize(key);
    return en ? en->c_str() : nullptr;
}

const char* translate_jp_to_en_cp932(const char* jp_cp932) {
    if (!jp_cp932 || !*jp_cp932) return nullptr;
    load_table_once();
    g_lookup_total++;
    std::string utf8 = utf8_from_cp932(jp_cp932);
    if (utf8.empty()) return nullptr;
    const std::string* en = find_with_normalize(utf8);
    return en ? en->c_str() : nullptr;
}

}  // namespace tr_runtime
