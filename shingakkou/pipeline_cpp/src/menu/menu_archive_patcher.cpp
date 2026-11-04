// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/menu_archive_patcher.h"

#include "ddp/ddp_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace shin::menu {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kTgaIdLengthOffset = 0;
constexpr std::size_t kTgaColorMapTypeOffset = 1;
constexpr std::size_t kTgaImageTypeOffset = 2;
constexpr std::size_t kTgaColorMapFirstEntryOffset = 3;
constexpr std::size_t kTgaColorMapLengthOffset = 5;
constexpr std::size_t kTgaColorMapEntrySizeOffset = 7;
constexpr std::size_t kTgaXOriginOffset = 8;
constexpr std::size_t kTgaYOriginOffset = 10;
constexpr std::size_t kTgaWidthOffset = 12;
constexpr std::size_t kTgaHeightOffset = 14;
constexpr std::size_t kTgaBppOffset = 16;
constexpr std::size_t kTgaDescriptorOffset = 17;

constexpr std::size_t kTgaRlePacketMask = 0x80U;
constexpr std::uint8_t kTgaRleCountMask = 0x7FU;
constexpr std::size_t kTgaRleCountBias = 1;
constexpr std::size_t kTgaMaxRleRunLength = 128;
constexpr std::size_t kMinRunLengthForRlePacket = 2;
constexpr std::size_t kSinglePixelCount = 1;

constexpr std::size_t kHighByteOffsetInU16 = 1;
constexpr std::size_t kByteOffset1 = 1;
constexpr std::size_t kByteOffset2 = 2;
constexpr std::size_t kByteOffset3 = 3;
constexpr std::size_t kByteShift = 8;
constexpr std::size_t kTwoBytesShift = 16;
constexpr std::size_t kThreeBytesShift = 24;
constexpr std::uint32_t kLowByteMask = 0xFFU;
constexpr std::size_t kDecimalRadix = 10;
constexpr std::size_t kWordBytes = 4;
constexpr std::size_t kRemThreeBytes = 3;
constexpr std::size_t kRemTwoBytes = 2;
constexpr std::size_t kRemOneByte = 1;

constexpr std::size_t kHxbMinHeaderBytes = 16;
constexpr std::size_t kHxbMagicLength = 7;
constexpr std::size_t kHxbLengthHighOffset = 8;
constexpr std::size_t kHxbLengthMidOffset = 9;
constexpr std::size_t kHxbLengthLowOffset = 10;

constexpr std::size_t kDdp2MagicByteCount = 4;
constexpr char kDdp2Magic[kDdp2MagicByteCount] = {'D', 'D', 'P', '2'};
constexpr std::size_t kDdp2HeaderByteCount = 32;
constexpr std::size_t kDdp2CountOffset = 4;
constexpr std::size_t kDdp2TailHeaderOffset = 8;
constexpr std::size_t kDdp2IndexEntryByteCount = 16;
constexpr std::size_t kDdp2EntryUnpackedOffset = 4;
constexpr std::size_t kDdp2EntryPackedOffset = 8;
constexpr std::size_t kShsWrapHeaderByteCount = 5;
constexpr std::uint8_t kShsLiteral32Opcode = 0x1FU;

constexpr char kXorMenuPatchMagic[kXorMenuPatchMagicSize] = {
    'S', 'H', 'I', 'N', 'M', 'E', 'N', 'U'};
constexpr std::size_t kPatchVersionOffset = 8;
constexpr std::size_t kPatchSinSysdTotalOffset = 12;
constexpr std::size_t kPatchSinSysdOrigSizeOffset = 16;
constexpr std::size_t kPatchSinSysdCountOffset = 20;
constexpr std::size_t kPatchBootupTotalOffset = 24;
constexpr std::size_t kPatchBootupOrigSizeOffset = 28;
constexpr std::size_t kPatchBootupCountOffset = 32;

constexpr std::size_t kRecordArchiveIdOffset = 0;
constexpr std::size_t kRecordEntryIndexOffset = 4;
constexpr std::size_t kRecordOrigHashOffset = 8;
constexpr std::size_t kRecordNewSizeOffset = 12;
constexpr std::size_t kRecordNewHashOffset = 16;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995U;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::uint32_t kXorStreamMultiplier = 0x9EU;
constexpr std::uint32_t kXorStreamAddend = 0x37U;
constexpr std::uint32_t kHashByteIndexMask = 3U;

enum class AssetTargetArchive {
    kSinSysd,
    kBootup,
};

enum class AssetPayloadKind {
    kTga,
    kCsv,
    kRaw,
    kDecryptedHxb,
};

struct DiscoveredAsset {
    AssetTargetArchive archive = AssetTargetArchive::kSinSysd;
    AssetPayloadKind kind = AssetPayloadKind::kRaw;
    std::size_t entry_index = 0;
    std::string file_path;
};

struct Ddp2EntryMeta {
    std::uint32_t offset = 0;
    std::uint32_t unpacked = 0;
    std::uint32_t packed = 0;
};

static std::uint16_t ReadU16Le(const Bytes& data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(data[offset + kHighByteOffsetInU16]) << kByteShift);
}

static void WriteU16Le(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((value >> kByteShift) & kLowByteMask));
}

static std::uint32_t ReadU32LePtr(const std::uint8_t* ptr) {
    return static_cast<std::uint32_t>(ptr[0]) |
           (static_cast<std::uint32_t>(ptr[kByteOffset1]) << kByteShift) |
           (static_cast<std::uint32_t>(ptr[kByteOffset2]) << kTwoBytesShift) |
           (static_cast<std::uint32_t>(ptr[kByteOffset3]) << kThreeBytesShift);
}

static std::uint32_t ReadU32Le(const Bytes& data, std::size_t offset) {
    if (offset + kWordBytes > data.size()) {
        throw std::runtime_error("ReadU32Le past end of buffer");
    }
    return ReadU32LePtr(data.data() + offset);
}

static void WriteU32Le(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((value >> kByteShift) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((value >> kTwoBytesShift) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((value >> kThreeBytesShift) & kLowByteMask));
}

static std::vector<Ddp2EntryMeta> ParseDdp2EntriesFromBytes(const Bytes& archive,
                                                            const std::string& label) {
    if (archive.size() < kDdp2HeaderByteCount ||
        std::memcmp(archive.data(), kDdp2Magic, kDdp2MagicByteCount) != 0) {
        throw std::runtime_error("Not a valid DDP2 archive: " + label);
    }
    const std::uint32_t count = ReadU32Le(archive, kDdp2CountOffset);
    const std::size_t index_end =
        kDdp2HeaderByteCount + static_cast<std::size_t>(count) * kDdp2IndexEntryByteCount;
    if (index_end > archive.size()) {
        throw std::runtime_error("DDP2 index table truncated in " + label);
    }
    std::vector<Ddp2EntryMeta> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off =
            kDdp2HeaderByteCount + static_cast<std::size_t>(i) * kDdp2IndexEntryByteCount;
        Ddp2EntryMeta entry;
        entry.offset = ReadU32Le(archive, off);
        entry.unpacked = ReadU32Le(archive, off + kDdp2EntryUnpackedOffset);
        entry.packed = ReadU32Le(archive, off + kDdp2EntryPackedOffset);
        entries.push_back(entry);
    }
    return entries;
}

static std::size_t ComputeStoredEntryLength(const Bytes& archive,
                                            const Ddp2EntryMeta& entry,
                                            const std::string& label) {
    if (entry.offset >= archive.size()) {
        throw std::runtime_error("DDP2 entry offset out of bounds in " + label);
    }
    const std::uint32_t raw_want = (entry.packed != 0) ? entry.packed : entry.unpacked;
    const std::size_t stored_len =
        std::min<std::size_t>(raw_want, archive.size() - static_cast<std::size_t>(entry.offset));
    if (stored_len == 0) {
        throw std::runtime_error("DDP2 entry has zero stored bytes in " + label);
    }
    return stored_len;
}

static std::uint8_t ComputeXorMaskByte(const std::uint8_t* orig_stored,
                                       std::size_t orig_stored_len,
                                       std::uint32_t orig_stored_hash,
                                       std::size_t byte_index) {
    const std::uint32_t idx32 = static_cast<std::uint32_t>(byte_index);
    const std::uint32_t shift =
        (idx32 & kHashByteIndexMask) * static_cast<std::uint32_t>(kBitsPerByte);
    const std::uint8_t hash_byte =
        static_cast<std::uint8_t>((orig_stored_hash >> shift) & kLowByteMask);
    const std::uint8_t stream_byte =
        static_cast<std::uint8_t>((idx32 * kXorStreamMultiplier + kXorStreamAddend) & kLowByteMask);
    return static_cast<std::uint8_t>(
        orig_stored[byte_index % orig_stored_len] ^ hash_byte ^ stream_byte);
}

static void AppendXorRecordsForArchive(const Bytes& orig_archive,
                                       const std::vector<Ddp2EntryMeta>& entries,
                                       std::uint32_t archive_id,
                                       const std::map<std::size_t, Bytes>& replacements,
                                       const std::string& label,
                                       Bytes& out_blob) {
    for (const auto& [entry_index, replacement] : replacements) {
        if (entry_index >= entries.size()) {
            throw std::runtime_error("Replacement index out of range for " + label + ": " +
                                     std::to_string(entry_index));
        }
        if (replacement.empty()) {
            throw std::runtime_error("Replacement payload is empty for " + label + " index " +
                                     std::to_string(entry_index));
        }
        const Ddp2EntryMeta& entry = entries[entry_index];
        const std::size_t stored_len = ComputeStoredEntryLength(orig_archive, entry, label);
        const std::uint8_t* orig_stored_ptr = orig_archive.data() + entry.offset;
        const std::uint32_t orig_stored_hash =
            ComputeMenuMurmurHash2(orig_stored_ptr, stored_len);
        const std::uint32_t new_unpacked_size = static_cast<std::uint32_t>(replacement.size());
        const std::uint32_t new_unpacked_hash =
            ComputeMenuMurmurHash2(replacement.data(), replacement.size());

        WriteU32Le(out_blob, archive_id);
        WriteU32Le(out_blob, static_cast<std::uint32_t>(entry_index));
        WriteU32Le(out_blob, orig_stored_hash);
        WriteU32Le(out_blob, new_unpacked_size);
        WriteU32Le(out_blob, new_unpacked_hash);

        for (std::size_t k = 0; k < replacement.size(); ++k) {
            const std::uint8_t mask =
                ComputeXorMaskByte(orig_stored_ptr, stored_len, orig_stored_hash, k);
            out_blob.push_back(static_cast<std::uint8_t>(replacement[k] ^ mask));
        }
    }
}

static Bytes RebuildDdp2InMemory(const Bytes& orig_archive,
                                 const std::vector<Ddp2EntryMeta>& entries,
                                 const std::map<std::size_t, Bytes>& replacements) {
    struct EntryPlan {
        std::uint32_t offset = 0;
        std::uint32_t unpacked = 0;
        std::uint32_t packed = 0;
        std::uint32_t stored = 0;
        const Bytes* replacement = nullptr;
    };

    const std::size_t count = entries.size();
    const std::uint32_t data_start =
        static_cast<std::uint32_t>(kDdp2HeaderByteCount + count * kDdp2IndexEntryByteCount);

    std::vector<EntryPlan> plans;
    plans.reserve(count);
    std::uint32_t cursor = data_start;
    for (std::size_t i = 0; i < count; ++i) {
        EntryPlan plan;
        plan.offset = cursor;
        plan.unpacked = entries[i].unpacked;
        plan.packed = entries[i].packed;
        const auto rep_it = replacements.find(i);
        if (rep_it != replacements.end()) {
            plan.replacement = &rep_it->second;
            plan.unpacked = static_cast<std::uint32_t>(rep_it->second.size());
            plan.packed = static_cast<std::uint32_t>(rep_it->second.size() + kShsWrapHeaderByteCount);
            plan.stored = plan.packed;
            cursor += plan.stored;
            plans.push_back(plan);
            continue;
        }
        const std::uint32_t raw_want =
            (entries[i].packed != 0) ? entries[i].packed : entries[i].unpacked;
        const std::uint32_t avail =
            (entries[i].offset >= orig_archive.size())
                ? 0U
                : static_cast<std::uint32_t>(std::min<std::size_t>(
                      raw_want, orig_archive.size() - static_cast<std::size_t>(entries[i].offset)));
        plan.stored = avail;
        cursor += plan.stored;
        plans.push_back(plan);
    }

    Bytes out;
    out.reserve(cursor);
    out.insert(out.end(),
               orig_archive.begin(),
               orig_archive.begin() + static_cast<std::ptrdiff_t>(kDdp2MagicByteCount));
    WriteU32Le(out, static_cast<std::uint32_t>(count));
    out.insert(out.end(),
               orig_archive.begin() + static_cast<std::ptrdiff_t>(kDdp2TailHeaderOffset),
               orig_archive.begin() + static_cast<std::ptrdiff_t>(kDdp2HeaderByteCount));

    for (const EntryPlan& plan : plans) {
        WriteU32Le(out, plan.offset);
        WriteU32Le(out, plan.unpacked);
        WriteU32Le(out, plan.packed);
        WriteU32Le(out, 0U);
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (plans[i].replacement != nullptr) {
            const Bytes& rep = *plans[i].replacement;
            out.push_back(kShsLiteral32Opcode);
            const std::uint32_t n = static_cast<std::uint32_t>(rep.size());
            out.push_back(static_cast<std::uint8_t>((n >> kThreeBytesShift) & kLowByteMask));
            out.push_back(static_cast<std::uint8_t>((n >> kTwoBytesShift) & kLowByteMask));
            out.push_back(static_cast<std::uint8_t>((n >> kByteShift) & kLowByteMask));
            out.push_back(static_cast<std::uint8_t>(n & kLowByteMask));
            out.insert(out.end(), rep.begin(), rep.end());
            continue;
        }
        const std::size_t src_off = entries[i].offset;
        const std::size_t src_len = plans[i].stored;
        out.insert(out.end(),
                   orig_archive.begin() + static_cast<std::ptrdiff_t>(src_off),
                   orig_archive.begin() + static_cast<std::ptrdiff_t>(src_off + src_len));
    }

    return out;
}

static void ValidateTgaHeaderFields(const TgaHeaderInfo& header) {
    if (header.color_map_type != 0) {
        throw std::runtime_error("TGA color map is not supported for menu sprites");
    }
    const bool valid_image_type =
        (header.image_type == kTgaImageTypeUncompressedTrueColor) ||
        (header.image_type == kTgaImageTypeRleTrueColor);
    if (!valid_image_type) {
        throw std::runtime_error("Unsupported TGA image_type: " +
                                 std::to_string(header.image_type));
    }
    const bool valid_bpp = (header.bpp == kTgaBitDepth24) || (header.bpp == kTgaBitDepth32);
    if (!valid_bpp) {
        throw std::runtime_error("Unsupported TGA bit depth: " + std::to_string(header.bpp));
    }
    if (header.width == 0 || header.height == 0) {
        throw std::runtime_error("TGA dimensions must be non-zero");
    }
}

static bool IsValidTgaPayload(const Bytes& payload) {
    if (payload.size() < kTgaHeaderByteCount) {
        return false;
    }
    const std::uint8_t color_map_type = payload[kTgaColorMapTypeOffset];
    const std::uint8_t image_type = payload[kTgaImageTypeOffset];
    const std::uint8_t bpp = payload[kTgaBppOffset];
    const std::uint16_t width = ReadU16Le(payload, kTgaWidthOffset);
    const std::uint16_t height = ReadU16Le(payload, kTgaHeightOffset);
    if (color_map_type != 0 || width == 0 || height == 0) {
        return false;
    }
    const bool valid_image_type =
        (image_type == kTgaImageTypeUncompressedTrueColor) ||
        (image_type == kTgaImageTypeRleTrueColor);
    const bool valid_bpp = (bpp == kTgaBitDepth24) || (bpp == kTgaBitDepth32);
    return valid_image_type && valid_bpp;
}

static bool PixelsEqualAt(const Bytes& pixels,
                          std::size_t first_pixel_offset,
                          std::size_t second_pixel_offset,
                          std::size_t bytes_per_pixel) {
    return std::memcmp(pixels.data() + first_pixel_offset,
                       pixels.data() + second_pixel_offset,
                       bytes_per_pixel) == 0;
}

static std::size_t MeasureIdenticalRun(const Bytes& pixels,
                                       std::size_t row_start_pixel,
                                       std::size_t current_pixel,
                                       std::size_t row_width,
                                       std::size_t bytes_per_pixel) {
    const std::size_t max_pixels =
        std::min(kTgaMaxRleRunLength, row_width - (current_pixel - row_start_pixel));
    const std::size_t base_offset = current_pixel * bytes_per_pixel;
    std::size_t run_length = kSinglePixelCount;
    while (run_length < max_pixels) {
        const std::size_t next_offset = (current_pixel + run_length) * bytes_per_pixel;
        if (!PixelsEqualAt(pixels, base_offset, next_offset, bytes_per_pixel)) {
            break;
        }
        ++run_length;
    }
    return run_length;
}

static std::size_t MeasureLiteralRun(const Bytes& pixels,
                                     std::size_t row_start_pixel,
                                     std::size_t current_pixel,
                                     std::size_t row_width,
                                     std::size_t bytes_per_pixel) {
    const std::size_t max_pixels =
        std::min(kTgaMaxRleRunLength, row_width - (current_pixel - row_start_pixel));
    std::size_t literal_length = kSinglePixelCount;
    while (literal_length < max_pixels) {
        const std::size_t idx = current_pixel + literal_length;
        const bool has_following_pixel = (idx + kSinglePixelCount) < (row_start_pixel + row_width);
        if (has_following_pixel &&
            PixelsEqualAt(pixels,
                          idx * bytes_per_pixel,
                          (idx + kSinglePixelCount) * bytes_per_pixel,
                          bytes_per_pixel)) {
            break;
        }
        ++literal_length;
    }
    return literal_length;
}

static void EncodeTgaScanlineRle(const Bytes& pixels,
                                 std::size_t row_start_pixel,
                                 std::size_t row_width,
                                 std::size_t bytes_per_pixel,
                                 Bytes& out) {
    std::size_t pixel_cursor = row_start_pixel;
    const std::size_t row_end_pixel = row_start_pixel + row_width;
    while (pixel_cursor < row_end_pixel) {
        const std::size_t identical_run = MeasureIdenticalRun(
            pixels, row_start_pixel, pixel_cursor, row_width, bytes_per_pixel);
        if (identical_run >= kMinRunLengthForRlePacket) {
            const std::uint8_t packet_header = static_cast<std::uint8_t>(
                kTgaRlePacketMask | static_cast<std::uint8_t>(identical_run - kTgaRleCountBias));
            out.push_back(packet_header);
            const std::size_t byte_offset = pixel_cursor * bytes_per_pixel;
            out.insert(out.end(),
                       pixels.begin() + static_cast<std::ptrdiff_t>(byte_offset),
                       pixels.begin() + static_cast<std::ptrdiff_t>(byte_offset + bytes_per_pixel));
            pixel_cursor += identical_run;
            continue;
        }

        const std::size_t literal_run = MeasureLiteralRun(
            pixels, row_start_pixel, pixel_cursor, row_width, bytes_per_pixel);
        const std::uint8_t packet_header =
            static_cast<std::uint8_t>((literal_run - kTgaRleCountBias) & kTgaRleCountMask);
        out.push_back(packet_header);
        const std::size_t byte_offset = pixel_cursor * bytes_per_pixel;
        const std::size_t byte_count = literal_run * bytes_per_pixel;
        out.insert(out.end(),
                   pixels.begin() + static_cast<std::ptrdiff_t>(byte_offset),
                   pixels.begin() + static_cast<std::ptrdiff_t>(byte_offset + byte_count));
        pixel_cursor += literal_run;
    }
}

static std::string ToLowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

static bool StartsWith(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    return text.compare(0, prefix.size(), prefix) == 0;
}

static std::optional<std::size_t> ParseDigitsOnly(const std::string& digits) {
    if (digits.empty()) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (char ch : digits) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
        value = value * kDecimalRadix + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

static std::optional<DiscoveredAsset> TryClassifyAssetFile(const fs::path& file_path) {
    const std::string stem_lower = ToLowerAscii(file_path.stem().u8string());
    const std::string ext_lower = ToLowerAscii(file_path.extension().u8string());

    if (StartsWith(stem_lower, "sin_sysd_") || StartsWith(stem_lower, "sin_sysd#")) {
        const std::string index_part = stem_lower.substr(std::strlen("sin_sysd_"));
        const std::optional<std::size_t> parsed_index = ParseDigitsOnly(index_part);
        if (!parsed_index.has_value()) {
            return std::nullopt;
        }
        if (ext_lower == ".tga") {
            return DiscoveredAsset{AssetTargetArchive::kSinSysd,
                                   AssetPayloadKind::kTga,
                                   *parsed_index,
                                   file_path.u8string()};
        }
        if (ext_lower == ".csv") {
            return DiscoveredAsset{AssetTargetArchive::kSinSysd,
                                   AssetPayloadKind::kCsv,
                                   *parsed_index,
                                   file_path.u8string()};
        }
        if (ext_lower == ".bin" || ext_lower == ".bmp" || ext_lower == ".raw") {
            return DiscoveredAsset{AssetTargetArchive::kSinSysd,
                                   AssetPayloadKind::kRaw,
                                   *parsed_index,
                                   file_path.u8string()};
        }
        return std::nullopt;
    }

    if (StartsWith(stem_lower, "bootup_") || StartsWith(stem_lower, "bootup#")) {
        const std::string index_part = stem_lower.substr(std::strlen("bootup_"));
        const std::optional<std::size_t> parsed_index = ParseDigitsOnly(index_part);
        if (!parsed_index.has_value()) {
            return std::nullopt;
        }
        if (ext_lower == ".hxb") {
            return DiscoveredAsset{AssetTargetArchive::kBootup,
                                   AssetPayloadKind::kDecryptedHxb,
                                   *parsed_index,
                                   file_path.u8string()};
        }
        if (ext_lower == ".bin" || ext_lower == ".txt" || ext_lower == ".raw") {
            return DiscoveredAsset{AssetTargetArchive::kBootup,
                                   AssetPayloadKind::kRaw,
                                   *parsed_index,
                                   file_path.u8string()};
        }
        return std::nullopt;
    }

    return std::nullopt;
}

static std::vector<DiscoveredAsset> DiscoverAssetsInDirectory(const std::string& assets_dir) {
    std::vector<DiscoveredAsset> assets;
    if (assets_dir.empty()) {
        return assets;
    }
    const fs::path root_path = fs::u8path(assets_dir);
    if (!fs::exists(root_path)) {
        return assets;
    }

    std::vector<fs::path> candidate_files;
    for (const fs::directory_entry& dir_entry : fs::recursive_directory_iterator(root_path)) {
        if (dir_entry.is_regular_file()) {
            candidate_files.push_back(dir_entry.path());
        }
    }
    std::sort(candidate_files.begin(), candidate_files.end());

    for (const fs::path& file_path : candidate_files) {
        const std::optional<DiscoveredAsset> classified = TryClassifyAssetFile(file_path);
        if (classified.has_value()) {
            assets.push_back(*classified);
        }
    }
    return assets;
}

static void EnsureBackupFileExists(const fs::path& live_path, const fs::path& backup_path) {
    if (fs::exists(backup_path)) {
        return;
    }
    if (!fs::exists(live_path)) {
        throw std::runtime_error("Archive not found: " + live_path.u8string());
    }
    fs::copy_file(live_path, backup_path, fs::copy_options::none);
}

static void WriteBytesAtomically(const fs::path& target_path, const Bytes& data) {
    const fs::path temp_path = fs::u8path(target_path.u8string() + ".tmp");
    write_file(temp_path.u8string(), data);
    std::error_code ec;
    fs::rename(temp_path, target_path, ec);
    if (!ec) {
        return;
    }
    fs::remove(target_path, ec);
    ec.clear();
    fs::rename(temp_path, target_path, ec);
    if (ec) {
        throw std::runtime_error("Failed to replace archive " + target_path.u8string() + ": " +
                                 ec.message());
    }
}

static Bytes ReadDecompressedDdp2Entry(std::ifstream& archive_stream,
                                       const ddp::ArchiveEntry& entry) {
    archive_stream.clear();
    archive_stream.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    if (!archive_stream) {
        throw std::runtime_error("Failed to seek to DDP2 entry " + entry.name);
    }
    const std::uint32_t stored_bytes = (entry.packed != 0) ? entry.packed : entry.unpacked;
    Bytes buffer(stored_bytes);
    if (stored_bytes > 0) {
        archive_stream.read(reinterpret_cast<char*>(buffer.data()),
                            static_cast<std::streamsize>(stored_bytes));
        if (archive_stream.gcount() != static_cast<std::streamsize>(stored_bytes)) {
            throw std::runtime_error("Truncated read for DDP2 entry " + entry.name);
        }
    }
    if (entry.packed != 0) {
        return ddp::shs_decompress(buffer, entry.unpacked);
    }
    return buffer;
}

static void ValidateReplacementTgaAgainstOriginal(const Bytes& original_bytes,
                                                  const Bytes& replacement_bytes,
                                                  const std::string& entry_label) {
    const TgaHeaderInfo original_header = ReadTgaHeader(original_bytes);
    const TgaHeaderInfo replacement_header = ReadTgaHeader(replacement_bytes);

    if (replacement_header.width != original_header.width ||
        replacement_header.height != original_header.height) {
        throw std::runtime_error(
            "TGA dimension mismatch for " + entry_label + ": expected " +
            std::to_string(original_header.width) + "x" + std::to_string(original_header.height) +
            ", got " + std::to_string(replacement_header.width) + "x" +
            std::to_string(replacement_header.height));
    }
    if (replacement_header.bpp != original_header.bpp) {
        throw std::runtime_error("TGA bit-depth mismatch for " + entry_label + ": expected " +
                                 std::to_string(original_header.bpp) + ", got " +
                                 std::to_string(replacement_header.bpp));
    }
    if (replacement_header.descriptor != original_header.descriptor) {
        throw std::runtime_error("TGA descriptor mismatch for " + entry_label + ": expected " +
                                 std::to_string(original_header.descriptor) + ", got " +
                                 std::to_string(replacement_header.descriptor));
    }

    const Bytes decoded_pixels = DecodeTgaPixels(replacement_bytes);
    const std::size_t expected_pixel_bytes =
        static_cast<std::size_t>(original_header.width) *
        static_cast<std::size_t>(original_header.height) *
        (static_cast<std::size_t>(original_header.bpp) / kBitsPerByte);
    if (decoded_pixels.size() != expected_pixel_bytes) {
        throw std::runtime_error("Decoded TGA pixel buffer size mismatch for " + entry_label);
    }
}

static Bytes PrepareBootupReplacementPayload(const DiscoveredAsset& asset) {
    Bytes payload = read_file(asset.file_path);
    if (payload.empty()) {
        throw std::runtime_error("Replacement Bootup asset is empty: " + asset.file_path);
    }
    if (asset.kind != AssetPayloadKind::kDecryptedHxb) {
        return payload;
    }
    if (payload.size() < kHxbMinHeaderBytes ||
        std::memcmp(payload.data(), "DDWuHXB", kHxbMagicLength) != 0) {
        throw std::runtime_error("Invalid DDWuHXB header in " + asset.file_path);
    }
    const std::size_t total_len = payload.size();
    payload[kHxbLengthHighOffset] =
        static_cast<std::uint8_t>((total_len >> kTwoBytesShift) & kLowByteMask);
    payload[kHxbLengthMidOffset] =
        static_cast<std::uint8_t>((total_len >> kByteShift) & kLowByteMask);
    payload[kHxbLengthLowOffset] = static_cast<std::uint8_t>(total_len & kLowByteMask);
    return ddp::decrypt_hxb(payload);
}

static void RepackAtomicallyFromBackup(const fs::path& backup_path,
                                       const fs::path& target_path,
                                       const ddp::Replacements& replacements) {
    const fs::path temp_path = fs::u8path(target_path.u8string() + ".tmp");
    ddp::repack_ddp2(backup_path.u8string(), temp_path.u8string(), replacements);
    std::error_code ec;
    fs::rename(temp_path, target_path, ec);
    if (!ec) {
        return;
    }
    fs::remove(target_path, ec);
    ec.clear();
    fs::rename(temp_path, target_path, ec);
    if (ec) {
        throw std::runtime_error("Failed to replace archive " + target_path.u8string() + ": " +
                                 ec.message());
    }
}

static void VerifyRepackedDdp2Archive(const fs::path& backup_path,
                                      const fs::path& repacked_path,
                                      const std::map<std::size_t, Bytes>& patched_by_index,
                                      const std::map<std::size_t, AssetPayloadKind>& kind_by_index) {
    const std::vector<ddp::ArchiveEntry> backup_entries = ddp::list_ddp2(backup_path.u8string());
    const std::vector<ddp::ArchiveEntry> repacked_entries =
        ddp::list_ddp2(repacked_path.u8string());
    if (backup_entries.size() != repacked_entries.size()) {
        throw std::runtime_error("Entry count mismatch after repacking " +
                                 repacked_path.u8string());
    }

    std::ifstream backup_stream(backup_path, std::ios::binary);
    std::ifstream repacked_stream(repacked_path, std::ios::binary);
    if (!backup_stream || !repacked_stream) {
        throw std::runtime_error("Cannot open archives for verification: " +
                                 repacked_path.u8string());
    }

    for (std::size_t i = 0; i < repacked_entries.size(); ++i) {
        const Bytes actual_payload = ReadDecompressedDdp2Entry(repacked_stream, repacked_entries[i]);
        const auto patched_it = patched_by_index.find(i);
        if (patched_it != patched_by_index.end()) {
            if (actual_payload != patched_it->second) {
                throw std::runtime_error("Patched payload mismatch at entry index " +
                                         std::to_string(i) + " in " + repacked_path.u8string());
            }
            const auto kind_it = kind_by_index.find(i);
            if (kind_it != kind_by_index.end() && kind_it->second == AssetPayloadKind::kTga) {
                const Bytes orig_payload =
                    ReadDecompressedDdp2Entry(backup_stream, backup_entries[i]);
                ValidateReplacementTgaAgainstOriginal(
                    orig_payload, actual_payload, repacked_entries[i].name);
            }
            continue;
        }

        const Bytes expected_unpatched =
            ReadDecompressedDdp2Entry(backup_stream, backup_entries[i]);
        if (actual_payload != expected_unpatched) {
            throw std::runtime_error("Untouched entry corrupted at index " + std::to_string(i) +
                                     " in " + repacked_path.u8string());
        }
    }
}

static fs::path ResolveSinSysdPath(const fs::path& game_root) {
    const fs::path data_subdir_path = game_root / "Data" / "sin_sysd.dat";
    const fs::path data_subdir_bak = game_root / "Data" / "sin_sysd.dat.bak";
    if (fs::exists(data_subdir_path) || fs::exists(data_subdir_bak)) {
        return data_subdir_path;
    }
    const fs::path direct_path = game_root / "sin_sysd.dat";
    const fs::path direct_bak = game_root / "sin_sysd.dat.bak";
    if (fs::exists(direct_path) || fs::exists(direct_bak)) {
        return direct_path;
    }
    return data_subdir_path;
}

static void UnmaskAndCollectXorRecords(
    const Bytes& orig_sin_sysd,
    const std::vector<Ddp2EntryMeta>& sin_sysd_entries,
    const Bytes& orig_bootup,
    const std::vector<Ddp2EntryMeta>& bootup_entries,
    const Bytes& patch_blob,
    std::map<std::size_t, Bytes>& out_sin_sysd_replacements,
    std::map<std::size_t, Bytes>& out_bootup_replacements) {
    out_sin_sysd_replacements.clear();
    out_bootup_replacements.clear();

    if (patch_blob.size() < kXorMenuPatchHeaderSize ||
        std::memcmp(patch_blob.data(), kXorMenuPatchMagic, kXorMenuPatchMagicSize) != 0) {
        throw std::runtime_error("Invalid SHINMENU patch magic or header size");
    }

    const std::uint32_t format_version = ReadU32Le(patch_blob, kPatchVersionOffset);
    if (format_version != kXorMenuPatchFormatVersion) {
        throw std::runtime_error("Unsupported SHINMENU patch format version: " +
                                 std::to_string(format_version));
    }

    const std::uint32_t sin_sysd_total = ReadU32Le(patch_blob, kPatchSinSysdTotalOffset);
    const std::uint32_t sin_sysd_orig_size = ReadU32Le(patch_blob, kPatchSinSysdOrigSizeOffset);
    const std::uint32_t sin_sysd_patch_count = ReadU32Le(patch_blob, kPatchSinSysdCountOffset);
    const std::uint32_t bootup_total = ReadU32Le(patch_blob, kPatchBootupTotalOffset);
    const std::uint32_t bootup_orig_size = ReadU32Le(patch_blob, kPatchBootupOrigSizeOffset);
    const std::uint32_t bootup_patch_count = ReadU32Le(patch_blob, kPatchBootupCountOffset);

    if (sin_sysd_total == 0 ||
        sin_sysd_entries.size() != sin_sysd_total ||
        orig_sin_sysd.size() != sin_sysd_orig_size) {
        throw std::runtime_error("sin_sysd.dat metadata mismatch against SHINMENU header");
    }

    const bool expects_bootup =
        (bootup_total > 0) || (bootup_orig_size > 0) || (bootup_patch_count > 0);
    if (expects_bootup) {
        if (bootup_total == 0 ||
            bootup_entries.size() != bootup_total ||
            orig_bootup.size() != bootup_orig_size) {
            throw std::runtime_error("Bootup.dat metadata mismatch against SHINMENU header");
        }
    }

    const std::size_t total_records =
        static_cast<std::size_t>(sin_sysd_patch_count) +
        static_cast<std::size_t>(bootup_patch_count);
    std::size_t cursor = kXorMenuPatchHeaderSize;

    for (std::size_t rec = 0; rec < total_records; ++rec) {
        if (cursor + kXorMenuPatchRecordHeaderSize > patch_blob.size()) {
            throw std::runtime_error("Truncated SHINMENU record header");
        }
        const std::uint32_t archive_id = ReadU32Le(patch_blob, cursor + kRecordArchiveIdOffset);
        const std::uint32_t entry_index = ReadU32Le(patch_blob, cursor + kRecordEntryIndexOffset);
        const std::uint32_t orig_stored_hash =
            ReadU32Le(patch_blob, cursor + kRecordOrigHashOffset);
        const std::uint32_t new_unpacked_size =
            ReadU32Le(patch_blob, cursor + kRecordNewSizeOffset);
        const std::uint32_t new_unpacked_hash =
            ReadU32Le(patch_blob, cursor + kRecordNewHashOffset);
        cursor += kXorMenuPatchRecordHeaderSize;

        if (new_unpacked_size == 0 || cursor + new_unpacked_size > patch_blob.size()) {
            throw std::runtime_error("Invalid or truncated SHINMENU record payload");
        }

        const bool is_sin_sysd_slot = (rec < sin_sysd_patch_count);
        const std::uint32_t expected_archive_id =
            is_sin_sysd_slot ? kXorMenuArchiveIdSinSysd : kXorMenuArchiveIdBootup;
        if (archive_id != expected_archive_id) {
            throw std::runtime_error("Unexpected archive_id in SHINMENU record");
        }

        const Bytes& target_orig_archive = is_sin_sysd_slot ? orig_sin_sysd : orig_bootup;
        const std::vector<Ddp2EntryMeta>& target_entries =
            is_sin_sysd_slot ? sin_sysd_entries : bootup_entries;
        std::map<std::size_t, Bytes>& target_map =
            is_sin_sysd_slot ? out_sin_sysd_replacements : out_bootup_replacements;

        if (entry_index >= target_entries.size()) {
            throw std::runtime_error("SHINMENU entry_index out of bounds");
        }

        const Ddp2EntryMeta& entry = target_entries[entry_index];
        const std::size_t stored_len =
            ComputeStoredEntryLength(target_orig_archive, entry, "SHINMENU target");
        const std::uint8_t* orig_stored_ptr = target_orig_archive.data() + entry.offset;
        const std::uint32_t actual_orig_hash =
            ComputeMenuMurmurHash2(orig_stored_ptr, stored_len);
        if (actual_orig_hash != orig_stored_hash) {
            throw std::runtime_error("Original entry MurmurHash2 mismatch at index " +
                                     std::to_string(entry_index));
        }

        Bytes unmasked(new_unpacked_size);
        for (std::size_t k = 0; k < new_unpacked_size; ++k) {
            const std::uint8_t mask =
                ComputeXorMaskByte(orig_stored_ptr, stored_len, orig_stored_hash, k);
            unmasked[k] = static_cast<std::uint8_t>(patch_blob[cursor + k] ^ mask);
        }
        cursor += new_unpacked_size;

        const std::uint32_t actual_new_hash =
            ComputeMenuMurmurHash2(unmasked.data(), unmasked.size());
        if (actual_new_hash != new_unpacked_hash) {
            throw std::runtime_error("Unmasked replacement MurmurHash2 mismatch at index " +
                                     std::to_string(entry_index));
        }

        const auto [it, inserted] =
            target_map.emplace(static_cast<std::size_t>(entry_index), std::move(unmasked));
        if (!inserted) {
            throw std::runtime_error("Duplicate entry_index in SHINMENU patch blob");
        }
    }

    if (cursor != patch_blob.size()) {
        throw std::runtime_error("Trailing bytes at end of SHINMENU patch blob");
    }
}

}  // namespace

TgaHeaderInfo ReadTgaHeader(const Bytes& data) {
    if (data.size() < kTgaHeaderByteCount) {
        throw std::runtime_error("TGA buffer shorter than 18-byte header");
    }
    TgaHeaderInfo header;
    header.id_length = data[kTgaIdLengthOffset];
    header.color_map_type = data[kTgaColorMapTypeOffset];
    header.image_type = data[kTgaImageTypeOffset];
    header.color_map_first_entry = ReadU16Le(data, kTgaColorMapFirstEntryOffset);
    header.color_map_length = ReadU16Le(data, kTgaColorMapLengthOffset);
    header.color_map_entry_size = data[kTgaColorMapEntrySizeOffset];
    header.x_origin = ReadU16Le(data, kTgaXOriginOffset);
    header.y_origin = ReadU16Le(data, kTgaYOriginOffset);
    header.width = ReadU16Le(data, kTgaWidthOffset);
    header.height = ReadU16Le(data, kTgaHeightOffset);
    header.bpp = data[kTgaBppOffset];
    header.descriptor = data[kTgaDescriptorOffset];

    ValidateTgaHeaderFields(header);
    const std::size_t pixel_data_offset =
        kTgaHeaderByteCount + static_cast<std::size_t>(header.id_length);
    if (data.size() < pixel_data_offset) {
        throw std::runtime_error("TGA buffer truncated inside image ID field");
    }
    return header;
}

TgaHeaderInfo ReadTgaInfo(const Bytes& data) {
    return ReadTgaHeader(data);
}

Bytes DecodeTgaPixels(const Bytes& data) {
    const TgaHeaderInfo header = ReadTgaHeader(data);
    const std::size_t bytes_per_pixel = static_cast<std::size_t>(header.bpp) / kBitsPerByte;
    const std::size_t total_pixels =
        static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height);
    const std::size_t expected_pixel_bytes = total_pixels * bytes_per_pixel;
    std::size_t cursor = kTgaHeaderByteCount + static_cast<std::size_t>(header.id_length);

    if (header.image_type == kTgaImageTypeUncompressedTrueColor) {
        if (cursor + expected_pixel_bytes > data.size()) {
            throw std::runtime_error("Uncompressed TGA pixel data is truncated");
        }
        return Bytes(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                     data.begin() + static_cast<std::ptrdiff_t>(cursor + expected_pixel_bytes));
    }

    Bytes decoded;
    decoded.reserve(expected_pixel_bytes);
    while (decoded.size() < expected_pixel_bytes) {
        if (cursor >= data.size()) {
            throw std::runtime_error("RLE TGA stream ended before all pixels were decoded");
        }
        const std::uint8_t packet_header = data[cursor];
        ++cursor;
        const std::size_t run_length =
            static_cast<std::size_t>(packet_header & kTgaRleCountMask) + kTgaRleCountBias;
        const std::size_t run_byte_count = run_length * bytes_per_pixel;
        if (decoded.size() + run_byte_count > expected_pixel_bytes) {
            throw std::runtime_error("RLE TGA packet overflows image pixel capacity");
        }

        const bool is_rle_packet = (packet_header & kTgaRlePacketMask) != 0;
        if (is_rle_packet) {
            if (cursor + bytes_per_pixel > data.size()) {
                throw std::runtime_error("RLE TGA repeat packet truncated");
            }
            for (std::size_t i = 0; i < run_length; ++i) {
                decoded.insert(decoded.end(),
                               data.begin() + static_cast<std::ptrdiff_t>(cursor),
                               data.begin() +
                                   static_cast<std::ptrdiff_t>(cursor + bytes_per_pixel));
            }
            cursor += bytes_per_pixel;
            continue;
        }

        if (cursor + run_byte_count > data.size()) {
            throw std::runtime_error("RLE TGA literal packet truncated");
        }
        decoded.insert(decoded.end(),
                       data.begin() + static_cast<std::ptrdiff_t>(cursor),
                       data.begin() + static_cast<std::ptrdiff_t>(cursor + run_byte_count));
        cursor += run_byte_count;
    }

    return decoded;
}

Bytes EncodeTga(const TgaHeaderInfo& header, const Bytes& pixels) {
    ValidateTgaHeaderFields(header);
    if (header.id_length != 0) {
        throw std::runtime_error("EncodeTga requires id_length == 0");
    }
    const std::size_t bytes_per_pixel = static_cast<std::size_t>(header.bpp) / kBitsPerByte;
    const std::size_t expected_pixel_bytes =
        static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height) *
        bytes_per_pixel;
    if (pixels.size() != expected_pixel_bytes) {
        throw std::runtime_error("EncodeTga pixel buffer size does not match header dimensions");
    }

    Bytes out;
    out.reserve(kTgaHeaderByteCount + expected_pixel_bytes);
    out.push_back(header.id_length);
    out.push_back(header.color_map_type);
    out.push_back(header.image_type);
    WriteU16Le(out, header.color_map_first_entry);
    WriteU16Le(out, header.color_map_length);
    out.push_back(header.color_map_entry_size);
    WriteU16Le(out, header.x_origin);
    WriteU16Le(out, header.y_origin);
    WriteU16Le(out, header.width);
    WriteU16Le(out, header.height);
    out.push_back(header.bpp);
    out.push_back(header.descriptor);

    if (header.image_type == kTgaImageTypeUncompressedTrueColor) {
        out.insert(out.end(), pixels.begin(), pixels.end());
        return out;
    }

    const std::size_t width = static_cast<std::size_t>(header.width);
    const std::size_t height = static_cast<std::size_t>(header.height);
    for (std::size_t row = 0; row < height; ++row) {
        EncodeTgaScanlineRle(pixels, row * width, width, bytes_per_pixel, out);
    }
    return out;
}

std::uint32_t ComputeMenuMurmurHash2(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h = static_cast<std::uint32_t>(len);
    std::size_t i = 0;
    while (i + kWordBytes <= len) {
        std::uint32_t k = ReadU32LePtr(data + i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }
    const std::size_t rem = len - i;
    if (rem == kRemThreeBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset2]) << kTwoBytesShift;
    }
    if (rem >= kRemTwoBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset1]) << kByteShift;
    }
    if (rem >= kRemOneByte) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

Bytes BuildXorMenuPatchBlob(const Bytes& orig_sin_sysd,
                            const std::map<std::size_t, Bytes>& sin_sysd_replacements,
                            const Bytes& orig_bootup,
                            const std::map<std::size_t, Bytes>& bootup_replacements) {
    const std::vector<Ddp2EntryMeta> sin_sysd_entries =
        ParseDdp2EntriesFromBytes(orig_sin_sysd, "sin_sysd.dat");
    std::vector<Ddp2EntryMeta> bootup_entries;
    if (!orig_bootup.empty()) {
        bootup_entries = ParseDdp2EntriesFromBytes(orig_bootup, "Bootup.dat");
    }
    if (orig_bootup.empty() && !bootup_replacements.empty()) {
        throw std::runtime_error("Cannot build Bootup XOR records when orig_bootup is empty");
    }

    Bytes blob;
    blob.insert(blob.end(), kXorMenuPatchMagic, kXorMenuPatchMagic + kXorMenuPatchMagicSize);
    WriteU32Le(blob, kXorMenuPatchFormatVersion);
    WriteU32Le(blob, static_cast<std::uint32_t>(sin_sysd_entries.size()));
    WriteU32Le(blob, static_cast<std::uint32_t>(orig_sin_sysd.size()));
    WriteU32Le(blob, static_cast<std::uint32_t>(sin_sysd_replacements.size()));
    WriteU32Le(blob, static_cast<std::uint32_t>(bootup_entries.size()));
    WriteU32Le(blob, static_cast<std::uint32_t>(orig_bootup.size()));
    WriteU32Le(blob, static_cast<std::uint32_t>(bootup_replacements.size()));

    AppendXorRecordsForArchive(orig_sin_sysd,
                               sin_sysd_entries,
                               kXorMenuArchiveIdSinSysd,
                               sin_sysd_replacements,
                               "sin_sysd.dat",
                               blob);
    if (!orig_bootup.empty()) {
        AppendXorRecordsForArchive(orig_bootup,
                                   bootup_entries,
                                   kXorMenuArchiveIdBootup,
                                   bootup_replacements,
                                   "Bootup.dat",
                                   blob);
    }

    return blob;
}

void ApplyXorMenuPatchBlobToDdp2(const Bytes& orig_sin_sysd,
                                 const Bytes& orig_bootup,
                                 const Bytes& patch_blob,
                                 Bytes& out_patched_sin_sysd,
                                 Bytes& out_patched_bootup) {
    out_patched_sin_sysd.clear();
    out_patched_bootup.clear();

    const std::vector<Ddp2EntryMeta> sin_sysd_entries =
        ParseDdp2EntriesFromBytes(orig_sin_sysd, "sin_sysd.dat");
    std::vector<Ddp2EntryMeta> bootup_entries;
    if (!orig_bootup.empty()) {
        bootup_entries = ParseDdp2EntriesFromBytes(orig_bootup, "Bootup.dat");
    }

    std::map<std::size_t, Bytes> sin_sysd_replacements;
    std::map<std::size_t, Bytes> bootup_replacements;
    UnmaskAndCollectXorRecords(orig_sin_sysd,
                               sin_sysd_entries,
                               orig_bootup,
                               bootup_entries,
                               patch_blob,
                               sin_sysd_replacements,
                               bootup_replacements);

    out_patched_sin_sysd =
        RebuildDdp2InMemory(orig_sin_sysd, sin_sysd_entries, sin_sysd_replacements);
    if (!orig_bootup.empty()) {
        out_patched_bootup =
            RebuildDdp2InMemory(orig_bootup, bootup_entries, bootup_replacements);
    }
}

struct MenuArchivePatcher::Impl {
    MenuPatchOptions options;

    explicit Impl(MenuPatchOptions opts) : options(std::move(opts)) {}

    MenuPatchStats ExecuteFromPatchFile(const fs::path& sin_sysd_path,
                                        const fs::path& sin_sysd_bak_path,
                                        const fs::path& bootup_path,
                                        const fs::path& bootup_bak_path,
                                        bool has_bootup) {
        const Bytes orig_sin_sysd = read_file(sin_sysd_bak_path.u8string());
        const Bytes orig_bootup = has_bootup ? read_file(bootup_bak_path.u8string()) : Bytes{};
        const Bytes patch_blob = read_file(options.patch_file);

        const std::vector<Ddp2EntryMeta> sin_sysd_entries =
            ParseDdp2EntriesFromBytes(orig_sin_sysd, sin_sysd_bak_path.u8string());
        std::vector<Ddp2EntryMeta> bootup_entries;
        if (has_bootup) {
            bootup_entries = ParseDdp2EntriesFromBytes(orig_bootup, bootup_bak_path.u8string());
        }

        std::map<std::size_t, Bytes> sin_sysd_payloads;
        std::map<std::size_t, Bytes> bootup_payloads;
        UnmaskAndCollectXorRecords(orig_sin_sysd,
                                   sin_sysd_entries,
                                   orig_bootup,
                                   bootup_entries,
                                   patch_blob,
                                   sin_sysd_payloads,
                                   bootup_payloads);

        const Bytes patched_sin_sysd =
            RebuildDdp2InMemory(orig_sin_sysd, sin_sysd_entries, sin_sysd_payloads);
        WriteBytesAtomically(sin_sysd_path, patched_sin_sysd);

        if (has_bootup) {
            const Bytes patched_bootup =
                RebuildDdp2InMemory(orig_bootup, bootup_entries, bootup_payloads);
            WriteBytesAtomically(bootup_path, patched_bootup);
        }

        MenuPatchStats stats;
        stats.sin_sysd_total_entries = sin_sysd_entries.size();
        stats.bootup_total_entries = bootup_entries.size();
        stats.bootup_patched_count = bootup_payloads.size();

        std::map<std::size_t, AssetPayloadKind> sin_sysd_kinds;
        std::map<std::size_t, AssetPayloadKind> bootup_kinds;
        for (const auto& [entry_index, payload] : sin_sysd_payloads) {
            if (IsValidTgaPayload(payload)) {
                ++stats.sin_sysd_patched_tga_count;
                sin_sysd_kinds.emplace(entry_index, AssetPayloadKind::kTga);
                continue;
            }
            ++stats.sin_sysd_patched_csv_count;
            sin_sysd_kinds.emplace(entry_index, AssetPayloadKind::kCsv);
        }
        for (const auto& [entry_index, payload] : bootup_payloads) {
            bootup_kinds.emplace(entry_index, AssetPayloadKind::kRaw);
        }

        if (options.verify) {
            VerifyRepackedDdp2Archive(
                sin_sysd_bak_path, sin_sysd_path, sin_sysd_payloads, sin_sysd_kinds);
            if (has_bootup) {
                VerifyRepackedDdp2Archive(
                    bootup_bak_path, bootup_path, bootup_payloads, bootup_kinds);
            }
            stats.verified = true;
        }

        return stats;
    }

    MenuPatchStats Execute() {
        const fs::path game_root = fs::u8path(options.game_dir);
        const fs::path sin_sysd_path = ResolveSinSysdPath(game_root);
        const fs::path sin_sysd_bak_path = fs::u8path(sin_sysd_path.u8string() + ".bak");
        EnsureBackupFileExists(sin_sysd_path, sin_sysd_bak_path);

        const fs::path bootup_path = game_root / "Bootup.dat";
        const fs::path bootup_bak_path = game_root / "Bootup.dat.bak";
        const bool has_bootup = fs::exists(bootup_path) || fs::exists(bootup_bak_path);
        if (has_bootup) {
            EnsureBackupFileExists(bootup_path, bootup_bak_path);
        }

        const bool has_assets_dir =
            !options.assets_dir.empty() && fs::exists(fs::u8path(options.assets_dir));
        const bool has_patch_file =
            !options.patch_file.empty() && fs::exists(fs::u8path(options.patch_file));
        if (options.build_patch_out.empty() && !has_assets_dir && has_patch_file) {
            return ExecuteFromPatchFile(
                sin_sysd_path, sin_sysd_bak_path, bootup_path, bootup_bak_path, has_bootup);
        }

        const std::vector<DiscoveredAsset> discovered =
            DiscoverAssetsInDirectory(options.assets_dir);

        const std::vector<ddp::ArchiveEntry> sin_sysd_entries =
            ddp::list_ddp2(sin_sysd_bak_path.u8string());
        std::ifstream sin_sysd_bak_stream(sin_sysd_bak_path, std::ios::binary);
        if (!sin_sysd_bak_stream) {
            throw std::runtime_error("Cannot open backup archive: " +
                                     sin_sysd_bak_path.u8string());
        }

        std::map<std::size_t, Bytes> sin_sysd_payloads;
        std::map<std::size_t, AssetPayloadKind> sin_sysd_kinds;
        std::map<std::size_t, Bytes> bootup_payloads;
        std::map<std::size_t, AssetPayloadKind> bootup_kinds;

        MenuPatchStats stats;
        stats.sin_sysd_total_entries = sin_sysd_entries.size();

        std::vector<ddp::ArchiveEntry> bootup_entries;
        if (has_bootup) {
            bootup_entries = ddp::list_ddp2(bootup_bak_path.u8string());
            stats.bootup_total_entries = bootup_entries.size();
        }

        for (const DiscoveredAsset& asset : discovered) {
            if (asset.archive == AssetTargetArchive::kSinSysd) {
                if (asset.entry_index >= sin_sysd_entries.size()) {
                    throw std::runtime_error("sin_sysd replacement index out of range: " +
                                             std::to_string(asset.entry_index));
                }
                if (sin_sysd_payloads.count(asset.entry_index) != 0) {
                    throw std::runtime_error("Duplicate replacement for sin_sysd index " +
                                             std::to_string(asset.entry_index));
                }
                Bytes replacement_bytes = read_file(asset.file_path);
                if (replacement_bytes.empty()) {
                    throw std::runtime_error("Replacement sin_sysd asset is empty: " +
                                             asset.file_path);
                }
                if (asset.kind == AssetPayloadKind::kTga) {
                    const Bytes orig_bytes = ReadDecompressedDdp2Entry(
                        sin_sysd_bak_stream, sin_sysd_entries[asset.entry_index]);
                    ValidateReplacementTgaAgainstOriginal(
                        orig_bytes, replacement_bytes, asset.file_path);
                    ++stats.sin_sysd_patched_tga_count;
                }
                if (asset.kind == AssetPayloadKind::kCsv) {
                    ++stats.sin_sysd_patched_csv_count;
                }
                if (asset.kind == AssetPayloadKind::kRaw) {
                    ++stats.sin_sysd_patched_raw_count;
                }
                sin_sysd_payloads.emplace(asset.entry_index, std::move(replacement_bytes));
                sin_sysd_kinds.emplace(asset.entry_index, asset.kind);
                continue;
            }

            if (!has_bootup) {
                throw std::runtime_error(
                    "Found Bootup replacement asset, but Bootup.dat does not exist in " +
                    options.game_dir);
            }
            if (asset.entry_index >= bootup_entries.size()) {
                throw std::runtime_error("Bootup replacement index out of range: " +
                                         std::to_string(asset.entry_index));
            }
            if (bootup_payloads.count(asset.entry_index) != 0) {
                throw std::runtime_error("Duplicate replacement for Bootup index " +
                                         std::to_string(asset.entry_index));
            }
            Bytes bootup_bytes = PrepareBootupReplacementPayload(asset);
            bootup_payloads.emplace(asset.entry_index, std::move(bootup_bytes));
            bootup_kinds.emplace(asset.entry_index, asset.kind);
            ++stats.bootup_patched_count;
        }
        sin_sysd_bak_stream.close();

        if (!options.build_patch_out.empty()) {
            const Bytes orig_sin_sysd = read_file(sin_sysd_bak_path.u8string());
            const Bytes orig_bootup =
                has_bootup ? read_file(bootup_bak_path.u8string()) : Bytes{};
            const Bytes patch_blob = BuildXorMenuPatchBlob(
                orig_sin_sysd, sin_sysd_payloads, orig_bootup, bootup_payloads);
            Bytes verify_sin_sysd;
            Bytes verify_bootup;
            ApplyXorMenuPatchBlobToDdp2(
                orig_sin_sysd, orig_bootup, patch_blob, verify_sin_sysd, verify_bootup);
            write_file(options.build_patch_out, patch_blob);
        }

        ddp::Replacements sin_sysd_replacements;
        sin_sysd_replacements.reserve(sin_sysd_payloads.size());
        for (const auto& [entry_index, payload] : sin_sysd_payloads) {
            sin_sysd_replacements.emplace_back(sin_sysd_entries[entry_index].name, payload);
        }
        RepackAtomicallyFromBackup(sin_sysd_bak_path, sin_sysd_path, sin_sysd_replacements);

        if (has_bootup) {
            ddp::Replacements bootup_replacements;
            bootup_replacements.reserve(bootup_payloads.size());
            for (const auto& [entry_index, payload] : bootup_payloads) {
                bootup_replacements.emplace_back(bootup_entries[entry_index].name, payload);
            }
            RepackAtomicallyFromBackup(bootup_bak_path, bootup_path, bootup_replacements);
        }

        if (options.verify) {
            VerifyRepackedDdp2Archive(
                sin_sysd_bak_path, sin_sysd_path, sin_sysd_payloads, sin_sysd_kinds);
            if (has_bootup) {
                VerifyRepackedDdp2Archive(
                    bootup_bak_path, bootup_path, bootup_payloads, bootup_kinds);
            }
            stats.verified = true;
        }

        return stats;
    }
};

MenuArchivePatcher::MenuArchivePatcher(MenuPatchOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

MenuArchivePatcher::MenuArchivePatcher(MenuArchivePatcher&& other) noexcept = default;

MenuArchivePatcher& MenuArchivePatcher::operator=(MenuArchivePatcher&& other) noexcept = default;

MenuArchivePatcher::~MenuArchivePatcher() = default;

MenuPatchStats MenuArchivePatcher::PatchArchives() {
    return impl_->Execute();
}

const MenuPatchOptions& MenuArchivePatcher::options() const noexcept {
    return impl_->options;
}

}  // namespace shin::menu
