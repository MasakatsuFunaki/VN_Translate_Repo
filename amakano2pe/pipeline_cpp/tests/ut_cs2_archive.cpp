// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Standalone unit tests for CatSystem2 KIF archives and CatScene scripts using
// synthetic archives built with MT19937-derived Blowfish keys, LE/BE block
// swapping, and zlib-deflated CatScene payloads.
#include "cs2/cs2_archive.h"

#include "common/util.h"

#ifndef OPENSSL_SUPPRESS_DEPRECATED
#  define OPENSSL_SUPPRESS_DEPRECATED
#endif
#include <gtest/gtest.h>
#include <openssl/blowfish.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace ama;

namespace {

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
constexpr std::uint32_t kSinglePayloadEntryCount = 2;
constexpr std::uint32_t kFirstPayloadEntryIndex = 1;

constexpr std::size_t kNonCatSceneJunkBytes = 32;
constexpr std::uint8_t kAsciiUpperA = 0x41;

constexpr std::uint32_t kSeedZero = 0u;
constexpr std::uint32_t kSeedOne = 1u;
constexpr std::uint32_t kSeedTwo = 2u;
constexpr std::uint32_t kSeed12345 = 12345u;
constexpr std::uint32_t kSeedHex1234 = 0x1234u;

constexpr std::uint32_t kExpectedMtZero = 477480905u;
constexpr std::uint32_t kExpectedMtOne = 3556162021u;
constexpr std::uint32_t kExpectedMtTwo = 1688516118u;
constexpr std::uint32_t kExpectedMt12345 = 3490534064u;
constexpr std::uint32_t kExpectedMtHex1234 = 3472957741u;

constexpr std::uint16_t kKindNone = 0;
constexpr std::uint16_t kKindInput = 0x0201;
constexpr std::uint16_t kKindPage = 0x0301;
constexpr std::uint16_t kKindUnknown = 0x1234;
constexpr std::uint16_t kKindMessage = 0x2001;
constexpr std::uint16_t kKindName = 0x2101;
constexpr std::uint16_t kKindCommand = 0x3001;

constexpr std::size_t kExpectedThreeLines = 3;

constexpr std::uint8_t kKifSignature[] = {'K', 'I', 'F', 0};
constexpr const char kKeyDatName[] = "__key__.dat";

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

// One CatScene line: uint16 kind + CP932 bytes + NUL.
struct RawLine {
    std::uint16_t kind;
    Bytes text;
};

// Build a valid "CatScene" file for the given lines.
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

// Build a KIF (.int) archive holding one payload under `name`.
Bytes make_kif(const std::string& name, const Bytes& payload, std::uint32_t seed) {
    BF_KEY key{};
    const std::uint32_t key_int = cs2::mt_genrand(seed);
    std::uint8_t key_le[kBlowfishKeyBytes] = {
        static_cast<std::uint8_t>(key_int),
        static_cast<std::uint8_t>(key_int >> kShift8),
        static_cast<std::uint8_t>(key_int >> kShift16),
        static_cast<std::uint8_t>(key_int >> kShift24),
    };
    BF_set_key(&key, static_cast<int>(kBlowfishKeyBytes), key_le);

    const std::uint32_t count = kSinglePayloadEntryCount;
    const std::uint32_t data_off =
        static_cast<std::uint32_t>(kKifHeaderBytes + count * kKifEntryBytes);

    Bytes stored = payload;
    const std::size_t aligned = stored.size() & ~kBlowfishAlignMask;
    for (std::size_t i = 0; i + kBlowfishBlockBytes <= aligned; i += kBlowfishBlockBytes) {
        const std::uint32_t l = rd_le32(stored, i);
        const std::uint32_t r = rd_le32(stored, i + kBlowfishHalfBlockBytes);
        std::uint8_t block[kBlowfishBlockBytes];
        std::uint8_t cipher[kBlowfishBlockBytes];
        put_be32(block, l);
        put_be32(block + kBlowfishHalfBlockBytes, r);
        BF_ecb_encrypt(block, cipher, &key, BF_ENCRYPT);
        wr_le32(stored, i, get_be32(cipher));
        wr_le32(stored, i + kBlowfishHalfBlockBytes,
                get_be32(cipher + kBlowfishHalfBlockBytes));
    }

    std::uint8_t plain[kBlowfishBlockBytes];
    std::uint8_t enc[kBlowfishBlockBytes];
    put_be32(plain, data_off);
    put_be32(plain + kBlowfishHalfBlockBytes, static_cast<std::uint32_t>(payload.size()));
    BF_ecb_encrypt(plain, enc, &key, BF_ENCRYPT);
    const std::uint32_t stored_off = get_be32(enc) - kFirstPayloadEntryIndex;
    const std::uint32_t stored_len = get_be32(enc + kBlowfishHalfBlockBytes);

    Bytes out;
    out.insert(out.end(), std::begin(kKifSignature), std::end(kKifSignature));
    put_u32(out, count);

    Bytes name0(kKifNameBytes, 0);
    std::memcpy(name0.data(), kKeyDatName, std::strlen(kKeyDatName));
    out.insert(out.end(), name0.begin(), name0.end());
    put_u32(out, 0);
    put_u32(out, seed);

    Bytes name1(kKifNameBytes, 0);
    std::memcpy(name1.data(), name.data(), name.size());
    out.insert(out.end(), name1.begin(), name1.end());
    put_u32(out, stored_off);
    put_u32(out, stored_len);

    out.insert(out.end(), stored.begin(), stored.end());
    return out;
}

}  // namespace

// The MT19937 key derivation is the linchpin: a wrong first word means every
// offset in the archive decrypts to garbage.
TEST(Cs2Archive, mt_genrand_is_deterministic) {
    EXPECT_EQ(cs2::mt_genrand(kSeedZero), kExpectedMtZero);
    EXPECT_EQ(cs2::mt_genrand(kSeedOne), kExpectedMtOne);
    EXPECT_EQ(cs2::mt_genrand(kSeedTwo), kExpectedMtTwo);
    EXPECT_EQ(cs2::mt_genrand(kSeed12345), kExpectedMt12345);
    EXPECT_EQ(cs2::mt_genrand(kSeedHex1234), kExpectedMtHex1234);
}

TEST(Cs2Archive, scene_line_type_names) {
    EXPECT_EQ(cs2::scene_line_type_name(kKindMessage), "MESSAGE");
    EXPECT_EQ(cs2::scene_line_type_name(kKindName), "NAME");
    EXPECT_EQ(cs2::scene_line_type_name(kKindCommand), "COMMAND");
    EXPECT_EQ(cs2::scene_line_type_name(kKindInput), "INPUT");
    EXPECT_EQ(cs2::scene_line_type_name(kKindPage), "PAGE");
    EXPECT_EQ(cs2::scene_line_type_name(kKindNone), "NONE");
    EXPECT_EQ(cs2::scene_line_type_name(kKindUnknown), "0x1234");
}

TEST(Cs2Archive, parses_catscene_lines) {
    const Bytes cst = make_catscene({
        {kKindName, utf8_to_cp932_replace("ちとせ")},
        {kKindMessage, utf8_to_cp932_replace("「こんにちは」")},
        {kKindCommand, Bytes{'f', 'w', ' ', '0'}},
    });

    const cs2::SceneScript scene(cst);
    ASSERT_EQ(scene.lines().size(), kExpectedThreeLines);
    EXPECT_EQ(scene.lines()[0].kind, kKindName);
    EXPECT_EQ(scene.lines()[0].idx, 0);
    EXPECT_EQ(scene.lines()[0].content, "ちとせ");
    EXPECT_EQ(scene.lines()[1].kind, kKindMessage);
    EXPECT_EQ(scene.lines()[1].content, "「こんにちは」");
    EXPECT_EQ(scene.lines()[2].content, "fw 0");
}

TEST(Cs2Archive, rejects_non_catscene) {
    const Bytes junk(kNonCatSceneJunkBytes, kAsciiUpperA);
    EXPECT_THROW(cs2::SceneScript{junk}, std::runtime_error);
}

TEST(Cs2Archive, kif_round_trip) {
    const ScopedTempDir temp("ama_ut_cs2_roundtrip");
    const Bytes cst = make_catscene({{kKindMessage, utf8_to_cp932_replace("あ")}});
    const Bytes kif = make_kif("01_TEST_01.cst", cst, kSeedHex1234);

    const std::string path = (temp.path() / "scene.int").u8string();
    write_file(path, kif);

    const cs2::KifArchive arc(path);
    ASSERT_EQ(arc.entries().size(), 1u) << "__key__.dat must not be listed";
    EXPECT_EQ(arc.entries()[0].name, "01_TEST_01.cst");
    EXPECT_EQ(arc.entries()[0].length, cst.size());

    const Bytes got = arc.extract(arc.entries()[0]);
    ASSERT_EQ(got.size(), cst.size());
    EXPECT_EQ(got, cst);

    const cs2::SceneScript scene(got);
    ASSERT_EQ(scene.lines().size(), 1u);
    EXPECT_EQ(scene.lines()[0].content, "あ");
}

TEST(Cs2Archive, rejects_bad_signature) {
    const ScopedTempDir temp("ama_ut_cs2_badsig");
    const Bytes bad{'X', 'I', 'F', 0, 1, 0, 0, 0};
    const std::string path = (temp.path() / "bad.int").u8string();
    write_file(path, bad);
    EXPECT_THROW(cs2::KifArchive{path}, std::runtime_error);
}

TEST(Cs2Archive, kif_v3_name_cipher_round_trip_and_slice_reader) {
    const cs2::KifNameCipher cipher("NXT-76BPQ5YD");
    const std::string plain_name = "sys_config_basic.hg3";
    constexpr std::uint32_t kEntryIdx = 1;
    const std::string obfuscated = cipher.encode(plain_name, kEntryIdx);
    EXPECT_NE(obfuscated, plain_name);
    EXPECT_EQ(cipher.decode(obfuscated, kEntryIdx), plain_name);

    const ScopedTempDir temp("ama_ut_cs2_v3_slice");
    const Bytes cst = make_catscene({{kKindMessage, utf8_to_cp932_replace("テスト")}});
    const Bytes kif = make_kif(obfuscated, cst, kSeed12345);
    const std::string path = (temp.path() / "image01.int").u8string();
    write_file(path, kif);

    const cs2::KifArchive arc(path, cipher);
    ASSERT_EQ(arc.entries().size(), 1u);
    EXPECT_EQ(arc.entries()[0].name, plain_name);
    EXPECT_EQ(arc.entries()[0].index, kEntryIdx);
    ASSERT_NE(arc.find("SYS_CONFIG_BASIC.HG3"), nullptr);

    const Bytes raw_slice = cs2::read_kif_raw_slice(
        path, arc.entries()[0].offset, arc.entries()[0].length);
    EXPECT_EQ(raw_slice, arc.raw_slice(arc.entries()[0]));
    const Bytes decrypted = arc.decrypt_slice(raw_slice);
    EXPECT_EQ(decrypted, cst);
}
