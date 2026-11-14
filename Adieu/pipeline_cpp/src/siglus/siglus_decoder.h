// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// SiglusEngine Scene.pck scene decoder for Adieu.
//
// Algorithm verified against SiglusEngine.exe (Scene::get_str at RVA 0x14C8E0):
//   1. XOR the chunk with the per-game 16-byte private key (decrypt_1), then
//      with the universal 256-byte SiglusEngine key (decrypt_2).
//   2. The first 8 bytes are {comp_size, decomp_size}.
//   3. The body is a SiglusEngine LZSS variant.
//   4. The plaintext is a SCENEHEADERV2 whose string index/blob pairs describe
//      a UTF-16LE string table where every entry `j` is unconditionally XORed
//      with `(j * 0x7087) & 0xFFFF`.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/util.h"

namespace adi::siglus {

inline constexpr std::size_t kSiglusKey256Size = 256;
inline constexpr std::size_t kPrivateKeySize = 16;
inline constexpr std::size_t kPartialKeySize = 8;
inline constexpr std::uint16_t kSsV2Multiplier = 0x7087u;

// Universal SiglusEngine XOR key (decrypt_2), indexed by `i & 0xFF`.
extern const std::array<std::uint8_t, kSiglusKey256Size> SIGLUS_KEY_256;

// Per-game key (decrypt_1) for Adieu. All 194 scene chunks decode with it.
extern const std::array<std::uint8_t, kPrivateKeySize> ADIEU_PRIVATE_KEY;

// Cross-check prefix retained for key-recovery verification.
extern const std::array<std::uint8_t, kPartialKeySize> ADIEU_PRIVATE_KEY_PARTIAL;

Bytes decrypt_universal(const Bytes& buf);

// SiglusEngine byte-LZSS decompressor. Truncates to whatever was produced when
// the input runs out rather than zero-padding past truncated streams.
Bytes decompress_lzss(const std::uint8_t* comp, std::size_t comp_len,
                      std::uint32_t decomp_size);

// Decrypt + decompress one scene blob. `private_key` may be null (skips
// decrypt_1); decode_scene always passes ADIEU_PRIVATE_KEY.
// Throws std::runtime_error on a truncated header or implausible decomp_size.
Bytes decode_scene_chunk(const Bytes& scene_bytes, const std::uint8_t* private_key);

// Walk the SCENEHEADERV2 sub-header and decode its UTF-16 string table using
// unconditional per-entry XOR `(j * kSsV2Multiplier) & 0xFFFF`.
// Returns an empty list for any malformed header; throws LoneSurrogateError if
// a decoded UTF-16 unit falls in 0xD800..0xDFFF.
std::vector<std::string> parse_scene_strings(const Bytes& decompressed);

// Extract entry point for a single scene chunk. Swallows chunk-decryption or
// decompression exceptions (returning an empty vector), while propagating
// LoneSurrogateError from a malformed string pool.
std::vector<std::string> decode_scene(const Bytes& scene_bytes);

// Returns SIGLUS_KEY_256 for key-table diagnostic logging.
const std::array<std::uint8_t, kSiglusKey256Size>& warm_global_key();

}  // namespace adi::siglus
