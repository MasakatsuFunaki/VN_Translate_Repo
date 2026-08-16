// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// YPF v490 index parsing, YSTB decryption and entry reading.
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.h"
#include "yuris/ypf_archive.h"

using namespace frat;
using namespace frat::yuris;
using frat_test::build_ypf;
using frat_test::put_u32;
using frat_test::put_u64;

namespace {

Bytes body(const std::string& s) { return Bytes(s.begin(), s.end()); }

// A minimal YSTB whose four section sizes add up, so decrypt_ystb engages.
Bytes make_ystb(std::uint32_t csz, std::uint32_t asz, std::uint32_t rsz, std::uint32_t osz) {
    Bytes b;
    for (char c : std::string("YSTB")) b.push_back(static_cast<std::uint8_t>(c));
    put_u32(b, 0x1E1);  // version
    put_u32(b, 1);      // inst_count
    put_u32(b, csz);
    put_u32(b, asz);
    put_u32(b, rsz);
    put_u32(b, osz);
    put_u32(b, 0);  // reserved
    for (std::uint32_t i = 0; i < csz + asz + rsz + osz; ++i)
        b.push_back(static_cast<std::uint8_t>(0x11 * (i % 15) + 1));
    return b;
}

}  // namespace

TEST(YpfArchive, ParseIndex_SyntheticArchive_RoundTrips) {
    const std::vector<std::pair<std::string, Bytes>> files = {
        {"ysbin\\yst00156.ybn", body("first entry payload")},
        {"ysbin\\yse.ybn", body("second")},
        {"ysbin\\yst_list.ybn", body("third entry, a bit longer")},
    };
    const Bytes image = build_ypf(files);
    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(entries[i].name, files[i].first);
        EXPECT_EQ(entries[i].compressed, 1);
        EXPECT_EQ(entries[i].raw_size, files[i].second.size());
        EXPECT_EQ(read_entry(image, entries[i]), files[i].second);
    }
    // Entry stride is 27 + name length; a wrong stride shows up as a garbled
    // second name, so pin the data offsets too.
    EXPECT_GT(entries[1].data_offset, entries[0].data_offset);
    EXPECT_GT(entries[2].data_offset, entries[1].data_offset);
}

TEST(YpfArchive, ParseIndex_NameProbe_StopsAtTheRealLength) {
    // name_size_raw lies (hinted = real + 3).  The probe must still land on
    // the real length: an ASCII name byte XOR 0xFF is 0x81-0xDF, always > 20,
    // so the `t > 20` guard can never stop inside the name.
    const std::string name = "ysbin\\yse.ybn";
    Bytes index;
    put_u32(index, 0);
    index.push_back(static_cast<std::uint8_t>((name.size() + 3) ^ 0xFF));
    for (char c : name) index.push_back(static_cast<std::uint8_t>(c) ^ 0xFF);
    index.push_back(0);   // type
    index.push_back(1);   // compressed
    put_u32(index, 7);    // raw_size
    put_u32(index, 7);    // packed_size
    put_u64(index, 999);  // data_offset
    put_u32(index, 0);

    Bytes image;
    for (char c : std::string("YPF")) image.push_back(static_cast<std::uint8_t>(c));
    image.push_back(0);
    put_u32(image, 490);
    put_u32(image, 1);
    put_u32(image, static_cast<std::uint32_t>(index.size()));
    image.insert(image.end(), 16, 0);
    image.insert(image.end(), index.begin(), index.end());

    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, name);
    EXPECT_EQ(entries[0].data_offset, 999u);
}

TEST(YpfArchive, ParseIndex_NameProbe_FallsBackToHint) {
    // compressed == 0 AND raw != packed, so no probe length is ever accepted;
    // name_len stays 0 and the hinted length is used verbatim.
    const std::string name = "ab";
    Bytes index;
    put_u32(index, 0);
    index.push_back(static_cast<std::uint8_t>(name.size() ^ 0xFF));
    for (char c : name) index.push_back(static_cast<std::uint8_t>(c) ^ 0xFF);
    index.push_back(0);    // type
    index.push_back(0);    // compressed
    put_u32(index, 100);   // raw_size
    put_u32(index, 50);    // packed_size != raw_size
    put_u64(index, 4242);
    put_u32(index, 0);

    Bytes image;
    for (char c : std::string("YPF")) image.push_back(static_cast<std::uint8_t>(c));
    image.push_back(0);
    put_u32(image, 490);
    put_u32(image, 1);
    put_u32(image, static_cast<std::uint32_t>(index.size()));
    image.insert(image.end(), 16, 0);
    image.insert(image.end(), index.begin(), index.end());

    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "ab");
    EXPECT_EQ(entries[0].raw_size, 100u);
    EXPECT_EQ(entries[0].packed_size, 50u);
    EXPECT_EQ(entries[0].data_offset, 4242u);
}

TEST(YpfArchive, ParseIndex_RejectsNonYpf) {
    EXPECT_THROW(parse_ypf_index_bytes(body("short"), "x.ypf"), std::runtime_error);
    Bytes wrong_magic(64, 0);
    wrong_magic[0] = 'Y';
    wrong_magic[1] = 'P';
    wrong_magic[2] = 'X';
    EXPECT_THROW(parse_ypf_index_bytes(wrong_magic, "x.ypf"), std::runtime_error);
}

TEST(YpfArchive, DecryptYstb_IsAnInvolution_AllSectionRemainders) {
    for (std::uint32_t c = 4; c < 8; ++c)
        for (std::uint32_t a = 4; a < 8; ++a)
            for (std::uint32_t r = 4; r < 8; ++r)
                for (std::uint32_t o = 4; o < 8; ++o) {
                    const Bytes plain = make_ystb(c, a, r, o);
                    const Bytes enc = decrypt_ystb(plain);
                    EXPECT_NE(enc, plain);
                    EXPECT_EQ(decrypt_ystb(enc), plain)
                        << c << "/" << a << "/" << r << "/" << o;
                }
}

TEST(YpfArchive, DecryptYstb_TailBytesUseTheKeyLowBytesInOrder) {
    // A 3-byte code section: the tail bytes XOR with 0xC3, 0xDA, 0x94 -- the
    // key's low bytes ascending.  A big-endian tail would use 0x65 first.
    const Bytes plain = make_ystb(3, 0, 0, 0);
    const Bytes enc = decrypt_ystb(plain);
    EXPECT_EQ(enc[0x20] ^ plain[0x20], 0xC3);
    EXPECT_EQ(enc[0x21] ^ plain[0x21], 0xDA);
    EXPECT_EQ(enc[0x22] ^ plain[0x22], 0x94);
}

TEST(YpfArchive, DecryptYstb_NoOpOnSizeMismatchAndOnNonYstbMagic) {
    Bytes bad = make_ystb(4, 4, 4, 4);
    bad.push_back(0xAB);  // total no longer equals 0x20 + sections
    EXPECT_EQ(decrypt_ystb(bad), bad);

    // The plaintext sidecars are YSCF / YSER / YSTL; decrypting them would
    // turn readable strings into garbage.
    for (const char* magic : {"YSCF", "YSER", "YSTL"}) {
        Bytes blob = make_ystb(4, 4, 4, 4);
        std::memcpy(blob.data(), magic, 4);
        EXPECT_EQ(decrypt_ystb(blob), blob) << magic;
    }
}

TEST(YpfArchive, ReadEntry_InflatesThenDecrypts) {
    // The checked-in sample is already decrypted, so re-encrypt it (the XOR is
    // an involution) to make a realistic archive body.
    const Bytes plain = frat_test::sample("yst00001.ybn");
    ASSERT_EQ(plain.size(), 532u);
    const Bytes encrypted = decrypt_ystb(plain);
    const Bytes sidecar = frat_test::sample("yscfg.ybn");

    const Bytes image = build_ypf({{"ysbin\\yst00001.ybn", encrypted},
                                   {"ysbin\\yscfg.ybn", sidecar}});
    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(read_entry(image, entries[0]), plain);   // inflated AND decrypted
    EXPECT_EQ(read_entry(image, entries[1]), sidecar);  // merely inflated
}

TEST(YpfArchive, Basename_SplitsOnBothSeparators) {
    EXPECT_EQ(basename("ysbin\\yst00156.ybn"), "yst00156.ybn");
    EXPECT_EQ(basename("a/b\\c.ybn"), "c.ybn");
    EXPECT_EQ(basename("plain.ybn"), "plain.ybn");
}
