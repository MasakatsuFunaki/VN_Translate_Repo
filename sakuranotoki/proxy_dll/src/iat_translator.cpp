// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// iat_translator.cpp — IAT-hook based translator for Artemis / CrackProof.
//
// Why this exists (vs. memory_scanner.cpp):
//   memory_scanner overwrites JP bytes in place with EN. That works
//   when a translation fits in the JP byte budget, but for raw script
//   blobs with no std::string wrapper the budget IS jp_len, so any EN
//   longer than the JP gets truncated ("without a wor", "Shizur",
//   "seen him somewhe"). This is structural — no scanner cleverness
//   gets around a buffer with no extensible owner.
//
// Why IAT hooks (vs. inline JMP rel32 hooks):
//   sakuranotoki.exe is a CrackProof launcher: anti-tamper hashes .text
//   bytes at runtime and kills the process if they change. Inline hooks
//   on engine functions modify .text and trip the check. IAT entries
//   live in .idata and are written every time the loader resolves an
//   import — modifying them post-load is invisible to .text-watching
//   anti-tamper. FRATERNITE_HD (YU-RIS + SoftDenchi DRM) uses the same
//   approach for the same reason.
//
// Hook target rationale:
//   sakuranotoki.exe's static IAT is the launcher's, but the unpacked
//   Artemis engine shares it (one PE, one IAT). Imports relevant to
//   text handling, in order of likely dialogue-path usage:
//     1. lstrlenA       — measures CP932 strings, called per text op
//     2. lstrcpyA       — copies CP932 between buffers (script -> render)
//     3. lstrcpynA      — bounded copy
//     4. lstrcatA       — concatenation (used during text formatting)
//     5. MultiByteToWideChar — CP932 -> UTF-16 for D3D9 font rendering
//     6. WideCharToMultiByte — opposite direction
//
// We hook ALL of them. Each hook:
//   1. Looks up the input string in the TSV.
//   2. If it's a known JP key, substitutes a pointer to our EN slot.
//   3. Calls through to the real WinAPI with the substituted pointer.
//
// This means whichever API the engine actually uses for dialogue gets
// the substitution — we don't have to know in advance. The engine's
// downstream code (D3D9 glyph cache, font atlas, layout) sees ASCII
// English, which is a valid CP932 byte sequence (single-byte chars in
// the 0x20-0x7F range), so it renders without further conversion.

#include "iat_translator.h"
#include "iat_hook.h"
#include "export_hook.h"
#include "translator.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>


namespace tr {
    using TranslationMap = std::unordered_map<std::string, std::string>;
    size_t load_tsv_file(const std::string& path, TranslationMap& out);
}


namespace {


// ── String pool ─────────────────────────────────────────────────────
//
// Each JP -> EN substitution returns a stable char* into this pool.
// The pool is round-robin: 64 slots × 4096 bytes. The engine holds
// onto the pointer briefly (one render call), so 64 slots is plenty
// of headroom for re-entrancy and concurrent calls. Slot bytes never
// move once written, so the engine's downstream code can safely walk
// them (lstrlen, copy, render).

constexpr int    POOL_SLOTS = 64;
constexpr size_t POOL_SLOT_BYTES = 4096;
char  g_pool[POOL_SLOTS][POOL_SLOT_BYTES];
std::atomic<int> g_pool_idx{0};

const char* alloc_slot(const char* s, size_t n) {
    int idx = g_pool_idx.fetch_add(1, std::memory_order_relaxed) % POOL_SLOTS;
    char* slot = g_pool[idx];
    if (n >= POOL_SLOT_BYTES) n = POOL_SLOT_BYTES - 1;
    std::memcpy(slot, s, n);
    slot[n] = '\0';
    return slot;
}


// ── Translation table ───────────────────────────────────────────────

// CP932 (Shift-JIS) JP key -> ASCII EN value. The TSV is UTF-8 on
// disk; we re-key UTF-8 -> CP932 once at load so the runtime lookup
// matches the bytes the engine hands us. ASCII EN values pass
// through unchanged (a valid CP932 byte sequence).

std::unordered_map<std::string, std::string> g_translations;
std::once_flag g_load_flag;

std::string utf8_to_cp932(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                   (int)utf8.size(), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                        w.data(), wlen);
    int alen = WideCharToMultiByte(932, 0, w.data(), wlen,
                                   nullptr, 0, nullptr, nullptr);
    if (alen <= 0) return {};
    std::string out(alen, '\0');
    WideCharToMultiByte(932, 0, w.data(), wlen, out.data(), alen,
                        nullptr, nullptr);
    return out;
}

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

void load_table_once() {
    std::call_once(g_load_flag, []() {
        tr::TranslationMap utf8_map;
        std::string path = get_dll_dir() + "\\translations.tsv";
        size_t n = tr::load_tsv_file(path, utf8_map);
        if (n == SIZE_MAX || utf8_map.empty()) {
            LOG("iat: TSV load failed (%s)", path.c_str());
            return;
        }
        size_t kept = 0, dropped = 0;
        for (auto& kv : utf8_map) {
            if (kv.first.size() < 4) continue;
            if (kv.first == kv.second) continue;
            std::string cp932 = utf8_to_cp932(kv.first);
            if (cp932.empty()) { ++dropped; continue; }
            g_translations.emplace(std::move(cp932), kv.second);
            ++kept;
        }
        LOG("iat: loaded %zu CP932 translations (dropped %zu, total %zu utf8)",
            kept, dropped, utf8_map.size());
    });
}


// ── Patched-address registry ────────────────────────────────────────
//
// memory_scanner calls iat_register_patched(addr, full_en, en_len)
// after every successful WriteProcessMemory. The hooks consult this map
// to detect when the engine queries one of OUR patched buffers.
//
// Why we need this: the engine asks lstrlenA(buffer) how long the
// dialogue is, but the buffer contains EN truncated to jp_len bytes
// (because we wrote NUL at jp_len to satisfy CrackProof's no-extension
// constraint). lstrlenA returns jp_len. The engine then renders only
// jp_len bytes. Result: "...without a wor" instead of "...without a
// word.". By returning en_len (the FULL English length) when the input
// address is registered, we tell the engine the correct size, AND in
// the str/MB2WC hooks we substitute src to point at our pool slot
// holding the untruncated EN, so the rendered text is complete.
//
// Memory model:
//   - addr is the heap address memory_scanner patched (lives in engine
//     heap, freed when engine reuses the slot — but the engine is the
//     only writer to that heap slot, so a re-use means the slot now
//     holds a different JP/EN; the registry entry would be stale. In
//     practice, dialogue buffers live for the duration of the line +
//     a screen, and the IAT hooks are queried during that window).
//   - full_en is a stable pointer (memory_scanner reserves g_pairs).
//   - We never delete entries — the map grows monotonically with each
//     unique JP. Bounded by TSV size (~hundreds of pairs), so memory
//     usage is negligible.

struct PatchedEntry {
    const char* full_en;
    int         en_len;
};

std::unordered_map<DWORD, PatchedEntry> g_patched;
CRITICAL_SECTION g_patched_cs;
std::atomic<bool> g_patched_init{false};
std::atomic<int>  g_patched_count{0};

void ensure_patched_init() {
    if (g_patched_init.load(std::memory_order_acquire)) return;
    static std::once_flag once;
    std::call_once(once, []() {
        InitializeCriticalSection(&g_patched_cs);
        g_patched_init.store(true, std::memory_order_release);
    });
}

bool lookup_patched(const void* addr, PatchedEntry* out) {
    if (!g_patched_init.load(std::memory_order_acquire)) return false;
    if (!addr || !out) return false;
    DWORD key = (DWORD)(uintptr_t)addr;
    bool found = false;
    EnterCriticalSection(&g_patched_cs);
    auto it = g_patched.find(key);
    if (it != g_patched.end()) {
        *out = it->second;
        found = true;
    }
    LeaveCriticalSection(&g_patched_cs);
    return found;
}


// ── Hit tracking ────────────────────────────────────────────────────
//
// Per-hook stats so the log shows which API path actually carries
// dialogue: counts BOTH calls in total and calls that hit a TSV entry.
// First-N-of-each detail logs let us see what JP bytes are arriving
// without flooding the log.

struct HookStats {
    const char*       name;
    std::atomic<int>  calls{0};
    std::atomic<int>  hits{0};
    std::atomic<int>  detail_logged{0};
};

HookStats g_st_lstrlenA       { "lstrlenA",       {0}, {0}, {0} };
HookStats g_st_lstrcpyA       { "lstrcpyA",       {0}, {0}, {0} };
HookStats g_st_lstrcpynA      { "lstrcpynA",      {0}, {0}, {0} };
HookStats g_st_lstrcatA       { "lstrcatA",       {0}, {0}, {0} };
HookStats g_st_MB2WC          { "MultiByteToWideChar", {0}, {0}, {0} };

constexpr int DETAIL_LOG_PER_HOOK = 10;

bool has_high_byte(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if ((unsigned char)s[i] >= 0x80) return true;
    }
    return false;
}

// SEH-isolated NUL-terminated length probe. Lives in its own function
// because __try cannot share scope with C++ destructible objects
// (std::string in try_translate). Bound at 4096 bytes — any dialogue
// we'd care about is well under that.
__declspec(noinline)
static int safe_strlen_bounded(const char* s, int max_len) {
    __try {
        int n = 0;
        while (n < max_len && s[n]) ++n;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Try to translate a CP932 buffer. Returns the EN slot pointer on hit,
// or nullptr on miss (caller passes the original through).
const char* try_translate(const char* src, size_t* out_len = nullptr) {
    if (!src || !*src) return nullptr;
    load_table_once();
    if (g_translations.empty()) return nullptr;

    int len = safe_strlen_bounded(src, 4096);
    if (len < 4) return nullptr;
    if (!has_high_byte(src, (size_t)len)) return nullptr;

    std::string key(src, (size_t)len);
    auto it = g_translations.find(key);
    if (it == g_translations.end()) return nullptr;

    const std::string& en = it->second;
    const char* slot = alloc_slot(en.data(), en.size());
    if (out_len) *out_len = en.size();
    return slot;
}

// SEH-isolated copy. Returns bytes copied (may be 0) or -1 on fault.
__declspec(noinline)
static int safe_copy_bounded(char* dst, const char* src, int max_len) {
    __try {
        int n = 0;
        while (n < max_len - 1 && src[n]) {
            dst[n] = src[n];
            ++n;
        }
        dst[n] = '\0';
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void log_first_n(HookStats& st, const char* prefix, const char* s,
                 const char* substituted) {
    int n = st.detail_logged.fetch_add(1, std::memory_order_relaxed);
    if (n >= DETAIL_LOG_PER_HOOK) return;
    if (!s) {
        LOG("iat[%s] #%d: <null>", st.name, n + 1);
        return;
    }
    // Cap at 80 bytes so the log line stays readable; the hit/miss
    // distinction matters more than the full content.
    char preview[96] = {};
    safe_copy_bounded(preview, s, (int)sizeof(preview));

    if (substituted) {
        LOG("iat[%s] #%d %s [%s] -> [%s]",
            st.name, n + 1, prefix, preview, substituted);
    } else {
        LOG("iat[%s] #%d %s [%s]",
            st.name, n + 1, prefix, preview);
    }
}


// ── Real-API pointers (filled in by IATHook return values) ───────────

using PFN_lstrlenA  = int  (WINAPI*)(LPCSTR);
using PFN_lstrcpyA  = LPSTR(WINAPI*)(LPSTR, LPCSTR);
using PFN_lstrcpynA = LPSTR(WINAPI*)(LPSTR, LPCSTR, int);
using PFN_lstrcatA  = LPSTR(WINAPI*)(LPSTR, LPCSTR);
using PFN_MB2WC     = int  (WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);

PFN_lstrlenA  g_real_lstrlenA  = nullptr;
PFN_lstrcpyA  g_real_lstrcpyA  = nullptr;
PFN_lstrcpynA g_real_lstrcpynA = nullptr;
PFN_lstrcatA  g_real_lstrcatA  = nullptr;
PFN_MB2WC     g_real_MB2WC     = nullptr;


// ── Hooks ───────────────────────────────────────────────────────────

int WINAPI Hooked_lstrlenA(LPCSTR s) {
    g_st_lstrlenA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe;
    if (s && lookup_patched((const void*)s, &pe)) {
        // Address is one we patched. Return the FULL EN length so the
        // engine sizes downstream operations (render buffers, copies)
        // for the complete English string, not the JP byte budget.
        g_st_lstrlenA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrlenA, "ADDR", s, pe.full_en);
        return pe.en_len;
    }
    const char* en = try_translate(s);
    if (en) {
        g_st_lstrlenA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrlenA, "HIT ", s, en);
        return g_real_lstrlenA(en);
    }
    log_first_n(g_st_lstrlenA, "miss", s, nullptr);
    return g_real_lstrlenA(s);
}

LPSTR WINAPI Hooked_lstrcpyA(LPSTR dst, LPCSTR src) {
    g_st_lstrcpyA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe;
    if (src && lookup_patched((const void*)src, &pe)) {
        // Substitute src with the full EN (untruncated). dst was
        // allocated by the engine; if it sized via lstrlenA it now
        // has room for en_len + NUL.
        g_st_lstrcpyA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpyA, "ADDR", src, pe.full_en);
        return g_real_lstrcpyA(dst, pe.full_en);
    }
    const char* en = try_translate(src);
    if (en) {
        g_st_lstrcpyA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpyA, "HIT ", src, en);
        return g_real_lstrcpyA(dst, en);
    }
    log_first_n(g_st_lstrcpyA, "miss", src, nullptr);
    return g_real_lstrcpyA(dst, src);
}

LPSTR WINAPI Hooked_lstrcpynA(LPSTR dst, LPCSTR src, int n) {
    g_st_lstrcpynA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe;
    if (src && lookup_patched((const void*)src, &pe)) {
        g_st_lstrcpynA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpynA, "ADDR", src, pe.full_en);
        // n is dst capacity (incl. NUL). If the engine sized via
        // lstrlenA(en_len) it will fit; if it sized for JP, it
        // will still clamp to n - but the substitute is the best we
        // can do at this hook — caller is downstream of allocation.
        return g_real_lstrcpynA(dst, pe.full_en, n);
    }
    const char* en = try_translate(src);
    if (en) {
        g_st_lstrcpynA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcpynA, "HIT ", src, en);
        return g_real_lstrcpynA(dst, en, n);
    }
    log_first_n(g_st_lstrcpynA, "miss", src, nullptr);
    return g_real_lstrcpynA(dst, src, n);
}

LPSTR WINAPI Hooked_lstrcatA(LPSTR dst, LPCSTR src) {
    g_st_lstrcatA.calls.fetch_add(1, std::memory_order_relaxed);
    PatchedEntry pe;
    if (src && lookup_patched((const void*)src, &pe)) {
        g_st_lstrcatA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcatA, "ADDR", src, pe.full_en);
        return g_real_lstrcatA(dst, pe.full_en);
    }
    const char* en = try_translate(src);
    if (en) {
        g_st_lstrcatA.hits.fetch_add(1, std::memory_order_relaxed);
        log_first_n(g_st_lstrcatA, "HIT ", src, en);
        return g_real_lstrcatA(dst, en);
    }
    log_first_n(g_st_lstrcatA, "miss", src, nullptr);
    return g_real_lstrcatA(dst, src);
}

int WINAPI Hooked_MB2WC(UINT cp, DWORD flags, LPCCH src, int srcLen,
                        LPWSTR dst, int dstCap) {
    g_st_MB2WC.calls.fetch_add(1, std::memory_order_relaxed);

    // Address-based path FIRST: if src points at a buffer we patched,
    // substitute with full EN and let MB2WC compute its own length
    // (-1). This works for any code page and any srcLen because we
    // know the buffer holds our (truncated) EN.
    if (src) {
        PatchedEntry pe;
        if (lookup_patched((const void*)src, &pe)) {
            g_st_MB2WC.hits.fetch_add(1, std::memory_order_relaxed);
            log_first_n(g_st_MB2WC, "ADDR", (const char*)src, pe.full_en);
            return g_real_MB2WC(cp, flags, pe.full_en, -1, dst, dstCap);
        }
    }

    // Content-based fallback for CP932 / CP_ACP (rare hit, but kept).
    if (cp == 932 || cp == CP_ACP) {
        if (srcLen == -1 && src) {
            const char* en = try_translate((const char*)src);
            if (en) {
                g_st_MB2WC.hits.fetch_add(1, std::memory_order_relaxed);
                log_first_n(g_st_MB2WC, "HIT ", (const char*)src, en);
                return g_real_MB2WC(cp, flags, en, srcLen, dst, dstCap);
            }
        }
    }
    if (srcLen == -1 && src) {
        log_first_n(g_st_MB2WC, "miss", (const char*)src, nullptr);
    }
    return g_real_MB2WC(cp, flags, src, srcLen, dst, dstCap);
}


extern "C" void iat_register_patched(void* addr, const char* full_en,
                                      int en_len) {
    if (!addr || !full_en || en_len <= 0) return;
    ensure_patched_init();
    DWORD key = (DWORD)(uintptr_t)addr;
    EnterCriticalSection(&g_patched_cs);
    g_patched[key] = { full_en, en_len };
    int n = (int)g_patched.size();
    LeaveCriticalSection(&g_patched_cs);
    g_patched_count.store(n, std::memory_order_relaxed);
}


}  // namespace


extern "C" void start_iat_translator() {
    // Pre-load the table so the first hook call doesn't pay the
    // file-IO cost (and so the log shows the load result before any
    // hit/miss line).
    load_table_once();

    HMODULE exe = GetModuleHandleA(nullptr);
    LOG("iat: installing IAT hooks on %p", exe);

    // Layer 1 — IAT hooks on sakuranotoki.exe's static import table.
    // These catch calls from the LAUNCHER (CrackProof unpacker stub)
    // but in practice get 0 hits from the engine, because CrackProof
    // resolves engine imports through its own private path and caches
    // the resolved function pointers, never going back through the
    // EXE's IAT. We keep them so we can see if the launcher itself
    // ever processes a JP string (it doesn't, on every test so far).
    g_real_lstrlenA = (PFN_lstrlenA)IATHook(
        exe, "KERNEL32.dll", "lstrlenA",  (void*)&Hooked_lstrlenA);
    g_real_lstrcpyA = (PFN_lstrcpyA)IATHook(
        exe, "KERNEL32.dll", "lstrcpyA",  (void*)&Hooked_lstrcpyA);
    g_real_lstrcpynA = (PFN_lstrcpynA)IATHook(
        exe, "KERNEL32.dll", "lstrcpynA", (void*)&Hooked_lstrcpynA);
    g_real_lstrcatA = (PFN_lstrcatA)IATHook(
        exe, "KERNEL32.dll", "lstrcatA",  (void*)&Hooked_lstrcatA);
    g_real_MB2WC = (PFN_MB2WC)IATHook(
        exe, "KERNEL32.dll", "MultiByteToWideChar",
        (void*)&Hooked_MB2WC);

    // If any IAT hook came back null the import wasn't in the EXE's
    // static IAT — fall back to the real proc address from kernel32
    // so the hook function can still call through.
    HMODULE k32 = GetModuleHandleA("KERNEL32.dll");
    if (!g_real_lstrlenA)
        g_real_lstrlenA  = (PFN_lstrlenA)GetProcAddress(k32, "lstrlenA");
    if (!g_real_lstrcpyA)
        g_real_lstrcpyA  = (PFN_lstrcpyA)GetProcAddress(k32, "lstrcpyA");
    if (!g_real_lstrcpynA)
        g_real_lstrcpynA = (PFN_lstrcpynA)GetProcAddress(k32, "lstrcpynA");
    if (!g_real_lstrcatA)
        g_real_lstrcatA  = (PFN_lstrcatA)GetProcAddress(k32, "lstrcatA");
    if (!g_real_MB2WC)
        g_real_MB2WC     = (PFN_MB2WC)GetProcAddress(k32, "MultiByteToWideChar");

    // Layer 2 — Detours-style direct prologue patches on kernel32
    // exports. THIS is the hook that the engine actually hits —
    // because the engine calls these functions through cached
    // pointers that point at the export bytes, patching the
    // prologue intercepts every caller regardless of how they
    // resolved the function. Trampolines override the IAT-layer
    // function pointers (g_real_*) so when our hook calls through,
    // it goes around the prologue patch via the trampoline rather
    // than recursing into itself.
    //
    // We pass &g_real_* directly as ExportHookInstall's
    // trampoline_out — the install routine writes the trampoline
    // pointer BEFORE patching the target, so any recursive call
    // (e.g. log_msg using MultiByteToWideChar to detect UTF-8) goes
    // through the trampoline and not back into the hook.
    LOG("iat: installing direct-export hooks (kernel32 prologue patches)");
    if (g_real_lstrlenA) {
        ExportHookInstall((void*)g_real_lstrlenA,
                          (void*)&Hooked_lstrlenA,
                          (void**)&g_real_lstrlenA);
    }
    if (g_real_lstrcpyA) {
        ExportHookInstall((void*)g_real_lstrcpyA,
                          (void*)&Hooked_lstrcpyA,
                          (void**)&g_real_lstrcpyA);
    }
    if (g_real_lstrcpynA) {
        ExportHookInstall((void*)g_real_lstrcpynA,
                          (void*)&Hooked_lstrcpynA,
                          (void**)&g_real_lstrcpynA);
    }
    if (g_real_lstrcatA) {
        ExportHookInstall((void*)g_real_lstrcatA,
                          (void*)&Hooked_lstrcatA,
                          (void**)&g_real_lstrcatA);
    }
    if (g_real_MB2WC) {
        ExportHookInstall((void*)g_real_MB2WC,
                          (void*)&Hooked_MB2WC,
                          (void**)&g_real_MB2WC);
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
