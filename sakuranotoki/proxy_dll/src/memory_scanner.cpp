// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// memory_scanner.cpp — periodic JP-bytes-in-memory finder + in-place patcher.
//
// Approach: for every TSV entry (jp, en), search committed writable
// regions for the JP bytes. On match, overwrite with EN bytes + NUL
// (truncating EN if it's longer than the JP byte count). Track patched
// addresses so we don't re-scan / re-patch.
//
// Patch shape:
//   - write up to JP_size - 1 bytes of EN (leaving room for NUL)
//   - NUL-terminate
//   - The std::string size field is NOT updated. The engine's renderer
//     reads bytes until the NUL we wrote, which is BEFORE the original
//     size, so the rendered text stops at the NUL. Trailing JP bytes
//     after our NUL are still in memory but never displayed.
//
// Region filter:
//   - State must be MEM_COMMIT
//   - Protect must include WRITE (PAGE_READWRITE / PAGE_WRITECOPY /
//     PAGE_EXECUTE_READWRITE)
//   - Skip MEM_IMAGE regions (executable images — JP strings rarely
//     live there, scanning them is wasted time)
//   - Skip regions > 256 MB (too large; not where small std::string
//     buffers live)
//
// Scan technique:
//   - Boyer-Moore-Horspool would be overkill; use std::memchr/memcmp.
//   - First-byte filter: scan for JP[0], then memcmp full needle.

#include "memory_scanner.h"
#include "translator.h"
#include "text_finder.h"
#include "page_guard.h"
#include "iat_translator.h"
#include "log.h"

#include <windows.h>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <utility>
#include <atomic>


// translator.h declares the lookup interface — but for the scanner we
// need direct access to ALL (jp, en) pairs. translator.cpp keeps the
// table private, so add a small accessor here that walks it via a
// callback.
//
// Cheaper: re-load the TSV ourselves with the same parser. The TSV is
// tiny (~3 KB) so duplicating in memory is fine.
namespace tr {
    using TranslationMap = std::unordered_map<std::string, std::string>;
    size_t load_tsv_file(const std::string& path, TranslationMap& out);
}


namespace {

// JP needle bytes live in a VirtualAlloc'd PAGE_READONLY pool so our
// own scanner never "finds" and overwrites them. Without this, the
// scanner walks its own DLL's heap, finds the JP bytes inside g_pairs
// std::strings, replaces them with EN, and from that point on
// "searches" for EN bytes everywhere (logs full of garbage like
//   patch: ["Oh, I'm Kusanagi Naoya."] -> ["Oh, I'm Kusanagi Naoya."]
// The pool sidesteps that entirely — PAGE_READONLY pages are filtered
// out by region_is_scannable so the needles are immutable.
struct Pair {
    const BYTE* jp_bytes;   // points into g_needle_pool (PAGE_READONLY)
    size_t jp_len;
    std::string en;         // OK in heap; we never search for en bytes
    std::string jp_label;   // small heap copy for logging only
};

std::vector<Pair> g_pairs;
LPVOID g_needle_pool = nullptr;

// Snapshot of all scannable regions, refreshed at the start of each
// pass. patch_one's heap-obj fallback uses it to look for std::string
// wrappers that live in a different region than the buffer — the case
// that left "Shizur" / "without a wor" / "seen him somewhe" truncated
// even after the per-region search was widened to the whole region.
struct RegionSnap { BYTE* base; size_t size; };
std::vector<RegionSnap> g_regions;

// (region_base, jp_offset_in_region) of every place we already patched.
// Avoids re-scanning the same address. Address-based, not value-based,
// because the EN we wrote LOOKS DIFFERENT from the JP — we'd never
// re-match it anyway, but the set keeps memory usage bounded if memory
// is reused.
std::atomic<int> g_passes{0};
std::atomic<int> g_total_patches{0};
std::atomic<int> g_total_misses{0};


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

bool load_pairs() {
    tr::TranslationMap m;
    std::string path = get_dll_dir() + "\\translations.tsv";
    size_t n = tr::load_tsv_file(path, m);
    if (n == SIZE_MAX || m.empty()) {
        LOG("scanner: failed to load TSV from %s", path.c_str());
        return false;
    }

    // First pass: filter, then total bytes.
    std::vector<std::pair<std::string,std::string>> filtered;
    size_t total = 0;
    for (auto& kv : m) {
        if (kv.first.size() < 4) continue;       // shorter strings spam-match
        if (kv.first == kv.second) continue;
        bool has_high = false;
        for (unsigned char b : kv.first) if (b >= 0x80) { has_high = true; break; }
        if (!has_high) continue;
        filtered.emplace_back(kv.first, kv.second);
        total += kv.first.size();
    }
    if (filtered.empty()) {
        LOG("scanner: TSV had no JP-bearing entries");
        return false;
    }

    // Allocate a private read-only pool for the JP bytes.
    g_needle_pool = VirtualAlloc(nullptr, total,
                                 MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
    if (!g_needle_pool) {
        LOG("scanner: VirtualAlloc(needle pool, %zu) failed gle=%lu",
            total, GetLastError());
        return false;
    }
    // Reserve so the vector NEVER reallocates during push_back. SSO
    // strings (≤15 chars) live inside the std::string struct, which is
    // inside Pair, which is inside the vector — if the vector resized,
    // SSO string buffers would move and invalidate the c_str() pointers
    // we hand to iat_register_patched. With reserve(), no reallocation,
    // pointers stable for the process lifetime.
    g_pairs.reserve(filtered.size());

    BYTE* p = static_cast<BYTE*>(g_needle_pool);
    for (auto& [jp, en] : filtered) {
        std::memcpy(p, jp.data(), jp.size());
        Pair np;
        np.jp_bytes = p;
        np.jp_len   = jp.size();
        np.en       = en;
        np.jp_label = jp;       // keep a heap copy for logging only
        g_pairs.push_back(std::move(np));
        p += jp.size();
    }

    // Lock the pool so the scanner won't overwrite our own needles.
    // region_is_scannable rejects non-writable protections.
    DWORD oldProt = 0;
    VirtualProtect(g_needle_pool, total, PAGE_READONLY, &oldProt);

    LOG("scanner: loaded %zu JP->EN pairs (pool=%p size=%zu, read-only)",
        g_pairs.size(), g_needle_pool, total);
    return true;
}


bool region_is_scannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    bool writable = (prot == PAGE_READWRITE
                  || prot == PAGE_WRITECOPY
                  || prot == PAGE_EXECUTE_READWRITE
                  || prot == PAGE_EXECUTE_WRITECOPY);
    if (!writable) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (mbi.RegionSize > 0x10000000) return false;  // > 256 MB skip
    if (mbi.RegionSize < 0x100) return false;       // tiny noise
    // Skip executable image sections — engine code, not data.
    if (mbi.Type == MEM_IMAGE) return false;
    return true;
}


// Diagnostic: log `n` bytes from `addr` as both hex and ASCII so the
// metadata around each patched dialogue buffer is visible in the proxy
// log. Used once per (pair, address) under the same per_pair_logged
// throttle that throttles the patch-line log.
//
// SEH-isolated through ReadProcessMemory: the page may straddle
// uninitialised memory or guard pages and a fault here must not abort
// the scanner thread. The function lives in its own __declspec(noinline)
// so the SEH frame doesn't share scope with C++ destructible objects in
// the caller.
__declspec(noinline)
static void dump_hex_safe(const char* label, const BYTE* addr, size_t n) {
    BYTE buf[32];
    if (n > sizeof(buf)) n = sizeof(buf);
    SIZE_T got = 0;
    BOOL ok = ReadProcessMemory(GetCurrentProcess(), addr, buf, n, &got);
    if (!ok || got == 0) {
        LOG("%s%p bytes=<unreadable> gle=%lu", label, addr, GetLastError());
        return;
    }

    // Hex part: "HH HH HH ..." (3 chars per byte).
    char hex_part[32 * 3 + 1];
    char* h = hex_part;
    for (size_t i = 0; i < got; ++i) {
        static const char DIGITS[] = "0123456789ABCDEF";
        h[0] = DIGITS[(buf[i] >> 4) & 0xF];
        h[1] = DIGITS[buf[i] & 0xF];
        h[2] = ' ';
        h += 3;
    }
    if (h > hex_part) --h;     // drop trailing space
    *h = '\0';

    // ASCII part: printable bytes verbatim, others as '.'. We escape
    // '%' to avoid log_msg's printf interpreting them as format
    // specifiers when this string is passed through "%s".
    char ascii_part[32 * 2 + 1];
    char* a = ascii_part;
    for (size_t i = 0; i < got; ++i) {
        BYTE b = buf[i];
        if (b == '%') { *a++ = '%'; *a++ = '%'; }
        else if (b >= 0x20 && b < 0x7F) { *a++ = (char)b; }
        else { *a++ = '.'; }
    }
    *a = '\0';

    LOG("%s%p bytes=%s | %s", label, addr, hex_part, ascii_part);
}


// Search `hay[0..hayLen)` for the first occurrence of `needle[0..needleLen)`.
// Naive but adequate; this runs every 100ms over MB-scale memory.
const BYTE* find_bytes(const BYTE* hay, size_t hayLen,
                       const BYTE* needle, size_t needleLen) {
    if (needleLen == 0 || hayLen < needleLen) return nullptr;
    const BYTE first = needle[0];
    const BYTE* end = hay + hayLen - needleLen;
    for (const BYTE* p = hay; p <= end; ++p) {
        // Quick reject on first byte
        const BYTE* q = static_cast<const BYTE*>(
            std::memchr(p, first, (end - p) + 1));
        if (!q) return nullptr;
        if (std::memcmp(q, needle, needleLen) == 0) return q;
        p = q;
    }
    return nullptr;
}


// Patch outcome flags returned to scan_region for one-shot logging.
//
// PM_LEN_REWRITE — fixes truncation by rewriting the cached length DWORD
// at hit-4 in the Artemis dialogue struct
//   { DWORD zero, flags, hash, jp_len; BYTE text[]; }.
// Discovered via diagnostic pre/post byte dumps: the engine reads
// jp_len bytes from `hit` regardless of NUL termination — that's why
// our slack-based extensions never rendered the trailing characters.
// PM_LEN_REWRITE writes the missing tail of EN past the original buffer
// end and updates the length DWORD, so the engine reads the full EN.
enum PatchMode { PM_FAIL = 0, PM_TRUNC, PM_FIT, PM_INLINE_EXT, PM_HEAP_EXT,
                 PM_HEAP_REALLOC, PM_LEN_REWRITE };

__declspec(noinline)
static PatchMode patch_one(BYTE* dst, const Pair& pair,
                           BYTE* region_base, size_t region_size) {
    HANDLE proc = GetCurrentProcess();
    SIZE_T r = 0;

    // Default: clamp EN to JP byte budget (truncate if longer).
    size_t budget = pair.jp_len;

    // Inline-std::string detection: MSVC's basic_string has the SBO
    // buffer at offset 0, then size at +0x10 and capacity at +0x14
    // (capacity == 0x0F for an inline string). When we see that
    // signature AND it points to our buffer, we can write up to 15
    // bytes (the inline capacity) and update the size field — fixing
    // "静流" -> "Shizuru" which currently truncates to "Shizu".
    bool inline_str = false;
    DWORD orig_size = 0, orig_cap = 0;
    if (ReadProcessMemory(proc, dst + 0x10, &orig_size, 4, &r) && r == 4
        && ReadProcessMemory(proc, dst + 0x14, &orig_cap, 4, &r) && r == 4) {
        if (orig_cap == 0x0F && orig_size == pair.jp_len) {
            inline_str = true;
            budget = 0x10;   // 15 bytes + NUL
        }
    }

    // Heap-std::string detection: scan for a 4-byte-aligned DWORD equal
    // to `dst`. That's the std::string-like OBJECT (MSVC layout
    // {ptr, ..., size@+0x10, capacity@+0x14}). When EN fits in capacity,
    // update the size field and write more bytes.
    //
    // Search order:
    //   1. The region containing dst — most std::string wrappers live in
    //      the same heap as their buffer.
    //   2. ALL other scannable regions — engine std::string objects can
    //      sit in a different region than the heap their buffer points
    //      at (this was the cause of "Shizur" / "without a wor" /
    //      "seen him somewhe" still truncating even after we widened
    //      the per-region search to the full region). The fallback is
    //      bounded by total scannable memory; in practice ~25 regions
    //      averaging a few MB each, and only runs on the FIRST pass per
    //      pair (after that the JP bytes are EN bytes and won't match
    //      again), so the cumulative cost is one-shot.
    BYTE* heap_obj = nullptr;
    if (!inline_str && pair.en.size() > pair.jp_len) {
        const DWORD ptr_val = reinterpret_cast<DWORD>(dst);
        const size_t want_sz = pair.jp_len;
        const size_t want_min_cap = pair.en.size();

        auto search_in = [&](BYTE* rb, size_t rs) -> BYTE* {
            if (!rb || rs <= 0x18) return nullptr;
            BYTE* lo = rb;
            BYTE* hi = rb + rs - 0x18;
            for (BYTE* q = lo; q <= hi; q += 4) {
                __try {
                    if (*reinterpret_cast<DWORD*>(q) != ptr_val) continue;
                    DWORD sz  = *reinterpret_cast<DWORD*>(q + 0x10);
                    DWORD cap = *reinterpret_cast<DWORD*>(q + 0x14);
                    if (sz == want_sz
                        && cap >= want_min_cap
                        && cap <= 0x4000) {
                        return q;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    return nullptr;
                }
            }
            return nullptr;
        };

        heap_obj = search_in(region_base, region_size);
        if (!heap_obj) {
            for (const auto& r : g_regions) {
                if (r.base == region_base) continue;
                heap_obj = search_in(r.base, r.size);
                if (heap_obj) break;
            }
        }
        if (heap_obj) {
            DWORD cap = *reinterpret_cast<DWORD*>(heap_obj + 0x14);
            budget = cap + 1;
        }
    }

    // (Pass 2 heap-realloc is currently disabled — the empirical
    // result was that updating wrapper.ptr/size/cap doesn't make the
    // engine read more bytes anyway because the engine has a separate
    // cached length we haven't located. Worse, repointing the wrapper
    // appears to corrupt other state — sentences that used to render
    // EN started rendering JP after realloc was added. We keep the
    // diagnostic dumps below so phase 4 can locate the cached length
    // without re-introducing this regression.)
    BYTE* realloc_wrapper = nullptr;

    // Slack-byte rescue: when neither inline nor heap detection found
    // the wrapping object, peek the byte right after the JP. If it's
    // already NUL, the renderer already considers byte `jp_len` as the
    // string terminator — extending into that NUL costs us nothing
    // because we'll just write our own NUL one byte later. This fixes
    // exact-byte-equal cases ("真琴"6 -> "Makoto"6) which currently
    // truncate to "Makot" because the budget=jp_len leaves no room
    // for our NUL.
    if (!inline_str && !heap_obj
        && pair.en.size() + 1 > budget
        && pair.en.size() <= pair.jp_len + 16) {
        BYTE peek[16] = {};
        SIZE_T pr = 0;
        if (ReadProcessMemory(proc, dst + pair.jp_len, peek,
                              sizeof(peek), &pr) && pr > 0) {
            size_t slack = 0;
            for (size_t i = 0; i < pr && peek[i] == 0; ++i) ++slack;
            if (slack > 0) budget = pair.jp_len + slack;
        }
    }

    size_t copy = pair.en.size();
    if (copy + 1 > budget) copy = budget - 1;

    BYTE patched[512];
    size_t total = copy + 1;
    if (total > sizeof(patched)) {
        copy  = sizeof(patched) - 1;
        total = sizeof(patched);
    }
    std::memcpy(patched, pair.en.data(), copy);
    patched[copy] = '\0';

    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, dst, patched, total, &written)
        || written != total) return PM_FAIL;

    // Register the patched address's page with page_guard (no-op if
    // already armed or page already registered). Coverage: every
    // successful write — fit, trunc, inline-ext, heap-ext. The set is
    // deduplicated and capped, so even if the same JP appears at
    // many addresses the page set stays bounded.
    page_guard_register_patched((void*)dst);

    // Register (addr -> full_en) so the IAT hooks can substitute the
    // FULL EN string when the engine queries this buffer's length or
    // copies/converts it. The buffer itself only holds `copy` bytes
    // (≤ jp_len), so without this hook the engine reads the truncated
    // EN. pair.en.c_str() is stable because g_pairs is reserved.
    iat_register_patched((void*)dst, pair.en.c_str(),
                         (int)pair.en.size());

    // ── Artemis cached-length rewrite (the truncation fix) ──────────
    //
    // The Artemis dialogue struct stores a 4-byte LE length DWORD at
    // hit-4. The engine reads exactly that many bytes from `hit` to
    // render — NUL termination is ignored. Diagnostic pre-dumps proved
    // this for every TRUNC entry in our test session: the DWORD at
    // hit-4 always equals pair.jp_len.
    //
    // Fix: when EN is longer than what we wrote, rewrite the missing
    // tail of EN past the original NUL position AND update the length
    // DWORD to en.size(). The engine then renders full EN.
    //
    // Safety:
    //   - Only attempt if the DWORD at hit-4 actually equals jp_len
    //     (verifies it really is the length field, not a coincidence).
    //   - Cap the extension at 16 bytes to limit blast radius if the
    //     post bytes are next-entry header / adjacent text. From the
    //     diagnostic dumps, slack between adjacent dialogue entries is
    //     typically 8-16 bytes (for short JP) or near-zero (for long).
    //     Most truncations we care about (JP=78 → EN=80) need only 2-3
    //     extra bytes — well within the cap.
    //   - This runs AFTER the existing truncated write, so failure of
    //     the rewrite step leaves the buffer in the same state as
    //     today's TRUNC behaviour (worst case = current behaviour,
    //     never worse).
    if (copy < pair.en.size()
        && (pair.en.size() - pair.jp_len) <= 16
        && dst >= region_base + 4) {
        DWORD cached = 0;
        SIZE_T rr = 0;
        if (ReadProcessMemory(proc, dst - 4, &cached, 4, &rr) && rr == 4
            && cached == (DWORD)pair.jp_len) {
            // It's the length field. Write missing EN tail then update
            // the length DWORD. We write from dst+copy because the
            // original WriteProcessMemory above already filled bytes
            // [dst, dst+copy) with truncated EN + NUL.
            //
            // Note: the byte at dst+copy was our NUL terminator. We
            // overwrite it with the real next character of EN. The
            // engine doesn't care about NUL — it uses cached length.
            SIZE_T extra = pair.en.size() - copy;
            const char* tail = pair.en.data() + copy;
            SIZE_T w = 0;
            if (WriteProcessMemory(proc, dst + copy, tail, extra, &w)
                && w == extra) {
                DWORD new_len = (DWORD)pair.en.size();
                SIZE_T w2 = 0;
                if (WriteProcessMemory(proc, dst - 4, &new_len, 4, &w2)
                    && w2 == 4) {
                    return PM_LEN_REWRITE;
                }
            }
        }
    }

    // Update the std::string size field so the engine reads `copy`
    // bytes (the new EN length), not the original JP byte count.
    if (inline_str) {
        DWORD new_sz = (DWORD)copy;
        WriteProcessMemory(proc, dst + 0x10, &new_sz, 4, &written);
        return PM_INLINE_EXT;
    } else if (heap_obj) {
        DWORD new_sz = (DWORD)copy;
        WriteProcessMemory(proc, heap_obj + 0x10, &new_sz, 4, &written);
        return PM_HEAP_EXT;
    }

    // Heap-realloc path. Reached only when:
    //   - en > jp_len (we need more bytes than the buffer holds)
    //   - we found a wrapper whose cap is in [jp_len, en.size()) — a
    //     real std::string sized to fit JP exactly.
    //   - the standard truncated write above already happened, so if
    //     anything goes wrong here the engine still sees the truncated
    //     EN at `dst` (worst case = current TRUNC behaviour, never
    //     worse). This is the defense-in-depth.
    //
    // We allocate a fresh buffer from the process heap (the same heap
    // MSVC's std::string default allocator uses, so the engine's
    // eventual destructor `HeapFree` succeeds on it), copy EN into it,
    // and repoint wrapper.{ptr, size, cap}. The original buffer at
    // dst is leaked but the OS reclaims at process exit.
    if (realloc_wrapper) {
        size_t en_size = pair.en.size();
        size_t alloc_size = en_size + 1 + 16;     // +NUL + small slack
        void* newbuf = HeapAlloc(GetProcessHeap(), 0, alloc_size);
        if (newbuf) {
            std::memcpy(newbuf, pair.en.data(), en_size);
            ((char*)newbuf)[en_size] = '\0';

            DWORD orig_ptr = *reinterpret_cast<DWORD*>(realloc_wrapper);
            DWORD orig_sz  = *reinterpret_cast<DWORD*>(realloc_wrapper + 0x10);
            DWORD orig_cap = *reinterpret_cast<DWORD*>(realloc_wrapper + 0x14);

            DWORD new_ptr = (DWORD)(uintptr_t)newbuf;
            DWORD new_sz  = (DWORD)en_size;
            DWORD new_cap = (DWORD)en_size;

            // Order: write cap first, then size, then ptr — the engine
            // is unlikely to read mid-update, but if it does, it will
            // see the OLD ptr until the very last write swaps it.
            WriteProcessMemory(proc, realloc_wrapper + 0x14,
                               &new_cap, 4, &written);
            WriteProcessMemory(proc, realloc_wrapper + 0x10,
                               &new_sz,  4, &written);
            WriteProcessMemory(proc, realloc_wrapper,
                               &new_ptr, 4, &written);

            LOG("heap-realloc: wrapper=%p  orig{ptr=%p sz=%lu cap=%lu}"
                "  new{ptr=%p sz=%lu cap=%lu}",
                realloc_wrapper, (void*)(uintptr_t)orig_ptr,
                (unsigned long)orig_sz, (unsigned long)orig_cap,
                newbuf, (unsigned long)new_sz, (unsigned long)new_cap);
            return PM_HEAP_REALLOC;
        }
    }

    if (copy < pair.en.size()) return PM_TRUNC;
    return PM_FIT;
}


__declspec(noinline)
static int scan_region(BYTE* base, size_t size) {
    int patches = 0;
    for (auto& pair : g_pairs) {
        const BYTE* needle = pair.jp_bytes;     // <- read-only pool
        size_t needleLen = pair.jp_len;
        BYTE* p = base;
        size_t remaining = size;
        while (remaining >= needleLen) {
            __try {
                const BYTE* hit = find_bytes(p, remaining,
                                             needle, needleLen);
                if (!hit) break;
                PatchMode pm = patch_one(const_cast<BYTE*>(hit), pair,
                                         base, size);
                if (pm != PM_FAIL) {
                    ++patches;
                    // Hand the patched address to text_finder so it
                    // can install hardware read BPs on a few known
                    // dialogue locations and identify the engine
                    // function that renders from them.
                    text_finder_register_patched((void*)hit);
                    int idx = (int)(&pair - &g_pairs[0]);
                    if (idx >= 0 && idx < (int)g_pairs.size()) {
                        static std::vector<int> per_pair_logged;
                        if (per_pair_logged.empty())
                            per_pair_logged.assign(g_pairs.size(), 0);
                        if (per_pair_logged[idx] == 0) {
                            ++per_pair_logged[idx];
                            const char* tag = "fit";
                            switch (pm) {
                                case PM_TRUNC:        tag = "TRUNC"; break;
                                case PM_INLINE_EXT:   tag = "inline-ext"; break;
                                case PM_HEAP_EXT:     tag = "heap-ext"; break;
                                case PM_HEAP_REALLOC: tag = "heap-realloc"; break;
                                case PM_LEN_REWRITE:  tag = "len-fix"; break;
                                default: break;
                            }
                            // Print full strings: %.Ns truncation can
                                // split a UTF-8 codepoint, which makes
                                // log_msg's UTF-8 validity check fail and
                                // fall back to CP932 — rendering the
                                // whole JP line as mojibake. Each pair
                                // logs at most once, so length is fine.
                            LOG("patch [%-10s] %p  [%s] -> [%s]",
                                tag, hit, pair.jp_label.c_str(),
                                pair.en.c_str());
                            // Diagnostic pre/post hex dumps — TRUNC ONLY.
                            // The earlier all-pairs version was removed
                            // because it slowed the first scan pass; we
                            // limit to TRUNC (the cases that need the
                            // length field) so cost is bounded by the
                            // count of unique TRUNCs (~20 per session).
                            //
                            // Goal: identify the cached jp_len that the
                            // engine uses regardless of NUL position. It
                            // lives somewhere in the bytes immediately
                            // before/after the JP buffer. Patterns we
                            // expect:
                            //   - 4-byte LE length DWORD at hit-4/-8/-16
                            //   - std::string {ptr, size, cap} struct
                            //     somewhere upstream (size@+0x10)
                            //   - terminator/sentinel padding after
                            if (pm == PM_TRUNC) {
                                dump_hex_safe("  pre  ",
                                              hit - 16, 16);
                                dump_hex_safe("  post ",
                                              hit + pair.jp_len, 16);
                            }
                        }
                    }
                }
                size_t advance = (hit - p) + needleLen;
                p += advance;
                remaining -= advance;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
        }
    }
    return patches;
}


DWORD WINAPI scanner_thread(LPVOID) {
    LOG("scanner: thread starting");
    if (!load_pairs()) return 1;

    // No initial sleep: the engine_scout in our worker_thread already
    // does Sleep(1500) before invoking us, so CrackProof has had time
    // to unpack. We want our FIRST scan to happen ASAP so the first
    // dialogue line ("「いらっしゃい」") gets caught before the
    // engine renders it. (Previous 500 ms wait still missed it.)

    SYSTEM_INFO si;
    GetSystemInfo(&si);

    while (true) {
        int pass_patches = 0;
        int regions_scanned = 0;

        // Refresh the region snapshot once per pass: heaps grow/shrink
        // as the engine runs, and patch_one's cross-region heap-obj
        // fallback needs an up-to-date list. Doing this in the same
        // VirtualQuery walk we already do for scanning avoids a second
        // syscall sweep.
        g_regions.clear();
        BYTE* addr = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
        BYTE* maxAddr = static_cast<BYTE*>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
            if (region_is_scannable(mbi)) {
                g_regions.push_back({static_cast<BYTE*>(mbi.BaseAddress),
                                     mbi.RegionSize});
            }
            BYTE* next = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }

        for (const auto& r : g_regions) {
            ++regions_scanned;
            pass_patches += scan_region(r.base, r.size);
        }

        int passN = ++g_passes;
        int total = (g_total_patches += pass_patches);
        // Only log: first 3 passes (init), or whenever the pass count
        // CHANGES from the previous pass (interesting transitions),
        // or every 200th pass as a heartbeat. Steady-state runs no
        // longer flood the log with thousands of identical lines.
        static int s_last_count = -1;
        bool changed = (pass_patches != s_last_count);
        if (passN <= 3 || changed || passN % 200 == 0) {
            LOG("scanner: pass#%d patched=%d (regions=%d, cumulative=%d)",
                passN, pass_patches, regions_scanned, total);
            s_last_count = pass_patches;
        }

        Sleep(100);
    }
}

}  // namespace


extern "C" void start_memory_scanner() {
    HANDLE h = CreateThread(nullptr, 0, scanner_thread, nullptr, 0, nullptr);
    LOG("scanner: created thread %p", h);
    if (h) CloseHandle(h);
}

extern "C" void log_scanner_summary() {
    LOG("scanner summary: passes=%d total patches applied=%d",
        g_passes.load(), g_total_patches.load());
}
