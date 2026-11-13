// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// IAT and direct-export hook translator for Artemis / CrackProof.
//
// Hooks WinAPI CP932 string entry points (lstrlenA, lstrcpyA, lstrcpynA,
// lstrcatA, MultiByteToWideChar) via both sakuranotoki.exe's static IAT and
// prologue trampolines on kernel32 exports, substituting known Japanese keys
// and memory_scanner-patched buffers with untruncated English pool slots.

#include "iat_translator.h"

#include "export_hook.h"
#include "iat_hook.h"
#include "log.h"
#include "translator.h"
#include "translator_logic.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kPoolSlots = 64;
constexpr std::size_t kPoolSlotBytes = 4096;
constexpr UINT kCp932CodePage = 932U;
constexpr DWORD kInitialModulePathChars = MAX_PATH;
constexpr DWORD kMaxModulePathChars = 32768U;
constexpr DWORD kPathBufferGrowthFactor = 2U;
constexpr std::size_t kMinJpKeyBytes = 4;
constexpr int kDetailLogPerHook = 10;
constexpr unsigned char kHighByteThreshold = 0x80U;
constexpr int kMaxProbeStringBytes = 4096;
constexpr std::size_t kLogPreviewBufferBytes = 96;
constexpr int kNullTerminatedSrcLen = -1;

class ScopedCsLock {
public:
    explicit ScopedCsLock(CRITICAL_SECTION& cs) noexcept : cs_(cs) {
        EnterCriticalSection(&cs_);
    }

    ScopedCsLock(const ScopedCsLock&) = delete;
    ScopedCsLock& operator=(const ScopedCsLock&) = delete;

    ~ScopedCsLock() { LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION& cs_;
};

// Round-robin pool providing stable char* slots for substituted English text.
char g_pool[kPoolSlots][kPoolSlotBytes] = {};
std::atomic<int> g_pool_idx{0};

const char* alloc_slot(const char* s, std::size_t n) {
    const int idx =
        g_pool_idx.fetch_add(1, std::memory_order_relaxed) % kPoolSlots;
    char* const slot = g_pool[idx];
    if (n >= kPoolSlotBytes) {
        n = kPoolSlotBytes - 1;
    }
    std::memcpy(slot, s, n);
    slot[n] = '\0';
    return slot;
}

// CP932 JP key -> UTF-8/ASCII EN value, transcoded once from translations.tsv.
std::unordered_map<std::string, std::string> g_translations;
std::once_flag g_load_flag;

std::string utf8_to_cp932(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int src_len = static_cast<int>(utf8.size());
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), src_len, nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src_len, w.data(), wlen);
    const int alen = WideCharToMultiByte(
        kCp932CodePage, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (alen <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(alen), '\0');
    WideCharToMultiByte(
        kCp932CodePage, 0, w.data(), wlen, out.data(), alen, nullptr, nullptr);
    return out;
}

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

void load_table_once() {
    std::call_once(g_load_flag, []() {
        std::unordered_map<std::string, std::string> utf8_map;
        const std::string path = get_dll_dir() + "\\translations.tsv";
        const std::size_t n = tr::load_tsv_file(path, utf8_map);
        if (n == SIZE_MAX || utf8_map.empty()) {
            LOG("iat: TSV load failed (%s)", path.c_str());
            return;
        }
        std::size_t kept = 0;
        std::size_t dropped = 0;
        for (const auto& kv : utf8_map) {
            if (kv.first.size() < kMinJpKeyBytes) {
                continue;
            }
            if (kv.first == kv.second) {
                continue;
            }
            std::string cp932 = utf8_to_cp932(kv.first);
            if (cp932.empty()) {
                ++dropped;
                continue;
            }
            g_translations.emplace(std::move(cp932), kv.second);
            ++kept;
        }
        LOG("iat: loaded %zu CP932 translations (dropped %zu, total %zu utf8)",
            kept, dropped, utf8_map.size());
    });
}

// Registry mapping heap addresses patched by memory_scanner to full English
// strings so length/copy/conversion hooks see untruncated English.
struct PatchedEntry {
    const char* full_en{nullptr};
    int en_len{0};
};

std::unordered_map<DWORD, PatchedEntry> g_patched;
CRITICAL_SECTION g_patched_cs{};
std::atomic<bool> g_patched_init{false};
std::atomic<int> g_patched_count{0};

void ensure_patched_init() {
    if (g_patched_init.load(std::memory_order_acquire)) {
        return;
    }
    static std::once_flag once;
    std::call_once(once, []() {
        InitializeCriticalSection(&g_patched_cs);
        g_patched_init.store(true, std::memory_order_release);
    });
}

bool lookup_patched(const void* addr, PatchedEntry* out) {
    if (!g_patched_init.load(std::memory_order_acquire)) {
        return false;
    }
    if (addr == nullptr || out == nullptr) {
        return false;
    }
    const auto key = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(addr));
    const ScopedCsLock lock(g_patched_cs);
    const auto it = g_patched.find(key);
    if (it == g_patched.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

// Per-hook hit counters and bounded diagnostic logging.
struct HookStats {
    const char* name;
    std::atomic<int> calls{0};
    std::atomic<int> hits{0};
    std::atomic<int> detail_logged{0};
};

HookStats g_st_lstrlenA{"lstrlenA", {0}, {0}, {0}};
HookStats g_st_lstrcpyA{"lstrcpyA", {0}, {0}, {0}};
HookStats g_st_lstrcpynA{"lstrcpynA", {0}, {0}, {0}};
HookStats g_st_lstrcatA{"lstrcatA", {0}, {0}, {0}};
HookStats g_st_MB2WC{"MultiByteToWideChar", {0}, {0}, {0}};

bool has_high_byte(const char* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<unsigned char>(s[i]) >= kHighByteThreshold) {
            return true;
        }
    }
    return false;
}

// SEH-isolated NUL-terminated length probe without C++ unwind objects.
__declspec(noinline)
int safe_strlen_bounded(const char* s, int max_len) {
    __try {
        int n = 0;
        while (n < max_len && s[n] != '\0') {
            ++n;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char* try_translate(const char* src, std::size_t* out_len = nullptr) {
    if (src == nullptr || *src == '\0') {
        return nullptr;
    }
    load_table_once();
    if (g_translations.empty()) {
        return nullptr;
    }

    const int len = safe_strlen_bounded(src, kMaxProbeStringBytes);
    if (len < static_cast<int>(kMinJpKeyBytes)) {
        return nullptr;
    }
    const auto byte_len = static_cast<std::size_t>(len);
    if (!has_high_byte(src, byte_len)) {
        return nullptr;
    }

    const std::string key(src, byte_len);
    const auto it = g_translations.find(key);
    if (it == g_translations.end()) {
        return nullptr;
    }

    const std::string& en = it->second;
    const char* const slot = alloc_slot(en.data(), en.size());
    if (out_len != nullptr) {
        *out_len = en.size();
    }
    return slot;
}

__declspec(noinline)
int safe_copy_bounded(char* dst, const char* src, int max_len) {
    __try {
        int n = 0;
        while (n < max_len - 1 && src[n] != '\0') {
            dst[n] = src[n];
            ++n;
        }
        dst[n] = '\0';
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void log_first_n(HookStats& st,
                 const char* prefix,
                 const char* s,
                 const char* substituted) {
    const int n = st.detail_logged.fetch_add(1, std::memory_order_relaxed);
    if (n >= kDetailLogPerHook) {
        return;
    }
    if (s == nullptr) {
        LOG("iat[%s] #%d: <null>", st.name, n + 1);
        return;
    }
    char preview[kLogPreviewBufferBytes] = {};
    safe_copy_bounded(preview, s, static_cast<int>(sizeof(preview)));

    if (substituted != nullptr) {
        LOG("iat[%s] #%d %s [%s] -> [%s]",
            st.name, n + 1, prefix, preview, substituted);
        return;
    }
    LOG("iat[%s] #%d %s [%s]", st.name, n + 1, prefix, preview);
}

// Real-API pointers populated by IATHook and overridden by ExportHookInstall.
using PFN_lstrlenA = int(WINAPI*)(LPCSTR);
using PFN_lstrcpyA = LPSTR(WINAPI*)(LPSTR, LPCSTR);
using PFN_lstrcpynA = LPSTR(WINAPI*)(LPSTR, LPCSTR, int);
using PFN_lstrcatA = LPSTR(WINAPI*)(LPSTR, LPCSTR);
using PFN_MB2WC = int(WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);

PFN_lstrlenA g_real_lstrlenA = nullptr;
PFN_lstrcpyA g_real_lstrcpyA = nullptr;
PFN_lstrcpynA g_real_lstrcpynA = nullptr;
PFN_lstrcatA g_real_lstrcatA = nullptr;
PFN_MB2WC g_real_MB2WC = nullptr;

int WINAPI Hooked_lstrlenA(LPCSTR s) {
    g_st_lstrlenA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe{};
    if (s != nullptr && lookup_patched(static_cast<const void*>(s), &pe)) {
        g_st_lstrlenA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrlenA, "ADDR", s, pe.full_en);
        return pe.en_len;
    }
    const char* const en = try_translate(s);
    if (en != nullptr) {
        g_st_lstrlenA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrlenA, "HIT ", s, en);
        return g_real_lstrlenA(en);
    }
    log_first_n(g_st_lstrlenA, "miss", s, nullptr);
    return g_real_lstrlenA(s);
}

LPSTR WINAPI Hooked_lstrcpyA(LPSTR dst, LPCSTR src) {
    g_st_lstrcpyA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe{};
    if (src != nullptr && lookup_patched(static_cast<const void*>(src), &pe)) {
        g_st_lstrcpyA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpyA, "ADDR", src, pe.full_en);
        return g_real_lstrcpyA(dst, pe.full_en);
    }
    const char* const en = try_translate(src);
    if (en != nullptr) {
        g_st_lstrcpyA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpyA, "HIT ", src, en);
        return g_real_lstrcpyA(dst, en);
    }
    log_first_n(g_st_lstrcpyA, "miss", src, nullptr);
    return g_real_lstrcpyA(dst, src);
}

LPSTR WINAPI Hooked_lstrcpynA(LPSTR dst, LPCSTR src, int n) {
    g_st_lstrcpynA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe{};
    if (src != nullptr && lookup_patched(static_cast<const void*>(src), &pe)) {
        g_st_lstrcpynA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpynA, "ADDR", src, pe.full_en);
        return g_real_lstrcpynA(dst, pe.full_en, n);
    }
    const char* const en = try_translate(src);
    if (en != nullptr) {
        g_st_lstrcpynA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpynA, "HIT ", src, en);
        return g_real_lstrcpynA(dst, en, n);
    }
    log_first_n(g_st_lstrcpynA, "miss", src, nullptr);
    return g_real_lstrcpynA(dst, src, n);
}

LPSTR WINAPI Hooked_lstrcatA(LPSTR dst, LPCSTR src) {
    g_st_lstrcatA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe{};
    if (src != nullptr && lookup_patched(static_cast<const void*>(src), &pe)) {
        g_st_lstrcatA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcatA, "ADDR", src, pe.full_en);
        return g_real_lstrcatA(dst, pe.full_en);
    }
    const char* const en = try_translate(src);
    if (en != nullptr) {
        g_st_lstrcatA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcatA, "HIT ", src, en);
        return g_real_lstrcatA(dst, en);
    }
    log_first_n(g_st_lstrcatA, "miss", src, nullptr);
    return g_real_lstrcatA(dst, src);
}

int WINAPI Hooked_MB2WC(UINT cp,
                        DWORD flags,
                        LPCCH src,
                        int srcLen,
                        LPWSTR dst,
                        int dstCap) {
    g_st_MB2WC.calls.fetch_add(1, std::memory_order_relaxed);

    if (src != nullptr) {
        PatchedEntry pe{};
        if (lookup_patched(static_cast<const void*>(src), &pe)) {
            g_st_MB2WC.hits.fetch_add(1, std::memory_order_relaxed);
            log_first_n(g_st_MB2WC, "ADDR", src, pe.full_en);
            return g_real_MB2WC(
                cp, flags, pe.full_en, kNullTerminatedSrcLen, dst, dstCap);
        }
    }

    if ((cp == kCp932CodePage || cp == CP_ACP) &&
        srcLen == kNullTerminatedSrcLen && src != nullptr) {
        const char* const en = try_translate(src);
        if (en != nullptr) {
            g_st_MB2WC.hits.fetch_add(1, std::memory_order_relaxed);
            log_first_n(g_st_MB2WC, "HIT ", src, en);
            return g_real_MB2WC(cp, flags, en, srcLen, dst, dstCap);
        }
    }
    if (srcLen == kNullTerminatedSrcLen && src != nullptr) {
        log_first_n(g_st_MB2WC, "miss", src, nullptr);
    }
    return g_real_MB2WC(cp, flags, src, srcLen, dst, dstCap);
}

}  // namespace

extern "C" void iat_register_patched(void* addr,
                                     const char* full_en,
                                     int en_len) {
    if (addr == nullptr || full_en == nullptr || en_len <= 0) {
        return;
    }
    ensure_patched_init();
    const auto key = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(addr));
    int count = 0;
    {
        const ScopedCsLock lock(g_patched_cs);
        g_patched[key] = {full_en, en_len};
        count = static_cast<int>(g_patched.size());
    }
    g_patched_count.store(count, std::memory_order_relaxed);
}

extern "C" void start_iat_translator() {
    load_table_once();

    const HMODULE exe = GetModuleHandleW(nullptr);
    LOG("iat: installing IAT hooks on %p", exe);

    g_real_lstrlenA = reinterpret_cast<PFN_lstrlenA>(
        IATHook(exe, "KERNEL32.dll", "lstrlenA", reinterpret_cast<void*>(&Hooked_lstrlenA)));
    g_real_lstrcpyA = reinterpret_cast<PFN_lstrcpyA>(
        IATHook(exe, "KERNEL32.dll", "lstrcpyA", reinterpret_cast<void*>(&Hooked_lstrcpyA)));
    g_real_lstrcpynA = reinterpret_cast<PFN_lstrcpynA>(
        IATHook(exe, "KERNEL32.dll", "lstrcpynA", reinterpret_cast<void*>(&Hooked_lstrcpynA)));
    g_real_lstrcatA = reinterpret_cast<PFN_lstrcatA>(
        IATHook(exe, "KERNEL32.dll", "lstrcatA", reinterpret_cast<void*>(&Hooked_lstrcatA)));
    g_real_MB2WC = reinterpret_cast<PFN_MB2WC>(
        IATHook(exe, "KERNEL32.dll", "MultiByteToWideChar", reinterpret_cast<void*>(&Hooked_MB2WC)));

    const HMODULE k32 = GetModuleHandleW(L"KERNEL32.dll");
    if (g_real_lstrlenA == nullptr) {
        g_real_lstrlenA = reinterpret_cast<PFN_lstrlenA>(
            GetProcAddress(k32, "lstrlenA"));
    }
    if (g_real_lstrcpyA == nullptr) {
        g_real_lstrcpyA = reinterpret_cast<PFN_lstrcpyA>(
            GetProcAddress(k32, "lstrcpyA"));
    }
    if (g_real_lstrcpynA == nullptr) {
        g_real_lstrcpynA = reinterpret_cast<PFN_lstrcpynA>(
            GetProcAddress(k32, "lstrcpynA"));
    }
    if (g_real_lstrcatA == nullptr) {
        g_real_lstrcatA = reinterpret_cast<PFN_lstrcatA>(
            GetProcAddress(k32, "lstrcatA"));
    }
    if (g_real_MB2WC == nullptr) {
        g_real_MB2WC = reinterpret_cast<PFN_MB2WC>(
            GetProcAddress(k32, "MultiByteToWideChar"));
    }

    LOG("iat: installing direct-export hooks (kernel32 prologue patches)");
    if (g_real_lstrlenA != nullptr) {
        ExportHookInstall(reinterpret_cast<void*>(g_real_lstrlenA),
                          reinterpret_cast<void*>(&Hooked_lstrlenA),
                          reinterpret_cast<void**>(&g_real_lstrlenA));
    }
    if (g_real_lstrcpyA != nullptr) {
        ExportHookInstall(reinterpret_cast<void*>(g_real_lstrcpyA),
                          reinterpret_cast<void*>(&Hooked_lstrcpyA),
                          reinterpret_cast<void**>(&g_real_lstrcpyA));
    }
    if (g_real_lstrcpynA != nullptr) {
        ExportHookInstall(reinterpret_cast<void*>(g_real_lstrcpynA),
                          reinterpret_cast<void*>(&Hooked_lstrcpynA),
                          reinterpret_cast<void**>(&g_real_lstrcpynA));
    }
    if (g_real_lstrcatA != nullptr) {
        ExportHookInstall(reinterpret_cast<void*>(g_real_lstrcatA),
                          reinterpret_cast<void*>(&Hooked_lstrcatA),
                          reinterpret_cast<void**>(&g_real_lstrcatA));
    }
    if (g_real_MB2WC != nullptr) {
        ExportHookInstall(reinterpret_cast<void*>(g_real_MB2WC),
                          reinterpret_cast<void*>(&Hooked_MB2WC),
                          reinterpret_cast<void**>(&g_real_MB2WC));
    }
}

extern "C" void log_iat_translator_summary() {
    auto dump = [](HookStats& st) {
        LOG("iat summary: %-22s calls=%d hits=%d",
            st.name, st.calls.load(), st.hits.load());
    };
    dump(g_st_lstrlenA);
    dump(g_st_lstrcpyA);
    dump(g_st_lstrcpynA);
    dump(g_st_lstrcatA);
    dump(g_st_MB2WC);
    LOG("iat patched-addr registry: %d entries (memory_scanner registered)",
        g_patched_count.load());
}
