// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Hook-site verification system tests against unpacked cc_dumped.exe.
//
// Every hook installer in translator.cpp targets a specific RVA in cc.exe and
// expects exact bytes and imm32 values there.  When the local unpacked runtime
// snapshot (analysys/cc_dumped.exe) is present on disk, these tests assert the
// expected bytes match.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef CC_DUMPED_EXE_PATH
#define CC_DUMPED_EXE_PATH ""
#endif

namespace {

constexpr std::size_t kDefaultProbeBytes = 16u;
constexpr std::size_t kHexByteFormatLen = 4u;
constexpr std::size_t kPrologueLength = 6u;
constexpr std::uint32_t kLzssRva = 0x45910u;
constexpr std::uint32_t kTextboxRva = 0x2A7A0u;
constexpr std::uint32_t kTextboxA1Offset = 474u;
constexpr std::uint32_t kTextboxA3Offset = 493u;
constexpr std::uint32_t kNarrationRva = 0x2BCC0u;
constexpr std::uint32_t kNarrationA1Offset = 468u;
constexpr std::uint32_t kNarrationA3Offset = 491u;
constexpr std::uint8_t kMovEaxFromImm32Opcode = 0xA1u;
constexpr std::uint8_t kMovImm32FromEaxOpcode = 0xA3u;
constexpr std::uint32_t kDialogueTextGlobal = 0x06D92B78u;

const std::vector<std::uint8_t>& DumpedExe() {
    static const std::vector<std::uint8_t> bytes = [] {
        std::vector<std::uint8_t> out;
        const char* path = CC_DUMPED_EXE_PATH;
        if (path == nullptr || *path == '\0') {
            return out;
        }
        try {
            std::ifstream f(std::filesystem::u8path(path),
                            std::ios::binary | std::ios::ate);
            if (!f) {
                return out;
            }
            const std::streamsize sz = f.tellg();
            if (sz <= 0) {
                return out;
            }
            f.seekg(0, std::ios::beg);
            out.resize(static_cast<std::size_t>(sz));
            f.read(reinterpret_cast<char*>(out.data()), sz);
            if (!f) {
                out.clear();
            }
        } catch (const std::exception&) {
            out.clear();
        }
        return out;
    }();
    return bytes;
}

const std::uint8_t* AtRva(std::uint32_t rva, std::size_t need = kDefaultProbeBytes) {
    const auto& b = DumpedExe();
    if (b.empty() || rva + need > b.size()) {
        return nullptr;
    }
    return b.data() + rva;
}

std::uint32_t ReadLeU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

::testing::AssertionResult BytesMatch(const char* what,
                                      const std::uint8_t* actual,
                                      const std::uint8_t* expected,
                                      std::size_t len) {
    if (std::memcmp(actual, expected, len) == 0) {
        return ::testing::AssertionSuccess();
    }
    auto fail = ::testing::AssertionFailure() << what << " mismatch.\n  expected: ";
    for (std::size_t i = 0; i < len; ++i) {
        char buf[kHexByteFormatLen];
        std::snprintf(buf, sizeof(buf), "%02X ", expected[i]);
        fail << buf;
    }
    fail << "\n  actual:   ";
    for (std::size_t i = 0; i < len; ++i) {
        char buf[kHexByteFormatLen];
        std::snprintf(buf, sizeof(buf), "%02X ", actual[i]);
        fail << buf;
    }
    return fail;
}

}  // namespace

TEST(HookSites, LzssDecompressorPrologue) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kLzssRva);
    ASSERT_NE(p, nullptr);
    constexpr std::uint8_t kExpected[kPrologueLength] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C};
    EXPECT_TRUE(BytesMatch("LZSS prologue (RVA 0x45910)", p, kExpected, kPrologueLength));
}

TEST(HookSites, TextboxRendererPrologue) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kTextboxRva);
    ASSERT_NE(p, nullptr);
    constexpr std::uint8_t kExpected[kPrologueLength] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24};
    EXPECT_TRUE(BytesMatch("textbox prologue (RVA 0x2A7A0)", p, kExpected, kPrologueLength));
}

TEST(HookSites, TextboxA1ReadAt474) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kTextboxRva + kTextboxA1Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovEaxFromImm32Opcode, p[0]) << "expected MOV EAX,[imm32] (A1) at +474";
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1))
        << "imm32 should reference text-ptr global";
}

TEST(HookSites, TextboxA3WriteAt493) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kTextboxRva + kTextboxA3Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovImm32FromEaxOpcode, p[0]) << "expected MOV [imm32],EAX (A3) at +493";
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1));
}

TEST(HookSites, NarrationRendererPrologue) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kNarrationRva);
    ASSERT_NE(p, nullptr);
    constexpr std::uint8_t kExpected[kPrologueLength] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18};
    EXPECT_TRUE(BytesMatch("narration prologue (RVA 0x2BCC0)", p, kExpected, kPrologueLength));
}

TEST(HookSites, NarrationA1ReadAt468) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kNarrationRva + kNarrationA1Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovEaxFromImm32Opcode, p[0]);
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1));
}

TEST(HookSites, NarrationA3WriteAt491) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kNarrationRva + kNarrationA3Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovImm32FromEaxOpcode, p[0]);
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1));
}
