// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// page_guard.cpp — final attempt to identify the engine's dialogue-
// read function via PAGE_GUARD.
//
// Why PAGE_GUARD when HW BPs failed:
//   text_finder armed HW DATA breakpoints on 4 patched dialogue
//   addresses across 5 process threads. ~46K BP fires happened, but
//   ZERO had EIP in the engine's RVA range — every fire was from our
//   own scanner thread's memchr/memcmp loops. The engine never
//   triggered the BPs. Three reasons that could be:
//     (a) Engine renders from a thread spawned AFTER our arm — ruled
//         out by adding periodic re-arm; thread count stayed flat.
//     (b) Engine reads via kernel-mode memcpy/ReadFile — possible.
//     (c) The 4 addresses we armed are leaked/duplicate copies; the
//         engine reads a 5th+ address we couldn't reach (4 DR slots
//         is the per-thread hardware limit). Probable.
//
//   PAGE_GUARD covers a full 4KB page regardless of thread or DR
//   limit, fires on every access (incl. wide CPU-word reads that
//   might overlap our address by a few bytes), and is process-wide.
//   If hypothesis (b) or (c) is true, page-guard catches what HW BPs
//   missed.
//
// How PAGE_GUARD works:
//   VirtualProtect with PAGE_GUARD makes any access to that page
//   raise EXCEPTION_GUARD_PAGE (STATUS_GUARD_PAGE_VIOLATION). The OS
//   then AUTOMATICALLY clears the PAGE_GUARD flag (one-shot). To get
//   subsequent accesses to fire, we must re-arm the flag after each
//   access. Standard pattern: in the GUARD_PAGE VEH, set EFLAGS.TF
//   (trap flag) so a SINGLE_STEP exception fires after the next
//   instruction. In the SINGLE_STEP VEH, re-apply PAGE_GUARD and
//   clear TF.
//
// Anti-tamper safety:
//   PAGE_GUARD modifies page protection metadata, not page contents.
//   CrackProof watches the bytes of sakuranotoki.exe's .text section
//   and would catch byte modifications, but it doesn't (and can't
//   easily) check protection flags on every page. Other engines we
//   shipped against (FRATERNITE_HD's SoftDenchi, mushigurui's
//   anti-tamper) have not detected page-guard either.
//
// Cost:
//   Every access to the guarded page costs one exception cycle
//   (GUARD_PAGE VEH → single-step → SINGLE_STEP VEH → re-arm). For a
//   tight loop that hammers the page this is a real slowdown. To
//   keep the cost bounded we disarm after kMaxHits engine fires —
//   that gives us enough samples to identify the read function and
//   stops the perf hit.

#include "page_guard.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>


namespace {


constexpr DWORD kEngineRvaLo = 0x00400000;
constexpr DWORD kEngineRvaHi = 0x00B00000;
constexpr int   kMaxHits     = 100;         // disarm after this
constexpr DWORD kPageSize    = 0x1000;
constexpr DWORD STATUS_GUARD_PAGE = 0x80000001;
constexpr int   kMaxPages    = 8;           // capped: more = perf hang

// Only guard pages whose ADDRESS lies in the engine's plausible
// heap range. The previous (24-page) build registered pages at
// 0x02E7x000 and 0x03A6x000 — those are system DLL / loaded-image
// data pages; setting PAGE_GUARD on them caused every system call
// that touched those pages to fault, hanging the game on startup.
// The engine's heap allocations are clustered in [0x00400000,
// 0x01000000) on every run we've observed.
constexpr DWORD kPageAddrLo = 0x00400000;
constexpr DWORD kPageAddrHi = 0x01000000;

// Set of pages we're guarding. Memory_scanner registers every
// successful inline-ext / heap-ext patch with us; we dedup by page
// address. Each entry's page gets PAGE_GUARD applied. Last-page-faulted
// is tracked so the single-step VEH knows which page to re-arm.
DWORD g_pages[kMaxPages];
int   g_n_pages = 0;
DWORD g_last_faulted_page = 0;
PVOID g_veh_handle = nullptr;
std::atomic<int> g_hits{0};
std::atomic<int> g_skipped{0};
std::atomic<int> g_step_resets{0};
std::atomic<bool> g_armed{false};
std::atomic<bool> g_disarmed{false};
CRITICAL_SECTION g_pages_lock;
bool g_pages_lock_init = false;


bool eip_in_engine(DWORD eip) {
    return eip >= kEngineRvaLo && eip < kEngineRvaHi;
}


bool page_in_set(DWORD page) {
    // Linear search — kMaxPages is small. Called from VEH and from
    // re-arm; both paths take the lock briefly. Could be lock-free
    // since the set is append-only after init, but the lock is cheap.
    EnterCriticalSection(&g_pages_lock);
    bool found = false;
    for (int i = 0; i < g_n_pages; ++i) {
        if (g_pages[i] == page) { found = true; break; }
    }
    LeaveCriticalSection(&g_pages_lock);
    return found;
}


void rearm_one_page(DWORD page) {
    if (g_disarmed.load(std::memory_order_relaxed)) return;
    DWORD old_prot = 0;
    VirtualProtect((LPVOID)(uintptr_t)page, kPageSize,
                   PAGE_READWRITE | PAGE_GUARD, &old_prot);
}


void disarm_guard() {
    if (g_disarmed.exchange(true, std::memory_order_relaxed)) return;
    EnterCriticalSection(&g_pages_lock);
    for (int i = 0; i < g_n_pages; ++i) {
        DWORD old_prot = 0;
        VirtualProtect((LPVOID)(uintptr_t)g_pages[i], kPageSize,
                       PAGE_READWRITE, &old_prot);
    }
    LeaveCriticalSection(&g_pages_lock);
    LOG("page-guard: disarmed %d pages (hits=%d skipped=%d step_resets=%d)",
        g_n_pages, g_hits.load(), g_skipped.load(), g_step_resets.load());
}


LONG WINAPI page_guard_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* ctx = ep->ContextRecord;

    if (code == STATUS_GUARD_PAGE) {
        DWORD fault_addr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
        DWORD fault_page = fault_addr & ~(kPageSize - 1);

        // Defensive: this VEH gets called for ANY guard-page fault in
        // the process. If the page isn't one of ours, bail so other
        // OS components (heap manager, stack guards, etc.) handle it.
        if (!page_in_set(fault_page)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        DWORD eip = (DWORD)ctx->Eip;
        if (eip_in_engine(eip)) {
            int n = g_hits.fetch_add(1, std::memory_order_relaxed);
            if (n < kMaxHits) {
                DWORD access_type =
                    (DWORD)ep->ExceptionRecord->ExceptionInformation[0];
                LOG("page-guard hit#%d EIP=%08X access=%s addr=%08X "
                    "EAX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X",
                    n + 1, eip,
                    access_type == 0 ? "READ" : (access_type == 1 ? "WRITE"
                                                                  : "EXEC"),
                    fault_addr,
                    (DWORD)ctx->Eax, (DWORD)ctx->Ecx, (DWORD)ctx->Edx,
                    (DWORD)ctx->Esi, (DWORD)ctx->Edi);
            } else if (n == kMaxHits) {
                disarm_guard();
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        } else {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
        }

        // PAGE_GUARD on the faulting page has been auto-cleared by
        // the OS. Stash which page faulted so the SINGLE_STEP handler
        // knows which one to re-arm; set TF for single-step.
        g_last_faulted_page = fault_page;
        ctx->EFlags |= 0x100;   // TF
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_SINGLE_STEP) {
        // Distinguish OUR TF-induced step from HW BP fires (DR0..3
        // would have a B0..B3 bit set in DR6).
        DWORD dr6 = (DWORD)ctx->Dr6;
        if ((dr6 & 0xF) == 0 && g_last_faulted_page) {
            g_step_resets.fetch_add(1, std::memory_order_relaxed);
            ctx->EFlags &= ~0x100u;
            DWORD page = g_last_faulted_page;
            g_last_faulted_page = 0;
            rearm_one_page(page);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}


DWORD WINAPI page_guard_thread(LPVOID) {
    // Wait for memory_scanner to populate at least one page entry.
    for (int i = 0; i < 30; ++i) {
        if (g_n_pages > 0) break;
        Sleep(200);
    }
    if (g_n_pages == 0) {
        LOG("page-guard: no pages registered after 6s, aborting");
        return 1;
    }

    LOG("page-guard: %d unique page(s) registered:", g_n_pages);
    EnterCriticalSection(&g_pages_lock);
    for (int i = 0; i < g_n_pages; ++i) {
        LOG("  page[%d] = %08X", i, g_pages[i]);
    }
    LeaveCriticalSection(&g_pages_lock);

    g_veh_handle = AddVectoredExceptionHandler(1, page_guard_veh);
    if (!g_veh_handle) {
        LOG("page-guard: AddVectoredExceptionHandler FAILED gle=%lu",
            GetLastError());
        return 1;
    }

    int armed_pages = 0;
    EnterCriticalSection(&g_pages_lock);
    for (int i = 0; i < g_n_pages; ++i) {
        DWORD old_prot = 0;
        if (VirtualProtect((LPVOID)(uintptr_t)g_pages[i], kPageSize,
                           PAGE_READWRITE | PAGE_GUARD, &old_prot)) {
            ++armed_pages;
        } else {
            LOG("page-guard: VirtualProtect(%08X) failed gle=%lu",
                g_pages[i], GetLastError());
        }
    }
    LeaveCriticalSection(&g_pages_lock);
    LOG("page-guard: armed %d / %d pages", armed_pages, g_n_pages);
    if (armed_pages == 0) return 1;
    g_armed = true;

    // Run for up to 60 seconds OR until kMaxHits fires.
    for (int i = 0; i < 60; ++i) {
        Sleep(1000);
        if (g_disarmed.load() || g_hits.load() >= kMaxHits) break;
    }

    if (!g_disarmed.load()) {
        disarm_guard();
    }
    return 0;
}


}  // namespace


extern "C" void page_guard_register_patched(void* addr) {
    if (!addr) return;
    if (g_armed.load(std::memory_order_relaxed)) return;  // already armed
    DWORD page = (DWORD)(uintptr_t)addr & ~(kPageSize - 1);

    // Filter: only register pages within the engine's plausible heap
    // range. PAGE_GUARD on system DLL pages or loaded-image data
    // hangs the game.
    if (page < kPageAddrLo || page >= kPageAddrHi) {
        return;
    }

    EnterCriticalSection(&g_pages_lock);
    // Dedup
    bool found = false;
    for (int i = 0; i < g_n_pages; ++i) {
        if (g_pages[i] == page) { found = true; break; }
    }
    if (!found && g_n_pages < kMaxPages) {
        g_pages[g_n_pages++] = page;
    }
    LeaveCriticalSection(&g_pages_lock);
}


extern "C" void start_page_guard() {
    InitializeCriticalSection(&g_pages_lock);
    g_pages_lock_init = true;
    LOG("page-guard: starting (will arm once pages register)");
    HANDLE h = CreateThread(nullptr, 0, page_guard_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else LOG("page-guard: CreateThread FAILED gle=%lu", GetLastError());
}


extern "C" void log_page_guard_summary() {
    LOG("page-guard summary: armed=%d pages=%d hits=%d "
        "skipped(non-engine)=%d step_resets=%d disarmed=%d",
        g_armed.load() ? 1 : 0, g_n_pages,
        g_hits.load(), g_skipped.load(), g_step_resets.load(),
        g_disarmed.load() ? 1 : 0);
    if (g_veh_handle) {
        RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = nullptr;
    }
}
