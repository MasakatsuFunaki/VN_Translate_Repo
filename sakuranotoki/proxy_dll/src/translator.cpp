// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"

#include "log.h"
#include "translator_logic.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT kCp932 = 932;
constexpr DWORD kInitialModulePathChars = MAX_PATH;
constexpr DWORD kMaxModulePathChars = 32768;
constexpr DWORD kPathBufferGrowthFactor = 2;
constexpr int kMaxSampledEntries = 5;
constexpr int kInitialMissLogCap = 100;
constexpr int kPeriodicMissLogStride = 50;
constexpr std::string_view kIdeographicSpace = "　";
constexpr std::string_view kLeftCornerBracket = "「";
constexpr std::string_view kRightCornerBracket = "」";

std::unordered_map<std::string, std::string> g_table;
std::once_flag g_init_flag;

std::atomic<int> g_lookup_total{0};
std::atomic<int> g_lookup_hit_exact{0};
std::atomic<int> g_lookup_hit_norm{0};
std::atomic<int> g_lookup_miss{0};

std::string get_dll_dir() {
    HMODULE me = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&get_dll_dir), &me)) {
        return ".";
    }
    DWORD cap = kInitialModulePathChars;
    std::vector<wchar_t> wbuf(cap, L'\0');
    DWORD len = 0;
    while (cap <= kMaxModulePathChars) {
        len = GetModuleFileNameW(me, wbuf.data(), cap);
        if (len == 0) {
            return ".";
        }
        if (len < cap) {
            break;
        }
        if (cap == kMaxModulePathChars) {
            cap = kMaxModulePathChars + 1;
            break;
        }
        cap = std::min(cap * kPathBufferGrowthFactor, kMaxModulePathChars);
        wbuf.assign(cap, L'\0');
    }
    if (cap > kMaxModulePathChars) {
        return ".";
    }
    const int u8len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wbuf.data(), static_cast<int>(len),
        nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) {
        return ".";
    }
    std::string s(static_cast<std::size_t>(u8len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wbuf.data(), static_cast<int>(len),
        s.data(), u8len, nullptr, nullptr);
    if (written != u8len) {
        return ".";
    }
    const auto pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? "." : s.substr(0, pos);
}

std::string utf8_from_cp932(const char* in) {
    if (!in || !*in) {
        return {};
    }
    const int wlen = MultiByteToWideChar(kCp932, 0, in, -1, nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring w(static_cast<std::size_t>(wlen) - 1, L'\0');
    MultiByteToWideChar(kCp932, 0, in, -1, w.data(), wlen);
    const int u8len = WideCharToMultiByte(
        CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(u8len) - 1, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w.c_str(), -1, out.data(), u8len, nullptr, nullptr);
    return out;
}

// Engine wrappers the bytecode-extracted strings sometimes lack and
// sometimes embed. Stripping leading 　 (U+3000) and surrounding 「...」
// before retrying the lookup lets the same TSV match buffers handed to
// the renderer in either form.
std::string normalize_utf8_key(const std::string& s) {
    std::string_view view(s);
    while (view.substr(0, kIdeographicSpace.size()) == kIdeographicSpace) {
        view.remove_prefix(kIdeographicSpace.size());
    }
    if (view.size() >= kLeftCornerBracket.size() + kRightCornerBracket.size() &&
        view.substr(0, kLeftCornerBracket.size()) == kLeftCornerBracket &&
        view.substr(view.size() - kRightCornerBracket.size()) == kRightCornerBracket) {
        view.remove_prefix(kLeftCornerBracket.size());
        view.remove_suffix(kRightCornerBracket.size());
        while (view.substr(0, kIdeographicSpace.size()) == kIdeographicSpace) {
            view.remove_prefix(kIdeographicSpace.size());
        }
    }
    return std::string(view);
}

void load_table_once() {
    std::call_once(g_init_flag, []() {
        const std::string path = get_dll_dir() + "\\translations.tsv";
        const std::size_t n = tr::load_tsv_file(path, g_table);
        if (n == SIZE_MAX) {
            LOG("translator: FAILED to open %s", path.c_str());
            return;
        }
        LOG("translator: loaded %zu entries from %s", n, path.c_str());
        int sampled = 0;
        for (const auto& kv : g_table) {
            LOG("translator: sample [%s] -> [%s]",
                kv.first.c_str(), kv.second.c_str());
            if (++sampled >= kMaxSampledEntries) {
                break;
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
    const std::string norm = normalize_utf8_key(utf8_key);
    if (!norm.empty() && norm.size() != utf8_key.size()) {
        it = g_table.find(norm);
        if (it != g_table.end()) {
            g_lookup_hit_norm++;
            return &it->second;
        }
    }
    g_lookup_miss++;
    const int miss = g_lookup_miss.load();
    if (miss <= kInitialMissLogCap || (miss % kPeriodicMissLogStride) == 0) {
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
