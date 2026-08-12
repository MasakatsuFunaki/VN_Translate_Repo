// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Hook-site verification tests.
//
// Every hook installer in translator.cpp targets a specific RVA in cc.exe and
// expects exact bytes / imm32 values there.  These tests load cc_dumped.exe
// (the unpacked-runtime snapshot from analysys/) and assert the expected
// bytes match — turning silent post-launch failures ("WALKER-PROBE: unexpected
// prologue ... skip", "choice_calls=0") into compile-time confidence.
//
// What this tests:
//   - Each hook site has the expected opcode prologue.
//   - Each A1/A3 instruction has the right opcode byte.
//   - Each absolute imm32 (text-pointer global) matches the expected dumped VA.
//
// What this does NOT test:
//   - Live-process patching (depends on cc.exe ASLR + per-launch unpacker state).
//   - Whether the function actually fires for the use case we want it to.
//
// If cc_dumped.exe is missing the whole suite SKIPs — the dump is ~110MB and
// is not required to ship the DLL.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef CC_DUMPED_EXE_PATH
#  define CC_DUMPED_EXE_PATH ""
#endif

namespace {

// Lazy-load the dump once per test process. cc_dumped.exe was created with
// raw_offset == RVA for every section (verified against the PE section table
// in analysys/), so RVAs from translator.cpp can index the file directly.
const std::vector<uint8_t>& DumpedExe() {
    static std::vector<uint8_t> bytes;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const char* path = CC_DUMPED_EXE_PATH;
        if (path && *path) {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (f) {
                std::streamsize sz = f.tellg();
                f.seekg(0, std::ios::beg);
                bytes.resize(static_cast<size_t>(sz));
                f.read(reinterpret_cast<char*>(bytes.data()), sz);
            }
        }
    }
    return bytes;
}

const uint8_t* AtRva(uint32_t rva, size_t need = 16) {
    const auto& b = DumpedExe();
    if (b.empty() || rva + need > b.size()) return nullptr;
    return b.data() + rva;
}

#define SKIP_IF_NO_DUMP()                                                  \
    do {                                                                   \
        if (DumpedExe().empty()) {                                         \
            GTEST_SKIP() << "cc_dumped.exe not available at "              \
                         << CC_DUMPED_EXE_PATH                             \
                         << "  (set CC_DUMPED_EXE_PATH at CMake config)";  \
        }                                                                  \
    } while (0)

// Helper: compare a fixed-size byte sequence with a friendlier failure msg.
::testing::AssertionResult BytesMatch(const char* what,
                                       const uint8_t* actual,
                                       const uint8_t* expected,
                                       size_t len) {
    if (memcmp(actual, expected, len) == 0) return ::testing::AssertionSuccess();
    auto fail = ::testing::AssertionFailure() << what << " mismatch.\n  expected: ";
    for (size_t i = 0; i < len; i++) {
        char buf[4]; snprintf(buf, sizeof(buf), "%02X ", expected[i]);
        fail << buf;
    }
    fail << "\n  actual:   ";
    for (size_t i = 0; i < len; i++) {
        char buf[4]; snprintf(buf, sizeof(buf), "%02X ", actual[i]);
        fail << buf;
    }
    return fail;
}

// Dumped-EXE absolute VA of the dialogue text-pointer global.
constexpr uint32_t kDialogueTextGlobal = 0x06D92B78;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// LZSS decompressor — Phase-1 in-place patcher entry hook
// ─────────────────────────────────────────────────────────────────────────
TEST(HookSites, LzssDecompressorPrologue) {
    SKIP_IF_NO_DUMP();
    const uint8_t* p = AtRva(0x45910);
    ASSERT_NE(p, nullptr);
    const uint8_t expected[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
    EXPECT_TRUE(BytesMatch("LZSS prologue (RVA 0x45910)", p, expected, 6));
}

// ─────────────────────────────────────────────────────────────────────────
// Textbox dialogue renderer — FUN_0083a7a0
// ─────────────────────────────────────────────────────────────────────────
TEST(HookSites, TextboxRendererPrologue) {
    SKIP_IF_NO_DUMP();
    const uint8_t* p = AtRva(0x2A7A0);
    ASSERT_NE(p, nullptr);
    const uint8_t expected[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 };
    EXPECT_TRUE(BytesMatch("textbox prologue (RVA 0x2A7A0)", p, expected, 6));
}

TEST(HookSites, TextboxA1ReadAt474) {
    SKIP_IF_NO_DUMP();
    const uint8_t* p = AtRva(0x2A7A0 + 474);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(0xA1u, p[0]) << "expected MOV EAX,[imm32] (A1) at +474";
    uint32_t imm = *reinterpret_cast<const uint32_t*>(p + 1);
    EXPECT_EQ(kDialogueTextGlobal, imm) << "imm32 should reference text-ptr global";
}

TEST(HookSites, TextboxA3WriteAt493) {
    SKIP_IF_NO_DUMP();
    const uint8_t* p = AtRva(0x2A7A0 + 493);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(0xA3u, p[0]) << "expected MOV [imm32],EAX (A3) at +493";
    uint32_t imm = *reinterpret_cast<const uint32_t*>(p + 1);
    EXPECT_EQ(kDialogueTextGlobal, imm);
}

// ─────────────────────────────────────────────────────────────────────────
// Narration renderer — FUN_0083bcc0
// ─────────────────────────────────────────────────────────────────────────
TEST(HookSites, NarrationRendererPrologue) {
    SKIP_IF_NO_DUMP();
    const uint8_t* p = AtRva(0x2BCC0);
    ASSERT_NE(p, nullptr);
    const uint8_t expected[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };
    EXPECT_TRUE(BytesMatch("narration prologue (RVA 0x2BCC0)", p, expected, 6));
}

TEST(HookSites, NarrationA1ReadAt468) {
    SKIP_IF_NO_DUMP();
    const uint8_t* p = AtRva(0x2BCC0 + 468);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(0xA1u, p[0]);
    uint32_t imm = *reinterpret_cast<const uint32_t*>(p + 1);
    EXPECT_EQ(kDialogueTextGlobal, imm);
}

TEST(HookSites, NarrationA3WriteAt491) {
    SKIP_IF_NO_DUMP();
    const uint8_t* p = AtRva(0x2BCC0 + 491);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(0xA3u, p[0]);
    uint32_t imm = *reinterpret_cast<const uint32_t*>(p + 1);
    EXPECT_EQ(kDialogueTextGlobal, imm);
}

