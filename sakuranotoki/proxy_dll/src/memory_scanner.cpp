// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Periodic JP-bytes-in-memory finder and in-place patcher.
//
// Searches committed writable non-image regions for UTF-8 Japanese keys from
// translations.tsv, overwrites matched buffers with English bytes plus NUL,
// updates inline/heap std::string or Artemis cached-length fields when safe,
// and registers patched addresses with iat_translator.

#include "memory_scanner.h"

#include "iat_translator.h"
#include "log.h"
#include "page_guard.h"
#include "text_finder.h"
#include "translator.h"
#include "translator_logic.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kInitialModulePathChars = MAX_PATH;
constexpr DWORD kMaxModulePathChars = 32768U;
constexpr DWORD kPathBufferGrowthFactor = 2U;
constexpr std::size_t kMinJpPairBytes = 4;
constexpr unsigned char kHighByteThreshold = 0x80U;
constexpr DWORD kProtectLowByteMask = 0xFFU;
constexpr SIZE_T kMaxScannableRegionBytes = 0x10000000U;
constexpr SIZE_T kMinScannableRegionBytes = 0x100U;
constexpr std::size_t kHexDumpMaxBytes = 32;
constexpr std::size_t kHexCharsPerByte = 3;
constexpr std::size_t kEscapedAsciiCharsPerByte = 2;
constexpr unsigned kHighNibbleShift = 4U;
constexpr BYTE kNibbleMask = 0x0FU;
constexpr BYTE kPrintableAsciiMin = 0x20U;
constexpr BYTE kPrintableAsciiEnd = 0x7FU;
constexpr std::size_t kMsvcStringSizeOffset = 0x10;
constexpr std::size_t kMsvcStringCapOffset = 0x14;
constexpr DWORD kMsvcInlineStringCap = 0x0FU;
constexpr std::size_t kMsvcInlineBufferBudget = 0x10;
constexpr SIZE_T kDwordBytes = 4;
constexpr std::size_t kMinStringObjectSpan = 0x18;
constexpr DWORD kMaxHeapStringCap = 0x4000U;
constexpr std::size_t kSlackPeekBytes = 16;
constexpr std::size_t kMaxPatchBufferBytes = 512;
constexpr std::size_t kCachedLenOffset = 4;
constexpr std::size_t kMaxLenRewriteExtension = 16;
constexpr std::size_t kHeapReallocSlackBytes = 16;
constexpr std::size_t kDiagnosticContextBytes = 16;
constexpr int kInitialVerbosePasses = 3;
constexpr int kHeartbeatPassInterval = 200;
constexpr DWORD kScanIntervalMs = 100U;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h) noexcept : h_(h) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
    }

    HANDLE get() const noexcept { return h_; }

private:
    HANDLE h_{nullptr};
};

// JP needle bytes live in a VirtualAlloc'd PAGE_READONLY pool so the scanner
// never matches and overwrites its own search needles.
struct Pair {
    const BYTE* jp_bytes{nullptr};
    std::size_t jp_len{0};
    std::string en;
    std::string jp_label;
};

std::vector<Pair> g_pairs;
LPVOID g_needle_pool = nullptr;

struct RegionSnap {
    BYTE* base{nullptr};
    std::size_t size{0};
};
std::vector<RegionSnap> g_regions;

std::atomic<int> g_passes{0};
std::atomic<int> g_total_patches{0};

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

bool has_high_byte(const std::string& s) {
    for (const char ch : s) {
        if (static_cast<unsigned char>(ch) >= kHighByteThreshold) {
            return true;
        }
    }
    return false;
}

bool load_pairs() {
    std::unordered_map<std::string, std::string> m;
    const std::string path = get_dll_dir() + "\\translations.tsv";
    const std::size_t n = tr::load_tsv_file(path, m);
    if (n == SIZE_MAX || m.empty()) {
        LOG("scanner: failed to load TSV from %s", path.c_str());
        return false;
    }

    std::vector<std::pair<std::string, std::string>> filtered;
    std::size_t total = 0;
    for (const auto& kv : m) {
        if (kv.first.size() < kMinJpPairBytes) {
            continue;
        }
        if (kv.first == kv.second) {
            continue;
        }
        if (!has_high_byte(kv.first)) {
            continue;
        }
        filtered.emplace_back(kv.first, kv.second);
        total += kv.first.size();
    }
    if (filtered.empty()) {
        LOG("scanner: TSV had no JP-bearing entries");
        return false;
    }

    g_needle_pool = VirtualAlloc(
        nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_needle_pool == nullptr) {
        LOG("scanner: VirtualAlloc(needle pool, %zu) failed gle=%lu",
            total, GetLastError());
        return false;
    }

    // Reserve once so SSO string buffers inside Pair never relocate after
    // pointers are registered with iat_register_patched.
    g_pairs.reserve(filtered.size());

    auto* p = static_cast<BYTE*>(g_needle_pool);
    for (const auto& [jp, en] : filtered) {
        std::memcpy(p, jp.data(), jp.size());
        Pair np;
        np.jp_bytes = p;
        np.jp_len = jp.size();
        np.en = en;
        np.jp_label = jp;
        g_pairs.push_back(std::move(np));
        p += jp.size();
    }

    DWORD old_prot = 0;
    VirtualProtect(g_needle_pool, total, PAGE_READONLY, &old_prot);

    LOG("scanner: loaded %zu JP->EN pairs (pool=%p size=%zu, read-only)",
        g_pairs.size(), g_needle_pool, total);
    return true;
}

bool region_is_scannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD prot = mbi.Protect & kProtectLowByteMask;
    const bool writable = (prot == PAGE_READWRITE ||
                           prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READWRITE ||
                           prot == PAGE_EXECUTE_WRITECOPY);
    if (!writable) {
        return false;
    }
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    if (mbi.RegionSize > kMaxScannableRegionBytes) {
        return false;
    }
    if (mbi.RegionSize < kMinScannableRegionBytes) {
        return false;
    }
    if (mbi.Type == MEM_IMAGE) {
        return false;
    }
    return true;
}

__declspec(noinline)
void dump_hex_safe(const char* label, const BYTE* addr, std::size_t n) {
    BYTE buf[kHexDumpMaxBytes] = {};
    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    SIZE_T got = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(), addr, buf, n, &got);
    if (!ok || got == 0) {
        LOG("%s%p bytes=<unreadable> gle=%lu", label, addr, GetLastError());
        return;
    }

    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    char hex_part[kHexDumpMaxBytes * kHexCharsPerByte + 1] = {};
    char* h = hex_part;
    for (std::size_t i = 0; i < got; ++i) {
        h[0] = kHexDigits[(buf[i] >> kHighNibbleShift) & kNibbleMask];
        h[1] = kHexDigits[buf[i] & kNibbleMask];
        h[2] = ' ';
        h += kHexCharsPerByte;
    }
    if (h > hex_part) {
        --h;
    }
    *h = '\0';

    char ascii_part[kHexDumpMaxBytes * kEscapedAsciiCharsPerByte + 1] = {};
    char* a = ascii_part;
    for (std::size_t i = 0; i < got; ++i) {
        const BYTE b = buf[i];
        if (b == '%') {
            *a++ = '%';
            *a++ = '%';
            continue;
        }
        if (b >= kPrintableAsciiMin && b < kPrintableAsciiEnd) {
            *a++ = static_cast<char>(b);
            continue;
        }
        *a++ = '.';
    }
    *a = '\0';

    LOG("%s%p bytes=%s | %s", label, addr, hex_part, ascii_part);
}

const BYTE* find_bytes(const BYTE* hay,
                       std::size_t hay_len,
                       const BYTE* needle,
                       std::size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) {
        return nullptr;
    }
    const BYTE first = needle[0];
    const BYTE* const end = hay + hay_len - needle_len;
    for (const BYTE* p = hay; p <= end; ++p) {
        const auto* const q = static_cast<const BYTE*>(
            std::memchr(p, first, static_cast<std::size_t>(end - p) + 1));
        if (q == nullptr) {
            return nullptr;
        }
        if (std::memcmp(q, needle, needle_len) == 0) {
            return q;
        }
        p = q;
    }
    return nullptr;
}

__declspec(noinline)
BYTE* search_heap_string_wrapper(BYTE* rb,
                                 std::size_t rs,
                                 DWORD ptr_val,
                                 std::size_t want_sz,
                                 std::size_t want_min_cap) {
    if (rb == nullptr || rs <= kMinStringObjectSpan) {
        return nullptr;
    }
    BYTE* const lo = rb;
    BYTE* const hi = rb + rs - kMinStringObjectSpan;
    for (BYTE* q = lo; q <= hi; q += kDwordBytes) {
        __try {
            if (*reinterpret_cast<DWORD*>(q) != ptr_val) {
                continue;
            }
            const DWORD sz =
                *reinterpret_cast<DWORD*>(q + kMsvcStringSizeOffset);
            const DWORD cap =
                *reinterpret_cast<DWORD*>(q + kMsvcStringCapOffset);
            if (sz == want_sz && cap >= want_min_cap && cap <= kMaxHeapStringCap) {
                return q;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
    return nullptr;
}

enum PatchMode {
    PM_FAIL = 0,
    PM_TRUNC,
    PM_FIT,
    PM_INLINE_EXT,
    PM_HEAP_EXT,
    PM_HEAP_REALLOC,
    PM_LEN_REWRITE
};

__declspec(noinline)
PatchMode patch_one(BYTE* dst,
                    const Pair& pair,
                    BYTE* region_base,
                    std::size_t region_size) {
    const HANDLE proc = GetCurrentProcess();
    SIZE_T r = 0;

    std::size_t budget = pair.jp_len;

    bool inline_str = false;
    DWORD orig_size = 0;
    DWORD orig_cap = 0;
    if (ReadProcessMemory(proc, dst + kMsvcStringSizeOffset, &orig_size, kDwordBytes, &r) &&
        r == kDwordBytes &&
        ReadProcessMemory(proc, dst + kMsvcStringCapOffset, &orig_cap, kDwordBytes, &r) &&
        r == kDwordBytes) {
        if (orig_cap == kMsvcInlineStringCap && orig_size == pair.jp_len) {
            inline_str = true;
            budget = kMsvcInlineBufferBudget;
        }
    }

    BYTE* heap_obj = nullptr;
    if (!inline_str && pair.en.size() > pair.jp_len) {
        const auto ptr_val =
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(dst));
        heap_obj = search_heap_string_wrapper(
            region_base, region_size, ptr_val, pair.jp_len, pair.en.size());
        if (heap_obj == nullptr) {
            for (const auto& reg : g_regions) {
                if (reg.base == region_base) {
                    continue;
                }
                heap_obj = search_heap_string_wrapper(
                    reg.base, reg.size, ptr_val, pair.jp_len, pair.en.size());
                if (heap_obj != nullptr) {
                    break;
                }
            }
        }
        if (heap_obj != nullptr) {
            const DWORD cap =
                *reinterpret_cast<DWORD*>(heap_obj + kMsvcStringCapOffset);
            budget = static_cast<std::size_t>(cap) + 1;
        }
    }

    BYTE* const realloc_wrapper = nullptr;

    if (!inline_str && heap_obj == nullptr &&
        pair.en.size() + 1 > budget &&
        pair.en.size() <= pair.jp_len + kSlackPeekBytes) {
        BYTE peek[kSlackPeekBytes] = {};
        SIZE_T pr = 0;
        if (ReadProcessMemory(proc, dst + pair.jp_len, peek, sizeof(peek), &pr) &&
            pr > 0) {
            std::size_t slack = 0;
            for (std::size_t i = 0; i < pr && peek[i] == 0; ++i) {
                ++slack;
            }
            if (slack > 0) {
                budget = pair.jp_len + slack;
            }
        }
    }

    std::size_t copy = pair.en.size();
    if (copy + 1 > budget) {
        copy = budget - 1;
    }

    BYTE patched[kMaxPatchBufferBytes] = {};
    std::size_t total = copy + 1;
    if (total > sizeof(patched)) {
        copy = sizeof(patched) - 1;
        total = sizeof(patched);
    }
    std::memcpy(patched, pair.en.data(), copy);
    patched[copy] = '\0';

    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, dst, patched, total, &written) ||
        written != total) {
        return PM_FAIL;
    }

    page_guard_register_patched(static_cast<void*>(dst));
    iat_register_patched(
        static_cast<void*>(dst), pair.en.c_str(), static_cast<int>(pair.en.size()));

    // Artemis dialogue entries store a 4-byte LE length immediately preceding
    // the text buffer (`dst - 4`). When that field matches `jp_len` and the
    // overhang is small, write the remaining English tail and update the
    // cached length.
    if (copy < pair.en.size() &&
        (pair.en.size() - pair.jp_len) <= kMaxLenRewriteExtension &&
        dst >= region_base + kCachedLenOffset) {
        DWORD cached = 0;
        SIZE_T rr = 0;
        if (ReadProcessMemory(proc, dst - kCachedLenOffset, &cached, kDwordBytes, &rr) &&
            rr == kDwordBytes &&
            cached == static_cast<DWORD>(pair.jp_len)) {
            const SIZE_T extra = pair.en.size() - copy;
            const char* const tail = pair.en.data() + copy;
            SIZE_T w = 0;
            if (WriteProcessMemory(proc, dst + copy, tail, extra, &w) &&
                w == extra) {
                const auto new_len = static_cast<DWORD>(pair.en.size());
                SIZE_T w2 = 0;
                if (WriteProcessMemory(
                        proc, dst - kCachedLenOffset, &new_len, kDwordBytes, &w2) &&
                    w2 == kDwordBytes) {
                    return PM_LEN_REWRITE;
                }
            }
        }
    }

    if (inline_str) {
        const auto new_sz = static_cast<DWORD>(copy);
        WriteProcessMemory(
            proc, dst + kMsvcStringSizeOffset, &new_sz, kDwordBytes, &written);
        return PM_INLINE_EXT;
    }
    if (heap_obj != nullptr) {
        const auto new_sz = static_cast<DWORD>(copy);
        WriteProcessMemory(
            proc, heap_obj + kMsvcStringSizeOffset, &new_sz, kDwordBytes, &written);
        return PM_HEAP_EXT;
    }

    if (realloc_wrapper != nullptr) {
        const std::size_t en_size = pair.en.size();
        const std::size_t alloc_size = en_size + 1 + kHeapReallocSlackBytes;
        void* const newbuf = HeapAlloc(GetProcessHeap(), 0, alloc_size);
        if (newbuf != nullptr) {
            std::memcpy(newbuf, pair.en.data(), en_size);
            static_cast<char*>(newbuf)[en_size] = '\0';

            const DWORD orig_ptr = *reinterpret_cast<DWORD*>(realloc_wrapper);
            const DWORD orig_sz =
                *reinterpret_cast<DWORD*>(realloc_wrapper + kMsvcStringSizeOffset);
            const DWORD orig_cap =
                *reinterpret_cast<DWORD*>(realloc_wrapper + kMsvcStringCapOffset);

            const auto new_ptr =
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(newbuf));
            const auto new_sz = static_cast<DWORD>(en_size);
            const auto new_cap = static_cast<DWORD>(en_size);

            WriteProcessMemory(
                proc, realloc_wrapper + kMsvcStringCapOffset, &new_cap, kDwordBytes, &written);
            WriteProcessMemory(
                proc, realloc_wrapper + kMsvcStringSizeOffset, &new_sz, kDwordBytes, &written);
            WriteProcessMemory(
                proc, realloc_wrapper, &new_ptr, kDwordBytes, &written);

            LOG("heap-realloc: wrapper=%p  orig{ptr=%p sz=%lu cap=%lu}"
                "  new{ptr=%p sz=%lu cap=%lu}",
                realloc_wrapper,
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(orig_ptr)),
                static_cast<unsigned long>(orig_sz),
                static_cast<unsigned long>(orig_cap),
                newbuf,
                static_cast<unsigned long>(new_sz),
                static_cast<unsigned long>(new_cap));
            return PM_HEAP_REALLOC;
        }
    }

    if (copy < pair.en.size()) {
        return PM_TRUNC;
    }
    return PM_FIT;
}

void log_patch_once(std::size_t pair_index,
                    PatchMode pm,
                    const BYTE* hit,
                    const Pair& pair) {
    static std::vector<int> per_pair_logged;
    if (per_pair_logged.empty()) {
        per_pair_logged.assign(g_pairs.size(), 0);
    }
    if (pair_index >= per_pair_logged.size() || per_pair_logged[pair_index] != 0) {
        return;
    }
    ++per_pair_logged[pair_index];

    const char* tag = "fit";
    switch (pm) {
    case PM_TRUNC:        tag = "TRUNC"; break;
    case PM_INLINE_EXT:   tag = "inline-ext"; break;
    case PM_HEAP_EXT:     tag = "heap-ext"; break;
    case PM_HEAP_REALLOC: tag = "heap-realloc"; break;
    case PM_LEN_REWRITE:  tag = "len-fix"; break;
    default:              break;
    }

    LOG("patch [%-10s] %p  [%s] -> [%s]",
        tag, hit, pair.jp_label.c_str(), pair.en.c_str());

    if (pm == PM_TRUNC) {
        dump_hex_safe("  pre  ", hit - kDiagnosticContextBytes, kDiagnosticContextBytes);
        dump_hex_safe("  post ", hit + pair.jp_len, kDiagnosticContextBytes);
    }
}

__declspec(noinline)
const BYTE* safe_find_and_patch(BYTE* p,
                                std::size_t remaining,
                                const Pair& pair,
                                std::size_t pair_index,
                                BYTE* base,
                                std::size_t size,
                                PatchMode* out_mode) {
    __try {
        const BYTE* const hit =
            find_bytes(p, remaining, pair.jp_bytes, pair.jp_len);
        if (hit == nullptr) {
            return nullptr;
        }
        *out_mode = patch_one(const_cast<BYTE*>(hit), pair, base, size);
        if (*out_mode != PM_FAIL) {
            text_finder_register_patched(
                const_cast<void*>(static_cast<const void*>(hit)));
            log_patch_once(pair_index, *out_mode, hit, pair);
        }
        return hit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int scan_region(BYTE* base, std::size_t size) {
    int patches = 0;
    for (std::size_t idx = 0; idx < g_pairs.size(); ++idx) {
        const Pair& pair = g_pairs[idx];
        const std::size_t needle_len = pair.jp_len;
        BYTE* p = base;
        std::size_t remaining = size;
        while (remaining >= needle_len) {
            PatchMode pm = PM_FAIL;
            const BYTE* const hit = safe_find_and_patch(
                p, remaining, pair, idx, base, size, &pm);
            if (hit == nullptr) {
                break;
            }
            if (pm != PM_FAIL) {
                ++patches;
            }
            const std::size_t advance =
                static_cast<std::size_t>(hit - p) + needle_len;
            p += advance;
            remaining -= advance;
        }
    }
    return patches;
}

DWORD WINAPI scanner_thread(LPVOID) {
    LOG("scanner: thread starting");
    if (!load_pairs()) {
        return 1;
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    for (;;) {
        int pass_patches = 0;
        int regions_scanned = 0;

        g_regions.clear();
        auto* addr = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
        auto* const max_addr = static_cast<BYTE*>(si.lpMaximumApplicationAddress);
        while (addr < max_addr) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
                break;
            }
            if (region_is_scannable(mbi)) {
                g_regions.push_back(
                    {static_cast<BYTE*>(mbi.BaseAddress), mbi.RegionSize});
            }
            BYTE* const next =
                static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= addr) {
                break;
            }
            addr = next;
        }

        for (const auto& r : g_regions) {
            ++regions_scanned;
            pass_patches += scan_region(r.base, r.size);
        }

        const int pass_n = ++g_passes;
        const int total = (g_total_patches += pass_patches);
        static int s_last_count = -1;
        const bool changed = (pass_patches != s_last_count);
        if (pass_n <= kInitialVerbosePasses || changed ||
            pass_n % kHeartbeatPassInterval == 0) {
            LOG("scanner: pass#%d patched=%d (regions=%d, cumulative=%d)",
                pass_n, pass_patches, regions_scanned, total);
            s_last_count = pass_patches;
        }

        Sleep(kScanIntervalMs);
    }
}

}  // namespace

extern "C" void start_memory_scanner() {
    const ScopedHandle h(
        CreateThread(nullptr, 0, scanner_thread, nullptr, 0, nullptr));
    LOG("scanner: created thread %p", h.get());
}

extern "C" void log_scanner_summary() {
    LOG("scanner summary: passes=%d total patches applied=%d",
        g_passes.load(), g_total_patches.load());
}
