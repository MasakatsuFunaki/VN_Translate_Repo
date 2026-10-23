// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// text_finder.cpp — runtime discovery of the dialogue-reader function.
//
// Why this exists:
//   Static analysis in Ghidra hits a wall — the Artemis engine
//   dispatches dialogue rendering through an indirect call (script
//   command table -> vtable -> render path) and Ghidra's xref graph
//   doesn't trace that. We saw FUN_00578380 (the
//   character_reference_to_utf8 wrapper) has only 1 caller, and that
//   caller has 1 caller that itself has 0 callers — clearly cut off
//   at an indirect dispatch.
//
//   But we DO know exactly which BYTES the engine reads to render
//   dialogue: memory_scanner finds them every pass. So the simplest
//   way to find the reader function is to ask the CPU: install a
//   hardware data breakpoint on a patched dialogue address, let the
//   CPU page-fault on first read, and log EIP from the VEH. The EIP
//   tells us the function that reads dialogue text — exactly the
//   function we want to hook.
//
// Implementation:
//   1. memory_scanner calls text_finder_register_patched() for each
//      successful patch. We keep the first 4 addresses (one per
//      hardware DR register).
//   2. After enough patches accumulate (4), arm DR0..3 on EVERY
//      thread in the process via Toolhelp + SuspendThread/
//      GetThreadContext/SetThreadContext. DR7 sets type=READ_OR_WRITE,
//      len=1 byte.
//   3. AddVectoredExceptionHandler catches EXCEPTION_SINGLE_STEP
//      (which is what HW BP reports). DR6 status bits tell us which
//      DR0..3 fired. We log EIP + which DR + the read-target address.
//   4. Filter: only log EIPs in the engine's RVA range (0x00400000 -
//      0x00B00000). DLL-internal noise gets dropped.
//   5. Cap total log lines so a hot loop doesn't flood the file.
//   6. After N hits, disarm — we have the EIPs we need; let the game
//      run normally afterward.

#include "text_finder.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdint>


namespace {


constexpr int kSlotCount = 4;            // DR0..DR3
constexpr DWORD kEngineRvaLo = 0x00400000;
constexpr DWORD kEngineRvaHi = 0x00B00000;
constexpr int kMaxHits = 200;            // disarm after this many

void* g_addr_slots[kSlotCount] = {};
int   g_addr_count = 0;
bool  g_armed = false;
PVOID g_veh_handle = nullptr;
std::atomic<int> g_hits{0};
std::atomic<int> g_skipped_dll{0};


bool eip_in_engine(DWORD eip) {
    return eip >= kEngineRvaLo && eip < kEngineRvaHi;
}


LONG WINAPI text_finder_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* ctx = ep->ContextRecord;
    DWORD dr6 = (DWORD)ctx->Dr6;

    // DR6 low 4 bits indicate which DR fired.
    int which = -1;
    if      (dr6 & 0x1) which = 0;
    else if (dr6 & 0x2) which = 1;
    else if (dr6 & 0x4) which = 2;
    else if (dr6 & 0x8) which = 3;
    if (which < 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD eip = (DWORD)ctx->Eip;
    if (!eip_in_engine(eip)) {
        // Hit from our own DLL or kernel32 etc. — ignore.
        g_skipped_dll.fetch_add(1, std::memory_order_relaxed);
    } else {
        int n = g_hits.fetch_add(1, std::memory_order_relaxed);
        if (n < kMaxHits) {
            DWORD addr = (which == 0) ? (DWORD)ctx->Dr0
                       : (which == 1) ? (DWORD)ctx->Dr1
                       : (which == 2) ? (DWORD)ctx->Dr2
                                      : (DWORD)ctx->Dr3;
            LOG("text-finder hit#%d EIP=%08X EAX=%08X ECX=%08X EDX=%08X "
                "ESI=%08X EDI=%08X reads=%08X(DR%d)",
                n + 1, eip, (DWORD)ctx->Eax, (DWORD)ctx->Ecx,
                (DWORD)ctx->Edx, (DWORD)ctx->Esi, (DWORD)ctx->Edi,
                addr, which);
        }
    }

    // Clear DR6 status bits so the next access fires too. The CPU
    // sets DR6.RTM (bit 16) on resume; setting RF in EFLAGS prevents
    // immediate re-trigger on the SAME instruction.
    ctx->Dr6 = ctx->Dr6 & ~0xFu;
    ctx->EFlags |= 0x10000;  // RF — skip-this-instruction-for-BP

    return EXCEPTION_CONTINUE_EXECUTION;
}


bool arm_one_thread(DWORD tid) {
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT
                           | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) {
        LOG("text-finder: OpenThread(tid=%lu) failed gle=%lu",
            tid, GetLastError());
        return false;
    }

    DWORD prev = SuspendThread(th);
    if (prev == (DWORD)-1) {
        LOG("text-finder: SuspendThread(tid=%lu) failed gle=%lu",
            tid, GetLastError());
        CloseHandle(th);
        return false;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &ctx)) {
        LOG("text-finder: GetThreadContext(tid=%lu) failed gle=%lu",
            tid, GetLastError());
        ResumeThread(th);
        CloseHandle(th);
        return false;
    }

    ctx.Dr0 = (g_addr_count > 0) ? (DWORD_PTR)g_addr_slots[0] : 0;
    ctx.Dr1 = (g_addr_count > 1) ? (DWORD_PTR)g_addr_slots[1] : 0;
    ctx.Dr2 = (g_addr_count > 2) ? (DWORD_PTR)g_addr_slots[2] : 0;
    ctx.Dr3 = (g_addr_count > 3) ? (DWORD_PTR)g_addr_slots[3] : 0;

    // DR7 layout (32-bit):
    //   bits  0-1   L0/G0 (DR0 local/global enable)
    //   bits  2-3   L1/G1
    //   bits  4-5   L2/G2
    //   bits  6-7   L3/G3
    //   bits 16-17  R/W0 (00=execute, 01=write, 11=read/write)
    //   bits 18-19  LEN0 (00=1B, 01=2B, 11=4B)
    //   bits 20-21  R/W1   bits 22-23 LEN1
    //   ...
    DWORD_PTR dr7 = 0;
    for (int i = 0; i < g_addr_count; ++i) {
        // L<i> = local-enable for DR<i>
        dr7 |= (1u << (i * 2));
        // RW<i> = 11 (read/write data BP)
        dr7 |= (3u << (16 + i * 4));
        // LEN<i> = 00 (1 byte)
    }
    ctx.Dr7 = dr7;

    if (!SetThreadContext(th, &ctx)) {
        LOG("text-finder: SetThreadContext(tid=%lu) failed gle=%lu",
            tid, GetLastError());
        ResumeThread(th);
        CloseHandle(th);
        return false;
    }
    ResumeThread(th);
    CloseHandle(th);
    return true;
}


bool arm_all_threads() {
    DWORD pid = GetCurrentProcessId();
    DWORD self_tid = GetCurrentThreadId();   // CRITICAL: skip self
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        LOG("text-finder: CreateToolhelp32Snapshot FAILED gle=%lu",
            GetLastError());
        return false;
    }

    // Two-pass: collect TIDs first (skip self to avoid SuspendThread
    // deadlocking the calling thread), then operate. Without this
    // skip we suspend ourselves and never reach the resume call —
    // identical bug as function_hook.cpp had before its fix.
    DWORD tids[64];
    int n_tids = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self_tid) continue;
            if (n_tids >= (int)(sizeof(tids) / sizeof(tids[0]))) break;
            tids[n_tids++] = te.th32ThreadID;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    int armed = 0;
    for (int i = 0; i < n_tids; ++i) {
        if (arm_one_thread(tids[i])) ++armed;
    }
    LOG("text-finder: armed DR0..%d on %d / %d threads "
        "(self tid=%lu skipped)",
        g_addr_count - 1, armed, n_tids, self_tid);
    return armed > 0;
}


DWORD WINAPI arm_thread_entry(LPVOID) {
    // Sleep briefly so memory_scanner has a chance to patch — it
    // needs CrackProof to finish unpacking and the script to be
    // loaded. By 3 seconds we should have a few patched addresses.
    Sleep(3000);

    if (g_addr_count == 0) {
        LOG("text-finder: no patched addresses registered yet, retrying");
        Sleep(2000);
    }
    if (g_addr_count == 0) {
        LOG("text-finder: still no addresses; aborting (memory_scanner "
            "may have failed to find dialogue this run)");
        return 1;
    }

    g_veh_handle = AddVectoredExceptionHandler(1, text_finder_veh);
    if (!g_veh_handle) {
        LOG("text-finder: AddVectoredExceptionHandler failed gle=%lu",
            GetLastError());
        return 1;
    }

    LOG("text-finder: arming DR0..%d on:", g_addr_count - 1);
    for (int i = 0; i < g_addr_count; ++i) {
        LOG("  DR%d -> %p", i, g_addr_slots[i]);
    }

    // Initial arm
    arm_all_threads();
    g_armed = true;

    // Periodic re-arm to catch threads created AFTER the initial arm.
    // Hypothesis: the engine's render thread might be spawned on
    // demand (when first dialogue is rendered) and inherits zero DR
    // registers from CreateThread, so its dialogue reads escape our
    // initial arm. Re-arming every 1s catches new threads within
    // that window. We stop after 60s — long enough to play through
    // ~10-20 dialogue lines.
    for (int i = 0; i < 60; ++i) {
        Sleep(1000);
        if (g_hits.load() >= 10) break;   // got hits — stop
        arm_all_threads();
    }
    LOG("text-finder: periodic re-arm finished (hits=%d)", g_hits.load());
    return 0;
}


}  // namespace


extern "C" void text_finder_register_patched(void* addr) {
    if (g_armed) return;        // ignore once armed
    if (!addr) return;
    // Dedup
    for (int i = 0; i < g_addr_count; ++i) {
        if (g_addr_slots[i] == addr) return;
    }
    if (g_addr_count >= kSlotCount) return;
    g_addr_slots[g_addr_count++] = addr;
}


extern "C" void start_text_finder() {
    LOG("text-finder: started (will arm HW data BPs after first scan)");
    HANDLE h = CreateThread(nullptr, 0, arm_thread_entry, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}


extern "C" void log_text_finder_summary() {
    LOG("text-finder summary: hits=%d skipped(non-engine EIP)=%d armed=%d",
        g_hits.load(), g_skipped_dll.load(), g_armed ? 1 : 0);
    if (g_veh_handle) {
        RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = nullptr;
    }
}
