// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box component tests for MenuPatchBuilder / BuildMenuPatch over
// synthetic multi-entry BURIKO ARC20 archives (sysgrp.arc, data02501.arc,
// data02502.arc, and BurikoCompiledScriptVer1.00 choice census), verifying
// "SNUMENU1" v1 serialization, raw-asset-signature obfuscation, round-trip
// consumer unmasking + RepackArc20Entries + dsc_decompress, idempotent .bak
// fallback, and atomic failure rollback.

#include "bgi/bgi_archive.h"
#include "bgi/bgi_sysgrp.h"
#include "common/util.h"
#include "menu/bgra_image.h"
#include "menu/menu_catalog.h"
#include "menu/menu_patch_builder.h"
#include "menu_patch_format.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_)
#error "Windows header leaked through public menu/bgi/wire headers"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bgi = snu::bgi;
namespace menu = snu::menu;

using menu_patch_format::ArcEntryReplacement;
using menu_patch_format::ArcStatus;
using menu_patch_format::ByteSpan;
using menu_patch_format::FindArc20EntrySlice;
using menu_patch_format::kArc20ExtraFieldBytes;
using menu_patch_format::kArc20ExtraFieldOffset;
using menu_patch_format::kArc20HeaderBytes;
using menu_patch_format::kArc20IndexEntryBytes;
using menu_patch_format::kArc20Magic;
using menu_patch_format::kArc20MagicBytes;
using menu_patch_format::kArc20NameFieldBytes;
using menu_patch_format::kFlagArchiveEntry;
using menu_patch_format::kFlagBackupStandalone;
using menu_patch_format::kFormatVersion;
using menu_patch_format::ManifestStatus;
using menu_patch_format::MenuPatchManifest;
using menu_patch_format::MenuPatchRecord;
using menu_patch_format::ParseMenuPatchManifest;
using menu_patch_format::RepackArc20Entries;
using menu_patch_format::UnmaskRecordPayload;
using menu_patch_format::UnmaskStatus;
using snu::Bytes;

constexpr int kSplashWidth = 1280;
constexpr int kSplashHeight = 720;
constexpr int kTitleWidth = 1280;
constexpr int kTitleHeight = 720;
constexpr int kLockedCgWidth = 291;
constexpr int kLockedCgHeight = 170;
constexpr int kConfigToggleWidth = 552;
constexpr int kConfigToggleHeight = 37;
constexpr int kDialogWidth = 722;
constexpr int kDialogHeight = 242;
constexpr int kTooltipWidth = 440;
constexpr int kTooltipHeight = 20;
constexpr int kMusicWidth = 864;
constexpr int kMusicHeight = 42;
constexpr int kChoicePlateWidth = 2982;
constexpr int kChoicePlateHeight = 58;
constexpr int kChoiceTileCount = 3;
constexpr int kChoiceTileWidth = kChoicePlateWidth / kChoiceTileCount;

constexpr std::uint8_t kDialogTranslucentAlpha = 223u;
constexpr std::uint8_t kChoiceTile0Alpha = 130u;
constexpr std::uint8_t kChoiceTile1Alpha = 182u;
constexpr std::uint8_t kChoiceTile2Alpha = 182u;

constexpr int kDialogTriangleX = 157;
constexpr int kDialogTriangleY = 108;
constexpr int kDialogTriangleW = 27;
constexpr int kDialogTriangleH = 24;

constexpr int kMusicSakuraCropX = 581;
constexpr int kMusicSakuraCropY = 11;
constexpr int kMusicSakuraCropW = 21;
constexpr int kMusicSakuraCropH = 20;

constexpr std::size_t kBurikoMagicChars = 27u;
constexpr std::uint32_t kScriptPushStrOp = 0x03u;
constexpr std::uint32_t kScriptMenuOptionOp = 0x149u;
constexpr std::uint32_t kScriptEndMarkerOp = 0x1Bu;
constexpr int kDwordBytes = 4;
constexpr int kBitsPerByte = 8;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr std::size_t kUntouchedPayloadBytes = 96u;

class ScopedSandbox {
public:
    ScopedSandbox() {
        static std::atomic<unsigned int> seq{0u};
        const std::string leaf =
            "snu_ct_menu_patch_櫻の詩_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(leaf);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedSandbox() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedSandbox(const ScopedSandbox&) = delete;
    ScopedSandbox& operator=(const ScopedSandbox&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

void AppendLe32(Bytes& out, std::uint32_t val) {
    for (int i = 0; i < kDwordBytes; ++i) {
        out.push_back(
            static_cast<std::uint8_t>((val >> (kBitsPerByte * i)) & kByteMask));
    }
}

struct ArcItemSpec {
    std::string name;
    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra{};
    Bytes payload;
};

Bytes BuildArc20WithExtras(const std::vector<ArcItemSpec>& items) {
    Bytes arc;
    arc.insert(arc.end(), kArc20Magic, kArc20Magic + kArc20MagicBytes);
    AppendLe32(arc, static_cast<std::uint32_t>(items.size()));

    std::uint32_t rel_offset = 0u;
    for (const auto& item : items) {
        std::array<std::uint8_t, kArc20NameFieldBytes> name_field{};
        const std::size_t copy_len =
            std::min(item.name.size(), kArc20NameFieldBytes - 1u);
        std::memcpy(name_field.data(), item.name.data(), copy_len);
        arc.insert(arc.end(), name_field.begin(), name_field.end());
        AppendLe32(arc, rel_offset);
        AppendLe32(arc, static_cast<std::uint32_t>(item.payload.size()));
        arc.insert(arc.end(), item.extra.begin(), item.extra.end());
        rel_offset += static_cast<std::uint32_t>(item.payload.size());
    }

    for (const auto& item : items) {
        arc.insert(arc.end(), item.payload.begin(), item.payload.end());
    }
    return arc;
}

Bytes BuildChoiceScriptBytecode(
    const std::vector<std::pair<std::string, std::string>>& choices) {
    Bytes text_sec;
    auto push_cp932 = [&](const std::string& utf8) -> std::uint32_t {
        const Bytes cp = snu::utf8_to_cp932_replace(utf8);
        const auto off = static_cast<std::uint32_t>(text_sec.size());
        text_sec.insert(text_sec.end(), cp.begin(), cp.end());
        text_sec.push_back(0u);
        return off;
    };

    std::vector<std::pair<std::uint32_t, std::uint32_t>> str_offs;
    str_offs.reserve(choices.size());
    for (const auto& [label, jp_text] : choices) {
        const std::uint32_t label_off = push_cp932(label);
        const std::uint32_t opt_off = push_cp932(jp_text);
        str_offs.emplace_back(label_off, opt_off);
    }

    const std::size_t code_dword_count = choices.size() * 5u + 1u;
    const auto code_byte_size =
        static_cast<std::uint32_t>(code_dword_count * kDwordBytes);

    std::vector<std::uint32_t> dwords;
    dwords.reserve(code_dword_count);
    for (const auto& [label_off, opt_off] : str_offs) {
        dwords.push_back(kScriptPushStrOp);
        dwords.push_back(code_byte_size + label_off);
        dwords.push_back(kScriptPushStrOp);
        dwords.push_back(code_byte_size + opt_off);
        dwords.push_back(kScriptMenuOptionOp);
    }
    dwords.push_back(kScriptEndMarkerOp);

    Bytes script;
    const char magic[] = "BurikoCompiledScriptVer1.00";
    script.insert(script.end(), magic, magic + kBurikoMagicChars);
    script.push_back(0u);
    AppendLe32(script, static_cast<std::uint32_t>(kDwordBytes));
    for (const std::uint32_t d : dwords) {
        AppendLe32(script, d);
    }
    script.insert(script.end(), text_sec.begin(), text_sec.end());
    return script;
}

menu::BgraImage MakeSyntheticSplash(bool is_att01) {
    menu::BgraImage img(kSplashWidth, kSplashHeight,
                        menu::BgraColor{228u, 220u, 236u, menu::kOpaqueAlpha});
    for (int y = 0; y < kSplashHeight; ++y) {
        for (int x = 0; x < kSplashWidth; ++x) {
            menu::BgraColor c{
                static_cast<std::uint8_t>(220u + ((x + y) & 0x0Fu)),
                static_cast<std::uint8_t>(212u + ((x * 3 + y) & 0x0Fu)),
                static_cast<std::uint8_t>(232u + ((x + y * 2) & 0x0Fu)),
                menu::kOpaqueAlpha,
            };
            img.SetPixel(x, y, c);
        }
    }
    // Synthetic navy Japanese disclaimer strokes in non-overlapping bands so
    // cross-donor restoration between att01 and att02 exercises both images.
    if (is_att01) {
        img.FillRect(menu::RectI{150, 580, 180, 6},
                     menu::BgraColor{120u, 50u, 20u, menu::kOpaqueAlpha});
    } else {
        img.FillRect(menu::RectI{150, 450, 180, 6},
                     menu::BgraColor{120u, 50u, 20u, menu::kOpaqueAlpha});
    }
    // Synthetic dark subtitle strokes between the bottom-right logo rules.
    img.FillRect(menu::RectI{810, 660, 120, 4},
                 menu::BgraColor{60u, 45u, 35u, menu::kOpaqueAlpha});
    return img;
}

menu::BgraImage MakeSyntheticTitleBg() {
    menu::BgraImage img(kTitleWidth, kTitleHeight,
                        menu::BgraColor{235u, 210u, 190u, menu::kOpaqueAlpha});
    // Feather-moon calligraphy region outside the subtitle band (must stay intact).
    img.FillRect(menu::RectI{260, 180, 200, 40},
                 menu::BgraColor{45u, 35u, 30u, menu::kOpaqueAlpha});
    // Small dark subtitle kanji inside [225..560, 292..318].
    img.FillRect(menu::RectI{280, 302, 160, 5},
                 menu::BgraColor{50u, 40u, 35u, menu::kOpaqueAlpha});
    return img;
}

menu::BgraImage MakeSyntheticLockedCg() {
    menu::BgraImage img(kLockedCgWidth, kLockedCgHeight,
                        menu::BgraColor{225u, 222u, 220u, menu::kOpaqueAlpha});
    img.FillRect(menu::RectI{104, 115, 80, 4},
                 menu::BgraColor{55u, 48u, 44u, menu::kOpaqueAlpha});
    return img;
}

menu::BgraImage MakeSyntheticToggleStrip() {
    menu::BgraImage img(kConfigToggleWidth, kConfigToggleHeight,
                        menu::BgraColor{0u, 0u, 0u, menu::kTransparentAlpha});
    for (int cell = 0; cell < 4; ++cell) {
        const int base_x = cell * 138;
        img.FillRect(menu::RectI{base_x + 8, 4, 122, 29},
                     menu::BgraColor{236u, 214u, 228u, 240u});
        img.FillRect(menu::RectI{base_x + 44, 15, 50, 5},
                     menu::BgraColor{40u, 38u, 42u, menu::kOpaqueAlpha});
    }
    return img;
}

menu::BgraImage MakeSyntheticDialogPlate() {
    menu::BgraImage img(kDialogWidth, kDialogHeight,
                        menu::BgraColor{246u, 246u, 248u, kDialogTranslucentAlpha});
    // Orange '!' warning triangle (`R > 190, 90 < G < 210, B < 100`) at retail
    // SGDialog990000 coordinates `(157..183, 108..131)`.
    for (int y = kDialogTriangleY; y < kDialogTriangleY + kDialogTriangleH; ++y) {
        for (int x = kDialogTriangleX; x < kDialogTriangleX + kDialogTriangleW; ++x) {
            const menu::BgraColor orange{
                static_cast<std::uint8_t>(42u + ((x + y) & 0x07u)),
                static_cast<std::uint8_t>(148u + ((x * 2) & 0x0Fu)),
                static_cast<std::uint8_t>(232u + (y & 0x07u)),
                menu::kOpaqueAlpha,
            };
            img.SetPixel(x, y, orange);
        }
    }
    // Old Japanese prompt strokes purely to the right of the warning triangle.
    img.FillRect(menu::RectI{204, 116, 180, 8},
                 menu::BgraColor{88u, 88u, 88u, kDialogTranslucentAlpha});
    return img;
}

menu::BgraImage MakeSyntheticMsgTooltip() {
    menu::BgraImage img(kTooltipWidth, kTooltipHeight,
                        menu::BgraColor{48u, 62u, 88u, 232u});
    // Distinct rounded endcap markers at x = 4 and x = 435 that WipeRectRowMedian
    // on the interior [16..424) must keep intact.
    img.FillRect(menu::RectI{2, 2, 6, 16},
                 menu::BgraColor{95u, 118u, 152u, 210u});
    img.FillRect(menu::RectI{kTooltipWidth - 8, 2, 6, 16},
                 menu::BgraColor{95u, 118u, 152u, 210u});
    img.FillRect(menu::RectI{150, 7, 140, 6},
                 menu::BgraColor{240u, 240u, 240u, 232u});
    return img;
}

menu::BgraImage MakeSyntheticMusicBanner01() {
    menu::BgraImage img(kMusicWidth, kMusicHeight,
                        menu::BgraColor{0u, 0u, 0u, menu::kTransparentAlpha});
    // Pink sakura blossom icon at retail crop RectI{581, 11, 21, 20}.
    img.FillRect(
        menu::RectI{kMusicSakuraCropX + 3, kMusicSakuraCropY + 3, 15, 14},
        menu::BgraColor{172u, 118u, 242u, menu::kOpaqueAlpha});
    return img;
}

Bytes EncodeCompressedSysGrp(const menu::BgraImage& img,
                             int bpp,
                             std::uint8_t reserved_tag) {
    std::array<std::uint8_t, bgi::kSysGrpReservedBytes> reserved{};
    reserved.fill(reserved_tag);
    return bgi::dsc_compress(bgi::encode_sysgrp(img, bpp, reserved));
}

struct SyntheticGameFixture {
    menu::BgraImage orig_dialog;
    Bytes orig_untouched_payload;
    std::array<std::uint8_t, kArc20ExtraFieldBytes> untouched_extra{};
    Bytes orig_sysgrp_arc;
    Bytes orig_splash_arc;
    Bytes orig_choice_arc;
    Bytes orig_script_arc;
};

SyntheticGameFixture PopulateSyntheticGameDir(const fs::path& game_dir,
                                              bool include_second_choice_tsv = true) {
    SyntheticGameFixture fx;
    fx.orig_dialog = MakeSyntheticDialogPlate();

    fx.untouched_extra.fill(0xA7u);
    fx.orig_untouched_payload.resize(kUntouchedPayloadBytes);
    constexpr std::array<std::uint8_t, 4> kEdgeBytes = {0x00u, 0x7Fu, 0x80u, 0xFFu};
    for (std::size_t i = 0u; i < fx.orig_untouched_payload.size(); ++i) {
        fx.orig_untouched_payload[i] =
            (i < kEdgeBytes.size())
                ? kEdgeBytes[i]
                : static_cast<std::uint8_t>((i * 19u + 0x5Du) & 0xFFu);
    }

    std::array<std::uint8_t, kArc20ExtraFieldBytes> sysgrp_extra{};
    sysgrp_extra.fill(0x3Cu);

    fx.orig_sysgrp_arc = BuildArc20WithExtras({
        {"SGTitle990000", sysgrp_extra,
         EncodeCompressedSysGrp(MakeSyntheticTitleBg(), menu::kBmpBpp24, 0x11u)},
        {"SGCG800001", sysgrp_extra,
         EncodeCompressedSysGrp(MakeSyntheticLockedCg(), menu::kBmpBpp24, 0x12u)},
        {"SGUntouchedIcon", fx.untouched_extra, fx.orig_untouched_payload},
        {"SGCnfgWnd010000", sysgrp_extra,
         EncodeCompressedSysGrp(MakeSyntheticToggleStrip(), menu::kBmpBpp32, 0x13u)},
        {"SGCnfgWnd010100", sysgrp_extra,
         EncodeCompressedSysGrp(MakeSyntheticToggleStrip(), menu::kBmpBpp32, 0x14u)},
        {"SGDialog990000", sysgrp_extra,
         EncodeCompressedSysGrp(fx.orig_dialog, menu::kBmpBpp32, 0x15u)},
        {"SGMsgWnd800000", sysgrp_extra,
         EncodeCompressedSysGrp(MakeSyntheticMsgTooltip(), menu::kBmpBpp32, 0x16u)},
        {"SGMusic000100", sysgrp_extra,
         EncodeCompressedSysGrp(MakeSyntheticMusicBanner01(), menu::kBmpBpp32, 0x17u)},
        {"SGMusic000200", sysgrp_extra,
         EncodeCompressedSysGrp(MakeSyntheticMusicBanner01(), menu::kBmpBpp32, 0x18u)},
    });

    fx.orig_splash_arc = BuildArc20WithExtras({
        {"att01", sysgrp_extra,
         MakeSyntheticSplash(/*is_att01=*/true).EncodeBmp(menu::kBmpBpp24)},
        {"att02", sysgrp_extra,
         MakeSyntheticSplash(/*is_att01=*/false).EncodeBmp(menu::kBmpBpp24)},
    });

    const Bytes choice_bmp =
        menu::BgraImage(kChoicePlateWidth, kChoicePlateHeight,
                        menu::BgraColor{255u, 255u, 255u, kChoiceTile0Alpha})
            .EncodeBmp(menu::kBmpBpp32);
    fx.orig_choice_arc = BuildArc20WithExtras({
        {"select01-1", sysgrp_extra, choice_bmp},
        {"select01-2", sysgrp_extra, choice_bmp},
    });

    const Bytes choice_script = BuildChoiceScriptBytecode({
        {"select01-1", "しかし彼女は極めてお人好しでもある。"},
        {"select01-2", "純粋な好奇心からだ。"},
    });
    fx.orig_script_arc = BuildArc20WithExtras({
        {"01_fruhlingsbeginn_01", sysgrp_extra, bgi::dsc_compress(choice_script)},
    });

    snu::write_file((game_dir / "sysgrp.arc").u8string(), fx.orig_sysgrp_arc);
    snu::write_file((game_dir / "data02501.arc").u8string(), fx.orig_splash_arc);
    snu::write_file((game_dir / "data02502.arc").u8string(), fx.orig_choice_arc);
    snu::write_file((game_dir / "data01100.arc").u8string(), fx.orig_script_arc);

    std::string tsv_text =
        "しかし彼女は極めてお人好しでもある。\tBut she's also extremely soft-hearted.\n";
    if (include_second_choice_tsv) {
        tsv_text += "純粋な好奇心からだ。\tJust genuine curiosity.\n";
    }
    const Bytes tsv_bytes(tsv_text.begin(), tsv_text.end());
    snu::write_file((game_dir / "translations.tsv").u8string(), tsv_bytes);
    return fx;
}

std::size_t CountTempFiles(const fs::path& dir) {
    std::size_t count = 0u;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().u8string();
        if (name.size() >= 4u && name.substr(name.size() - 4u) == ".tmp") {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST(MenuPatchComponentTest,
     BuildsMultiArchiveContainerSatisfyingWireObfuscationAndConsumerRoundTrip) {
    ScopedSandbox sandbox;
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path sheets_dir = sandbox.path() / "sheets";
    const fs::path previews_dir = sandbox.path() / "previews";
    const fs::path out_patch = project_dir / "menu_patch.bin";
    fs::create_directories(game_dir);
    fs::create_directories(project_dir);

    const SyntheticGameFixture fx = PopulateSyntheticGameDir(game_dir);

    menu::MenuPatchBuildOptions opts;
    opts.project_dir = project_dir;
    opts.game_dir = game_dir;
    opts.output_patch_path = out_patch;
    opts.contact_sheets_dir = sheets_dir;
    opts.previews_dir = previews_dir;
    opts.strict_retail_inventory = false;

    Bytes emitted_blob;
    const menu::MenuPatchBuildSummary summary =
        menu::BuildMenuPatch(opts, &emitted_blob);

    // 2 splash + 8 localized sysgrp (SGUntouchedIcon excluded) + 2 choices = 12.
    EXPECT_EQ(summary.splash_records, 2u);
    EXPECT_EQ(summary.sysgrp_records, 8u);
    EXPECT_EQ(summary.choice_records, 2u);
    EXPECT_EQ(summary.total_records, 12u);
    EXPECT_NE(summary.manifest_hash, 0u);
    ASSERT_TRUE(fs::is_regular_file(out_patch));
    EXPECT_EQ(snu::read_file(out_patch.u8string()), emitted_blob);
    EXPECT_EQ(summary.patch_bytes, emitted_blob.size());
    EXPECT_EQ(CountTempFiles(sandbox.path()), 0u);

    // Read-only hygiene: synthetic game_dir inputs are byte-for-byte untouched.
    EXPECT_EQ(snu::read_file((game_dir / "sysgrp.arc").u8string()),
              fx.orig_sysgrp_arc);
    EXPECT_EQ(snu::read_file((game_dir / "data02501.arc").u8string()),
              fx.orig_splash_arc);
    EXPECT_EQ(snu::read_file((game_dir / "data02502.arc").u8string()),
              fx.orig_choice_arc);

    // Keyless XOR obfuscation: zero raw asset signatures or >=64 zero runs.
    std::string violation;
    EXPECT_TRUE(menu::VerifyNoRawAssetSignatures(ByteSpan(emitted_blob), &violation))
        << violation;
    EXPECT_NO_THROW(menu::AssertNoRawAssetSignatures(ByteSpan(emitted_blob)));

    // Contact sheets and 1280x720 previews were written and decode cleanly.
    EXPECT_FALSE(summary.contact_sheet_files.empty());
    for (const fs::path& p : summary.contact_sheet_files) {
        ASSERT_TRUE(fs::is_regular_file(p)) << p.u8string();
        const menu::BgraImage sheet = menu::BgraImage::LoadPng(p);
        EXPECT_GT(sheet.width(), 0);
        EXPECT_GT(sheet.height(), 0);
    }
    EXPECT_FALSE(summary.preview_files.empty());
    for (const fs::path& p : summary.preview_files) {
        ASSERT_TRUE(fs::is_regular_file(p)) << p.u8string();
        const menu::BgraImage preview = menu::BgraImage::LoadPng(p);
        EXPECT_EQ(preview.width(), kSplashWidth);
        EXPECT_EQ(preview.height(), kSplashHeight);
    }

    // Parse "SNUMENU1" v1 manifest with the real runtime wire parser.
    MenuPatchManifest manifest{};
    ASSERT_EQ(ParseMenuPatchManifest(ByteSpan(emitted_blob), manifest),
              ManifestStatus::kOk);
    EXPECT_EQ(manifest.header.version, kFormatVersion);
    EXPECT_EQ(manifest.header.record_count, 12u);
    EXPECT_EQ(manifest.header.manifest_hash, summary.manifest_hash);
    ASSERT_EQ(manifest.records.size(), 12u);

    // Producer -> Real Consumer verification: unmask every record against the
    // synthetic source archives, repack sysgrp.arc via RepackArc20Entries, and
    // verify DSC decompression + pixel invariants.
    std::vector<Bytes> unmasked_sysgrp_storage;
    unmasked_sysgrp_storage.reserve(summary.sysgrp_records);
    std::vector<ArcEntryReplacement> sysgrp_replacements;
    sysgrp_replacements.reserve(summary.sysgrp_records);

    std::map<std::string, Bytes> standalone_outputs;

    for (const MenuPatchRecord& rec : manifest.records) {
        SCOPED_TRACE(rec.target_rel_path + "::" + rec.entry_name);
        const Bytes src_arc =
            snu::read_file((game_dir / fs::u8path(rec.source_rel_path)).u8string());
        ASSERT_LE(rec.orig_offset + rec.orig_size, src_arc.size());
        const ByteSpan orig_slice = ByteSpan(src_arc).subspan(
            static_cast<std::size_t>(rec.orig_offset),
            static_cast<std::size_t>(rec.orig_size));
        const ByteSpan xor_payload = ByteSpan(emitted_blob)
                                         .subspan(rec.payload_offset,
                                                  static_cast<std::size_t>(rec.new_size));

        Bytes unmasked;
        ASSERT_EQ(UnmaskRecordPayload(xor_payload, orig_slice, rec.orig_slice_hash,
                                      rec.new_raw_hash, unmasked),
                  UnmaskStatus::kOk);

        if ((rec.flags & kFlagArchiveEntry) != 0u) {
            EXPECT_EQ(rec.source_rel_path, "sysgrp.arc");
            EXPECT_EQ(rec.target_rel_path, "sysgrp.arc");
            unmasked_sysgrp_storage.push_back(std::move(unmasked));
            sysgrp_replacements.push_back(
                ArcEntryReplacement{rec.entry_name,
                                    ByteSpan(unmasked_sysgrp_storage.back())});
        } else {
            EXPECT_EQ(rec.flags, kFlagBackupStandalone);
            standalone_outputs.emplace(rec.target_rel_path, std::move(unmasked));
        }
    }

    ASSERT_EQ(sysgrp_replacements.size(), 8u);
    ASSERT_EQ(standalone_outputs.size(), 4u);

    Bytes repacked_sysgrp;
    ASSERT_EQ(RepackArc20Entries(ByteSpan(fx.orig_sysgrp_arc),
                                 sysgrp_replacements, repacked_sysgrp),
              ArcStatus::kOk);

    // Untouched entry payload and its 24-byte extra metadata [104..128) in index
    // slot #2 must survive RepackArc20Entries byte-for-byte.
    std::uint64_t untouched_off = 0u;
    std::uint64_t untouched_sz = 0u;
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(repacked_sysgrp), "SGUntouchedIcon",
                                  untouched_off, untouched_sz),
              ArcStatus::kOk);
    ASSERT_EQ(untouched_sz, fx.orig_untouched_payload.size());
    EXPECT_EQ(std::memcmp(repacked_sysgrp.data() + untouched_off,
                          fx.orig_untouched_payload.data(),
                          fx.orig_untouched_payload.size()),
              0);
    const std::size_t untouched_extra_pos =
        kArc20HeaderBytes + 2u * kArc20IndexEntryBytes + kArc20ExtraFieldOffset;
    EXPECT_EQ(std::memcmp(repacked_sysgrp.data() + untouched_extra_pos,
                          fx.untouched_extra.data(), kArc20ExtraFieldBytes),
              0);

    // Every repacked sysgrp entry must DSC-decompress and decode cleanly with
    // preserved dimensions, bpp, reserved header bytes, and visual invariants.
    auto read_repacked_sysgrp = [&](std::string_view name) -> bgi::SysGrpImage {
        std::uint64_t off = 0u;
        std::uint64_t sz = 0u;
        EXPECT_EQ(FindArc20EntrySlice(ByteSpan(repacked_sysgrp), name, off, sz),
                  ArcStatus::kOk);
        const Bytes comp(repacked_sysgrp.begin() + static_cast<std::ptrdiff_t>(off),
                         repacked_sysgrp.begin() +
                             static_cast<std::ptrdiff_t>(off + sz));
        EXPECT_TRUE(bgi::is_dsc(comp)) << name;
        return bgi::decode_sysgrp(bgi::dsc_decompress(comp));
    };

    const bgi::SysGrpImage patched_dialog = read_repacked_sysgrp("SGDialog990000");
    EXPECT_EQ(patched_dialog.orig_bpp, menu::kBmpBpp32);
    ASSERT_EQ(patched_dialog.image.width(), kDialogWidth);
    ASSERT_EQ(patched_dialog.image.height(), kDialogHeight);
    EXPECT_EQ(patched_dialog.reserved[0], 0x15u);
    // Orange '!' warning triangle must remain 100% pixel-identical!
    for (int y = kDialogTriangleY; y < kDialogTriangleY + kDialogTriangleH; ++y) {
        for (int x = kDialogTriangleX; x < kDialogTriangleX + kDialogTriangleW; ++x) {
            ASSERT_EQ(patched_dialog.image.PixelAt(x, y),
                      fx.orig_dialog.PixelAt(x, y))
                << "x=" << x << " y=" << y;
        }
    }
    EXPECT_EQ(patched_dialog.image.PixelAt(20, 20).a, kDialogTranslucentAlpha);
    EXPECT_NE(patched_dialog.image, fx.orig_dialog);

    const bgi::SysGrpImage patched_music = read_repacked_sysgrp("SGMusic000100");
    EXPECT_EQ(patched_music.orig_bpp, menu::kBmpBpp32);
    ASSERT_EQ(patched_music.image.width(), kMusicWidth);
    ASSERT_EQ(patched_music.image.height(), kMusicHeight);
    EXPECT_EQ(patched_music.image.PixelAt(0, 0),
              (menu::BgraColor{0u, 0u, 0u, menu::kTransparentAlpha}));
    EXPECT_EQ(patched_music.image.PixelAt(kMusicWidth - 1, kMusicHeight - 1),
              (menu::BgraColor{0u, 0u, 0u, menu::kTransparentAlpha}));
    EXPECT_FALSE(patched_music.image.NonZeroAlphaBounds().empty());

    const bgi::SysGrpImage patched_tooltip = read_repacked_sysgrp("SGMsgWnd800000");
    EXPECT_EQ(patched_tooltip.image.PixelAt(4, 8),
              (menu::BgraColor{95u, 118u, 152u, 210u}));

    // Verify standalone BMP outputs (att01, att02, select01-1, select01-2).
    for (const char* splash_name : {"att01", "att02"}) {
        ASSERT_TRUE(standalone_outputs.count(splash_name) > 0u) << splash_name;
        const Bytes& bmp = standalone_outputs.at(splash_name);
        ASSERT_GE(bmp.size(), 2u);
        EXPECT_EQ(bmp[0], 'B');
        EXPECT_EQ(bmp[1], 'M');
        const menu::BgraImage img = menu::BgraImage::DecodeBmpOrPng(bmp);
        EXPECT_EQ(img.width(), kSplashWidth);
        EXPECT_EQ(img.height(), kSplashHeight);
    }

    for (const char* choice_name : {"select01-1", "select01-2"}) {
        ASSERT_TRUE(standalone_outputs.count(choice_name) > 0u) << choice_name;
        const menu::BgraImage img =
            menu::BgraImage::DecodeBmpOrPng(standalone_outputs.at(choice_name));
        ASSERT_EQ(img.width(), kChoicePlateWidth);
        ASSERT_EQ(img.height(), kChoicePlateHeight);
        EXPECT_EQ(img.PixelAt(4, 4).a, kChoiceTile0Alpha);
        EXPECT_EQ(img.PixelAt(kChoiceTileWidth + 4, 4).a, kChoiceTile1Alpha);
        EXPECT_EQ(img.PixelAt(2 * kChoiceTileWidth + 4, 4).a, kChoiceTile2Alpha);
    }

    // Idempotence & .bak/.backup fallback: replace live archives with repacked /
    // mutated copies while keeping immutable .bak/.backup originals, then
    // rebuild and confirm byte-for-byte identical output.
    snu::write_file((game_dir / "sysgrp.arc.bak").u8string(), fx.orig_sysgrp_arc);
    snu::write_file((game_dir / "sysgrp.arc").u8string(), repacked_sysgrp);
    snu::write_file((game_dir / "data02501.arc.backup").u8string(),
                    fx.orig_splash_arc);
    snu::write_file((game_dir / "data02501.arc").u8string(), Bytes(64u, 0xCCu));

    const fs::path out_patch_2 = project_dir / "menu_patch_2.bin";
    menu::MenuPatchBuildOptions opts_reapply = opts;
    opts_reapply.output_patch_path = out_patch_2;
    opts_reapply.contact_sheets_dir.clear();
    opts_reapply.previews_dir.clear();

    Bytes emitted_blob_2;
    const menu::MenuPatchBuildSummary summary_2 =
        menu::BuildMenuPatch(opts_reapply, &emitted_blob_2);
    EXPECT_EQ(summary_2.manifest_hash, summary.manifest_hash);
    EXPECT_EQ(emitted_blob_2, emitted_blob);
}

TEST(MenuPatchComponentTest,
     RejectsIncompleteInventoryOrMissingChoiceTranslationWithoutPartialOutput) {
    ScopedSandbox sandbox;
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path project_dir = sandbox.path() / "project";
    fs::create_directories(game_dir);
    fs::create_directories(project_dir);

    // Case 1: Missing English translation for select01-2 fails atomically.
    PopulateSyntheticGameDir(game_dir, /*include_second_choice_tsv=*/false);
    const fs::path aborted_out_1 = project_dir / "aborted_missing_tsv.bin";
    menu::MenuPatchBuildOptions missing_tsv_opts;
    missing_tsv_opts.project_dir = project_dir;
    missing_tsv_opts.game_dir = game_dir;
    missing_tsv_opts.output_patch_path = aborted_out_1;
    missing_tsv_opts.strict_retail_inventory = false;

    EXPECT_THROW((void)menu::BuildMenuPatch(missing_tsv_opts),
                 std::runtime_error);
    EXPECT_FALSE(fs::exists(aborted_out_1));
    EXPECT_EQ(CountTempFiles(sandbox.path()), 0u);

    // Case 2: strict_retail_inventory = true on a miniature subset archive
    // fails atomically without writing output_patch_path.
    PopulateSyntheticGameDir(game_dir, /*include_second_choice_tsv=*/true);
    const fs::path aborted_out_2 = project_dir / "aborted_strict_inventory.bin";
    menu::MenuPatchBuildOptions strict_opts;
    strict_opts.project_dir = project_dir;
    strict_opts.game_dir = game_dir;
    strict_opts.output_patch_path = aborted_out_2;
    strict_opts.strict_retail_inventory = true;

    EXPECT_THROW((void)menu::BuildMenuPatch(strict_opts), std::runtime_error);
    EXPECT_FALSE(fs::exists(aborted_out_2));
    EXPECT_EQ(CountTempFiles(sandbox.path()), 0u);
}
