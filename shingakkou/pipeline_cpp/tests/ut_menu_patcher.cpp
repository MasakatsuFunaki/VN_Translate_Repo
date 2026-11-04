// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for TGA 24/32-bit RAW/RLE codec, SHINMENU v1 keyless XOR-delta
// patcher, and MenuArchivePatcher DDP2 validation, backup creation, untouched
// entry preservation, and idempotency.

#include "menu/menu_archive_patcher.h"

#include "common/util.h"
#include "ddp/ddp_archive.h"

#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace shin::menu {
namespace {

namespace fs = std::filesystem;

constexpr std::uint16_t kTestImageWidth = 8;
constexpr std::uint16_t kTestImageHeight = 4;
constexpr std::uint16_t kMismatchedWidth = 12;
constexpr std::uint8_t kDescriptorBottomUp = 0x00U;
constexpr std::uint8_t kDescriptorTopDown = 0x20U;
constexpr std::uint8_t kAlphaEightBits = 0x08U;
constexpr std::size_t kBytesPerPixel24 = 3;
constexpr std::size_t kBytesPerPixel32 = 4;

constexpr std::size_t kDdp2HeaderBytes = 32;
constexpr std::size_t kDdp2EntryRecordBytes = 16;
constexpr std::size_t kSyntheticSinSysdEntryCount = 3;
constexpr std::size_t kSyntheticBootupEntryCount = 2;
constexpr std::size_t kEntryIndexZero = 0;
constexpr std::size_t kEntryIndexOne = 1;
constexpr std::size_t kEntryIndexTwo = 2;

constexpr std::uint8_t kSampleBlue = 0x12U;
constexpr std::uint8_t kSampleGreen = 0x34U;
constexpr std::uint8_t kSampleRed = 0x56U;
constexpr std::uint8_t kSampleAlpha = 0xE0U;
constexpr std::uint8_t kPatchedBlue = 0x9AU;
constexpr std::uint8_t kPatchedGreen = 0xBCU;
constexpr std::uint8_t kPatchedRed = 0xDEU;
constexpr std::size_t kRepeatedPixelBandEnd = 4;
constexpr std::uint8_t kColorStep = 7U;

constexpr std::size_t kByteOffset1 = 1;
constexpr std::size_t kByteOffset2 = 2;
constexpr std::size_t kByteOffset3 = 3;
constexpr std::size_t kByteShift8 = 8;
constexpr std::size_t kByteShift16 = 16;
constexpr std::size_t kByteShift24 = 24;
constexpr std::uint32_t kByteMask = 0xFFU;

constexpr std::size_t kHxbHeaderSize = 16;
constexpr std::size_t kHxbLengthHighIdx = 8;
constexpr std::size_t kHxbLengthMidIdx = 9;
constexpr std::size_t kHxbLengthLowIdx = 10;
constexpr std::uint8_t kHxbSampleOpcodeA = 0x46U;
constexpr std::uint8_t kHxbSampleOpcodeB = 0x80U;

constexpr std::size_t kPatchVersionOffset = 8;
constexpr std::size_t kPatchSinSysdTotalOffset = 12;
constexpr std::size_t kPatchSinSysdOrigSizeOffset = 16;
constexpr std::size_t kPatchSinSysdCountOffset = 20;
constexpr std::size_t kPatchBootupTotalOffset = 24;
constexpr std::size_t kPatchBootupOrigSizeOffset = 28;
constexpr std::size_t kPatchBootupCountOffset = 32;
constexpr std::size_t kFirstRecordOrigHashOffset = 44;
constexpr std::uint8_t kTamperFlipMask = 0xFFU;

constexpr std::uint32_t kExpectedLiveSinSysdTotal = 229;
constexpr std::uint32_t kExpectedLiveSinSysdOrigSize = 13486629;
constexpr std::uint32_t kExpectedLiveSinSysdPatchCount = 37;
constexpr std::uint32_t kExpectedLiveBootupTotal = 18;
constexpr std::uint32_t kExpectedLiveBootupOrigSize = 48447;
constexpr std::uint32_t kExpectedLiveBootupPatchCount = 9;
constexpr std::size_t kExpectedLiveSinSysdPatchedSize = 20928356;
constexpr std::size_t kExpectedLiveBootupPatchedSize = 58961;
constexpr const char* kExpectedLiveSinSysdSha256 =
    "ba84988290db3c5341dcfb2fc70e1087b4947c75b807ef3a629d1ea4e4587417";
constexpr const char* kExpectedLiveBootupSha256 =
    "3810178413ab31c8d1355d2b9d84b6e3ecaf69a81924bcda1dd65a671e2a4977";
constexpr std::size_t kHexCharsPerByte = 2;

static void AppendU32Le(Bytes& dst, std::uint32_t value) {
    dst.push_back(static_cast<std::uint8_t>(value & kByteMask));
    dst.push_back(static_cast<std::uint8_t>((value >> kByteShift8) & kByteMask));
    dst.push_back(static_cast<std::uint8_t>((value >> kByteShift16) & kByteMask));
    dst.push_back(static_cast<std::uint8_t>((value >> kByteShift24) & kByteMask));
}

static std::uint32_t ReadU32LeAt(const Bytes& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + kByteOffset1]) << kByteShift8) |
           (static_cast<std::uint32_t>(data[offset + kByteOffset2]) << kByteShift16) |
           (static_cast<std::uint32_t>(data[offset + kByteOffset3]) << kByteShift24);
}

static bool ContainsAsciiSubstr(const Bytes& haystack, const std::string& needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
           haystack.end();
}

static std::string ComputeSha256Hex(const Bytes& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    SHA256(data.data(), data.size(), digest);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * kHexCharsPerByte);
    for (std::size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        char buf[kHexCharsPerByte + 1] = {};
        std::snprintf(buf, sizeof(buf), "%02x", digest[i]);
        hex += buf;
    }
    return hex;
}

static Bytes BuildDdp2ArchiveFromPayloads(const std::vector<Bytes>& raw_payloads) {
    Bytes archive;
    archive.push_back(static_cast<std::uint8_t>('D'));
    archive.push_back(static_cast<std::uint8_t>('D'));
    archive.push_back(static_cast<std::uint8_t>('P'));
    archive.push_back(static_cast<std::uint8_t>('2'));
    AppendU32Le(archive, static_cast<std::uint32_t>(raw_payloads.size()));
    while (archive.size() < kDdp2HeaderBytes) {
        archive.push_back(0);
    }

    std::vector<Bytes> packed_payloads;
    packed_payloads.reserve(raw_payloads.size());
    for (const Bytes& raw : raw_payloads) {
        packed_payloads.push_back(ddp::shs_wrap(raw));
    }

    std::uint32_t current_offset = static_cast<std::uint32_t>(
        kDdp2HeaderBytes + raw_payloads.size() * kDdp2EntryRecordBytes);
    for (std::size_t i = 0; i < raw_payloads.size(); ++i) {
        AppendU32Le(archive, current_offset);
        AppendU32Le(archive, static_cast<std::uint32_t>(raw_payloads[i].size()));
        AppendU32Le(archive, static_cast<std::uint32_t>(packed_payloads[i].size()));
        AppendU32Le(archive, 0);
        current_offset += static_cast<std::uint32_t>(packed_payloads[i].size());
    }

    for (const Bytes& packed : packed_payloads) {
        archive.insert(archive.end(), packed.begin(), packed.end());
    }
    return archive;
}

static Bytes MakeSamplePixels24(std::uint16_t width,
                                std::uint16_t height,
                                std::uint8_t base_blue,
                                std::uint8_t base_green,
                                std::uint8_t base_red) {
    Bytes pixels;
    pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                   kBytesPerPixel24);
    for (std::uint16_t y = 0; y < height; ++y) {
        for (std::uint16_t x = 0; x < width; ++x) {
            if (x < kRepeatedPixelBandEnd) {
                pixels.push_back(base_blue);
                pixels.push_back(base_green);
                pixels.push_back(base_red);
                continue;
            }
            const std::uint8_t delta = static_cast<std::uint8_t>((x + y) * kColorStep);
            pixels.push_back(static_cast<std::uint8_t>(base_blue + delta));
            pixels.push_back(static_cast<std::uint8_t>(base_green + delta));
            pixels.push_back(static_cast<std::uint8_t>(base_red + delta));
        }
    }
    return pixels;
}

static Bytes MakeSamplePixels32(std::uint16_t width,
                                std::uint16_t height,
                                std::uint8_t base_blue,
                                std::uint8_t base_green,
                                std::uint8_t base_red,
                                std::uint8_t base_alpha) {
    Bytes pixels;
    pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                   kBytesPerPixel32);
    for (std::uint16_t y = 0; y < height; ++y) {
        for (std::uint16_t x = 0; x < width; ++x) {
            if (x < kRepeatedPixelBandEnd) {
                pixels.push_back(base_blue);
                pixels.push_back(base_green);
                pixels.push_back(base_red);
                pixels.push_back(base_alpha);
                continue;
            }
            const std::uint8_t delta = static_cast<std::uint8_t>((x + y) * kColorStep);
            pixels.push_back(static_cast<std::uint8_t>(base_blue + delta));
            pixels.push_back(static_cast<std::uint8_t>(base_green + delta));
            pixels.push_back(static_cast<std::uint8_t>(base_red + delta));
            pixels.push_back(base_alpha);
        }
    }
    return pixels;
}

static TgaHeaderInfo MakeTgaHeader(std::uint8_t image_type,
                                   std::uint16_t width,
                                   std::uint16_t height,
                                   std::uint8_t bpp,
                                   std::uint8_t descriptor) {
    TgaHeaderInfo header;
    header.image_type = image_type;
    header.width = width;
    header.height = height;
    header.bpp = bpp;
    header.descriptor = descriptor;
    return header;
}

static Bytes MakeDecryptedHxbSample(std::uint8_t opcode_byte) {
    Bytes hxb(kHxbHeaderSize, 0);
    const char magic[] = "DDWuHXB";
    for (std::size_t i = 0; i < sizeof(magic) - 1; ++i) {
        hxb[i] = static_cast<std::uint8_t>(magic[i]);
    }
    hxb.push_back(opcode_byte);
    hxb.push_back(kHxbSampleOpcodeB);
    const std::size_t total_len = hxb.size();
    hxb[kHxbLengthHighIdx] = static_cast<std::uint8_t>((total_len >> kByteShift16) & kByteMask);
    hxb[kHxbLengthMidIdx] = static_cast<std::uint8_t>((total_len >> kByteShift8) & kByteMask);
    hxb[kHxbLengthLowIdx] = static_cast<std::uint8_t>(total_len & kByteMask);
    return hxb;
}

class TempTestDirectory {
public:
    explicit TempTestDirectory(const std::string& suffix)
        : path_(fs::temp_directory_path() / ("shin_ut_menu_patcher_" + suffix)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
    }

    TempTestDirectory(const TempTestDirectory&) = delete;
    TempTestDirectory& operator=(const TempTestDirectory&) = delete;

    ~TempTestDirectory() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

TEST(TgaCodecTest, RoundTrips24BitUncompressedAndRle) {
    const Bytes expected_pixels = MakeSamplePixels24(
        kTestImageWidth, kTestImageHeight, kSampleBlue, kSampleGreen, kSampleRed);

    const TgaHeaderInfo raw_header = MakeTgaHeader(kTgaImageTypeUncompressedTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth24,
                                                   kDescriptorBottomUp);
    const Bytes encoded_raw = EncodeTga(raw_header, expected_pixels);
    const TgaHeaderInfo parsed_raw = ReadTgaInfo(encoded_raw);
    EXPECT_EQ(parsed_raw.image_type, kTgaImageTypeUncompressedTrueColor);
    EXPECT_EQ(parsed_raw.width, kTestImageWidth);
    EXPECT_EQ(parsed_raw.height, kTestImageHeight);
    EXPECT_EQ(parsed_raw.bpp, kTgaBitDepth24);
    EXPECT_EQ(DecodeTgaPixels(encoded_raw), expected_pixels);

    const TgaHeaderInfo rle_header = MakeTgaHeader(kTgaImageTypeRleTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth24,
                                                   kDescriptorBottomUp);
    const Bytes encoded_rle = EncodeTga(rle_header, expected_pixels);
    const TgaHeaderInfo parsed_rle = ReadTgaHeader(encoded_rle);
    EXPECT_EQ(parsed_rle.image_type, kTgaImageTypeRleTrueColor);
    EXPECT_EQ(DecodeTgaPixels(encoded_rle), expected_pixels);
}

TEST(TgaCodecTest, RoundTrips32BitUncompressedAndRle) {
    const Bytes expected_pixels = MakeSamplePixels32(kTestImageWidth,
                                                     kTestImageHeight,
                                                     kSampleBlue,
                                                     kSampleGreen,
                                                     kSampleRed,
                                                     kSampleAlpha);

    const std::uint8_t top_down_alpha_desc =
        static_cast<std::uint8_t>(kDescriptorTopDown | kAlphaEightBits);
    const TgaHeaderInfo raw_header = MakeTgaHeader(kTgaImageTypeUncompressedTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth32,
                                                   top_down_alpha_desc);
    const Bytes encoded_raw = EncodeTga(raw_header, expected_pixels);
    EXPECT_EQ(DecodeTgaPixels(encoded_raw), expected_pixels);

    const TgaHeaderInfo rle_header = MakeTgaHeader(kTgaImageTypeRleTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth32,
                                                   top_down_alpha_desc);
    const Bytes encoded_rle = EncodeTga(rle_header, expected_pixels);
    const TgaHeaderInfo parsed_rle = ReadTgaHeader(encoded_rle);
    EXPECT_EQ(parsed_rle.descriptor, top_down_alpha_desc);
    EXPECT_EQ(DecodeTgaPixels(encoded_rle), expected_pixels);
}

TEST(MenuArchivePatcherTest, CreatesBackupsPatchesEntriesPreservesUntouchedAndIsIdempotent) {
    TempTestDirectory temp_dir("patch_and_idempotency");
    const fs::path game_dir = temp_dir.path() / "game";
    const fs::path data_dir = game_dir / "Data";
    const fs::path assets_dir = temp_dir.path() / "staged_assets";
    fs::create_directories(data_dir);
    fs::create_directories(assets_dir / "02a_config");
    fs::create_directories(assets_dir / "02d_tables_and_bootup");

    const TgaHeaderInfo tga_header = MakeTgaHeader(kTgaImageTypeRleTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth24,
                                                   kDescriptorBottomUp);
    const Bytes orig_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kSampleBlue, kSampleGreen, kSampleRed));
    const Bytes untouched_entry = {'B', 'M', 'U', 'N', 'T', 'O', 'U', 'C', 'H', 'E', 'D'};
    const Bytes orig_csv = {'1', ',', '"', 'J', 'P', '"', '\r', '\n'};

    const fs::path sin_sysd_path = data_dir / "sin_sysd.dat";
    const Bytes orig_sin_sysd_archive =
        BuildDdp2ArchiveFromPayloads({orig_tga, untouched_entry, orig_csv});
    write_file(sin_sysd_path.u8string(), orig_sin_sysd_archive);

    const Bytes orig_bootup_hxb = ddp::decrypt_hxb(MakeDecryptedHxbSample(kHxbSampleOpcodeA));
    const Bytes orig_bootup_cfg = {'T', ':', 'O', 'R', 'I', 'G', '\r', '\n'};
    const fs::path bootup_path = game_dir / "Bootup.dat";
    const Bytes orig_bootup_archive =
        BuildDdp2ArchiveFromPayloads({orig_bootup_hxb, orig_bootup_cfg});
    write_file(bootup_path.u8string(), orig_bootup_archive);

    const Bytes replacement_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kPatchedBlue, kPatchedGreen, kPatchedRed));
    const Bytes replacement_csv = {'1', ',', '"', 'E', 'N', '"', '\r', '\n'};
    const Bytes replacement_bootup_hxb_plain = MakeDecryptedHxbSample(kHxbSampleOpcodeB);
    const Bytes replacement_bootup_cfg = {'T', ':', 'E', 'N', 'G', '\r', '\n'};

    write_file((assets_dir / "02a_config" / "sin_sysd_00000.tga").u8string(), replacement_tga);
    write_file((assets_dir / "02d_tables_and_bootup" / "sin_sysd_00002.csv").u8string(),
               replacement_csv);
    write_file((assets_dir / "02d_tables_and_bootup" / "bootup_00000.hxb").u8string(),
               replacement_bootup_hxb_plain);
    write_file((assets_dir / "02d_tables_and_bootup" / "bootup_00001.bin").u8string(),
               replacement_bootup_cfg);

    MenuPatchOptions opts;
    opts.game_dir = game_dir.u8string();
    opts.assets_dir = assets_dir.u8string();
    opts.verify = true;

    MenuArchivePatcher patcher_run_one(opts);
    const MenuPatchStats stats_one = patcher_run_one.PatchArchives();
    EXPECT_EQ(stats_one.sin_sysd_total_entries, kSyntheticSinSysdEntryCount);
    EXPECT_EQ(stats_one.sin_sysd_patched_tga_count, kEntryIndexOne);
    EXPECT_EQ(stats_one.sin_sysd_patched_csv_count, kEntryIndexOne);
    EXPECT_EQ(stats_one.bootup_total_entries, kSyntheticBootupEntryCount);
    EXPECT_EQ(stats_one.bootup_patched_count, kSyntheticBootupEntryCount);
    EXPECT_TRUE(stats_one.verified);

    const fs::path sin_sysd_bak_path = data_dir / "sin_sysd.dat.bak";
    const fs::path bootup_bak_path = game_dir / "Bootup.dat.bak";
    ASSERT_TRUE(fs::exists(sin_sysd_bak_path));
    ASSERT_TRUE(fs::exists(bootup_bak_path));
    EXPECT_EQ(read_file(sin_sysd_bak_path.u8string()), orig_sin_sysd_archive);
    EXPECT_EQ(read_file(bootup_bak_path.u8string()), orig_bootup_archive);

    const Bytes sin_sysd_after_run_one = read_file(sin_sysd_path.u8string());
    const Bytes bootup_after_run_one = read_file(bootup_path.u8string());

    MenuArchivePatcher patcher_run_two(opts);
    const MenuPatchStats stats_two = patcher_run_two.PatchArchives();
    EXPECT_TRUE(stats_two.verified);
    EXPECT_EQ(read_file(sin_sysd_path.u8string()), sin_sysd_after_run_one);
    EXPECT_EQ(read_file(bootup_path.u8string()), bootup_after_run_one);
}

TEST(MenuArchivePatcherTest, RejectsMismatchedTgaDimensionsOrBitDepthWithoutCorruptingArchive) {
    TempTestDirectory temp_dir("reject_mismatch");
    const fs::path game_dir = temp_dir.path() / "game";
    const fs::path data_dir = game_dir / "Data";
    const fs::path assets_dir = temp_dir.path() / "staged_assets";
    fs::create_directories(data_dir);
    fs::create_directories(assets_dir);

    const TgaHeaderInfo orig_header = MakeTgaHeader(kTgaImageTypeUncompressedTrueColor,
                                                    kTestImageWidth,
                                                    kTestImageHeight,
                                                    kTgaBitDepth24,
                                                    kDescriptorBottomUp);
    const Bytes orig_tga = EncodeTga(
        orig_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kSampleBlue, kSampleGreen, kSampleRed));
    const fs::path sin_sysd_path = data_dir / "sin_sysd.dat";
    const Bytes orig_archive = BuildDdp2ArchiveFromPayloads({orig_tga});
    write_file(sin_sysd_path.u8string(), orig_archive);

    const TgaHeaderInfo wrong_width_header = MakeTgaHeader(kTgaImageTypeUncompressedTrueColor,
                                                           kMismatchedWidth,
                                                           kTestImageHeight,
                                                           kTgaBitDepth24,
                                                           kDescriptorBottomUp);
    const Bytes wrong_width_tga = EncodeTga(
        wrong_width_header,
        MakeSamplePixels24(
            kMismatchedWidth, kTestImageHeight, kPatchedBlue, kPatchedGreen, kPatchedRed));
    const fs::path asset_file = assets_dir / "sin_sysd_00000.tga";
    write_file(asset_file.u8string(), wrong_width_tga);

    MenuPatchOptions opts;
    opts.game_dir = game_dir.u8string();
    opts.assets_dir = assets_dir.u8string();
    opts.verify = true;

    MenuArchivePatcher patcher_wrong_width(opts);
    EXPECT_THROW(patcher_wrong_width.PatchArchives(), std::runtime_error);
    EXPECT_EQ(read_file(sin_sysd_path.u8string()), orig_archive);

    const TgaHeaderInfo wrong_bpp_header = MakeTgaHeader(kTgaImageTypeUncompressedTrueColor,
                                                         kTestImageWidth,
                                                         kTestImageHeight,
                                                         kTgaBitDepth32,
                                                         kDescriptorBottomUp);
    const Bytes wrong_bpp_tga = EncodeTga(wrong_bpp_header,
                                          MakeSamplePixels32(kTestImageWidth,
                                                             kTestImageHeight,
                                                             kPatchedBlue,
                                                             kPatchedGreen,
                                                             kPatchedRed,
                                                             kSampleAlpha));
    write_file(asset_file.u8string(), wrong_bpp_tga);

    MenuArchivePatcher patcher_wrong_bpp(opts);
    EXPECT_THROW(patcher_wrong_bpp.PatchArchives(), std::runtime_error);
    EXPECT_EQ(read_file(sin_sysd_path.u8string()), orig_archive);
}

TEST(XorMenuPatchTest, BuildsAndAppliesXorPatchBlobWithZeroPlaintextHeaders) {
    const TgaHeaderInfo tga_header = MakeTgaHeader(kTgaImageTypeRleTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth24,
                                                   kDescriptorBottomUp);
    const Bytes orig_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kSampleBlue, kSampleGreen, kSampleRed));
    const Bytes untouched_entry = {'B', 'M', 'U', 'N', 'T', 'O', 'U', 'C', 'H', 'E', 'D'};
    const Bytes orig_csv = {'1', ',', '"', 'J', 'P', '"', '\r', '\n'};
    const Bytes orig_sin_sysd =
        BuildDdp2ArchiveFromPayloads({orig_tga, untouched_entry, orig_csv});

    const Bytes orig_bootup_hxb = ddp::decrypt_hxb(MakeDecryptedHxbSample(kHxbSampleOpcodeA));
    const Bytes orig_bootup_cfg = {'T', ':', 'O', 'R', 'I', 'G', '\r', '\n'};
    const Bytes orig_bootup =
        BuildDdp2ArchiveFromPayloads({orig_bootup_hxb, orig_bootup_cfg});

    const Bytes repl_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kPatchedBlue, kPatchedGreen, kPatchedRed));
    const Bytes repl_csv = {'r', 'e', 'p', '_', '0', '1', '_', '0', '1', '\r', '\n'};
    const Bytes repl_bootup_hxb = ddp::decrypt_hxb(MakeDecryptedHxbSample(kHxbSampleOpcodeB));
    const Bytes repl_bootup_cfg = {
        'T', ':', 'S', 'h', 'i', 'n', 'g', 'a', 'k', 'k', 'o', 'u', '\r', '\n'};

    const std::map<std::size_t, Bytes> sin_sysd_replacements = {
        {kEntryIndexZero, repl_tga},
        {kEntryIndexTwo, repl_csv},
    };
    const std::map<std::size_t, Bytes> bootup_replacements = {
        {kEntryIndexZero, repl_bootup_hxb},
        {kEntryIndexOne, repl_bootup_cfg},
    };

    const Bytes patch_blob = BuildXorMenuPatchBlob(
        orig_sin_sysd, sin_sysd_replacements, orig_bootup, bootup_replacements);
    ASSERT_GE(patch_blob.size(), kXorMenuPatchHeaderSize);
    EXPECT_EQ(std::memcmp(patch_blob.data(), "SHINMENU", kXorMenuPatchMagicSize), 0);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchVersionOffset), kXorMenuPatchFormatVersion);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchSinSysdTotalOffset), kSyntheticSinSysdEntryCount);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchSinSysdOrigSizeOffset), orig_sin_sysd.size());
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchSinSysdCountOffset), sin_sysd_replacements.size());
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchBootupTotalOffset), kSyntheticBootupEntryCount);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchBootupOrigSizeOffset), orig_bootup.size());
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchBootupCountOffset), bootup_replacements.size());

    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "DDWuHXB"));
    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "T:Shingakkou"));
    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "rep_01_01"));

    Bytes patched_sin_sysd;
    Bytes patched_bootup;
    ApplyXorMenuPatchBlobToDdp2(
        orig_sin_sysd, orig_bootup, patch_blob, patched_sin_sysd, patched_bootup);

    const Bytes expected_sin_sysd =
        BuildDdp2ArchiveFromPayloads({repl_tga, untouched_entry, repl_csv});
    const Bytes expected_bootup =
        BuildDdp2ArchiveFromPayloads({repl_bootup_hxb, repl_bootup_cfg});
    EXPECT_EQ(patched_sin_sysd, expected_sin_sysd);
    EXPECT_EQ(patched_bootup, expected_bootup);
}

TEST(XorMenuPatchTest, RejectsTamperedOrigHashOrCorruptedXorPayload) {
    const TgaHeaderInfo tga_header = MakeTgaHeader(kTgaImageTypeUncompressedTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth24,
                                                   kDescriptorBottomUp);
    const Bytes orig_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kSampleBlue, kSampleGreen, kSampleRed));
    const Bytes repl_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kPatchedBlue, kPatchedGreen, kPatchedRed));
    const Bytes orig_sin_sysd = BuildDdp2ArchiveFromPayloads({orig_tga});
    const Bytes orig_bootup;

    const std::map<std::size_t, Bytes> sin_sysd_replacements = {
        {kEntryIndexZero, repl_tga},
    };
    const std::map<std::size_t, Bytes> bootup_replacements;
    const Bytes valid_patch = BuildXorMenuPatchBlob(
        orig_sin_sysd, sin_sysd_replacements, orig_bootup, bootup_replacements);

    Bytes tampered_orig_hash = valid_patch;
    tampered_orig_hash[kFirstRecordOrigHashOffset] ^= kTamperFlipMask;
    Bytes out_sin_sysd;
    Bytes out_bootup;
    EXPECT_THROW(ApplyXorMenuPatchBlobToDdp2(
                     orig_sin_sysd, orig_bootup, tampered_orig_hash, out_sin_sysd, out_bootup),
                 std::runtime_error);

    Bytes tampered_payload = valid_patch;
    tampered_payload.back() ^= kTamperFlipMask;
    EXPECT_THROW(ApplyXorMenuPatchBlobToDdp2(
                     orig_sin_sysd, orig_bootup, tampered_payload, out_sin_sysd, out_bootup),
                 std::runtime_error);
}

TEST(XorMenuPatchTest, PatchesArchivesFromXorPatchFileWhenAssetsDirectoryIsAbsent) {
    TempTestDirectory temp_dir("xor_patch_file_mode");
    const fs::path game_dir = temp_dir.path() / "game";
    const fs::path data_dir = game_dir / "Data";
    fs::create_directories(data_dir);

    const TgaHeaderInfo tga_header = MakeTgaHeader(kTgaImageTypeRleTrueColor,
                                                   kTestImageWidth,
                                                   kTestImageHeight,
                                                   kTgaBitDepth24,
                                                   kDescriptorBottomUp);
    const Bytes orig_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kSampleBlue, kSampleGreen, kSampleRed));
    const Bytes orig_csv = {'1', ',', '"', 'J', 'P', '"', '\r', '\n'};
    const Bytes orig_sin_sysd = BuildDdp2ArchiveFromPayloads({orig_tga, orig_csv});

    const Bytes orig_bootup_cfg = {'T', ':', 'O', 'R', 'I', 'G', '\r', '\n'};
    const Bytes orig_bootup = BuildDdp2ArchiveFromPayloads({orig_bootup_cfg});

    const Bytes repl_tga = EncodeTga(
        tga_header,
        MakeSamplePixels24(
            kTestImageWidth, kTestImageHeight, kPatchedBlue, kPatchedGreen, kPatchedRed));
    const Bytes repl_csv = {'1', ',', '"', 'E', 'N', '"', '\r', '\n'};
    const Bytes repl_bootup_cfg = {'T', ':', 'E', 'N', 'G', '\r', '\n'};

    const Bytes patch_blob = BuildXorMenuPatchBlob(
        orig_sin_sysd,
        {{kEntryIndexZero, repl_tga}, {kEntryIndexOne, repl_csv}},
        orig_bootup,
        {{kEntryIndexZero, repl_bootup_cfg}});

    const fs::path sin_sysd_path = data_dir / "sin_sysd.dat";
    const fs::path bootup_path = game_dir / "Bootup.dat";
    const fs::path patch_path = temp_dir.path() / "menu_patch.bin";
    write_file(sin_sysd_path.u8string(), orig_sin_sysd);
    write_file(bootup_path.u8string(), orig_bootup);
    write_file(patch_path.u8string(), patch_blob);

    MenuPatchOptions opts;
    opts.game_dir = game_dir.u8string();
    opts.assets_dir = (temp_dir.path() / "nonexistent_assets").u8string();
    opts.patch_file = patch_path.u8string();
    opts.verify = true;

    MenuArchivePatcher patcher(opts);
    const MenuPatchStats stats = patcher.PatchArchives();
    EXPECT_EQ(stats.sin_sysd_total_entries, kSyntheticBootupEntryCount);
    EXPECT_EQ(stats.sin_sysd_patched_tga_count, kEntryIndexOne);
    EXPECT_EQ(stats.sin_sysd_patched_csv_count, kEntryIndexOne);
    EXPECT_EQ(stats.bootup_total_entries, kEntryIndexOne);
    EXPECT_EQ(stats.bootup_patched_count, kEntryIndexOne);
    EXPECT_TRUE(stats.verified);

    EXPECT_EQ(read_file((data_dir / "sin_sysd.dat.bak").u8string()), orig_sin_sysd);
    EXPECT_EQ(read_file((game_dir / "Bootup.dat.bak").u8string()), orig_bootup);
    EXPECT_EQ(read_file(sin_sysd_path.u8string()),
              BuildDdp2ArchiveFromPayloads({repl_tga, repl_csv}));
    EXPECT_EQ(read_file(bootup_path.u8string()),
              BuildDdp2ArchiveFromPayloads({repl_bootup_cfg}));
}

TEST(XorMenuPatchTest, LiveMenuPatchBinHasZeroPlainHeadersAndReconstructsBitExactArchives) {
    const fs::path project_root = fs::u8path(SHIN_PROJECT_DIR);
    const fs::path tracked_patch_path = project_root / "menu_patch.bin";
    ASSERT_TRUE(fs::exists(tracked_patch_path));
    EXPECT_FALSE(fs::exists(project_root / "menu_assets"));

    const Bytes patch_blob = read_file(tracked_patch_path.u8string());
    ASSERT_GE(patch_blob.size(), kXorMenuPatchHeaderSize);
    EXPECT_EQ(std::memcmp(patch_blob.data(), "SHINMENU", kXorMenuPatchMagicSize), 0);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchVersionOffset), kXorMenuPatchFormatVersion);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchSinSysdTotalOffset), kExpectedLiveSinSysdTotal);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchSinSysdOrigSizeOffset), kExpectedLiveSinSysdOrigSize);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchSinSysdCountOffset), kExpectedLiveSinSysdPatchCount);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchBootupTotalOffset), kExpectedLiveBootupTotal);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchBootupOrigSizeOffset), kExpectedLiveBootupOrigSize);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchBootupCountOffset), kExpectedLiveBootupPatchCount);

    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "DDWuHXB"));
    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "T:Shingakkou"));
    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "rep_01_01"));

    const fs::path live_game_root =
        fs::u8path("d:\\Kits\\my_downloadedGames\\Shingakkou + FD\\Shingakkou");
    const fs::path live_sin_sysd_bak = live_game_root / "Data" / "sin_sysd.dat.bak";
    const fs::path live_bootup_bak = live_game_root / "Bootup.dat.bak";
    if (!fs::exists(live_sin_sysd_bak) || !fs::exists(live_bootup_bak)) {
        return;
    }

    const Bytes orig_sin_sysd = read_file(live_sin_sysd_bak.u8string());
    const Bytes orig_bootup = read_file(live_bootup_bak.u8string());
    Bytes patched_sin_sysd;
    Bytes patched_bootup;
    ApplyXorMenuPatchBlobToDdp2(
        orig_sin_sysd, orig_bootup, patch_blob, patched_sin_sysd, patched_bootup);

    EXPECT_EQ(patched_sin_sysd.size(), kExpectedLiveSinSysdPatchedSize);
    EXPECT_EQ(ComputeSha256Hex(patched_sin_sysd), kExpectedLiveSinSysdSha256);
    EXPECT_EQ(patched_bootup.size(), kExpectedLiveBootupPatchedSize);
    EXPECT_EQ(ComputeSha256Hex(patched_bootup), kExpectedLiveBootupSha256);
}

}  // namespace
}  // namespace shin::menu
