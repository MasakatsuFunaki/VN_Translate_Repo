// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// DSC decompression + ARC20 list/read/repack round-trip tests.  The DSC
// helper crafts a perfect-tree blob (all code lengths 9) so the encoder is trivial.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "bgi/bgi_archive.h"
#include "common/util.h"

#include "arc_fixture.h"

using snu::Bytes;
namespace bgi = snu::bgi;
namespace fs = std::filesystem;

namespace {

// Mirror of the asmodean PRNG for scrambling test data.
std::uint32_t hash_update(std::uint32_t& state) {
    std::uint32_t eax = (state & 0xFFFF) * 20021u;
    std::uint32_t edx = ((state >> 16) & 0xFFFF) * 20021u + state * 346u;
    edx = edx + (eax >> 16);
    state = (((edx & 0xFFFF) << 16) | (eax & 0xFFFF)) + 1u;
    return edx & 0x7FFF;
}

class BitWriter {
public:
    void put_bits(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) put_bit((value >> i) & 1);
    }
    void put_bit(int b) {
        cur_ = static_cast<std::uint8_t>((cur_ << 1) | (b & 1));
        if (++count_ == 8) {
            out_.push_back(cur_);
            cur_ = 0;
            count_ = 0;
        }
    }
    Bytes finish() {
        if (count_) out_.push_back(static_cast<std::uint8_t>(cur_ << (8 - count_)));
        return out_;
    }

private:
    Bytes out_;
    std::uint8_t cur_ = 0;
    int count_ = 0;
};

// Build a DSC blob whose Huffman stream encodes `symbols` (0..255 literal,
// 256+n = LZ copy of length n+2 with the 12-bit offsets from `offsets`).
Bytes make_dsc(const std::vector<int>& symbols, const std::vector<int>& offsets,
               std::uint32_t dec_size, std::uint32_t key = 0x12345678) {
    Bytes blob;
    const char magic[] = "DSC FORMAT 1.00";
    blob.insert(blob.end(), magic, magic + 16);
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) blob.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    put32(key);
    put32(dec_size);
    put32(static_cast<std::uint32_t>(symbols.size()));  // dec_count
    put32(0);                                           // reserved

    // 512 code lengths, all 9 (perfect tree), scrambled with the PRNG.
    std::uint32_t state = key;
    for (int i = 0; i < 512; ++i) {
        std::uint32_t adv = hash_update(state);
        blob.push_back(static_cast<std::uint8_t>((9 + adv) & 0xFF));
    }

    BitWriter bw;
    std::size_t off_idx = 0;
    for (int sym : symbols) {
        bw.put_bits(static_cast<std::uint32_t>(sym), 9);
        if (sym >= 256)
            bw.put_bits(static_cast<std::uint32_t>(offsets.at(off_idx++)), 12);
    }
    Bytes bits = bw.finish();
    blob.insert(blob.end(), bits.begin(), bits.end());
    return blob;
}

}  // namespace

TEST(BgiArchive, dsc_literal_round_trip) {
    const std::string payload = "Hello, BGI world!";
    std::vector<int> symbols(payload.begin(), payload.end());
    Bytes blob = make_dsc(symbols, {}, static_cast<std::uint32_t>(payload.size()));
    Bytes out = bgi::dsc_decompress(blob);
    ASSERT_EQ(out.size(), payload.size());
    EXPECT_EQ(std::memcmp(out.data(), payload.data(), payload.size()), 0);
}

TEST(BgiArchive, dsc_lz_copy) {
    // "ABAB" + LZ(length 4, offset 4) -> "ABABABAB".  Symbol 258 = length
    // (258-256)+2 = 4; stream offset field stores offset-2 = 2.
    std::vector<int> symbols = {'A', 'B', 'A', 'B', 258};
    Bytes blob = make_dsc(symbols, {2}, 8);
    Bytes out = bgi::dsc_decompress(blob);
    const std::string expect = "ABABABAB";
    ASSERT_EQ(out.size(), expect.size());
    EXPECT_EQ(std::memcmp(out.data(), expect.data(), expect.size()), 0);
}

TEST(BgiArchive, dsc_bad_magic_throws) {
    Bytes junk(64, 0x41);
    EXPECT_THROW(bgi::dsc_decompress(junk), std::runtime_error);
}

using snu::test_fixture::make_arc;
using snu::test_fixture::temp_path;

TEST(BgiArchive, arc20_list_and_read) {
    Bytes a = {1, 2, 3, 4};
    Bytes b = {9, 8, 7};
    std::string path = temp_path("snu_ut_arc20.arc");
    snu::write_file(path, make_arc({{"first", a}, {"second", b}}));

    auto entries = bgi::list_arc(path);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "first");
    EXPECT_EQ(entries[1].name, "second");
    EXPECT_EQ(entries[1].offset, 4u);
    EXPECT_EQ(entries[1].size, 3u);

    Bytes r = bgi::read_entry(path, 1);
    EXPECT_EQ(r, b);
    Bytes r2 = bgi::read_entry_by_name(path, "first");
    EXPECT_EQ(r2, a);
    fs::remove(fs::u8path(path));
}

TEST(BgiArchive, arc20_repack_replaces_and_preserves) {
    Bytes a = {1, 2, 3, 4};
    Bytes b = {9, 8, 7};
    std::string in = temp_path("snu_ut_repack_in.arc");
    std::string out = temp_path("snu_ut_repack_out.arc");
    snu::write_file(in, make_arc({{"keep", a}, {"swap", b}}));

    Bytes replacement = {0x55, 0x66, 0x77, 0x88, 0x99};
    bgi::repack_arc(in, out, {{"swap", replacement}});

    auto entries = bgi::list_arc(out);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(bgi::read_entry(out, 0), a);
    EXPECT_EQ(bgi::read_entry(out, 1), replacement);

    // Index-extra bytes (offset 104..128 of each record) survive verbatim.
    Bytes raw = snu::read_file(out);
    EXPECT_EQ(raw[16 + 104], 0xEE);
    EXPECT_EQ(raw[16 + 128 + 127], 0xEE);
    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(BgiArchive, arc20_unknown_name_throws) {
    std::string path = temp_path("snu_ut_arc20_unk.arc");
    snu::write_file(path, make_arc({{"a", Bytes{0x78}}}));
    EXPECT_THROW(bgi::read_entry_by_name(path, "not_in_archive"),
                 std::out_of_range);
    fs::remove(fs::u8path(path));
}

TEST(BgiArchive, arc20_bad_magic_rejected) {
    std::string path = temp_path("snu_ut_arc20_bad.arc");
    Bytes bogus = {'N', 'O', 'T', ' ', 'B', 'U', 'R', 'I', 'K', 'O', '!', '!',
                   0, 0, 0, 0};
    snu::write_file(path, bogus);
    EXPECT_THROW(bgi::list_arc(path), std::runtime_error);
    fs::remove(fs::u8path(path));
}

// Real-file integration: DSC-decompress a known script from the shipped
// game.  Skips when the game is absent.
TEST(BgiArchive, real_arc_dsc_decompresses_to_buriko_script) {
    const std::string arc = "C:\\SakuraNoUta\\data01100.arc";
    if (!fs::exists(fs::u8path(arc))) {
        GTEST_SKIP() << "game arc not present at " << arc;
    }
    auto entries = bgi::list_arc(arc);
    bool found = false;
    for (const auto& e : entries) {
        EXPECT_GT(e.size, 0u);
        if (e.name == "00_op_01") found = true;
    }
    ASSERT_TRUE(found);
    Bytes data = bgi::read_entry_by_name(arc, "00_op_01");
    ASSERT_GE(data.size(), 27u);
    EXPECT_EQ(std::memcmp(data.data(), "BurikoCompiledScriptVer1.00", 27), 0);
    EXPECT_GT(data.size(), 10'000u);  // real scripts are >10 KB decompressed
}

TEST(BgiArchive, arc20_dsc_payload_transparent_decompress) {
    const std::string payload = "compressed!";
    std::vector<int> symbols(payload.begin(), payload.end());
    Bytes dsc = make_dsc(symbols, {}, static_cast<std::uint32_t>(payload.size()));
    std::string path = temp_path("snu_ut_arc20_dsc.arc");
    snu::write_file(path, make_arc({{"script", dsc}}));

    Bytes plain = bgi::read_entry(path, 0);  // decompress=true default
    ASSERT_EQ(plain.size(), payload.size());
    EXPECT_EQ(std::memcmp(plain.data(), payload.data(), payload.size()), 0);

    Bytes raw = bgi::read_entry(path, 0, /*decompress=*/false);
    EXPECT_EQ(raw, dsc);
    fs::remove(fs::u8path(path));
}
