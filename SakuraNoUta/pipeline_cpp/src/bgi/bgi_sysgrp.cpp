// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "bgi/bgi_sysgrp.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace snu::bgi {
namespace {

constexpr std::size_t kWidthOffset = 0;
constexpr std::size_t kHeightOffset = 2;
constexpr std::size_t kBppOffset = 4;
constexpr std::size_t kReservedOffset = 6;
constexpr int kBitsPerByte = 8;
constexpr int kMaxU16Dimension = static_cast<int>(std::numeric_limits<std::uint16_t>::max());

std::uint16_t ReadLe16(const Bytes& data, std::size_t offset) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, data.data() + offset, sizeof(v));
    return v;
}

void WriteLe16(Bytes& dst, std::uint16_t v) {
    dst.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<std::uint8_t>((v >> kBitsPerByte) & 0xFFu));
}

}  // namespace

SysGrpImage decode_sysgrp(const Bytes& decompressed_raw) {
    if (decompressed_raw.size() < kSysGrpHeaderBytes) {
        throw std::runtime_error("sysgrp: payload shorter than 16-byte header");
    }

    const std::uint16_t width = ReadLe16(decompressed_raw, kWidthOffset);
    const std::uint16_t height = ReadLe16(decompressed_raw, kHeightOffset);
    const std::uint16_t bpp = ReadLe16(decompressed_raw, kBppOffset);

    if (width == 0u || height == 0u) {
        throw std::runtime_error("sysgrp: zero image dimension");
    }
    if (bpp != snu::menu::kBmpBpp24 && bpp != snu::menu::kBmpBpp32) {
        throw std::runtime_error("sysgrp: unsupported bpp");
    }

    const auto bytes_per_px = static_cast<std::size_t>(bpp / kBitsPerByte);
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t expected_size = kSysGrpHeaderBytes + pixel_count * bytes_per_px;
    if (decompressed_raw.size() != expected_size) {
        throw std::runtime_error("sysgrp: payload size does not match header dimensions");
    }

    std::array<std::uint8_t, kSysGrpReservedBytes> reserved{};
    std::memcpy(reserved.data(), decompressed_raw.data() + kReservedOffset, kSysGrpReservedBytes);

    Bytes bgra(pixel_count * static_cast<std::size_t>(snu::menu::kBgraBytesPerPixel));
    const std::uint8_t* src = decompressed_raw.data() + kSysGrpHeaderBytes;

    if (bpp == snu::menu::kBmpBpp32) {
        std::memcpy(bgra.data(), src, bgra.size());
    } else {
        for (std::size_t i = 0; i < pixel_count; ++i) {
            const std::size_t s_off = i * static_cast<std::size_t>(snu::menu::kBgrBytesPerPixel);
            const std::size_t d_off = i * static_cast<std::size_t>(snu::menu::kBgraBytesPerPixel);
            bgra[d_off + snu::menu::kChannelBlue] = src[s_off + snu::menu::kChannelBlue];
            bgra[d_off + snu::menu::kChannelGreen] = src[s_off + snu::menu::kChannelGreen];
            bgra[d_off + snu::menu::kChannelRed] = src[s_off + snu::menu::kChannelRed];
            bgra[d_off + snu::menu::kChannelAlpha] = snu::menu::kOpaqueAlpha;
        }
    }

    return SysGrpImage{
        snu::menu::BgraImage(static_cast<int>(width), static_cast<int>(height), std::move(bgra)),
        static_cast<int>(bpp),
        reserved,
    };
}

Bytes encode_sysgrp(const snu::menu::BgraImage& image, int bpp,
                    const std::array<std::uint8_t, kSysGrpReservedBytes>& reserved) {
    if (image.empty() || image.width() > kMaxU16Dimension || image.height() > kMaxU16Dimension) {
        throw std::runtime_error("sysgrp: invalid image dimensions for encoding");
    }
    if (bpp != snu::menu::kBmpBpp24 && bpp != snu::menu::kBmpBpp32) {
        throw std::runtime_error("sysgrp: unsupported bpp for encoding");
    }

    const auto bytes_per_px = static_cast<std::size_t>(bpp / kBitsPerByte);
    const std::size_t pixel_count =
        static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height());

    Bytes out;
    out.reserve(kSysGrpHeaderBytes + pixel_count * bytes_per_px);
    WriteLe16(out, static_cast<std::uint16_t>(image.width()));
    WriteLe16(out, static_cast<std::uint16_t>(image.height()));
    WriteLe16(out, static_cast<std::uint16_t>(bpp));
    out.insert(out.end(), reserved.begin(), reserved.end());

    const std::uint8_t* src = image.data();
    if (bpp == snu::menu::kBmpBpp32) {
        out.insert(out.end(), src, src + image.byte_size());
        return out;
    }

    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::size_t s_off = i * static_cast<std::size_t>(snu::menu::kBgraBytesPerPixel);
        out.push_back(src[s_off + snu::menu::kChannelBlue]);
        out.push_back(src[s_off + snu::menu::kChannelGreen]);
        out.push_back(src[s_off + snu::menu::kChannelRed]);
    }
    return out;
}

}  // namespace snu::bgi
