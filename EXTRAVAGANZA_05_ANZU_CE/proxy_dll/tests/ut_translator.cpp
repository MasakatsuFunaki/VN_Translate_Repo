// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for TSV parsing/unescaping, x86 relative branch encoding, and
// ANZMENU1 first-launch delta auto-patching.
#include "menu_autopatch.h"
#include "patcher_logic.h"
#include "translator_logic.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pl = patcher_logic;
namespace fs = std::filesystem;

namespace {

constexpr UINT kCp932CodePage = 932;
constexpr std::uintptr_t kSampleBranchTarget = 0x00500000;
constexpr std::size_t kMagicBytesLen = 8;
constexpr std::uint32_t kFormatVersionV1 = 1u;
constexpr std::uint32_t kSingleRecordCount = 1u;
constexpr std::uint32_t kTargetGpkEntryIndex = 1u;
constexpr std::size_t kGtbTableWordBytes = 4;
constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr int kChoiceOrigFontSize = 24;
constexpr int kChoiceOrigSpace = 1;
constexpr int kChoiceAnimMaxChars = 12;
constexpr std::uint8_t kFxfXorMask = 0xFFu;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string ToCp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    BOOL used_default = FALSE;
    const int needed = WideCharToMultiByte(
        kCp932CodePage, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, &used_default);
    if (needed <= 0 || used_default) {
        throw std::runtime_error("Wide string cannot be represented in CP932");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        kCp932CodePage, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, &used_default);
    if (written != needed || used_default) {
        throw std::runtime_error("CP932 conversion failed");
    }
    return out;
}

void AppendU16Le(std::vector<std::uint8_t>& buf, std::uint16_t val) {
    buf.push_back(static_cast<std::uint8_t>(val & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift1) & kByteMask));
}

void AppendU32Le(std::vector<std::uint8_t>& buf, std::uint32_t val) {
    buf.push_back(static_cast<std::uint8_t>(val & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift1) & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift2) & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift3) & kByteMask));
}

void WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}

std::vector<std::uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.is_open());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> BuildGtb(const std::vector<std::uint32_t>& offsets) {
    std::vector<std::uint8_t> gtb;
    const auto count = static_cast<std::uint32_t>(offsets.size());
    AppendU32Le(gtb, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        AppendU32Le(gtb, i * static_cast<std::uint32_t>(kGtbTableWordBytes));
    }
    for (std::uint32_t off : offsets) {
        AppendU32Le(gtb, off);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        gtb.push_back(static_cast<std::uint8_t>('e'));
        gtb.push_back(static_cast<std::uint8_t>('0' + i));
        gtb.push_back(0);
        gtb.push_back(0);
    }
    return gtb;
}

}  // namespace

TEST(TranslatorContract, ParseTsvBufferUnescapesAndLoadsLookupEntries) {
    translator_logic::TranslationMap map;
    const std::string tsv =
        ToCp932(L"夢美") + "\\r\\n" + ToCp932(L"「あ」") + "\tYumemi: \"Ah\"\\r\\n\n"
        "second\tSecond\r\n";
    const int count = translator_logic::ParseTsvBuffer(tsv.data(), tsv.size(), map);
    EXPECT_EQ(count, 2);
    const std::string expectedKey = ToCp932(L"夢美\r\n「あ」");
    EXPECT_EQ(map[expectedKey], "Yumemi: \"Ah\"\r\n");
    EXPECT_EQ(map["second"], "Second");
}

TEST(TranslatorContract, EmitRelBranchInstructionsAndVerifySentinelLayout) {
    std::uint8_t callBuf[pl::kRelBranchInstructionSize] = {};
    EXPECT_EQ(pl::EmitRelCall32(callBuf, kSampleBranchTarget),
              callBuf + pl::kRelBranchInstructionSize);
    EXPECT_EQ(callBuf[0], pl::kRelCallOpcode);

    EXPECT_EQ(pl::kPrintMessageSentinelRva - pl::kRelTextSplitterFromSentinel,
              pl::kTextSplitterRva);
    EXPECT_EQ(pl::kPrintMessageSentinelRva - pl::kAltRelTextSplitterFromSentinel,
              pl::kFallbackTextSplitterRva);
    EXPECT_EQ(pl::kAltPrintMessageSentinelRva - pl::kAltRelTextSplitterFromSentinel,
              pl::kAltTextSplitterRva);

    EXPECT_EQ(pl::kTextRenderCoreRva, 0x02D480u);
    EXPECT_EQ(pl::kAltTextRenderCoreRva, 0x03B870u);
    EXPECT_EQ(pl::kFontSetupRva, 0x031790u);
    EXPECT_EQ(pl::kAltFontSetupRva, 0x03E890u);
    const std::uint8_t familyAPrologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x01, 0x00, 0x00};
    EXPECT_TRUE(pl::MatchesTextRenderCorePrologue(familyAPrologue));
}

TEST(TranslatorContract, LookupTranslationAndUiFontAdjustmentAndFxfTitle) {
    translator_logic::TranslationMap map;
    map[ToCp932(L"%#黄エタ")] = "%Use Eternal Mode?";
    std::string out;
    const std::string runtimePrompt = ToCp932(L"#黄エタ");
    EXPECT_TRUE(translator_logic::LookupTranslation(
        map, runtimePrompt.c_str(), true, out));
    EXPECT_EQ(out, ToCp932(L"#黄Use Eternal Mode?"));

    const auto adj = translator_logic::ComputeUiFontAdjustment(
        out.c_str(), true, kChoiceOrigFontSize, kChoiceOrigSpace, kChoiceAnimMaxChars);
    EXPECT_TRUE(adj.shouldAdjust);
    EXPECT_EQ(adj.newFontSize, translator_logic::kStandardChoiceFontSize);
    EXPECT_TRUE(adj.expandMaxChars);

    const std::string plain = "\r\nwindowTitle Welcome to AnzU! CE\r\n";
    std::vector<std::uint8_t> enc(plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        enc[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(plain[i]) ^ kFxfXorMask);
    }
    EXPECT_EQ(translator_logic::ExtractFxfWindowTitle(enc.data(), enc.size()),
              "Welcome to AnzU! CE");
}

TEST(TranslatorComponent, EnsureGameFilesPatchedInDirAppliesAnzMenu1AndIsIdempotent) {
    const ScopedTempDir tmp("anzu_ce_ut_menu_autopatch");
    const fs::path& temp_dir = tmp.path();

    const std::vector<std::uint8_t> orig_fxf = {0x91, 0x8E, 0x93, 0x9A};
    const std::vector<std::uint8_t> patched_fxf = {0xB1, 0xAE, 0xB3, 0xBA, 0xBC};
    const std::vector<std::uint8_t> orig_e0 = {'E', 'N', 'T', 'R', 'Y', '_', '0'};
    const std::vector<std::uint8_t> orig_e1 = {'O', 'L', 'D', '_', 'E', '1'};
    const std::vector<std::uint8_t> patched_e1 = {'N', 'E', 'W', '_', 'P', 'A', 'T', 'C', 'H'};

    std::vector<std::uint8_t> orig_gpk = orig_e0;
    orig_gpk.insert(orig_gpk.end(), orig_e1.begin(), orig_e1.end());
    std::vector<std::uint8_t> expected_gpk = orig_e0;
    expected_gpk.insert(expected_gpk.end(), patched_e1.begin(), patched_e1.end());

    const auto orig_gtb = BuildGtb({0u, static_cast<std::uint32_t>(orig_e0.size())});
    const auto expected_gtb = orig_gtb;

    WriteBytes(temp_dir / "init2" / "mainControl.fxf", orig_fxf);
    WriteBytes(temp_dir / "dwq" / "sys0.gtb", orig_gtb);
    WriteBytes(temp_dir / "dwq" / "sys0.gpk", orig_gpk);

    std::vector<std::uint8_t> patch_bin;
    const char kMagic[kMagicBytesLen + 1] = "ANZMENU1";
    patch_bin.insert(patch_bin.end(), kMagic, kMagic + kMagicBytesLen);
    const std::uint32_t orig_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size());
    const std::uint32_t expected_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(expected_gtb.data(), expected_gtb.size());
    AppendU32Le(patch_bin, kFormatVersionV1);
    AppendU32Le(patch_bin, kSingleRecordCount);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_gpk.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(expected_gpk.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_gtb.size()));
    AppendU32Le(patch_bin, orig_gtb_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(expected_gtb.size()));
    AppendU32Le(patch_bin, expected_gtb_hash);
    AppendU32Le(patch_bin, kSingleRecordCount);

    const std::string rel_path = "init2/mainControl.fxf";
    AppendU16Le(patch_bin, static_cast<std::uint16_t>(rel_path.size()));
    patch_bin.insert(patch_bin.end(), rel_path.begin(), rel_path.end());
    const std::uint32_t orig_fxf_hash =
        menu_autopatch::ComputeMurmurHash2(orig_fxf.data(), orig_fxf.size());
    const std::uint32_t patched_fxf_hash =
        menu_autopatch::ComputeMurmurHash2(patched_fxf.data(), patched_fxf.size());
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_fxf.size()));
    AppendU32Le(patch_bin, orig_fxf_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(patched_fxf.size()));
    AppendU32Le(patch_bin, patched_fxf_hash);
    std::vector<std::uint8_t> enc_fxf = patched_fxf;
    menu_autopatch::XorMaskPayload(enc_fxf.data(), enc_fxf.size(),
                                   orig_fxf.data(), orig_fxf.size(), orig_fxf_hash);
    patch_bin.insert(patch_bin.end(), enc_fxf.begin(), enc_fxf.end());

    std::vector<std::uint8_t> enc_gtb = expected_gtb;
    menu_autopatch::XorMaskPayload(enc_gtb.data(), enc_gtb.size(),
                                   orig_gtb.data(), orig_gtb.size(), orig_gtb_hash);
    patch_bin.insert(patch_bin.end(), enc_gtb.begin(), enc_gtb.end());

    const std::uint32_t orig_e1_hash =
        menu_autopatch::ComputeMurmurHash2(orig_e1.data(), orig_e1.size());
    const std::uint32_t patched_e1_hash =
        menu_autopatch::ComputeMurmurHash2(patched_e1.data(), patched_e1.size());
    AppendU32Le(patch_bin, kTargetGpkEntryIndex);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_e0.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_e1.size()));
    AppendU32Le(patch_bin, orig_e1_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(patched_e1.size()));
    AppendU32Le(patch_bin, patched_e1_hash);
    std::vector<std::uint8_t> enc_e1 = patched_e1;
    menu_autopatch::XorMaskPayload(enc_e1.data(), enc_e1.size(),
                                   orig_e1.data(), orig_e1.size(), orig_e1_hash);
    patch_bin.insert(patch_bin.end(), enc_e1.begin(), enc_e1.end());

    WriteBytes(temp_dir / "menu_patch.bin", patch_bin);

    EXPECT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(temp_dir));
    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf"), patched_fxf);
    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf.bak"), orig_fxf);
    EXPECT_EQ(ReadBytes(temp_dir / "dwq" / "sys0.gpk"), expected_gpk);
    EXPECT_EQ(ReadBytes(temp_dir / "dwq" / "sys0.gpk.bak"), orig_gpk);

    EXPECT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(temp_dir));
    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf.bak"), orig_fxf);
}
