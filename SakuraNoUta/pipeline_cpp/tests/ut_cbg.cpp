// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CompressedBG decoder tests against a hand-crafted 2x2 24bpp blob with
// hand-computed differential-decode expectations, plus BMP writer shape.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "bgi/bgi_archive.h"
#include "bgi/bgi_cbg.h"
#include "cg/image.h"

using snu::Bytes;
namespace bgi = snu::bgi;

namespace {

// Mirror of the decoder's PRNG (asmodean hash, same as DSC).
std::uint32_t cbg_hash_update(std::uint32_t& state) {
    std::uint32_t eax = (state & 0xFFFF) * 20021u;
    std::uint32_t edx = ((state >> 16) & 0xFFFF) * 20021u + state * 346u;
    edx = edx + (eax >> 16);
    state = (((edx & 0xFFFF) << 16) | (eax & 0xFFFF)) + 1u;
    return edx & 0x7FFF;
}

class BitWriter {
public:
    void put_bits(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            cur_ = static_cast<std::uint8_t>((cur_ << 1) | ((value >> i) & 1));
            if (++count_ == 8) {
                out_.push_back(cur_);
                cur_ = 0;
                count_ = 0;
            }
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

// All 256 leaf weights = 1 gives a perfect merge-two-smallest tree where
// symbol b's Huffman code is b in 8-bit MSB-first binary.
Bytes make_cbg_2x2(const Bytes& rle_stream, std::uint32_t key = 0xCAFE) {
    Bytes blob(0x30, 0);
    const char magic[] = "CompressedBG___";
    std::memcpy(blob.data(), magic, 16);
    auto put16 = [&](std::size_t off, std::uint16_t v) {
        blob[off] = static_cast<std::uint8_t>(v & 0xFF);
        blob[off + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            blob[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    put16(0x10, 2);   // width
    put16(0x12, 2);   // height
    put16(0x14, 24);  // bpp
    put32(0x20, static_cast<std::uint32_t>(rle_stream.size()));  // intermediate len
    put32(0x24, key);
    put32(0x28, 256);  // enc length: 256 single-byte weight varints
    // 256 x weight-1 varints: plain bytes all 0x01 -> sum = 0x00, xor = 0x00.
    blob[0x2C] = 0x00;
    blob[0x2D] = 0x00;
    put16(0x2E, 1);  // version

    std::uint32_t state = key;
    for (int i = 0; i < 256; ++i) {
        std::uint32_t adv = cbg_hash_update(state);
        blob.push_back(static_cast<std::uint8_t>((1 + adv) & 0xFF));
    }

    BitWriter bw;
    for (std::uint8_t b : rle_stream) bw.put_bits(b, 8);
    Bytes bits = bw.finish();
    blob.insert(blob.end(), bits.begin(), bits.end());
    return blob;
}

}  // namespace

TEST(Cbg, cbg_decode_2x2_diff) {
    // Deltas (BGR): p00=(10,20,30) p10=(+1,+1,+1) p01=(+5,+5,+5) p11=(+2,+2,+2)
    // RLE: single data run of 12 -> varint (12<<1)|0 = 24, then the bytes.
    Bytes rle = {24, 10, 20, 30, 1, 1, 1, 5, 5, 5, 2, 2, 2};
    Bytes blob = make_cbg_2x2(rle);
    bgi::CbgImage img = bgi::decode_cbg(blob);
    EXPECT_EQ(img.width, 2);
    EXPECT_EQ(img.height, 2);
    EXPECT_EQ(img.bpp, 24);
    // Row 0: left-neighbour sums; row 1: (left+above)>>1 predictor.
    const Bytes expect = {10, 20, 30, 11, 21, 31, 15, 25, 35, 15, 25, 35};
    EXPECT_EQ(img.pixels, expect);
}

TEST(Cbg, cbg_zero_run) {
    // First varint (5<<1)|1 = 11 -> 5 zero bytes; the follow-up varint is a
    // RAW length (only the first one carries the mode bit): 7 data bytes.
    Bytes rle = {11, 7, 100, 100, 100, 1, 2, 3, 4};
    Bytes blob = make_cbg_2x2(rle);
    bgi::CbgImage img = bgi::decode_cbg(blob);
    // deltas = [0,0,0, 0,0,100, 100,100,1, 2,3,4]
    // row0: p00=(0,0,0); p10=(0,0,100)
    // row1: p01 = delta+above = (100,100,1)
    //       p11: left=(100,100,1) above=(0,0,100) avg=(50,50,50)
    //            +(2,3,4) = (52,53,54)
    const Bytes expect = {0, 0, 0, 0, 0, 100, 100, 100, 1, 52, 53, 54};
    EXPECT_EQ(img.pixels, expect);
}

TEST(Cbg, cbg_checksum_mismatch_throws) {
    Bytes rle = {24, 10, 20, 30, 1, 1, 1, 5, 5, 5, 2, 2, 2};
    Bytes blob = make_cbg_2x2(rle);
    blob[0x2C] ^= 0xFF;  // corrupt sum check
    EXPECT_THROW(bgi::decode_cbg(blob), std::runtime_error);
}

TEST(Cbg, cbg_v2_rejected) {
    Bytes rle = {24, 10, 20, 30, 1, 1, 1, 5, 5, 5, 2, 2, 2};
    Bytes blob = make_cbg_2x2(rle);
    blob[0x2E] = 2;  // version 2 = DCT format, not implemented
    EXPECT_THROW(bgi::decode_cbg(blob), std::runtime_error);
}

TEST(Cbg, cbg_bad_magic_throws) {
    Bytes junk(64, 0);
    EXPECT_THROW(bgi::decode_cbg(junk), std::runtime_error);
}

// Cross-validate against GARbro on a real v1 CBG from data02503.arc.
// Skips when the game or extract_cbg.exe is absent.
TEST(Cbg, real_cbg_matches_garbro_reference) {
    namespace fs = std::filesystem;
    const std::string arc = "C:\\SakuraNoUta\\data02503.arc";
    const std::string ref_exe =
        std::string(SNU_PROJECT_DIR) + "\\..\\TOOLS\\garbro\\extract_cbg.exe";
    if (!fs::exists(fs::u8path(arc)) || !fs::exists(fs::u8path(ref_exe))) {
        GTEST_SKIP() << "game data or extract_cbg.exe not available";
    }

    // First v1 CBG entry in the arc (version u16 @0x2E).
    auto entries = snu::bgi::list_arc(arc);
    Bytes raw;
    std::string picked;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Bytes r = snu::bgi::read_entry(arc, i, /*decompress=*/false);
        if (snu::bgi::is_cbg(r) && r.size() > 0x30 &&
            r[0x2E] == 1 && r[0x2F] == 0) {
            raw = std::move(r);
            picked = entries[i].name;
            break;
        }
    }
    if (raw.empty()) {
        GTEST_SKIP() << "no v1 CBG entry found in " << arc;
    }
    GTEST_LOG_(INFO) << "cross-validating entry: " << picked;

    bgi::CbgImage ours = bgi::decode_cbg(raw);

    std::string tmp_cbg = (fs::temp_directory_path() / "snu_ut_ref.cbg").u8string();
    std::string tmp_bmp = (fs::temp_directory_path() / "snu_ut_ref.bmp").u8string();
    snu::write_file(tmp_cbg, raw);
    std::string cmd = "\"\"" + ref_exe + "\" \"" + tmp_cbg + "\" \"" + tmp_bmp + "\"\"";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    Bytes ref_bmp = snu::read_file(tmp_bmp);
    auto ref = snu::cg::decode_image(ref_bmp);
    ASSERT_TRUE(ref.has_value());
    snu::cg::Image mine =
        snu::cg::from_cbg_pixels(ours.pixels, ours.width, ours.height, ours.bpp);

    ASSERT_EQ(mine.width, ref->width);
    ASSERT_EQ(mine.height, ref->height);
    EXPECT_EQ(mine.rgb, ref->rgb);

    fs::remove(fs::u8path(tmp_cbg));
    fs::remove(fs::u8path(tmp_bmp));
}

TEST(Cbg, to_bmp_shape) {
    // 2x1 BGR image -> 24-bit BMP: 54-byte header + one padded row.
    Bytes px = {1, 2, 3, 4, 5, 6};
    Bytes bmp = bgi::to_bmp(px, 2, 1, 24);
    ASSERT_GE(bmp.size(), 54u + 8u);
    EXPECT_EQ(bmp[0], 'B');
    EXPECT_EQ(bmp[1], 'M');
    auto rd32 = [&](std::size_t off) {
        return static_cast<std::uint32_t>(bmp[off]) | (bmp[off + 1] << 8) |
               (bmp[off + 2] << 16) | (bmp[off + 3] << 24);
    };
    EXPECT_EQ(rd32(10), 54u);       // pixel data offset
    EXPECT_EQ(rd32(18), 2u);        // width
    EXPECT_EQ(rd32(22), 1u);        // height
    EXPECT_EQ(bmp[28], 24);         // bpp
    // Row data: bottom-up equals the single row, padded to 8 bytes.
    EXPECT_EQ(bmp[54], 1);
    EXPECT_EQ(bmp[57], 4);
}
