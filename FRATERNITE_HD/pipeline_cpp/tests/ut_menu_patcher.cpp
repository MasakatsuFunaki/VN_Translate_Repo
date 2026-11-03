// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for MenuArchivePatcher and PNG IHDR validation.
#include "menu_patcher/menu_patcher.h"

#include "test_support.h"
#include "yuris/ypf_archive.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

using namespace frat;
using namespace frat::menu;
using namespace frat::yuris;
using frat_test::put_u32;
using frat_test::put_u64;
using frat_test::ScratchDir;
using frat_test::sha256_hex;

namespace {

constexpr std::uint32_t kYpfVersion490 = 490u;
constexpr std::uint8_t kPngEntryType = 2u;
constexpr std::uint8_t kUncompressedFlag = 0u;
constexpr std::size_t kHeaderReservedZeros = 16u;
constexpr std::size_t kPayloadTrailerZeros = 4u;
constexpr std::uint32_t kIhdrChunkSize = 13u;
constexpr std::uint8_t kBitDepth8 = 8u;
constexpr std::uint8_t kColorTypeRgba = 6u;
constexpr std::uint8_t kColorTypeRgb = 2u;
constexpr int kShiftByte1 = 8;
constexpr int kShiftByte2 = 16;
constexpr int kShiftByte3 = 24;

constexpr std::uint32_t kWidthHd = 1280u;
constexpr std::uint32_t kHeightHd = 720u;
constexpr std::uint32_t kWidthBtn = 240u;
constexpr std::uint32_t kHeightBtn = 48u;
constexpr std::uint32_t kWidthWrong = 800u;
constexpr std::uint32_t kHeightWrong = 600u;
constexpr std::size_t kPaddingShort = 16u;
constexpr std::size_t kPaddingLong = 64u;
constexpr std::size_t kExpectedLiveEntryCount = 1483u;
constexpr std::size_t kExpectedLiveTranslatedCount = 507u;
constexpr std::size_t kExpectedLiveUnchangedCount =
    kExpectedLiveEntryCount - kExpectedLiveTranslatedCount;
constexpr std::size_t kExpectedTwoEntries = 2u;
constexpr std::size_t kPngSignatureSize = 8u;
constexpr std::uint8_t kPermutedRawSize = 0xF9u;
constexpr std::uint8_t kFillByteHeader = 0xAAu;
constexpr std::uint8_t kFillByteA = 0x11u;
constexpr std::uint8_t kFillByteB = 0x22u;
constexpr std::uint8_t kFillByteC = 0x33u;
constexpr std::uint8_t kFillByteOrig0 = 0x10u;
constexpr std::uint8_t kFillByteOrig1 = 0x20u;
constexpr std::uint8_t kFillByteRepl0 = 0x99u;

void put_be_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> kShiftByte3));
    b.push_back(static_cast<std::uint8_t>(v >> kShiftByte2));
    b.push_back(static_cast<std::uint8_t>(v >> kShiftByte1));
    b.push_back(static_cast<std::uint8_t>(v));
}

Bytes make_synthetic_png(std::uint32_t width, std::uint32_t height,
                         std::uint8_t color_type, std::size_t extra_bytes,
                         std::uint8_t fill_byte) {
    constexpr std::uint8_t kSig[kPngSignatureSize] = {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    Bytes out(kSig, kSig + sizeof(kSig));
    put_be_u32(out, kIhdrChunkSize);
    for (char c : std::string("IHDR")) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    put_be_u32(out, width);
    put_be_u32(out, height);
    out.push_back(kBitDepth8);
    out.push_back(color_type);
    out.push_back(0);  // compression
    out.push_back(0);  // filter
    out.push_back(0);  // interlace
    put_be_u32(out, 0);  // synthetic CRC
    out.insert(out.end(), extra_bytes, fill_byte);
    return out;
}

struct SyntheticEntrySpec {
    std::string utf8_name;
    std::uint8_t name_size_raw = 0;
    Bytes payload;
    bool share_previous_payload = false;
};

Bytes build_cgsys_like_ypf(const std::vector<SyntheticEntrySpec>& specs) {
    std::vector<Bytes> cp932_names;
    cp932_names.reserve(specs.size());
    std::size_t index_bytes = 0;
    for (const auto& s : specs) {
        const auto cp = utf8_to_cp932_strict(s.utf8_name);
        if (!cp) {
            throw std::runtime_error("failed to encode CP932 name: " + s.utf8_name);
        }
        index_bytes += YPF_ENTRY_PREFIX_SIZE + cp->size() + YPF_ENTRY_FOOTER_SIZE;
        cp932_names.push_back(*cp);
    }

    const std::uint64_t data_start = YPF_HEADER_SIZE + index_bytes;
    Bytes index;
    Bytes payload_section;
    std::uint64_t last_offset = data_start;

    for (std::size_t i = 0; i < specs.size(); ++i) {
        const Bytes& name_cp932 = cp932_names[i];
        const Bytes& blob = specs[i].payload;
        const std::uint8_t raw_len_byte =
            specs[i].name_size_raw != 0
                ? specs[i].name_size_raw
                : static_cast<std::uint8_t>(name_cp932.size() ^ YPF_NAME_XOR_KEY);

        std::uint64_t entry_offset = data_start + payload_section.size();
        if (specs[i].share_previous_payload && i > 0) {
            entry_offset = last_offset;
        } else {
            last_offset = entry_offset;
            payload_section.insert(payload_section.end(), blob.begin(), blob.end());
            payload_section.insert(payload_section.end(), kPayloadTrailerZeros, 0u);
        }

        put_u32(index, ypf_murmur_hash2(name_cp932));
        index.push_back(raw_len_byte);
        for (const std::uint8_t b : name_cp932) {
            index.push_back(static_cast<std::uint8_t>(b ^ YPF_NAME_XOR_KEY));
        }
        index.push_back(kPngEntryType);
        index.push_back(kUncompressedFlag);
        put_u32(index, static_cast<std::uint32_t>(blob.size()));
        put_u32(index, static_cast<std::uint32_t>(blob.size()));
        put_u64(index, entry_offset);
        put_u32(index, ypf_murmur_hash2(blob));
    }

    Bytes out;
    for (char c : std::string("YPF")) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    out.push_back(0);
    put_u32(out, kYpfVersion490);
    put_u32(out, static_cast<std::uint32_t>(specs.size()));
    put_u32(out, static_cast<std::uint32_t>(data_start));
    out.insert(out.end(), kHeaderReservedZeros, 0u);
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), payload_section.begin(), payload_section.end());
    return out;
}

}  // namespace

TEST(MenuPatcher, ParsePngIhdr_ExtractsDimensionsAndColorType) {
    const Bytes png =
        make_synthetic_png(kWidthHd, kHeightHd, kColorTypeRgba, kPaddingShort, kFillByteHeader);
    const PngHeaderInfo info = parse_png_ihdr(png, "test.png");
    EXPECT_EQ(info.width, kWidthHd);
    EXPECT_EQ(info.height, kHeightHd);
    EXPECT_EQ(info.bit_depth, kBitDepth8);
    EXPECT_EQ(info.color_type, kColorTypeRgba);
}

TEST(MenuPatcher, ParsePngIhdr_RejectsInvalidSignatureOrZeroDimensions) {
    const Bytes bad_sig = {'N', 'O', 'T', '_', 'A', '_', 'P', 'N', 'G'};
    EXPECT_THROW(parse_png_ihdr(bad_sig, "bad.png"), std::runtime_error);

    const Bytes zero_dim =
        make_synthetic_png(0u, kHeightHd, kColorTypeRgb, kPaddingShort, kFillByteA);
    EXPECT_THROW(parse_png_ihdr(zero_dim, "zero.png"), std::runtime_error);
}

TEST(MenuPatcher, RepackArchiveBytes_ZeroReplacements_IsByteForByteIdentical) {
    const Bytes png1 =
        make_synthetic_png(kWidthHd, kHeightHd, kColorTypeRgb, kPaddingShort, kFillByteA);
    const Bytes png2 =
        make_synthetic_png(kWidthBtn, kHeightBtn, kColorTypeRgba, kPaddingLong, kFillByteB);
    const Bytes png3 =
        make_synthetic_png(kWidthBtn, kHeightBtn, kColorTypeRgba, kPaddingShort, kFillByteC);

    const std::vector<SyntheticEntrySpec> specs = {
        {"cgsys\\config\\back_easy.png", 0u, png1, false},
        {"cgsys\\clear.png", 0u, png2, false},
        {"cgsys\\dummy.png", 0u, png2, true},
        {u8"cgsys\\extra\\cgmode\\bgv\\\u7D17\u82F1\u5B50_a.png", kPermutedRawSize, png3, false},
    };
    const Bytes original = build_cgsys_like_ypf(specs);

    MenuArchivePatcher patcher;
    PatchSummary summary;
    const std::unordered_map<std::string, Bytes> empty_replacements;
    const Bytes repacked =
        patcher.repack_archive_bytes(original, empty_replacements, &summary);

    EXPECT_EQ(summary.total_entries, specs.size());
    EXPECT_EQ(summary.replaced_entries, 0u);
    EXPECT_EQ(repacked, original);
}

TEST(MenuPatcher, PatchArchive_CreatesBackupReplacesTargetAndIsIdempotent) {
    ScratchDir dir("menu_patch");
    const std::string pac_dir = dir / "game\\pac";
    const std::string assets_dir = dir / "menu_assets\\config_group\\cgsys\\config";
    std::filesystem::create_directories(std::filesystem::u8path(pac_dir));
    std::filesystem::create_directories(std::filesystem::u8path(assets_dir));

    const Bytes orig_png0 =
        make_synthetic_png(kWidthHd, kHeightHd, kColorTypeRgb, kPaddingShort, kFillByteOrig0);
    const Bytes orig_png1 =
        make_synthetic_png(kWidthBtn, kHeightBtn, kColorTypeRgba, kPaddingShort, kFillByteOrig1);
    const Bytes repl_png0 =
        make_synthetic_png(kWidthHd, kHeightHd, kColorTypeRgb, kPaddingLong, kFillByteRepl0);

    const std::vector<SyntheticEntrySpec> specs = {
        {"cgsys\\config\\back_easy.png", 0u, orig_png0, false},
        {"cgsys\\config\\btn_tab_easy_bt3.png", 0u, orig_png1, false},
    };
    const Bytes orig_archive = build_cgsys_like_ypf(specs);
    const std::string ypf_path = pac_dir + "\\cgsys.ypf";
    const std::string bak_path = pac_dir + "\\cgsys.ypf.bak";
    write_file(ypf_path, orig_archive);
    write_file(assets_dir + "\\back_easy.png", repl_png0);

    PatchConfig cfg;
    cfg.game_dir = dir / "game";
    cfg.assets_dir = dir / "menu_assets";

    MenuArchivePatcher patcher;
    const PatchSummary run1 = patcher.patch_archive(cfg);
    EXPECT_TRUE(run1.backup_created);
    EXPECT_EQ(run1.total_entries, kExpectedTwoEntries);
    EXPECT_EQ(run1.replaced_entries, 1u);
    EXPECT_EQ(run1.discovered_assets, 1u);
    EXPECT_EQ(read_file(bak_path), orig_archive);

    const Bytes patched_bytes1 = read_file(ypf_path);
    const auto entries = parse_ypf_index_bytes(patched_bytes1, ypf_path);
    ASSERT_EQ(entries.size(), kExpectedTwoEntries);
    EXPECT_EQ(entries[0].raw_size, repl_png0.size());
    EXPECT_EQ(entries[0].packed_size, repl_png0.size());
    EXPECT_EQ(entries[0].data_hash, ypf_murmur_hash2(repl_png0));
    EXPECT_EQ(read_entry(patched_bytes1, entries[0]), repl_png0);
    EXPECT_EQ(read_entry(patched_bytes1, entries[1]), orig_png1);
    const std::ptrdiff_t size_delta =
        static_cast<std::ptrdiff_t>(repl_png0.size()) -
        static_cast<std::ptrdiff_t>(orig_png0.size());
    const auto orig_entries = parse_ypf_index_bytes(orig_archive, bak_path);
    EXPECT_EQ(static_cast<std::ptrdiff_t>(entries[1].data_offset),
              static_cast<std::ptrdiff_t>(orig_entries[1].data_offset) + size_delta);

    const PatchSummary run2 = patcher.patch_archive(cfg);
    EXPECT_FALSE(run2.backup_created);
    const Bytes patched_bytes2 = read_file(ypf_path);
    EXPECT_EQ(sha256_hex(patched_bytes1.data(), patched_bytes1.size()),
              sha256_hex(patched_bytes2.data(), patched_bytes2.size()));
    EXPECT_EQ(read_file(bak_path), orig_archive);
}

TEST(MenuPatcher, PatchArchive_DimensionMismatchThrowsAndLeavesArchiveUntouched) {
    ScratchDir dir("menu_dim_mismatch");
    const std::string pac_dir = dir / "game\\pac";
    const std::string assets_dir = dir / "menu_assets\\config_group\\cgsys\\config";
    std::filesystem::create_directories(std::filesystem::u8path(pac_dir));
    std::filesystem::create_directories(std::filesystem::u8path(assets_dir));

    const Bytes orig_png =
        make_synthetic_png(kWidthHd, kHeightHd, kColorTypeRgb, kPaddingShort, kFillByteOrig0);
    const Bytes wrong_dim_png =
        make_synthetic_png(kWidthWrong, kHeightWrong, kColorTypeRgb, kPaddingShort, kFillByteOrig1);

    const Bytes orig_archive = build_cgsys_like_ypf({
        {"cgsys\\config\\back_easy.png", 0u, orig_png, false},
    });
    const std::string ypf_path = pac_dir + "\\cgsys.ypf";
    write_file(ypf_path, orig_archive);
    write_file(assets_dir + "\\back_easy.png", wrong_dim_png);

    PatchConfig cfg;
    cfg.game_dir = dir / "game";
    cfg.assets_dir = dir / "menu_assets";

    MenuArchivePatcher patcher;
    EXPECT_THROW(patcher.patch_archive(cfg), std::runtime_error);
    EXPECT_EQ(read_file(ypf_path), orig_archive);
}

TEST(MenuPatcher, PatchArchive_CorruptedReplacementPngThrows) {
    ScratchDir dir("menu_corrupt_png");
    const std::string pac_dir = dir / "game\\pac";
    const std::string assets_dir = dir / "menu_assets\\cgsys\\config";
    std::filesystem::create_directories(std::filesystem::u8path(pac_dir));
    std::filesystem::create_directories(std::filesystem::u8path(assets_dir));

    const Bytes orig_png =
        make_synthetic_png(kWidthHd, kHeightHd, kColorTypeRgb, kPaddingShort, kFillByteOrig0);
    const Bytes orig_archive = build_cgsys_like_ypf({
        {"cgsys\\config\\back_easy.png", 0u, orig_png, false},
    });
    const std::string ypf_path = pac_dir + "\\cgsys.ypf";
    write_file(ypf_path, orig_archive);
    const Bytes corrupted = {'B', 'A', 'D', '_', 'P', 'N', 'G'};
    write_file(assets_dir + "\\back_easy.png", corrupted);

    PatchConfig cfg;
    cfg.game_dir = dir / "game";
    cfg.assets_dir = dir / "menu_assets";

    MenuArchivePatcher patcher;
    EXPECT_THROW(patcher.patch_archive(cfg), std::runtime_error);
    EXPECT_EQ(read_file(ypf_path), orig_archive);
}

TEST(MenuPatcher, LiveArchive_ParsesAll1483EntriesAndIdentityRepacksLosslessly) {
    const std::string live_bak =
        "D:\\Kits\\my_downloadedGames\\Fraternite\\FRATERNITE_HD_DL\\pac\\cgsys.ypf.bak";
    if (!std::filesystem::exists(std::filesystem::u8path(live_bak))) {
        GTEST_SKIP() << "Live cgsys.ypf.bak not present on this machine";
    }
    const Bytes orig = read_file(live_bak);
    const auto entries = parse_ypf_index_bytes(orig, live_bak);
    ASSERT_EQ(entries.size(), kExpectedLiveEntryCount);

    MenuArchivePatcher patcher;
    PatchSummary summary;
    const std::unordered_map<std::string, Bytes> no_replacements;
    const Bytes repacked = patcher.repack_archive_bytes(orig, no_replacements, &summary);
    EXPECT_EQ(summary.total_entries, kExpectedLiveEntryCount);
    EXPECT_EQ(summary.replaced_entries, 0u);
    EXPECT_EQ(repacked, orig);
}

TEST(MenuPatcher, LiveArchive_RepackedArchiveMatchesAll1483EntriesAndReplacementAssets) {
    const std::string tracked_ypf = std::string(FRAT_PROJECT_DIR) + "/pac/cgsys.ypf";
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(tracked_ypf)));

    const Bytes repacked = read_file(tracked_ypf);
    const auto repacked_entries = parse_ypf_index_bytes(repacked, tracked_ypf);
    ASSERT_EQ(repacked_entries.size(), kExpectedLiveEntryCount);

    for (const YpfEntry& re : repacked_entries) {
        const Bytes payload = read_entry(repacked, re);
        const PngHeaderInfo ihdr = parse_png_ihdr(payload, re.name);
        EXPECT_GT(ihdr.width, 0u);
        EXPECT_GT(ihdr.height, 0u);
    }

    MenuArchivePatcher patcher;
    PatchSummary summary;
    const std::unordered_map<std::string, Bytes> no_replacements;
    const Bytes identity_repacked =
        patcher.repack_archive_bytes(repacked, no_replacements, &summary);
    EXPECT_EQ(summary.total_entries, kExpectedLiveEntryCount);
    EXPECT_EQ(summary.replaced_entries, 0u);
    EXPECT_EQ(identity_repacked, repacked);

    const std::string live_bak =
        "D:\\Kits\\my_downloadedGames\\Fraternite\\FRATERNITE_HD_DL\\pac\\cgsys.ypf.bak";
    if (!std::filesystem::exists(std::filesystem::u8path(live_bak))) {
        return;
    }

    const Bytes orig = read_file(live_bak);
    const auto orig_entries = parse_ypf_index_bytes(orig, live_bak);
    ASSERT_EQ(orig_entries.size(), kExpectedLiveEntryCount);

    std::size_t translated_count = 0;
    std::size_t unchanged_count = 0;
    for (std::size_t i = 0; i < kExpectedLiveEntryCount; ++i) {
        const YpfEntry& oe = orig_entries[i];
        const YpfEntry& re = repacked_entries[i];
        EXPECT_EQ(re.name, oe.name);
        EXPECT_EQ(re.name_hash, oe.name_hash);
        EXPECT_EQ(re.name_size_raw, oe.name_size_raw);
        EXPECT_EQ(re.name_bytes_xor, oe.name_bytes_xor);
        EXPECT_EQ(re.type, oe.type);
        EXPECT_EQ(re.compressed, oe.compressed);

        const Bytes orig_payload = read_entry(orig, oe);
        const Bytes repacked_payload = read_entry(repacked, re);
        const PngHeaderInfo orig_ihdr = parse_png_ihdr(orig_payload, oe.name);
        const PngHeaderInfo repacked_ihdr = parse_png_ihdr(repacked_payload, re.name);
        EXPECT_EQ(repacked_ihdr.width, orig_ihdr.width);
        EXPECT_EQ(repacked_ihdr.height, orig_ihdr.height);
        EXPECT_EQ(repacked_ihdr.bit_depth, orig_ihdr.bit_depth);
        EXPECT_EQ(repacked_ihdr.color_type, orig_ihdr.color_type);

        if (repacked_payload != orig_payload) {
            ++translated_count;
            continue;
        }
        ++unchanged_count;
    }
    EXPECT_EQ(translated_count, kExpectedLiveTranslatedCount);
    EXPECT_EQ(unchanged_count, kExpectedLiveUnchangedCount);
}
