// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for XTX two-table byte substitution bijection, plain-vs-encoded
// detection, and WelcomeToAnzU!_CE charaname.xtx translation (both plain and
// ciphered inputs, backup immutability, and shrinking-map restore).
#include "xtx/xtx.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace xtx = exm::xtx;
using namespace exm;

namespace {

constexpr int kExpectedAnzUCharanameChanges = 5;

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

}  // namespace

TEST(XtxContract, TwoTableCipherBijectionPinnedVectorAndPlainDetectionBoundaries) {
    // Pinned concrete wire vector across the sign-bit seam (prevents symmetric-bug pass).
    const Bytes plain_seam = {0x00u, 0x2Cu, 0x7Fu, 0x80u, 0xFFu};
    const Bytes expected_cipher = {0x5Au, 0x3Bu, 0xA1u, 0x5Eu, 0xA5u};
    EXPECT_EQ(xtx::encode_xtx(plain_seam), expected_cipher);
    EXPECT_EQ(xtx::decode_xtx(expected_cipher), plain_seam);

    const auto& enc_tbl = xtx::xtx_encode_table();
    const auto& dec_tbl = xtx::xtx_decode_table();

    std::array<bool, xtx::kXtxByteTableSize> seen_encoded{};
    std::vector<std::uint8_t> all_bytes(xtx::kXtxByteTableSize);
    for (std::size_t i = 0; i < xtx::kXtxByteTableSize; ++i) {
        all_bytes[i] = static_cast<std::uint8_t>(i);
        const std::uint8_t mapped = enc_tbl[i];
        EXPECT_FALSE(seen_encoded[mapped]) << "Duplicate encode table output at " << i;
        seen_encoded[mapped] = true;
        EXPECT_EQ(dec_tbl[mapped], static_cast<std::uint8_t>(i));
    }

    const auto encoded = xtx::encode_xtx(all_bytes);
    ASSERT_EQ(encoded.size(), all_bytes.size());
    EXPECT_EQ(xtx::decode_xtx(encoded), all_bytes);

    const Bytes plain_csv = utf8_to_cp932_replace("0,名前なし\r\n1,夢美\r\n");
    const Bytes ciphered_csv = xtx::encode_xtx(plain_csv);
    EXPECT_TRUE(xtx::is_plain_cp932_xtx(plain_csv));
    EXPECT_FALSE(xtx::is_plain_cp932_xtx(ciphered_csv));
    EXPECT_EQ(xtx::decode_xtx(ciphered_csv), plain_csv);
    EXPECT_FALSE(xtx::is_plain_cp932_xtx(Bytes{0x00u, 0x01u, 0x02u, 0x03u}));
}

TEST(XtxContract, TranslateXtxFileHandlesPlainAndCipheredInputsIdempotentlyAndRestores) {
    const ScopedTempDir tmp("anzu_ce_ut_xtx_dir");
    const fs::path& temp_dir = tmp.path();
    fs::create_directories(temp_dir / "spt");

    const std::string initial_utf8 =
        "no,名前なし\r\n"
        "aa,夢美\r\n"
        "bb,アゲハ\r\n"
        "za,夢美\r\n"
        "zb,アゲハ\r\n";
    const std::string expected_utf8 =
        "no,\r\n"
        "aa,Ｙｕｍｅｍｉ\r\n"
        "bb,Ａｇｅｈａ\r\n"
        "za,Ｙｕｍｅｍｉ\r\n"
        "zb,Ａｇｅｈａ\r\n";
    const Bytes initial_plain = utf8_to_cp932_replace(initial_utf8);

    // 1. Plain CP932 XTX: translates, preserves immutable backup, idempotent on replay,
    //    and restores pristine Japanese names from backup when translation table shrinks.
    {
        const std::string filepath = (temp_dir / "spt" / "charaname.xtx").u8string();
        const std::string backup_dir = (temp_dir / "xtx_backup").u8string();
        write_file(filepath, initial_plain);

        EXPECT_EQ(xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir),
                  kExpectedAnzUCharanameChanges);
        EXPECT_EQ(read_file((temp_dir / "xtx_backup" / "charaname.xtx").u8string()),
                  initial_plain);
        EXPECT_EQ(read_file((temp_dir / "xtx_backup" / "spt" / "charaname.xtx").u8string()),
                  initial_plain);
        EXPECT_EQ(cp932_to_utf8_strict(read_file(filepath)), expected_utf8);

        // Idempotent replay does not corrupt backup or double-mutate output.
        EXPECT_EQ(xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir),
                  kExpectedAnzUCharanameChanges);
        EXPECT_EQ(read_file((temp_dir / "xtx_backup" / "charaname.xtx").u8string()),
                  initial_plain);
        EXPECT_EQ(cp932_to_utf8_strict(read_file(filepath)), expected_utf8);

        // Shrinking/empty translation map restores original names from immutable backup.
        const std::vector<std::pair<std::string, std::string>> empty_map;
        EXPECT_EQ(xtx::translate_xtx_file(filepath, empty_map, backup_dir), 0);
        EXPECT_EQ(cp932_to_utf8_strict(read_file(filepath)), initial_utf8);
    }

    // 2. Ciphered XTX: decodes, translates, and re-encodes back into ciphered form.
    {
        const std::string filepath = (temp_dir / "spt" / "ciphered.xtx").u8string();
        const std::string backup_dir = (temp_dir / "xtx_backup_enc").u8string();
        const Bytes initial_ciphered = xtx::encode_xtx(initial_plain);
        write_file(filepath, initial_ciphered);

        EXPECT_EQ(xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir),
                  kExpectedAnzUCharanameChanges);
        const Bytes out_bytes = read_file(filepath);
        EXPECT_FALSE(xtx::is_plain_cp932_xtx(out_bytes));
        EXPECT_EQ(cp932_to_utf8_strict(xtx::decode_xtx(out_bytes)), expected_utf8);
        EXPECT_EQ(read_file((temp_dir / "xtx_backup_enc" / "ciphered.xtx").u8string()),
                  initial_ciphered);
    }
}
