// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end system verification of Adieu's committed `menu_patch.bin` and full
// 5-domain `MenuPatchPipeline` against retail `StartData\GameData` (strictly
// read-only, staging into a temporary sandbox). Skips cleanly when `VN_GAME_DIR`
// is unset.

#include "g00/g00_codec.h"
#include "g00/gameexe_codec.h"
#include "imaging/bgra_image.h"
#include "menu/adieu_menu_pipeline.h"
#include "menu/core/menu_patch_container.h"
#include "menu/domain_renderer.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || defined(__wincodec_h__)
#error "Windows header leaked through public headers"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace adi::menu {
namespace {

namespace fs = std::filesystem;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        const std::string name =
            "adi_st_menu_patcher_さよなら_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

std::optional<fs::path> TryResolveRetailGameDirFromEnv() {
    const wchar_t* raw_env = _wgetenv(L"VN_GAME_DIR");
    if (raw_env == nullptr || *raw_env == L'\0') {
        return std::nullopt;
    }
    return ResolveRetailGameDataDir(fs::path(raw_env));
}

fs::path PreferFactoryBackupPath(const fs::path& target) {
    fs::path bak = target;
    bak += ".bak";
    std::error_code ec;
    if (fs::is_regular_file(bak, ec)) {
        return bak;
    }
    return target;
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("failed to open file: " + p.u8string());
    }
    const std::streamoff sz = in.tellg();
    if (sz < 0) {
        throw std::runtime_error("failed to stat file: " + p.u8string());
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz), 0u);
    in.seekg(0, std::ios::beg);
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()), sz);
        if (!in || in.gcount() != sz) {
            throw std::runtime_error("short read on file: " + p.u8string());
        }
    }
    return out;
}

bool ContainsJapaneseUiText(std::string_view utf8, std::string* sample_out) {
    std::size_t i = 0u;
    while (i < utf8.size()) {
        const auto b0 = static_cast<std::uint8_t>(utf8[i]);
        if (b0 < 0x80u) {
            ++i;
            continue;
        }
        std::uint32_t cp = 0u;
        std::size_t width = 1u;
        if ((b0 & 0xE0u) == 0xC0u && i + 1u < utf8.size()) {
            cp = ((b0 & 0x1Fu) << 6u) |
                 (static_cast<std::uint8_t>(utf8[i + 1u]) & 0x3Fu);
            width = 2u;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2u < utf8.size()) {
            cp = ((b0 & 0x0Fu) << 12u) |
                 ((static_cast<std::uint8_t>(utf8[i + 1u]) & 0x3Fu) << 6u) |
                 (static_cast<std::uint8_t>(utf8[i + 2u]) & 0x3Fu);
            width = 3u;
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3u < utf8.size()) {
            width = 4u;
        }

        const bool is_jp =
            (cp >= 0x3040u && cp <= 0x309Fu) ||  // Hiragana
            (cp >= 0x30A0u && cp <= 0x30FFu) ||  // Katakana
            (cp >= 0x4E00u && cp <= 0x9FFFu) ||  // CJK Unified Ideographs
            (cp >= 0xFF01u && cp <= 0xFF9Fu);    // Fullwidth/halfwidth JP forms
        if (is_jp) {
            if (sample_out != nullptr) {
                const std::size_t start = (i > 24u) ? (i - 24u) : 0u;
                const std::size_t len = std::min<std::size_t>(64u, utf8.size() - start);
                *sample_out = std::string(utf8.substr(start, len));
            }
            return true;
        }
        i += width;
    }
    return false;
}

}  // namespace

TEST(MenuPatcherSt,
     CommittedMenuPatchBinUnmasksCleanlyAgainstRetailAssetsAndRebakesDeterministically) {
    const std::optional<fs::path> retail_dir_opt = TryResolveRetailGameDirFromEnv();
    if (!retail_dir_opt.has_value()) {
        GTEST_SKIP() << "VN_GAME_DIR unset; skipping retail menu_patch.bin system test";
    }
    const fs::path retail_dir = *retail_dir_opt;

    const fs::path retail_gameexe = retail_dir / "Gameexe.dat";
    const auto gameexe_mtime_before = fs::last_write_time(retail_gameexe);
    const auto gameexe_size_before = fs::file_size(retail_gameexe);

    const fs::path committed_patch_path =
        fs::u8path(ADIEU_SOURCE_DIR) / "menu_patch.bin";
    ASSERT_TRUE(fs::is_regular_file(committed_patch_path))
        << "Missing committed Adieu/menu_patch.bin at "
        << committed_patch_path.u8string();

    const std::vector<std::uint8_t> committed_blob =
        ReadBinaryFile(committed_patch_path);
    ASSERT_TRUE(VerifyNoRawAssetSignatures(committed_blob));
    const MenuPatchManifest manifest = ParseMenuPatchContainer(committed_blob);
    ASSERT_GE(manifest.records.size(), 10u);

    // Stage all referenced retail files into an isolated sandbox (retail dir is
    // strictly read-only).
    ScopedTempDir sandbox;
    const fs::path staged_dir = sandbox.path() / "GameData";
    fs::create_directories(staged_dir / "g00");
    fs::copy_file(PreferFactoryBackupPath(retail_dir / "SiglusEngine.exe"),
                  staged_dir / "SiglusEngine.exe",
                  fs::copy_options::overwrite_existing);

    const g00::GameexeKey256 key256 =
        g00::LoadGameexeKeyFromExe(staged_dir / "SiglusEngine.exe");

    bool saw_gameexe_record = false;
    std::size_t g00_records_verified = 0u;

    for (const MenuPatchRecord& rec : manifest.records) {
        SCOPED_TRACE(rec.target_rel_path);
        const fs::path src_retail =
            PreferFactoryBackupPath(retail_dir / fs::u8path(rec.source_rel_path));
        ASSERT_TRUE(fs::is_regular_file(src_retail))
            << "Missing retail source asset: " << src_retail.u8string();

        const fs::path staged_src = staged_dir / fs::u8path(rec.source_rel_path);
        if (staged_src.has_parent_path()) {
            fs::create_directories(staged_src.parent_path());
        }
        if (!fs::exists(staged_src)) {
            fs::copy_file(src_retail, staged_src);
        }

        const std::vector<std::uint8_t> full_orig = ReadBinaryFile(staged_src);
        ASSERT_LE(rec.orig_offset + rec.orig_size, full_orig.size());
        const boost::span<const std::uint8_t> orig_slice(
            full_orig.data() + static_cast<std::size_t>(rec.orig_offset),
            static_cast<std::size_t>(rec.orig_size));

        const std::vector<std::uint8_t> patched_bytes =
            UnmaskMenuPatchRecord(rec, orig_slice);
        ASSERT_EQ(patched_bytes.size(), static_cast<std::size_t>(rec.new_size));

        if (rec.target_rel_path == "Gameexe.dat") {
            saw_gameexe_record = true;
            const g00::GameexeDocument orig_doc =
                g00::GameexeDocument::FromUtf16(g00::DecodeGameexe(full_orig, key256));
            const g00::GameexeDocument patched_doc = g00::GameexeDocument::FromUtf16(
                g00::DecodeGameexe(patched_bytes, key256));

            const std::string orig_utf8 = orig_doc.ToUtf8(false);
            const std::string patched_utf8 = patched_doc.ToUtf8(false);
            g00::VerifyCriticalDirectivesUnchanged(orig_utf8, patched_utf8);

            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#DISCMARK"),
                      std::optional<std::string>("\"sayonara.env\""));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#GAMENAME"),
                      std::optional<std::string>(
                          "\"Sayonara o Oshiete ~comment te dire adieu~\""));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#MWND.002.MOJI_SIZE"),
                      std::optional<std::string>("20"));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#MWND.002.MOJI_SPACE"),
                      std::optional<std::string>("0, 3"));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#MWND.002.MOJI_CNT"),
                      std::optional<std::string>("30, 19"));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#MWND.002.NAME_MOJI_SIZE"),
                      std::optional<std::string>("16"));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#MWND.002.TALK_MARGIN"),
                      std::optional<std::string>("20, 24, 0, 20"));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#SELBTN.000.MOJI_SIZE"),
                      std::optional<std::string>("22, 0, 0, 0"));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#SELBTN.001.MOJI_SIZE"),
                      std::optional<std::string>("22, 0, 0, 0"));

            std::string jp_sample;
            EXPECT_FALSE(ContainsJapaneseUiText(patched_utf8, &jp_sample))
                << "Remaining Japanese UI text in patched Gameexe.dat: " << jp_sample;
            continue;
        }

        ++g00_records_verified;
        const g00::G00Image orig_img = g00::G00Codec::Decode(full_orig);
        const g00::G00Image patched_img = g00::G00Codec::Decode(patched_bytes);
        ASSERT_EQ(patched_img.type, orig_img.type);
        ASSERT_EQ(patched_img.width, orig_img.width);
        ASSERT_EQ(patched_img.height, orig_img.height);

        if (patched_img.type == g00::G00Type::kType2MultiCutBgra32) {
            ASSERT_EQ(patched_img.cuts.size(), orig_img.cuts.size());
            std::set<std::size_t> changed_cuts;
            for (std::size_t c = 0u; c < patched_img.cuts.size(); ++c) {
                if (patched_img.cuts[c].canvas != orig_img.cuts[c].canvas) {
                    changed_cuts.insert(c);
                }
            }
            EXPECT_FALSE(changed_cuts.empty())
                << "Patched Type 2 .g00 modified zero cuts: " << rec.target_rel_path;
            std::string parity_err;
            EXPECT_TRUE(g00::G00Codec::VerifyMetadataParity(
                full_orig, patched_bytes, changed_cuts, &parity_err))
                << parity_err;
        } else {
            EXPECT_NE(patched_img.canvas, orig_img.canvas)
                << "Patched Type 0/1 .g00 is pixel-identical to original: "
                << rec.target_rel_path;
            std::string parity_err;
            EXPECT_TRUE(g00::G00Codec::VerifyMetadataParity(
                full_orig, patched_bytes, {}, &parity_err))
                << parity_err;
        }
    }

    EXPECT_TRUE(saw_gameexe_record);
    EXPECT_GE(g00_records_verified, 9u);

    // Re-running the full 5-domain production pipeline against retail must
    // reproduce the committed Adieu/menu_patch.bin byte-for-byte.
    const fs::path rebaked_path = sandbox.path() / "rebaked_menu_patch.bin";
    const MenuPatchPipeline pipeline = CreateAdieuMenuPatchPipeline();
    const MenuPatchBuildSummary rebake_summary =
        pipeline.Run(MenuPatchBuildOptions{retail_dir, rebaked_path, {}});
    EXPECT_EQ(rebake_summary.manifest_hash, manifest.manifest_hash);
    EXPECT_EQ(ReadBinaryFile(rebaked_path), committed_blob);

    // Retail folder remained 100% untouched.
    EXPECT_EQ(fs::last_write_time(retail_gameexe), gameexe_mtime_before);
    EXPECT_EQ(fs::file_size(retail_gameexe), gameexe_size_before);
}

}  // namespace adi::menu
