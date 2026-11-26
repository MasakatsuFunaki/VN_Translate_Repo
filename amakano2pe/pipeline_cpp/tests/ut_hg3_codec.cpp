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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

std::uint32_t ReadU32Le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Independent wire-format scanner locating `wanted_tag` in frame 0 of an HG-3 buffer.
std::optional<std::vector<std::uint8_t>> FindWireTagPayload(
    const std::vector<std::uint8_t>& hg3, std::string_view wanted_tag) {
    if (hg3.size() < kHg3HeaderBytes + kFrameHeaderBytes) {
        return std::nullopt;
    }
    const std::uint32_t next_frame_rel = ReadU32Le(hg3.data() + kHg3HeaderBytes);
    const std::size_t frame_end =
        (next_frame_rel != 0U && kHg3HeaderBytes + next_frame_rel <= hg3.size())
            ? (kHg3HeaderBytes + next_frame_rel)
            : hg3.size();
    std::size_t off = kHg3HeaderBytes + kFrameHeaderBytes;
    while (off + kTagHeaderBytes <= frame_end) {
        std::size_t name_len = 0;
        while (name_len < 8 && hg3[off + name_len] != 0) {
            ++name_len;
        }
        const std::string_view name(reinterpret_cast<const char*>(hg3.data() + off), name_len);
        const std::uint32_t next_rel = ReadU32Le(hg3.data() + off + 8);
        const std::uint32_t data_len = ReadU32Le(hg3.data() + off + 12);
        const std::size_t payload_off = off + kTagHeaderBytes;
        if (payload_off + data_len > frame_end) {
            return std::nullopt;
        }
        if (name == wanted_tag) {
            return std::vector<std::uint8_t>(
                hg3.begin() + static_cast<std::ptrdiff_t>(payload_off),
                hg3.begin() + static_cast<std::ptrdiff_t>(payload_off + data_len));
        }
        if (next_rel == 0U) {
            break;
        }
        off += next_rel;
    }
    return std::nullopt;
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
    // 32-bpp img0000 carries alpha inline and must not emit a redundant img_al tag.
    EXPECT_FALSE(FindWireTagPayload(reencoded, "img_al").has_value());
    const ama::hg3::Hg3Container round_tripped = ama::hg3::Hg3Codec::Decode(reencoded);
    EXPECT_TRUE(ama::hg3::Hg3Codec::VerifyFrameMetadataParity(decoded, round_tripped));
    EXPECT_EQ(round_tripped.frames[0].pixels.pixels, decoded.frames[0].pixels.pixels);
}

TEST(Hg3Codec, PreservesAlphaOn24BppImgJpgPlusImgAlAndDetectsAlphaLossInParityCheck) {
    constexpr int kW = 8;
    constexpr int kH = 6;
    ama::imaging::BgraImage base_img = ama::imaging::MakeBgraImage(kW, kH);
    std::vector<std::uint8_t> raw_alpha(static_cast<std::size_t>(kW * kH), 0);
    constexpr std::uint8_t kSeamAlpha[4] = {0x00U, 0x7FU, 0x80U, 0xFFU};
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            ama::imaging::SetPixel(
                base_img, x, y,
                ama::imaging::BgraColor{static_cast<std::uint8_t>(x * 31),
                                        static_cast<std::uint8_t>(y * 41),
                                        static_cast<std::uint8_t>((x + y) * 19), kOpaqueAlpha});
            const std::size_t idx = static_cast<std::size_t>(y * kW + x);
            raw_alpha[idx] = (idx < 4U)
                                 ? kSeamAlpha[idx]
                                 : static_cast<std::uint8_t>((x * 29 + y * 17 + 13) & 0xFF);
        }
    }

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
    WriteU32Le(stdinfo_payload.data() + 8, kBpp24);
    WriteU32Le(stdinfo_payload.data() + 20, kW);
    WriteU32Le(stdinfo_payload.data() + 24, kH);
    WriteU32Le(stdinfo_payload.data() + 28, 1U);

    std::vector<std::uint8_t> tags_buf;
    AppendRawTag(tags_buf, "stdinfo", stdinfo_payload, false);
    AppendRawTag(tags_buf, "img_jpg", wic_payload, false);
    AppendRawTag(tags_buf, "img_al", img_al_payload, true);

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
    EXPECT_EQ(decoded.frames[0].stdinfo.bpp, kBpp24);
    EXPECT_TRUE(decoded.frames[0].was_jpeg_encoded);
    EXPECT_TRUE(decoded.frames[0].has_img_al);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            EXPECT_EQ(ama::imaging::GetPixel(decoded.frames[0].pixels, x, y).a,
                      raw_alpha[static_cast<std::size_t>(y * kW + x)]);
        }
    }

    const std::vector<std::uint8_t> reencoded = ama::hg3::Hg3Codec::Encode(decoded);
    const std::optional<std::vector<std::uint8_t>> emitted_al =
        FindWireTagPayload(reencoded, "img_al");
    ASSERT_TRUE(emitted_al.has_value());
    ASSERT_GE(emitted_al->size(), kImgAlHeaderBytes);
    const std::uint32_t emitted_comp_len = ReadU32Le(emitted_al->data());
    const std::uint32_t emitted_orig_len = ReadU32Le(emitted_al->data() + 4);
    EXPECT_EQ(emitted_orig_len, static_cast<std::uint32_t>(kW * kH));
    EXPECT_EQ(kImgAlHeaderBytes + emitted_comp_len, emitted_al->size());

    std::vector<std::uint8_t> inflated_alpha(raw_alpha.size(), 0);
    uLongf inflated_len = static_cast<uLongf>(inflated_alpha.size());
    ASSERT_EQ(uncompress(reinterpret_cast<Bytef*>(inflated_alpha.data()), &inflated_len,
                         reinterpret_cast<const Bytef*>(emitted_al->data() + kImgAlHeaderBytes),
                         static_cast<uLong>(emitted_comp_len)),
              Z_OK);
    ASSERT_EQ(inflated_len, static_cast<uLongf>(raw_alpha.size()));
    EXPECT_EQ(inflated_alpha, raw_alpha);

    const ama::hg3::Hg3Container round_tripped = ama::hg3::Hg3Codec::Decode(reencoded);
    ASSERT_EQ(round_tripped.frames.size(), 1U);
    EXPECT_EQ(round_tripped.frames[0].stdinfo.bpp, kBpp24);
    EXPECT_TRUE(round_tripped.frames[0].has_img_al);
    EXPECT_TRUE(ama::hg3::Hg3Codec::VerifyFrameMetadataParity(decoded, round_tripped));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const ama::imaging::BgraColor px =
                ama::imaging::GetPixel(round_tripped.frames[0].pixels, x, y);
            EXPECT_EQ(px.a, raw_alpha[static_cast<std::size_t>(y * kW + x)]);
        }
    }
    EXPECT_EQ(round_tripped.frames[0].pixels.pixels, decoded.frames[0].pixels.pixels);

    // Synthetic 24-bpp frame with has_img_al == false and non-opaque seam alpha
    // must still emit img_al and preserve every alpha value losslessly.
    ama::hg3::Hg3Container synthetic_24_alpha = decoded;
    synthetic_24_alpha.frames[0].has_img_al = false;
    const std::vector<std::uint8_t> synth_encoded = ama::hg3::Hg3Codec::Encode(synthetic_24_alpha);
    ASSERT_TRUE(FindWireTagPayload(synth_encoded, "img_al").has_value());
    const ama::hg3::Hg3Container synth_decoded = ama::hg3::Hg3Codec::Decode(synth_encoded);
    ASSERT_EQ(synth_decoded.frames.size(), 1U);
    EXPECT_TRUE(synth_decoded.frames[0].has_img_al);
    EXPECT_EQ(synth_decoded.frames[0].pixels.pixels, decoded.frames[0].pixels.pixels);
    EXPECT_TRUE(ama::hg3::Hg3Codec::VerifyFrameMetadataParity(synthetic_24_alpha, synth_decoded));

    // Stripping img_al and flattening alpha to 0xFF must fail metadata parity.
    ama::hg3::Hg3Container alpha_dropped = round_tripped;
    alpha_dropped.frames[0].has_img_al = false;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            ama::imaging::BgraColor c = ama::imaging::GetPixel(alpha_dropped.frames[0].pixels, x, y);
            c.a = kOpaqueAlpha;
            ama::imaging::SetPixel(alpha_dropped.frames[0].pixels, x, y, c);
        }
    }
    EXPECT_FALSE(ama::hg3::Hg3Codec::VerifyFrameMetadataParity(decoded, alpha_dropped));

    // Opaque 24-bpp frame without img_al must not emit a spurious img_al tag.
    const std::vector<std::uint8_t> opaque_encoded = ama::hg3::Hg3Codec::Encode(alpha_dropped);
    EXPECT_FALSE(FindWireTagPayload(opaque_encoded, "img_al").has_value());
}

TEST(Hg3Codec, RejectsTruncatedOrInvalidBuffers) {
    const std::vector<std::uint8_t> bad_magic{'B', 'A', 'D', '!', 0x0C, 0, 0, 0, 0, 3, 0, 0};
    EXPECT_THROW((void)ama::hg3::Hg3Codec::Decode(bad_magic), std::runtime_error);

    const std::vector<std::uint8_t> too_short{'H', 'G', '-', '3'};
    EXPECT_THROW((void)ama::hg3::Hg3Codec::Decode(too_short), std::runtime_error);
}
