// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/menu_patch_container.h"
#include "menu/domain_renderer.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || \
    defined(__wincodec_h__)
#error "Windows/GDI/WIC header leaked into menu_patch_container.h or domain_renderer.h"
#endif

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kZeroSeed = 0u;
constexpr std::uint32_t kTailCancelSeed = 0x00807F00u;
constexpr std::uint32_t kExpectedEmptyHash = 0x00000000u;
constexpr std::uint32_t kExpectedHighBitSingleByteHash = 0x9B891BB0u;
constexpr std::uint32_t kExpectedFourZeroBytesHash = 0xB469B2CCu;
constexpr std::uint32_t kExpectedAdieumenMagicHash = 0x4006AB44u;

constexpr std::size_t kOrigSliceLenA = 96u;
constexpr std::size_t kOrigSliceLenB = 128u;
constexpr std::size_t kNewPayloadLenA = 140u;
constexpr std::size_t kNewPayloadLenB = 73u;
constexpr std::size_t kVersionFieldOffset = 0x08u;
constexpr std::size_t kRecordCountFieldOffset = 0x0Cu;
constexpr std::size_t kManifestHashFieldOffset = 0x10u;
constexpr std::size_t kReservedFieldOffset = 0x14u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        dir_ = fs::temp_directory_path() /
               fs::u8path("adi_ut_mpatch_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return dir_; }

private:
    fs::path dir_;
};

std::vector<std::uint8_t> MakeEntropyBytes(std::size_t count, std::uint8_t salt) {
    std::vector<std::uint8_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::uint8_t>(
            ((i * 131u + 17u) ^ (static_cast<std::size_t>(salt) * 73u + (i >> 2u))) &
            0xFFu);
    }
    return out;
}

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& buf, std::size_t off) {
    return static_cast<std::uint32_t>(buf.at(off)) |
           (static_cast<std::uint32_t>(buf.at(off + 1u)) << 8u) |
           (static_cast<std::uint32_t>(buf.at(off + 2u)) << 16u) |
           (static_cast<std::uint32_t>(buf.at(off + 3u)) << 24u);
}

std::uint64_t ReadLe64(const std::vector<std::uint8_t>& buf, std::size_t off) {
    return static_cast<std::uint64_t>(ReadLe32(buf, off)) |
           (static_cast<std::uint64_t>(ReadLe32(buf, off + 4u)) << 32u);
}

void WriteLe32At(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    buf.at(off) = static_cast<std::uint8_t>(v & 0xFFu);
    buf.at(off + 1u) = static_cast<std::uint8_t>((v >> 8u) & 0xFFu);
    buf.at(off + 2u) = static_cast<std::uint8_t>((v >> 16u) & 0xFFu);
    buf.at(off + 3u) = static_cast<std::uint8_t>((v >> 24u) & 0xFFu);
}

void RecomputeManifestHash(std::vector<std::uint8_t>& blob) {
    const std::uint32_t h = adi::menu::MurmurHash2(
        blob.data() + adi::menu::kMenuPatchHeaderBytes,
        blob.size() - adi::menu::kMenuPatchHeaderBytes, kZeroSeed);
    WriteLe32At(blob, kManifestHashFieldOffset, h);
}

void WriteBinaryFile(const fs::path& file_path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(file_path.parent_path());
    std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("Failed to open test file for write");
    }
    if (!bytes.empty()) {
        ofs.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& file_path) {
    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("Failed to open test file for read");
    }
    const std::streamsize sz = ifs.tellg();
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(out.data()), sz);
    }
    return out;
}

adi::g00::GameexeKey256 BuildSyntheticGameexeKey256() {
    adi::g00::GameexeKey256 key{};
    for (std::size_t i = 0; i < adi::g00::kExpectedAdieuGameexeKeyPrefix.size(); ++i) {
        key[i] = adi::g00::kExpectedAdieuGameexeKeyPrefix[i];
    }
    for (std::size_t i = adi::g00::kExpectedAdieuGameexeKeyPrefix.size();
         i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>((i * 53u + 0x29u) & 0xFFu);
    }
    return key;
}

std::vector<std::uint8_t> BuildSyntheticTwoCutG00() {
    adi::g00::G00Image img;
    img.type = adi::g00::G00Type::kType2MultiCutBgra32;
    img.width = 24u;
    img.height = 16u;
    img.cuts.resize(2u);

    for (std::size_t idx = 0; idx < 2u; ++idx) {
        adi::g00::G00Cut& c = img.cuts[idx];
        c.is_empty = false;
        c.modified = true;
        c.descriptor = {2, 2, 11, 9, static_cast<std::int32_t>(idx * 5), -3};
        c.cut_header.cut_type = 1u;
        c.cut_header.center_x = 4;
        c.cut_header.center_y = 3;
        c.canvas = adi::imaging::BgraImage(img.width, img.height);
        for (int y = 2; y <= 9; ++y) {
            for (int x = 2; x <= 11; ++x) {
                c.canvas.SetPixel(
                    x, y,
                    adi::imaging::BgraColor{
                        static_cast<std::uint8_t>((x * 19 + idx * 40) & 0xFF),
                        static_cast<std::uint8_t>((y * 23 + idx * 30) & 0xFF),
                        static_cast<std::uint8_t>(((x + y) * 17 + 0x55) & 0xFF),
                        220u,
                    });
            }
        }
    }
    return adi::g00::G00Codec::Encode(img);
}

class StubTitleDomainRenderer final : public adi::menu::IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "title_stub"; }

    void Apply(adi::menu::PatchBuildContext& ctx) override {
        adi::g00::GameexeDocument doc = ctx.LoadGameexeDocument();
        doc.SetDirectiveStringValue("#GAMENAME", "Comment te dire adieu");
        ctx.SubmitPatchedGameexe(doc);

        adi::g00::G00Image sprite = ctx.LoadG00("ME_02.g00");
        sprite.MutableCutCanvas(1u).SetPixel(
            14, 6, adi::imaging::BgraColor{240u, 245u, 250u, 255u});
        ctx.SubmitPatchedG00("ME_02.g00", sprite, {1u});
    }
};

}  // namespace

TEST(MenuPatchContainer, MurmurHash2PinnedCanonicalVectorsAndTailSeams) {
    const std::array<std::uint8_t, 0> empty{};
    EXPECT_EQ(adi::menu::MurmurHash2(empty.data(), empty.size(), kZeroSeed),
              kExpectedEmptyHash);

    // Tail length 1 with high sign bit set (0x80): catches signed-char sign extension.
    const std::array<std::uint8_t, 1> high_bit_byte{{0x80u}};
    EXPECT_EQ(adi::menu::MurmurHash2(high_bit_byte, kZeroSeed),
              kExpectedHighBitSingleByteHash);

    // Tail length 2 algebraic cancellation: h = (0 ^ 2) ^ (0x00 << 8) ^ 0x02 == 0.
    const std::array<std::uint8_t, 2> tail2{{0x02u, 0x00u}};
    EXPECT_EQ(adi::menu::MurmurHash2(tail2, kZeroSeed), kExpectedEmptyHash);

    // Tail length 3 with 0x7F/0x80 sign-bit seam: cancels to 0 iff shifts are
    // unsigned (16, 8, 0) and fallthrough covers all 3 tail bytes.
    const std::array<std::uint8_t, 3> tail3{{0x03u, 0x7Fu, 0x80u}};
    EXPECT_EQ(adi::menu::MurmurHash2(tail3, kTailCancelSeed), kExpectedEmptyHash);

    // Exact 4-byte block vector (0 tail bytes).
    const std::array<std::uint8_t, 4> four_zeros{{0x00u, 0x00u, 0x00u, 0x00u}};
    EXPECT_EQ(adi::menu::MurmurHash2(four_zeros, kZeroSeed),
              kExpectedFourZeroBytesHash);

    // 8-byte "ADIEUMEN" header magic must match its pinned canonical hash and be
    // distinct from truncated 7-byte and extended 9-byte inputs.
    const std::array<std::uint8_t, 9> magic_plus{{
        'A', 'D', 'I', 'E', 'U', 'M', 'E', 'N', '1',
    }};
    const std::uint32_t h7 = adi::menu::MurmurHash2(magic_plus.data(), 7u, kZeroSeed);
    const std::uint32_t h8 = adi::menu::MurmurHash2(magic_plus.data(), 8u, kZeroSeed);
    const std::uint32_t h9 = adi::menu::MurmurHash2(magic_plus.data(), 9u, kZeroSeed);
    EXPECT_EQ(h8, kExpectedAdieumenMagicHash);
    EXPECT_NE(h8, h7);
    EXPECT_NE(h8, h9);
}

TEST(MenuPatchContainer, RelativePathSecurityValidationMatrix) {
    struct PathCase {
        std::string_view path;
        bool expected_valid;
    };
    const PathCase kCases[] = {
        {"Gameexe.dat", true},
        {"g00\\ME_01.g00", true},
        {"g00\\_name_input_ok.g00", true},
        {"g00\\COM0.g00", true},
        {"g00\\COM10.g00", true},
        {"g00\\CONSOLE.g00", true},
        {"", false},
        {"\\g00\\ME_01.g00", false},
        {"/g00/ME_01.g00", false},
        {"/foo", false},
        {"g00/ME_01.g00", false},
        {"..\\foo", false},
        {"g00\\..\\ME_01.g00", false},
        {".\\Gameexe.dat", false},
        {"g00\\.\\ME_01.g00", false},
        {"C:\\foo", false},
        {"D:Gameexe.dat", false},
        {"g00\\\\ME_01.g00", false},
        {"g00\\ME_01.g00\\", false},
        {"Gameexe.dat ", false},
        {"Gameexe.dat.", false},
        {"g00 \\ME_01.g00", false},
        {"g00.\\ME_01.g00", false},
        {"CON", false},
        {"prn", false},
        {"Aux.dat", false},
        {"g00\\nul.g00", false},
        {"g00\\com1.g00", false},
        {"g00\\COM9.dat", false},
        {"g00\\lpt1.g00", false},
        {"g00\\LPT9.bin", false},
    };
    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.path);
        EXPECT_EQ(adi::menu::IsSafeRelativeAssetPath(tc.path), tc.expected_valid);
    }

    const PathCase kTargetCases[] = {
        {"Gameexe.dat", true},
        {"g00\\ME_02.g00", true},
        {"menu_patch.bin", false},
        {"MENU_PATCH.BIN", false},
        {"menu_patch.applied", false},
        {"g00\\Menu_Patch.Applied", false},
        {"SiglusEngine.exe", false},
        {"g00\\helper.EXE", false},
        {"winmm.dll", false},
        {"g00\\hook.DLL", false},
        {"Gameexe.dat.bak", false},
        {"g00\\ME_01.g00.BAK", false},
        {"g00\\ME_01.g00.tmp", false},
    };
    for (const auto& tc : kTargetCases) {
        SCOPED_TRACE(tc.path);
        EXPECT_EQ(adi::menu::IsAllowedMenuTargetPath(tc.path), tc.expected_valid);
    }

    const std::string embedded_nul("g00\0ME_01.g00", 13u);
    EXPECT_FALSE(adi::menu::IsSafeRelativeAssetPath(embedded_nul));

    const std::string over_max_len(adi::menu::kMaxMenuRelPathBytes + 1u, 'a');
    EXPECT_FALSE(adi::menu::IsSafeRelativeAssetPath(over_max_len));

    const std::string exact_max_len(adi::menu::kMaxMenuRelPathBytes, 'a');
    EXPECT_TRUE(adi::menu::IsSafeRelativeAssetPath(exact_max_len));

    const std::string invalid_utf8("g00\\\xFF\xFE.g00");
    EXPECT_FALSE(adi::menu::IsSafeRelativeAssetPath(invalid_utf8));
}

TEST(MenuPatchContainer, WireLayoutRoundTripAndUnmaskVerification) {
    const auto orig_a = MakeEntropyBytes(kOrigSliceLenA, 0x11u);
    const auto new_a = MakeEntropyBytes(kNewPayloadLenA, 0x5Au);
    const auto orig_b = MakeEntropyBytes(kOrigSliceLenB, 0x37u);
    const auto new_b = MakeEntropyBytes(kNewPayloadLenB, 0xC3u);

    adi::menu::MenuPatchBuilder builder;
    builder.AddRecord({
        "Gameexe.dat",
        "Gameexe.dat",
        0u,
        orig_a,
        new_a,
        adi::menu::kFlagBackupStandalone,
    });
    builder.AddRecord({
        "g00\\ME_02.g00",
        "g00\\ME_02.g00",
        0u,
        orig_b,
        new_b,
        adi::menu::kFlagBackupStandalone,
    });
    ASSERT_EQ(builder.record_count(), 2u);

    ScopedTempDir scratch;
    const fs::path bin_path = scratch.path() / "menu_patch.bin";
    builder.WriteToFile(bin_path);

    const auto blob = builder.BuildBytes();
    ASSERT_GT(blob.size(), adi::menu::kMenuPatchHeaderBytes);
    EXPECT_TRUE(adi::menu::VerifyNoRawAssetSignatures(blob));

    // Pin exact 32-byte header wire fields (+0x00..+0x1F) and first record wire layout.
    for (std::size_t i = 0; i < adi::menu::kMenuPatchMagic.size(); ++i) {
        EXPECT_EQ(static_cast<char>(blob[i]), adi::menu::kMenuPatchMagic[i]);
    }
    EXPECT_EQ(ReadLe32(blob, kVersionFieldOffset), adi::menu::kMenuPatchVersion);
    EXPECT_EQ(ReadLe32(blob, kRecordCountFieldOffset), 2u);
    const std::uint32_t expected_manifest_hash = adi::menu::MurmurHash2(
        blob.data() + adi::menu::kMenuPatchHeaderBytes,
        blob.size() - adi::menu::kMenuPatchHeaderBytes, kZeroSeed);
    EXPECT_EQ(ReadLe32(blob, kManifestHashFieldOffset), expected_manifest_hash);
    for (std::size_t i = 0; i < adi::menu::kMenuPatchReservedBytes; ++i) {
        EXPECT_EQ(blob[kReservedFieldOffset + i], 0u);
    }

    // Verify first record wire field offsets starting at +0x20:
    // [u16 src_len=11]["Gameexe.dat"][u16 dst_len=11]["Gameexe.dat"][u64 off][u64 orig_sz]
    // [u32 orig_hash][u64 new_sz][u32 new_hash][u32 flags][xor_payload]
    std::size_t cur = adi::menu::kMenuPatchHeaderBytes;
    const std::uint16_t src_len =
        static_cast<std::uint16_t>(blob[cur] | (blob[cur + 1u] << 8u));
    ASSERT_EQ(src_len, 11u);
    cur += 2u + src_len;
    const std::uint16_t dst_len =
        static_cast<std::uint16_t>(blob[cur] | (blob[cur + 1u] << 8u));
    ASSERT_EQ(dst_len, 11u);
    cur += 2u + dst_len;
    EXPECT_EQ(ReadLe64(blob, cur), 0u);
    cur += 8u;
    EXPECT_EQ(ReadLe64(blob, cur), kOrigSliceLenA);
    cur += 8u;
    EXPECT_EQ(ReadLe32(blob, cur), adi::menu::MurmurHash2(orig_a, kZeroSeed));
    cur += 4u;
    EXPECT_EQ(ReadLe64(blob, cur), kNewPayloadLenA);
    cur += 8u;
    EXPECT_EQ(ReadLe32(blob, cur), adi::menu::MurmurHash2(new_a, kZeroSeed));
    cur += 4u;
    EXPECT_EQ(ReadLe32(blob, cur), adi::menu::kFlagBackupStandalone);

    const auto manifest = adi::menu::ParseMenuPatchContainer(blob);
    EXPECT_EQ(manifest.version, adi::menu::kMenuPatchVersion);
    EXPECT_EQ(manifest.manifest_hash, expected_manifest_hash);
    ASSERT_EQ(manifest.records.size(), 2u);

    EXPECT_EQ(adi::menu::UnmaskMenuPatchRecord(manifest.records[0], orig_a), new_a);
    EXPECT_EQ(adi::menu::UnmaskMenuPatchRecord(manifest.records[1], orig_b), new_b);
}

TEST(MenuPatchContainer, RejectsShortOrigSliceHeaderTamperingAndHashCorruption) {
    const auto valid_orig = MakeEntropyBytes(adi::menu::kMinOrigSliceBytes, 0x22u);
    const auto short_orig = MakeEntropyBytes(adi::menu::kMinOrigSliceBytes - 1u, 0x22u);
    const auto valid_new = MakeEntropyBytes(80u, 0x44u);

    adi::menu::MenuPatchBuilder builder;
    EXPECT_THROW(
        builder.AddRecord({"Gameexe.dat", "Gameexe.dat", 0u, short_orig, valid_new,
                           adi::menu::kFlagBackupStandalone}),
        std::exception);

    // Valid -> Failing -> Valid state seam: rejected AddRecord must not taint builder.
    ASSERT_EQ(builder.record_count(), 0u);
    EXPECT_THROW(
        builder.AddRecord({"Gameexe.dat", "menu_patch.bin", 0u, valid_orig, valid_new,
                           adi::menu::kFlagBackupStandalone}),
        std::invalid_argument);
    EXPECT_THROW(
        builder.AddRecord({"Gameexe.dat", "SiglusEngine.exe", 0u, valid_orig, valid_new,
                           adi::menu::kFlagBackupStandalone}),
        std::invalid_argument);
    ASSERT_EQ(builder.record_count(), 0u);

    builder.AddRecord({"Gameexe.dat", "Gameexe.dat", 0u, valid_orig, valid_new,
                       adi::menu::kFlagBackupStandalone});
    ASSERT_EQ(builder.record_count(), 1u);

    // Case-insensitive duplicate target_rel_path rejected without tainting builder.
    EXPECT_THROW(
        builder.AddRecord({"Gameexe.dat", "GAMEEXE.DAT", 0u, valid_orig, valid_new,
                           adi::menu::kFlagBackupStandalone}),
        std::invalid_argument);
    ASSERT_EQ(builder.record_count(), 1u);

    const auto clean_blob = builder.BuildBytes();

    // Forbidden target_rel_path inside wire image (e.g. "1234567.exe", 11 chars)
    // rejected by ParseMenuPatchContainer even when manifest_hash is recomputed.
    {
        auto forbidden_target_blob = clean_blob;
        const std::size_t target_str_off = adi::menu::kMenuPatchHeaderBytes + 2u + 11u + 2u;
        const std::string_view kForbiddenEleven = "1234567.exe";
        std::memcpy(forbidden_target_blob.data() + target_str_off,
                    kForbiddenEleven.data(), kForbiddenEleven.size());
        RecomputeManifestHash(forbidden_target_blob);
        EXPECT_THROW(adi::menu::ParseMenuPatchContainer(forbidden_target_blob),
                     std::runtime_error);
    }

    // Case-insensitive duplicate target_rel_path inside wire image rejected by
    // ParseMenuPatchContainer even when manifest_hash is recomputed.
    {
        adi::menu::MenuPatchBuilder two_builder;
        two_builder.AddRecord({"g00\\ME_01.g00", "g00\\ME_01.g00", 0u, valid_orig, valid_new,
                               adi::menu::kFlagBackupStandalone});
        two_builder.AddRecord({"g00\\ME_02.g00", "g00\\ME_02.g00", 0u, valid_orig, valid_new,
                               adi::menu::kFlagBackupStandalone});
        auto dup_blob = two_builder.BuildBytes();
        // Overwrite second record's target_rel_path ("g00\\ME_02.g00") with "G00\\me_01.G00".
        const std::size_t rec0_bytes =
            2u + 13u + 2u + 13u + 36u + valid_new.size();
        const std::size_t rec1_target_off =
            adi::menu::kMenuPatchHeaderBytes + rec0_bytes + 2u + 13u + 2u;
        const std::string_view kDupCaseTarget = "G00\\me_01.G00";
        std::memcpy(dup_blob.data() + rec1_target_off,
                    kDupCaseTarget.data(), kDupCaseTarget.size());
        RecomputeManifestHash(dup_blob);
        EXPECT_THROW(adi::menu::ParseMenuPatchContainer(dup_blob), std::runtime_error);
    }

    // Non-zero reserved byte rejected.
    auto bad_reserved = clean_blob;
    bad_reserved[kReservedFieldOffset + 5u] = 0x01u;
    EXPECT_THROW(adi::menu::ParseMenuPatchContainer(bad_reserved), std::runtime_error);

    // Tampered manifest_hash rejected.
    auto bad_manifest = clean_blob;
    bad_manifest[kManifestHashFieldOffset] ^= 0x01u;
    EXPECT_THROW(adi::menu::ParseMenuPatchContainer(bad_manifest), std::runtime_error);

    // Corrupting xor_payload rejected by manifest_hash on parse AND by new_raw_hash on unmask.
    auto parsed = adi::menu::ParseMenuPatchContainer(clean_blob);
    ASSERT_EQ(parsed.records.size(), 1u);

    auto wrong_orig = valid_orig;
    wrong_orig[0] ^= 0x80u;
    EXPECT_THROW(adi::menu::UnmaskMenuPatchRecord(parsed.records[0], wrong_orig),
                 std::runtime_error);

    parsed.records[0].xor_payload.back() ^= 0x01u;
    EXPECT_THROW(adi::menu::UnmaskMenuPatchRecord(parsed.records[0], valid_orig),
                 std::runtime_error);
}

TEST(MenuPatchContainer, VerifyNoRawAssetSignaturesRejectsPngGameexeAndZeroRuns) {
    auto base = MakeEntropyBytes(160u, 0x6Du);
    for (std::size_t i = 0; i < adi::menu::kMenuPatchMagic.size(); ++i) {
        base[i] = static_cast<std::uint8_t>(adi::menu::kMenuPatchMagic[i]);
    }
    EXPECT_TRUE(adi::menu::VerifyNoRawAssetSignatures(base));

    // 1. Raw PNG 8-byte signature inside [0x20..EOF).
    auto with_png = base;
    const std::array<std::uint8_t, 8> kPngSig = {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    for (std::size_t i = 0; i < kPngSig.size(); ++i) {
        with_png[48u + i] = kPngSig[i];
    }
    std::string reason;
    EXPECT_FALSE(adi::menu::VerifyNoRawAssetSignatures(with_png, &reason));
    EXPECT_FALSE(reason.empty());

    // 2. UTF-16LE "#GAMENAME" inside [0x20..EOF).
    auto with_gamename = base;
    const std::string_view kAsciiTag = "#GAMENAME";
    for (std::size_t i = 0; i < kAsciiTag.size(); ++i) {
        with_gamename[40u + i * 2u] = static_cast<std::uint8_t>(kAsciiTag[i]);
        with_gamename[40u + i * 2u + 1u] = 0x00u;
    }
    reason.clear();
    EXPECT_FALSE(adi::menu::VerifyNoRawAssetSignatures(with_gamename, &reason));
    EXPECT_FALSE(reason.empty());

    // 3. UTF-16LE "#DISCMARK" inside [0x20..EOF).
    auto with_discmark = base;
    const std::string_view kDiscTag = "#DISCMARK";
    for (std::size_t i = 0; i < kDiscTag.size(); ++i) {
        with_discmark[40u + i * 2u] = static_cast<std::uint8_t>(kDiscTag[i]);
        with_discmark[40u + i * 2u + 1u] = 0x00u;
    }
    EXPECT_FALSE(adi::menu::VerifyNoRawAssetSignatures(with_discmark));

    // 4. Exact boundary seam on consecutive zero bytes: 63 passes, 64 fails.
    auto with_63_zeros = base;
    for (std::size_t i = 0; i < adi::menu::kForbiddenZeroRunBytes - 1u; ++i) {
        with_63_zeros[40u + i] = 0x00u;
    }
    with_63_zeros[40u + adi::menu::kForbiddenZeroRunBytes - 1u] = 0xFFu;
    EXPECT_TRUE(adi::menu::VerifyNoRawAssetSignatures(with_63_zeros));

    auto with_64_zeros = with_63_zeros;
    with_64_zeros[40u + adi::menu::kForbiddenZeroRunBytes - 1u] = 0x00u;
    EXPECT_FALSE(adi::menu::VerifyNoRawAssetSignatures(with_64_zeros));
}

TEST(MenuPatchContainer, MenuPatchPipelineRunsDomainRendererValidatesParityAndPrefersBakSource) {
    ScopedTempDir scratch;
    const fs::path root_dir = scratch.path() / "InstallRoot";
    const fs::path game_dir = root_dir / "StartData" / "GameData";

    const adi::g00::GameexeKey256 key = BuildSyntheticGameexeKey256();
    const std::size_t exe_size =
        static_cast<std::size_t>(adi::g00::kAdieuGameexeKeyFileOffset) + key.size();
    std::vector<std::uint8_t> synthetic_exe(exe_size, 0u);
    std::memcpy(synthetic_exe.data() + adi::g00::kAdieuGameexeKeyFileOffset,
                key.data(), key.size());
    WriteBinaryFile(game_dir / "SiglusEngine.exe", synthetic_exe);

    const std::string pristine_ini =
        "#GAMENAME = \"さよならを教えて\"\r\n"
        "#DISCMARK = \"sayonara.env\"\r\n"
        "#EXENAME = \"SiglusEngine.exe\"\r\n"
        "#SAVEDIR = \"savedata\"\r\n"
        "#SAVE.CNT = 100\r\n"
        "#WINDOW_ATTR = 1, 2, 3, 4\r\n";
    const adi::g00::GameexeDocument pristine_doc =
        adi::g00::GameexeDocument::FromUtf8(pristine_ini);
    const std::vector<std::uint8_t> pristine_gameexe =
        adi::g00::EncodeGameexe(pristine_doc.ToUtf16(), key);
    ASSERT_GE(pristine_gameexe.size(), adi::menu::kMinOrigSliceBytes);

    // Populate `Gameexe.dat.bak` with pristine bytes and overwrite live `Gameexe.dat`
    // with stale bytes so `PatchBuildContext` must prefer `.bak` for idempotence.
    adi::g00::GameexeDocument stale_doc = pristine_doc;
    stale_doc.SetDirectiveStringValue("#GAMENAME", "Stale Patched Copy");
    const std::vector<std::uint8_t> stale_gameexe =
        adi::g00::EncodeGameexe(stale_doc.ToUtf16(), key);
    WriteBinaryFile(game_dir / "Gameexe.dat.bak", pristine_gameexe);
    WriteBinaryFile(game_dir / "Gameexe.dat", stale_gameexe);

    const std::vector<std::uint8_t> pristine_g00 = BuildSyntheticTwoCutG00();
    ASSERT_GE(pristine_g00.size(), adi::menu::kMinOrigSliceBytes);
    WriteBinaryFile(game_dir / "g00" / "ME_02.g00", pristine_g00);

    EXPECT_EQ(adi::menu::ResolveRetailGameDataDir(root_dir), game_dir);

    // Negative guards on PatchBuildContext: critical-directive drift and origin drift throw.
    {
        adi::menu::PatchBuildContext guard_ctx(root_dir);
        EXPECT_TRUE(guard_ctx.HasAssetFile("Gameexe.dat"));
        EXPECT_TRUE(guard_ctx.HasAssetFile("g00\\ME_02.g00"));
        EXPECT_FALSE(guard_ctx.HasAssetFile("..\\outside.bin"));

        adi::g00::GameexeDocument bad_doc = guard_ctx.LoadGameexeDocument();
        bad_doc.SetDirectiveStringValue("#DISCMARK", "mutated.env");
        EXPECT_THROW(guard_ctx.SubmitPatchedGameexe(bad_doc), std::runtime_error);

        adi::g00::G00Image bad_g00 = guard_ctx.LoadG00("ME_02.g00");
        bad_g00.MutableCutCanvas(1u).SetPixel(4, 4, adi::imaging::BgraColor{1u, 2u, 3u, 255u});
        bad_g00.cuts[1].descriptor.origin_x += 7;
        EXPECT_THROW(guard_ctx.SubmitPatchedG00("ME_02.g00", bad_g00, {1u}),
                     std::runtime_error);
        EXPECT_EQ(guard_ctx.submitted_record_count(), 0u);
    }

    // Empty pipeline (0 records) must refuse to emit an empty container.
    {
        adi::menu::MenuPatchPipeline empty_pipeline;
        EXPECT_EQ(empty_pipeline.renderer_count(), 0u);
        adi::menu::MenuPatchBuildOptions empty_opts{
            root_dir, scratch.path() / "unused.bin", {}};
        EXPECT_THROW(empty_pipeline.Run(empty_opts), std::runtime_error);
    }

    // Full end-to-end synthetic pipeline execution + contact sheet generation.
    adi::menu::MenuPatchPipeline pipeline;
    pipeline.RegisterRenderer(std::make_unique<StubTitleDomainRenderer>());
    EXPECT_EQ(pipeline.renderer_count(), 1u);

    const fs::path out_bin = scratch.path() / "out" / "menu_patch.bin";
    const fs::path sheet_dir = scratch.path() / "sheets";
    const adi::menu::MenuPatchBuildSummary summary =
        pipeline.Run({root_dir, out_bin, sheet_dir});

    EXPECT_EQ(summary.renderer_count, 1u);
    EXPECT_EQ(summary.record_count, 2u);
    EXPECT_GT(summary.patch_bytes, adi::menu::kMenuPatchHeaderBytes);
    ASSERT_EQ(summary.contact_sheet_pages.size(), 1u);
    EXPECT_TRUE(fs::is_regular_file(summary.contact_sheet_pages[0]));

    const std::vector<std::uint8_t> patch_blob = ReadBinaryFile(out_bin);
    EXPECT_TRUE(adi::menu::VerifyNoRawAssetSignatures(patch_blob));

    const adi::menu::MenuPatchManifest manifest =
        adi::menu::ParseMenuPatchContainer(patch_blob);
    ASSERT_EQ(manifest.records.size(), 2u);
    EXPECT_EQ(manifest.records[0].target_rel_path, "Gameexe.dat");
    EXPECT_EQ(manifest.records[1].target_rel_path, "g00\\ME_02.g00");

    // Unmasking against pristine `Gameexe.dat.bak` succeeds (whereas stale `Gameexe.dat` fails).
    EXPECT_THROW(adi::menu::UnmaskMenuPatchRecord(manifest.records[0], stale_gameexe),
                 std::runtime_error);
    const std::vector<std::uint8_t> unmasked_gameexe =
        adi::menu::UnmaskMenuPatchRecord(manifest.records[0], pristine_gameexe);
    const adi::g00::GameexeDocument unmasked_doc = adi::g00::GameexeDocument::FromUtf16(
        adi::g00::DecodeGameexe(unmasked_gameexe, key));
    EXPECT_EQ(unmasked_doc.FindDirectiveRawValue("#GAMENAME"),
              std::optional<std::string>("\"Comment te dire adieu\""));
    EXPECT_EQ(unmasked_doc.FindDirectiveRawValue("#DISCMARK"),
              std::optional<std::string>("\"sayonara.env\""));

    const std::vector<std::uint8_t> unmasked_g00_bytes =
        adi::menu::UnmaskMenuPatchRecord(manifest.records[1], pristine_g00);
    const adi::g00::G00Image orig_decoded = adi::g00::G00Codec::Decode(pristine_g00);
    const adi::g00::G00Image patched_decoded = adi::g00::G00Codec::Decode(unmasked_g00_bytes);
    ASSERT_EQ(patched_decoded.cuts.size(), 2u);
    EXPECT_EQ(patched_decoded.cuts[0].raw_cut_bytes, orig_decoded.cuts[0].raw_cut_bytes);
    EXPECT_EQ(patched_decoded.cuts[1].canvas.PixelAt(14, 6),
              (adi::imaging::BgraColor{240u, 245u, 250u, 255u}));
}
