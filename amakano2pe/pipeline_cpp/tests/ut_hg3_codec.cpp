// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "hg3/hg3_codec.h"
#include "imaging/wic_image_io.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t kFrameIdFirst = 0;
constexpr std::uint32_t kFrameIdSecond = 108;
constexpr int kFrameWidth32 = 24;
constexpr int kFrameHeight32 = 16;
constexpr int kFrameWidth24 = 19;
constexpr int kFrameHeight24 = 11;
constexpr std::uint32_t kBpp32 = 32;
constexpr std::uint32_t kBpp24 = 24;
constexpr std::uint8_t kOpaqueAlpha = 0xFFU;
constexpr std::size_t kHg3HeaderBytes = 12;
constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::size_t kTagHeaderBytes = 16;
constexpr std::size_t kStdInfoBytes = 40;
constexpr std::size_t kImgAlHeaderBytes = 8;

void WriteU32Le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
}

void AppendRawTag(std::vector<std::uint8_t>& buf, const char* name,
                  const std::vector<std::uint8_t>& payload, bool is_last) {
    const std::size_t start = buf.size();
    buf.resize(start + kTagHeaderBytes + payload.size(), 0);
    std::memcpy(buf.data() + start, name, std::min<std::size_t>(std::strlen(name), 8));
    const std::uint32_t next_rel =
        is_last ? 0U : static_cast<std::uint32_t>(kTagHeaderBytes + payload.size());
    WriteU32Le(buf.data() + start + 8, next_rel);
    WriteU32Le(buf.data() + start + 12, static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(buf.data() + start + kTagHeaderBytes, payload.data(), payload.size());
    }
}

}  // namespace

TEST(Hg3Codec, MultiFrame24BppAnd32BppImg0000RoundTripAndMetadataParity) {
    ama::hg3::Hg3Container orig;

    ama::hg3::Hg3Frame f0;
    f0.frame_id = kFrameIdFirst;
    f0.stdinfo.width = kFrameWidth32;
    f0.stdinfo.height = kFrameHeight32;
    f0.stdinfo.bpp = kBpp32;
    f0.stdinfo.offset_x = 12;
    f0.stdinfo.offset_y = 34;
    f0.stdinfo.total_width = 1280;
    f0.stdinfo.total_height = 720;
    f0.stdinfo.transparent_flag = 1;
    f0.stdinfo.canvas_x = -4;
    f0.stdinfo.canvas_y = 8;
    f0.cptype = 0U;
    f0.imgmode = 1U;
    f0.extra_tags.push_back({"ats0000", {0x11, 0x22, 0x33, 0x44}});
    f0.pixels = ama::imaging::MakeBgraImage(kFrameWidth32, kFrameHeight32);
    for (int y = 0; y < kFrameHeight32; ++y) {
        for (int x = 0; x < kFrameWidth32; ++x) {
            ama::imaging::SetPixel(
                f0.pixels, x, y,
                ama::imaging::BgraColor{
                    static_cast<std::uint8_t>((x * 11 + y * 7) & 0xFF),
                    static_cast<std::uint8_t>((x * 5 + y * 13) & 0xFF),
                    static_cast<std::uint8_t>((x * 17 + y * 3) & 0xFF),
                    static_cast<std::uint8_t>((x + y) % 3 == 0 ? 0 : ((x * 19 + y * 23) & 0xFF))});
        }
    }
    orig.frames.push_back(f0);

    ama::hg3::Hg3Frame spacer;
    spacer.frame_id = 54U;
    spacer.has_stdinfo = false;
    spacer.cptype = 0U;
    orig.frames.push_back(spacer);

    ama::hg3::Hg3Frame f1;
    f1.frame_id = kFrameIdSecond;
    f1.stdinfo.width = kFrameWidth24;
    f1.stdinfo.height = kFrameHeight24;
    f1.stdinfo.bpp = kBpp24;
    f1.stdinfo.offset_x = 80;
    f1.stdinfo.offset_y = 120;
    f1.stdinfo.total_width = 1280;
    f1.stdinfo.total_height = 720;
    f1.stdinfo.transparent_flag = 0;
    f1.pixels = ama::imaging::MakeBgraImage(kFrameWidth24, kFrameHeight24);
    for (int y = 0; y < kFrameHeight24; ++y) {
        for (int x = 0; x < kFrameWidth24; ++x) {
            ama::imaging::SetPixel(f1.pixels, x, y,
                                   ama::imaging::BgraColor{static_cast<std::uint8_t>(x * 9),
                                                           static_cast<std::uint8_t>(y * 21),
                                                           static_cast<std::uint8_t>(200 - x * 4),
                                                           kOpaqueAlpha});
        }
    }
    orig.frames.push_back(f1);

    const std::vector<std::uint8_t> encoded = ama::hg3::Hg3Codec::Encode(orig);
    const ama::hg3::Hg3Container decoded = ama::hg3::Hg3Codec::Decode(encoded);

    ASSERT_TRUE(ama::hg3::Hg3Codec::VerifyFrameMetadataParity(orig, decoded));
    ASSERT_EQ(decoded.frames.size(), 3U);
    EXPECT_TRUE(decoded.frames[0].has_stdinfo);
    EXPECT_EQ(decoded.frames[0].extra_tags, orig.frames[0].extra_tags);
    EXPECT_EQ(decoded.frames[0].pixels.pixels, orig.frames[0].pixels.pixels);
    EXPECT_FALSE(decoded.frames[1].has_stdinfo);
    EXPECT_EQ(decoded.frames[1].frame_id, 54U);
    EXPECT_EQ(decoded.frames[1].cptype, 0U);
    EXPECT_TRUE(decoded.frames[2].has_stdinfo);
    EXPECT_EQ(decoded.frames[2].pixels.pixels, orig.frames[2].pixels.pixels);
    EXPECT_NE(decoded.FindFrame(kFrameIdSecond), nullptr);
    EXPECT_EQ(decoded.FindFrame(999U), nullptr);
}

TEST(Hg3Codec, DecodesImgJpgPlusImgAlAndReencodesLosslesslyAsImg0000) {
    constexpr int kW = 8;
    constexpr int kH = 6;
    ama::imaging::BgraImage base_img = ama::imaging::MakeBgraImage(kW, kH);
    std::vector<std::uint8_t> raw_alpha(static_cast<std::size_t>(kW * kH), 0);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            ama::imaging::SetPixel(base_img, x, y,
                                   ama::imaging::BgraColor{40, 120, 220, kOpaqueAlpha});
            raw_alpha[static_cast<std::size_t>(y * kW + x)] =
                static_cast<std::uint8_t>((x * 25 + y * 15) & 0xFF);
        }
    }

    // WIC DecodeImage decodes any WIC container format in img_jpg (JPEG/PNG/BMP).
    const std::vector<std::uint8_t> wic_payload = ama::imaging::EncodePng(base_img);

    uLongf comp_bound = compressBound(static_cast<uLong>(raw_alpha.size()));
    std::vector<std::uint8_t> img_al_payload(kImgAlHeaderBytes + comp_bound, 0);
    ASSERT_EQ(compress2(reinterpret_cast<Bytef*>(img_al_payload.data() + kImgAlHeaderBytes),
                        &comp_bound, reinterpret_cast<const Bytef*>(raw_alpha.data()),
                        static_cast<uLong>(raw_alpha.size()), Z_BEST_COMPRESSION),
              Z_OK);
    img_al_payload.resize(kImgAlHeaderBytes + comp_bound);
    WriteU32Le(img_al_payload.data(), static_cast<std::uint32_t>(comp_bound));
    WriteU32Le(img_al_payload.data() + 4, static_cast<std::uint32_t>(raw_alpha.size()));

    std::vector<std::uint8_t> stdinfo_payload(kStdInfoBytes, 0);
    WriteU32Le(stdinfo_payload.data() + 0, kW);
    WriteU32Le(stdinfo_payload.data() + 4, kH);
    WriteU32Le(stdinfo_payload.data() + 8, kBpp32);
    WriteU32Le(stdinfo_payload.data() + 20, kW);
    WriteU32Le(stdinfo_payload.data() + 24, kH);
    WriteU32Le(stdinfo_payload.data() + 28, 1U);

    std::vector<std::uint8_t> imgmode_payload(4, 0);
    WriteU32Le(imgmode_payload.data(), 1U);

    std::vector<std::uint8_t> tags_buf;
    AppendRawTag(tags_buf, "stdinfo", stdinfo_payload, false);
    AppendRawTag(tags_buf, "img_jpg", wic_payload, false);
    AppendRawTag(tags_buf, "img_al", img_al_payload, false);
    AppendRawTag(tags_buf, "imgmode", imgmode_payload, true);

    std::vector<std::uint8_t> hg3_buf(kHg3HeaderBytes + kFrameHeaderBytes + tags_buf.size(), 0);
    std::memcpy(hg3_buf.data(), "HG-3", 4);
    WriteU32Le(hg3_buf.data() + 4, 0x0CU);
    WriteU32Le(hg3_buf.data() + 8, 0x0300U);
    WriteU32Le(hg3_buf.data() + kHg3HeaderBytes, 0U);
    WriteU32Le(hg3_buf.data() + kHg3HeaderBytes + 4, kFrameIdSecond);
    std::memcpy(hg3_buf.data() + kHg3HeaderBytes + kFrameHeaderBytes, tags_buf.data(),
                tags_buf.size());

    const ama::hg3::Hg3Container decoded = ama::hg3::Hg3Codec::Decode(hg3_buf);
    ASSERT_EQ(decoded.frames.size(), 1U);
    EXPECT_TRUE(decoded.frames[0].was_jpeg_encoded);
    ASSERT_TRUE(decoded.frames[0].imgmode.has_value());
    EXPECT_EQ(*decoded.frames[0].imgmode, 1U);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            EXPECT_EQ(ama::imaging::GetPixel(decoded.frames[0].pixels, x, y).a,
                      raw_alpha[static_cast<std::size_t>(y * kW + x)]);
        }
    }

    const std::vector<std::uint8_t> reencoded = ama::hg3::Hg3Codec::Encode(decoded);
    const ama::hg3::Hg3Container round_tripped = ama::hg3::Hg3Codec::Decode(reencoded);
    EXPECT_TRUE(ama::hg3::Hg3Codec::VerifyFrameMetadataParity(decoded, round_tripped));
    EXPECT_EQ(round_tripped.frames[0].pixels.pixels, decoded.frames[0].pixels.pixels);
}

TEST(Hg3Codec, RejectsTruncatedOrInvalidBuffers) {
    const std::vector<std::uint8_t> bad_magic{'B', 'A', 'D', '!', 0x0C, 0, 0, 0, 0, 3, 0, 0};
    EXPECT_THROW((void)ama::hg3::Hg3Codec::Decode(bad_magic), std::runtime_error);

    const std::vector<std::uint8_t> too_short{'H', 'G', '-', '3'};
    EXPECT_THROW((void)ama::hg3::Hg3Codec::Decode(too_short), std::runtime_error);
}
