// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests verifying repository-resident artifacts for
// EXTRA_VA_MIZUNA_CE: MIZMENU1 menu_patch.bin format invariants and
// script_output/translated_text.json speaker coverage.
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "menu/menu_patcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kMaxWalkLevels = 8;
constexpr int kMemcmpEqual = 0;
constexpr std::size_t kBufferGrowthFactor = 2;
constexpr std::size_t kExpectedDistinctDialogueSpeakers = 29;
constexpr std::size_t kExpectedDialogueLineCount = 971;

fs::path CurrentTestExecutableDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path();
}

fs::path FindGameCheckoutRootFrom(fs::path start) {
    std::error_code ec;
    const fs::path menu_patch_name = fs::u8path("menu_patch.bin");
    const fs::path build_py_name = fs::u8path("build.py");
    const fs::path game_dir_name = fs::u8path("EXTRAVAGANZA_08_MIZUNA_CE");
    for (int i = 0; i < kMaxWalkLevels && !start.empty(); ++i) {
        if (fs::is_regular_file(start / menu_patch_name, ec) &&
            fs::is_regular_file(start / build_py_name, ec)) {
            return start;
        }
        const fs::path nested = start / game_dir_name;
        if (fs::is_regular_file(nested / menu_patch_name, ec)) {
            return nested;
        }
        if (!start.has_parent_path() || start.parent_path() == start) {
            break;
        }
        start = start.parent_path();
    }
    return {};
}

fs::path FindGameCheckoutRoot() {
    const fs::path from_exe = FindGameCheckoutRootFrom(CurrentTestExecutableDir());
    if (!from_exe.empty()) {
        return from_exe;
    }
    return FindGameCheckoutRootFrom(fs::current_path());
}

bool ContainsAsciiSubsequence(const std::vector<std::uint8_t>& haystack,
                              const std::uint8_t* needle,
                              std::size_t needle_len) {
    const auto it =
        std::search(haystack.begin(), haystack.end(), needle, needle + needle_len);
    return it != haystack.end();
}

TEST(MenuPatchBinContract, CommittedMenuPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path game_root = FindGameCheckoutRoot();
    ASSERT_FALSE(game_root.empty()) << "EXTRAVAGANZA_08_MIZUNA_CE checkout root not found.";
    const fs::path patch_bin_path = game_root / fs::u8path("menu_patch.bin");
    ASSERT_TRUE(fs::exists(patch_bin_path));

    const auto bytes = exm::read_file(patch_bin_path.u8string());
    ASSERT_GT(bytes.size(), exm::menu::kPatchMagicSize);
    EXPECT_EQ(
        std::memcmp(bytes.data(), exm::menu::kPatchMagic, exm::menu::kPatchMagicSize),
        kMemcmpEqual);

    const auto parsed = exm::menu::parse_patch_bin(bytes);
    EXPECT_EQ(parsed.version, exm::menu::kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_records.size(), exm::menu::kExpectedStandaloneFileCount);
    EXPECT_GT(parsed.gpk_records.size(), 0u);

    constexpr std::uint8_t kPackBmpNeedle[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    constexpr std::uint8_t kPngNeedle[] = {0x89u, 'P', 'N', 'G'};
    EXPECT_FALSE(ContainsAsciiSubsequence(bytes, kPackBmpNeedle, sizeof(kPackBmpNeedle)));
    EXPECT_FALSE(ContainsAsciiSubsequence(bytes, kPngNeedle, sizeof(kPngNeedle)));
}

TEST(CorpusSpeakerContract, ExtractedCorpusSpeakerCoverageAndGatePass) {
    const fs::path game_root = FindGameCheckoutRoot();
    ASSERT_FALSE(game_root.empty()) << "EXTRAVAGANZA_08_MIZUNA_CE checkout root not found.";
    const fs::path corpus_path =
        game_root / fs::u8path("script_output") / fs::u8path("translated_text.json");
    ASSERT_TRUE(fs::exists(corpus_path))
        << "script_output/translated_text.json not found at " << corpus_path.u8string();

    const auto& map = exm::translate::character_names();
    const auto root_val = exm::json_parse_file(corpus_path.u8string());
    ASSERT_TRUE(root_val.is_object());
    const auto& root = root_val.get_object();

    std::size_t dialogue_count = 0;
    std::set<std::string> dialogue_speakers;
    std::vector<std::string> unmapped_speakers;

    for (const auto& fkv : root) {
        ASSERT_TRUE(fkv.value().is_object());
        const auto& fobj = fkv.value().get_object();
        const auto* strings = fobj.if_contains("strings");
        ASSERT_NE(strings, nullptr);
        ASSERT_TRUE(strings->is_array());
        for (const auto& sv : strings->get_array()) {
            ASSERT_TRUE(sv.is_object());
            const auto& s = sv.get_object();
            if (std::string(s.at("type").get_string()) != "dialogue") {
                continue;
            }
            ++dialogue_count;
            const std::string text(s.at("text").get_string());
            const std::size_t crlf = text.find("\r\n");
            ASSERT_NE(crlf, std::string::npos) << "Dialogue entry missing CRLF: " << text;
            const std::string jp_speaker = exm::trim(text.substr(0, crlf));
            ASSERT_FALSE(jp_speaker.empty());
            dialogue_speakers.insert(jp_speaker);
            if (map.find(jp_speaker) == map.end()) {
                unmapped_speakers.push_back(jp_speaker);
            }
            const std::string resolved = exm::translate::extract_speaker(text);
            EXPECT_NE(resolved, "NARRATION") << "Failed to extract speaker from: " << text;
            EXPECT_EQ(resolved, map.at(jp_speaker));
        }
    }

    EXPECT_EQ(dialogue_count, kExpectedDialogueLineCount);
    ASSERT_EQ(dialogue_speakers.size(), kExpectedDistinctDialogueSpeakers);
    EXPECT_TRUE(unmapped_speakers.empty());

    const auto report = exm::translate::run_speaker_checks(root);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.dialogue_total, kExpectedDialogueLineCount);
    EXPECT_EQ(report.speakers_in_data, kExpectedDistinctDialogueSpeakers);
}

class ScopedCtSandbox {
public:
    explicit ScopedCtSandbox(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        path_ = fs::temp_directory_path() /
                fs::u8path(std::string(prefix) + "_" +
                           std::to_string(GetCurrentProcessId()) + "_" +
                           std::to_string(id));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedCtSandbox() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedCtSandbox(const ScopedCtSandbox&) = delete;
    ScopedCtSandbox& operator=(const ScopedCtSandbox&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::vector<std::uint8_t> MakePngDwqEntry(const exm::menu::RgbaImage& img) {
    constexpr std::size_t kDwqHeaderSize = 64u;
    constexpr std::size_t kDwqTypeFieldLen = 16u;
    constexpr std::size_t kDwqDataSizeOffset = 0x20u;
    constexpr std::size_t kDwqWidthOffset = 0x24u;
    constexpr std::size_t kDwqHeightOffset = 0x28u;
    constexpr std::size_t kDwqBppOffset = 0x2Au;
    constexpr std::size_t kDwqPackStrOffset = 0x2Cu;
    constexpr std::size_t kDwqPackStrLen = 20u;
    constexpr std::uint32_t kByteMask = 0xFFu;
    constexpr unsigned int kShift8 = 8u;
    constexpr unsigned int kShift16 = 16u;
    constexpr unsigned int kShift24 = 24u;
    constexpr std::size_t kOff0 = 0u;
    constexpr std::size_t kOff1 = 1u;
    constexpr std::size_t kOff2 = 2u;
    constexpr std::size_t kOff3 = 3u;

    const auto png = exm::menu::encode_rgba_to_png_bytes(img, true);
    std::vector<std::uint8_t> dwq(kDwqHeaderSize, 0u);
    std::memcpy(dwq.data(), "PNG             ", kDwqTypeFieldLen);
    const std::uint32_t sz = static_cast<std::uint32_t>(png.size());
    dwq[kDwqDataSizeOffset + kOff0] = static_cast<std::uint8_t>(sz & kByteMask);
    dwq[kDwqDataSizeOffset + kOff1] = static_cast<std::uint8_t>((sz >> kShift8) & kByteMask);
    dwq[kDwqDataSizeOffset + kOff2] = static_cast<std::uint8_t>((sz >> kShift16) & kByteMask);
    dwq[kDwqDataSizeOffset + kOff3] = static_cast<std::uint8_t>((sz >> kShift24) & kByteMask);
    dwq[kDwqWidthOffset + kOff0] = static_cast<std::uint8_t>(img.width & kByteMask);
    dwq[kDwqWidthOffset + kOff1] = static_cast<std::uint8_t>((img.width >> kShift8) & kByteMask);
    dwq[kDwqHeightOffset + kOff0] = static_cast<std::uint8_t>(img.height & kByteMask);
    dwq[kDwqHeightOffset + kOff1] = static_cast<std::uint8_t>((img.height >> kShift8) & kByteMask);
    dwq[kDwqBppOffset + kOff0] = static_cast<std::uint8_t>(img.bpp & kByteMask);
    dwq[kDwqBppOffset + kOff1] = static_cast<std::uint8_t>((img.bpp >> kShift8) & kByteMask);
    std::memcpy(dwq.data() + kDwqPackStrOffset, "    PACKTYPE=8A     ", kDwqPackStrLen);
    dwq.insert(dwq.end(), png.begin(), png.end());
    return dwq;
}

exm::menu::RgbaImage MakeSolidRgba(std::uint16_t w, std::uint16_t h,
                                   std::uint8_t r, std::uint8_t g,
                                   std::uint8_t b, std::uint8_t a) {
    constexpr std::uint16_t kBpp32 = 32u;
    constexpr std::size_t kRgbaChannels = 4u;
    constexpr std::size_t kChanR = 0u;
    constexpr std::size_t kChanG = 1u;
    constexpr std::size_t kChanB = 2u;
    constexpr std::size_t kChanA = 3u;

    exm::menu::RgbaImage img;
    img.width = w;
    img.height = h;
    img.bpp = kBpp32;
    img.has_alpha = true;
    img.pixels.resize(static_cast<std::size_t>(w) * h * kRgbaChannels);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        img.pixels[i * kRgbaChannels + kChanR] = r;
        img.pixels[i * kRgbaChannels + kChanG] = g;
        img.pixels[i * kRgbaChannels + kChanB] = b;
        img.pixels[i * kRgbaChannels + kChanA] = a;
    }
    return img;
}

void WriteTwoEntryArchive(const fs::path& gpk_path, const fs::path& gtb_path,
                          const std::vector<std::uint8_t>& dwq0,
                          const std::vector<std::uint8_t>& dwq1) {
    constexpr std::uint32_t kEntryCount = 2u;
    constexpr std::size_t kGtbCountOffset = 0u;
    constexpr std::size_t kGtbNameOff0Pos = 4u;
    constexpr std::size_t kGtbNameOff1Pos = 8u;
    constexpr std::size_t kGtbDataOff0Pos = 12u;
    constexpr std::size_t kGtbDataOff1Pos = 16u;
    constexpr std::size_t kGtbNamesBlobPos = 20u;
    constexpr std::size_t kGtbSecondNamePos = 32u;
    constexpr std::uint32_t kName0RelOffset = 0u;
    constexpr std::uint32_t kName1RelOffset = 12u;
    constexpr std::size_t kSpriteNameLen = 10u;
    constexpr std::size_t kGtbTotalBytes = 44u;

    auto write_u32 = [](std::uint8_t* p, std::uint32_t v) {
        constexpr std::uint32_t kByteMask = 0xFFu;
        constexpr unsigned int kShift8 = 8u;
        constexpr unsigned int kShift16 = 16u;
        constexpr unsigned int kShift24 = 24u;
        constexpr std::size_t kOff0 = 0u;
        constexpr std::size_t kOff1 = 1u;
        constexpr std::size_t kOff2 = 2u;
        constexpr std::size_t kOff3 = 3u;
        p[kOff0] = static_cast<std::uint8_t>(v & kByteMask);
        p[kOff1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
        p[kOff2] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
        p[kOff3] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
    };
    std::vector<std::uint8_t> gpk;
    gpk.insert(gpk.end(), dwq0.begin(), dwq0.end());
    gpk.insert(gpk.end(), dwq1.begin(), dwq1.end());
    exm::write_file(gpk_path.u8string(), gpk);

    std::vector<std::uint8_t> gtb(kGtbTotalBytes, 0u);
    write_u32(gtb.data() + kGtbCountOffset, kEntryCount);
    write_u32(gtb.data() + kGtbNameOff0Pos, kName0RelOffset);
    write_u32(gtb.data() + kGtbNameOff1Pos, kName1RelOffset);
    write_u32(gtb.data() + kGtbDataOff0Pos, 0u);
    write_u32(gtb.data() + kGtbDataOff1Pos, static_cast<std::uint32_t>(dwq0.size()));
    std::memcpy(gtb.data() + kGtbNamesBlobPos, "sprite_one", kSpriteNameLen);
    std::memcpy(gtb.data() + kGtbSecondNamePos, "sprite_two", kSpriteNameLen);
    exm::write_file(gtb_path.u8string(), gtb);
}

TEST(MenuPatchBinContract, PngDirModePrefersDotBakOverAlreadyPatchedLiveAndSupportsShrinkingReplay) {
    constexpr std::uint16_t kSpriteDim = 4u;
    const ScopedCtSandbox sandbox("mizuna_ct_pngdir_bak");
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path rendered_dir = sandbox.path() / "rendered";
    fs::create_directories(game_dir / "dwq");
    fs::create_directories(game_dir / "init2");
    fs::create_directories(game_dir / "spt");
    fs::create_directories(rendered_dir);

    const auto orig_s0 = MakeSolidRgba(kSpriteDim, kSpriteDim, 0x10u, 0x20u, 0x30u, 0xFFu);
    const auto orig_s1 = MakeSolidRgba(kSpriteDim, kSpriteDim, 0x40u, 0x50u, 0x60u, 0x80u);
    const auto stale_live_s0 = MakeSolidRgba(kSpriteDim, kSpriteDim, 0xEEu, 0x00u, 0x00u, 0xFFu);
    const auto stale_live_s1 = MakeSolidRgba(kSpriteDim, kSpriteDim, 0xDDu, 0x00u, 0x00u, 0xFFu);
    const auto replacement_s0 = MakeSolidRgba(kSpriteDim, kSpriteDim, 0x00u, 0x7Fu, 0x80u, 0xFFu);

    WriteTwoEntryArchive(game_dir / "dwq" / "sys0.gpk.bak", game_dir / "dwq" / "sys0.gtb.bak",
                         MakePngDwqEntry(orig_s0), MakePngDwqEntry(orig_s1));
    WriteTwoEntryArchive(game_dir / "dwq" / "sys0.gpk", game_dir / "dwq" / "sys0.gtb",
                         MakePngDwqEntry(stale_live_s0), MakePngDwqEntry(stale_live_s1));

    const std::string pristine_main = exm::utf8_to_cp932(
        "name mainControl\r\npristineMarker keep_main\r\nwindowTitle 『EXTRA VA MIZUNA』\r\n");
    const std::string stale_main = exm::utf8_to_cp932(
        "name mainControl\r\nstaleMarker bad_main\r\nwindowTitle STALE\r\n");
    const std::string pristine_backlog = exm::utf8_to_cp932(
        "modename backlog\r\npristineMarker keep_log\r\nfirstMessage1 \t#苺　ログ\r\n");
    const std::string stale_backlog = exm::utf8_to_cp932(
        "modename backlog\r\nstaleMarker bad_log\r\nfirstMessage1 STALE\r\n");
    exm::write_file((game_dir / "init2" / "mainControl.fxf.bak").u8string(),
                    exm::menu::encrypt_fxf(pristine_main));
    exm::write_file((game_dir / "init2" / "mainControl.fxf").u8string(),
                    exm::menu::encrypt_fxf(stale_main));
    exm::write_file((game_dir / "init2" / "backlog.fxf.bak").u8string(),
                    exm::menu::encrypt_fxf(pristine_backlog));
    exm::write_file((game_dir / "init2" / "backlog.fxf").u8string(),
                    exm::menu::encrypt_fxf(stale_backlog));

    const std::string pristine_xtx = "0,名前なし,nobody\r\n1,水菜,mizuna\r\n9,pristine_row,ok\r\n";
    const std::string stale_xtx = "0,,nobody\r\n1,Ｓｔａｌｅ,mizuna\r\n";
    exm::write_file((game_dir / "spt" / "charaname.xtx.bak").u8string(),
                    exm::utf8_to_cp932_replace(pristine_xtx));
    exm::write_file((game_dir / "spt" / "charaname.xtx").u8string(),
                    exm::utf8_to_cp932_replace(stale_xtx));

    const fs::path png0 = rendered_dir / "000_sprite_one.png";
    exm::write_file(png0.u8string(), exm::menu::encode_rgba_to_png_bytes(replacement_s0, true));

    exm::menu::PatchMenusOptions opt;
    opt.game_dir = game_dir;
    opt.rendered_dir = rendered_dir;

    ASSERT_EQ(exm::menu::run_patch_menus(opt), 0);

    EXPECT_EQ(exm::read_file((game_dir / "dwq_backup" / "sys0.gpk").u8string()),
              exm::read_file((game_dir / "dwq" / "sys0.gpk.bak").u8string()));
    EXPECT_EQ(exm::read_file((game_dir / "dwq_backup" / "sys0.gtb").u8string()),
              exm::read_file((game_dir / "dwq" / "sys0.gtb.bak").u8string()));
    EXPECT_EQ(exm::read_file((game_dir / "init2_backup" / "mainControl.fxf").u8string()),
              exm::read_file((game_dir / "init2" / "mainControl.fxf.bak").u8string()));
    EXPECT_EQ(exm::read_file((game_dir / "init2_backup" / "backlog.fxf").u8string()),
              exm::read_file((game_dir / "init2" / "backlog.fxf.bak").u8string()));
    EXPECT_EQ(exm::read_file((game_dir / "xtx_backup" / "charaname.xtx").u8string()),
              exm::read_file((game_dir / "spt" / "charaname.xtx.bak").u8string()));

    exm::menu::MenuArchivePatcher live_after_p1;
    live_after_p1.load(game_dir / "dwq" / "sys0.gpk", game_dir / "dwq" / "sys0.gtb");
    ASSERT_EQ(live_after_p1.entry_count(), 2u);
    EXPECT_EQ(live_after_p1.decode_entry_rgba(0).pixels, replacement_s0.pixels);
    EXPECT_EQ(live_after_p1.decode_entry_rgba(1).pixels, orig_s1.pixels);

    const std::string expected_main = exm::utf8_to_cp932(
        "name mainControl\r\npristineMarker keep_main\r\n"
        "windowTitle EXTRA VA MIZUNA ~EXTRAVAGANZA~ CE\r\n");
    const std::string expected_backlog = exm::utf8_to_cp932(
        "modename backlog\r\npristineMarker keep_log\r\n"
        "firstMessage1 \t#苺　　　　　　　　Ｂａｃｋｌｏｇ\r\n");
    EXPECT_EQ(exm::menu::decrypt_fxf(exm::read_file((game_dir / "init2" / "mainControl.fxf").u8string())),
              expected_main);
    EXPECT_EQ(exm::menu::decrypt_fxf(exm::read_file((game_dir / "init2" / "backlog.fxf").u8string())),
              expected_backlog);
    EXPECT_EQ(exm::cp932_to_utf8_strict(exm::read_file((game_dir / "spt" / "charaname.xtx").u8string())),
              "0,,nobody\r\n1,Ｍｉｚｕｎａ,mizuna\r\n9,pristine_row,ok\r\n");

    fs::remove(png0);
    ASSERT_EQ(exm::menu::run_patch_menus(opt), 0);
    exm::menu::MenuArchivePatcher live_after_p2;
    live_after_p2.load(game_dir / "dwq" / "sys0.gpk", game_dir / "dwq" / "sys0.gtb");
    ASSERT_EQ(live_after_p2.entry_count(), 2u);
    EXPECT_EQ(live_after_p2.decode_entry_rgba(0).pixels, orig_s0.pixels);
    EXPECT_EQ(live_after_p2.decode_entry_rgba(1).pixels, orig_s1.pixels);
}

}  // namespace
