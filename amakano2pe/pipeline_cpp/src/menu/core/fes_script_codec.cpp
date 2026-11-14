// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/fes_script_codec.h"

#include <zlib.h>

#include <cstring>
#include <stdexcept>

namespace ama::menu {

namespace {

constexpr std::size_t kMagicBytes = 4;
constexpr const char kFesMagic[kMagicBytes] = {'F', 'E', 'S', '\0'};
constexpr const char kKcsMagic[kMagicBytes] = {'K', 'C', 'S', '\0'};

constexpr std::size_t kFesCompLenOffset = 4;
constexpr std::size_t kFesOrigLenOffset = 8;
constexpr std::size_t kFesReservedOffset = 12;

constexpr std::size_t kKcsCompLenFieldOffset = 8;
constexpr std::size_t kKcsHeaderSizeFieldOffset = 12;
constexpr std::size_t kKcsUnpackedSizeFieldOffset = 20;
constexpr std::uint32_t kDefaultKcsVersion = 2;

constexpr int kByteShift0 = 0;
constexpr int kByteShift1 = 8;
constexpr int kByteShift2 = 16;
constexpr int kByteShift3 = 24;
constexpr std::uint32_t kByteMask = 0xFFU;
constexpr std::size_t kInflateChunkBytes = 65536;

std::uint32_t ReadU32Le(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << kByteShift0) |
           (static_cast<std::uint32_t>(p[1]) << kByteShift1) |
           (static_cast<std::uint32_t>(p[2]) << kByteShift2) |
           (static_cast<std::uint32_t>(p[3]) << kByteShift3);
}

void WriteU32Le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>((v >> kByteShift0) & kByteMask);
    p[1] = static_cast<std::uint8_t>((v >> kByteShift1) & kByteMask);
    p[2] = static_cast<std::uint8_t>((v >> kByteShift2) & kByteMask);
    p[3] = static_cast<std::uint8_t>((v >> kByteShift3) & kByteMask);
}

std::vector<std::uint8_t> ZlibDecompress(const std::uint8_t* src, std::size_t comp_len,
                                         std::uint32_t expected_orig_len) {
    if (comp_len == 0) {
        if (expected_orig_len != 0) {
            throw std::runtime_error("zlib stream is empty for non-empty payload");
        }
        return {};
    }

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed in FesScriptCodec");
    }
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    zs.avail_in = static_cast<uInt>(comp_len);

    std::vector<std::uint8_t> out;
    if (expected_orig_len > 0) {
        out.reserve(expected_orig_len);
    }
    std::vector<std::uint8_t> chunk(kInflateChunkBytes);
    int rc = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(chunk.data());
        zs.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("inflate failed in FesScriptCodec");
        }
        const std::size_t produced = chunk.size() - static_cast<std::size_t>(zs.avail_out);
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(produced));
        if (rc == Z_BUF_ERROR && zs.avail_out != 0) {
            break;
        }
    } while (rc != Z_STREAM_END);

    inflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        throw std::runtime_error("truncated zlib stream in FesScriptCodec");
    }
    return out;
}

std::vector<std::uint8_t> ZlibCompress(const std::uint8_t* src, std::size_t orig_len) {
    uLongf bound = compressBound(static_cast<uLong>(orig_len));
    std::vector<std::uint8_t> compressed(static_cast<std::size_t>(bound));
    const Bytef* input_ptr =
        orig_len > 0 ? reinterpret_cast<const Bytef*>(src) : reinterpret_cast<const Bytef*>("");
    const int rc = compress2(reinterpret_cast<Bytef*>(compressed.data()), &bound, input_ptr,
                             static_cast<uLong>(orig_len), Z_BEST_COMPRESSION);
    if (rc != Z_OK) {
        throw std::runtime_error("compress2 failed in FesScriptCodec");
    }
    compressed.resize(static_cast<std::size_t>(bound));
    return compressed;
}

}  // namespace

struct FesScriptCodec::Impl {};

std::string FesPayload::AsText() const {
    return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

void FesPayload::SetText(std::string_view text) {
    body.assign(reinterpret_cast<const std::uint8_t*>(text.data()),
                reinterpret_cast<const std::uint8_t*>(text.data()) + text.size());
}

FesScriptCodec::FesScriptCodec() : impl_(std::make_unique<Impl>()) {}
FesScriptCodec::~FesScriptCodec() = default;
FesScriptCodec::FesScriptCodec(FesScriptCodec&&) noexcept = default;
FesScriptCodec& FesScriptCodec::operator=(FesScriptCodec&&) noexcept = default;

FesPayload FesScriptCodec::UnpackFes(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < kFesHeaderBytes) {
        throw std::runtime_error("FES buffer is smaller than header");
    }
    if (std::memcmp(data, kFesMagic, kMagicBytes) != 0) {
        throw std::runtime_error("invalid FES magic signature");
    }

    const std::uint32_t comp_len = ReadU32Le(data + kFesCompLenOffset);
    const std::uint32_t orig_len = ReadU32Le(data + kFesOrigLenOffset);
    const std::uint32_t reserved = ReadU32Le(data + kFesReservedOffset);
    if (kFesHeaderBytes + static_cast<std::size_t>(comp_len) > size) {
        throw std::runtime_error("FES compressed stream exceeds buffer size");
    }

    FesPayload result;
    result.reserved = reserved;
    result.body = ZlibDecompress(data + kFesHeaderBytes, comp_len, orig_len);
    return result;
}

FesPayload FesScriptCodec::UnpackFes(const std::vector<std::uint8_t>& bytes) {
    return UnpackFes(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> FesScriptCodec::PackFes(const FesPayload& payload) {
    return PackFes(payload.body.data(), payload.body.size(), payload.reserved);
}

std::vector<std::uint8_t> FesScriptCodec::PackFes(const std::uint8_t* body, std::size_t size,
                                                  std::uint32_t reserved) {
    if (body == nullptr && size > 0) {
        throw std::invalid_argument("null body pointer with non-zero size in PackFes");
    }

    const std::vector<std::uint8_t> comp = ZlibCompress(body, size);
    std::vector<std::uint8_t> out(kFesHeaderBytes + comp.size());
    std::memcpy(out.data(), kFesMagic, kMagicBytes);
    WriteU32Le(out.data() + kFesCompLenOffset, static_cast<std::uint32_t>(comp.size()));
    WriteU32Le(out.data() + kFesOrigLenOffset, static_cast<std::uint32_t>(size));
    WriteU32Le(out.data() + kFesReservedOffset, reserved);
    if (!comp.empty()) {
        std::memcpy(out.data() + kFesHeaderBytes, comp.data(), comp.size());
    }
    return out;
}

std::vector<std::uint8_t> FesScriptCodec::PackFesText(std::string_view text,
                                                      std::uint32_t reserved) {
    return PackFes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), reserved);
}

KcsPayload FesScriptCodec::UnpackKcs(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < kKcsHeaderBytes) {
        throw std::runtime_error("KCS buffer is smaller than header");
    }
    if (std::memcmp(data, kKcsMagic, kMagicBytes) != 0) {
        throw std::runtime_error("invalid KCS magic signature");
    }

    const std::uint32_t header_size = ReadU32Le(data + kKcsHeaderSizeFieldOffset);
    if (header_size != kKcsHeaderBytes) {
        throw std::runtime_error("unsupported KCS header size");
    }

    const std::uint32_t recorded_comp_len = ReadU32Le(data + kKcsCompLenFieldOffset);
    const std::uint32_t orig_len = ReadU32Le(data + kKcsUnpackedSizeFieldOffset);
    const std::size_t available_comp = size - kKcsHeaderBytes;
    const std::size_t comp_len =
        (recorded_comp_len != 0 && recorded_comp_len <= available_comp)
            ? static_cast<std::size_t>(recorded_comp_len)
            : available_comp;

    KcsPayload result;
    std::memcpy(result.header.data(), data, kKcsHeaderBytes);
    result.body = ZlibDecompress(data + kKcsHeaderBytes, comp_len, orig_len);
    return result;
}

KcsPayload FesScriptCodec::UnpackKcs(const std::vector<std::uint8_t>& bytes) {
    return UnpackKcs(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> FesScriptCodec::PackKcs(const KcsPayload& payload) {
    std::array<std::uint8_t, kKcsHeaderBytes> hdr = payload.header;
    if (std::memcmp(hdr.data(), kKcsMagic, kMagicBytes) != 0) {
        std::memcpy(hdr.data(), kKcsMagic, kMagicBytes);
        WriteU32Le(hdr.data() + kMagicBytes, kDefaultKcsVersion);
        WriteU32Le(hdr.data() + kKcsHeaderSizeFieldOffset,
                   static_cast<std::uint32_t>(kKcsHeaderBytes));
    }

    const std::vector<std::uint8_t> comp =
        ZlibCompress(payload.body.data(), payload.body.size());
    const std::uint32_t old_comp_field = ReadU32Le(hdr.data() + kKcsCompLenFieldOffset);
    if (old_comp_field != 0) {
        WriteU32Le(hdr.data() + kKcsCompLenFieldOffset,
                   static_cast<std::uint32_t>(comp.size()));
    }
    WriteU32Le(hdr.data() + kKcsUnpackedSizeFieldOffset,
               static_cast<std::uint32_t>(payload.body.size()));

    std::vector<std::uint8_t> out(kKcsHeaderBytes + comp.size());
    std::memcpy(out.data(), hdr.data(), kKcsHeaderBytes);
    if (!comp.empty()) {
        std::memcpy(out.data() + kKcsHeaderBytes, comp.data(), comp.size());
    }
    return out;
}

}  // namespace ama::menu
