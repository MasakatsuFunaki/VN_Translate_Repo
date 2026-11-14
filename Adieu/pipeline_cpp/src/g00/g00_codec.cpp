// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "g00/g00_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adi::g00 {
namespace {

using imaging::BgraColor;
using imaging::BgraImage;
using imaging::ClampRect;
using imaging::RectI;
using imaging::UnionRect;

constexpr std::uint8_t kByteMask = 0xFFu;
constexpr unsigned kShift8 = 8u;
constexpr unsigned kShift16 = 16u;
constexpr unsigned kShift24 = 24u;

constexpr std::size_t kHeaderWidthOffset = 1u;
constexpr std::size_t kHeaderHeightOffset = 3u;
constexpr std::size_t kLzssUnpSizeOffset = 4u;

constexpr int kBitsPerFlagByte = 8;
constexpr unsigned kLzssOffsetShift = 4u;
constexpr std::uint16_t kLzssCountMask = 0x0Fu;

constexpr std::size_t kHashBucketCount = 65536u;
constexpr std::size_t kHashBucketMask = 0xFFFFu;
constexpr int kHashEmptyPos = -1;
constexpr int kMaxHashChainSteps = 128;
constexpr std::uint32_t kPixHashMulB = 257u;
constexpr std::uint32_t kPixHashMulG = 65537u;
constexpr std::uint32_t kPixHashMulR = 17u;

constexpr std::size_t kMaxPlausibleUnpackedBytes = 256u * 1024u * 1024u;
constexpr std::uint32_t kMaxPlausibleCutCount = 65536u;

constexpr std::size_t kType1PaletteCountHeaderBytes = 2u;
constexpr std::size_t kMaxType1PaletteColors = 256u;

constexpr std::size_t kType2CutCountOffset = 5u;
constexpr std::size_t kType2DescriptorArrayOffset = 9u;
constexpr std::size_t kType2IndexHeaderBytes = 4u;
constexpr std::size_t kType2IndexEntryStride = 8u;
constexpr std::size_t kType2IndexSizeFieldOffset = 4u;

constexpr std::size_t kDescX1Offset = 0u;
constexpr std::size_t kDescY1Offset = 4u;
constexpr std::size_t kDescX2Offset = 8u;
constexpr std::size_t kDescY2Offset = 12u;
constexpr std::size_t kDescOriginXOffset = 16u;
constexpr std::size_t kDescOriginYOffset = 20u;

constexpr std::size_t kCutHdrTypeOffset = 0u;
constexpr std::size_t kCutHdrPartCountOffset = 2u;
constexpr std::size_t kCutHdrBboxXOffset = 4u;
constexpr std::size_t kCutHdrBboxYOffset = 8u;
constexpr std::size_t kCutHdrBboxWOffset = 12u;
constexpr std::size_t kCutHdrBboxHOffset = 16u;
constexpr std::size_t kCutHdrCenterXOffset = 20u;
constexpr std::size_t kCutHdrCenterYOffset = 24u;
constexpr std::size_t kCutHdrCanvasWOffset = 28u;
constexpr std::size_t kCutHdrCanvasHOffset = 32u;
constexpr std::size_t kCutHdrReservedOffset = 36u;

constexpr std::size_t kPartHdrXOffset = 0u;
constexpr std::size_t kPartHdrYOffset = 2u;
constexpr std::size_t kPartHdrTransFlagOffset = 4u;
constexpr std::size_t kPartHdrWOffset = 6u;
constexpr std::size_t kPartHdrHOffset = 8u;
constexpr std::size_t kPartHdrReservedOffset = 10u;

constexpr std::uint16_t kTransFlagOpaque = 0u;
constexpr std::uint16_t kTransFlagTranslucent = 1u;
constexpr std::uint16_t kDefaultCutType = 1u;
constexpr std::uint8_t kMinVisibleAlpha = 1u;

std::uint16_t ReadU16Le(boost::span<const std::uint8_t> buf, std::size_t off) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(buf[off]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(buf[off + 1u]) << kShift8));
}

std::uint32_t ReadU32Le(boost::span<const std::uint8_t> buf, std::size_t off) {
    return static_cast<std::uint32_t>(buf[off]) |
           (static_cast<std::uint32_t>(buf[off + 1u]) << kShift8) |
           (static_cast<std::uint32_t>(buf[off + 2u]) << kShift16) |
           (static_cast<std::uint32_t>(buf[off + 3u]) << kShift24);
}

std::int32_t ReadI32Le(boost::span<const std::uint8_t> buf, std::size_t off) {
    return static_cast<std::int32_t>(ReadU32Le(buf, off));
}

void AppendU16Le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kByteMask));
}

void AppendU32Le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift16) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift24) & kByteMask));
}

void AppendI32Le(std::vector<std::uint8_t>& out, std::int32_t v) {
    AppendU32Le(out, static_cast<std::uint32_t>(v));
}

void WriteU32LeAt(std::vector<std::uint8_t>& out, std::size_t off, std::uint32_t v) {
    out[off] = static_cast<std::uint8_t>(v & kByteMask);
    out[off + 1u] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    out[off + 2u] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    out[off + 3u] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

std::size_t HashBytePair(boost::span<const std::uint8_t> plain, std::size_t pos) noexcept {
    return (static_cast<std::size_t>(plain[pos]) |
            (static_cast<std::size_t>(plain[pos + 1u]) << kShift8)) &
           kHashBucketMask;
}

std::size_t HashPixelBgr(const std::uint8_t* bgra, std::size_t pixel_idx) noexcept {
    const std::size_t off =
        pixel_idx * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
    const std::uint32_t h =
        static_cast<std::uint32_t>(bgra[off + static_cast<std::size_t>(imaging::kChannelBlue)]) *
            kPixHashMulB ^
        static_cast<std::uint32_t>(bgra[off + static_cast<std::size_t>(imaging::kChannelGreen)]) *
            kPixHashMulG ^
        static_cast<std::uint32_t>(bgra[off + static_cast<std::size_t>(imaging::kChannelRed)]) *
            kPixHashMulR;
    return static_cast<std::size_t>(h) & kHashBucketMask;
}

bool SamePixelBgr(const std::uint8_t* bgra, std::size_t pix_a, std::size_t pix_b) noexcept {
    const std::size_t off_a = pix_a * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
    const std::size_t off_b = pix_b * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
    return bgra[off_a + 0u] == bgra[off_b + 0u] &&
           bgra[off_a + 1u] == bgra[off_b + 1u] &&
           bgra[off_a + 2u] == bgra[off_b + 2u];
}

std::uint32_t PackBgraKey(BgraColor c) noexcept {
    return static_cast<std::uint32_t>(c.b) |
           (static_cast<std::uint32_t>(c.g) << kShift8) |
           (static_cast<std::uint32_t>(c.r) << kShift16) |
           (static_cast<std::uint32_t>(c.a) << kShift24);
}

struct WeightedColorEntry {
    BgraColor color{};
    std::size_t count = 0u;
};

struct ColorBox {
    std::size_t begin = 0u;
    std::size_t end = 0u;
};

std::uint8_t ChannelValue(BgraColor c, int channel) noexcept {
    if (channel == imaging::kChannelBlue) {
        return c.b;
    }
    if (channel == imaging::kChannelGreen) {
        return c.g;
    }
    if (channel == imaging::kChannelRed) {
        return c.r;
    }
    return c.a;
}

int WidestChannelRange(const std::vector<WeightedColorEntry>& entries,
                       const ColorBox& box, int* best_channel_out) noexcept {
    int best_range = -1;
    int best_channel = imaging::kChannelBlue;
    for (int ch = 0; ch < imaging::kBgraBytesPerPixel; ++ch) {
        std::uint8_t lo = std::numeric_limits<std::uint8_t>::max();
        std::uint8_t hi = 0u;
        for (std::size_t i = box.begin; i < box.end; ++i) {
            const std::uint8_t v = ChannelValue(entries[i].color, ch);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const int range = static_cast<int>(hi) - static_cast<int>(lo);
        if (range > best_range) {
            best_range = range;
            best_channel = ch;
        }
    }
    *best_channel_out = best_channel;
    return best_range;
}

// Builds a <=256-entry BGRA palette + 8-bit index map for a modified Type 1
// canvas. Uses exact lossless mapping whenever unique colors <= 256, and falls
// back to 4D BGRA median-cut quantization only when unique colors exceed 256.
void BuildType1PaletteAndIndices(const BgraImage& canvas,
                                 std::vector<BgraColor>& palette_out,
                                 std::vector<std::uint8_t>& indices_out) {
    const std::size_t total_pixels =
        static_cast<std::size_t>(canvas.width()) * static_cast<std::size_t>(canvas.height());
    const std::uint8_t* raw = canvas.data();

    std::vector<WeightedColorEntry> unique_entries;
    std::unordered_map<std::uint32_t, std::size_t> key_to_entry;
    unique_entries.reserve(std::min(total_pixels, kMaxType1PaletteColors * 2u));
    key_to_entry.reserve(std::min(total_pixels, kMaxType1PaletteColors * 2u));

    // Fast exact path for <= 256 unique colors preserving scanline discovery order.
    indices_out.assign(total_pixels, 0u);
    bool exceeded_256 = false;
    for (std::size_t p = 0; p < total_pixels; ++p) {
        const std::size_t off = p * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
        const BgraColor c{raw[off + 0u], raw[off + 1u], raw[off + 2u], raw[off + 3u]};
        const std::uint32_t key = PackBgraKey(c);

        const auto [it, inserted] = key_to_entry.emplace(key, unique_entries.size());
        const std::size_t entry_idx = it->second;
        if (inserted) {
            unique_entries.push_back(WeightedColorEntry{c, 1u});
            if (unique_entries.size() > kMaxType1PaletteColors) {
                exceeded_256 = true;
            }
        } else {
            ++unique_entries[entry_idx].count;
        }
        if (!exceeded_256) {
            indices_out[p] = static_cast<std::uint8_t>(entry_idx);
        }
    }

    if (!exceeded_256) {
        palette_out.clear();
        palette_out.reserve(std::max<std::size_t>(1u, unique_entries.size()));
        for (const WeightedColorEntry& entry : unique_entries) {
            palette_out.push_back(entry.color);
        }
        if (palette_out.empty()) {
            palette_out.push_back(BgraColor{0u, 0u, 0u, 0u});
        }
        return;
    }

    // Median-cut quantization in 4D (B, G, R, A) space to 256 boxes.
    std::vector<ColorBox> boxes;
    boxes.reserve(kMaxType1PaletteColors);
    boxes.push_back(ColorBox{0u, unique_entries.size()});

    while (boxes.size() < kMaxType1PaletteColors) {
        std::size_t split_idx = boxes.size();
        int best_range = 0;
        int split_channel = imaging::kChannelBlue;

        for (std::size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].end - boxes[b].begin < 2u) {
                continue;
            }
            int ch = imaging::kChannelBlue;
            const int range = WidestChannelRange(unique_entries, boxes[b], &ch);
            if (range > best_range) {
                best_range = range;
                split_idx = b;
                split_channel = ch;
            }
        }
        if (split_idx == boxes.size()) {
            break;
        }

        ColorBox target = boxes[split_idx];
        std::sort(unique_entries.begin() + static_cast<std::ptrdiff_t>(target.begin),
                  unique_entries.begin() + static_cast<std::ptrdiff_t>(target.end),
                  [split_channel](const WeightedColorEntry& lhs,
                                  const WeightedColorEntry& rhs) {
                      return ChannelValue(lhs.color, split_channel) <
                             ChannelValue(rhs.color, split_channel);
                  });

        std::size_t total_weight = 0u;
        for (std::size_t i = target.begin; i < target.end; ++i) {
            total_weight += unique_entries[i].count;
        }
        const std::size_t half_weight = total_weight / 2u;
        std::size_t accum = 0u;
        std::size_t mid = target.begin + 1u;
        for (std::size_t i = target.begin; i + 1u < target.end; ++i) {
            accum += unique_entries[i].count;
            if (accum >= half_weight) {
                mid = i + 1u;
                break;
            }
        }

        boxes[split_idx] = ColorBox{target.begin, mid};
        boxes.push_back(ColorBox{mid, target.end});
    }

    palette_out.clear();
    palette_out.reserve(boxes.size());
    for (const ColorBox& box : boxes) {
        std::uint64_t sum_b = 0u;
        std::uint64_t sum_g = 0u;
        std::uint64_t sum_r = 0u;
        std::uint64_t sum_a = 0u;
        std::uint64_t weight = 0u;
        for (std::size_t i = box.begin; i < box.end; ++i) {
            const std::uint64_t w = unique_entries[i].count;
            sum_b += static_cast<std::uint64_t>(unique_entries[i].color.b) * w;
            sum_g += static_cast<std::uint64_t>(unique_entries[i].color.g) * w;
            sum_r += static_cast<std::uint64_t>(unique_entries[i].color.r) * w;
            sum_a += static_cast<std::uint64_t>(unique_entries[i].color.a) * w;
            weight += w;
        }
        const std::uint64_t half = weight / 2u;
        palette_out.push_back(BgraColor{
            static_cast<std::uint8_t>((sum_b + half) / weight),
            static_cast<std::uint8_t>((sum_g + half) / weight),
            static_cast<std::uint8_t>((sum_r + half) / weight),
            static_cast<std::uint8_t>((sum_a + half) / weight),
        });
    }

    for (std::size_t p = 0; p < total_pixels; ++p) {
        const std::size_t off = p * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
        const int pb = static_cast<int>(raw[off + 0u]);
        const int pg = static_cast<int>(raw[off + 1u]);
        const int pr = static_cast<int>(raw[off + 2u]);
        const int pa = static_cast<int>(raw[off + 3u]);

        std::uint32_t best_dist = std::numeric_limits<std::uint32_t>::max();
        std::uint8_t best_idx = 0u;
        for (std::size_t k = 0; k < palette_out.size(); ++k) {
            const int db = pb - static_cast<int>(palette_out[k].b);
            const int dg = pg - static_cast<int>(palette_out[k].g);
            const int dr = pr - static_cast<int>(palette_out[k].r);
            const int da = pa - static_cast<int>(palette_out[k].a);
            const std::uint32_t dist =
                static_cast<std::uint32_t>(db * db + dg * dg + dr * dr + da * da);
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = static_cast<std::uint8_t>(k);
                if (dist == 0u) {
                    break;
                }
            }
        }
        indices_out[p] = best_idx;
    }
}

G00CutDescriptor ParseCutDescriptor(boost::span<const std::uint8_t> bytes) {
    G00CutDescriptor d{};
    d.x1 = ReadI32Le(bytes, kDescX1Offset);
    d.y1 = ReadI32Le(bytes, kDescY1Offset);
    d.x2 = ReadI32Le(bytes, kDescX2Offset);
    d.y2 = ReadI32Le(bytes, kDescY2Offset);
    d.origin_x = ReadI32Le(bytes, kDescOriginXOffset);
    d.origin_y = ReadI32Le(bytes, kDescOriginYOffset);
    return d;
}

std::array<std::uint8_t, kG00CutDescriptorBytes> SerializeCutDescriptor(
    const G00CutDescriptor& d) {
    std::vector<std::uint8_t> tmp;
    tmp.reserve(kG00CutDescriptorBytes);
    AppendI32Le(tmp, d.x1);
    AppendI32Le(tmp, d.y1);
    AppendI32Le(tmp, d.x2);
    AppendI32Le(tmp, d.y2);
    AppendI32Le(tmp, d.origin_x);
    AppendI32Le(tmp, d.origin_y);
    std::array<std::uint8_t, kG00CutDescriptorBytes> out{};
    std::copy(tmp.begin(), tmp.end(), out.begin());
    return out;
}

G00CutHeader ParseCutHeader(boost::span<const std::uint8_t> bytes) {
    G00CutHeader hdr{};
    hdr.cut_type = ReadU16Le(bytes, kCutHdrTypeOffset);
    hdr.part_count = ReadU16Le(bytes, kCutHdrPartCountOffset);
    hdr.bbox_x = ReadI32Le(bytes, kCutHdrBboxXOffset);
    hdr.bbox_y = ReadI32Le(bytes, kCutHdrBboxYOffset);
    hdr.bbox_w = ReadI32Le(bytes, kCutHdrBboxWOffset);
    hdr.bbox_h = ReadI32Le(bytes, kCutHdrBboxHOffset);
    hdr.center_x = ReadI32Le(bytes, kCutHdrCenterXOffset);
    hdr.center_y = ReadI32Le(bytes, kCutHdrCenterYOffset);
    hdr.cut_canvas_w = ReadI32Le(bytes, kCutHdrCanvasWOffset);
    hdr.cut_canvas_h = ReadI32Le(bytes, kCutHdrCanvasHOffset);
    std::copy_n(bytes.data() + kCutHdrReservedOffset, kG00CutHeaderReservedBytes,
                hdr.reserved.begin());
    return hdr;
}

std::vector<std::uint8_t> SerializeCutHeader(const G00CutHeader& hdr) {
    std::vector<std::uint8_t> out;
    out.reserve(kG00CutHeaderBytes);
    AppendU16Le(out, hdr.cut_type);
    AppendU16Le(out, hdr.part_count);
    AppendI32Le(out, hdr.bbox_x);
    AppendI32Le(out, hdr.bbox_y);
    AppendI32Le(out, hdr.bbox_w);
    AppendI32Le(out, hdr.bbox_h);
    AppendI32Le(out, hdr.center_x);
    AppendI32Le(out, hdr.center_y);
    AppendI32Le(out, hdr.cut_canvas_w);
    AppendI32Le(out, hdr.cut_canvas_h);
    out.insert(out.end(), hdr.reserved.begin(), hdr.reserved.end());
    return out;
}

G00PartHeader ParsePartHeader(boost::span<const std::uint8_t> bytes) {
    G00PartHeader part{};
    part.part_x = ReadU16Le(bytes, kPartHdrXOffset);
    part.part_y = ReadU16Le(bytes, kPartHdrYOffset);
    part.trans_flag = ReadU16Le(bytes, kPartHdrTransFlagOffset);
    part.part_w = ReadU16Le(bytes, kPartHdrWOffset);
    part.part_h = ReadU16Le(bytes, kPartHdrHOffset);
    std::copy_n(bytes.data() + kPartHdrReservedOffset, kG00PartHeaderReservedBytes,
                part.reserved.begin());
    return part;
}

void AppendPartHeader(std::vector<std::uint8_t>& out, const G00PartHeader& part) {
    AppendU16Le(out, part.part_x);
    AppendU16Le(out, part.part_y);
    AppendU16Le(out, part.trans_flag);
    AppendU16Le(out, part.part_w);
    AppendU16Le(out, part.part_h);
    out.insert(out.end(), part.reserved.begin(), part.reserved.end());
}

bool SlabHasAnyNonOpaquePixel(const BgraImage& slab) noexcept {
    const std::size_t total_pixels =
        static_cast<std::size_t>(slab.width()) * static_cast<std::size_t>(slab.height());
    const std::uint8_t* raw = slab.data();
    for (std::size_t p = 0; p < total_pixels; ++p) {
        if (raw[p * static_cast<std::size_t>(imaging::kBgraBytesPerPixel) +
                static_cast<std::size_t>(imaging::kChannelAlpha)] != imaging::kOpaqueAlpha) {
            return true;
        }
    }
    return false;
}

bool FailParity(std::string* error_out, std::string msg) {
    if (error_out != nullptr) {
        *error_out = std::move(msg);
    }
    return false;
}

}  // namespace

BgraImage& G00Image::MutableCanvas() {
    canvas_modified = true;
    return canvas;
}

BgraImage& G00Image::MutableCutCanvas(std::size_t cut_index) {
    G00Cut& cut = cuts.at(cut_index);
    if (cut.canvas.empty() && width > 0u && height > 0u) {
        cut.canvas = BgraImage(static_cast<int>(width), static_cast<int>(height));
    }
    cut.modified = true;
    return cut.canvas;
}

std::vector<std::uint8_t> CompressByteLzss(boost::span<const std::uint8_t> plain) {
    if (plain.size() > kMaxPlausibleUnpackedBytes) {
        throw std::runtime_error("CompressByteLzss input exceeds maximum supported size");
    }

    std::vector<std::uint8_t> out(kLzssFrameHeaderBytes, 0u);
    if (plain.empty()) {
        WriteU32LeAt(out, 0u, static_cast<std::uint32_t>(kLzssFrameHeaderBytes));
        WriteU32LeAt(out, kLzssUnpSizeOffset, 0u);
        return out;
    }

    out.reserve(kLzssFrameHeaderBytes + plain.size() + (plain.size() / 8u) + 8u);
    std::vector<int> head(kHashBucketCount, kHashEmptyPos);
    std::vector<int> prev(plain.size(), kHashEmptyPos);

    auto insert_pos = [&](std::size_t p) {
        if (p + 1u >= plain.size()) {
            return;
        }
        const std::size_t h = HashBytePair(plain, p);
        prev[p] = head[h];
        head[h] = static_cast<int>(p);
    };

    std::size_t flag_pos = 0u;
    std::uint8_t flag_byte = 0u;
    int bit_index = 0;

    std::size_t pos = 0u;
    while (pos < plain.size()) {
        if (bit_index == 0) {
            flag_pos = out.size();
            flag_byte = 0u;
            out.push_back(0u);
        }

        std::size_t best_len = 0u;
        std::size_t best_off = 0u;
        const std::size_t max_len = std::min(kByteLzssMaxMatch, plain.size() - pos);

        if (max_len >= kByteLzssMinMatch && pos > 0u) {
            const std::size_t min_cand =
                pos > kLzssWindowUnits ? (pos - kLzssWindowUnits) : 0u;
            const std::size_t h = HashBytePair(plain, pos);
            int cand = head[h];
            int steps = 0;

            while (cand >= static_cast<int>(min_cand) && steps < kMaxHashChainSteps) {
                ++steps;
                const std::size_t cpos = static_cast<std::size_t>(cand);
                if (plain[cpos + best_len] == plain[pos + best_len]) {
                    std::size_t match_len = 0u;
                    while (match_len < max_len &&
                           plain[cpos + match_len] == plain[pos + match_len]) {
                        ++match_len;
                    }
                    if (match_len >= kByteLzssMinMatch && match_len > best_len) {
                        best_len = match_len;
                        best_off = pos - cpos;
                        if (best_len == max_len) {
                            break;
                        }
                    }
                }
                cand = prev[cpos];
            }
        }

        if (best_len >= kByteLzssMinMatch) {
            const std::uint16_t token = static_cast<std::uint16_t>(
                (best_off << kLzssOffsetShift) | (best_len - kByteLzssMinMatch));
            AppendU16Le(out, token);
            for (std::size_t s = 0u; s < best_len; ++s) {
                insert_pos(pos + s);
            }
            pos += best_len;
        } else {
            flag_byte = static_cast<std::uint8_t>(flag_byte | (1u << bit_index));
            out[flag_pos] = flag_byte;
            out.push_back(plain[pos]);
            insert_pos(pos);
            ++pos;
        }

        bit_index = (bit_index + 1) % kBitsPerFlagByte;
    }

    WriteU32LeAt(out, 0u, static_cast<std::uint32_t>(out.size()));
    WriteU32LeAt(out, kLzssUnpSizeOffset, static_cast<std::uint32_t>(plain.size()));
    return out;
}

std::vector<std::uint8_t> DecompressByteLzss(boost::span<const std::uint8_t> framed_arc) {
    if (framed_arc.size() < kLzssFrameHeaderBytes) {
        throw std::runtime_error("DecompressByteLzss buffer shorter than 8-byte frame header");
    }

    const std::uint32_t arc_size = ReadU32Le(framed_arc, 0u);
    const std::uint32_t unp_size = ReadU32Le(framed_arc, kLzssUnpSizeOffset);

    if (arc_size < kLzssFrameHeaderBytes ||
        static_cast<std::size_t>(arc_size) > framed_arc.size()) {
        throw std::runtime_error("DecompressByteLzss invalid or truncated arc_size");
    }
    if (static_cast<std::size_t>(unp_size) > kMaxPlausibleUnpackedBytes) {
        throw std::runtime_error("DecompressByteLzss implausible unp_size");
    }
    if (unp_size == 0u) {
        return {};
    }

    const std::size_t arc_limit = static_cast<std::size_t>(arc_size);
    const std::size_t out_limit = static_cast<std::size_t>(unp_size);
    std::vector<std::uint8_t> out(out_limit, 0u);

    std::size_t si = kLzssFrameHeaderBytes;
    std::size_t di = 0u;

    while (di < out_limit) {
        if (si >= arc_limit) {
            throw std::runtime_error("DecompressByteLzss truncated before reaching unp_size");
        }
        const std::uint8_t flag = framed_arc[si++];
        for (int bit = 0; bit < kBitsPerFlagByte && di < out_limit; ++bit) {
            if ((flag & (1u << bit)) != 0u) {
                if (si >= arc_limit) {
                    throw std::runtime_error("DecompressByteLzss truncated literal byte");
                }
                out[di++] = framed_arc[si++];
                continue;
            }

            if (si + 1u >= arc_limit) {
                throw std::runtime_error("DecompressByteLzss truncated back-reference token");
            }
            const std::uint16_t token = ReadU16Le(framed_arc, si);
            si += 2u;

            const std::size_t offset = static_cast<std::size_t>(token >> kLzssOffsetShift);
            const std::size_t count =
                static_cast<std::size_t>(token & kLzssCountMask) + kByteLzssMinMatch;
            if (offset == 0u || offset > di) {
                throw std::runtime_error("DecompressByteLzss invalid back-reference offset");
            }

            const std::size_t copy_len = std::min(count, out_limit - di);
            for (std::size_t k = 0u; k < copy_len; ++k) {
                out[di] = out[di - offset];
                ++di;
            }
        }
    }

    return out;
}

std::vector<std::uint8_t> CompressPixelLzss24(
    boost::span<const std::uint8_t> bgra_pixels, std::uint16_t width,
    std::uint16_t height) {
    if (width == 0u || height == 0u) {
        throw std::runtime_error("CompressPixelLzss24 requires non-zero dimensions");
    }

    const std::size_t total_pixels =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t expected_bytes =
        total_pixels * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
    if (bgra_pixels.size() != expected_bytes) {
        throw std::runtime_error("CompressPixelLzss24 buffer byte count mismatch");
    }

    std::vector<std::uint8_t> out(kLzssFrameHeaderBytes, 0u);
    out.reserve(kLzssFrameHeaderBytes + total_pixels * 3u + (total_pixels / 8u) + 8u);

    const std::uint8_t* raw = bgra_pixels.data();
    std::vector<int> head(kHashBucketCount, kHashEmptyPos);
    std::vector<int> prev(total_pixels, kHashEmptyPos);

    auto insert_pixel = [&](std::size_t p) {
        const std::size_t h = HashPixelBgr(raw, p);
        prev[p] = head[h];
        head[h] = static_cast<int>(p);
    };

    std::size_t flag_pos = 0u;
    std::uint8_t flag_byte = 0u;
    int bit_index = 0;

    std::size_t pos = 0u;
    while (pos < total_pixels) {
        if (bit_index == 0) {
            flag_pos = out.size();
            flag_byte = 0u;
            out.push_back(0u);
        }

        std::size_t best_len = 0u;
        std::size_t best_off = 0u;
        const std::size_t max_len = std::min(kPixelLzssMaxMatch, total_pixels - pos);

        if (pos > 0u) {
            const std::size_t min_cand =
                pos > kLzssWindowUnits ? (pos - kLzssWindowUnits) : 0u;
            const std::size_t h = HashPixelBgr(raw, pos);
            int cand = head[h];
            int steps = 0;

            while (cand >= static_cast<int>(min_cand) && steps < kMaxHashChainSteps) {
                ++steps;
                const std::size_t cpos = static_cast<std::size_t>(cand);
                if (SamePixelBgr(raw, cpos + best_len, pos + best_len)) {
                    std::size_t match_len = 0u;
                    while (match_len < max_len &&
                           SamePixelBgr(raw, cpos + match_len, pos + match_len)) {
                        ++match_len;
                    }
                    if (match_len >= kPixelLzssMinMatch && match_len > best_len) {
                        best_len = match_len;
                        best_off = pos - cpos;
                        if (best_len == max_len) {
                            break;
                        }
                    }
                }
                cand = prev[cpos];
            }
        }

        if (best_len >= kPixelLzssMinMatch) {
            const std::uint16_t token = static_cast<std::uint16_t>(
                (best_off << kLzssOffsetShift) | (best_len - kPixelLzssMinMatch));
            AppendU16Le(out, token);
            for (std::size_t s = 0u; s < best_len; ++s) {
                insert_pixel(pos + s);
            }
            pos += best_len;
        } else {
            flag_byte = static_cast<std::uint8_t>(flag_byte | (1u << bit_index));
            out[flag_pos] = flag_byte;
            const std::size_t byte_off =
                pos * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
            out.push_back(raw[byte_off + 0u]);
            out.push_back(raw[byte_off + 1u]);
            out.push_back(raw[byte_off + 2u]);
            insert_pixel(pos);
            ++pos;
        }

        bit_index = (bit_index + 1) % kBitsPerFlagByte;
    }

    WriteU32LeAt(out, 0u, static_cast<std::uint32_t>(out.size()));
    WriteU32LeAt(out, kLzssUnpSizeOffset, static_cast<std::uint32_t>(expected_bytes));
    return out;
}

std::vector<std::uint8_t> DecompressPixelLzss24(
    boost::span<const std::uint8_t> framed_arc, std::uint16_t width,
    std::uint16_t height) {
    if (width == 0u || height == 0u) {
        throw std::runtime_error("DecompressPixelLzss24 requires non-zero dimensions");
    }
    if (framed_arc.size() < kLzssFrameHeaderBytes) {
        throw std::runtime_error("DecompressPixelLzss24 buffer shorter than 8-byte frame header");
    }

    const std::uint32_t arc_size = ReadU32Le(framed_arc, 0u);
    const std::uint32_t unp_size = ReadU32Le(framed_arc, kLzssUnpSizeOffset);
    if (arc_size < kLzssFrameHeaderBytes ||
        static_cast<std::size_t>(arc_size) > framed_arc.size()) {
        throw std::runtime_error("DecompressPixelLzss24 invalid or truncated arc_size");
    }

    const std::size_t total_pixels =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t expected_unp_bytes =
        total_pixels * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
    if (static_cast<std::size_t>(unp_size) != expected_unp_bytes) {
        throw std::runtime_error(
            "DecompressPixelLzss24 unp_size mismatch (expected width * height * 4)");
    }

    const std::size_t arc_limit = static_cast<std::size_t>(arc_size);
    std::vector<std::uint8_t> out(expected_unp_bytes, 0u);

    std::size_t si = kLzssFrameHeaderBytes;
    std::size_t written_pixels = 0u;

    while (written_pixels < total_pixels) {
        if (si >= arc_limit) {
            throw std::runtime_error("DecompressPixelLzss24 truncated before completing raster");
        }
        const std::uint8_t flag = framed_arc[si++];
        for (int bit = 0; bit < kBitsPerFlagByte && written_pixels < total_pixels; ++bit) {
            if ((flag & (1u << bit)) != 0u) {
                if (si + 2u >= arc_limit) {
                    throw std::runtime_error("DecompressPixelLzss24 truncated 3-byte BGR literal");
                }
                const std::size_t dst_off =
                    written_pixels * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
                out[dst_off + 0u] = framed_arc[si];
                out[dst_off + 1u] = framed_arc[si + 1u];
                out[dst_off + 2u] = framed_arc[si + 2u];
                out[dst_off + 3u] = imaging::kOpaqueAlpha;
                si += 3u;
                ++written_pixels;
                continue;
            }

            if (si + 1u >= arc_limit) {
                throw std::runtime_error("DecompressPixelLzss24 truncated back-reference token");
            }
            const std::uint16_t token = ReadU16Le(framed_arc, si);
            si += 2u;

            const std::size_t offset_pixels =
                static_cast<std::size_t>(token >> kLzssOffsetShift);
            const std::size_t count_pixels =
                static_cast<std::size_t>(token & kLzssCountMask) + kPixelLzssMinMatch;
            if (offset_pixels == 0u || offset_pixels > written_pixels) {
                throw std::runtime_error("DecompressPixelLzss24 invalid back-reference offset");
            }

            const std::size_t copy_pixels =
                std::min(count_pixels, total_pixels - written_pixels);
            for (std::size_t k = 0u; k < copy_pixels; ++k) {
                const std::size_t src_off =
                    (written_pixels - offset_pixels) *
                    static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
                const std::size_t dst_off =
                    written_pixels * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
                out[dst_off + 0u] = out[src_off + 0u];
                out[dst_off + 1u] = out[src_off + 1u];
                out[dst_off + 2u] = out[src_off + 2u];
                out[dst_off + 3u] = out[src_off + 3u];
                ++written_pixels;
            }
        }
    }

    return out;
}

G00Image G00Codec::Decode(boost::span<const std::uint8_t> raw_g00) {
    if (raw_g00.size() < kG00FileHeaderBytes) {
        throw std::runtime_error("G00Codec::Decode input shorter than 5-byte .g00 header");
    }

    const std::uint8_t raw_type = raw_g00[0u];
    if (raw_type > static_cast<std::uint8_t>(G00Type::kType2MultiCutBgra32)) {
        throw std::runtime_error("G00Codec::Decode unsupported .g00 type " +
                                 std::to_string(raw_type));
    }

    const std::uint16_t width = ReadU16Le(raw_g00, kHeaderWidthOffset);
    const std::uint16_t height = ReadU16Le(raw_g00, kHeaderHeightOffset);
    if (width == 0u || height == 0u) {
        throw std::runtime_error("G00Codec::Decode invalid zero sprite dimensions");
    }

    G00Image img;
    img.type = static_cast<G00Type>(raw_type);
    img.width = width;
    img.height = height;

    if (img.type == G00Type::kType0Bgr24) {
        if (raw_g00.size() < kG00FileHeaderBytes + kLzssFrameHeaderBytes) {
            throw std::runtime_error("G00Codec::Decode Type 0 missing LZSS header");
        }
        std::vector<std::uint8_t> pixels =
            DecompressPixelLzss24(raw_g00.subspan(kG00FileHeaderBytes), width, height);
        img.canvas = BgraImage(static_cast<int>(width), static_cast<int>(height),
                               std::move(pixels));
        img.canvas_modified = false;
        return img;
    }

    if (img.type == G00Type::kType1Paletted8) {
        if (raw_g00.size() < kG00FileHeaderBytes + kLzssFrameHeaderBytes) {
            throw std::runtime_error("G00Codec::Decode Type 1 missing LZSS header");
        }
        const std::vector<std::uint8_t> plain =
            DecompressByteLzss(raw_g00.subspan(kG00FileHeaderBytes));
        if (plain.size() < kType1PaletteCountHeaderBytes) {
            throw std::runtime_error("G00Codec::Decode Type 1 missing palette_count");
        }

        const std::uint16_t palette_count = ReadU16Le(plain, 0u);
        if (palette_count == 0u || palette_count > kMaxType1PaletteColors) {
            throw std::runtime_error("G00Codec::Decode Type 1 invalid palette_count");
        }

        const std::size_t pixel_count =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t palette_bytes =
            static_cast<std::size_t>(palette_count) *
            static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
        const std::size_t expected_plain =
            kType1PaletteCountHeaderBytes + palette_bytes + pixel_count;
        if (plain.size() != expected_plain) {
            throw std::runtime_error("G00Codec::Decode Type 1 payload size mismatch");
        }

        img.type1_palette.reserve(palette_count);
        for (std::size_t i = 0u; i < palette_count; ++i) {
            const std::size_t off =
                kType1PaletteCountHeaderBytes +
                i * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
            img.type1_palette.push_back(
                BgraColor{plain[off + 0u], plain[off + 1u], plain[off + 2u], plain[off + 3u]});
        }

        const std::size_t idx_start = kType1PaletteCountHeaderBytes + palette_bytes;
        img.type1_indices.assign(plain.begin() + static_cast<std::ptrdiff_t>(idx_start),
                                 plain.end());

        std::vector<std::uint8_t> bgra(
            pixel_count * static_cast<std::size_t>(imaging::kBgraBytesPerPixel), 0u);
        for (std::size_t p = 0u; p < pixel_count; ++p) {
            const std::uint8_t pal_idx = img.type1_indices[p];
            if (static_cast<std::size_t>(pal_idx) >= img.type1_palette.size()) {
                throw std::runtime_error("G00Codec::Decode Type 1 palette index out of bounds");
            }
            const BgraColor c = img.type1_palette[pal_idx];
            const std::size_t dst_off =
                p * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
            bgra[dst_off + 0u] = c.b;
            bgra[dst_off + 1u] = c.g;
            bgra[dst_off + 2u] = c.r;
            bgra[dst_off + 3u] = c.a;
        }
        img.canvas = BgraImage(static_cast<int>(width), static_cast<int>(height),
                               std::move(bgra));
        img.canvas_modified = false;
        return img;
    }

    // Type 2: multi-cut, multi-part 32-bpp straight-BGRA container.
    if (raw_g00.size() < kType2DescriptorArrayOffset) {
        throw std::runtime_error("G00Codec::Decode Type 2 missing cut_count");
    }
    const std::uint32_t cut_count = ReadU32Le(raw_g00, kType2CutCountOffset);
    if (cut_count > kMaxPlausibleCutCount) {
        throw std::runtime_error("G00Codec::Decode Type 2 implausible cut_count");
    }

    const std::uint64_t lzss_off_u64 =
        static_cast<std::uint64_t>(kType2DescriptorArrayOffset) +
        static_cast<std::uint64_t>(cut_count) *
            static_cast<std::uint64_t>(kG00CutDescriptorBytes);
    if (lzss_off_u64 + kLzssFrameHeaderBytes > raw_g00.size()) {
        throw std::runtime_error("G00Codec::Decode Type 2 truncated descriptor table");
    }
    const std::size_t lzss_off = static_cast<std::size_t>(lzss_off_u64);

    const std::vector<std::uint8_t> decomp = DecompressByteLzss(raw_g00.subspan(lzss_off));
    if (decomp.size() < kType2IndexHeaderBytes) {
        throw std::runtime_error("G00Codec::Decode Type 2 missing index_count header");
    }

    const std::uint32_t index_count = ReadU32Le(decomp, 0u);
    if (index_count != cut_count) {
        throw std::runtime_error("G00Codec::Decode Type 2 index_count != cut_count");
    }

    const std::uint64_t index_end_u64 =
        static_cast<std::uint64_t>(kType2IndexHeaderBytes) +
        static_cast<std::uint64_t>(index_count) *
            static_cast<std::uint64_t>(kType2IndexEntryStride);
    if (index_end_u64 > decomp.size()) {
        throw std::runtime_error("G00Codec::Decode Type 2 truncated cut index table");
    }

    img.cuts.reserve(cut_count);
    for (std::uint32_t i = 0u; i < cut_count; ++i) {
        G00Cut cut;
        const std::size_t desc_off =
            kType2DescriptorArrayOffset +
            static_cast<std::size_t>(i) * kG00CutDescriptorBytes;
        const boost::span<const std::uint8_t> desc_span =
            raw_g00.subspan(desc_off, kG00CutDescriptorBytes);
        std::copy(desc_span.begin(), desc_span.end(), cut.raw_descriptor_bytes.begin());
        cut.has_raw_descriptor = true;
        cut.descriptor = ParseCutDescriptor(desc_span);
        cut.canvas = BgraImage(static_cast<int>(width), static_cast<int>(height));
        cut.modified = false;

        const std::size_t idx_off =
            kType2IndexHeaderBytes + static_cast<std::size_t>(i) * kType2IndexEntryStride;
        const std::uint32_t cut_offset = ReadU32Le(decomp, idx_off);
        const std::uint32_t cut_size =
            ReadU32Le(decomp, idx_off + kType2IndexSizeFieldOffset);

        if (cut_size == 0u) {
            cut.is_empty = true;
            cut.empty_index_offset = cut_offset;
            img.cuts.push_back(std::move(cut));
            continue;
        }

        if (cut_size < kG00CutHeaderBytes) {
            throw std::runtime_error("G00Codec::Decode Type 2 cut smaller than 116-byte header");
        }
        const std::uint64_t cut_end_u64 =
            static_cast<std::uint64_t>(cut_offset) + static_cast<std::uint64_t>(cut_size);
        if (cut_end_u64 > decomp.size()) {
            throw std::runtime_error("G00Codec::Decode Type 2 cut slice exceeds stream");
        }

        const std::size_t cut_beg = static_cast<std::size_t>(cut_offset);
        const std::size_t cut_end = static_cast<std::size_t>(cut_end_u64);

        cut.is_empty = false;
        cut.raw_cut_bytes.assign(decomp.begin() + static_cast<std::ptrdiff_t>(cut_beg),
                                 decomp.begin() + static_cast<std::ptrdiff_t>(cut_end));
        cut.cut_header = ParseCutHeader(
            boost::span<const std::uint8_t>(decomp.data() + cut_beg, kG00CutHeaderBytes));

        std::size_t part_cursor = cut_beg + kG00CutHeaderBytes;
        cut.part_headers.reserve(cut.cut_header.part_count);
        for (std::uint16_t p = 0u; p < cut.cut_header.part_count; ++p) {
            if (part_cursor + kG00PartHeaderBytes > cut_end) {
                throw std::runtime_error("G00Codec::Decode Type 2 truncated 92-byte part header");
            }
            const G00PartHeader phdr = ParsePartHeader(boost::span<const std::uint8_t>(
                decomp.data() + part_cursor, kG00PartHeaderBytes));
            part_cursor += kG00PartHeaderBytes;

            const std::uint64_t slab_bytes_u64 =
                static_cast<std::uint64_t>(phdr.part_w) *
                static_cast<std::uint64_t>(phdr.part_h) *
                static_cast<std::uint64_t>(imaging::kBgraBytesPerPixel);
            if (static_cast<std::uint64_t>(part_cursor) + slab_bytes_u64 > cut_end) {
                throw std::runtime_error("G00Codec::Decode Type 2 truncated part pixel slab");
            }

            for (std::uint16_t py = 0u; py < phdr.part_h; ++py) {
                const int dst_y =
                    static_cast<int>(cut.descriptor.y1) + static_cast<int>(phdr.part_y) +
                    static_cast<int>(py);
                for (std::uint16_t px = 0u; px < phdr.part_w; ++px) {
                    const int dst_x =
                        static_cast<int>(cut.descriptor.x1) + static_cast<int>(phdr.part_x) +
                        static_cast<int>(px);
                    const std::size_t pix_off =
                        part_cursor +
                        (static_cast<std::size_t>(py) * static_cast<std::size_t>(phdr.part_w) +
                         static_cast<std::size_t>(px)) *
                            static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
                    if (dst_x < 0 || dst_x >= static_cast<int>(width) || dst_y < 0 ||
                        dst_y >= static_cast<int>(height)) {
                        continue;
                    }
                    const BgraColor src_c{decomp[pix_off + 0u], decomp[pix_off + 1u],
                                          decomp[pix_off + 2u], decomp[pix_off + 3u]};
                    if (src_c.a != imaging::kTransparentAlpha ||
                        cut.canvas.PixelAt(dst_x, dst_y).a == imaging::kTransparentAlpha) {
                        cut.canvas.SetPixel(dst_x, dst_y, src_c);
                    }
                }
            }

            part_cursor += static_cast<std::size_t>(slab_bytes_u64);
            cut.part_headers.push_back(phdr);
        }

        img.cuts.push_back(std::move(cut));
    }

    return img;
}

std::vector<std::uint8_t> G00Codec::Encode(const G00Image& image) {
    if (image.width == 0u || image.height == 0u) {
        throw std::runtime_error("G00Codec::Encode requires non-zero sprite dimensions");
    }

    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(image.type));
    AppendU16Le(out, image.width);
    AppendU16Le(out, image.height);

    if (image.type == G00Type::kType0Bgr24) {
        if (image.canvas.width() != static_cast<int>(image.width) ||
            image.canvas.height() != static_cast<int>(image.height)) {
            throw std::runtime_error("G00Codec::Encode Type 0 canvas dimension mismatch");
        }
        const std::vector<std::uint8_t> framed =
            CompressPixelLzss24(image.canvas.pixels(), image.width, image.height);
        const std::vector<std::uint8_t> verified =
            DecompressPixelLzss24(framed, image.width, image.height);
        const std::size_t total_pixels =
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
        for (std::size_t p = 0u; p < total_pixels; ++p) {
            const std::size_t off =
                p * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
            if (verified[off + 0u] != image.canvas.data()[off + 0u] ||
                verified[off + 1u] != image.canvas.data()[off + 1u] ||
                verified[off + 2u] != image.canvas.data()[off + 2u] ||
                verified[off + 3u] != imaging::kOpaqueAlpha) {
                throw std::runtime_error(
                    "G00Codec::Encode Type 0 LZSS round-trip self-verification failed");
            }
        }
        out.insert(out.end(), framed.begin(), framed.end());
        return out;
    }

    if (image.type == G00Type::kType1Paletted8) {
        if (image.canvas.width() != static_cast<int>(image.width) ||
            image.canvas.height() != static_cast<int>(image.height)) {
            throw std::runtime_error("G00Codec::Encode Type 1 canvas dimension mismatch");
        }

        const std::size_t pixel_count =
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
        std::vector<BgraColor> palette;
        std::vector<std::uint8_t> indices;

        const bool can_preserve_untouched =
            !image.canvas_modified && !image.type1_palette.empty() &&
            image.type1_palette.size() <= kMaxType1PaletteColors &&
            image.type1_indices.size() == pixel_count;

        if (can_preserve_untouched) {
            for (const std::uint8_t idx : image.type1_indices) {
                if (static_cast<std::size_t>(idx) >= image.type1_palette.size()) {
                    throw std::runtime_error(
                        "G00Codec::Encode Type 1 untouched index exceeds palette");
                }
            }
            palette = image.type1_palette;
            indices = image.type1_indices;
        } else {
            BuildType1PaletteAndIndices(image.canvas, palette, indices);
        }

        std::vector<std::uint8_t> plain;
        plain.reserve(kType1PaletteCountHeaderBytes +
                      palette.size() * static_cast<std::size_t>(imaging::kBgraBytesPerPixel) +
                      indices.size());
        AppendU16Le(plain, static_cast<std::uint16_t>(palette.size()));
        for (const BgraColor& c : palette) {
            plain.push_back(c.b);
            plain.push_back(c.g);
            plain.push_back(c.r);
            plain.push_back(c.a);
        }
        plain.insert(plain.end(), indices.begin(), indices.end());

        const std::vector<std::uint8_t> framed = CompressByteLzss(plain);
        if (DecompressByteLzss(framed) != plain) {
            throw std::runtime_error(
                "G00Codec::Encode Type 1 LZSS round-trip self-verification failed");
        }
        out.insert(out.end(), framed.begin(), framed.end());
        return out;
    }

    if (image.type != G00Type::kType2MultiCutBgra32) {
        throw std::runtime_error("G00Codec::Encode unsupported G00Type");
    }

    if (image.cuts.size() > kMaxPlausibleCutCount) {
        throw std::runtime_error("G00Codec::Encode Type 2 cut count exceeds limit");
    }

    const std::uint32_t cut_count = static_cast<std::uint32_t>(image.cuts.size());
    AppendU32Le(out, cut_count);

    std::vector<std::array<std::uint8_t, kG00CutDescriptorBytes>> encoded_descriptors;
    encoded_descriptors.reserve(cut_count);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> index_entries;
    index_entries.reserve(cut_count);

    const std::size_t table_bytes =
        kType2IndexHeaderBytes + static_cast<std::size_t>(cut_count) * kType2IndexEntryStride;
    std::vector<std::uint8_t> concatenated_cuts;

    for (const G00Cut& cut : image.cuts) {
        if (!cut.modified && cut.is_empty) {
            encoded_descriptors.push_back(cut.has_raw_descriptor
                                              ? cut.raw_descriptor_bytes
                                              : SerializeCutDescriptor(cut.descriptor));
            index_entries.emplace_back(cut.empty_index_offset, 0u);
            continue;
        }

        if (!cut.modified && !cut.is_empty && !cut.raw_cut_bytes.empty()) {
            encoded_descriptors.push_back(cut.has_raw_descriptor
                                              ? cut.raw_descriptor_bytes
                                              : SerializeCutDescriptor(cut.descriptor));
            const std::uint32_t cut_off =
                static_cast<std::uint32_t>(table_bytes + concatenated_cuts.size());
            const std::uint32_t cut_sz = static_cast<std::uint32_t>(cut.raw_cut_bytes.size());
            index_entries.emplace_back(cut_off, cut_sz);
            concatenated_cuts.insert(concatenated_cuts.end(), cut.raw_cut_bytes.begin(),
                                     cut.raw_cut_bytes.end());
            continue;
        }

        if (cut.canvas.width() != static_cast<int>(image.width) ||
            cut.canvas.height() != static_cast<int>(image.height)) {
            throw std::runtime_error("G00Codec::Encode Type 2 modified cut canvas size mismatch");
        }

        RectI orig_rect{0, 0, 0, 0};
        if (!cut.is_empty && cut.descriptor.x2 >= cut.descriptor.x1 &&
            cut.descriptor.y2 >= cut.descriptor.y1) {
            orig_rect = ClampRect(
                RectI{static_cast<int>(cut.descriptor.x1),
                      static_cast<int>(cut.descriptor.y1),
                      static_cast<int>(cut.descriptor.x2 - cut.descriptor.x1 + 1),
                      static_cast<int>(cut.descriptor.y2 - cut.descriptor.y1 + 1)},
                static_cast<int>(image.width), static_cast<int>(image.height));
        }

        const RectI alpha_rect = cut.canvas.NonZeroAlphaBounds(kMinVisibleAlpha);
        const RectI union_bounds = ClampRect(
            UnionRect(orig_rect, alpha_rect),
            static_cast<int>(image.width), static_cast<int>(image.height));

        if (union_bounds.empty()) {
            G00CutDescriptor empty_desc{};
            empty_desc.origin_x = cut.descriptor.origin_x;
            empty_desc.origin_y = cut.descriptor.origin_y;
            encoded_descriptors.push_back(SerializeCutDescriptor(empty_desc));
            const std::uint32_t cut_off =
                static_cast<std::uint32_t>(table_bytes + concatenated_cuts.size());
            index_entries.emplace_back(cut_off, 0u);
            continue;
        }

        G00CutDescriptor new_desc{};
        new_desc.x1 = static_cast<std::int32_t>(union_bounds.x);
        new_desc.y1 = static_cast<std::int32_t>(union_bounds.y);
        new_desc.x2 = static_cast<std::int32_t>(union_bounds.x + union_bounds.w - 1);
        new_desc.y2 = static_cast<std::int32_t>(union_bounds.y + union_bounds.h - 1);
        new_desc.origin_x = cut.descriptor.origin_x;
        new_desc.origin_y = cut.descriptor.origin_y;
        encoded_descriptors.push_back(SerializeCutDescriptor(new_desc));

        G00CutHeader new_hdr{};
        new_hdr.cut_type = cut.is_empty ? kDefaultCutType : cut.cut_header.cut_type;
        new_hdr.part_count = 1u;
        new_hdr.bbox_x = 0;
        new_hdr.bbox_y = 0;
        new_hdr.bbox_w = static_cast<std::int32_t>(union_bounds.w);
        new_hdr.bbox_h = static_cast<std::int32_t>(union_bounds.h);
        new_hdr.center_x = cut.cut_header.center_x;
        new_hdr.center_y = cut.cut_header.center_y;
        new_hdr.cut_canvas_w = static_cast<std::int32_t>(union_bounds.w);
        new_hdr.cut_canvas_h = static_cast<std::int32_t>(union_bounds.h);
        new_hdr.reserved = cut.cut_header.reserved;

        const BgraImage slab = cut.canvas.Crop(union_bounds);
        G00PartHeader new_part{};
        new_part.part_x = 0u;
        new_part.part_y = 0u;
        new_part.trans_flag =
            SlabHasAnyNonOpaquePixel(slab) ? kTransFlagTranslucent : kTransFlagOpaque;
        new_part.part_w = static_cast<std::uint16_t>(union_bounds.w);
        new_part.part_h = static_cast<std::uint16_t>(union_bounds.h);
        if (!cut.part_headers.empty()) {
            new_part.reserved = cut.part_headers[0].reserved;
        }

        std::vector<std::uint8_t> cut_blob = SerializeCutHeader(new_hdr);
        cut_blob.reserve(kG00CutHeaderBytes + kG00PartHeaderBytes + slab.byte_size());
        AppendPartHeader(cut_blob, new_part);
        cut_blob.insert(cut_blob.end(), slab.pixels().begin(), slab.pixels().end());

        const std::uint32_t cut_off =
            static_cast<std::uint32_t>(table_bytes + concatenated_cuts.size());
        const std::uint32_t cut_sz = static_cast<std::uint32_t>(cut_blob.size());
        index_entries.emplace_back(cut_off, cut_sz);
        concatenated_cuts.insert(concatenated_cuts.end(), cut_blob.begin(), cut_blob.end());
    }

    for (const auto& desc_bytes : encoded_descriptors) {
        out.insert(out.end(), desc_bytes.begin(), desc_bytes.end());
    }

    std::vector<std::uint8_t> decomp_stream;
    decomp_stream.reserve(table_bytes + concatenated_cuts.size());
    AppendU32Le(decomp_stream, cut_count);
    for (const auto& [off, sz] : index_entries) {
        AppendU32Le(decomp_stream, off);
        AppendU32Le(decomp_stream, sz);
    }
    decomp_stream.insert(decomp_stream.end(), concatenated_cuts.begin(),
                         concatenated_cuts.end());

    const std::vector<std::uint8_t> framed = CompressByteLzss(decomp_stream);
    if (DecompressByteLzss(framed) != decomp_stream) {
        throw std::runtime_error(
            "G00Codec::Encode Type 2 LZSS round-trip self-verification failed");
    }
    out.insert(out.end(), framed.begin(), framed.end());
    return out;
}

bool G00Codec::VerifyMetadataParity(
    boost::span<const std::uint8_t> orig_g00,
    boost::span<const std::uint8_t> patched_g00,
    const std::set<std::size_t>& modified_cuts,
    std::string* error_out) {
    G00Image orig;
    G00Image patched;
    try {
        orig = Decode(orig_g00);
    } catch (const std::exception& ex) {
        return FailParity(error_out, std::string("failed to decode orig_g00: ") + ex.what());
    }
    try {
        patched = Decode(patched_g00);
    } catch (const std::exception& ex) {
        return FailParity(error_out, std::string("failed to decode patched_g00: ") + ex.what());
    }

    if (orig.type != patched.type) {
        return FailParity(error_out, "G00Type mismatch between original and patched sprite");
    }
    if (orig.width != patched.width || orig.height != patched.height) {
        return FailParity(error_out, "sprite canvas dimensions changed");
    }

    if (orig.type != G00Type::kType2MultiCutBgra32) {
        if (!modified_cuts.empty()) {
            return FailParity(error_out, "modified_cuts must be empty for single-canvas G00");
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    if (orig.cuts.size() != patched.cuts.size()) {
        return FailParity(error_out, "Type 2 cut_count mismatch");
    }
    for (const std::size_t cut_idx : modified_cuts) {
        if (cut_idx >= orig.cuts.size()) {
            return FailParity(error_out, "modified_cuts index out of range: " +
                                             std::to_string(cut_idx));
        }
    }

    for (std::size_t i = 0u; i < orig.cuts.size(); ++i) {
        const G00Cut& oc = orig.cuts[i];
        const G00Cut& pc = patched.cuts[i];

        if (oc.descriptor.origin_x != pc.descriptor.origin_x ||
            oc.descriptor.origin_y != pc.descriptor.origin_y) {
            return FailParity(error_out,
                              "CutDescriptor origin_x/y drifted on cut " + std::to_string(i));
        }

        if (!oc.is_empty && !pc.is_empty) {
            if (oc.cut_header.cut_type != pc.cut_header.cut_type) {
                return FailParity(error_out,
                                  "CutHeader cut_type drifted on cut " + std::to_string(i));
            }
            if (oc.cut_header.center_x != pc.cut_header.center_x ||
                oc.cut_header.center_y != pc.cut_header.center_y) {
                return FailParity(error_out,
                                  "CutHeader center_x/y drifted on cut " + std::to_string(i));
            }
            if (oc.cut_header.reserved != pc.cut_header.reserved) {
                return FailParity(error_out,
                                  "CutHeader reserved[80] drifted on cut " + std::to_string(i));
            }
        }

        if (modified_cuts.find(i) != modified_cuts.end()) {
            continue;
        }

        if (oc.is_empty != pc.is_empty) {
            return FailParity(error_out,
                              "untouched cut emptiness changed on cut " + std::to_string(i));
        }
        if (oc.raw_descriptor_bytes != pc.raw_descriptor_bytes) {
            return FailParity(error_out,
                              "untouched raw_descriptor_bytes drifted on cut " +
                                  std::to_string(i));
        }
        if (oc.is_empty && oc.empty_index_offset != pc.empty_index_offset) {
            return FailParity(error_out,
                              "untouched empty_index_offset drifted on cut " +
                                  std::to_string(i));
        }
        if (!oc.is_empty && oc.raw_cut_bytes != pc.raw_cut_bytes) {
            return FailParity(error_out,
                              "untouched raw_cut_bytes drifted on cut " + std::to_string(i));
        }
        if (oc.canvas != pc.canvas) {
            return FailParity(error_out,
                              "untouched cut pixels drifted on cut " + std::to_string(i));
        }
    }

    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

void G00Codec::AssertMetadataParity(
    boost::span<const std::uint8_t> orig_g00,
    boost::span<const std::uint8_t> patched_g00,
    const std::set<std::size_t>& modified_cuts) {
    std::string err;
    if (!VerifyMetadataParity(orig_g00, patched_g00, modified_cuts, &err)) {
        throw std::runtime_error("G00 metadata parity failure: " + err);
    }
}

}  // namespace adi::g00
