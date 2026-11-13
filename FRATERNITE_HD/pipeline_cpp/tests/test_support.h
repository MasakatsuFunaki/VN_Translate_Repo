// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Test helpers: SHA-256, scratch directories, synthetic YPF builder.
#pragma once

#include "common/util.h"

#include <openssl/evp.h>
#include <process.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace frat_test {

inline constexpr int kHexHighNibbleShift = 4;
inline constexpr unsigned char kHexLowNibbleMask = 0xFu;
inline constexpr int kMaxZlibCompressionLevel = 9;
inline constexpr int kU32ByteCount = 4;
inline constexpr int kU64ByteCount = 8;
inline constexpr int kBitsPerByte = 8;
inline constexpr std::size_t kYpfEntryOverhead = 27u;
inline constexpr std::uint64_t kYpfHeaderSize = 32u;
inline constexpr std::uint8_t kYpfNameXorKey = 0xFFu;
inline constexpr std::uint8_t kCompressedFlag = 1u;
inline constexpr std::uint32_t kYpfVersion490 = 490u;
inline constexpr std::size_t kReservedHeaderZeros = 16u;

inline std::string sha256_hex(const void* data, std::size_t size) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data, size, md, &len, EVP_sha256(), nullptr);
    static constexpr const char* kHexDigits = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; ++i) {
        out += kHexDigits[md[i] >> kHexHighNibbleShift];
        out += kHexDigits[md[i] & kHexLowNibbleMask];
    }
    return out;
}

inline std::string sha256_hex(const std::string& s) {
    return sha256_hex(s.data(), s.size());
}

class ScratchDir {
public:
    explicit ScratchDir(const std::string& tag) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                std::filesystem::u8path(
                    "frat_ut_" + tag + "_" + std::to_string(++counter) + "_" +
                    std::to_string(static_cast<unsigned>(::_getpid())));
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::string str() const { return path_.u8string(); }
    std::string operator/(const std::string& leaf) const {
        return (path_ / std::filesystem::u8path(leaf)).u8string();
    }

private:
    std::filesystem::path path_;
};

inline frat::Bytes zlib_deflate(const frat::Bytes& in) {
    uLongf bound = ::compressBound(static_cast<uLong>(in.size()));
    frat::Bytes out(bound);
    ::compress2(out.data(), &bound, in.data(), static_cast<uLong>(in.size()),
                kMaxZlibCompressionLevel);
    out.resize(bound);
    return out;
}

inline void put_u32(frat::Bytes& b, std::uint32_t v) {
    for (int i = 0; i < kU32ByteCount; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (kBitsPerByte * i)));
    }
}

inline void put_u64(frat::Bytes& b, std::uint64_t v) {
    for (int i = 0; i < kU64ByteCount; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (kBitsPerByte * i)));
    }
}

inline frat::Bytes build_ypf(
    const std::vector<std::pair<std::string, frat::Bytes>>& files) {
    std::size_t index_size = 0;
    for (const auto& [name, body] : files) {
        index_size += kYpfEntryOverhead + name.size();
    }
    const std::uint64_t data_start = kYpfHeaderSize + index_size;

    frat::Bytes index;
    frat::Bytes data;
    for (const auto& [name, body] : files) {
        const frat::Bytes packed = zlib_deflate(body);
        put_u32(index, 0);
        index.push_back(static_cast<std::uint8_t>(name.size() ^ kYpfNameXorKey));
        for (char c : name) {
            index.push_back(static_cast<std::uint8_t>(c) ^ kYpfNameXorKey);
        }
        index.push_back(0);
        index.push_back(kCompressedFlag);
        put_u32(index, static_cast<std::uint32_t>(body.size()));
        put_u32(index, static_cast<std::uint32_t>(packed.size()));
        put_u64(index, data_start + data.size());
        put_u32(index, 0);
        data.insert(data.end(), packed.begin(), packed.end());
    }

    frat::Bytes out;
    for (char c : std::string("YPF")) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    out.push_back(0);
    put_u32(out, kYpfVersion490);
    put_u32(out, static_cast<std::uint32_t>(files.size()));
    put_u32(out, static_cast<std::uint32_t>(index_size));
    out.insert(out.end(), kReservedHeaderZeros, 0);
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

}  // namespace frat_test
