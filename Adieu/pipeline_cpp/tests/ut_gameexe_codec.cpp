// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Contract-driven unit tests for SiglusEngine Gameexe.dat decryption/encryption,
// runtime key extraction from SiglusEngine.exe, line-preserving GameexeDocument
// editing, and critical save-identity directive guards.
#include "g00/gameexe_codec.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || defined(__wincodec_h__)
#error "Windows header leaked"
#endif

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace adi::g00 {
namespace {

namespace fs = std::filesystem;

constexpr std::uint8_t kByteMask = 0xFFu;
constexpr std::size_t kPinnedKeyIdx0 = 0u;
constexpr std::size_t kPinnedKeyIdx1 = 1u;
constexpr std::size_t kPinnedKeyIdx16 = 16u;
constexpr std::uint8_t kSignSeam0x80 = 0x80u;
constexpr std::uint8_t kSignSeam0xFF = 0xFFu;
constexpr std::uint8_t kSignSeam0x7F = 0x7Fu;
constexpr std::size_t kWrongPrefixFlipIdx = 8u;
constexpr std::uint8_t kSiglusKeyTrapByte8 = 0xA6u;
constexpr std::size_t kExpectedFixtureLineCount = 8u;
constexpr std::size_t kExpectedFixtureDirectiveCount = 5u;
constexpr std::size_t kExpectedDuplicateUpdates = 2u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        dir_ = fs::temp_directory_path() /
               fs::u8path("adi_ut_gameexe_" + std::to_string(++seq));
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

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

GameexeKey256 BuildDeterministicKey256() {
    GameexeKey256 key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>((i * 0x5Du + 0x83u) & kByteMask);
    }
    for (std::size_t i = 0; i < kGameexeKeyPrefixBytes; ++i) {
        key[i] = kExpectedAdieuGameexeKeyPrefix[i];
    }
    return key;
}

void WriteBinaryFile(const fs::path& file_path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(file_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to create temporary test file");
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        throw std::runtime_error("failed to write temporary test file");
    }
}

}  // namespace

TEST(GameexeCodec, HandCraftedWireVectorAndDeterministicKeyRoundTrip) {
    // Pinned wire vector for UTF-16LE `u"\uFEFF#A=1"` (10 bytes):
    //   plain = {FF, FE, 23, 00, 41, 00, 3D, 00, 31, 00}
    //   framed LZSS (20 bytes) = {14,00,00,00, 0A,00,00,00, FF, FF,FE,23,00,41,00,3D,00, 03, 31,00}
    //   XORed at payload offset +0x08 with ADIEU_PRIVATE_KEY[i & 0x0F] ^ pinned_key[i & 0xFF],
    //   where pinned_key[0]=0x80, pinned_key[1]=0xFF, pinned_key[16]=0x7F (proving 16-byte vs
    //   256-byte key periods and 0x7F/0x80/0xFF sign-bit seams).
    GameexeKey256 pinned_key{};
    pinned_key[kPinnedKeyIdx0] = kSignSeam0x80;
    pinned_key[kPinnedKeyIdx1] = kSignSeam0xFF;
    pinned_key[kPinnedKeyIdx16] = kSignSeam0x7F;

    const std::vector<std::uint8_t> hand_crafted_dat = {
        0x00, 0x00, 0x00, 0x00,  // header_version = 0
        0x01, 0x00, 0x00, 0x00,  // use_private_key = 1
        0x36, 0x81, 0xC9, 0xDA, 0x24, 0x2D, 0x09, 0x08,
        0x1A, 0x15, 0x04, 0xB5, 0xA3, 0x5A, 0xED, 0xDD,
        0xDD, 0x7D, 0xF8, 0xDA,
    };

    const std::u16string decoded_pinned = DecodeGameexe(hand_crafted_dat, pinned_key);
    EXPECT_EQ(decoded_pinned, u"\uFEFF#A=1");
    EXPECT_EQ(GameexeUtf16ToUtf8(decoded_pinned, true), "#A=1");

    // Multi-directive Japanese round-trip exceeding 256 payload bytes so both
    // ADIEU_PRIVATE_KEY[i & 0x0F] and key256[i & 0xFF] wrap multiple times.
    const GameexeKey256 key256 = BuildDeterministicKey256();
    const std::string sample_utf8 =
        "; SiglusEngine 設定ファイル\r\n"
        "#GAMENAME = \"さよならを教えて\"\r\n"
        "#DISCMARK = \"sayonara.env\"\r\n"
        "#WARNINGINFO.RESTART_WARNING_STR = \"最初から始めてもよろしいですか？\"\r\n"
        "#SYSCOMMENU.READSKIP.STR = \"既読文章を早送り\"\r\n"
        "#DIALOG.CHRCOLOR.STR = \"文章を色分けする。\"\r\n"
        "#SAVE.NO_DATA_STR = \"データがありません。\"\r\n"
        "#CHRKOE.NOT_LOOK_NAME_STR = \"？？？\"\r\n"
        "#MWND.002.MOJI_SIZE = 24\r\n";

    const std::u16string sample_utf16 = GameexeUtf8ToUtf16(sample_utf8, true);
    ASSERT_FALSE(sample_utf16.empty());
    EXPECT_EQ(sample_utf16.front(), u'\uFEFF');

    const std::vector<std::uint8_t> encoded_priv =
        EncodeGameexe(sample_utf16, key256, kGameexeUsePrivateKey);
    EXPECT_EQ(DecodeGameexe(encoded_priv, key256), sample_utf16);
    EXPECT_EQ(GameexeUtf16ToUtf8(DecodeGameexe(encoded_priv, key256), true), sample_utf8);

    // Mode 0 (`use_private_key = 0`) round-trips without applying ADIEU_PRIVATE_KEY.
    const std::vector<std::uint8_t> encoded_pub = EncodeGameexe(sample_utf16, key256, 0u);
    EXPECT_NE(encoded_pub, encoded_priv);
    EXPECT_EQ(DecodeGameexe(encoded_pub, key256), sample_utf16);

    // Corrupt header version or truncated buffer must throw std::runtime_error.
    std::vector<std::uint8_t> bad_ver = hand_crafted_dat;
    bad_ver[0] = 0x02u;
    EXPECT_THROW(DecodeGameexe(bad_ver, pinned_key), std::runtime_error);

    const std::vector<std::uint8_t> short_dat = {0x00, 0x00, 0x00, 0x00, 0x01};
    EXPECT_THROW(DecodeGameexe(short_dat, pinned_key), std::runtime_error);
}

TEST(GameexeCodec, DocumentPreservesBomCommentsBlankLinesCrlfAndUpdatesDirectives) {
    const std::string raw_utf8 =
        "; 先頭コメント\r\n"
        "\r\n"
        "#GAMENAME = \"さよならを教えて\"   ; window caption\r\n"
        "#DISCMARK = \"sayonara.env\"\r\n"
        "  #MWND.002.MOJI_SIZE\t=\t24 ; font px\n"
        "#NOTE = \"a;b\" ; semicolon inside quotes\r\n"
        "#DUP.STR = \"旧1\"\r\n"
        "#DUP.STR = \"旧2\"";

    const std::u16string orig_utf16 = GameexeUtf8ToUtf16(raw_utf8, true);
    GameexeDocument doc = GameexeDocument::FromUtf16(orig_utf16);

    EXPECT_TRUE(doc.has_leading_bom());
    EXPECT_EQ(doc.line_count(), kExpectedFixtureLineCount);
    EXPECT_EQ(doc.directive_count(), kExpectedFixtureDirectiveCount + 1u);

    // Untouched document round-trips 100% unit-for-unit (BOM, comments, blank
    // lines, mixed \r\n and \n endings, and final line without trailing newline).
    EXPECT_EQ(doc.ToUtf16(), orig_utf16);
    EXPECT_EQ(doc.ToUtf8(false), raw_utf8);

    // Lookup with or without leading '#' and proper handling of quoted ';'.
    EXPECT_EQ(doc.FindDirectiveRawValue("#GAMENAME"),
              std::optional<std::string>("\"さよならを教えて\""));
    EXPECT_EQ(doc.FindDirectiveRawValue("DISCMARK"),
              std::optional<std::string>("\"sayonara.env\""));
    EXPECT_EQ(doc.FindDirectiveRawValue("#MWND.002.MOJI_SIZE"),
              std::optional<std::string>("24"));
    EXPECT_EQ(doc.FindDirectiveRawValue("NOTE"),
              std::optional<std::string>("\"a;b\""));
    EXPECT_EQ(doc.FindDirectiveRawValue("NON_EXISTENT"), std::nullopt);

    const std::vector<std::string> dup_before = doc.FindAllDirectiveRawValues("#DUP.STR");
    const std::vector<std::string> expected_dup_before = {"\"旧1\"", "\"旧2\""};
    EXPECT_EQ(dup_before, expected_dup_before);

    // Update string and raw directives while preserving indentation, = spacing,
    // inline ';' comments, and exact line endings.
    EXPECT_EQ(doc.SetDirectiveStringValue("#GAMENAME", "Comment te dire adieu"), 1u);
    EXPECT_EQ(doc.SetDirectiveRawValue("MWND.002.MOJI_SIZE", "20"), 1u);
    EXPECT_EQ(doc.SetDirectiveStringValue("DUP.STR", "Updated"), kExpectedDuplicateUpdates);
    EXPECT_EQ(doc.SetDirectiveStringValue("#MISSING_KEY", "Nope"), 0u);

    const std::string expected_patched_utf8 =
        "; 先頭コメント\r\n"
        "\r\n"
        "#GAMENAME = \"Comment te dire adieu\"   ; window caption\r\n"
        "#DISCMARK = \"sayonara.env\"\r\n"
        "  #MWND.002.MOJI_SIZE\t=\t20 ; font px\n"
        "#NOTE = \"a;b\" ; semicolon inside quotes\r\n"
        "#DUP.STR = \"Updated\"\r\n"
        "#DUP.STR = \"Updated\"";
    EXPECT_EQ(doc.ToUtf8(false), expected_patched_utf8);
}

TEST(GameexeCodec, VerifyCriticalDirectivesGuardBlocksIdentityDriftAndAllowsUiEdits) {
    const std::string orig =
        "#GAMENAME = \"さよならを教えて\"\r\n"
        "#DISCMARK = \"sayonara.env\"\r\n"
        "#EXENAME = \"SiglusEngine.exe\"\r\n"
        "#SAVEDIR = \"savedata\"\r\n"
        "#SAVE.CNT = 100\r\n"
        "#SAVE.NO_DATA_STR = \"データがありません。\"\r\n"
        "#SAVEINFO.WARNING_STR = \"セーブしてもよろしいですか？\"\r\n"
        "#MWND.002.MOJI_SIZE = 24\r\n"
        "#MWND.002.MOJI_CNT = 26, 16\r\n"
        "#SELBTN.000.MOJI_SIZE = 30\r\n";

    GameexeDocument allowed = GameexeDocument::FromUtf8(orig);
    ASSERT_EQ(allowed.SetDirectiveStringValue("#GAMENAME", "Comment te dire adieu"), 1u);
    ASSERT_EQ(allowed.SetDirectiveStringValue("#SAVE.NO_DATA_STR", "No Data"), 1u);
    ASSERT_EQ(allowed.SetDirectiveStringValue("#SAVEINFO.WARNING_STR", "Overwrite save?"), 1u);
    ASSERT_EQ(allowed.SetDirectiveRawValue("#MWND.002.MOJI_SIZE", "20"), 1u);
    ASSERT_EQ(allowed.SetDirectiveRawValue("#MWND.002.MOJI_CNT", "30, 19"), 1u);
    ASSERT_EQ(allowed.SetDirectiveRawValue("#SELBTN.000.MOJI_SIZE", "22"), 1u);

    VerifyCriticalDirectivesUnchanged(orig, allowed.ToUtf8(false));
    EXPECT_EQ(allowed.FindDirectiveRawValue("#DISCMARK"),
              std::optional<std::string>("\"sayonara.env\""));

    struct ForbiddenMutation {
        const char* label;
        std::string patched_text;
    };

    GameexeDocument bad_discmark = GameexeDocument::FromUtf8(orig);
    bad_discmark.SetDirectiveStringValue("#DISCMARK", "adieu_en.env");

    GameexeDocument bad_exename = GameexeDocument::FromUtf8(orig);
    bad_exename.SetDirectiveStringValue("#EXENAME", "Adieu.exe");

    GameexeDocument bad_savedir = GameexeDocument::FromUtf8(orig);
    bad_savedir.SetDirectiveStringValue("#SAVEDIR", "savedata_en");

    GameexeDocument bad_save_cnt = GameexeDocument::FromUtf8(orig);
    bad_save_cnt.SetDirectiveRawValue("#SAVE.CNT", "200");

    const std::string added_reg_name = orig + "#REG_NAME = \"CRAFTWORK\\Adieu\"\r\n";
    const std::string orig_with_reg = orig + "#REG_NAME = \"CRAFTWORK\\Adieu\"\r\n";
    GameexeDocument mutated_reg = GameexeDocument::FromUtf8(orig_with_reg);
    mutated_reg.SetDirectiveStringValue("#REG_NAME", "CRAFTWORK\\AdieuEN");

    const std::vector<ForbiddenMutation> forbidden_cases = {
        {"discmark_changed", bad_discmark.ToUtf8(false)},
        {"exename_changed", bad_exename.ToUtf8(false)},
        {"savedir_changed", bad_savedir.ToUtf8(false)},
        {"save_cnt_structure_changed", bad_save_cnt.ToUtf8(false)},
        {"reg_name_added", added_reg_name},
    };

    ASSERT_EQ(forbidden_cases.size(), 5u);
    for (const ForbiddenMutation& tc : forbidden_cases) {
        SCOPED_TRACE(tc.label);
        EXPECT_THROW(VerifyCriticalDirectivesUnchanged(orig, tc.patched_text),
                     std::runtime_error);
    }
    EXPECT_THROW(
        VerifyCriticalDirectivesUnchanged(orig_with_reg, mutated_reg.ToUtf8(false)),
        std::runtime_error);
}

TEST(GameexeCodec, LoadGameexeKeyFromExeValidatesOffsetAndPrefix) {
    ScopedTempDir sandbox;
    const GameexeKey256 expected_key = BuildDeterministicKey256();
    const std::size_t exact_valid_size =
        static_cast<std::size_t>(kAdieuGameexeKeyFileOffset) + kGameexeKey256Size;

    // 1. Valid synthetic EXE with exact prefix D8 29 B9 16 3D 1A 76 D0 87 9B 2D 0C 7B D1 A9 19
    //    at offset 0x5F7D90.
    std::vector<std::uint8_t> exe_image(exact_valid_size, 0u);
    for (std::size_t i = 0; i < kGameexeKey256Size; ++i) {
        exe_image[static_cast<std::size_t>(kAdieuGameexeKeyFileOffset) + i] = expected_key[i];
    }
    const fs::path valid_exe = sandbox.path() / "SiglusEngine_valid.exe";
    WriteBinaryFile(valid_exe, exe_image);
    EXPECT_EQ(LoadGameexeKeyFromExe(valid_exe), expected_key);

    // 2. Wrong prefix at 0x5F7D90 (e.g. byte 8 = 0xA6 from SIGLUS_KEY_256 instead of 0x87).
    std::vector<std::uint8_t> wrong_prefix_image = exe_image;
    wrong_prefix_image[static_cast<std::size_t>(kAdieuGameexeKeyFileOffset) +
                       kWrongPrefixFlipIdx] = kSiglusKeyTrapByte8;
    const fs::path wrong_prefix_exe = sandbox.path() / "SiglusEngine_wrong_prefix.exe";
    WriteBinaryFile(wrong_prefix_exe, wrong_prefix_image);
    EXPECT_THROW(LoadGameexeKeyFromExe(wrong_prefix_exe), std::runtime_error);

    // 3. Short file (`0x5F7D90 + 255` bytes — 1 byte shy of full 256-byte table).
    const std::vector<std::uint8_t> short_image(exact_valid_size - 1u, 0u);
    const fs::path short_exe = sandbox.path() / "SiglusEngine_short.exe";
    WriteBinaryFile(short_exe, short_image);
    EXPECT_THROW(LoadGameexeKeyFromExe(short_exe), std::runtime_error);

    // 4. Missing file path.
    EXPECT_THROW(LoadGameexeKeyFromExe(sandbox.path() / "Missing.exe"), std::runtime_error);
}

}  // namespace adi::g00
