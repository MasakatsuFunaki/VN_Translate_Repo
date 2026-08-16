// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CatSystem2 KIF archive + CatScene script tests.
//
// The real scene.int is only present on a machine with the game installed, so
// these build a synthetic archive with the same construction the engine uses
// (MT19937-derived Blowfish key, LE/BE block swapping, zlib-deflated CatScene
// payload) and round-trip it back through the reader.  The real archive is
// covered by the gated test at the bottom of this file.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/blowfish.h>
#include <zlib.h>

#include "common/util.h"
#include "cs2/cs2_archive.h"

namespace fs = std::filesystem;
using namespace ama;

namespace {

void put_u16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}
void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
std::uint32_t get_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// One CatScene line: uint16 kind + CP932 bytes + NUL.
struct RawLine {
    std::uint16_t kind;
    Bytes text;
};

// Build a valid "CatScene" file for the given lines.
Bytes make_catscene(const std::vector<RawLine>& lines) {
    // String blob + per-line offsets (relative to the blob start).
    Bytes blob;
    std::vector<std::uint32_t> offs;
    for (const auto& l : lines) {
        offs.push_back(static_cast<std::uint32_t>(blob.size()));
        put_u16(blob, l.kind);
        blob.insert(blob.end(), l.text.begin(), l.text.end());
        blob.push_back(0);
    }

    // Body layout after the 16-byte inner header: offsets table then blob.
    const std::uint32_t table_off = 0;
    const std::uint32_t string_off = static_cast<std::uint32_t>(offs.size() * 4);

    Bytes script;
    put_u32(script, 0);          // script_len (unused by the reader)
    put_u32(script, 0);          // input_num
    put_u32(script, table_off);
    put_u32(script, string_off);
    for (std::uint32_t o : offs) put_u32(script, o);
    script.insert(script.end(), blob.begin(), blob.end());

    // Deflate it.
    uLongf bound = compressBound(static_cast<uLong>(script.size()));
    Bytes comp(bound);
    EXPECT_EQ(compress(comp.data(), &bound, script.data(),
                       static_cast<uLong>(script.size())),
              Z_OK);
    comp.resize(bound);

    Bytes out;
    const char* sig = "CatScene";
    out.insert(out.end(), sig, sig + 8);
    put_u32(out, static_cast<std::uint32_t>(comp.size()));
    put_u32(out, static_cast<std::uint32_t>(script.size()));
    out.insert(out.end(), comp.begin(), comp.end());
    return out;
}

// Build a KIF (.int) archive holding one payload under `name`.
// Mirrors the engine's construction: __key__.dat's length field is the
// MT19937 seed, entry (offset+i, length) pairs are Blowfish-encrypted as a
// big-endian LR block, and payload blocks are LE-stored but BE-ciphered.
Bytes make_kif(const std::string& name, const Bytes& payload, std::uint32_t seed) {
    BF_KEY key{};
    const std::uint32_t key_int = cs2::mt_genrand(seed);
    std::uint8_t key_le[4] = {static_cast<std::uint8_t>(key_int),
                              static_cast<std::uint8_t>(key_int >> 8),
                              static_cast<std::uint8_t>(key_int >> 16),
                              static_cast<std::uint8_t>(key_int >> 24)};
    BF_set_key(&key, 4, key_le);

    constexpr std::size_t ENTRY_SIZE = 72;
    const std::uint32_t count = 2;
    const std::uint32_t data_off = static_cast<std::uint32_t>(8 + count * ENTRY_SIZE);

    // Encrypt the payload the way the archive stores it.
    Bytes stored = payload;
    const std::size_t aligned = stored.size() & ~std::size_t{7};
    for (std::size_t i = 0; i + 8 <= aligned; i += 8) {
        const std::uint32_t l = stored[i] | (stored[i + 1] << 8) | (stored[i + 2] << 16) |
                                (static_cast<std::uint32_t>(stored[i + 3]) << 24);
        const std::uint32_t r = stored[i + 4] | (stored[i + 5] << 8) | (stored[i + 6] << 16) |
                                (static_cast<std::uint32_t>(stored[i + 7]) << 24);
        std::uint8_t block[8], cipher[8];
        put_be32(block, l);
        put_be32(block + 4, r);
        BF_ecb_encrypt(block, cipher, &key, BF_ENCRYPT);
        const std::uint32_t cl = get_be32(cipher), cr = get_be32(cipher + 4);
        stored[i + 0] = static_cast<std::uint8_t>(cl);
        stored[i + 1] = static_cast<std::uint8_t>(cl >> 8);
        stored[i + 2] = static_cast<std::uint8_t>(cl >> 16);
        stored[i + 3] = static_cast<std::uint8_t>(cl >> 24);
        stored[i + 4] = static_cast<std::uint8_t>(cr);
        stored[i + 5] = static_cast<std::uint8_t>(cr >> 8);
        stored[i + 6] = static_cast<std::uint8_t>(cr >> 16);
        stored[i + 7] = static_cast<std::uint8_t>(cr >> 24);
    }

    // Entry 1's (offset, length) must decrypt to the true values, so encrypt
    // the true pair and undo the +index the reader applies to the offset.
    std::uint8_t plain[8], enc[8];
    put_be32(plain, data_off);
    put_be32(plain + 4, static_cast<std::uint32_t>(payload.size()));
    BF_ecb_encrypt(plain, enc, &key, BF_ENCRYPT);
    const std::uint32_t stored_off = get_be32(enc) - 1;  // entry index is 1
    const std::uint32_t stored_len = get_be32(enc + 4);

    Bytes out;
    const char* sig = "KIF\x00";
    out.insert(out.end(), sig, sig + 4);
    put_u32(out, count);

    Bytes name0(64, 0);
    std::memcpy(name0.data(), "__key__.dat", 11);
    out.insert(out.end(), name0.begin(), name0.end());
    put_u32(out, 0);      // __key__.dat offset (ignored)
    put_u32(out, seed);   // ...its length IS the MT seed

    Bytes name1(64, 0);
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
    // Pinned expectations -- re-deriving them from a changed implementation
    // would defeat the point of the test.
    EXPECT_EQ(cs2::mt_genrand(0u), 477480905u);
    EXPECT_EQ(cs2::mt_genrand(1u), 3556162021u);
    EXPECT_EQ(cs2::mt_genrand(2u), 1688516118u);
    EXPECT_EQ(cs2::mt_genrand(12345u), 3490534064u);
    EXPECT_EQ(cs2::mt_genrand(0x1234u), 3472957741u);
}

TEST(Cs2Archive, scene_line_type_names) {
    EXPECT_EQ(cs2::scene_line_type_name(0x2001), "MESSAGE");
    EXPECT_EQ(cs2::scene_line_type_name(0x2101), "NAME");
    EXPECT_EQ(cs2::scene_line_type_name(0x3001), "COMMAND");
    EXPECT_EQ(cs2::scene_line_type_name(0x0201), "INPUT");
    EXPECT_EQ(cs2::scene_line_type_name(0x0301), "PAGE");
    EXPECT_EQ(cs2::scene_line_type_name(0), "NONE");
    // Unknown kinds fall back to the raw hex form.
    EXPECT_EQ(cs2::scene_line_type_name(0x1234), "0x1234");
}

TEST(Cs2Archive, parses_catscene_lines) {
    // 「こんにちは」 in CP932 + an ASCII COMMAND.
    const Bytes jp{0x81, 0x75, 0x82, 0xB1, 0x82, 0xF1, 0x82, 0xC9,
                   0x82, 0xBF, 0x82, 0xCD, 0x81, 0x76};
    Bytes cst = make_catscene({
        {0x2101, Bytes{0x8E, 0x71, 0x82, 0xC6, 0x82, 0xB9}},  // NAME
        {0x2001, jp},                                          // MESSAGE
        {0x3001, Bytes{'f', 'w', ' ', '0'}},                   // COMMAND
    });

    cs2::SceneScript scene(cst);
    ASSERT_EQ(scene.lines().size(), 3u);
    EXPECT_EQ(scene.lines()[0].kind, 0x2101);
    EXPECT_EQ(scene.lines()[0].idx, 0);
    EXPECT_EQ(scene.lines()[1].kind, 0x2001);
    EXPECT_EQ(scene.lines()[1].content,
              "\xE3\x80\x8C\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"
              "\xE3\x80\x8D");
    EXPECT_EQ(scene.lines()[2].content, "fw 0");
}

TEST(Cs2Archive, rejects_non_catscene) {
    Bytes junk(32, 0x41);
    EXPECT_THROW(cs2::SceneScript{junk}, std::runtime_error);
}

TEST(Cs2Archive, kif_round_trip) {
    Bytes cst = make_catscene({{0x2001, Bytes{0x82, 0xA0}}});  // あ
    Bytes kif = make_kif("01_TEST_01.cst", cst, /*seed=*/0x1234u);

    const std::string path = (fs::temp_directory_path() / "ama_ut_scene.int").u8string();
    write_file(path, kif);

    cs2::KifArchive arc(path);
    ASSERT_EQ(arc.entries().size(), 1u) << "__key__.dat must not be listed";
    EXPECT_EQ(arc.entries()[0].name, "01_TEST_01.cst");
    EXPECT_EQ(arc.entries()[0].length, cst.size());

    Bytes got = arc.extract(arc.entries()[0]);
    // Only the 8-byte-aligned prefix is enciphered; the tail is stored raw and
    // must survive untouched.
    ASSERT_EQ(got.size(), cst.size());
    EXPECT_EQ(got, cst);

    cs2::SceneScript scene(got);
    ASSERT_EQ(scene.lines().size(), 1u);
    EXPECT_EQ(scene.lines()[0].content, "\xE3\x81\x82");

    fs::remove(fs::u8path(path));
}

TEST(Cs2Archive, rejects_bad_signature) {
    Bytes bad{'X', 'I', 'F', 0, 1, 0, 0, 0};
    const std::string path = (fs::temp_directory_path() / "ama_ut_bad.int").u8string();
    write_file(path, bad);
    EXPECT_THROW(cs2::KifArchive{path}, std::runtime_error);
    fs::remove(fs::u8path(path));
}

// Real-archive gate: skips cleanly when the game isn't installed.
TEST(Cs2Archive, real_scene_int_opens) {
    fs::path p = fs::u8path(std::string(AMA_PROJECT_DIR));  // only for the message
    fs::path scene = fs::u8path(
        "C:\\\xE3\x81\x82\xE3\x81\x96\xE3\x82\x89\xE3\x81\x97\xE3\x81\x9D\xE3\x81\xB5"
        "\xE3\x81\xA8\\\xE3\x82\xA2\xE3\x83\x9E\xE3\x82\xAB\xE3\x83\x8E"
        "2\xEF\xBD\x9EPerfect Edition\xEF\xBD\x9E\\scene.int");
    if (!fs::exists(scene)) GTEST_SKIP() << "scene.int not installed";
    cs2::KifArchive arc(scene.u8string());
    EXPECT_GT(arc.entries().size(), 100u);
}
