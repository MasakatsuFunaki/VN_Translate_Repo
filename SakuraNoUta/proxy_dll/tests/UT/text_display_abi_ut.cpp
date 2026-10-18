// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — Call-contract tests for the dialogue renderer thunks: ECX/EDX register
// parameters and eleven stack dwords must round-trip exactly.

#include "text_display_abi.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

const DWORD SENTINEL_ECX = 0xC0DEC0DE;
const DWORD SENTINEL_EDX = 0xEDEDEDED;
const DWORD STUB_RESULT  = 0x5A5A1234;

// The eleven stack dwords, distinguishable from each other and from any
// plausible register value.
const DWORD ARGS[TEXT_DISPLAY_STACK_DWORDS] = {
    0xA0000001, 0xA0000002, 0xA0000003, 0xA0000004, 0xA0000005, 0xA0000006,
    0xA0000007, 0xA0000008, 0xA0000009, 0xA000000A, 0xA000000B
};

struct Capture {
    DWORD ecx;
    DWORD edx;
    DWORD args[TEXT_DISPLAY_STACK_DWORDS];
    int   calls;
};

Capture g_capture;

void ResetCapture() { std::memset(&g_capture, 0, sizeof(g_capture)); }

// ---- entry side ----------------------------------------------------
// Stands in for the hook body the entry thunk forwards to.
DWORD __cdecl CaptureHandler(DWORD ecx, DWORD edx,
                             void* p1, char* text, int* p3, unsigned short* p4,
                             int p5, char* p6, int* p7, int p8, int p9,
                             int p10, int p11) {
    g_capture.ecx = ecx;
    g_capture.edx = edx;
    g_capture.args[0]  = reinterpret_cast<DWORD>(p1);
    g_capture.args[1]  = reinterpret_cast<DWORD>(text);
    g_capture.args[2]  = reinterpret_cast<DWORD>(p3);
    g_capture.args[3]  = reinterpret_cast<DWORD>(p4);
    g_capture.args[4]  = static_cast<DWORD>(p5);
    g_capture.args[5]  = reinterpret_cast<DWORD>(p6);
    g_capture.args[6]  = reinterpret_cast<DWORD>(p7);
    g_capture.args[7]  = static_cast<DWORD>(p8);
    g_capture.args[8]  = static_cast<DWORD>(p9);
    g_capture.args[9]  = static_cast<DWORD>(p10);
    g_capture.args[10] = static_cast<DWORD>(p11);
    ++g_capture.calls;
    return STUB_RESULT;
}

// Calls the entry thunk the way the engine does: eleven dwords pushed, the
// register parameters loaded, and the caller cleaning 0x2c afterwards.
// Reports the net stack movement so a stray `ret n` cannot hide.
DWORD InvokeLikeTheEngine(DWORD ecx_in, DWORD edx_in, const DWORD* a,
                          int* esp_delta) {
    DWORD result = 0;
    DWORD esp_before = 0, esp_after = 0;
    __asm {
        push esi
        mov  esi, a
        mov  esp_before, esp
        push dword ptr [esi + 40]
        push dword ptr [esi + 36]
        push dword ptr [esi + 32]
        push dword ptr [esi + 28]
        push dword ptr [esi + 24]
        push dword ptr [esi + 20]
        push dword ptr [esi + 16]
        push dword ptr [esi + 12]
        push dword ptr [esi + 8]
        push dword ptr [esi + 4]
        push dword ptr [esi]
        mov  ecx, ecx_in
        mov  edx, edx_in
        call TextDisplayHookEntry
        add  esp, 0x2C
        mov  esp_after, esp
        mov  result, eax
        pop  esi
    }
    *esp_delta = static_cast<int>(esp_after) - static_cast<int>(esp_before);
    return result;
}

// ---- trampoline side -----------------------------------------------
// Records what the engine would have received, and hands back a sentinel in
// EAX for the thunk to propagate.
extern "C" DWORD __cdecl RecordTrampolineCall(DWORD ecx, DWORD edx,
                                              const DWORD* args) {
    g_capture.ecx = ecx;
    g_capture.edx = edx;
    std::memcpy(g_capture.args, args, sizeof(g_capture.args));
    ++g_capture.calls;
    return STUB_RESULT;
}

// Stands in for the trampoline. Reads its arguments where the real renderer
// reads them ([ebp+0x08] onwards) and returns bare, as the renderer does.
__declspec(naked) void TrampolineStub() {
    __asm {
        push ebp
        mov  ebp, esp
        lea  eax, [ebp + 8]
        push eax
        push edx
        push ecx
        call RecordTrampolineCall
        add  esp, 12
        mov  esp, ebp
        pop  ebp
        ret
    }
}

class TextDisplayAbi : public ::testing::Test {
protected:
    void SetUp() override {
        ResetCapture();
        g_textDisplayHandler    = &CaptureHandler;
        g_textDisplayTrampoline = reinterpret_cast<void*>(&TrampolineStub);
    }
    void TearDown() override {
        g_textDisplayHandler    = nullptr;
        g_textDisplayTrampoline = nullptr;
    }
};

}  // namespace

TEST(TextDisplayContract, ElevenStackDwordsIsTheCleanupBothCallSitesPerform) {
    EXPECT_EQ(TEXT_DISPLAY_STACK_DWORDS, 11u);
    EXPECT_EQ(TEXT_DISPLAY_STACK_BYTES, 0x2Cu);
}

TEST_F(TextDisplayAbi, EntryDeliversTheRegisterParametersToTheHandler) {
    int delta = 0;
    DWORD r = InvokeLikeTheEngine(SENTINEL_ECX, SENTINEL_EDX, ARGS, &delta);

    ASSERT_EQ(g_capture.calls, 1);
    EXPECT_EQ(g_capture.ecx, SENTINEL_ECX);
    EXPECT_EQ(g_capture.edx, SENTINEL_EDX);
    EXPECT_EQ(r, STUB_RESULT);
}

TEST_F(TextDisplayAbi, EntryForwardsTheElevenStackDwordsInOrder) {
    int delta = 0;
    InvokeLikeTheEngine(SENTINEL_ECX, SENTINEL_EDX, ARGS, &delta);

    ASSERT_EQ(g_capture.calls, 1);
    for (size_t i = 0; i < TEXT_DISPLAY_STACK_DWORDS; ++i) {
        EXPECT_EQ(g_capture.args[i], ARGS[i]) << "stack dword " << (i + 1);
    }
}

// A `ret n` in the entry thunk would pop the engine's arguments a second time.
TEST_F(TextDisplayAbi, EntryLeavesTheCallerToCleanItsOwnArguments) {
    int delta = 0;
    InvokeLikeTheEngine(SENTINEL_ECX, SENTINEL_EDX, ARGS, &delta);
    EXPECT_EQ(delta, 0);
}

TEST_F(TextDisplayAbi, TrampolineCallRestoresBothRegisters) {
    DWORD r = CallTextDisplayOriginal(
        SENTINEL_ECX, SENTINEL_EDX,
        reinterpret_cast<void*>(ARGS[0]), reinterpret_cast<char*>(ARGS[1]),
        reinterpret_cast<int*>(ARGS[2]), reinterpret_cast<unsigned short*>(ARGS[3]),
        static_cast<int>(ARGS[4]), reinterpret_cast<char*>(ARGS[5]),
        reinterpret_cast<int*>(ARGS[6]), static_cast<int>(ARGS[7]),
        static_cast<int>(ARGS[8]), static_cast<int>(ARGS[9]),
        static_cast<int>(ARGS[10]));

    ASSERT_EQ(g_capture.calls, 1);
    EXPECT_EQ(g_capture.ecx, SENTINEL_ECX);
    EXPECT_EQ(g_capture.edx, SENTINEL_EDX);
    EXPECT_EQ(r, STUB_RESULT);
}

TEST_F(TextDisplayAbi, TrampolineCallPushesTheElevenStackDwordsInOrder) {
    CallTextDisplayOriginal(
        SENTINEL_ECX, SENTINEL_EDX,
        reinterpret_cast<void*>(ARGS[0]), reinterpret_cast<char*>(ARGS[1]),
        reinterpret_cast<int*>(ARGS[2]), reinterpret_cast<unsigned short*>(ARGS[3]),
        static_cast<int>(ARGS[4]), reinterpret_cast<char*>(ARGS[5]),
        reinterpret_cast<int*>(ARGS[6]), static_cast<int>(ARGS[7]),
        static_cast<int>(ARGS[8]), static_cast<int>(ARGS[9]),
        static_cast<int>(ARGS[10]));

    ASSERT_EQ(g_capture.calls, 1);
    for (size_t i = 0; i < TEXT_DISPLAY_STACK_DWORDS; ++i) {
        EXPECT_EQ(g_capture.args[i], ARGS[i]) << "stack dword " << (i + 1);
    }
}

// The trampoline returns bare, so the thunk owes the 0x2c itself.
TEST_F(TextDisplayAbi, TrampolineCallCleansTheStackItPushed) {
    DWORD esp_before = 0, esp_after = 0;
    __asm { mov esp_before, esp }
    CallTextDisplayOriginal(
        SENTINEL_ECX, SENTINEL_EDX,
        reinterpret_cast<void*>(ARGS[0]), reinterpret_cast<char*>(ARGS[1]),
        reinterpret_cast<int*>(ARGS[2]), reinterpret_cast<unsigned short*>(ARGS[3]),
        static_cast<int>(ARGS[4]), reinterpret_cast<char*>(ARGS[5]),
        reinterpret_cast<int*>(ARGS[6]), static_cast<int>(ARGS[7]),
        static_cast<int>(ARGS[8]), static_cast<int>(ARGS[9]),
        static_cast<int>(ARGS[10]));
    __asm { mov esp_after, esp }

    ASSERT_EQ(g_capture.calls, 1);
    EXPECT_EQ(esp_before, esp_after);
}

// Both halves back to back: what the engine put in ECX/EDX is what the
// trampoline receives, with the hook body in between.
TEST_F(TextDisplayAbi, RegisterParametersSurviveEntryAndTrampolineTogether) {
    struct Relay {
        static DWORD __cdecl Handler(DWORD ecx, DWORD edx, void* p1, char* text,
                                     int* p3, unsigned short* p4, int p5,
                                     char* p6, int* p7, int p8, int p9,
                                     int p10, int p11) {
            return CallTextDisplayOriginal(ecx, edx, p1, text, p3, p4, p5, p6,
                                           p7, p8, p9, p10, p11);
        }
    };
    g_textDisplayHandler = &Relay::Handler;

    int delta = 0;
    DWORD r = InvokeLikeTheEngine(SENTINEL_ECX, SENTINEL_EDX, ARGS, &delta);

    ASSERT_EQ(g_capture.calls, 1);
    EXPECT_EQ(g_capture.ecx, SENTINEL_ECX);
    EXPECT_EQ(g_capture.edx, SENTINEL_EDX);
    for (size_t i = 0; i < TEXT_DISPLAY_STACK_DWORDS; ++i) {
        EXPECT_EQ(g_capture.args[i], ARGS[i]) << "stack dword " << (i + 1);
    }
    EXPECT_EQ(r, STUB_RESULT);
    EXPECT_EQ(delta, 0);
}
