// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Test helpers: SHA-256, scratch directories, synthetic YPF builder.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <zlib.h>

#include "common/util.h"

namespace frat_test {

inline std::string sha256_hex(const void* data, std::size_t size) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data, size, md, &len, EVP_sha256(), nullptr);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; ++i) {
        out += hex[md[i] >> 4];
        out += hex[md[i] & 0xF];
    }
    return out;
}

inline std::string sha256_hex(const std::string& s) { return sha256_hex(s.data(), s.size()); }

class ScratchDir {
public:
    explicit ScratchDir(const std::string& tag) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("frat_ut_" + tag + "_" + std::to_string(++counter) + "_" +
                 std::to_string(static_cast<unsigned>(::_getpid())));
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::string str() const { return path_.string(); }
    std::string operator/(const std::string& leaf) const { return (path_ / leaf).string(); }

private:
    std::filesystem::path path_;
};

inline frat::Bytes zlib_deflate(const frat::Bytes& in) {
    uLongf bound = ::compressBound(static_cast<uLong>(in.size()));
    frat::Bytes out(bound);
    ::compress2(out.data(), &bound, in.data(), static_cast<uLong>(in.size()), 9);
    out.resize(bound);
    return out;
}

inline void put_u32(frat::Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
inline void put_u64(frat::Bytes& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

inline frat::Bytes build_ypf(const std::vector<std::pair<std::string, frat::Bytes>>& files) {
    std::size_t index_size = 0;
    for (const auto& [name, body] : files) index_size += 27 + name.size();
    const std::uint64_t data_start = 32 + index_size;

    frat::Bytes index, data;
    for (const auto& [name, body] : files) {
        const frat::Bytes packed = zlib_deflate(body);
        put_u32(index, 0);  // name_hash (unused)
        index.push_back(static_cast<std::uint8_t>(name.size() ^ 0xFF));
        for (char c : name) index.push_back(static_cast<std::uint8_t>(c) ^ 0xFF);
        index.push_back(0);  // type
        index.push_back(1);  // compressed
        put_u32(index, static_cast<std::uint32_t>(body.size()));
        put_u32(index, static_cast<std::uint32_t>(packed.size()));
        put_u64(index, data_start + data.size());
        put_u32(index, 0);  // data_hash (unused)
        data.insert(data.end(), packed.begin(), packed.end());
    }

    frat::Bytes out;
    for (char c : std::string("YPF")) out.push_back(static_cast<std::uint8_t>(c));
    out.push_back(0);
    put_u32(out, 490);
    put_u32(out, static_cast<std::uint32_t>(files.size()));
    put_u32(out, static_cast<std::uint32_t>(index_size));
    out.insert(out.end(), 16, 0);
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

inline std::string samples_dir() {
    return std::string(FRAT_PROJECT_DIR) + "\\analysys\\ybn_samples";
}

inline frat::Bytes sample(const std::string& name) {
    return frat::read_file(samples_dir() + "\\" + name);
}

}  // namespace frat_test
