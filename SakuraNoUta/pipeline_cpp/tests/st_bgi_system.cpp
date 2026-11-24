// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests verifying committed/staged menu_patch.bin invariants and
// real-archive BGI/CBG/sysgrp compatibility when VN_GAME_DIR is configured.
#include "apps/paths.h"
#include "bgi/bgi_archive.h"
#include "bgi/bgi_cbg.h"
#include "bgi/bgi_sysgrp.h"
#include "cg/image.h"
#include "common/util.h"
#include "menu/bgra_image.h"
#include "menu/menu_catalog.h"
#include "menu/menu_patch_builder.h"
#include "menu_patch_format.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using snu::Bytes;
namespace bgi = snu::bgi;
namespace fs = std::filesystem;
namespace menu = snu::menu;

constexpr std::size_t kBurikoMagicBytes = 27u;
constexpr std::size_t kMinDecompressedScriptBytes = 10'000u;
constexpr std::size_t kCbgMinHeaderSize = 0x30u;
constexpr std::size_t kCbgVersionLoOffset = 0x2Eu;
constexpr std::size_t kCbgVersionHiOffset = 0x2Fu;
constexpr std::uint8_t kCbgVersionOne = 1u;
constexpr int kExpectedSplashWidth = 1280;
constexpr int kExpectedSplashHeight = 720;
constexpr int kExpectedChoiceWidth = 2982;
constexpr int kExpectedChoiceHeight = 58;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
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

std::optional<std::string> try_resolve_game_dir() {
    const wchar_t* env = _wgetenv(L"VN_GAME_DIR");
    if (env == nullptr || *env == L'\0') {
        return std::nullopt;
    }
    return snu::apps::wide_to_utf8(env);
}

fs::path resolve_pristine_arc(const fs::path& game_dir,
                              std::string_view rel_arc) {
    std::error_code ec;
    const fs::path base = game_dir / fs::u8path(rel_arc);
    fs::path dot_bak = base;
    dot_bak += ".bak";
    if (fs::is_regular_file(dot_bak, ec)) {
        return dot_bak;
    }
    fs::path dot_backup = base;
    dot_backup += ".backup";
    if (fs::is_regular_file(dot_backup, ec)) {
        return dot_backup;
    }
    return base;
}

}  // namespace

TEST(MenuPatchSystem,
     CommittedMenuPatchBinSatisfiesManifestSignaturesAndRetailUnmaskParity) {
    const fs::path project_dir = fs::u8path(SNU_PROJECT_DIR);
    const fs::path patch_path = project_dir / "menu_patch.bin";
    ASSERT_TRUE(fs::is_regular_file(patch_path))
        << "Missing required SakuraNoUta/menu_patch.bin at "
        << patch_path.u8string();

    const Bytes blob = snu::read_file(patch_path.u8string());
    ASSERT_GT(blob.size(), menu_patch_format::kPatchHeaderBytes);

    // Staged install/game/menu_patch.bin (when staged) must match byte-for-byte.
    const fs::path staged_patch =
        project_dir / "build" / "install" / "game" / "menu_patch.bin";
    std::error_code ec;
    if (fs::is_regular_file(staged_patch, ec)) {
        EXPECT_EQ(snu::read_file(staged_patch.u8string()), blob);
    }

    std::string sig_violation;
    EXPECT_TRUE(menu::VerifyNoRawAssetSignatures(
        menu_patch_format::ByteSpan(blob), &sig_violation))
        << sig_violation;
    EXPECT_NO_THROW(
        menu::AssertNoRawAssetSignatures(menu_patch_format::ByteSpan(blob)));

    menu_patch_format::MenuPatchManifest manifest{};
    ASSERT_EQ(menu_patch_format::ParseMenuPatchManifest(
                  menu_patch_format::ByteSpan(blob), manifest),
              menu_patch_format::ManifestStatus::kOk);
    EXPECT_EQ(manifest.header.version, menu_patch_format::kFormatVersion);
    EXPECT_NE(manifest.header.manifest_hash, 0u);
    ASSERT_EQ(manifest.header.record_count, menu::kExpectedRetailRecordCount);
    ASSERT_EQ(manifest.records.size(), menu::kExpectedRetailRecordCount);

    std::set<std::string> sysgrp_entries;
    std::set<std::string> splash_targets;
    std::set<std::string> choice_targets;

    for (const auto& rec : manifest.records) {
        SCOPED_TRACE(rec.target_rel_path + "::" + rec.entry_name);
        EXPECT_TRUE(menu_patch_format::IsValidRelativePath(rec.source_rel_path));
        EXPECT_TRUE(menu_patch_format::IsAllowedTargetPath(rec.target_rel_path));
        EXPECT_GE(rec.orig_size, menu_patch_format::kMinOrigSliceBytes);
        EXPECT_LE(rec.orig_size, menu_patch_format::kMaxAssetPayloadBytes);
        EXPECT_GT(rec.new_size, 0u);
        EXPECT_LE(rec.new_size, menu_patch_format::kMaxAssetPayloadBytes);

        if ((rec.flags & menu_patch_format::kFlagArchiveEntry) != 0u) {
            EXPECT_EQ(rec.source_rel_path, menu::kSysGrpArcRelPath);
            EXPECT_EQ(rec.target_rel_path, menu::kSysGrpArcRelPath);
            EXPECT_TRUE(sysgrp_entries.insert(rec.entry_name).second);
        } else {
            EXPECT_EQ(rec.flags, menu_patch_format::kFlagBackupStandalone);
            if (rec.source_rel_path == menu::kSplashArcRelPath) {
                EXPECT_TRUE(splash_targets.insert(rec.target_rel_path).second);
            } else {
                EXPECT_EQ(rec.source_rel_path, menu::kChoiceArcRelPath);
                EXPECT_TRUE(choice_targets.insert(rec.target_rel_path).second);
            }
        }
    }

    EXPECT_EQ(splash_targets.size(), menu::kExpectedSplashRecordCount);
    EXPECT_TRUE(splash_targets.count("att01") > 0u);
    EXPECT_TRUE(splash_targets.count("att02") > 0u);
    EXPECT_EQ(sysgrp_entries.size(), menu::kExpectedSysGrpRecordCount);
    for (const std::string_view expected_name :
         menu::AllLocalizedSysGrpEntryNames()) {
        EXPECT_TRUE(sysgrp_entries.count(std::string(expected_name)) > 0u)
            << "Missing localized sysgrp entry: " << expected_name;
    }
    EXPECT_EQ(choice_targets.size(), menu::kExpectedChoiceRecordCount);
    EXPECT_TRUE(choice_targets.count("select01-1") > 0u);
    EXPECT_TRUE(choice_targets.count("select17-2") > 0u);

    // Optional real-archive verification when VN_GAME_DIR points to a retail
    // installation: unmasks all 118 records read-only without mutating the game.
    const auto game_dir_opt = try_resolve_game_dir();
    if (!game_dir_opt.has_value()) {
        return;
    }
    const fs::path game_dir = fs::u8path(*game_dir_opt);
    const fs::path sysgrp_path =
        resolve_pristine_arc(game_dir, menu::kSysGrpArcRelPath);
    if (!fs::is_regular_file(sysgrp_path, ec)) {
        GTEST_SKIP() << "sysgrp.arc not present under VN_GAME_DIR="
                     << *game_dir_opt;
    }

    const auto sysgrp_mtime_before = fs::last_write_time(sysgrp_path);
    const auto sysgrp_size_before = fs::file_size(sysgrp_path);

    std::map<std::string, Bytes> loaded_arcs;
    for (const std::string_view rel_arc :
         {menu::kSysGrpArcRelPath, menu::kSplashArcRelPath,
          menu::kChoiceArcRelPath}) {
        const fs::path arc_file = resolve_pristine_arc(game_dir, rel_arc);
        ASSERT_TRUE(fs::is_regular_file(arc_file))
            << "Missing retail archive: " << arc_file.u8string();
        loaded_arcs.emplace(std::string(rel_arc),
                            snu::read_file(arc_file.u8string()));
    }

    std::vector<Bytes> unmasked_sysgrp_payloads;
    unmasked_sysgrp_payloads.reserve(menu::kExpectedSysGrpRecordCount);
    std::vector<menu_patch_format::ArcEntryReplacement> replacements;
    replacements.reserve(menu::kExpectedSysGrpRecordCount);

    for (const auto& rec : manifest.records) {
        SCOPED_TRACE(rec.target_rel_path + "::" + rec.entry_name);
        const Bytes& arc_bytes = loaded_arcs.at(rec.source_rel_path);
        std::uint64_t slice_off = rec.orig_offset;
        std::uint64_t slice_sz = rec.orig_size;
        bool direct_ok =
            (slice_off + slice_sz <= arc_bytes.size()) &&
            (menu_patch_format::MurmurHash2(
                 menu_patch_format::ByteSpan(arc_bytes).subspan(
                     static_cast<std::size_t>(slice_off),
                     static_cast<std::size_t>(slice_sz))) ==
             rec.orig_slice_hash);
        if (!direct_ok && !rec.entry_name.empty()) {
            ASSERT_EQ(menu_patch_format::FindArc20EntrySlice(
                          menu_patch_format::ByteSpan(arc_bytes),
                          rec.entry_name, slice_off, slice_sz),
                      menu_patch_format::ArcStatus::kOk);
        }

        const menu_patch_format::ByteSpan orig_slice =
            menu_patch_format::ByteSpan(arc_bytes).subspan(
                static_cast<std::size_t>(slice_off),
                static_cast<std::size_t>(slice_sz));
        const menu_patch_format::ByteSpan xor_payload =
            menu_patch_format::ByteSpan(blob).subspan(
                rec.payload_offset, static_cast<std::size_t>(rec.new_size));

        Bytes unmasked;
        ASSERT_EQ(menu_patch_format::UnmaskRecordPayload(
                      xor_payload, orig_slice, rec.orig_slice_hash,
                      rec.new_raw_hash, unmasked),
                  menu_patch_format::UnmaskStatus::kOk);

        if ((rec.flags & menu_patch_format::kFlagArchiveEntry) != 0u) {
            const Bytes orig_comp(orig_slice.begin(), orig_slice.end());
            const bgi::SysGrpImage orig_img =
                bgi::decode_sysgrp(bgi::dsc_decompress(orig_comp));
            const bgi::SysGrpImage new_img =
                bgi::decode_sysgrp(bgi::dsc_decompress(unmasked));
            EXPECT_EQ(new_img.image.width(), orig_img.image.width());
            EXPECT_EQ(new_img.image.height(), orig_img.image.height());
            EXPECT_EQ(new_img.orig_bpp, orig_img.orig_bpp);
            EXPECT_EQ(new_img.reserved, orig_img.reserved);

            unmasked_sysgrp_payloads.push_back(std::move(unmasked));
            replacements.push_back(menu_patch_format::ArcEntryReplacement{
                rec.entry_name,
                menu_patch_format::ByteSpan(unmasked_sysgrp_payloads.back())});
        } else {
            const menu::BgraImage bmp =
                menu::BgraImage::DecodeBmpOrPng(unmasked);
            if (rec.source_rel_path == menu::kSplashArcRelPath) {
                EXPECT_EQ(bmp.width(), kExpectedSplashWidth);
                EXPECT_EQ(bmp.height(), kExpectedSplashHeight);
            } else {
                EXPECT_EQ(bmp.width(), kExpectedChoiceWidth);
                EXPECT_EQ(bmp.height(), kExpectedChoiceHeight);
            }
        }
    }

    Bytes repacked_sysgrp;
    EXPECT_EQ(menu_patch_format::RepackArc20Entries(
                  menu_patch_format::ByteSpan(
                      loaded_arcs.at(std::string(menu::kSysGrpArcRelPath))),
                  replacements, repacked_sysgrp),
              menu_patch_format::ArcStatus::kOk);
    EXPECT_GT(repacked_sysgrp.size(), menu_patch_format::kArc20HeaderBytes);

    EXPECT_EQ(fs::last_write_time(sysgrp_path), sysgrp_mtime_before);
    EXPECT_EQ(fs::file_size(sysgrp_path), sysgrp_size_before);
}

TEST(BgiArchive, real_arc_dsc_decompresses_to_buriko_script) {
    const auto game_dir_opt = try_resolve_game_dir();
    if (!game_dir_opt.has_value()) {
        GTEST_SKIP() << "VN_GAME_DIR unset; skipping real archive DSC test";
    }
    const std::string arc = *game_dir_opt + "\\data01100.arc";
    if (!fs::exists(fs::u8path(arc))) {
        GTEST_SKIP() << "game arc not present at " << arc;
    }
    auto entries = bgi::list_arc(arc);
    bool found = false;
    for (const auto& e : entries) {
        EXPECT_GT(e.size, 0u);
        if (e.name == "00_op_01") {
            found = true;
        }
    }
    ASSERT_TRUE(found);
    Bytes data = bgi::read_entry_by_name(arc, "00_op_01");
    ASSERT_GE(data.size(), kBurikoMagicBytes);
    EXPECT_EQ(std::memcmp(data.data(), "BurikoCompiledScriptVer1.00", kBurikoMagicBytes), 0);
    EXPECT_GT(data.size(), kMinDecompressedScriptBytes);
}

TEST(Cbg, real_cbg_matches_garbro_reference) {
    const auto game_dir_opt = try_resolve_game_dir();
    if (!game_dir_opt.has_value()) {
        GTEST_SKIP() << "VN_GAME_DIR unset; skipping CBG reference test";
    }
    const std::string arc = *game_dir_opt + "\\data02503.arc";
    const std::string ref_exe =
        std::string(SNU_PROJECT_DIR) + "\\..\\TOOLS\\garbro\\extract_cbg.exe";
    if (!fs::exists(fs::u8path(arc)) || !fs::exists(fs::u8path(ref_exe))) {
        GTEST_SKIP() << "game data or extract_cbg.exe not available";
    }

    auto entries = snu::bgi::list_arc(arc);
    Bytes raw;
    std::string picked;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Bytes r = snu::bgi::read_entry(arc, i, /*decompress=*/false);
        if (snu::bgi::is_cbg(r) && r.size() > kCbgMinHeaderSize &&
            r[kCbgVersionLoOffset] == kCbgVersionOne &&
            r[kCbgVersionHiOffset] == 0) {
            raw = std::move(r);
            picked = entries[i].name;
            break;
        }
    }
    if (raw.empty()) {
        GTEST_SKIP() << "no v1 CBG entry found in " << arc;
    }
    GTEST_LOG_(INFO) << "cross-validating entry: " << picked;

    bgi::CbgImage ours = bgi::decode_cbg(raw);

    const ScopedTempDir scratch("snu_st_bgi_cbg_ref");
    const std::string tmp_cbg =
        (scratch.path() / fs::u8path("ref.cbg")).u8string();
    const std::string tmp_bmp =
        (scratch.path() / fs::u8path("ref.bmp")).u8string();
    snu::write_file(tmp_cbg, raw);
    ASSERT_EQ(snu::apps::run_process(ref_exe, {tmp_cbg, tmp_bmp}),
              snu::apps::kExitOk);

    Bytes ref_bmp = snu::read_file(tmp_bmp);
    auto ref = snu::cg::decode_image(ref_bmp);
    ASSERT_TRUE(ref.has_value());
    snu::cg::Image mine =
        snu::cg::from_cbg_pixels(ours.pixels, ours.width, ours.height, ours.bpp);

    ASSERT_EQ(mine.width, ref->width);
    ASSERT_EQ(mine.height, ref->height);
    EXPECT_EQ(mine.rgb, ref->rgb);
}
