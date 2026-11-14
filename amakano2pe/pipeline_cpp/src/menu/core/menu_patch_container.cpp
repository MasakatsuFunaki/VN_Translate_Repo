// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/menu_patch_container.h"

#include "common/util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace ama::menu {

namespace {

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995U;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurTailShift1 = 13;
constexpr int kMurmurTailShift2 = 15;
constexpr std::size_t kMurmurWordBytes = 4;
constexpr std::size_t kTailIndex0 = 0;
constexpr std::size_t kTailIndex1 = 1;
constexpr std::size_t kTailIndex2 = 2;

constexpr int kShift8 = 8;
constexpr int kShift16 = 16;
constexpr int kShift24 = 24;
constexpr int kShift32 = 32;
constexpr int kShift40 = 40;
constexpr int kShift48 = 48;
constexpr int kShift56 = 56;
constexpr std::uint32_t kByteMask = 0xFFU;
constexpr std::size_t kHashByteCycleMask = 3U;

constexpr std::uint32_t kXorStreamMultiplier = 0x9EU;
constexpr std::uint32_t kXorStreamOffset = 0x37U;

constexpr std::size_t kU16Bytes = 2;
constexpr std::size_t kU32Bytes = 4;
constexpr std::size_t kU64Bytes = 8;
constexpr std::size_t kMaxPathLength = 0xFFFFU;

constexpr std::array<std::string_view, 5> kForbiddenSignatures = {
    std::string_view("HG-3", 4),
    std::string_view("\x89PNG", 4),
    std::string_view("JFIF", 4),
    std::string_view("FES\0", 4),
    std::string_view("KCS\0", 4),
};

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

void AppendU64Le(std::vector<std::uint8_t>& out, std::uint64_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift16) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift24) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift32) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift40) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift48) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift56) & kByteMask));
}

std::uint16_t ReadU16Le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << kShift8));
}

std::uint32_t ReadU32Le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << kShift8) |
           (static_cast<std::uint32_t>(p[2]) << kShift16) |
           (static_cast<std::uint32_t>(p[3]) << kShift24);
}

std::uint64_t ReadU64Le(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(ReadU32Le(p)) |
           (static_cast<std::uint64_t>(ReadU32Le(p + kU32Bytes)) << kShift32);
}

bool EqualsAsciiIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

bool EndsWithIgnoreCase(std::string_view path, std::string_view suffix) {
    if (path.size() < suffix.size()) {
        return false;
    }
    return EqualsAsciiIgnoreCase(path.substr(path.size() - suffix.size()), suffix);
}

void ValidateRelativePath(std::string_view rel_path, const char* field_name) {
    if (rel_path.empty() || rel_path.size() > kMaxPathLength) {
        throw std::invalid_argument(std::string(field_name) + " is empty or too long");
    }
    if (rel_path.front() == '\\' || rel_path.back() == '\\') {
        throw std::invalid_argument(std::string(field_name) +
                                    " must not start or end with a backslash");
    }
    for (const char ch : rel_path) {
        if (ch == '/' || ch == ':' || ch == '\0') {
            throw std::invalid_argument(std::string(field_name) +
                                        " contains forbidden path character");
        }
    }
    std::size_t pos = 0;
    while (pos <= rel_path.size()) {
        const std::size_t sep = rel_path.find('\\', pos);
        const std::size_t end = (sep == std::string_view::npos) ? rel_path.size() : sep;
        const std::string_view part = rel_path.substr(pos, end - pos);
        if (part.empty() || part == "." || part == "..") {
            throw std::invalid_argument(std::string(field_name) +
                                        " contains invalid path traversal component");
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 1;
    }
}

void ValidateRecordSemantics(std::string_view source_rel_path,
                             std::string_view target_rel_path,
                             std::uint8_t flags,
                             std::uint32_t orig_size) {
    ValidateRelativePath(source_rel_path, "source_rel_path");
    ValidateRelativePath(target_rel_path, "target_rel_path");
    if (EndsWithIgnoreCase(target_rel_path, ".int") ||
        EndsWithIgnoreCase(target_rel_path, ".exe") ||
        EndsWithIgnoreCase(target_rel_path, ".dll")) {
        throw std::invalid_argument("target_rel_path must not target .int, .exe, or .dll");
    }
    if (flags != kFlagLooseOverride && flags != kFlagBackupStandalone) {
        throw std::invalid_argument("unknown MenuPatchRecord flags");
    }
    const bool same_path = EqualsAsciiIgnoreCase(source_rel_path, target_rel_path);
    if (flags == kFlagBackupStandalone && !same_path) {
        throw std::invalid_argument(
            "kFlagBackupStandalone requires source_rel_path == target_rel_path");
    }
    if (flags == kFlagLooseOverride && same_path) {
        throw std::invalid_argument(
            "loose override record requires distinct source_rel_path and target_rel_path");
    }
    if (orig_size < kMinOrigSliceBytes) {
        throw std::invalid_argument("orig_size must be at least 64 bytes");
    }
}

bool ContainsSignature(const std::uint8_t* data, std::size_t len,
                       std::string_view sig) {
    if (len < sig.size()) {
        return false;
    }
    const auto* sig_bytes = reinterpret_cast<const std::uint8_t*>(sig.data());
    const auto* end = data + len;
    return std::search(data, end, sig_bytes, sig_bytes + sig.size()) != end;
}

}  // namespace

std::uint32_t MurmurHash2(const std::uint8_t* data, std::size_t len,
                          std::uint32_t seed) {
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);
    std::size_t offset = 0;
    while (len - offset >= kMurmurWordBytes) {
        std::uint32_t k = ReadU32Le(data + offset);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;

        h *= kMurmurMultiplier;
        h ^= k;
        offset += kMurmurWordBytes;
    }

    const std::size_t rem = len - offset;
    if (rem == kTailIndex2 + 1) {
        h ^= static_cast<std::uint32_t>(data[offset + kTailIndex2]) << kShift16;
    }
    if (rem >= kTailIndex1 + 1) {
        h ^= static_cast<std::uint32_t>(data[offset + kTailIndex1]) << kShift8;
    }
    if (rem >= kTailIndex0 + 1) {
        h ^= static_cast<std::uint32_t>(data[offset + kTailIndex0]);
        h *= kMurmurMultiplier;
    }

    h ^= h >> kMurmurTailShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurTailShift2;
    return h;
}

std::uint32_t ComputeManifestHash(const std::vector<MenuPatchRecord>& records) {
    std::vector<std::uint8_t> desc;
    for (const MenuPatchRecord& rec : records) {
        AppendU16Le(desc, static_cast<std::uint16_t>(rec.source_rel_path.size()));
        desc.insert(desc.end(), rec.source_rel_path.begin(), rec.source_rel_path.end());
        AppendU16Le(desc, static_cast<std::uint16_t>(rec.target_rel_path.size()));
        desc.insert(desc.end(), rec.target_rel_path.begin(), rec.target_rel_path.end());
        desc.push_back(rec.flags);
        AppendU64Le(desc, rec.orig_offset);
        AppendU32Le(desc, rec.orig_size);
        AppendU32Le(desc, rec.orig_slice_hash);
        AppendU32Le(desc, rec.new_raw_size);
        AppendU32Le(desc, rec.new_raw_hash);
    }
    return MurmurHash2(desc);
}

std::vector<std::uint8_t> ApplyMenuPatchXorMask(
    const std::uint8_t* input, std::size_t input_size,
    const std::uint8_t* orig_slice, std::size_t orig_size,
    std::uint32_t orig_slice_hash) {
    if (orig_size < kMinOrigSliceBytes) {
        throw std::invalid_argument("orig_slice must be at least 64 bytes");
    }
    std::vector<std::uint8_t> out(input_size);
    for (std::size_t k = 0; k < input_size; ++k) {
        const auto k32 = static_cast<std::uint32_t>(k);
        const std::uint8_t slice_byte = orig_slice[k % orig_size];
        const std::uint8_t hash_byte = static_cast<std::uint8_t>(
            (orig_slice_hash >> ((k32 & kHashByteCycleMask) * kShift8)) & kByteMask);
        const std::uint8_t stream_byte = static_cast<std::uint8_t>(
            (k32 * kXorStreamMultiplier + kXorStreamOffset) & kByteMask);
        out[k] = static_cast<std::uint8_t>(input[k] ^ slice_byte ^ hash_byte ^ stream_byte);
    }
    return out;
}

std::vector<std::uint8_t> UnmaskMenuPatchRecord(
    const MenuPatchRecord& record, const std::vector<std::uint8_t>& orig_slice) {
    if (orig_slice.size() != record.orig_size || orig_slice.size() < kMinOrigSliceBytes) {
        throw std::runtime_error("orig_slice size mismatch for " + record.target_rel_path);
    }
    const std::uint32_t actual_orig_hash = MurmurHash2(orig_slice);
    if (actual_orig_hash != record.orig_slice_hash) {
        throw std::runtime_error("orig_slice_hash mismatch for " + record.target_rel_path);
    }
    if (record.xor_payload.size() != record.new_raw_size) {
        throw std::runtime_error("xor_payload size mismatch for " + record.target_rel_path);
    }
    std::vector<std::uint8_t> unmasked = ApplyMenuPatchXorMask(
        record.xor_payload.data(), record.xor_payload.size(),
        orig_slice.data(), orig_slice.size(), record.orig_slice_hash);
    const std::uint32_t actual_new_hash = MurmurHash2(unmasked);
    if (actual_new_hash != record.new_raw_hash) {
        throw std::runtime_error("new_raw_hash mismatch for " + record.target_rel_path);
    }
    return unmasked;
}

bool VerifyNoRawAssetSignatures(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return true;
    }
    for (const std::string_view sig : kForbiddenSignatures) {
        if (ContainsSignature(data, len, sig)) {
            return false;
        }
    }
    return true;
}

MenuPatchContainerData ParseMenuPatchContainer(const std::uint8_t* data,
                                               std::size_t len) {
    if (data == nullptr || len < kMenuPatchHeaderSize) {
        throw std::runtime_error("menu_patch container truncated at header");
    }
    if (std::memcmp(data, kMenuPatchMagic, sizeof(kMenuPatchMagic)) != 0) {
        throw std::runtime_error("menu_patch container has invalid magic");
    }
    const std::uint32_t version = ReadU32Le(data + sizeof(kMenuPatchMagic));
    if (version != kMenuPatchFormatVersion) {
        throw std::runtime_error("unsupported menu_patch format_version");
    }
    const std::uint32_t record_count =
        ReadU32Le(data + sizeof(kMenuPatchMagic) + kU32Bytes);
    const std::uint32_t manifest_hash =
        ReadU32Le(data + sizeof(kMenuPatchMagic) + kU32Bytes * 2);
    const std::uint8_t* const hdr_reserved =
        data + kMenuPatchHeaderSize - kMenuPatchHeaderReservedBytes;
    for (std::size_t i = 0; i < kMenuPatchHeaderReservedBytes; ++i) {
        if (hdr_reserved[i] != 0) {
            throw std::runtime_error("menu_patch header reserved bytes must be zero");
        }
    }

    MenuPatchContainerData out;
    out.format_version = version;
    out.manifest_hash = manifest_hash;
    out.records.reserve(record_count);

    std::size_t pos = kMenuPatchHeaderSize;
    for (std::uint32_t idx = 0; idx < record_count; ++idx) {
        if (pos + kU16Bytes > len) {
            throw std::runtime_error("menu_patch truncated at source_path_len");
        }
        const std::uint16_t src_len = ReadU16Le(data + pos);
        pos += kU16Bytes;
        if (pos + src_len + kU16Bytes > len) {
            throw std::runtime_error("menu_patch truncated at source_rel_path");
        }
        std::string src_path(reinterpret_cast<const char*>(data + pos), src_len);
        pos += src_len;

        const std::uint16_t dst_len = ReadU16Le(data + pos);
        pos += kU16Bytes;
        constexpr std::size_t kFixedMetaBytes =
            1 + kMenuPatchRecordReservedFlagBytes + kU64Bytes + kU32Bytes * 4;
        if (pos + dst_len + kFixedMetaBytes > len) {
            throw std::runtime_error("menu_patch truncated at record metadata");
        }
        std::string dst_path(reinterpret_cast<const char*>(data + pos), dst_len);
        pos += dst_len;

        const std::uint8_t flags = data[pos++];
        for (std::size_t r = 0; r < kMenuPatchRecordReservedFlagBytes; ++r) {
            if (data[pos++] != 0) {
                throw std::runtime_error("menu_patch record reserved_flags must be zero");
            }
        }
        const std::uint64_t orig_offset = ReadU64Le(data + pos);
        pos += kU64Bytes;
        const std::uint32_t orig_size = ReadU32Le(data + pos);
        pos += kU32Bytes;
        const std::uint32_t orig_slice_hash = ReadU32Le(data + pos);
        pos += kU32Bytes;
        const std::uint32_t new_raw_size = ReadU32Le(data + pos);
        pos += kU32Bytes;
        const std::uint32_t new_raw_hash = ReadU32Le(data + pos);
        pos += kU32Bytes;

        ValidateRecordSemantics(src_path, dst_path, flags, orig_size);
        if (new_raw_size == 0 || pos + new_raw_size > len) {
            throw std::runtime_error("menu_patch truncated or empty xor_payload");
        }

        MenuPatchRecord rec;
        rec.source_rel_path = std::move(src_path);
        rec.target_rel_path = std::move(dst_path);
        rec.flags = flags;
        rec.orig_offset = orig_offset;
        rec.orig_size = orig_size;
        rec.orig_slice_hash = orig_slice_hash;
        rec.new_raw_size = new_raw_size;
        rec.new_raw_hash = new_raw_hash;
        rec.xor_payload.assign(data + pos, data + pos + new_raw_size);
        pos += new_raw_size;

        out.records.push_back(std::move(rec));
    }

    if (pos != len) {
        throw std::runtime_error("menu_patch has trailing bytes after last record");
    }
    if (ComputeManifestHash(out.records) != out.manifest_hash) {
        throw std::runtime_error("menu_patch manifest_hash mismatch");
    }
    return out;
}

struct MenuPatchBuilder::Impl {
    std::vector<MenuPatchRecord> records;
};

MenuPatchBuilder::MenuPatchBuilder() : impl_(std::make_unique<Impl>()) {}
MenuPatchBuilder::MenuPatchBuilder(MenuPatchBuilder&&) noexcept = default;
MenuPatchBuilder& MenuPatchBuilder::operator=(MenuPatchBuilder&&) noexcept = default;
MenuPatchBuilder::~MenuPatchBuilder() = default;

void MenuPatchBuilder::AddRecord(MenuPatchRecordInput input) {
    if (input.orig_slice.size() > std::numeric_limits<std::uint32_t>::max() ||
        input.new_raw.size() > std::numeric_limits<std::uint32_t>::max() ||
        input.new_raw.empty()) {
        throw std::invalid_argument("invalid MenuPatchRecordInput payload sizes");
    }
    const auto orig_size = static_cast<std::uint32_t>(input.orig_slice.size());
    ValidateRecordSemantics(input.source_rel_path, input.target_rel_path, input.flags,
                            orig_size);

    MenuPatchRecord rec;
    rec.source_rel_path = std::move(input.source_rel_path);
    rec.target_rel_path = std::move(input.target_rel_path);
    rec.flags = input.flags;
    rec.orig_offset = input.orig_offset;
    rec.orig_size = orig_size;
    rec.orig_slice_hash = MurmurHash2(input.orig_slice);
    rec.new_raw_size = static_cast<std::uint32_t>(input.new_raw.size());
    rec.new_raw_hash = MurmurHash2(input.new_raw);
    rec.xor_payload = ApplyMenuPatchXorMask(
        input.new_raw.data(), input.new_raw.size(),
        input.orig_slice.data(), input.orig_slice.size(), rec.orig_slice_hash);
    impl_->records.push_back(std::move(rec));
}

std::size_t MenuPatchBuilder::record_count() const {
    return impl_->records.size();
}

std::vector<std::uint8_t> MenuPatchBuilder::BuildBytes() const {
    const std::uint32_t manifest = ComputeManifestHash(impl_->records);
    std::vector<std::uint8_t> out;
    out.insert(out.end(), std::begin(kMenuPatchMagic), std::end(kMenuPatchMagic));
    AppendU32Le(out, kMenuPatchFormatVersion);
    AppendU32Le(out, static_cast<std::uint32_t>(impl_->records.size()));
    AppendU32Le(out, manifest);
    out.insert(out.end(), kMenuPatchHeaderReservedBytes, 0);

    for (const MenuPatchRecord& rec : impl_->records) {
        AppendU16Le(out, static_cast<std::uint16_t>(rec.source_rel_path.size()));
        out.insert(out.end(), rec.source_rel_path.begin(), rec.source_rel_path.end());
        AppendU16Le(out, static_cast<std::uint16_t>(rec.target_rel_path.size()));
        out.insert(out.end(), rec.target_rel_path.begin(), rec.target_rel_path.end());
        out.push_back(rec.flags);
        out.insert(out.end(), kMenuPatchRecordReservedFlagBytes, 0);
        AppendU64Le(out, rec.orig_offset);
        AppendU32Le(out, rec.orig_size);
        AppendU32Le(out, rec.orig_slice_hash);
        AppendU32Le(out, rec.new_raw_size);
        AppendU32Le(out, rec.new_raw_hash);
        out.insert(out.end(), rec.xor_payload.begin(), rec.xor_payload.end());
    }

    if (!VerifyNoRawAssetSignatures(out)) {
        throw std::runtime_error(
            "serialized menu_patch.bin contains a forbidden raw asset signature");
    }
    return out;
}

void MenuPatchBuilder::WriteToFile(const std::filesystem::path& output_path) const {
    const std::vector<std::uint8_t> bytes = BuildBytes();
    ama::write_file(output_path.u8string(), bytes);
}

}  // namespace ama::menu
