// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// export_hook.cpp — Detours-style prologue hook for system DLL exports.
//
// Why we need this for sakuranotoki specifically:
//
//   IAT hooks on sakuranotoki.exe's static IAT get 0 calls from the
//   engine — confirmed empirically (proxy_log shows hits=0 for all
//   five hooked CRT/string APIs across two launches). CrackProof's
//   unpacker resolves engine imports through its own private path
//   (likely direct walk of kernel32's export table after unpacking)
//   and the engine caches the resolved pointers, never going back
//   through the EXE's IAT.
//
//   Since the engine calls the WinAPI through cached pointers that
//   point AT the actual export, the only place we can intercept it
//   is at the export bytes themselves. So: patch the function
//   prologue in kernel32.dll with a JMP rel32 to our hook.
//
//   Anti-tamper safety: CrackProof watches sakuranotoki.exe's .text
//   bytes. kernel32.dll lives in a different mapped image, and its
//   pages are copy-on-write per process — our patches stay in our
//   private copy, invisible to other processes and to CrackProof's
//   tamper checks (which inspect the EXE only).
//
// Implementation:
//
//   1. Determine prologue length: read instructions starting at the
//      target until the cumulative byte count >= 5 (the size of a
//      JMP rel32). Use a minimal x86 length decoder that handles the
//      instructions actually seen in Windows DLL prologues. If the
//      decoder hits an unknown opcode it bails (logs the bytes and
//      returns 0) — we'd rather skip a hook than corrupt the
//      function by splitting an instruction.
//
//   2. Build trampoline: in a VirtualAlloc'd PAGE_EXECUTE_READWRITE
//      buffer, copy the stolen prologue bytes verbatim, then append
//      a JMP rel32 back to (target + prologue_len). The hook code
//      can call the trampoline to invoke the original function.
//
//   3. Patch target: write 0xE9 (JMP rel32) at target[0], with the
//      relative offset to the hook function. NOP-pad any bytes
//      between target+5 and target+prologue_len (they're now
//      orphaned — the JMP at offset 0 jumps over them).

#include "export_hook.h"
#include "log.h"

#include <cstdint>
#include <cstring>


namespace {


// Minimal x86 instruction length decoder for prologue instructions.
// Returns instruction length in bytes, or 0 if the opcode is unknown.
//
// Coverage is limited to instructions actually seen in Windows DLL
// export prologues — push reg, mov ebp/esp, sub esp, mov edi-edi
// hotpatch hint, fs/gs prefix loads, and a handful of ModR/M ops.
// If a function starts with something exotic (e.g. a SSE
// vector op as part of an inline-asm-driven export) the decoder
// returns 0 and the caller skips the hook rather than risk patching
// a partial instruction.
int modrm_size(uint8_t modrm) {
    int extra = 0;
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm  = modrm & 7;
    if (mod == 3) return 0;             // register-direct
    if (rm  == 4) extra += 1;           // SIB byte
    if (mod == 0 && rm == 5) extra += 4;  // [disp32], no base
    else if (mod == 1)        extra += 1;  // [...+disp8]
    else if (mod == 2)        extra += 4;  // [...+disp32]
    return extra;
}

int decode_one(const uint8_t* p) {
    uint8_t op = p[0];

    // Single-byte: push/pop reg, ret, leave, nop, int3.
    if (op >= 0x50 && op <= 0x5F) return 1;
    if (op == 0x90 || op == 0xCC || op == 0xC3 || op == 0xC9) return 1;

    // Two-byte: push imm8, jmp rel8, mov al, imm8.
    if (op == 0x6A || op == 0xEB) return 2;
    if (op == 0xB0 || op == 0xB1 || op == 0xB2 || op == 0xB3 ||
        op == 0xB4 || op == 0xB5 || op == 0xB6 || op == 0xB7) return 2;

    // Five-byte: push imm32, mov reg32 imm32, call rel32, jmp rel32,
    // mov eax,[imm32], mov [imm32],eax.
    if (op == 0x68 || op == 0xE8 || op == 0xE9 ||
        op == 0xA1 || op == 0xA3) return 5;
    if (op >= 0xB8 && op <= 0xBF) return 5;

    // FS / GS prefix → recurse on next byte and add 1.
    if (op == 0x64 || op == 0x65 || op == 0x66 || op == 0x67) {
        int sub = decode_one(p + 1);
        return sub == 0 ? 0 : sub + 1;
    }

    // ModR/M arithmetic / mov / lea forms.
    //   8B / 89 / 8D / 33 / 03 / 0B = MOV / LEA / XOR / ADD / OR  (r, r/m)
    //   31 / 29 / 39 / 21 / 09     = ... (r/m, r)
    if (op == 0x8B || op == 0x89 || op == 0x8D ||
        op == 0x33 || op == 0x03 || op == 0x0B ||
        op == 0x31 || op == 0x29 || op == 0x39 ||
        op == 0x21 || op == 0x09 || op == 0x85) {
        return 2 + modrm_size(p[1]);
    }

    // 83 /n imm8 — sub/add/and/or/cmp imm8 against r/m.
    if (op == 0x83) {
        return 3 + modrm_size(p[1]);
    }

    // 81 /n imm32 — same as 83 but with imm32.
    if (op == 0x81) {
        return 6 + modrm_size(p[1]);
    }

    // C7 /0 imm32 — mov r/m32, imm32
    if (op == 0xC7) {
        return 6 + modrm_size(p[1]);
    }

    // F7 /n — test/not/neg/mul/imul/div/idiv. /0 and /1 take imm32.
    if (op == 0xF7) {
        uint8_t reg = (p[1] >> 3) & 7;
        int base = 2 + modrm_size(p[1]);
        return (reg == 0 || reg == 1) ? base + 4 : base;
    }

    // 0F escape: only handle a few common forms (jcc rel32, movzx,
    // movsx). Conservatively bail on everything else.
    if (op == 0x0F) {
        uint8_t op2 = p[1];
        if (op2 >= 0x80 && op2 <= 0x8F) return 6;            // jcc rel32
        if (op2 == 0xB6 || op2 == 0xB7 ||                     // movzx
            op2 == 0xBE || op2 == 0xBF) {                     // movsx
            return 3 + modrm_size(p[2]);
        }
        return 0;
    }

    return 0;
}


}  // namespace


bool ExportHookInstall(void* target, void* hook, void** trampoline_out) {
    if (!target || !hook || !trampoline_out) return false;

    uint8_t* t = static_cast<uint8_t*>(target);

    // Step 1 — Decode prologue until we have >= 5 bytes covered. 5 is
    // the size of the JMP rel32 we need to write at target[0].
    constexpr int kMinSteal = 5;
    constexpr int kMaxSteal = 24;
    int stolen = 0;
    while (stolen < kMinSteal) {
        int n = decode_one(t + stolen);
        if (n == 0 || stolen + n > kMaxSteal) {
            LOG("export_hook: undecodable prologue at %p (bytes: "
                "%02X %02X %02X %02X %02X %02X %02X %02X)",
                target, t[0], t[1], t[2], t[3],
                t[4], t[5], t[6], t[7]);
            return false;
        }
        stolen += n;
    }

    // Step 2 — Build trampoline: stolen bytes + JMP rel32 back to
    // (target + stolen). The hook code calls into this trampoline
    // when it wants to invoke the original function.
    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        LOG("export_hook: VirtualAlloc trampoline failed");
        return false;
    }
    std::memcpy(trampoline, t, stolen);
    trampoline[stolen] = 0xE9;  // JMP rel32
    int32_t back_rel = static_cast<int32_t>(
        (intptr_t)(t + stolen) - (intptr_t)(trampoline + stolen + 5));
    std::memcpy(trampoline + stolen + 1, &back_rel, 4);

    // CRITICAL: write the trampoline pointer BEFORE patching the
    // target. Otherwise the caller's "real function pointer" still
    // points at the about-to-be-patched target, and any recursive
    // call through that pointer hits the patch and re-enters the
    // hook — stack overflow.
    //
    // Concrete failure: log_msg uses kernel32!MultiByteToWideChar
    // for UTF-8/CP932 detection. If we patch MB2WC, then call LOG
    // ("installed at..."), log_msg calls MB2WC → our patched target
    // jumps to Hooked_MB2WC → Hooked_MB2WC calls g_real_MB2WC which
    // STILL POINTS AT THE PATCHED TARGET → recursion → silent crash.
    // Setting *trampoline_out first means g_real_MB2WC is the
    // trampoline by the time the patch lands, so recursion goes
    // trampoline → stolen prologue → JMP back to function+stolen
    // and the call completes normally.
    *trampoline_out = trampoline;

    // Step 3 — Patch target with JMP rel32 to hook, NOP-pad any
    // leftover bytes between target+5 and target+stolen so they
    // disassemble cleanly (they're orphaned anyway, but a debugger
    // walking the page should see clean instructions).
    DWORD old_prot = 0;
    if (!VirtualProtect(t, stolen, PAGE_EXECUTE_READWRITE, &old_prot)) {
        LOG("export_hook: VirtualProtect(RWX) failed at %p (gle=%lu)",
            target, GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        *trampoline_out = nullptr;
        return false;
    }
    int32_t hook_rel = static_cast<int32_t>(
        (intptr_t)hook - (intptr_t)(t + 5));
    t[0] = 0xE9;
    std::memcpy(t + 1, &hook_rel, 4);
    for (int i = 5; i < stolen; ++i) t[i] = 0x90;  // NOP pad
    DWORD restore = 0;
    VirtualProtect(t, stolen, old_prot, &restore);
    FlushInstructionCache(GetCurrentProcess(), t, stolen);

    LOG("export_hook: installed at %p (stolen=%d, trampoline=%p, hook=%p)",
        target, stolen, trampoline, hook);
    return true;
}
