// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System round-trip verification of SiglusEngine.exe runtime key extraction,
// Gameexe.dat decryption/re-encryption, and Type 0/1/2 .g00 sprite decoding and
// lossless re-encoding against shipped retail Adieu assets (read-only).
#include "g00/gameexe_codec.h"
#include "g00/g00_codec.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || defined(__wincodec_h__)
#error "Windows header leaked"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace adi::g00 {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kExpectedGameexeDatBytes = 9443u;
constexpr std::size_t kExpectedGameexeUnpackedBytes = 47602u;
constexpr std::size_t kExpectedGameexeDirectiveCount = 499u;
constexpr std::size_t kBytesPerUtf16Unit = 2u;

constexpr std::uint16_t kFullScreenWidth = 640u;
constexpr std::uint16_t kFullScreenHeight = 480u;
constexpr std::uint16_t kNameInputBtnWidth = 145u;
constexpr std::uint16_t kNameInputBtnHeight = 25u;

constexpr std::size_t kExpectedMe02CutCount = 20u;
constexpr std::size_t kExpectedMess99CutCount = 31u;
constexpr std::size_t kExpectedNameInputOkCutCount = 3u;

struct ResolvedGameDir {
    bool env_configured = false;
    fs::path game_data_dir;
    fs::path raw_env_path;
};

bool FileOrBackupExists(const fs::path& target) noexcept {
    std::error_code ec;
    if (fs::is_regular_file(target, ec)) {
        return true;
    }
    fs::path bak = target;
    bak += ".bak";
    return fs::is_regular_file(bak, ec);
}

ResolvedGameDir ResolveRetailGameDataDir() {
    ResolvedGameDir res;
    const wchar_t* raw_env = _wgetenv(L"VN_GAME_DIR");
    if (raw_env == nullptr || *raw_env == L'\0') {
        return res;
    }

    res.env_configured = true;
    res.raw_env_path = fs::path(raw_env);

    for (const fs::path& candidate : {
             res.raw_env_path / "StartData" / "GameData",
             res.raw_env_path / "GameData",
             res.raw_env_path,
         }) {
        if (FileOrBackupExists(candidate / "SiglusEngine.exe") &&
            FileOrBackupExists(candidate / "Gameexe.dat")) {
            res.game_data_dir = candidate;
            return res;
        }
    }
    return res;
}

// Reads a retail asset strictly read-only, preferring `<path>.bak` if an
// earlier deployment ever created an original backup next to the live file.
std::vector<std::uint8_t> ReadRetailAssetReadOnly(const fs::path& asset_path) {
    fs::path actual = asset_path;
    fs::path bak = asset_path;
    bak += ".bak";
    std::error_code ec;
    if (fs::is_regular_file(bak, ec)) {
        actual = bak;
    }

    std::ifstream in(actual, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("failed to open retail asset: " + actual.u8string());
    }
    const std::streamoff sz = in.tellg();
    if (sz < 0) {
        throw std::runtime_error("failed to stat retail asset: " + actual.u8string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz), 0u);
    in.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
        if (!in || in.gcount() != sz) {
            throw std::runtime_error("short read on retail asset: " + actual.u8string());
        }
    }
    return bytes;
}

fs::path PreferBackupPath(const fs::path& asset_path) {
    fs::path bak = asset_path;
    bak += ".bak";
    std::error_code ec;
    if (fs::is_regular_file(bak, ec)) {
        return bak;
    }
    return asset_path;
}

}  // namespace

TEST(GameexeRoundtrip, RetailSiglusExeKeyAndGameexeDatLosslessRoundTrip) {
    const ResolvedGameDir resolved = ResolveRetailGameDataDir();
    if (!resolved.env_configured) {
        GTEST_SKIP() << "VN_GAME_DIR unset; skipping retail Gameexe.dat round-trip ST";
    }
    ASSERT_FALSE(resolved.game_data_dir.empty())
        << "VN_GAME_DIR is set (" << resolved.raw_env_path.u8string()
        << "), but SiglusEngine.exe and Gameexe.dat were not found";

    const fs::path exe_path = PreferBackupPath(resolved.game_data_dir / "SiglusEngine.exe");
    const GameexeKey256 key256 = LoadGameexeKeyFromExe(exe_path);
    EXPECT_TRUE(std::equal(kExpectedAdieuGameexeKeyPrefix.begin(),
                           kExpectedAdieuGameexeKeyPrefix.end(), key256.begin()));

    const std::vector<std::uint8_t> orig_dat =
        ReadRetailAssetReadOnly(resolved.game_data_dir / "Gameexe.dat");
    EXPECT_EQ(orig_dat.size(), kExpectedGameexeDatBytes);

    const std::u16string orig_utf16 = DecodeGameexe(orig_dat, key256);
    EXPECT_EQ(orig_utf16.size() * kBytesPerUtf16Unit, kExpectedGameexeUnpackedBytes);

    const GameexeDocument orig_doc = GameexeDocument::FromUtf16(orig_utf16);
    EXPECT_EQ(orig_doc.directive_count(), kExpectedGameexeDirectiveCount);
    EXPECT_EQ(orig_doc.FindDirectiveRawValue("#GAMENAME"),
              std::optional<std::string>("\"さよならを教えて\""));
    EXPECT_EQ(orig_doc.FindDirectiveRawValue("#DISCMARK"),
              std::optional<std::string>("\"sayonara.env\""));
    EXPECT_EQ(orig_doc.ToUtf16(), orig_utf16);

    // Lossless in-memory re-encode -> re-decode round-trip with real key256.
    const std::vector<std::uint8_t> reencoded_dat =
        EncodeGameexe(orig_doc.ToUtf16(), key256);
    const std::u16string redecoded_utf16 = DecodeGameexe(reencoded_dat, key256);
    EXPECT_EQ(redecoded_utf16, orig_utf16);

    const std::string orig_utf8 = orig_doc.ToUtf8(false);
    const GameexeDocument roundtrip_doc = GameexeDocument::FromUtf16(redecoded_utf16);
    EXPECT_EQ(roundtrip_doc.ToUtf8(false), orig_utf8);
    VerifyCriticalDirectivesUnchanged(orig_utf8, roundtrip_doc.ToUtf8(false));

    // In-memory UI title edit round-trip preserves #DISCMARK and all 499 directives.
    GameexeDocument localized_doc = orig_doc;
    ASSERT_EQ(localized_doc.SetDirectiveStringValue("#GAMENAME", "Comment te dire adieu"),
              1u);
    VerifyCriticalDirectivesUnchanged(orig_utf8, localized_doc.ToUtf8(false));

    const std::vector<std::uint8_t> localized_dat =
        EncodeGameexe(localized_doc.ToUtf16(), key256);
    const GameexeDocument localized_redecoded =
        GameexeDocument::FromUtf16(DecodeGameexe(localized_dat, key256));
    EXPECT_EQ(localized_redecoded.directive_count(), kExpectedGameexeDirectiveCount);
    EXPECT_EQ(localized_redecoded.FindDirectiveRawValue("#GAMENAME"),
              std::optional<std::string>("\"Comment te dire adieu\""));
    EXPECT_EQ(localized_redecoded.FindDirectiveRawValue("#DISCMARK"),
              std::optional<std::string>("\"sayonara.env\""));
}

TEST(GameexeRoundtrip, RetailG00SpritesLosslessRoundTripAcrossAllTypes) {
    const ResolvedGameDir resolved = ResolveRetailGameDataDir();
    if (!resolved.env_configured) {
        GTEST_SKIP() << "VN_GAME_DIR unset; skipping retail .g00 round-trip ST";
    }
    ASSERT_FALSE(resolved.game_data_dir.empty())
        << "VN_GAME_DIR is set (" << resolved.raw_env_path.u8string()
        << "), but SiglusEngine.exe and Gameexe.dat were not found";

    struct RetailSpriteSpec {
        const char* filename;
        G00Type expected_type;
        std::uint16_t expected_width;   // 0 when only >0 is asserted
        std::uint16_t expected_height;  // 0 when only >0 is asserted
        std::size_t expected_cuts;
    };

    const std::vector<RetailSpriteSpec> specs = {
        {"MENU01.g00", G00Type::kType0Bgr24, kFullScreenWidth, kFullScreenHeight, 0u},
        {"ME_01.g00", G00Type::kType0Bgr24, kFullScreenWidth, kFullScreenHeight, 0u},
        {"BLACK.g00", G00Type::kType1Paletted8, 0u, 0u, 0u},
        {"ME_02.g00", G00Type::kType2MultiCutBgra32, kFullScreenWidth, kFullScreenHeight,
         kExpectedMe02CutCount},
        {"MESS99.g00", G00Type::kType2MultiCutBgra32, kFullScreenWidth, kFullScreenHeight,
         kExpectedMess99CutCount},
        {"_name_input_ok.g00", G00Type::kType2MultiCutBgra32, kNameInputBtnWidth,
         kNameInputBtnHeight, kExpectedNameInputOkCutCount},
    };

    const fs::path g00_dir = resolved.game_data_dir / "g00";
    for (const RetailSpriteSpec& spec : specs) {
        SCOPED_TRACE(spec.filename);
        const std::vector<std::uint8_t> orig_bytes =
            ReadRetailAssetReadOnly(g00_dir / spec.filename);
        ASSERT_GT(orig_bytes.size(), kG00FileHeaderBytes);

        const G00Image orig_img = G00Codec::Decode(orig_bytes);
        EXPECT_EQ(orig_img.type, spec.expected_type);
        EXPECT_GT(orig_img.width, 0u);
        EXPECT_GT(orig_img.height, 0u);
        if (spec.expected_width != 0u) {
            EXPECT_EQ(orig_img.width, spec.expected_width);
            EXPECT_EQ(orig_img.height, spec.expected_height);
        }

        const std::vector<std::uint8_t> reencoded_bytes = G00Codec::Encode(orig_img);
        std::string parity_err;
        EXPECT_TRUE(
            G00Codec::VerifyMetadataParity(orig_bytes, reencoded_bytes, {}, &parity_err))
            << parity_err;

        const G00Image roundtrip_img = G00Codec::Decode(reencoded_bytes);
        EXPECT_EQ(roundtrip_img.type, orig_img.type);
        EXPECT_EQ(roundtrip_img.width, orig_img.width);
        EXPECT_EQ(roundtrip_img.height, orig_img.height);

        if (spec.expected_type != G00Type::kType2MultiCutBgra32) {
            EXPECT_EQ(roundtrip_img.canvas, orig_img.canvas);
            if (spec.expected_type == G00Type::kType1Paletted8) {
                EXPECT_EQ(roundtrip_img.type1_palette, orig_img.type1_palette);
                EXPECT_EQ(roundtrip_img.type1_indices, orig_img.type1_indices);
            }
            continue;
        }

        ASSERT_EQ(orig_img.cuts.size(), spec.expected_cuts);
        ASSERT_EQ(roundtrip_img.cuts.size(), orig_img.cuts.size());
        for (std::size_t c = 0u; c < orig_img.cuts.size(); ++c) {
            EXPECT_EQ(roundtrip_img.cuts[c].is_empty, orig_img.cuts[c].is_empty);
            EXPECT_EQ(roundtrip_img.cuts[c].raw_descriptor_bytes,
                      orig_img.cuts[c].raw_descriptor_bytes);
            EXPECT_EQ(roundtrip_img.cuts[c].raw_cut_bytes,
                      orig_img.cuts[c].raw_cut_bytes);
            EXPECT_EQ(roundtrip_img.cuts[c].canvas, orig_img.cuts[c].canvas);
        }
    }

    // Also verify in-memory single-cut consolidation on retail _name_input_ok.g00
    // keeps untouched cuts 1 and 2 byte-for-byte identical.
    const std::vector<std::uint8_t> ok_orig_bytes =
        ReadRetailAssetReadOnly(g00_dir / "_name_input_ok.g00");
    G00Image ok_edited = G00Codec::Decode(ok_orig_bytes);
    ASSERT_EQ(ok_edited.cuts.size(), kExpectedNameInputOkCutCount);
    ok_edited.MutableCutCanvas(0u).SetPixel(0, 0, imaging::BgraColor{0x20, 0x40, 0x60, 0xFF});

    const std::vector<std::uint8_t> ok_patched_bytes = G00Codec::Encode(ok_edited);
    G00Codec::AssertMetadataParity(ok_orig_bytes, ok_patched_bytes, {0u});

    const G00Image ok_redecoded = G00Codec::Decode(ok_patched_bytes);
    ASSERT_EQ(ok_redecoded.cuts.size(), kExpectedNameInputOkCutCount);
    EXPECT_EQ(ok_redecoded.cuts[0].canvas, ok_edited.cuts[0].canvas);
    EXPECT_EQ(ok_redecoded.cuts[0].part_headers.size(), 1u);
    EXPECT_EQ(ok_redecoded.cuts[1].raw_cut_bytes, ok_edited.cuts[1].raw_cut_bytes);
    EXPECT_EQ(ok_redecoded.cuts[2].raw_cut_bytes, ok_edited.cuts[2].raw_cut_bytes);
}

}  // namespace adi::g00
