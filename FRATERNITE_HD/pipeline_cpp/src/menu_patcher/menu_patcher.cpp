// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_patcher/menu_patcher.h"

#include "yuris/ypf_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zlib.h>

namespace frat::menu {

namespace {

constexpr std::size_t kPngSignatureSize = 8u;
constexpr std::uint8_t kPngSignature[kPngSignatureSize] = {
    0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
constexpr std::size_t kPngIhdrLengthOffset = 8u;
constexpr std::size_t kPngIhdrTypeOffset = 12u;
constexpr std::size_t kPngIhdrWidthOffset = 16u;
constexpr std::size_t kPngIhdrHeightOffset = 20u;
constexpr std::size_t kPngIhdrBitDepthOffset = 24u;
constexpr std::size_t kPngIhdrColorTypeOffset = 25u;
constexpr std::size_t kMinPngHeaderBytes = 26u;
constexpr std::uint32_t kExpectedIhdrDataLength = 13u;
constexpr std::size_t kChunkTypeLength = 4u;

constexpr std::size_t kByteOffset1 = 1u;
constexpr std::size_t kByteOffset2 = 2u;
constexpr std::size_t kByteOffset3 = 3u;
constexpr int kShiftByte1 = 8;
constexpr int kShiftByte2 = 16;
constexpr int kShiftByte3 = 24;
constexpr int kBitsPerByte = 8;
constexpr std::size_t kU32Bytes = 4u;
constexpr std::size_t kU64Bytes = 8u;
constexpr int kZlibBestCompression = 9;

constexpr std::size_t kPatchMagicSize = 8u;
constexpr char kPatchMagic[kPatchMagicSize] = {'F', 'R', 'A', 'T', 'M', 'E', 'N', 'U'};
constexpr std::uint32_t kPatchFormatVersion = 1u;
constexpr std::size_t kPatchHeaderSize = 24u;
constexpr std::size_t kPatchVersionOffset = 8u;
constexpr std::size_t kPatchTotalEntriesOffset = 12u;
constexpr std::size_t kPatchOrigSizeOffset = 16u;
constexpr std::size_t kPatchEntryCountOffset = 20u;
constexpr std::size_t kPatchRecordHeaderSize = 16u;
constexpr std::size_t kRecordOrigHashOffset = 4u;
constexpr std::size_t kRecordNewSizeOffset = 8u;
constexpr std::size_t kRecordNewHashOffset = 12u;
constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kXorStreamAddend = 0x37u;
constexpr std::uint32_t kHashByteIndexMask = 3u;
constexpr std::uint32_t kLowByteMask = 0xFFu;

std::uint32_t rd_le_u32(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + kByteOffset1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(b[off + kByteOffset2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(b[off + kByteOffset3]) << kShiftByte3);
}

std::uint8_t compute_xor_mask_byte(const Bytes& orig_payload,
                                   std::uint32_t orig_data_hash,
                                   std::size_t byte_index) {
    const std::uint32_t idx32 = static_cast<std::uint32_t>(byte_index);
    const std::uint32_t shift = (idx32 & kHashByteIndexMask) * static_cast<std::uint32_t>(kBitsPerByte);
    const std::uint8_t hash_byte =
        static_cast<std::uint8_t>((orig_data_hash >> shift) & kLowByteMask);
    const std::uint8_t stream_byte =
        static_cast<std::uint8_t>((idx32 * kXorStreamMultiplier + kXorStreamAddend) & kLowByteMask);
    return static_cast<std::uint8_t>(
        orig_payload[byte_index % orig_payload.size()] ^ hash_byte ^ stream_byte);
}

std::uint32_t rd_be_u32(const Bytes& b, std::size_t off) {
    return (static_cast<std::uint32_t>(b[off]) << kShiftByte3) |
           (static_cast<std::uint32_t>(b[off + kByteOffset1]) << kShiftByte2) |
           (static_cast<std::uint32_t>(b[off + kByteOffset2]) << kShiftByte1) |
           static_cast<std::uint32_t>(b[off + kByteOffset3]);
}

void append_le_u32(Bytes& out, std::uint32_t v) {
    for (std::size_t i = 0; i < kU32Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

void append_le_u64(Bytes& out, std::uint64_t v) {
    for (std::size_t i = 0; i < kU64Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

Bytes zlib_deflate_bytes(const Bytes& in) {
    uLongf bound = ::compressBound(static_cast<uLong>(in.size()));
    Bytes out(bound);
    const int rc = ::compress2(out.data(), &bound, in.data(),
                               static_cast<uLong>(in.size()), kZlibBestCompression);
    if (rc != Z_OK) {
        throw std::runtime_error("zlib deflate failed while repacking YPF entry");
    }
    out.resize(bound);
    return out;
}

void write_bytes_atomic(const std::string& path, const Bytes& content) {
    const std::string tmp = path + ".tmp";
    write_file(tmp, content);
    std::filesystem::rename(std::filesystem::u8path(tmp),
                            std::filesystem::u8path(path));
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string normalize_archive_key(const std::string& rel_path) {
    std::string norm = rel_path;
    std::replace(norm.begin(), norm.end(), '/', '\\');
    const std::string lower = to_lower_ascii(norm);
    if (lower.rfind("cgsys\\", 0) == 0) {
        return norm;
    }
    const std::size_t pos = lower.find("\\cgsys\\");
    if (pos != std::string::npos) {
        return norm.substr(pos + 1);
    }
    return norm;
}

struct PayloadBlock {
    std::uint64_t orig_offset = 0;
    std::uint32_t orig_packed_size = 0;
    std::vector<std::size_t> entry_indices;
    Bytes trailing_bytes;
};

}  // namespace

PngHeaderInfo parse_png_ihdr(const Bytes& png, const std::string& label) {
    if (png.size() < kMinPngHeaderBytes ||
        std::memcmp(png.data(), kPngSignature, kPngSignatureSize) != 0) {
        throw std::runtime_error("invalid PNG signature in " + label);
    }
    const std::uint32_t ihdr_len = rd_be_u32(png, kPngIhdrLengthOffset);
    if (ihdr_len != kExpectedIhdrDataLength ||
        std::memcmp(png.data() + kPngIhdrTypeOffset, "IHDR", kChunkTypeLength) != 0) {
        throw std::runtime_error("missing or malformed PNG IHDR chunk in " + label);
    }
    PngHeaderInfo info;
    info.width = rd_be_u32(png, kPngIhdrWidthOffset);
    info.height = rd_be_u32(png, kPngIhdrHeightOffset);
    info.bit_depth = png[kPngIhdrBitDepthOffset];
    info.color_type = png[kPngIhdrColorTypeOffset];
    if (info.width == 0 || info.height == 0) {
        throw std::runtime_error("zero PNG dimensions in " + label);
    }
    return info;
}

struct MenuArchivePatcher::Impl {
    std::unordered_map<std::string, Bytes> discover_assets(
        const std::string& assets_dir) const {
        std::unordered_map<std::string, Bytes> assets;
        if (assets_dir.empty()) {
            return assets;
        }
        const std::filesystem::path root = std::filesystem::u8path(assets_dir);
        if (!std::filesystem::exists(root)) {
            return assets;
        }
        for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!dir_entry.is_regular_file()) {
                continue;
            }
            const std::filesystem::path p = dir_entry.path();
            if (to_lower_ascii(p.extension().u8string()) != ".png") {
                continue;
            }
            const std::string rel = std::filesystem::relative(p, root).u8string();
            const std::string key = normalize_archive_key(rel);
            const std::string full_path = p.u8string();
            Bytes bytes = read_file(full_path);
            parse_png_ihdr(bytes, full_path);
            const auto [it, inserted] = assets.emplace(key, std::move(bytes));
            if (!inserted) {
                throw std::runtime_error("duplicate replacement PNG for archive entry: " + key);
            }
        }
        return assets;
    }

    Bytes repack_bytes(
        const Bytes& original_archive,
        const std::unordered_map<std::string, Bytes>& replacements,
        PatchSummary* summary) const {
        std::vector<yuris::YpfEntry> entries =
            yuris::parse_ypf_index_bytes(original_archive, "cgsys.ypf");
        if (summary != nullptr) {
            summary->total_entries = entries.size();
            summary->replaced_entries = 0;
        }
        if (entries.empty()) {
            return original_archive;
        }

        std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::size_t>> grouped;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            grouped[{entries[i].data_offset, entries[i].packed_size}].push_back(i);
        }

        std::vector<PayloadBlock> blocks;
        blocks.reserve(grouped.size());
        for (auto& [key, indices] : grouped) {
            PayloadBlock b;
            b.orig_offset = key.first;
            b.orig_packed_size = key.second;
            b.entry_indices = std::move(indices);
            blocks.push_back(std::move(b));
        }

        const std::uint64_t first_payload_offset = blocks.front().orig_offset;
        if (first_payload_offset < yuris::YPF_HEADER_SIZE ||
            first_payload_offset > original_archive.size()) {
            throw std::runtime_error("corrupt YPF payload start offset");
        }

        for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
            const std::uint64_t orig_end =
                blocks[bi].orig_offset + blocks[bi].orig_packed_size;
            const std::uint64_t next_start =
                (bi + 1 < blocks.size()) ? blocks[bi + 1].orig_offset : original_archive.size();
            if (orig_end > next_start || next_start > original_archive.size()) {
                throw std::runtime_error("overlapping or out-of-bounds YPF payload block");
            }
            blocks[bi].trailing_bytes.assign(
                original_archive.begin() + static_cast<std::ptrdiff_t>(orig_end),
                original_archive.begin() + static_cast<std::ptrdiff_t>(next_start));
        }

        Bytes payload_buf;
        payload_buf.reserve(original_archive.size() -
                            static_cast<std::size_t>(first_payload_offset));
        std::size_t replaced_count = 0;

        for (const PayloadBlock& block : blocks) {
            std::vector<std::size_t> unreplaced;
            std::vector<std::size_t> replaced;
            for (const std::size_t idx : block.entry_indices) {
                if (replacements.find(entries[idx].name) != replacements.end()) {
                    replaced.push_back(idx);
                    continue;
                }
                unreplaced.push_back(idx);
            }

            for (const std::size_t idx : replaced) {
                const yuris::YpfEntry& e = entries[idx];
                const Bytes orig_raw = yuris::read_entry(original_archive, e);
                const PngHeaderInfo orig_png = parse_png_ihdr(orig_raw, e.name);
                const Bytes& repl_raw = replacements.at(e.name);
                const PngHeaderInfo repl_png =
                    parse_png_ihdr(repl_raw, "replacement:" + e.name);
                if (repl_png.width != orig_png.width || repl_png.height != orig_png.height) {
                    throw std::runtime_error(
                        "PNG dimension mismatch for " + e.name + ": expected " +
                        std::to_string(orig_png.width) + "x" +
                        std::to_string(orig_png.height) + ", got " +
                        std::to_string(repl_png.width) + "x" +
                        std::to_string(repl_png.height));
                }
                if (repl_png.bit_depth != orig_png.bit_depth ||
                    repl_png.color_type != orig_png.color_type) {
                    throw std::runtime_error(
                        "PNG color format mismatch for " + e.name + ": expected bit_depth=" +
                        std::to_string(orig_png.bit_depth) + " color_type=" +
                        std::to_string(orig_png.color_type) + ", got bit_depth=" +
                        std::to_string(repl_png.bit_depth) + " color_type=" +
                        std::to_string(repl_png.color_type));
                }
            }

            if (!unreplaced.empty()) {
                const std::uint64_t new_off = first_payload_offset + payload_buf.size();
                payload_buf.insert(
                    payload_buf.end(),
                    original_archive.begin() + static_cast<std::ptrdiff_t>(block.orig_offset),
                    original_archive.begin() +
                        static_cast<std::ptrdiff_t>(block.orig_offset + block.orig_packed_size));
                payload_buf.insert(payload_buf.end(), block.trailing_bytes.begin(),
                                   block.trailing_bytes.end());
                for (const std::size_t idx : unreplaced) {
                    entries[idx].data_offset = new_off;
                }
            }

            for (const std::size_t idx : replaced) {
                yuris::YpfEntry& e = entries[idx];
                const Bytes& repl_raw = replacements.at(e.name);
                const Bytes repl_packed =
                    e.compressed ? zlib_deflate_bytes(repl_raw) : repl_raw;
                const std::uint64_t new_off = first_payload_offset + payload_buf.size();

                e.raw_size = static_cast<std::uint32_t>(repl_raw.size());
                e.packed_size = static_cast<std::uint32_t>(repl_packed.size());
                e.data_offset = new_off;
                if (e.data_hash != 0) {
                    e.data_hash = yuris::ypf_murmur_hash2(repl_packed);
                }

                payload_buf.insert(payload_buf.end(), repl_packed.begin(), repl_packed.end());
                payload_buf.insert(payload_buf.end(), block.trailing_bytes.begin(),
                                   block.trailing_bytes.end());
                ++replaced_count;
            }
        }

        Bytes out;
        out.reserve(static_cast<std::size_t>(first_payload_offset) + payload_buf.size());
        out.insert(out.end(), original_archive.begin(),
                   original_archive.begin() +
                       static_cast<std::ptrdiff_t>(yuris::YPF_HEADER_SIZE));

        for (const yuris::YpfEntry& e : entries) {
            append_le_u32(out, e.name_hash);
            out.push_back(e.name_size_raw);
            out.insert(out.end(), e.name_bytes_xor.begin(), e.name_bytes_xor.end());
            out.push_back(static_cast<std::uint8_t>(e.type));
            out.push_back(static_cast<std::uint8_t>(e.compressed));
            append_le_u32(out, e.raw_size);
            append_le_u32(out, e.packed_size);
            append_le_u64(out, e.data_offset);
            append_le_u32(out, e.data_hash);
        }

        if (out.size() < first_payload_offset) {
            out.insert(out.end(),
                       original_archive.begin() + static_cast<std::ptrdiff_t>(out.size()),
                       original_archive.begin() +
                           static_cast<std::ptrdiff_t>(first_payload_offset));
        }
        out.insert(out.end(), payload_buf.begin(), payload_buf.end());

        if (summary != nullptr) {
            summary->replaced_entries = replaced_count;
        }
        return out;
    }

    Bytes build_xor_patch(const Bytes& orig_ypf, const Bytes& patched_ypf) const {
        const std::vector<yuris::YpfEntry> orig_entries =
            yuris::parse_ypf_index_bytes(orig_ypf, "orig.ypf");
        const std::vector<yuris::YpfEntry> patched_entries =
            yuris::parse_ypf_index_bytes(patched_ypf, "patched.ypf");
        if (orig_entries.empty() || orig_entries.size() != patched_entries.size()) {
            throw std::runtime_error("YPF entry count mismatch when building XOR menu patch");
        }

        struct PatchRecord {
            std::uint32_t entry_index = 0;
            std::uint32_t orig_data_hash = 0;
            std::uint32_t new_raw_size = 0;
            std::uint32_t new_data_hash = 0;
            Bytes xor_payload;
        };

        std::vector<PatchRecord> records;
        std::size_t total_payload_bytes = 0;

        for (std::size_t i = 0; i < orig_entries.size(); ++i) {
            const yuris::YpfEntry& oe = orig_entries[i];
            const yuris::YpfEntry& pe = patched_entries[i];
            if (oe.name != pe.name) {
                throw std::runtime_error("YPF entry name order mismatch at index " +
                                         std::to_string(i));
            }
            const Bytes orig_payload = yuris::read_entry(orig_ypf, oe);
            const Bytes patched_payload = yuris::read_entry(patched_ypf, pe);
            if (orig_payload.empty() || patched_payload.empty()) {
                throw std::runtime_error("empty YPF entry payload at index " +
                                         std::to_string(i));
            }
            const PngHeaderInfo orig_ihdr = parse_png_ihdr(orig_payload, oe.name);
            const PngHeaderInfo patched_ihdr = parse_png_ihdr(patched_payload, pe.name);
            if (orig_ihdr.width != patched_ihdr.width ||
                orig_ihdr.height != patched_ihdr.height ||
                orig_ihdr.bit_depth != patched_ihdr.bit_depth ||
                orig_ihdr.color_type != patched_ihdr.color_type) {
                throw std::runtime_error("PNG IHDR mismatch while building XOR patch for " +
                                         oe.name);
            }
            if (patched_payload == orig_payload) {
                continue;
            }

            PatchRecord rec;
            rec.entry_index = static_cast<std::uint32_t>(i);
            rec.orig_data_hash = yuris::ypf_murmur_hash2(orig_payload);
            rec.new_raw_size = static_cast<std::uint32_t>(patched_payload.size());
            rec.new_data_hash = yuris::ypf_murmur_hash2(patched_payload);
            rec.xor_payload.resize(patched_payload.size());
            for (std::size_t k = 0; k < patched_payload.size(); ++k) {
                const std::uint8_t mask =
                    compute_xor_mask_byte(orig_payload, rec.orig_data_hash, k);
                rec.xor_payload[k] = static_cast<std::uint8_t>(patched_payload[k] ^ mask);
            }
            total_payload_bytes += kPatchRecordHeaderSize + rec.xor_payload.size();
            records.push_back(std::move(rec));
        }

        Bytes out;
        out.reserve(kPatchHeaderSize + total_payload_bytes);
        out.insert(out.end(), kPatchMagic, kPatchMagic + kPatchMagicSize);
        append_le_u32(out, kPatchFormatVersion);
        append_le_u32(out, static_cast<std::uint32_t>(orig_entries.size()));
        append_le_u32(out, static_cast<std::uint32_t>(orig_ypf.size()));
        append_le_u32(out, static_cast<std::uint32_t>(records.size()));

        for (const PatchRecord& rec : records) {
            append_le_u32(out, rec.entry_index);
            append_le_u32(out, rec.orig_data_hash);
            append_le_u32(out, rec.new_raw_size);
            append_le_u32(out, rec.new_data_hash);
            out.insert(out.end(), rec.xor_payload.begin(), rec.xor_payload.end());
        }
        return out;
    }

    Bytes apply_xor_patch(const Bytes& orig_ypf,
                          const Bytes& patch_blob,
                          PatchSummary* summary) const {
        if (patch_blob.size() < kPatchHeaderSize ||
            std::memcmp(patch_blob.data(), kPatchMagic, kPatchMagicSize) != 0) {
            throw std::runtime_error("invalid FRATMENU patch magic");
        }
        const std::uint32_t version = rd_le_u32(patch_blob, kPatchVersionOffset);
        if (version != kPatchFormatVersion) {
            throw std::runtime_error("unsupported FRATMENU patch version: " +
                                     std::to_string(version));
        }
        const std::uint32_t expected_entries =
            rd_le_u32(patch_blob, kPatchTotalEntriesOffset);
        const std::uint32_t expected_orig_size =
            rd_le_u32(patch_blob, kPatchOrigSizeOffset);
        const std::uint32_t patch_entry_count =
            rd_le_u32(patch_blob, kPatchEntryCountOffset);

        if (orig_ypf.size() != expected_orig_size) {
            throw std::runtime_error("original YPF size mismatch: expected " +
                                     std::to_string(expected_orig_size) + ", got " +
                                     std::to_string(orig_ypf.size()));
        }

        const std::vector<yuris::YpfEntry> orig_entries =
            yuris::parse_ypf_index_bytes(orig_ypf, "cgsys.ypf");
        if (orig_entries.size() != expected_entries) {
            throw std::runtime_error("original YPF entry count mismatch: expected " +
                                     std::to_string(expected_entries) + ", got " +
                                     std::to_string(orig_entries.size()));
        }

        std::unordered_map<std::string, Bytes> replacements;
        replacements.reserve(patch_entry_count);
        std::size_t off = kPatchHeaderSize;

        for (std::uint32_t r = 0; r < patch_entry_count; ++r) {
            if (off + kPatchRecordHeaderSize > patch_blob.size()) {
                throw std::runtime_error("truncated FRATMENU record header");
            }
            const std::uint32_t entry_index = rd_le_u32(patch_blob, off);
            const std::uint32_t orig_data_hash =
                rd_le_u32(patch_blob, off + kRecordOrigHashOffset);
            const std::uint32_t new_raw_size =
                rd_le_u32(patch_blob, off + kRecordNewSizeOffset);
            const std::uint32_t new_data_hash =
                rd_le_u32(patch_blob, off + kRecordNewHashOffset);
            off += kPatchRecordHeaderSize;

            if (entry_index >= orig_entries.size()) {
                throw std::runtime_error("FRATMENU entry_index out of range: " +
                                         std::to_string(entry_index));
            }
            if (new_raw_size == 0 || off + new_raw_size > patch_blob.size()) {
                throw std::runtime_error("truncated or zero-length FRATMENU payload");
            }

            const yuris::YpfEntry& oe = orig_entries[entry_index];
            const Bytes orig_payload = yuris::read_entry(orig_ypf, oe);
            if (orig_payload.empty()) {
                throw std::runtime_error("empty original YPF entry at index " +
                                         std::to_string(entry_index));
            }
            const std::uint32_t actual_orig_hash = yuris::ypf_murmur_hash2(orig_payload);
            if (actual_orig_hash != orig_data_hash ||
                (oe.data_hash != 0 && oe.data_hash != orig_data_hash)) {
                throw std::runtime_error("original entry hash mismatch for " + oe.name);
            }

            Bytes unmasked(new_raw_size);
            for (std::size_t k = 0; k < new_raw_size; ++k) {
                const std::uint8_t mask =
                    compute_xor_mask_byte(orig_payload, orig_data_hash, k);
                unmasked[k] = static_cast<std::uint8_t>(patch_blob[off + k] ^ mask);
            }
            off += new_raw_size;

            const std::uint32_t actual_new_hash = yuris::ypf_murmur_hash2(unmasked);
            if (actual_new_hash != new_data_hash) {
                throw std::runtime_error("unmasked entry hash mismatch for " + oe.name);
            }
            parse_png_ihdr(unmasked, oe.name);

            const auto [it, inserted] = replacements.emplace(oe.name, std::move(unmasked));
            if (!inserted) {
                throw std::runtime_error("duplicate FRATMENU entry for " + oe.name);
            }
        }

        if (off != patch_blob.size()) {
            throw std::runtime_error("unexpected trailing bytes in FRATMENU patch");
        }

        if (summary != nullptr) {
            summary->discovered_assets = patch_entry_count;
        }
        return repack_bytes(orig_ypf, replacements, summary);
    }
};

MenuArchivePatcher::MenuArchivePatcher() : impl_(std::make_unique<Impl>()) {}
MenuArchivePatcher::MenuArchivePatcher(MenuArchivePatcher&&) noexcept = default;
MenuArchivePatcher& MenuArchivePatcher::operator=(MenuArchivePatcher&&) noexcept = default;
MenuArchivePatcher::~MenuArchivePatcher() = default;

Bytes MenuArchivePatcher::repack_archive_bytes(
    const Bytes& original_archive,
    const std::unordered_map<std::string, Bytes>& replacements,
    PatchSummary* summary) const {
    return impl_->repack_bytes(original_archive, replacements, summary);
}

Bytes MenuArchivePatcher::build_xor_patch_bytes(
    const Bytes& orig_ypf,
    const Bytes& patched_ypf) const {
    return impl_->build_xor_patch(orig_ypf, patched_ypf);
}

Bytes MenuArchivePatcher::apply_xor_patch_bytes(
    const Bytes& orig_ypf,
    const Bytes& patch_blob,
    PatchSummary* summary) const {
    return impl_->apply_xor_patch(orig_ypf, patch_blob, summary);
}

PatchSummary MenuArchivePatcher::patch_archive(const PatchConfig& config) const {
    const std::filesystem::path game_root = std::filesystem::u8path(config.game_dir);
    const std::filesystem::path archive_fs =
        game_root / std::filesystem::u8path(config.archive_rel_path);
    const std::string archive_path = archive_fs.u8string();
    const std::string backup_path = archive_path + ".bak";

    PatchSummary summary;
    summary.archive_path = archive_path;
    summary.backup_path = backup_path;

    const auto replacements = impl_->discover_assets(config.assets_dir);
    summary.discovered_assets = replacements.size();

    if (!std::filesystem::exists(std::filesystem::u8path(backup_path))) {
        if (!std::filesystem::exists(archive_fs)) {
            throw std::runtime_error("cannot find target archive: " + archive_path);
        }
        const Bytes orig = read_file(archive_path);
        write_bytes_atomic(backup_path, orig);
        summary.backup_created = true;
    }

    const Bytes original_archive = read_file(backup_path);
    if (replacements.empty() && !config.patch_file_path.empty() &&
        std::filesystem::exists(std::filesystem::u8path(config.patch_file_path))) {
        const Bytes patch_blob = read_file(config.patch_file_path);
        const Bytes repacked = impl_->apply_xor_patch(original_archive, patch_blob, &summary);
        write_bytes_atomic(archive_path, repacked);
        return summary;
    }

    const Bytes repacked = impl_->repack_bytes(original_archive, replacements, &summary);
    write_bytes_atomic(archive_path, repacked);
    return summary;
}

}  // namespace frat::menu
