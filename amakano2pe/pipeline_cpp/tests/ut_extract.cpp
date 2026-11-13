// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Standalone unit tests for ama::extract::run_extract over synthetic KIF
// scene.int archives written to temporary directories.
#include "extract/extractor.h"

#include "common/util.h"
#include "cs2/cs2_archive.h"

#ifndef OPENSSL_SUPPRESS_DEPRECATED
#  define OPENSSL_SUPPRESS_DEPRECATED
#endif
#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <openssl/blowfish.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace ama;

namespace {

constexpr int kExitOk = 0;

constexpr int kShift8 = 8;
constexpr int kShift16 = 16;
constexpr int kShift24 = 24;

constexpr std::size_t kOffsetEntryBytes = 4;
constexpr std::size_t kBlowfishKeyBytes = 4;
constexpr std::size_t kBlowfishHalfBlockBytes = 4;
constexpr std::size_t kBlowfishBlockBytes = 8;
constexpr std::size_t kBlowfishAlignMask = 7;

constexpr std::size_t kCatSceneSigBytes = 8;
constexpr std::size_t kKifHeaderBytes = 8;
constexpr std::size_t kKifNameBytes = 64;
constexpr std::size_t kKifEntryBytes = 72;

constexpr std::uint32_t kArchiveSeed = 0x2468ACE0u;

constexpr std::uint16_t kKindPage = 0x0301;
constexpr std::uint16_t kKindMessage = 0x2001;
constexpr std::uint16_t kKindName = 0x2101;
constexpr std::uint16_t kKindCommand = 0x3001;

constexpr std::size_t kPrologueLineCount = 3;
constexpr std::size_t kSecondSceneLineCount = 2;
constexpr std::size_t kCorruptPayloadBytes = 24;
constexpr std::uint8_t kCorruptFillByte = 0x5A;

constexpr std::uint8_t kKifSignature[] = {'K', 'I', 'F', 0};
constexpr const char kKeyDatName[] = "__key__.dat";
constexpr const char kJapaneseDirSegment[] =
    "あざらしそふと_アマカノ2～Perfect Edition～";

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
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

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void put_u16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kShift8));
}

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kShift8));
    b.push_back(static_cast<std::uint8_t>(v >> kShift16));
    b.push_back(static_cast<std::uint8_t>(v >> kShift24));
}

void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> kShift24);
    p[1] = static_cast<std::uint8_t>(v >> kShift16);
    p[2] = static_cast<std::uint8_t>(v >> kShift8);
    p[3] = static_cast<std::uint8_t>(v);
}

std::uint32_t get_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << kShift24) |
           (static_cast<std::uint32_t>(p[1]) << kShift16) |
           (static_cast<std::uint32_t>(p[2]) << kShift8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint32_t rd_le32(const Bytes& b, std::size_t i) {
    return static_cast<std::uint32_t>(b[i]) |
           (static_cast<std::uint32_t>(b[i + 1]) << kShift8) |
           (static_cast<std::uint32_t>(b[i + 2]) << kShift16) |
           (static_cast<std::uint32_t>(b[i + 3]) << kShift24);
}

void wr_le32(Bytes& b, std::size_t i, std::uint32_t v) {
    b[i] = static_cast<std::uint8_t>(v);
    b[i + 1] = static_cast<std::uint8_t>(v >> kShift8);
    b[i + 2] = static_cast<std::uint8_t>(v >> kShift16);
    b[i + 3] = static_cast<std::uint8_t>(v >> kShift24);
}

struct RawLine {
    std::uint16_t kind;
    Bytes text;
};

Bytes make_catscene(const std::vector<RawLine>& lines) {
    Bytes blob;
    std::vector<std::uint32_t> offs;
    for (const auto& l : lines) {
        offs.push_back(static_cast<std::uint32_t>(blob.size()));
        put_u16(blob, l.kind);
        blob.insert(blob.end(), l.text.begin(), l.text.end());
        blob.push_back(0);
    }

    const std::uint32_t table_off = 0;
    const std::uint32_t string_off =
        static_cast<std::uint32_t>(offs.size() * kOffsetEntryBytes);

    Bytes script;
    put_u32(script, 0);
    put_u32(script, 0);
    put_u32(script, table_off);
    put_u32(script, string_off);
    for (const std::uint32_t o : offs) {
        put_u32(script, o);
    }
    script.insert(script.end(), blob.begin(), blob.end());

    uLongf bound = compressBound(static_cast<uLong>(script.size()));
    Bytes comp(bound);
    EXPECT_EQ(compress(comp.data(), &bound, script.data(),
                       static_cast<uLong>(script.size())),
              Z_OK);
    comp.resize(bound);

    Bytes out;
    const char* sig = "CatScene";
    out.insert(out.end(), sig, sig + kCatSceneSigBytes);
    put_u32(out, static_cast<std::uint32_t>(comp.size()));
    put_u32(out, static_cast<std::uint32_t>(script.size()));
    out.insert(out.end(), comp.begin(), comp.end());
    return out;
}

Bytes make_kif_archive(const std::vector<std::pair<std::string, Bytes>>& files,
                       std::uint32_t seed) {
    BF_KEY key{};
    const std::uint32_t key_int = cs2::mt_genrand(seed);
    std::uint8_t key_le[kBlowfishKeyBytes] = {
        static_cast<std::uint8_t>(key_int),
        static_cast<std::uint8_t>(key_int >> kShift8),
        static_cast<std::uint8_t>(key_int >> kShift16),
        static_cast<std::uint8_t>(key_int >> kShift24),
    };
    BF_set_key(&key, static_cast<int>(kBlowfishKeyBytes), key_le);

    const std::uint32_t count = static_cast<std::uint32_t>(files.size() + 1);
    std::uint32_t next_off =
        static_cast<std::uint32_t>(kKifHeaderBytes + count * kKifEntryBytes);

    Bytes out;
    out.insert(out.end(), std::begin(kKifSignature), std::end(kKifSignature));
    put_u32(out, count);

    Bytes key_entry_name(kKifNameBytes, 0);
    std::memcpy(key_entry_name.data(), kKeyDatName, std::strlen(kKeyDatName));
    out.insert(out.end(), key_entry_name.begin(), key_entry_name.end());
    put_u32(out, 0);
    put_u32(out, seed);

    Bytes concatenated_payloads;
    for (std::size_t idx = 0; idx < files.size(); ++idx) {
        const auto& [name, payload] = files[idx];
        const std::uint32_t entry_index = static_cast<std::uint32_t>(idx + 1);

        std::uint8_t plain[kBlowfishBlockBytes];
        std::uint8_t enc[kBlowfishBlockBytes];
        put_be32(plain, next_off);
        put_be32(plain + kBlowfishHalfBlockBytes, static_cast<std::uint32_t>(payload.size()));
        BF_ecb_encrypt(plain, enc, &key, BF_ENCRYPT);
        const std::uint32_t stored_off = get_be32(enc) - entry_index;
        const std::uint32_t stored_len = get_be32(enc + kBlowfishHalfBlockBytes);

        Bytes entry_name(kKifNameBytes, 0);
        std::memcpy(entry_name.data(), name.data(), name.size());
        out.insert(out.end(), entry_name.begin(), entry_name.end());
        put_u32(out, stored_off);
        put_u32(out, stored_len);

        Bytes stored = payload;
        const std::size_t aligned = stored.size() & ~kBlowfishAlignMask;
        for (std::size_t b = 0; b + kBlowfishBlockBytes <= aligned;
             b += kBlowfishBlockBytes) {
            const std::uint32_t l = rd_le32(stored, b);
            const std::uint32_t r = rd_le32(stored, b + kBlowfishHalfBlockBytes);
            std::uint8_t block[kBlowfishBlockBytes];
            std::uint8_t cipher[kBlowfishBlockBytes];
            put_be32(block, l);
            put_be32(block + kBlowfishHalfBlockBytes, r);
            BF_ecb_encrypt(block, cipher, &key, BF_ENCRYPT);
            wr_le32(stored, b, get_be32(cipher));
            wr_le32(stored, b + kBlowfishHalfBlockBytes,
                    get_be32(cipher + kBlowfishHalfBlockBytes));
        }

        concatenated_payloads.insert(concatenated_payloads.end(),
                                     stored.begin(), stored.end());
        next_off += static_cast<std::uint32_t>(payload.size());
    }

    out.insert(out.end(), concatenated_payloads.begin(), concatenated_payloads.end());
    return out;
}

std::vector<std::string> object_keys(const bj::object& obj) {
    std::vector<std::string> keys;
    keys.reserve(obj.size());
    for (const auto& kv : obj) {
        keys.emplace_back(kv.key());
    }
    return keys;
}

}  // namespace

TEST(Extract, ExtractsSyntheticSceneArchiveWithExpectedSchemaAndJapaneseContent) {
    const ScopedTempDir temp("ama_ut_extract_schema");
    const std::string scene_int = (temp.path() / "scene.int").u8string();
    const std::string out_json = (temp.path() / "extracted_text.json").u8string();

    const Bytes prologue_cst = make_catscene({
        {kKindName, utf8_to_cp932_replace("ちとせ")},
        {kKindMessage, utf8_to_cp932_replace("「こんにちは」")},
        {kKindCommand, Bytes{'f', 'w', ' ', '0'}},
    });
    const Bytes second_cst = make_catscene({
        {kKindPage, Bytes{}},
        {kKindMessage, utf8_to_cp932_replace("朝が来た。")},
    });

    write_file(scene_int,
               make_kif_archive(
                   {
                       {"01_PROLOGUE.cst", prologue_cst},
                       {"02_SCENE.cst", second_cst},
                   },
                   kArchiveSeed));

    ASSERT_EQ(ama::extract::run_extract(scene_int, out_json), kExitOk);
    ASSERT_TRUE(fs::exists(fs::u8path(out_json)));

    const bj::value root_val = json_parse_file(out_json);
    ASSERT_TRUE(root_val.is_object());
    const auto& root = root_val.get_object();
    ASSERT_EQ(root.size(), 2u);
    ASSERT_TRUE(root.contains("01_PROLOGUE.cst"));
    ASSERT_TRUE(root.contains("02_SCENE.cst"));

    const std::vector<std::string> expected_script_keys = {
        "arc_idx", "name", "offset", "size", "lines"};
    const std::vector<std::string> expected_line_keys = {
        "idx", "type", "type_val", "content"};

    const auto& s0 = root.at("01_PROLOGUE.cst").get_object();
    EXPECT_EQ(object_keys(s0), expected_script_keys);
    EXPECT_EQ(s0.at("arc_idx").get_int64(), 0);
    EXPECT_EQ(std::string(s0.at("name").get_string()), "01_PROLOGUE.cst");
    EXPECT_GT(s0.at("offset").get_int64(), 0);
    EXPECT_EQ(s0.at("size").get_int64(), static_cast<std::int64_t>(prologue_cst.size()));

    const auto& s0_lines = s0.at("lines").get_array();
    ASSERT_EQ(s0_lines.size(), kPrologueLineCount);
    for (const auto& lv : s0_lines) {
        EXPECT_EQ(object_keys(lv.get_object()), expected_line_keys);
    }

    EXPECT_EQ(s0_lines[0].get_object().at("idx").get_int64(), 0);
    EXPECT_EQ(std::string(s0_lines[0].get_object().at("type").get_string()), "NAME");
    EXPECT_EQ(s0_lines[0].get_object().at("type_val").get_int64(), kKindName);
    EXPECT_EQ(std::string(s0_lines[0].get_object().at("content").get_string()), "ちとせ");

    EXPECT_EQ(s0_lines[1].get_object().at("idx").get_int64(), 1);
    EXPECT_EQ(std::string(s0_lines[1].get_object().at("type").get_string()), "MESSAGE");
    EXPECT_EQ(s0_lines[1].get_object().at("type_val").get_int64(), kKindMessage);
    EXPECT_EQ(std::string(s0_lines[1].get_object().at("content").get_string()), "「こんにちは」");

    EXPECT_EQ(s0_lines[2].get_object().at("idx").get_int64(), 2);
    EXPECT_EQ(std::string(s0_lines[2].get_object().at("type").get_string()), "COMMAND");
    EXPECT_EQ(s0_lines[2].get_object().at("type_val").get_int64(), kKindCommand);
    EXPECT_EQ(std::string(s0_lines[2].get_object().at("content").get_string()), "fw 0");

    const auto& s1 = root.at("02_SCENE.cst").get_object();
    EXPECT_EQ(object_keys(s1), expected_script_keys);
    EXPECT_EQ(s1.at("arc_idx").get_int64(), 1);
    EXPECT_EQ(std::string(s1.at("name").get_string()), "02_SCENE.cst");
    EXPECT_GT(s1.at("offset").get_int64(), s0.at("offset").get_int64());
    EXPECT_EQ(s1.at("size").get_int64(), static_cast<std::int64_t>(second_cst.size()));

    const auto& s1_lines = s1.at("lines").get_array();
    ASSERT_EQ(s1_lines.size(), kSecondSceneLineCount);
    EXPECT_EQ(std::string(s1_lines[0].get_object().at("type").get_string()), "PAGE");
    EXPECT_EQ(std::string(s1_lines[1].get_object().at("content").get_string()), "朝が来た。");
}

TEST(Extract, SkipsCorruptEntryAndExtractsRemainingValidScripts) {
    const ScopedTempDir temp("ama_ut_extract_corrupt");
    const std::string scene_int = (temp.path() / "scene.int").u8string();
    const std::string out_json = (temp.path() / "extracted_text.json").u8string();

    const Bytes bad_entry(kCorruptPayloadBytes, kCorruptFillByte);
    const Bytes good_cst = make_catscene({
        {kKindName, utf8_to_cp932_replace("ちとせ")},
        {kKindMessage, utf8_to_cp932_replace("「こんにちは」")},
    });

    write_file(scene_int,
               make_kif_archive(
                   {
                       {"00_CORRUPT.cst", bad_entry},
                       {"01_VALID.cst", good_cst},
                   },
                   kArchiveSeed));

    ASSERT_EQ(ama::extract::run_extract(scene_int, out_json), kExitOk);
    const bj::object root = json_parse_file(out_json).get_object();
    EXPECT_FALSE(root.contains("00_CORRUPT.cst"));
    ASSERT_TRUE(root.contains("01_VALID.cst"));
    const auto& valid_script = root.at("01_VALID.cst").get_object();
    EXPECT_EQ(valid_script.at("arc_idx").get_int64(), 1);
}

TEST(Extract, SupportsJapaneseDirectoryPathsForInputAndOutput) {
    const ScopedTempDir temp("ama_ut_extract_jp_dir");
    const fs::path jp_dir = temp.path() / fs::u8path(kJapaneseDirSegment);
    fs::create_directories(jp_dir);

    const std::string scene_int = (jp_dir / "scene.int").u8string();
    const std::string out_json = (jp_dir / "extracted_text.json").u8string();

    const Bytes cst = make_catscene({
        {kKindName, utf8_to_cp932_replace("ちとせ")},
        {kKindMessage, utf8_to_cp932_replace("「こんにちは」")},
    });
    write_file(scene_int, make_kif_archive({{"01_JP_DIR.cst", cst}}, kArchiveSeed));

    ASSERT_EQ(ama::extract::run_extract(scene_int, out_json), kExitOk);
    const bj::object root = json_parse_file(out_json).get_object();
    ASSERT_TRUE(root.contains("01_JP_DIR.cst"));
    const auto& lines = root.at("01_JP_DIR.cst").get_object().at("lines").get_array();
    ASSERT_EQ(lines.size(), kSecondSceneLineCount);
    EXPECT_EQ(std::string(lines[0].get_object().at("content").get_string()), "ちとせ");
    EXPECT_EQ(std::string(lines[1].get_object().at("content").get_string()), "「こんにちは」");
}
