// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BLACKCyc (nnn/BCmkri) `.spt` script reader.
//
// SPT files are XOR-0xFF encrypted and hold CP932 text.  After decryption the
// header carries the magic "SPTHEADER0" at 0x30 and a string count at 0x18;
// the payload from 0x80 on is a run of NUL-terminated strings, 4-byte aligned.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/util.h"

namespace exm::spt {

// XOR every byte with 0xFF.
Bytes decrypt(const Bytes& data);

// True when the decrypted buffer carries the SPT header magic.
bool is_valid(const Bytes& dec);

// uint32 LE at 0x18 -- the header's own string count (informational).
std::uint32_t header_string_count(const Bytes& dec);

// Japanese test used by the extractor: kana, CJK, CJK-ext-A, fullwidth forms
// and CJK symbols.  Deliberately WIDER than the translator's
// has_real_japanese(), which excludes the fullwidth/symbol blocks.
bool has_japanese(const std::string& utf8);

struct SptString {
    std::uint32_t offset = 0;
    std::uint32_t byte_len = 0;
    std::string text;  // UTF-8
    bool has_jp = false;
    std::string type;  // narrative | dialogue | name | menu | label | empty | other
};

// Walk the NUL-terminated string run from `skip_offset`.
std::vector<SptString> extract_strings(const Bytes& dec, std::size_t skip_offset = 0x80);

// narrative / dialogue / name / menu / label / empty / other.
std::string classify(const std::string& text);

}  // namespace exm::spt
