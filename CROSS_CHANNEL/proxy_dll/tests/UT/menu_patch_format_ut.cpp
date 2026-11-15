// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — menu_patch_format (OS-independent CCMP v1 / LZSS / PAK / CPK codec)

#include "menu_patch_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void AppendBe16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void AppendBe32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void AppendBe64(Bytes& out, std::uint64_t v) {
    AppendBe32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
    AppendBe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}

void AppendLe32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void AppendLe64(Bytes& out, std::uint64_t v) {
    AppendLe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    AppendLe32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

std::uint32_t ReadBe32(const Bytes& b, std::size_t off) {
    return (static_cast<std::uint32_t>(b[off]) << 24) |
           (static_cast<std::uint32_t>(b[off + 1]) << 16) |
           (static_cast<std::uint32_t>(b[off + 2]) << 8) |
           static_cast<std::uint32_t>(b[off + 3]);
}

std::uint64_t ReadBe64(const Bytes& b, std::size_t off) {
    return (static_cast<std::uint64_t>(ReadBe32(b, off)) << 32) |
           static_cast<std::uint64_t>(ReadBe32(b, off + 4));
}

struct UtfColSpec {
    std::uint8_t type_flag = 0; // e.g. 0x52=u16, 0x54=u32, 0x56=u64, 0x5B=data
    std::string name;
};

// Minimal Criware @UTF table builder from the public @UTF specification for
// independent fixture construction (zero calls into menu_patch).
Bytes BuildUtfTable(const std::string& table_name,
                    const std::vector<UtfColSpec>& cols,
                    const std::vector<Bytes>& rows_encoded,
                    const Bytes& blob_pool = {}) {
    Bytes strs;
    auto add_str = [&](const std::string& s) -> std::uint32_t {
        const auto off = static_cast<std::uint32_t>(strs.size());
        strs.insert(strs.end(), s.begin(), s.end());
        strs.push_back(0);
        return off;
    };

    const std::uint32_t table_name_off = add_str(table_name);
    Bytes schema;
    for (const auto& c : cols) {
        schema.push_back(c.type_flag);
        AppendBe32(schema, add_str(c.name));
    }

    const std::uint16_t row_len =
        rows_encoded.empty()
            ? 0u
            : static_cast<std::uint16_t>(rows_encoded.front().size());
    Bytes row_bytes;
    for (const auto& r : rows_encoded) {
        row_bytes.insert(row_bytes.end(), r.begin(), r.end());
    }

    // Offsets inside @UTF are relative to +0x08 (after "@UTF" + table_size).
    const std::uint32_t rows_off =
        static_cast<std::uint32_t>(24u + schema.size());
    const std::uint32_t strs_off =
        rows_off + static_cast<std::uint32_t>(row_bytes.size());
    const std::uint32_t data_off =
        strs_off + static_cast<std::uint32_t>(strs.size());
    const std::uint32_t table_size =
        data_off + static_cast<std::uint32_t>(blob_pool.size());

    Bytes out;
    out.insert(out.end(), {'@', 'U', 'T', 'F'});
    AppendBe32(out, table_size);
    AppendBe32(out, rows_off);
    AppendBe32(out, strs_off);
    AppendBe32(out, data_off);
    AppendBe32(out, table_name_off);
    AppendBe16(out, static_cast<std::uint16_t>(cols.size()));
    AppendBe16(out, row_len);
    AppendBe32(out, static_cast<std::uint32_t>(rows_encoded.size()));
    out.insert(out.end(), schema.begin(), schema.end());
    out.insert(out.end(), row_bytes.begin(), row_bytes.end());
    out.insert(out.end(), strs.begin(), strs.end());
    out.insert(out.end(), blob_pool.begin(), blob_pool.end());
    return out;
}

Bytes BuildRawPak(const std::vector<Bytes>& sub_entries) {
    const auto count = static_cast<std::uint32_t>(sub_entries.size());
    const std::uint32_t dir_bytes = count * 16u;
    Bytes out;
    std::uint32_t cur_off = dir_bytes;
    for (const auto& sub : sub_entries) {
        AppendLe32(out, cur_off);
        AppendLe32(out, static_cast<std::uint32_t>(sub.size()));
        AppendLe32(out, 0u);
        AppendLe32(out, 0u);
        cur_off += static_cast<std::uint32_t>(sub.size());
    }
    for (const auto& sub : sub_entries) {
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

// Builds an unencrypted ITOC-indexed Criware CPK matching data\sys.cpk's
// structure: "CPK " @ 0x0000, "ITOC" @ 0x0800, payloads @ 0x1000 aligned to
// 0x800 (with the final entry unpadded at EOF, matching CPKMC2.30.07).
// Entries >= 64 KiB go to CpkItocH; entries < 64 KiB go to CpkItocL.
Bytes BuildSyntheticSysCpk(const std::vector<Bytes>& outer_entries) {
    constexpr std::uint32_t kAlign = 0x800u;
    constexpr std::uint64_t kItocOffset = 0x800u;
    constexpr std::uint64_t kContentOffset = 0x1000u;

    std::vector<Bytes> rows_l;
    std::vector<Bytes> rows_h;
    std::uint64_t sum_sizes = 0;
    std::uint64_t content_span = 0;

    for (std::size_t i = 0; i < outer_entries.size(); ++i) {
        const auto sz = static_cast<std::uint32_t>(outer_entries[i].size());
        sum_sizes += sz;
        if (i + 1 < outer_entries.size()) {
            content_span += (static_cast<std::uint64_t>(sz) + (kAlign - 1u)) &
                            ~static_cast<std::uint64_t>(kAlign - 1u);
        } else {
            content_span += sz;
        }
        if (sz <= 0xFFFFu) {
            Bytes row;
            AppendBe16(row, static_cast<std::uint16_t>(i));
            AppendBe16(row, static_cast<std::uint16_t>(sz));
            AppendBe16(row, static_cast<std::uint16_t>(sz));
            rows_l.push_back(std::move(row));
        } else {
            Bytes row;
            AppendBe16(row, static_cast<std::uint16_t>(i));
            AppendBe32(row, sz);
            AppendBe32(row, sz);
            rows_h.push_back(std::move(row));
        }
    }

    const std::uint64_t content_size =
        (content_span + (kAlign - 1u)) & ~static_cast<std::uint64_t>(kAlign - 1u);

    const Bytes data_l = BuildUtfTable(
        "CpkItocL",
        {{0x52, "ID"}, {0x52, "FileSize"}, {0x52, "ExtractSize"}},
        rows_l);
    const Bytes data_h = BuildUtfTable(
        "CpkItocH",
        {{0x52, "ID"}, {0x54, "FileSize"}, {0x54, "ExtractSize"}},
        rows_h);

    Bytes itoc_blobs = data_l;
    const auto data_h_off = static_cast<std::uint32_t>(itoc_blobs.size());
    itoc_blobs.insert(itoc_blobs.end(), data_h.begin(), data_h.end());

    Bytes itoc_row;
    AppendBe32(itoc_row, static_cast<std::uint32_t>(rows_l.size()));
    AppendBe32(itoc_row, static_cast<std::uint32_t>(rows_h.size()));
    AppendBe32(itoc_row, 0u);
    AppendBe32(itoc_row, static_cast<std::uint32_t>(data_l.size()));
    AppendBe32(itoc_row, data_h_off);
    AppendBe32(itoc_row, static_cast<std::uint32_t>(data_h.size()));

    const Bytes itoc_utf = BuildUtfTable(
        "CpkItocInfo",
        {{0x54, "FilesL"}, {0x54, "FilesH"}, {0x5B, "DataL"}, {0x5B, "DataH"}},
        {itoc_row},
        itoc_blobs);

    Bytes hdr_row;
    AppendBe64(hdr_row, kContentOffset);
    AppendBe64(hdr_row, content_size);
    AppendBe64(hdr_row, kItocOffset);
    AppendBe64(hdr_row, 16u + itoc_utf.size());
    AppendBe64(hdr_row, sum_sizes);
    AppendBe64(hdr_row, sum_sizes);
    AppendBe32(hdr_row, static_cast<std::uint32_t>(outer_entries.size()));
    AppendBe16(hdr_row, static_cast<std::uint16_t>(kAlign));

    const Bytes cpk_utf = BuildUtfTable(
        "CpkHeader",
        {{0x56, "ContentOffset"},
         {0x56, "ContentSize"},
         {0x56, "ItocOffset"},
         {0x56, "ItocSize"},
         {0x56, "EnabledPackedSize"},
         {0x56, "EnabledDataSize"},
         {0x54, "Files"},
         {0x52, "Align"}},
        {hdr_row});

    Bytes cpk(static_cast<std::size_t>(kContentOffset), 0u);
    // "CPK " packet @ 0x0000
    std::memcpy(cpk.data(), "CPK ", 4);
    cpk[4] = 0xFFu;
    for (int b = 0; b < 8; ++b) {
        cpk[8 + b] = static_cast<std::uint8_t>(
            (static_cast<std::uint64_t>(cpk_utf.size()) >> (b * 8)) & 0xFFu);
    }
    std::memcpy(cpk.data() + 16, cpk_utf.data(), cpk_utf.size());
    // Criware copyright marker at end of first 0x800 sector (must be preserved!)
    std::memcpy(cpk.data() + 0x7FA, "(c)CRI", 6);

    // "ITOC" packet @ 0x0800
    std::memcpy(cpk.data() + 0x800, "ITOC", 4);
    cpk[0x804] = 0xFFu;
    for (int b = 0; b < 8; ++b) {
        cpk[0x808 + b] = static_cast<std::uint8_t>(
            (static_cast<std::uint64_t>(itoc_utf.size()) >> (b * 8)) & 0xFFu);
    }
    std::memcpy(cpk.data() + 0x810, itoc_utf.data(), itoc_utf.size());

    // Append outer entries aligned to 0x800 (final entry unpadded).
    for (std::size_t i = 0; i < outer_entries.size(); ++i) {
        cpk.insert(cpk.end(), outer_entries[i].begin(), outer_entries[i].end());
        if (i + 1 < outer_entries.size()) {
            const std::size_t rem = outer_entries[i].size() % kAlign;
            if (rem != 0) {
                cpk.insert(cpk.end(), kAlign - rem, 0u);
            }
        }
    }
    return cpk;
}

}  // namespace

// --- MurmurHash2 ------------------------------------------------------------

TEST(MenuPatchFormatMurmur, EmptyBufferMatchesCanonicalSeedZeroHash) {
    const Bytes empty;
    EXPECT_EQ(menu_patch::ComputeMurmurHash2(nullptr, 0u), 0x00000000u);
    EXPECT_EQ(menu_patch::ComputeMurmurHash2(empty), 0x00000000u);
}

TEST(MenuPatchFormatMurmur, MatchesHandComputedSignBitBodyAndTailVectors) {
    // Hand-computed from Austin Appleby's 32-bit MurmurHash2 specification
    // (m = 0x5BD1E995, r = 24, h0 = len), covering tail lengths 1..3, full
    // 4-byte body word, and sign-bit byte boundaries (0x00, 0x7F, 0x80, 0xFF):
    //   {0x00}                   -> 0xE94E6EBD
    //   {0x80}                   -> 0x9B891BB0 (catches signed char in tail case 1)
    //   {0x7F, 0x80}             -> 0x14115A33 (catches signed char in tail case 2)
    //   {0x00, 0x80, 0xFF}       -> 0x74C47957 (catches signed char in tail case 3)
    //   {0x80, 0x7F, 0xFF, 0x80} -> 0xD47A921E (catches signed char in 4-byte word)
    struct Vector {
        Bytes input;
        std::uint32_t expected;
    };
    const Vector kVectors[] = {
        {{0x00u},                   0xE94E6EBDu},
        {{0x80u},                   0x9B891BB0u},
        {{0x7Fu, 0x80u},            0x14115A33u},
        {{0x00u, 0x80u, 0xFFu},     0x74C47957u},
        {{0x80u, 0x7Fu, 0xFFu, 0x80u}, 0xD47A921Eu},
    };
    for (std::size_t i = 0; i < sizeof(kVectors) / sizeof(kVectors[0]); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(menu_patch::ComputeMurmurHash2(kVectors[i].input),
                  kVectors[i].expected);
    }
}

// --- Zero-Ring LZSS (RVA 0x45910) -------------------------------------------

TEST(MenuPatchFormatLzss, DecompressesHandCraftedZeroRingWireStream) {
    // Uncompressed target (10 bytes): \0\0\0\0ABCABC
    // Op 0 (bit=0): match_pos=0x000, len=4 -> reads 4 zero bytes from initial
    //               0x00-filled ring and writes them to ring[0xFEE..0xFF1].
    //               (A 0x20-filled ring would wrongly emit spaces here!)
    // Op 1..3 (bits=1,1,1): literals 'A', 'B', 'C' -> ring[0xFF2..0xFF4].
    // Op 4 (bit=0): match_pos=0xFF2, len=3 -> b0=0xF2, b1=0xF0 -> "ABC".
    // Flags = 0b00001110 = 0x0E.
    const Bytes wire = {
        0x0Au, 0x00u, 0x00u, 0x00u,
        0x0Eu,
        0x00u, 0x01u,
        'A', 'B', 'C',
        0xF2u, 0xF0u
    };
    Bytes out;
    ASSERT_TRUE(menu_patch::LzssDecompressZeroRing(wire, out));
    const Bytes expected = {0u, 0u, 0u, 0u, 'A', 'B', 'C', 'A', 'B', 'C'};
    EXPECT_EQ(out, expected);
}

TEST(MenuPatchFormatLzss, RejectsTruncatedHeaderAndTruncatedBackReference) {
    Bytes out;
    const Bytes too_short = {0x01u, 0x00u, 0x00u};
    EXPECT_FALSE(menu_patch::LzssDecompressZeroRing(too_short, out));

    // Claims 5 bytes uncompressed, provides flag 0x00 (match) + only 1 byte of
    // 2-byte match token.
    const Bytes truncated_match = {0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u};
    EXPECT_FALSE(menu_patch::LzssDecompressZeroRing(truncated_match, out));
}

TEST(MenuPatchFormatLzss, RoundTripsEmptyZerosRleAndWrapAround4KiBWindow) {
    // 1. Empty payload
    {
        const Bytes empty;
        Bytes comp;
        Bytes decomp;
        ASSERT_TRUE(menu_patch::LzssCompressZeroRing(empty, comp));
        ASSERT_EQ(comp.size(), 4u);
        ASSERT_TRUE(menu_patch::LzssDecompressZeroRing(comp, decomp));
        EXPECT_TRUE(decomp.empty());
    }

    // 2. Large buffer (> 12 KiB) combining initial zero-ring runs, RLE runs,
    //    pseudo-random incompressible literals, and cross-4096-wrap matches.
    Bytes original(14000u, 0u);
    for (std::size_t i = 512; i < 4800; ++i) {
        original[i] = static_cast<std::uint8_t>((i * 73u + 19u) & 0xFFu);
    }
    // Repeat a 200-byte slice across a 4096-byte ring wrap boundary.
    std::copy_n(original.begin() + 600, 200, original.begin() + 3900);
    // RLE run of a single byte.
    std::fill_n(original.begin() + 9000, 300, static_cast<std::uint8_t>(0xA5u));

    Bytes compressed;
    ASSERT_TRUE(menu_patch::LzssCompressZeroRing(original, compressed));
    // Must achieve substantial compression on the zero & RLE regions.
    EXPECT_LT(compressed.size(), original.size() / 2u);

    Bytes round_trip;
    ASSERT_TRUE(menu_patch::LzssDecompressZeroRing(compressed, round_trip));
    EXPECT_EQ(round_trip, original);
}

// --- Inner .pak Container ---------------------------------------------------

TEST(MenuPatchFormatPak, ParsesAndRebuildsPakWithModifiedSubEntry) {
    const Bytes sub0 = {'x', 't', 'x', 0, 1, 2, 3, 4};
    const Bytes sub1 = {'H', 'E', 'L', 'L', 'O'};
    const Bytes sub2 = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    const Bytes pak = BuildRawPak({sub0, sub1, sub2});

    std::vector<menu_patch::PakEntrySlice> slices;
    ASSERT_TRUE(menu_patch::ParsePakArchive(pak, slices));
    ASSERT_EQ(slices.size(), 3u);
    EXPECT_EQ(slices[0].offset, 48u);
    EXPECT_EQ(slices[0].size, 8u);
    EXPECT_EQ(slices[1].offset, 56u);
    EXPECT_EQ(slices[1].size, 5u);
    EXPECT_EQ(slices[2].offset, 61u);
    EXPECT_EQ(slices[2].size, 9u);

    const Bytes new_sub1 = {'W', 'O', 'R', 'L', 'D', '!'};
    Bytes rebuilt;
    ASSERT_TRUE(menu_patch::RebuildPakArchive(
        pak, {{1u, new_sub1}}, rebuilt));

    std::vector<menu_patch::PakEntrySlice> rebuilt_slices;
    ASSERT_TRUE(menu_patch::ParsePakArchive(rebuilt, rebuilt_slices));
    ASSERT_EQ(rebuilt_slices.size(), 3u);
    EXPECT_EQ(rebuilt_slices[1].size, 6u);
    EXPECT_EQ(rebuilt_slices[2].offset, 48u + 8u + 6u);
    EXPECT_EQ(Bytes(rebuilt.begin() + rebuilt_slices[1].offset,
                    rebuilt.begin() + rebuilt_slices[1].offset +
                        rebuilt_slices[1].size),
              new_sub1);
    EXPECT_EQ(Bytes(rebuilt.begin() + rebuilt_slices[2].offset,
                    rebuilt.begin() + rebuilt_slices[2].offset +
                        rebuilt_slices[2].size),
              sub2);
}

TEST(MenuPatchFormatPak, RejectsCorruptPakDirectoryAndOutOfRangeReplacement) {
    std::vector<menu_patch::PakEntrySlice> slices;
    // Unaligned first offset (not a multiple of 16)
    Bytes bad_pak(32u, 0u);
    bad_pak[0] = 20u;
    EXPECT_FALSE(menu_patch::ParsePakArchive(bad_pak, slices));

    // 32-bit offset + size arithmetic wraparound (0xFFFFFFFCu + 8u == 4u <= 40u):
    // Entry 0 has valid offset=32 (so N = 32/16 = 2), Entry 1 wraps uint32_t.
    Bytes wraparound_pak;
    AppendLe32(wraparound_pak, 32u);
    AppendLe32(wraparound_pak, 8u);
    AppendLe32(wraparound_pak, 0u);
    AppendLe32(wraparound_pak, 0u);
    AppendLe32(wraparound_pak, 0xFFFFFFFCu);
    AppendLe32(wraparound_pak, 8u);
    AppendLe32(wraparound_pak, 0u);
    AppendLe32(wraparound_pak, 0u);
    wraparound_pak.insert(wraparound_pak.end(), 8u, 0xAAu);
    EXPECT_FALSE(menu_patch::ParsePakArchive(wraparound_pak, slices));

    // Replacement targeting out-of-range sub-entry index
    const Bytes valid_pak = BuildRawPak({Bytes{1, 2, 3}});
    Bytes out;
    EXPECT_FALSE(menu_patch::RebuildPakArchive(
        valid_pak, {{5u, Bytes{4, 5, 6}}}, out));
}

// --- Criware CPK + End-to-End CCMP v1 Keyless XOR-Delta Codec ---------------

TEST(MenuPatchFormatCpkAndPackage, IdentityRebuildPreservesSyntheticSysCpkByteForByte) {
    Bytes big_entry(70000u, 0x33u);
    Bytes small_entry(1200u, 0x77u);
    const Bytes orig_cpk = BuildSyntheticSysCpk({big_entry, small_entry});

    std::vector<menu_patch::CpkEntrySlice> toc;
    ASSERT_TRUE(menu_patch::ParseSysCpkToc(orig_cpk, toc));
    ASSERT_EQ(toc.size(), 2u);
    EXPECT_EQ(toc[0].id, 0u);
    EXPECT_FALSE(toc[0].is_low_table);
    EXPECT_EQ(toc[0].offset, 0x1000u);
    EXPECT_EQ(toc[0].size, 70000u);
    EXPECT_EQ(toc[1].id, 1u);
    EXPECT_TRUE(toc[1].is_low_table);
    EXPECT_EQ(toc[1].offset, 0x1000u + 0x11800u);
    EXPECT_EQ(toc[1].size, 1200u);

    Bytes rebuilt;
    ASSERT_TRUE(menu_patch::RebuildSysCpk(orig_cpk, {}, rebuilt));
    EXPECT_EQ(rebuilt, orig_cpk);
}

TEST(MenuPatchFormatCpkAndPackage, EndToEndBuildAndApplyKeylessXorPatchUpdatesCpkAndVerifiesHashes) {
    // Entry 0: LZSS-compressed .pak with 2 xtx-like sub-entries (> 64 KiB compressed
    // by filling sub0 with pseudo-random bytes so Entry 0 resides in CpkItocH).
    Bytes sub0(76000u);
    std::uint32_t rng = 0x9E3779B9u;
    for (std::size_t i = 0; i < sub0.size(); ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        sub0[i] = static_cast<std::uint8_t>(rng & 0xFFu);
    }
    Bytes orig_sub1(4096u, 0x10u);
    std::memcpy(orig_sub1.data(), "xtx\0", 4);

    Bytes pak0 = BuildRawPak({sub0, orig_sub1});
    Bytes comp_pak0;
    ASSERT_TRUE(menu_patch::LzssCompressZeroRing(pak0, comp_pak0));
    ASSERT_GT(comp_pak0.size(), 65536u);

    // Entry 1: untouched raw audio-like entry (> 64 KiB).
    const Bytes raw_audio(68000u, 0x55u);
    // Entry 2: untouched small tail entry (< 64 KiB, CpkItocL).
    const Bytes tail_entry(900u, 0xAAu);

    const Bytes orig_cpk = BuildSyntheticSysCpk({comp_pak0, raw_audio, tail_entry});

    // Localized sub1: keeps 90% of bytes identical so XOR delta is mostly 0x00,
    // modifying a localized glyph region.
    Bytes en_sub1 = orig_sub1;
    const char kMarker[] = "ENGLISH_RENDERED_DXT5_BLOCKS_SAMPLE";
    std::memcpy(en_sub1.data() + 128, kMarker, sizeof(kMarker));

    menu_patch::SubTextureReplacement rep;
    rep.cpk_entry_id = 0u;
    rep.pak_sub_index = 1u;
    rep.orig_bytes = orig_sub1;
    rep.new_bytes = en_sub1;

    Bytes patch_blob;
    Bytes expected_patched_cpk;
    ASSERT_TRUE(menu_patch::MenuPatchPackage::BuildPatchBlob(
        orig_cpk, {rep}, patch_blob, &expected_patched_cpk));

    // Copyright / keyless invariant: literal marker string MUST NOT appear in
    // plaintext inside patch_blob.
    const auto* marker_begin = reinterpret_cast<const std::uint8_t*>(kMarker);
    const auto* marker_end = marker_begin + sizeof(kMarker) - 1;
    EXPECT_EQ(std::search(patch_blob.begin(), patch_blob.end(),
                          marker_begin, marker_end),
              patch_blob.end());

    // Header prefix parser must report exact metadata.
    menu_patch::PatchHeaderInfo hdr{};
    ASSERT_TRUE(menu_patch::MenuPatchPackage::ParseHeaderPrefix(
        patch_blob.data(), patch_blob.size(), hdr));
    EXPECT_EQ(hdr.version, menu_patch::kPatchFormatVersion);
    EXPECT_EQ(hdr.record_count, 1u);
    EXPECT_EQ(hdr.expected_orig_cpk_size, orig_cpk.size());
    EXPECT_EQ(hdr.expected_patched_cpk_size, expected_patched_cpk.size());

    // Apply via stateful MenuPatchPackage.
    menu_patch::MenuPatchPackage pkg;
    ASSERT_TRUE(pkg.LoadFromMemory(patch_blob));
    EXPECT_TRUE(pkg.IsLoaded());
    EXPECT_EQ(pkg.RecordCount(), 1u);

    Bytes applied_cpk;
    ASSERT_TRUE(pkg.ApplyToSysCpk(orig_cpk, applied_cpk));
    EXPECT_EQ(applied_cpk, expected_patched_cpk);

    // Verify "(c)CRI" sector marker at 0x7FA was preserved untouched.
    EXPECT_EQ(std::memcmp(applied_cpk.data() + 0x7FA, "(c)CRI", 6), 0);

    // Parse patched CPK TOC, decompress Entry 0, and verify sub0 is untouched,
    // sub1 equals en_sub1, and Entries 1 & 2 are byte-identical.
    std::vector<menu_patch::CpkEntrySlice> patched_toc;
    ASSERT_TRUE(menu_patch::ParseSysCpkToc(applied_cpk, patched_toc));
    ASSERT_EQ(patched_toc.size(), 3u);

    Bytes decomp_pak0;
    ASSERT_TRUE(menu_patch::LzssDecompressZeroRing(
        applied_cpk.data() + patched_toc[0].offset,
        patched_toc[0].size,
        decomp_pak0));
    std::vector<menu_patch::PakEntrySlice> pak_slices;
    ASSERT_TRUE(menu_patch::ParsePakArchive(decomp_pak0, pak_slices));
    ASSERT_EQ(pak_slices.size(), 2u);
    EXPECT_EQ(Bytes(decomp_pak0.begin() + pak_slices[0].offset,
                    decomp_pak0.begin() + pak_slices[0].offset + pak_slices[0].size),
              sub0);
    EXPECT_EQ(Bytes(decomp_pak0.begin() + pak_slices[1].offset,
                    decomp_pak0.begin() + pak_slices[1].offset + pak_slices[1].size),
              en_sub1);
    const auto off1 = static_cast<std::ptrdiff_t>(patched_toc[1].offset);
    const auto sz1 = static_cast<std::ptrdiff_t>(patched_toc[1].size);
    const auto off2 = static_cast<std::ptrdiff_t>(patched_toc[2].offset);
    const auto sz2 = static_cast<std::ptrdiff_t>(patched_toc[2].size);
    EXPECT_EQ(Bytes(applied_cpk.begin() + off1,
                    applied_cpk.begin() + off1 + sz1),
              raw_audio);
    EXPECT_EQ(Bytes(applied_cpk.begin() + off2,
                    applied_cpk.begin() + off2 + sz2),
              tail_entry);

    // Rejects tampered original archive (MurmurHash2 mismatch on orig_sub1).
    Bytes wrong_sub1 = orig_sub1;
    wrong_sub1[64] ^= 0xFFu;
    Bytes wrong_pak0 = BuildRawPak({sub0, wrong_sub1});
    Bytes wrong_comp_pak0;
    ASSERT_TRUE(menu_patch::LzssCompressZeroRing(wrong_pak0, wrong_comp_pak0));
    const Bytes wrong_cpk = BuildSyntheticSysCpk({wrong_comp_pak0, raw_audio, tail_entry});
    Bytes should_fail;
    EXPECT_FALSE(pkg.ApplyToSysCpk(wrong_cpk, should_fail));

    // Valid -> Failing -> Valid state seam on the SAME MenuPatchPackage instance:
    // a rejected LoadFromMemory must reset IsLoaded()/RecordCount() cleanly and
    // never poison a subsequent valid LoadFromMemory.
    Bytes corrupted_patch = patch_blob;
    corrupted_patch.back() ^= 0x01u;
    EXPECT_FALSE(pkg.LoadFromMemory(corrupted_patch));
    EXPECT_FALSE(pkg.IsLoaded());
    EXPECT_EQ(pkg.RecordCount(), 0u);
    Bytes rejected_apply;
    EXPECT_FALSE(pkg.ApplyToSysCpk(orig_cpk, rejected_apply));

    ASSERT_TRUE(pkg.LoadFromMemory(patch_blob));
    EXPECT_TRUE(pkg.IsLoaded());
    EXPECT_EQ(pkg.RecordCount(), 1u);
    Bytes recovered_cpk;
    ASSERT_TRUE(pkg.ApplyToSysCpk(orig_cpk, recovered_cpk));
    EXPECT_EQ(recovered_cpk, expected_patched_cpk);

    // Rejects trailing garbage appended to patch_blob.
    Bytes trailing_patch = patch_blob;
    trailing_patch.push_back(0x00u);
    EXPECT_FALSE(pkg.LoadFromMemory(trailing_patch));
    EXPECT_FALSE(pkg.IsLoaded());
    EXPECT_EQ(pkg.RecordCount(), 0u);
}
