// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// text_display_abi.h -- naked thunks for the BGI renderer's custom calling
// convention (ECX+EDX register params, 11 stack dwords, caller-cleans).
// TextDisplayHookEntry captures registers for the hook body;
// CallTextDisplayOriginal restores them for the trampoline.

#include <windows.h>

#include <cstddef>

// The eleven stack dwords, and the byte count both call sites clean.
const size_t TEXT_DISPLAY_STACK_DWORDS = 11;
const size_t TEXT_DISPLAY_STACK_BYTES  = TEXT_DISPLAY_STACK_DWORDS * sizeof(DWORD);

extern "C" {

// Hook handler signature: register params as leading args, 11 stack dwords.
typedef DWORD (__cdecl *PFN_TextDisplayHandler)(
    DWORD          ecx,
    DWORD          edx,
    void*          p1,    // [ebp+0x08]
    char*          text,  // [ebp+0x0c]  CP932 text buffer
    int*           p3,    // [ebp+0x10]
    unsigned short*p4,    // [ebp+0x14]
    int            p5,    // [ebp+0x18]
    char*          p6,    // [ebp+0x1c]
    int*           p7,    // [ebp+0x20]
    int            p8,    // [ebp+0x24]
    int            p9,    // [ebp+0x28]
    int            p10,   // [ebp+0x2c]
    int            p11);  // [ebp+0x30]

// Both must be set before the renderer's entry is patched.
extern PFN_TextDisplayHandler g_textDisplayHandler;
extern void*                  g_textDisplayTrampoline;

// Jump target for the patched function entry.  Not callable from C++: it
// expects the engine's frame, and it returns without cleaning it.
void TextDisplayHookEntry();

// Calls the trampoline under the engine's own convention and returns its EAX.
DWORD __cdecl CallTextDisplayOriginal(
    DWORD          ecx,
    DWORD          edx,
    void*          p1,
    char*          text,
    int*           p3,
    unsigned short*p4,
    int            p5,
    char*          p6,
    int*           p7,
    int            p8,
    int            p9,
    int            p10,
    int            p11);

}  // extern "C"
