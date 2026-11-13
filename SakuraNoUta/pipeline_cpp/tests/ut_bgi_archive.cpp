// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// DSC decompression + ARC20 list/read/repack round-trip unit tests.  The DSC
// helper crafts a perfect-tree blob (all code lengths 9) so the encoder is trivial.
#include "arc_fixture.h"
#include "bgi/bgi_archive.h"
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using snu::Bytes;
namespace bgi = snu::bgi;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kHashWordMask = 0xFFFFu;
constexpr std::uint32_t kHashMulLo = 20021u;
constexpr std::uint32_t kHashMulHi = 346u;
constexpr int kWordShift = 16;
constexpr std::uint32_t kHashReturnMask = 0x7FFFu;
constexpr int kBitsPerByte = 8;
constexpr std::uint32_t kDefaultDscKey = 0x12345678u;
constexpr std::size_t kDscMagicFieldBytes = 16;
constexpr int kDwordBytes = 4;
constexpr int kHuffmanAlphabetSize = 512;
constexpr int kPerfectTreeCodeBits = 9;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr int kFirstLzSymbol = 256;
constexpr int kLzOffsetBits = 12;
constexpr int kLzCopyTestSymbol = 258;
constexpr int kLzCopyTestEncodedOffset = 2;
constexpr std::uint32_t kLzCopyTestDecodedSize = 8;
constexpr std::size_t kJunkDscBytes = 64;
constexpr std::uint8_t kJunkFillByte = 0x41;
constexpr std::size_t kArcHeaderBytes = 16;
constexpr std::size_t kArcIndexExtraOffset = 104;
constexpr std::size_t kArcRecordBytes = 128;
constexpr std::size_t kArcRecordLastOffset = 127;
constexpr std::uint8_t kFixturePaddingByte = 0xEE;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
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

// Mirror of the asmodean PRNG for scrambling test data.
std::uint32_t hash_update(std::uint32_t& state) {
    std::uint32_t eax = (state & kHashWordMask) * kHashMulLo;
    std::uint32_t edx =
        ((state >> kWordShift) & kHashWordMask) * kHashMulLo + state * kHashMulHi;
    edx = edx + (eax >> kWordShift);
    state = (((edx & kHashWordMask) << kWordShift) | (eax & kHashWordMask)) + 1u;
    return edx & kHashReturnMask;
}

class BitWriter {
public:
    void put_bits(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            put_bit(static_cast<int>((value >> i) & 1u));
        }
    }
    void put_bit(int b) {
        cur_ = static_cast<std::uint8_t>((cur_ << 1) | (b & 1));
        if (++count_ == kBitsPerByte) {
            out_.push_back(cur_);
            cur_ = 0;
            count_ = 0;
        }
    }
    Bytes finish() {
        if (count_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(cur_ << (kBitsPerByte - count_)));
        }
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
               std::uint32_t dec_size, std::uint32_t key = kDefaultDscKey) {
    Bytes blob;
    const char magic[] = "DSC FORMAT 1.00";
    blob.insert(blob.end(), magic, magic + kDscMagicFieldBytes);
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < kDwordBytes; ++i) {
            blob.push_back(static_cast<std::uint8_t>(v >> (kBitsPerByte * i)));
        }
    };
    put32(key);
    put32(dec_size);
    put32(static_cast<std::uint32_t>(symbols.size()));
    put32(0);

    std::uint32_t state = key;
    for (int i = 0; i < kHuffmanAlphabetSize; ++i) {
        std::uint32_t adv = hash_update(state);
        blob.push_back(static_cast<std::uint8_t>((kPerfectTreeCodeBits + adv) & kByteMask));
    }

    BitWriter bw;
    std::size_t off_idx = 0;
    for (int sym : symbols) {
        bw.put_bits(static_cast<std::uint32_t>(sym), kPerfectTreeCodeBits);
        if (sym >= kFirstLzSymbol) {
            bw.put_bits(static_cast<std::uint32_t>(offsets.at(off_idx++)),
                        kLzOffsetBits);
        }
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
    const std::vector<int> symbols = {'A', 'B', 'A', 'B', kLzCopyTestSymbol};
    const Bytes blob =
        make_dsc(symbols, {kLzCopyTestEncodedOffset}, kLzCopyTestDecodedSize);
    const Bytes out = bgi::dsc_decompress(blob);
    const std::string expect = "ABABABAB";
    ASSERT_EQ(out.size(), expect.size());
    EXPECT_EQ(std::memcmp(out.data(), expect.data(), expect.size()), 0);
}

TEST(BgiArchive, dsc_bad_magic_throws) {
    const Bytes junk(kJunkDscBytes, kJunkFillByte);
    EXPECT_THROW(bgi::dsc_decompress(junk), std::runtime_error);
}

using snu::test_fixture::make_arc;

TEST(BgiArchive, arc20_list_and_read) {
    const Bytes a = {1, 2, 3, 4};
    const Bytes b = {9, 8, 7};
    const ScopedTempDir scratch("snu_ut_arc20_list");
    const std::string path = (scratch.path() / fs::u8path("test.arc")).u8string();
    snu::write_file(path, make_arc({{"first", a}, {"second", b}}));

    const auto entries = bgi::list_arc(path);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "first");
    EXPECT_EQ(entries[1].name, "second");
    EXPECT_EQ(entries[1].offset, 4u);
    EXPECT_EQ(entries[1].size, 3u);

    const Bytes r = bgi::read_entry(path, 1);
    EXPECT_EQ(r, b);
    const Bytes r2 = bgi::read_entry_by_name(path, "first");
    EXPECT_EQ(r2, a);
}

TEST(BgiArchive, arc20_repack_replaces_and_preserves) {
    const Bytes a = {1, 2, 3, 4};
    const Bytes b = {9, 8, 7};
    const ScopedTempDir scratch("snu_ut_arc20_repack");
    const std::string in = (scratch.path() / fs::u8path("in.arc")).u8string();
    const std::string out = (scratch.path() / fs::u8path("out.arc")).u8string();
    snu::write_file(in, make_arc({{"keep", a}, {"swap", b}}));

    const Bytes replacement = {0x55, 0x66, 0x77, 0x88, 0x99};
    bgi::repack_arc(in, out, {{"swap", replacement}});

    const auto entries = bgi::list_arc(out);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(bgi::read_entry(out, 0), a);
    EXPECT_EQ(bgi::read_entry(out, 1), replacement);

    // Index-extra bytes (offset 104..128 of each record) survive verbatim.
    const Bytes raw = snu::read_file(out);
    EXPECT_EQ(raw[kArcHeaderBytes + kArcIndexExtraOffset], kFixturePaddingByte);
    EXPECT_EQ(raw[kArcHeaderBytes + kArcRecordBytes + kArcRecordLastOffset],
              kFixturePaddingByte);
}

TEST(BgiArchive, arc20_unknown_name_throws) {
    const ScopedTempDir scratch("snu_ut_arc20_unk");
    const std::string path = (scratch.path() / fs::u8path("unk.arc")).u8string();
    snu::write_file(path, make_arc({{"a", Bytes{0x78}}}));
    EXPECT_THROW(bgi::read_entry_by_name(path, "not_in_archive"),
                 std::out_of_range);
}

TEST(BgiArchive, arc20_bad_magic_rejected) {
    const ScopedTempDir scratch("snu_ut_arc20_bad");
    const std::string path = (scratch.path() / fs::u8path("bad.arc")).u8string();
    const Bytes bogus = {'N', 'O', 'T', ' ', 'B', 'U', 'R', 'I', 'K', 'O', '!', '!',
                         0, 0, 0, 0};
    snu::write_file(path, bogus);
    EXPECT_THROW(bgi::list_arc(path), std::runtime_error);
}

TEST(BgiArchive, arc20_dsc_payload_transparent_decompress) {
    const std::string payload = "compressed!";
    const std::vector<int> symbols(payload.begin(), payload.end());
    const Bytes dsc =
        make_dsc(symbols, {}, static_cast<std::uint32_t>(payload.size()));
    const ScopedTempDir scratch("snu_ut_arc20_dsc");
    const std::string path = (scratch.path() / fs::u8path("dsc.arc")).u8string();
    snu::write_file(path, make_arc({{"script", dsc}}));

    const Bytes plain = bgi::read_entry(path, 0);
    ASSERT_EQ(plain.size(), payload.size());
    EXPECT_EQ(std::memcmp(plain.data(), payload.data(), payload.size()), 0);

    const Bytes raw = bgi::read_entry(path, 0, /*decompress=*/false);
    EXPECT_EQ(raw, dsc);
}
