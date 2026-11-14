// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Lossless SiglusEngine .g00 sprite decoder, encoder, and metadata parity
// verifier supporting Type 0 (24-bpp BGR pixel-LZSS -> 32-bpp BGRA stride),
// Type 1 (8-bpp paletted BGRA byte-LZSS), and Type 2 (multi-cut, multi-part
// 32-bpp straight-BGRA byte-LZSS).
//
// Binary layout invariants verified against retail SiglusEngine.exe and Adieu:
//   * 5-byte file prefix: `[uint8_t type][uint16_t width][uint16_t height]`.
//   * Type 0: `[uint32_t arc_size][uint32_t unp_size]` at `+0x05` where
//     `unp_size == width * height * 4` (4-byte BGRA output stride). Literal
//     flag bit `1` reads 3 BGR bytes and writes `(B, G, R, 0xFF)`; back-ref
//     flag bit `0` reads `uint16_t token` (`offset_pixels = token >> 4` in
//     `1..4095`, `count_pixels = (token & 0x0F) + 1` in `1..16`) and copies
//     `count_pixels * 4` bytes from `op - offset_pixels * 4`.
//   * Type 1: `[uint32_t arc_size][uint32_t unp_size]` byte-LZSS (`min_match=2,
//     max_match=17`) decompressing to `[uint16_t palette_count]` +
//     `palette_count * 4` BGRA entries + `width * height` uint8_t indices.
//   * Type 2: `uint32_t cut_count` at `+0x05`, `cut_count` 24-byte
//     `G00CutDescriptor` records at `+0x09` with INCLUSIVE `(x2, y2)`, then
//     `[uint32_t arc_size][uint32_t unp_size]` byte-LZSS. Decompressed stream
//     starts with `uint32_t index_count` (`== cut_count`) at `+0x00`, followed
//     by `index_count` `{uint32_t cut_offset, uint32_t cut_size}` pairs at
//     `+0x04`. Each non-empty cut begins with a 116-byte `G00CutHeader` and
//     `part_count` 92-byte `G00PartHeader` + `part_w * part_h * 4` BGRA slabs,
//     where `bbox_x/y` and `part_x/y` are cut-local (relative to `x1, y1`).
//
// Thread-compatible.
#pragma once

#include "imaging/bgra_image.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <boost/core/span.hpp>

namespace adi::g00 {

inline constexpr std::size_t kG00FileHeaderBytes = 5u;
inline constexpr std::size_t kLzssFrameHeaderBytes = 8u;
inline constexpr std::size_t kG00CutDescriptorBytes = 24u;
inline constexpr std::size_t kG00CutHeaderBytes = 116u;
inline constexpr std::size_t kG00CutHeaderReservedBytes = 80u;
inline constexpr std::size_t kG00PartHeaderBytes = 92u;
inline constexpr std::size_t kG00PartHeaderReservedBytes = 82u;
inline constexpr std::size_t kLzssWindowUnits = 4095u;
inline constexpr std::size_t kByteLzssMinMatch = 2u;
inline constexpr std::size_t kByteLzssMaxMatch = 17u;
inline constexpr std::size_t kPixelLzssMinMatch = 1u;
inline constexpr std::size_t kPixelLzssMaxMatch = 16u;

enum class G00Type : std::uint8_t {
    kType0Bgr24 = 0,
    kType1Paletted8 = 1,
    kType2MultiCutBgra32 = 2,
};

// Outer 24-byte Type 2 cut descriptor. `x2` and `y2` are INCLUSIVE canvas
// coordinates (`x2 = x1 + cut_w - 1`, `y2 = y1 + cut_h - 1` for non-empty cuts).
struct G00CutDescriptor {
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
};

// Decompressed 116-byte (`0x74`) Type 2 cut header. `bbox_x` and `bbox_y` are
// cut-local coordinates relative to `(descriptor.x1, descriptor.y1)`.
struct G00CutHeader {
    std::uint16_t cut_type = 1u;
    std::uint16_t part_count = 0u;
    std::int32_t bbox_x = 0;
    std::int32_t bbox_y = 0;
    std::int32_t bbox_w = 0;
    std::int32_t bbox_h = 0;
    std::int32_t center_x = 0;
    std::int32_t center_y = 0;
    std::int32_t cut_canvas_w = 0;
    std::int32_t cut_canvas_h = 0;
    std::array<std::uint8_t, kG00CutHeaderReservedBytes> reserved{};
};

// Decompressed 92-byte (`0x5C`) Type 2 part header. `part_x` and `part_y` are
// cut-local coordinates relative to `(descriptor.x1, descriptor.y1)`.
struct G00PartHeader {
    std::uint16_t part_x = 0u;
    std::uint16_t part_y = 0u;
    std::uint16_t trans_flag = 0u;
    std::uint16_t part_w = 0u;
    std::uint16_t part_h = 0u;
    std::array<std::uint8_t, kG00PartHeaderReservedBytes> reserved{};
};

struct G00Cut {
    G00CutDescriptor descriptor{};
    std::array<std::uint8_t, kG00CutDescriptorBytes> raw_descriptor_bytes{};
    bool has_raw_descriptor = false;
    bool is_empty = true;
    std::uint32_t empty_index_offset = 0u;
    G00CutHeader cut_header{};
    std::vector<G00PartHeader> part_headers;
    std::vector<std::uint8_t> raw_cut_bytes;
    imaging::BgraImage canvas;
    bool modified = false;
};

struct G00Image {
    G00Type type = G00Type::kType0Bgr24;
    std::uint16_t width = 0u;
    std::uint16_t height = 0u;

    // Populated for Type 0 and Type 1 sprites.
    imaging::BgraImage canvas;
    bool canvas_modified = false;

    // Retained from Type 1 decode so untouched paletted sprites re-emit their
    // exact original palette and index stream.
    std::vector<imaging::BgraColor> type1_palette;
    std::vector<std::uint8_t> type1_indices;

    // Populated for Type 2 multi-cut sprites (`cuts.size() == cut_count`).
    std::vector<G00Cut> cuts;

    // Marks Type 0/1 `canvas` as modified and returns a mutable reference.
    imaging::BgraImage& MutableCanvas();

    // Marks Type 2 `cuts.at(cut_index)` as modified and returns its full
    // `(width, height)` canvas for editing.
    imaging::BgraImage& MutableCutCanvas(std::size_t cut_index);
};

// Framed Siglus byte-unit LZSS (`[uint32_t arc_size][uint32_t unp_size]` +
// LSB-first bitstream, `min_match = 2, max_match = 17, window = 4095`).
std::vector<std::uint8_t> CompressByteLzss(boost::span<const std::uint8_t> plain);
std::vector<std::uint8_t> DecompressByteLzss(boost::span<const std::uint8_t> framed_arc);

// Framed Siglus 24-bpp pixel-unit LZSS (`[uint32_t arc_size][uint32_t unp_size]`
// where `unp_size == width * height * 4` in 4-byte BGRA stride).
std::vector<std::uint8_t> CompressPixelLzss24(
    boost::span<const std::uint8_t> bgra_pixels, std::uint16_t width,
    std::uint16_t height);
std::vector<std::uint8_t> DecompressPixelLzss24(
    boost::span<const std::uint8_t> framed_arc, std::uint16_t width,
    std::uint16_t height);

class G00Codec {
public:
    static G00Image Decode(boost::span<const std::uint8_t> raw_g00);
    static std::vector<std::uint8_t> Encode(const G00Image& image);

    // Verifies `type`, `width`, `height`, `cut_count`, `index_count == cut_count`,
    // every cut's `origin_x/y` (24B descriptor) and `center_x/y + cut_type +
    // reserved[80]` (116B cut header), and pixel-exact equality of all cuts not
    // listed in `modified_cuts`. Populates `*error_out` when returning false.
    static bool VerifyMetadataParity(
        boost::span<const std::uint8_t> orig_g00,
        boost::span<const std::uint8_t> patched_g00,
        const std::set<std::size_t>& modified_cuts = {},
        std::string* error_out = nullptr);

    // Throws std::runtime_error if VerifyMetadataParity returns false.
    static void AssertMetadataParity(
        boost::span<const std::uint8_t> orig_g00,
        boost::span<const std::uint8_t> patched_g00,
        const std::set<std::size_t>& modified_cuts = {});
};

}  // namespace adi::g00
