// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// SiglusEngine Gameexe.dat codec, runtime key-table loader from SiglusEngine.exe,
// line-preserving GameexeDocument editor, and save-identity directive guard.
//
// Binary layout verified against retail SiglusEngine.exe and Gameexe.dat:
//   * 8-byte header: `[uint32_t header_version = 0][uint32_t use_private_key = 1]`.
//   * Payload at `+0x08`: XOR byte `i` with `ADIEU_PRIVATE_KEY[i & 0x0F]` (when
//     `use_private_key == 1`) and `GAMEEXE_KEY_256[i & 0xFF]` (256 bytes at
//     `SiglusEngine.exe` file offset `0x5F7D90`, verified by its 16-byte prefix
//     `D8 29 B9 16 3D 1A 76 D0 87 9B 2D 0C 7B D1 A9 19`), producing a framed
//     Siglus byte-LZSS stream (`[uint32_t arc_size][uint32_t unp_size]`) of
//     UTF-16LE configuration text.
//
// Thread-compatible.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/core/span.hpp>

namespace adi::g00 {

inline constexpr std::size_t kGameexeHeaderBytes = 8u;
inline constexpr std::size_t kGameexeKey256Size = 256u;
inline constexpr std::size_t kGameexeKeyPrefixBytes = 16u;
inline constexpr std::uint64_t kAdieuGameexeKeyFileOffset = 0x5F7D90u;
inline constexpr std::uint32_t kGameexeHeaderVersion = 0u;
inline constexpr std::uint32_t kGameexeUsePrivateKey = 1u;

inline constexpr std::array<std::uint8_t, kGameexeKeyPrefixBytes>
    kExpectedAdieuGameexeKeyPrefix = {{
        0xD8, 0x29, 0xB9, 0x16, 0x3D, 0x1A, 0x76, 0xD0,
        0x87, 0x9B, 0x2D, 0x0C, 0x7B, 0xD1, 0xA9, 0x19,
    }};

using GameexeKey256 = std::array<std::uint8_t, kGameexeKey256Size>;

// Reads 256 bytes at `kAdieuGameexeKeyFileOffset` (`0x5F7D90`) from
// `siglus_exe_path` and validates the first 16 bytes match
// `kExpectedAdieuGameexeKeyPrefix`. Throws std::runtime_error on I/O error,
// truncated executable, or prefix mismatch.
GameexeKey256 LoadGameexeKeyFromExe(const std::filesystem::path& siglus_exe_path);

// Decrypts and decompresses `raw_dat` into UTF-16LE code units (preserving any
// leading BOM, comments, blank lines, and line endings verbatim).
std::u16string DecodeGameexe(
    boost::span<const std::uint8_t> raw_dat,
    boost::span<const std::uint8_t, kGameexeKey256Size> key256);

// Compresses and encrypts UTF-16LE configuration text into a `Gameexe.dat`
// binary image (`header_version = 0`, `use_private_key = 1` by default).
std::vector<std::uint8_t> EncodeGameexe(
    std::u16string_view text_utf16,
    boost::span<const std::uint8_t, kGameexeKey256Size> key256,
    std::uint32_t use_private_key = kGameexeUsePrivateKey);

// Strict lossless UTF-16LE <-> UTF-8 conversions for Gameexe text.
std::string GameexeUtf16ToUtf8(std::u16string_view text_utf16,
                               bool strip_leading_bom = false);
std::u16string GameexeUtf8ToUtf16(std::string_view text_utf8,
                                  bool prepend_bom = false);

// Line-preserving view over a `Gameexe` configuration file. Untouched lines,
// comments, blank lines, leading BOM presence, and exact `\r\n` / `\n` endings
// round-trip byte-for-byte through `FromUtf16` -> `ToUtf16`.
class GameexeDocument {
public:
    GameexeDocument() = default;

    static GameexeDocument FromUtf16(std::u16string_view text_utf16);
    static GameexeDocument FromUtf8(std::string_view text_utf8);

    // Replaces the right-hand value of every `#<key> = <value>` line matching
    // `directive_key` (with or without leading `#`), preserving indentation,
    // `=` spacing, trailing `;` comments, and line endings. Returns the number
    // of matching lines updated.
    std::size_t SetDirectiveRawValue(std::string_view directive_key,
                                     std::string_view new_raw_value);

    // Quotes `unquoted_utf8` as `"..."` and updates matching `#<key>` lines.
    std::size_t SetDirectiveStringValue(std::string_view directive_key,
                                        std::string_view unquoted_utf8);

    bool has_leading_bom() const noexcept { return has_leading_bom_; }
    std::size_t line_count() const noexcept { return lines_.size(); }
    std::size_t directive_count() const noexcept;

    // Returns the trimmed raw value of the first matching `#<key> = <value>`
    // directive (excluding trailing comments), or `std::nullopt` if absent.
    std::optional<std::string> FindDirectiveRawValue(
        std::string_view directive_key) const;

    std::vector<std::string> FindAllDirectiveRawValues(
        std::string_view directive_key) const;

    std::u16string ToUtf16() const;
    std::string ToUtf8(bool include_bom = false) const;

private:
    struct LineRecord {
        std::string content_utf8;
        std::string ending_utf8;
    };

    bool has_leading_bom_ = false;
    std::vector<LineRecord> lines_;
};

// Asserts that `#DISCMARK`, `#EXENAME`, `#REG_NAME`, `#SAVEDIR`, and all
// non-string save-file structure directives are 100% identical between
// `orig_utf8` and `patched_utf8`. Throws std::runtime_error on any mismatch.
void VerifyCriticalDirectivesUnchanged(std::string_view orig_utf8,
                                       std::string_view patched_utf8);

}  // namespace adi::g00
