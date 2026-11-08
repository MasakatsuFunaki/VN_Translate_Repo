// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "lpk/lpk_archive.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

namespace oretsuba::lpk {

namespace {

constexpr std::uint32_t kRotateMask = 31U;
constexpr std::uint32_t kWordBits = 32U;
constexpr std::uint32_t kStemRotateBits = 7U;
constexpr std::uint32_t kLowByteMask = 0xFFU;
constexpr std::uint32_t kUpperBytesMask = 0xFFFFFF00U;

constexpr std::size_t kRingMask = 0x0FFFU;
constexpr std::uint32_t kLzssControlBitCount = 8U;
constexpr std::uint8_t kLzssLiteralBit = 0x01U;
constexpr std::uint8_t kLzssHighOffsetMask = 0xF0U;
constexpr std::uint8_t kLzssLengthMask = 0x0FU;
constexpr std::uint32_t kLzssHighOffsetShift = 4U;
constexpr std::size_t kLzssMinMatchLength = 3U;
constexpr std::size_t kLzssMatchPairBytes = 2U;

constexpr std::size_t kLpkMagicSize = 4U;
constexpr std::size_t kLpkHeaderSize = 8U;
constexpr const char kLpkMagic[kLpkMagicSize] = {'L', 'P', 'K', '1'};
constexpr std::uint32_t kHeaderFlagsShift = 24U;
constexpr std::uint32_t kHeaderSizeMask = 0x00FFFFFFU;
constexpr std::uint32_t kSectorAlignShift = 11U;

constexpr std::uint8_t kFlagAlign2048 = 0x01U;
constexpr std::uint8_t kFlagHasPrefixTrie = 0x02U;
constexpr std::uint8_t kFlagCryptPayload = 0x04U;
constexpr std::uint8_t kFlagCompressed = 0x08U;

constexpr std::size_t kDwordBytes = 4U;
constexpr std::size_t kWordBytes = 2U;
constexpr std::uint32_t kIndexCipherStepBits = 4U;
constexpr std::size_t kIndexHeaderMinBytes = 5U;
constexpr std::size_t kTrieHeaderBytes = 5U;
constexpr std::size_t kCompressedEntryStride = 13U;
constexpr std::size_t kUncompressedEntryStride = 9U;
constexpr std::size_t kEntryOffsetFieldPos = 1U;
constexpr std::size_t kEntryPackedFieldPos = 5U;
constexpr std::size_t kEntryUnpackedFieldPos = 9U;

constexpr std::uint8_t kEntryFlagObfuscatedBytes = 0x01U;
constexpr std::uint8_t kByteObfuscateXorLow = 0xFDU;
constexpr std::uint8_t kByteObfuscateXorHigh = 0x50U;
constexpr std::uint8_t kNibbleMask = 0x0FU;
constexpr std::uint32_t kNibbleShift = 4U;
constexpr std::size_t kMaxPayloadEncryptedDwords = 0x40U;
constexpr std::size_t kDefaultIndexNumberWidth = 5U;

std::uint32_t RotateRight32(std::uint32_t value, std::uint32_t count) {
    const std::uint32_t shift = count & kRotateMask;
    if (shift == 0U) {
        return value;
    }
    return (value >> shift) | (value << (kWordBits - shift));
}

std::uint32_t RotateLeft32(std::uint32_t value, std::uint32_t count) {
    const std::uint32_t shift = count & kRotateMask;
    if (shift == 0U) {
        return value;
    }
    return (value << shift) | (value >> (kWordBits - shift));
}

std::uint32_t ReadU32Le(const std::uint8_t* ptr) {
    std::uint32_t val = 0;
    std::memcpy(&val, ptr, sizeof(val));
    return val;
}

std::uint16_t ReadU16Le(const std::uint8_t* ptr) {
    std::uint16_t val = 0;
    std::memcpy(&val, ptr, sizeof(val));
    return val;
}

void WriteU32Le(std::uint8_t* ptr, std::uint32_t val) {
    std::memcpy(ptr, &val, sizeof(val));
}

char ToUpperAscii(char ch) {
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<char>(ch - 'a' + 'A');
    }
    return ch;
}

char ToLowerAscii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

std::string NormalizeLowerAscii(std::string_view input) {
    std::string out(input.size(), '\0');
    for (std::size_t i = 0; i < input.size(); ++i) {
        out[i] = ToLowerAscii(input[i]);
    }
    return out;
}

std::string ExtractArchiveStem(const std::string& archive_path) {
    const auto fs_path = std::filesystem::u8path(archive_path);
    return fs_path.stem().u8string();
}

std::string FallbackEntryName(std::uint32_t index) {
    std::string digits = std::to_string(index);
    if (digits.size() < kDefaultIndexNumberWidth) {
        digits.insert(0, kDefaultIndexNumberWidth - digits.size(), '0');
    }
    return "__entry_" + digits;
}

void WalkPrefixTrieNode(const std::uint8_t* tree_buf,
                        std::size_t tree_size,
                        bool use_32bit,
                        std::size_t node_pos,
                        Bytes& prefix,
                        std::unordered_map<std::uint32_t, std::string>& names_out) {
    if (node_pos >= tree_size) {
        throw std::runtime_error("LPK1 trie node out of bounds");
    }
    const std::uint8_t child_count = tree_buf[node_pos];
    std::size_t pos = node_pos + 1;
    const std::size_t value_bytes = use_32bit ? kDwordBytes : kWordBytes;

    for (std::uint8_t child = 0; child < child_count; ++child) {
        if (pos + 1 + value_bytes > tree_size) {
            throw std::runtime_error("LPK1 trie child entry truncated");
        }
        const std::uint8_t ch = tree_buf[pos];
        pos += 1;
        const std::uint32_t val = use_32bit
            ? ReadU32Le(tree_buf + pos)
            : static_cast<std::uint32_t>(ReadU16Le(tree_buf + pos));
        pos += value_bytes;

        if (ch == 0U) {
            names_out[val] = Cp932ToUtf8(prefix.data(), prefix.size());
            continue;
        }
        prefix.push_back(ch);
        WalkPrefixTrieNode(tree_buf, tree_size, use_32bit, pos + val, prefix, names_out);
        prefix.pop_back();
    }
}

std::unordered_map<std::uint32_t, std::string> ParsePrefixTrie(
    const std::uint8_t* tree_buf,
    std::size_t tree_size,
    bool use_32bit) {
    std::unordered_map<std::uint32_t, std::string> names;
    if (tree_size == 0) {
        return names;
    }
    Bytes prefix;
    WalkPrefixTrieNode(tree_buf, tree_size, use_32bit, 0, prefix, names);
    return names;
}

}  // namespace

LpkKeyPair DeriveLpkKeys(std::string_view stem,
                         std::uint32_t master_key0,
                         std::uint32_t master_key1) {
    std::string upper_stem(stem.size(), '\0');
    for (std::size_t i = 0; i < stem.size(); ++i) {
        upper_stem[i] = ToUpperAscii(stem[i]);
    }

    std::uint32_t key0 = kInitKey0;
    std::uint32_t key1 = kInitKey1;
    const std::size_t len = upper_stem.size();
    for (std::size_t i = 0; i < len; ++i) {
        const auto rev_byte = static_cast<std::uint8_t>(upper_stem[len - 1 - i]);
        const auto fwd_byte = static_cast<std::uint8_t>(upper_stem[i]);
        key0 = (key0 & kUpperBytesMask) | ((key0 & kLowByteMask) ^ rev_byte);
        key1 = (key1 & kUpperBytesMask) | ((key1 & kLowByteMask) ^ fwd_byte);
        key0 = RotateRight32(key0, kStemRotateBits);
        key1 = RotateLeft32(key1, kStemRotateBits);
    }

    LpkKeyPair out;
    out.file_key = master_key0 ^ key0;
    out.header_key = master_key1 ^ key1;
    return out;
}

Bytes DecompressLzss(const std::uint8_t* src,
                     std::size_t src_size,
                     std::size_t unpacked_size) {
    Bytes ring(kRingSize, 0U);
    std::size_t ring_pos = kInitialRingPos;
    Bytes out;
    out.reserve(unpacked_size);

    std::size_t src_pos = 0;
    std::uint32_t control = 0;
    std::uint32_t bits_remaining = 0;

    while (out.size() < unpacked_size && src_pos < src_size) {
        if (bits_remaining == 0U) {
            control = src[src_pos];
            ++src_pos;
            bits_remaining = kLzssControlBitCount;
            if (src_pos >= src_size) {
                break;
            }
        }

        if ((control & kLzssLiteralBit) != 0U) {
            const std::uint8_t literal = src[src_pos];
            ++src_pos;
            out.push_back(literal);
            ring[ring_pos] = literal;
            ring_pos = (ring_pos + 1U) & kRingMask;
            control >>= 1U;
            --bits_remaining;
            continue;
        }

        if (src_pos + kLzssMatchPairBytes > src_size) {
            break;
        }
        const std::uint8_t byte0 = src[src_pos];
        const std::uint8_t byte1 = src[src_pos + 1U];
        src_pos += kLzssMatchPairBytes;

        const std::size_t match_offset =
            static_cast<std::size_t>(byte0) |
            (static_cast<std::size_t>(byte1 & kLzssHighOffsetMask) << kLzssHighOffsetShift);
        const std::size_t match_count =
            static_cast<std::size_t>(byte1 & kLzssLengthMask) + kLzssMinMatchLength;

        for (std::size_t k = 0; k < match_count && out.size() < unpacked_size; ++k) {
            const std::uint8_t copied = ring[(match_offset + k) & kRingMask];
            out.push_back(copied);
            ring[ring_pos] = copied;
            ring_pos = (ring_pos + 1U) & kRingMask;
        }

        control >>= 1U;
        --bits_remaining;
    }

    return out;
}

Bytes DecompressLzss(const Bytes& src, std::size_t unpacked_size) {
    return DecompressLzss(src.data(), src.size(), unpacked_size);
}

struct LpkArchiveReader::Impl {
    Bytes archive_bytes;
    LpkKeyPair keys;
    bool flag_align_2048 = false;
    bool flag_has_tree = false;
    bool flag_crypt_payload = false;
    bool flag_compressed = false;
    Bytes shared_prefix;
    std::vector<LpkEntryInfo> entries;
    std::unordered_map<std::string, std::size_t> name_to_index;

    Impl(Bytes raw_archive,
         std::string_view stem,
         std::uint32_t master_key0,
         std::uint32_t master_key1)
        : archive_bytes(std::move(raw_archive)),
          keys(DeriveLpkKeys(stem, master_key0, master_key1)) {
        ParseArchiveIndex();
    }

    void ParseArchiveIndex() {
        if (archive_bytes.size() < kLpkHeaderSize) {
            throw std::runtime_error("LPK1 archive smaller than 8-byte header");
        }
        if (std::memcmp(archive_bytes.data(), kLpkMagic, kLpkMagicSize) != 0) {
            throw std::runtime_error("Invalid LPK1 magic");
        }

        const std::uint32_t raw_hdr = ReadU32Le(archive_bytes.data() + kLpkMagicSize);
        const std::uint32_t hdr_val = raw_hdr ^ keys.header_key;
        const auto flags = static_cast<std::uint8_t>((hdr_val >> kHeaderFlagsShift) & kLowByteMask);
        const std::uint32_t size_field = hdr_val & kHeaderSizeMask;

        flag_align_2048 = (flags & kFlagAlign2048) != 0U;
        flag_has_tree = (flags & kFlagHasPrefixTrie) != 0U;
        flag_crypt_payload = (flags & kFlagCryptPayload) != 0U;
        flag_compressed = (flags & kFlagCompressed) != 0U;

        const std::size_t index_size = flag_align_2048
            ? (static_cast<std::size_t>(size_field) << kSectorAlignShift) - kLpkHeaderSize
            : static_cast<std::size_t>(size_field);

        if (kLpkHeaderSize + index_size > archive_bytes.size()) {
            throw std::runtime_error("LPK1 index table exceeds archive size");
        }

        Bytes raw_index(
            archive_bytes.begin() + static_cast<std::ptrdiff_t>(kLpkHeaderSize),
            archive_bytes.begin() + static_cast<std::ptrdiff_t>(kLpkHeaderSize + index_size));

        const std::size_t dword_count = index_size / kDwordBytes;
        std::uint32_t eax = keys.header_key;
        std::uint32_t ecx = kIndexCipherSeed;
        for (std::size_t i = 0; i < dword_count; ++i) {
            ecx = RotateLeft32(ecx, kIndexCipherStepBits);
            const std::uint32_t decrypted = ReadU32Le(raw_index.data() + i * kDwordBytes) ^ eax;
            WriteU32Le(raw_index.data() + i * kDwordBytes, decrypted);
            eax = RotateRight32(eax, ecx & kLowByteMask);
        }

        if (raw_index.size() < kIndexHeaderMinBytes) {
            throw std::runtime_error("LPK1 decrypted index too short");
        }
        const std::uint32_t num_entries = ReadU32Le(raw_index.data());
        const std::uint8_t prefix_len = raw_index[kDwordBytes];
        std::size_t pos = kIndexHeaderMinBytes;
        if (pos + prefix_len > raw_index.size()) {
            throw std::runtime_error("LPK1 shared prefix exceeds index table");
        }
        shared_prefix.assign(
            raw_index.begin() + static_cast<std::ptrdiff_t>(pos),
            raw_index.begin() + static_cast<std::ptrdiff_t>(pos + prefix_len));
        pos += prefix_len;

        std::unordered_map<std::uint32_t, std::string> trie_names;
        if (flag_has_tree) {
            if (pos + kTrieHeaderBytes > raw_index.size()) {
                throw std::runtime_error("LPK1 trie header truncated");
            }
            const bool use_32bit = raw_index[pos] != 0U;
            pos += 1;
            const std::uint32_t tree_size = ReadU32Le(raw_index.data() + pos);
            pos += kDwordBytes;
            if (pos + tree_size > raw_index.size()) {
                throw std::runtime_error("LPK1 trie buffer exceeds index table");
            }
            trie_names = ParsePrefixTrie(raw_index.data() + pos, tree_size, use_32bit);
            pos += tree_size;
        }

        const std::size_t entry_stride =
            flag_compressed ? kCompressedEntryStride : kUncompressedEntryStride;
        if (pos + static_cast<std::size_t>(num_entries) * entry_stride > raw_index.size()) {
            throw std::runtime_error("LPK1 entry records exceed index table");
        }

        entries.reserve(num_entries);
        for (std::uint32_t idx = 0; idx < num_entries; ++idx) {
            LpkEntryInfo info;
            info.index = idx;
            const auto name_it = trie_names.find(idx);
            info.name = (name_it != trie_names.end()) ? name_it->second : FallbackEntryName(idx);
            info.entry_flags = raw_index[pos];
            info.offset = ReadU32Le(raw_index.data() + pos + kEntryOffsetFieldPos);
            if (flag_align_2048) {
                info.offset <<= kSectorAlignShift;
            }
            info.packed_size = ReadU32Le(raw_index.data() + pos + kEntryPackedFieldPos);
            if (flag_compressed) {
                info.unpacked_size = ReadU32Le(raw_index.data() + pos + kEntryUnpackedFieldPos);
                if (info.unpacked_size == 0U) {
                    info.unpacked_size = info.packed_size;
                }
            }
            if (!flag_compressed) {
                info.unpacked_size = info.packed_size;
            }
            pos += entry_stride;

            name_to_index.emplace(NormalizeLowerAscii(info.name), entries.size());
            entries.push_back(std::move(info));
        }
    }

    Bytes ExtractEntryPayload(std::size_t entry_index) const {
        if (entry_index >= entries.size()) {
            throw std::out_of_range("LPK1 entry index out of range");
        }
        const LpkEntryInfo& entry = entries[entry_index];
        const std::size_t prefix_len = shared_prefix.size();
        if (entry.unpacked_size < prefix_len) {
            throw std::runtime_error("LPK1 entry unpacked size smaller than shared prefix: " + entry.name);
        }
        const std::size_t body_unpacked_size = entry.unpacked_size - prefix_len;
        const bool is_compressed = entry.packed_size != entry.unpacked_size;
        const std::size_t read_bytes = is_compressed ? entry.packed_size : body_unpacked_size;

        if (static_cast<std::size_t>(entry.offset) + read_bytes > archive_bytes.size()) {
            throw std::runtime_error("LPK1 entry payload out of archive bounds: " + entry.name);
        }

        const std::uint8_t* raw_ptr = archive_bytes.data() + entry.offset;
        Bytes body = is_compressed
            ? DecompressLzss(raw_ptr, entry.packed_size, body_unpacked_size)
            : Bytes(raw_ptr, raw_ptr + body_unpacked_size);

        if (body.size() != body_unpacked_size) {
            throw std::runtime_error("LPK1 decompressed size mismatch for " + entry.name);
        }

        if ((entry.entry_flags & kEntryFlagObfuscatedBytes) != 0U) {
            for (std::uint8_t& byte_val : body) {
                const std::uint8_t low_swapped =
                    static_cast<std::uint8_t>(((byte_val ^ kByteObfuscateXorLow) & kNibbleMask) << kNibbleShift);
                const std::uint8_t high_swapped =
                    static_cast<std::uint8_t>((byte_val ^ kByteObfuscateXorHigh) >> kNibbleShift);
                byte_val = static_cast<std::uint8_t>(low_swapped | high_swapped);
            }
        }

        if (flag_crypt_payload) {
            const std::size_t num_dwords =
                std::min(body.size() / kDwordBytes, kMaxPayloadEncryptedDwords);
            std::uint32_t eax = keys.file_key;
            std::uint32_t ecx = kIndexCipherSeed;
            for (std::size_t i = 0; i < num_dwords; ++i) {
                ecx = RotateRight32(ecx, kIndexCipherStepBits);
                const std::uint32_t decrypted = ReadU32Le(body.data() + i * kDwordBytes) ^ eax;
                WriteU32Le(body.data() + i * kDwordBytes, decrypted);
                eax = RotateLeft32(eax, ecx & kLowByteMask);
            }
        }

        Bytes full;
        full.reserve(shared_prefix.size() + body.size());
        full.insert(full.end(), shared_prefix.begin(), shared_prefix.end());
        full.insert(full.end(), body.begin(), body.end());
        return full;
    }
};

LpkArchiveReader::LpkArchiveReader(const std::string& archive_path,
                                   std::uint32_t master_key0,
                                   std::uint32_t master_key1)
    : impl_(std::make_unique<Impl>(
          ReadFileBytes(archive_path),
          ExtractArchiveStem(archive_path),
          master_key0,
          master_key1)) {}

LpkArchiveReader::LpkArchiveReader(Bytes archive_bytes,
                                   std::string_view stem,
                                   std::uint32_t master_key0,
                                   std::uint32_t master_key1)
    : impl_(std::make_unique<Impl>(
          std::move(archive_bytes),
          stem,
          master_key0,
          master_key1)) {}

LpkArchiveReader::LpkArchiveReader(LpkArchiveReader&& other) noexcept = default;
LpkArchiveReader& LpkArchiveReader::operator=(LpkArchiveReader&& other) noexcept = default;
LpkArchiveReader::~LpkArchiveReader() = default;

std::size_t LpkArchiveReader::EntryCount() const {
    return impl_->entries.size();
}

const std::vector<LpkEntryInfo>& LpkArchiveReader::Entries() const {
    return impl_->entries;
}

std::optional<std::size_t> LpkArchiveReader::FindEntryIndex(std::string_view name) const {
    const auto it = impl_->name_to_index.find(NormalizeLowerAscii(name));
    if (it == impl_->name_to_index.end()) {
        return std::nullopt;
    }
    return it->second;
}

Bytes LpkArchiveReader::ReadEntry(std::size_t entry_index) const {
    return impl_->ExtractEntryPayload(entry_index);
}

Bytes LpkArchiveReader::ReadEntryByName(std::string_view name) const {
    const auto idx = FindEntryIndex(name);
    if (!idx) {
        throw std::runtime_error("LPK1 entry not found: " + std::string(name));
    }
    return impl_->ExtractEntryPayload(*idx);
}

}  // namespace oretsuba::lpk
