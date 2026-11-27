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
#include "build_tsv/box_budget.h"
#include "cg/image.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "menu/bgra_image.h"
#include "menu/menu_catalog.h"
#include "menu/menu_patch_builder.h"
#include "menu_patch_format.h"
#include "translator_logic.h"

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
#include <unordered_map>
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

TEST(BgiArchive,
     retail_scripts_resolve_every_op140_string_through_deployed_tsv_or_fallback) {
    const auto game_dir_opt = try_resolve_game_dir();
    if (!game_dir_opt.has_value()) {
        GTEST_SKIP() << "VN_GAME_DIR unset; skipping retail script coverage ST";
    }
    const fs::path game_dir = fs::u8path(*game_dir_opt);
    const fs::path tsv_path = fs::u8path(SNU_PROJECT_DIR) / "build" /
                              "install" / "game" / "translations.tsv";
    ASSERT_TRUE(fs::is_regular_file(tsv_path)) << tsv_path.u8string();

    namespace tl = translator_logic;
    const tl::TranslationMap table =
        snu::build_tsv::load_runtime_table(tsv_path.u8string());
    ASSERT_FALSE(table.empty());

    // Resolve retail scripts in the engine's exact archive overlay order.
    struct ArcSource {
        std::string arc;
        std::size_t idx = 0u;
    };
    std::map<std::string, ArcSource> overlay;
    for (const std::string& arc_name : snu::extract::SCRIPT_ARCS) {
        const fs::path arc_path = game_dir / fs::u8path(arc_name);
        std::error_code ec;
        if (!fs::is_regular_file(arc_path, ec)) {
            continue;
        }
        const auto entries = bgi::list_arc(arc_path.u8string());
        for (std::size_t i = 0u; i < entries.size(); ++i) {
            overlay.emplace(entries[i].name, ArcSource{arc_path.u8string(), i});
        }
    }
    if (overlay.empty()) {
        GTEST_SKIP() << "No retail script archives found under VN_GAME_DIR="
                     << *game_dir_opt;
    }

    constexpr std::size_t kHdrBaseBytes = 0x1Cu;
    constexpr std::uint32_t kOpPushStr = 0x03u;
    constexpr std::uint32_t kOpCallFunc = 0x1Cu;
    constexpr std::uint32_t kOpTextDisplay = 0x140u;
    constexpr std::size_t kYuzuTextFnMarkerOff = 0x1Cu;
    constexpr std::size_t kYuzuTextFnAddrOff = 0x20u;
    constexpr std::size_t kYuzuTextCallOff = 0x24u;
    constexpr std::uint8_t kEndMarker[4] = {0x1Bu, 0x00u, 0x00u, 0x00u};
    const std::string kDummyFwSpace = tl::Utf8ToCp932("　");
    const tl::TranslationMap kEmptyTable;

    auto is_yuzu_msg_helper = [](std::string_view sv) {
        if (sv.rfind("iMessage", 0u) == 0u) {
            sv.remove_prefix(8u);
        } else if (sv.rfind("Message", 0u) == 0u) {
            sv.remove_prefix(7u);
        } else {
            return false;
        }
        if (sv.rfind("Left", 0u) == 0u) {
            sv.remove_prefix(4u);
        } else if (sv.rfind("Center", 0u) == 0u) {
            sv.remove_prefix(6u);
        } else if (sv.rfind("Right", 0u) == 0u) {
            sv.remove_prefix(5u);
        } else {
            return false;
        }
        return sv.empty() || sv == "Ig" || sv == "NC";
    };

    auto has_leaked_amp = [](const std::string& bytes) {
        return tl::HasBgiWireDirectiveSuffix(bytes) || bytes == "&" ||
               (bytes.size() >= 2u &&
                (bytes[bytes.size() - 2u] == ' ' ||
                 bytes[bytes.size() - 2u] == '\t') &&
                bytes.back() == '&');
    };

    std::size_t scripts_checked = 0u;
    std::size_t extracted_checked = 0u;
    std::size_t extracted_misses = 0u;
    std::size_t extracted_bad_output = 0u;
    std::size_t non_extracted_checked = 0u;
    std::size_t fallback_resolved = 0u;
    std::size_t non_extracted_misses = 0u;
    std::size_t non_extracted_bad_output = 0u;
    std::string first_extracted_miss;
    std::string first_extracted_bad;
    std::string first_non_extracted_miss;
    std::string first_non_extracted_bad;

    for (const auto& [name, src] : overlay) {
        const Bytes data = bgi::read_entry(src.arc, src.idx);
        const auto entries = snu::extract::extract_one(data);
        if (!entries.has_value()) {
            continue;
        }
        ++scripts_checked;

        std::uint32_t hdr_extra = 0u;
        if (data.size() >= kHdrBaseBytes + sizeof(std::uint32_t)) {
            std::memcpy(&hdr_extra, data.data() + kHdrBaseBytes, sizeof(hdr_extra));
        }
        const std::size_t hdr_size =
            std::min<std::size_t>(kHdrBaseBytes + hdr_extra, data.size());
        std::size_t code_end = hdr_size;
        if (data.size() >= sizeof(kEndMarker)) {
            for (std::size_t i = data.size() - sizeof(kEndMarker) + 1u; i-- > 0u;) {
                if (std::memcmp(data.data() + i, kEndMarker, sizeof(kEndMarker)) == 0) {
                    code_end = std::max(i + sizeof(kEndMarker), hdr_size);
                    break;
                }
            }
        }
        const std::size_t code_size = code_end - hdr_size;

        // 1. Every extracted string's wire-normalized form must resolve directly
        //    in the deployed TSV (never hiding a TSV miss behind fallback) and
        //    satisfy concrete output & DrawBudget invariants.
        std::set<std::int64_t> extracted_addrs;
        for (const snu::extract::Entry& e : *entries) {
            ++extracted_checked;
            extracted_addrs.insert(e.offset - static_cast<std::int64_t>(code_end));
            const std::string wire =
                tl::Utf8ToCp932(tl::StripBgiWireDirectiveSuffix(e.text));
            std::string wire_no_tail = wire;
            while (!wire_no_tail.empty() &&
                   (wire_no_tail.back() == '\n' || wire_no_tail.back() == '\r')) {
                wire_no_tail.pop_back();
            }
            const bool direct_tsv_hit =
                table.count(wire) != 0u || table.count(wire_no_tail) != 0u;

            tl::Surface surface = tl::DIALOGUE_PANEL;
            const std::size_t budget_cap =
                tl::DrawBudget(wire.size(), tl::RECTS_PER_WIRE_BYTE * 8u);
            surface.max_bytes = budget_cap;
            tl::BoxLineBudget budget;
            tl::Substitution sub;
            const tl::SubstitutionStatus st = tl::BuildSubstitution(
                wire.c_str(), table, budget, /*box=*/1, surface, sub);
            if (!direct_tsv_hit || st != tl::SubstitutionStatus::Substituted) {
                ++extracted_misses;
                if (first_extracted_miss.empty()) {
                    first_extracted_miss = name + ": " + e.text;
                }
                continue;
            }
            if (sub.bytes.empty() || has_leaked_amp(sub.bytes) ||
                sub.bytes.size() > budget_cap) {
                ++extracted_bad_output;
                if (first_extracted_bad.empty()) {
                    first_extracted_bad = name + ": " + sub.bytes;
                }
            }
        }

        // 2. Scan every non-extracted op 0x140 TEXT_POS AND yuzu_2g op 0x1C
        //    positioned-message string containing CP932 lead bytes (excluding
        //    dummy "　"), asserting positive fallback proof + output bounds.
        std::unordered_map<std::int64_t, std::string> text_section;
        for (std::size_t pos = code_end, start = code_end; pos <= data.size(); ++pos) {
            if (pos == data.size() || data[pos] == 0u) {
                text_section.emplace(
                    static_cast<std::int64_t>(start - code_end),
                    std::string(reinterpret_cast<const char*>(data.data() + start),
                                pos - start));
                start = pos + 1u;
            }
        }

        std::set<std::int64_t> seen_non_extracted;
        for (std::size_t off = 4u; off + 8u <= code_size; off += 4u) {
            std::uint32_t marker = 0u;
            std::uint32_t addr = 0u;
            std::uint32_t next_op = 0u;
            std::memcpy(&marker, data.data() + hdr_size + off - 4u, 4u);
            std::memcpy(&addr, data.data() + hdr_size + off, 4u);
            std::memcpy(&next_op, data.data() + hdr_size + off + 4u, 4u);
            if (marker != kOpPushStr) {
                continue;
            }
            bool is_text_site = (next_op == kOpTextDisplay);
            if (!is_text_site && off + kYuzuTextCallOff + 4u <= code_size) {
                std::uint32_t fn_marker = 0u;
                std::uint32_t fn_addr = 0u;
                std::uint32_t call_op = 0u;
                std::memcpy(&fn_marker,
                            data.data() + hdr_size + off + kYuzuTextFnMarkerOff,
                            4u);
                std::memcpy(&fn_addr,
                            data.data() + hdr_size + off + kYuzuTextFnAddrOff,
                            4u);
                std::memcpy(&call_op,
                            data.data() + hdr_size + off + kYuzuTextCallOff,
                            4u);
                if (fn_marker == kOpPushStr && call_op == kOpCallFunc) {
                    const std::int64_t fn_ta =
                        static_cast<std::int64_t>(fn_addr) -
                        static_cast<std::int64_t>(code_size);
                    auto fn_it = text_section.find(fn_ta);
                    if (fn_it != text_section.end() &&
                        is_yuzu_msg_helper(fn_it->second)) {
                        is_text_site = true;
                    }
                }
            }
            if (!is_text_site) {
                continue;
            }
            const std::int64_t text_addr =
                static_cast<std::int64_t>(addr) - static_cast<std::int64_t>(code_size);
            if (extracted_addrs.count(text_addr) != 0u) {
                continue;
            }
            auto it = text_section.find(text_addr);
            if (it == text_section.end() ||
                !seen_non_extracted.insert(text_addr).second) {
                continue;
            }
            const std::string& raw_cp932 = it->second;
            if (raw_cp932.empty() || raw_cp932 == kDummyFwSpace ||
                !tl::HasJPLeadByte(raw_cp932.data(), raw_cp932.size())) {
                continue;
            }
            const std::string wire = tl::StripBgiWireDirectiveSuffix(raw_cp932);
            tl::Surface surface = tl::DIALOGUE_PANEL;
            const std::size_t budget_cap =
                tl::DrawBudget(wire.size(), tl::RECTS_PER_WIRE_BYTE * 8u);
            surface.max_bytes = budget_cap;
            tl::BoxLineBudget budget;
            tl::Substitution sub;
            const tl::SubstitutionStatus st = tl::BuildSubstitution(
                wire.c_str(), table, budget, /*box=*/1, surface, sub);
            ++non_extracted_checked;
            if (st != tl::SubstitutionStatus::Substituted) {
                ++non_extracted_misses;
                if (non_extracted_misses <= 10u) {
                    if (!first_non_extracted_miss.empty()) {
                        first_non_extracted_miss += " | ";
                    }
                    first_non_extracted_miss +=
                        name + ": " + tl::Cp932ToUtf8(raw_cp932) + " [";
                    for (unsigned char ch : raw_cp932) {
                        char hex[4];
                        std::snprintf(hex, sizeof(hex), "%02X", ch);
                        first_non_extracted_miss += hex;
                    }
                    first_non_extracted_miss += "]";
                }
                continue;
            }
            // Count positive proof that the punctuation fallback resolved it
            // independently of the TSV table.
            tl::BoxLineBudget fb_budget;
            tl::Substitution fb_sub;
            if (tl::BuildSubstitution(wire.c_str(), kEmptyTable, fb_budget,
                                      /*box=*/1, surface, fb_sub) ==
                tl::SubstitutionStatus::Substituted) {
                ++fallback_resolved;
            }
            if (sub.bytes.empty() || has_leaked_amp(sub.bytes) ||
                sub.bytes.size() > budget_cap) {
                ++non_extracted_bad_output;
                if (first_non_extracted_bad.empty()) {
                    first_non_extracted_bad = name + ": " + sub.bytes;
                }
            }
        }
    }

    EXPECT_GT(scripts_checked, 100u);
    EXPECT_GT(extracted_checked, 60'000u);
    EXPECT_GT(non_extracted_checked, 250u)
        << "expected retail non-extracted op 0x140 / yuzu_2g punctuation lines";
    EXPECT_GT(fallback_resolved, 250u)
        << "fallback must actively resolve non-extracted retail punctuation lines";
    EXPECT_EQ(extracted_misses, 0u)
        << extracted_misses << " extracted retail script entries missed TSV; "
        << "first: " << first_extracted_miss;
    EXPECT_EQ(extracted_bad_output, 0u)
        << extracted_bad_output << " extracted entries produced empty/leaked/over-budget output; "
        << "first: " << first_extracted_bad;
    EXPECT_EQ(non_extracted_misses, 0u)
        << non_extracted_misses
        << " non-extracted retail op 0x140 / yuzu_2g CP932 strings missed both TSV and fallback; "
        << "first: " << first_non_extracted_miss;
    EXPECT_EQ(non_extracted_bad_output, 0u)
        << non_extracted_bad_output
        << " non-extracted retail strings produced empty/leaked/over-budget output; "
        << "first: " << first_non_extracted_bad;
}
