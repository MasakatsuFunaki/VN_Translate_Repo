// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// function_hook.cpp — runtime EXEC hardware breakpoints on candidate
// engine functions, used to identify the dialogue-render path.
//
// Background:
//   memory_scanner has been patching dialogue buffers reliably but
//   the engine renders only `jp_len` bytes regardless of the buffer
//   contents — there's a separate cached length we couldn't locate
//   in the wrapper or trailing-pointer context. The remaining option
//   is to hook the engine function that READS the buffer and replace
//   its (text_ptr, length) parameters with our (en_ptr, en_size)
//   before it does any work. This is the FAVORITE / mushigurui /
//   amakano2pe pattern, but using HW execute breakpoints instead of
//   inline JMP rel32 — necessary because CrackProof checks the
//   engine's .text bytes and would terminate on inline patches.
//
// Approach (this module is the DIAGNOSTIC half of the work):
//   1. Pick top-N candidate functions from FindTextRenderers.java's
//      ranking, biased toward signatures with (text*, length) shape:
//        FUN_00611250, FUN_00610fb0, FUN_005faa60, FUN_004c9d70.
//   2. After CrackProof has unpacked (Sleep ~2s), enumerate every
//      thread in the process via Toolhelp32 and set DR0..DR3 on each
//      to the candidate function entry points, with R/W=00 (execute)
//      and LEN=00 (1 byte).
//   3. AddVectoredExceptionHandler catches EXCEPTION_SINGLE_STEP from
//      the HW BP, identifies which DR fired via DR6, reads the
//      parameters from ECX (this) + ESP-relative slots (cdecl args
//      after return address), and logs them.
//   4. After enough hits to confirm which candidate carries dialogue,
//      we'll either disarm or upgrade this hook to substitute the
//      params (replace JP pointer with our EN buffer pointer +
//      replace length with en.size).
//
// Failure mode that bit text_finder.cpp earlier:
//   text_finder's arm_all_threads() called CreateToolhelp32Snapshot
//   and silently returned false on failure (no GLE log) — so
//   "armed=0" with no diagnostic. This module logs GLE for every
//   step and never silently bails.

#include "function_hook.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdint>


namespace {


// Top candidates from FindStringConsumers.java — functions that
// read MSVC std::string size (offset +0x10) and the buffer pointer
// (offset 0). Ranked by call count (xrefs) and byte-iteration
// presence — the engine's dialogue render function should be called
// frequently AND walk text bytes.
struct Cand {
    DWORD rva;
    const char* name;
};
// Candidates from FindStringConsumers.java run AFTER PE-section-header
// patch (Sakuranotoki_unpacked_v2 project). The previous run was on an
// unpatched dump — Ghidra read sections from wrong file offsets, so its
// rankings were unreliable. New top candidates (sorted by std::string-
// consumer score):
//   #1  FUN_00459ef0  score=1010  xrefs=1  size=5933  (NEW — top score)
//   #12 FUN_00460587  score=425   xrefs=13           (kept — high xref)
//   #15 FUN_00432795  score=361   xrefs=17           (kept — highest xref)
//   #9  FUN_00484291  score=502   xrefs=5  byteIter=7 (kept — byte-iter)
// FUN_0060c240 dropped — not in the new top 15.
const Cand kCandidates[] = {
    { 0x00059ef0, "FUN_00459ef0 (score=1010, sz=5933)" },  // top score
    { 0x00032795, "FUN_00432795 (xref=17, size=5701)" },   // most-called
    { 0x00060587, "FUN_00460587 (xref=13, size=4216)" },   // 13 xrefs
    { 0x00084291, "FUN_00484291 (xref=5, byteIter=7)" },   // byte loop
};
constexpr int kCandCount = sizeof(kCandidates) / sizeof(kCandidates[0]);

DWORD g_addr[kCandCount];   // VA = base + rva
PVOID g_veh_handle = nullptr;
bool  g_armed = false;
std::atomic<int> g_hits[kCandCount];
std::atomic<int> g_total_hits{0};
constexpr int kMaxHitsPerCand = 50;
constexpr int kMaxParamLen = 80;


// SEH-safe NUL-bounded copy of a possibly-bad CP932/UTF-8 string into
// a fixed buffer. Returns chars copied. Lives in its own __declspec
// noinline helper so the SEH frame doesn't conflict with the C++
// stack frame of the VEH (which has no destructible objects but the
// compiler is finicky about mixing).
__declspec(noinline)
int safe_copy_str(const char* s, char* out, int max_len) {
    if (!s) {
        out[0] = '\0';
        return 0;
    }
    __try {
        int n = 0;
        while (n < max_len - 1) {
            char c = s[n];
            if (c == 0) break;
            // Replace control bytes with '.' so log lines don't get
            // corrupted by truncation/binary chars.
            out[n] = (c >= 0x20 && c < 0x7F) ? c
                   : ((unsigned char)c >= 0x80 ? c : '.');
            ++n;
        }
        out[n] = '\0';
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return -1;
    }
}


LONG WINAPI fn_hook_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    DWORD dr6 = (DWORD)ctx->Dr6;

    int which = -1;
    if      (dr6 & 0x1) which = 0;
    else if (dr6 & 0x2) which = 1;
    else if (dr6 & 0x4) which = 2;
    else if (dr6 & 0x8) which = 3;
    if (which < 0 || which >= kCandCount) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    int hits = g_hits[which].fetch_add(1, std::memory_order_relaxed);
    if (hits < kMaxHitsPerCand) {
        // Inspect the parameters. For thiscall: ECX = this, then the
        // remaining args are at [esp+4], [esp+8], ... (ret addr at
        // [esp]). For cdecl/stdcall: args at [esp+4], [esp+8], ...
        DWORD esp = (DWORD)ctx->Esp;
        DWORD ecx = (DWORD)ctx->Ecx;
        DWORD ret_addr = 0;
        DWORD a1 = 0, a2 = 0, a3 = 0, a4 = 0;
        SIZE_T r;
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)esp,       &ret_addr, 4, &r);
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(esp + 4), &a1,       4, &r);
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(esp + 8), &a2,       4, &r);
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(esp + 12),&a3,       4, &r);
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(esp + 16),&a4,       4, &r);

        // Try interpreting a1/a2/a3 as char* and dump preview if
        // they look like a high-byte (CP932/UTF-8) string. The
        // dialogue path will have a JP-bytes parameter somewhere.
        char preview1[kMaxParamLen + 1] = {};
        char preview2[kMaxParamLen + 1] = {};
        char preview3[kMaxParamLen + 1] = {};
        if (a1 >= 0x10000 && a1 < 0xC0000000)
            safe_copy_str((const char*)(uintptr_t)a1, preview1, kMaxParamLen);
        if (a2 >= 0x10000 && a2 < 0xC0000000)
            safe_copy_str((const char*)(uintptr_t)a2, preview2, kMaxParamLen);
        if (a3 >= 0x10000 && a3 < 0xC0000000)
            safe_copy_str((const char*)(uintptr_t)a3, preview3, kMaxParamLen);

        LOG("fn-hook hit#%d %s @DR%d  ecx=%08X ret=%08X "
            "a1=%08X a2=%08X a3=%08X a4=%08X",
            hits + 1, kCandidates[which].name, which,
            ecx, ret_addr, a1, a2, a3, a4);
        if (preview1[0]) LOG("    a1->[%s]", preview1);
        if (preview2[0]) LOG("    a2->[%s]", preview2);
        if (preview3[0]) LOG("    a3->[%s]", preview3);

        g_total_hits.fetch_add(1, std::memory_order_relaxed);
    }

    // RF skips the BP for this single instruction — without it, we'd
    // loop forever on the same instruction (DR0..3 fire BEFORE the
    // instruction executes for execute-type BPs).
    ctx->EFlags |= 0x10000;
    ctx->Dr6 = ctx->Dr6 & ~0xFu;
    return EXCEPTION_CONTINUE_EXECUTION;
}


bool arm_one_thread(DWORD tid) {
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT
                           | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) {
        LOG("fn-hook: OpenThread(tid=%lu) failed gle=%lu",
            tid, GetLastError());
        return false;
    }

    DWORD prev = SuspendThread(th);
    if (prev == (DWORD)-1) {
        LOG("fn-hook: SuspendThread(tid=%lu) failed gle=%lu",
            tid, GetLastError());
        CloseHandle(th);
        return false;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL;
    if (!GetThreadContext(th, &ctx)) {
        LOG("fn-hook: GetThreadContext(tid=%lu) failed gle=%lu",
            tid, GetLastError());
        ResumeThread(th);
        CloseHandle(th);
        return false;
    }

    ctx.Dr0 = (DWORD_PTR)g_addr[0];
    ctx.Dr1 = (DWORD_PTR)g_addr[1];
    ctx.Dr2 = (DWORD_PTR)g_addr[2];
    ctx.Dr3 = (DWORD_PTR)g_addr[3];

    // DR7: enable L0..L3 with RW=00 (execute) and LEN=00 (1 byte).
    DWORD_PTR dr7 = 0;
    for (int i = 0; i < kCandCount; ++i) {
        if (g_addr[i]) dr7 |= (1u << (i * 2));        // local enable
        // RW<i> bits at 16+i*4: leave as 00 (execute)
        // LEN<i> bits at 18+i*4: leave as 00 (1 byte)
    }
    // Bit 10 must be set per Intel SDM (always-on reserved bit).
    dr7 |= (1u << 10);
    ctx.Dr7 = dr7;

    if (!SetThreadContext(th, &ctx)) {
        LOG("fn-hook: SetThreadContext(tid=%lu) failed gle=%lu",
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
        LOG("fn-hook: CreateToolhelp32Snapshot FAILED gle=%lu",
            GetLastError());
        return false;
    }

    // Two-pass: collect TIDs first, then operate on each. This avoids
    // holding the snapshot while we suspend other threads (which can
    // race with the loader / engine threads).
    DWORD tids[64];
    int n_tids = 0;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (!Thread32First(snap, &te)) {
        LOG("fn-hook: Thread32First FAILED gle=%lu", GetLastError());
        CloseHandle(snap);
        return false;
    }
    do {
        if (te.th32OwnerProcessID != pid) continue;
        if (te.th32ThreadID == self_tid) continue;   // skip self
        if (n_tids >= (int)(sizeof(tids) / sizeof(tids[0]))) break;
        tids[n_tids++] = te.th32ThreadID;
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);

    LOG("fn-hook: enumerated %d in-process threads (excl. self tid=%lu)",
        n_tids, self_tid);

    int armed = 0;
    for (int i = 0; i < n_tids; ++i) {
        if (arm_one_thread(tids[i])) ++armed;
    }
    LOG("fn-hook: armed on %d / %d threads", armed, n_tids);
    return armed > 0;
}


// Read 2 bytes at addr and decide if they look like a real x86 prologue
// start: 8B FF (hot-patch hint), 55 8B (push ebp; mov ebp, esp), 55 6A
// (push ebp; push imm), 83 EC (sub esp, imm), or 6A (push imm).
bool prologue_bytes_valid(DWORD addr) {
    BYTE pro[2] = {};
    SIZE_T r = 0;
    ReadProcessMemory(GetCurrentProcess(),
                      (LPCVOID)(uintptr_t)addr, pro, 2, &r);
    if (r < 2) return false;
    return (pro[0] == 0x8B && pro[1] == 0xFF)
        || (pro[0] == 0x55 && pro[1] == 0x8B)
        || (pro[0] == 0x55 && pro[1] == 0x6A)
        || (pro[0] == 0x83 && pro[1] == 0xEC)
        || (pro[0] == 0x6A);
}


DWORD WINAPI arm_thread_entry(LPVOID) {
    HMODULE exe = GetModuleHandleA(nullptr);
    DWORD base = (DWORD)(uintptr_t)exe;
    LOG("fn-hook: exe base=%p, %d candidates", exe, kCandCount);

    for (int i = 0; i < kCandCount; ++i) {
        g_addr[i] = base + kCandidates[i].rva;
    }

    // PRIOR RUN BUG: we polled for engine code for only 30 s, then armed
    // anyway. The proxy log showed prologue bytes were ZEROS / RTTI data
    // at arm time — CrackProof hadn't unpacked yet. Hits stayed at 0
    // because (a) HW BPs were on stale addresses and (b) engine threads
    // spawned AFTER our arm don't inherit DR registers.
    //
    // FIX: poll forever (no timeout) until ALL candidates have valid
    // prologues, then re-arm every 2 s indefinitely. Re-arming the same
    // thread is idempotent (DRs already set); new threads get armed as
    // they appear. This lives for the whole game session.
    LOG("fn-hook: waiting for engine bytes to appear at candidate addrs "
        "(no timeout — CrackProof unpacks asynchronously)");

    int valid_count = 0;
    for (int wait_iter = 0; ; ++wait_iter) {
        Sleep(500);
        valid_count = 0;
        for (int i = 0; i < kCandCount; ++i) {
            if (prologue_bytes_valid(g_addr[i])) ++valid_count;
        }
        if (valid_count == kCandCount) {
            LOG("fn-hook: ALL %d candidates valid after %d ms",
                kCandCount, (wait_iter + 1) * 500);
            break;
        }
        // Periodic progress log (every 10 s) so we can see how long
        // CrackProof takes vs how many candidates have unpacked.
        if (wait_iter > 0 && (wait_iter % 20) == 0) {
            LOG("fn-hook:   ...still waiting (%d ms): %d/%d valid",
                (wait_iter + 1) * 500, valid_count, kCandCount);
        }
    }

    // Snapshot bytes once we have valid prologues.
    for (int i = 0; i < kCandCount; ++i) {
        BYTE pro[16] = {};
        SIZE_T r = 0;
        ReadProcessMemory(GetCurrentProcess(),
                          (LPCVOID)(uintptr_t)g_addr[i],
                          pro, 16, &r);
        LOG("fn-hook:   DR%d -> %s @%p (rva=%lX) "
            "prologue=%02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            i, kCandidates[i].name,
            (void*)(uintptr_t)g_addr[i],
            (unsigned long)kCandidates[i].rva,
            pro[0], pro[1], pro[2], pro[3], pro[4], pro[5], pro[6], pro[7],
            pro[8], pro[9], pro[10], pro[11], pro[12], pro[13], pro[14], pro[15]);
    }

    g_veh_handle = AddVectoredExceptionHandler(1, fn_hook_veh);
    if (!g_veh_handle) {
        LOG("fn-hook: AddVectoredExceptionHandler FAILED gle=%lu",
            GetLastError());
        return 1;
    }

    // Arm now and re-arm forever to catch new engine threads. CrackProof
    // spawns the engine on a thread that doesn't exist at first arm.
    if (!arm_all_threads()) {
        LOG("fn-hook: arm_all_threads returned false; BPs may not fire");
    }
    g_armed = true;

    // Periodic re-arm. Logs only when the thread count changes so the
    // log doesn't get flooded.
    int prev_armed = -1;
    while (true) {
        Sleep(2000);
        // Re-arming on every tick is cheap (SuspendThread + GetContext +
        // SetContext + ResumeThread per thread, ~12 threads * ~100 us).
        // The win is catching threads spawned between ticks.
        DWORD pid = GetCurrentProcessId();
        DWORD self_tid = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        DWORD tids[64];
        int n_tids = 0;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                if (te.th32ThreadID == self_tid) continue;
                if (n_tids >= 64) break;
                tids[n_tids++] = te.th32ThreadID;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        int armed = 0;
        for (int i = 0; i < n_tids; ++i) {
            if (arm_one_thread(tids[i])) ++armed;
        }
        if (armed != prev_armed) {
            LOG("fn-hook: re-arm: %d / %d threads (was %d)",
                armed, n_tids, prev_armed);
            prev_armed = armed;
        }
    }
}


}  // namespace


extern "C" void start_function_hook() {
    LOG("fn-hook: starting (will arm in 2s)");
    HANDLE h = CreateThread(nullptr, 0, arm_thread_entry, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else LOG("fn-hook: CreateThread FAILED gle=%lu", GetLastError());
}


extern "C" void log_function_hook_summary() {
    LOG("fn-hook summary: armed=%d total_hits=%d",
        g_armed ? 1 : 0, g_total_hits.load());
    for (int i = 0; i < kCandCount; ++i) {
        LOG("fn-hook   DR%d %s hits=%d",
            i, kCandidates[i].name, g_hits[i].load());
    }
    if (g_veh_handle) {
        RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = nullptr;
    }
}
