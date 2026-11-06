// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// BLACKCyc (nnn/BCmkri) `.spt` script reader.
//
// SPT files are XOR-0xFF encrypted and hold CP932 text. After decryption the
// header carries the magic "SPTHEADER0" at 0x30 and a string count at 0x18;
// the payload from 0x80 on is a run of NUL-terminated strings, 4-byte aligned.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/util.h"

namespace exm::spt {

constexpr std::size_t kDefaultSkipOffset = 0x80;

// XOR every byte with 0xFF.
Bytes decrypt(const Bytes& data);

// True when the decrypted buffer carries the SPT header magic.
bool is_valid(const Bytes& dec);

// uint32 LE at 0x18 -- the header's own string count (informational).
std::uint32_t header_string_count(const Bytes& dec);

// Japanese test used by the extractor: kana, CJK, CJK-ext-A, fullwidth forms
// and CJK symbols.
bool has_japanese(const std::string& utf8);

struct SptString {
    std::uint32_t offset = 0;
    std::uint32_t byte_len = 0;
    std::string text;  // UTF-8
    bool has_jp = false;
    std::string type;  // narrative | dialogue | name | menu | label | empty | other
};

// Walk the NUL-terminated string run from `skip_offset`.
std::vector<SptString> extract_strings(const Bytes& dec,
                                       std::size_t skip_offset = kDefaultSkipOffset);

// narrative / dialogue / name / menu / label / empty / other.
std::string classify(const std::string& text);

// Insulating wrapper around an encrypted `.spt` buffer that decrypts,
// validates the header, and extracts classified strings on construction.
class SptScript {
public:
    explicit SptScript(Bytes raw, std::size_t skip_offset = kDefaultSkipOffset);
    ~SptScript();

    SptScript(const SptScript&) = delete;
    SptScript& operator=(const SptScript&) = delete;
    SptScript(SptScript&&) noexcept;
    SptScript& operator=(SptScript&&) noexcept;

    bool is_valid() const noexcept;
    std::size_t file_size() const noexcept;
    std::uint32_t header_string_count() const noexcept;
    const Bytes& raw() const noexcept;
    const Bytes& decrypted() const noexcept;
    const std::vector<SptString>& strings() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace exm::spt
