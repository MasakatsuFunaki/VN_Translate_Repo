// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "text_display_abi.h"

// Frame offsets: 0x2c = 11 stack dwords, 0x34 = 11 + 2 register params.

extern "C" {

PFN_TextDisplayHandler g_textDisplayHandler    = nullptr;
void*                  g_textDisplayTrampoline = nullptr;

// Entry from the patched JMP: capture ECX/EDX, forward all args to the handler.
__declspec(naked) void TextDisplayHookEntry() {
    __asm {
        push ebp
        mov  ebp, esp

        push dword ptr [ebp + 0x30]   // p11
        push dword ptr [ebp + 0x2C]   // p10
        push dword ptr [ebp + 0x28]   // p9
        push dword ptr [ebp + 0x24]   // p8
        push dword ptr [ebp + 0x20]   // p7
        push dword ptr [ebp + 0x1C]   // p6
        push dword ptr [ebp + 0x18]   // p5
        push dword ptr [ebp + 0x14]   // p4
        push dword ptr [ebp + 0x10]   // p3
        push dword ptr [ebp + 0x0C]   // text
        push dword ptr [ebp + 0x08]   // p1
        push edx                      // register parameter 2
        push ecx                      // register parameter 1
        call dword ptr [g_textDisplayHandler]
        add  esp, 0x34                // our own thirteen arguments

        mov  esp, ebp
        pop  ebp
        ret                           // bare: the engine cleans its 0x2c
    }
}

// The inverse.  EAX is left exactly as the trampoline returned it.
__declspec(naked) DWORD __cdecl CallTextDisplayOriginal(
    DWORD, DWORD, void*, char*, int*, unsigned short*, int, char*,
    int*, int, int, int, int) {
    __asm {
        push ebp
        mov  ebp, esp

        push dword ptr [ebp + 0x38]   // p11
        push dword ptr [ebp + 0x34]   // p10
        push dword ptr [ebp + 0x30]   // p9
        push dword ptr [ebp + 0x2C]   // p8
        push dword ptr [ebp + 0x28]   // p7
        push dword ptr [ebp + 0x24]   // p6
        push dword ptr [ebp + 0x20]   // p5
        push dword ptr [ebp + 0x1C]   // p4
        push dword ptr [ebp + 0x18]   // p3
        push dword ptr [ebp + 0x14]   // text
        push dword ptr [ebp + 0x10]   // p1
        mov  ecx, dword ptr [ebp + 0x08]
        mov  edx, dword ptr [ebp + 0x0C]
        call dword ptr [g_textDisplayTrampoline]
        add  esp, 0x2C                // caller cleans, as both call sites do

        mov  esp, ebp
        pop  ebp
        ret
    }
}

}  // extern "C"
