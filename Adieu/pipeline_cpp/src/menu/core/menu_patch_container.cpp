// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/menu_patch_container.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace adi::menu {
namespace {

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr unsigned int kMurmurShiftR = 24u;
constexpr unsigned int kMurmurFinalShift1 = 13u;
constexpr unsigned int kMurmurFinalShift2 = 15u;

constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kXorStreamAddend = 0x37u;
constexpr std::size_t kHashByteCycleMask = 3u;
constexpr unsigned int kBitsPerByte = 8u;

constexpr std::size_t kRecordFixedMetadataBytes = 8u + 8u + 4u + 8u + 4u + 4u;
constexpr std::size_t kDosDeviceShortLen = 3u;
constexpr std::size_t kDosDevicePortLen = 4u;

constexpr std::array<std::uint8_t, 8> kPngMagic = {
    0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};

char ToAsciiLower(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch + ('a' - 'A'));
    }
    return ch;
}

std::string LowercaseAsciiCopy(std::string_view sv) {
    std::string out(sv.size(), '\0');
    for (std::size_t i = 0u; i < sv.size(); ++i) {
        out[i] = ToAsciiLower(sv[i]);
    }
    return out;
}

bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0u; i < a.size(); ++i) {
        if (ToAsciiLower(a[i]) != ToAsciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

bool EndsWithIgnoreAsciiCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    return EqualsIgnoreAsciiCase(text.substr(text.size() - suffix.size()), suffix);
}

bool IsDosReservedDeviceStem(std::string_view stem) noexcept {
    if (stem.size() == kDosDeviceShortLen) {
        return EqualsIgnoreAsciiCase(stem, "con") || EqualsIgnoreAsciiCase(stem, "prn") ||
               EqualsIgnoreAsciiCase(stem, "aux") || EqualsIgnoreAsciiCase(stem, "nul");
    }
    if (stem.size() == kDosDevicePortLen && stem[3] >= '1' && stem[3] <= '9') {
        const std::string_view prefix = stem.substr(0u, kDosDeviceShortLen);
        return EqualsIgnoreAsciiCase(prefix, "com") || EqualsIgnoreAsciiCase(prefix, "lpt");
    }
    return false;
}

bool IsValidPathSegment(std::string_view seg) noexcept {
    if (seg.empty() || seg == "." || seg == "..") {
        return false;
    }
    const char last = seg.back();
    if (last == ' ' || last == '.') {
        return false;
    }
    const std::size_t dot = seg.find('.');
    const std::string_view stem = (dot == std::string_view::npos) ? seg : seg.substr(0u, dot);
    return !IsDosReservedDeviceStem(stem);
}

bool IsValidUtf8NoControl(std::string_view text) noexcept {
    std::size_t i = 0u;
    while (i < text.size()) {
        const auto b0 = static_cast<std::uint8_t>(text[i]);
        if (b0 < 0x20u || b0 == 0x7Fu) {
            return false;
        }
        if (b0 < 0x80u) {
            ++i;
            continue;
        }
        if ((b0 & 0xE0u) == 0xC0u) {
            if (b0 < 0xC2u || i + 1u >= text.size()) {
                return false;
            }
            const auto b1 = static_cast<std::uint8_t>(text[i + 1u]);
            if ((b1 & 0xC0u) != 0x80u) {
                return false;
            }
            i += 2u;
            continue;
        }
        if ((b0 & 0xF0u) == 0xE0u) {
            if (i + 2u >= text.size()) {
                return false;
            }
            const auto b1 = static_cast<std::uint8_t>(text[i + 1u]);
            const auto b2 = static_cast<std::uint8_t>(text[i + 2u]);
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
                return false;
            }
            if (b0 == 0xE0u && b1 < 0xA0u) {
                return false;
            }
            if (b0 == 0xEDu && b1 >= 0xA0u) {
                return false;
            }
            i += 3u;
            continue;
        }
        if ((b0 & 0xF8u) == 0xF0u && b0 <= 0xF4u) {
            if (i + 3u >= text.size()) {
                return false;
            }
            const auto b1 = static_cast<std::uint8_t>(text[i + 1u]);
            const auto b2 = static_cast<std::uint8_t>(text[i + 2u]);
            const auto b3 = static_cast<std::uint8_t>(text[i + 3u]);
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
                return false;
            }
            if (b0 == 0xF0u && b1 < 0x90u) {
                return false;
            }
            if (b0 == 0xF4u && b1 >= 0x90u) {
                return false;
            }
            i += 4u;
            continue;
        }
        return false;
    }
    return true;
}

void AppendLe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
}

void AppendLe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
}

void AppendLe64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    AppendLe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
    AppendLe32(out, static_cast<std::uint32_t>((v >> 32u) & 0xFFFFFFFFULL));
}

std::uint16_t ReadLe16Checked(boost::span<const std::uint8_t> buf, std::size_t& cur) {
    if (buf.size() - cur < 2u) {
        throw std::runtime_error("ADIEUMEN container truncated reading uint16");
    }
    const auto v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(buf[cur]) |
                                              (static_cast<std::uint16_t>(buf[cur + 1u]) << 8u));
    cur += 2u;
    return v;
}

std::uint32_t ReadLe32Checked(boost::span<const std::uint8_t> buf, std::size_t& cur) {
    if (buf.size() - cur < 4u) {
        throw std::runtime_error("ADIEUMEN container truncated reading uint32");
    }
    const std::uint32_t v = static_cast<std::uint32_t>(buf[cur]) |
                            (static_cast<std::uint32_t>(buf[cur + 1u]) << 8u) |
                            (static_cast<std::uint32_t>(buf[cur + 2u]) << 16u) |
                            (static_cast<std::uint32_t>(buf[cur + 3u]) << 24u);
    cur += 4u;
    return v;
}

std::uint64_t ReadLe64Checked(boost::span<const std::uint8_t> buf, std::size_t& cur) {
    const std::uint64_t lo = ReadLe32Checked(buf, cur);
    const std::uint64_t hi = ReadLe32Checked(buf, cur);
    return lo | (hi << 32u);
}

bool ContainsBytes(boost::span<const std::uint8_t> haystack,
                   boost::span<const std::uint8_t> needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
    return it != haystack.end();
}

std::vector<std::uint8_t> AsciiToUtf16LeBytes(std::string_view ascii) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ascii.size() * 2u);
    for (const char ch : ascii) {
        bytes.push_back(static_cast<std::uint8_t>(ch));
        bytes.push_back(0x00u);
    }
    return bytes;
}

void ValidateRecordInput(const MenuPatchRecordInput& input) {
    if (!IsSafeRelativeAssetPath(input.source_rel_path)) {
        throw std::invalid_argument("Invalid source_rel_path in MenuPatchRecordInput: " +
                                    input.source_rel_path);
    }
    if (!IsAllowedMenuTargetPath(input.target_rel_path)) {
        throw std::invalid_argument("Invalid or forbidden target_rel_path in MenuPatchRecordInput: " +
                                    input.target_rel_path);
    }
    if (input.orig_slice.size() < kMinOrigSliceBytes ||
        input.orig_slice.size() > kMaxAssetPayloadBytes) {
        throw std::invalid_argument("MenuPatchRecordInput orig_slice size out of bounds [64, 128 MiB]");
    }
    if (input.new_raw.empty() || input.new_raw.size() > kMaxAssetPayloadBytes) {
        throw std::invalid_argument("MenuPatchRecordInput new_raw size out of bounds [1, 128 MiB]");
    }
    if ((input.flags & ~kFlagBackupStandalone) != 0u) {
        throw std::invalid_argument("MenuPatchRecordInput contains unsupported flags");
    }
}

}  // namespace

std::uint32_t MurmurHash2(const std::uint8_t* data, std::size_t len,
                          std::uint32_t seed) noexcept {
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);

    while (len >= 4u) {
        std::uint32_t k = static_cast<std::uint32_t>(data[0]) |
                          (static_cast<std::uint32_t>(data[1]) << 8u) |
                          (static_cast<std::uint32_t>(data[2]) << 16u) |
                          (static_cast<std::uint32_t>(data[3]) << 24u);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;

        h *= kMurmurMultiplier;
        h ^= k;

        data += 4u;
        len -= 4u;
    }

    switch (len) {
        case 3u:
            h ^= static_cast<std::uint32_t>(data[2]) << 16u;
            [[fallthrough]];
        case 2u:
            h ^= static_cast<std::uint32_t>(data[1]) << 8u;
            [[fallthrough]];
        case 1u:
            h ^= static_cast<std::uint32_t>(data[0]);
            h *= kMurmurMultiplier;
            break;
        default:
            break;
    }

    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

std::uint32_t MurmurHash2(boost::span<const std::uint8_t> data,
                          std::uint32_t seed) noexcept {
    return MurmurHash2(data.data(), data.size(), seed);
}

bool IsSafeRelativeAssetPath(std::string_view rel_path) noexcept {
    if (rel_path.size() < kMinMenuRelPathBytes || rel_path.size() > kMaxMenuRelPathBytes) {
        return false;
    }
    if (!IsValidUtf8NoControl(rel_path)) {
        return false;
    }
    for (const char ch : rel_path) {
        if (ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
            ch == '>' || ch == '|') {
            return false;
        }
    }
    if (rel_path.front() == '\\' || rel_path.back() == '\\') {
        return false;
    }

    std::size_t seg_start = 0u;
    while (seg_start <= rel_path.size()) {
        const std::size_t sep = rel_path.find('\\', seg_start);
        const std::string_view seg = (sep == std::string_view::npos)
                                         ? rel_path.substr(seg_start)
                                         : rel_path.substr(seg_start, sep - seg_start);
        if (!IsValidPathSegment(seg)) {
            return false;
        }
        if (sep == std::string_view::npos) {
            break;
        }
        seg_start = sep + 1u;
    }
    return true;
}

bool IsAllowedMenuTargetPath(std::string_view target_rel_path) noexcept {
    if (!IsSafeRelativeAssetPath(target_rel_path)) {
        return false;
    }
    const std::size_t sep = target_rel_path.rfind('\\');
    const std::string_view leaf =
        (sep == std::string_view::npos) ? target_rel_path : target_rel_path.substr(sep + 1u);
    if (EqualsIgnoreAsciiCase(leaf, "menu_patch.bin") ||
        EqualsIgnoreAsciiCase(leaf, "menu_patch.applied")) {
        return false;
    }
    if (EndsWithIgnoreAsciiCase(leaf, ".exe") || EndsWithIgnoreAsciiCase(leaf, ".dll") ||
        EndsWithIgnoreAsciiCase(leaf, ".bak") || EndsWithIgnoreAsciiCase(leaf, ".tmp")) {
        return false;
    }
    return true;
}

std::vector<std::uint8_t> ApplyMenuPatchXorMask(boost::span<const std::uint8_t> input,
                                                boost::span<const std::uint8_t> orig_slice,
                                                std::uint32_t orig_slice_hash) {
    if (orig_slice.size() < kMinOrigSliceBytes) {
        throw std::invalid_argument("ApplyMenuPatchXorMask requires orig_slice >= 64 bytes");
    }
    std::vector<std::uint8_t> output(input.size());
    const std::size_t orig_len = orig_slice.size();
    for (std::size_t k = 0u; k < input.size(); ++k) {
        const std::uint8_t slice_byte = orig_slice[k % orig_len];
        const unsigned int hash_shift =
            static_cast<unsigned int>((k & kHashByteCycleMask) * kBitsPerByte);
        const auto hash_byte = static_cast<std::uint8_t>((orig_slice_hash >> hash_shift) & 0xFFu);
        const auto stream_byte = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(k) * kXorStreamMultiplier + kXorStreamAddend) & 0xFFu);
        output[k] = static_cast<std::uint8_t>(input[k] ^ slice_byte ^ hash_byte ^ stream_byte);
    }
    return output;
}

MenuPatchManifest ParseMenuPatchContainer(boost::span<const std::uint8_t> blob) {
    if (blob.size() <= kMenuPatchHeaderBytes) {
        throw std::runtime_error("ADIEUMEN container smaller than 32-byte header + record");
    }
    if (std::memcmp(blob.data(), kMenuPatchMagic.data(), kMenuPatchMagic.size()) != 0) {
        throw std::runtime_error("ADIEUMEN container magic mismatch");
    }

    std::size_t cur = kMenuPatchMagic.size();
    const std::uint32_t version = ReadLe32Checked(blob, cur);
    if (version != kMenuPatchVersion) {
        throw std::runtime_error("Unsupported ADIEUMEN container version");
    }

    const std::uint32_t record_count = ReadLe32Checked(blob, cur);
    if (record_count < kMinMenuPatchRecords || record_count > kMaxMenuPatchRecords) {
        throw std::runtime_error("ADIEUMEN record_count out of bounds [1, 4096]");
    }

    const std::uint32_t stored_manifest_hash = ReadLe32Checked(blob, cur);
    for (std::size_t i = 0u; i < kMenuPatchReservedBytes; ++i) {
        if (blob[cur + i] != 0u) {
            throw std::runtime_error("ADIEUMEN header reserved bytes must be zero");
        }
    }
    cur += kMenuPatchReservedBytes;

    const std::uint32_t computed_manifest_hash =
        MurmurHash2(blob.data() + kMenuPatchHeaderBytes, blob.size() - kMenuPatchHeaderBytes, 0u);
    if (computed_manifest_hash != stored_manifest_hash) {
        throw std::runtime_error("ADIEUMEN manifest_hash mismatch");
    }

    MenuPatchManifest manifest;
    manifest.version = version;
    manifest.manifest_hash = stored_manifest_hash;
    manifest.records.reserve(record_count);

    std::unordered_set<std::string> seen_targets;
    seen_targets.reserve(record_count);

    for (std::uint32_t idx = 0u; idx < record_count; ++idx) {
        const std::uint16_t src_len = ReadLe16Checked(blob, cur);
        if (src_len < kMinMenuRelPathBytes || src_len > kMaxMenuRelPathBytes ||
            blob.size() - cur < src_len) {
            throw std::runtime_error("ADIEUMEN invalid or truncated source_rel_path");
        }
        std::string src_path(reinterpret_cast<const char*>(blob.data() + cur), src_len);
        cur += src_len;
        if (!IsSafeRelativeAssetPath(src_path)) {
            throw std::runtime_error("ADIEUMEN unsafe source_rel_path: " + src_path);
        }

        const std::uint16_t dst_len = ReadLe16Checked(blob, cur);
        if (dst_len < kMinMenuRelPathBytes || dst_len > kMaxMenuRelPathBytes ||
            blob.size() - cur < dst_len) {
            throw std::runtime_error("ADIEUMEN invalid or truncated target_rel_path");
        }
        std::string dst_path(reinterpret_cast<const char*>(blob.data() + cur), dst_len);
        cur += dst_len;
        if (!IsAllowedMenuTargetPath(dst_path)) {
            throw std::runtime_error("ADIEUMEN unsafe or forbidden target_rel_path: " + dst_path);
        }
        if (!seen_targets.insert(LowercaseAsciiCopy(dst_path)).second) {
            throw std::runtime_error("ADIEUMEN duplicate target_rel_path: " + dst_path);
        }

        if (blob.size() - cur < kRecordFixedMetadataBytes) {
            throw std::runtime_error("ADIEUMEN truncated record metadata");
        }
        MenuPatchRecord rec;
        rec.source_rel_path = std::move(src_path);
        rec.target_rel_path = std::move(dst_path);
        rec.orig_offset = ReadLe64Checked(blob, cur);
        rec.orig_size = ReadLe64Checked(blob, cur);
        rec.orig_slice_hash = ReadLe32Checked(blob, cur);
        rec.new_size = ReadLe64Checked(blob, cur);
        rec.new_raw_hash = ReadLe32Checked(blob, cur);
        rec.flags = ReadLe32Checked(blob, cur);

        if (rec.orig_size < kMinOrigSliceBytes || rec.orig_size > kMaxAssetPayloadBytes) {
            throw std::runtime_error("ADIEUMEN record orig_size out of bounds");
        }
        if (rec.new_size == 0u || rec.new_size > kMaxAssetPayloadBytes) {
            throw std::runtime_error("ADIEUMEN record new_size out of bounds");
        }
        if ((rec.flags & ~kFlagBackupStandalone) != 0u) {
            throw std::runtime_error("ADIEUMEN record contains unsupported flags");
        }
        if (blob.size() - cur < static_cast<std::size_t>(rec.new_size)) {
            throw std::runtime_error("ADIEUMEN truncated xor_payload");
        }
        rec.xor_payload.assign(blob.data() + cur,
                               blob.data() + cur + static_cast<std::size_t>(rec.new_size));
        cur += static_cast<std::size_t>(rec.new_size);
        manifest.records.push_back(std::move(rec));
    }

    if (cur != blob.size()) {
        throw std::runtime_error("ADIEUMEN container has trailing unparsed bytes");
    }
    return manifest;
}

std::vector<std::uint8_t> UnmaskMenuPatchRecord(const MenuPatchRecord& record,
                                                boost::span<const std::uint8_t> orig_slice) {
    if (orig_slice.size() != static_cast<std::size_t>(record.orig_size)) {
        throw std::runtime_error("UnmaskMenuPatchRecord orig_slice length mismatch");
    }
    const std::uint32_t actual_orig_hash = MurmurHash2(orig_slice, 0u);
    if (actual_orig_hash != record.orig_slice_hash) {
        throw std::runtime_error("UnmaskMenuPatchRecord orig_slice_hash mismatch");
    }
    if (record.xor_payload.size() != static_cast<std::size_t>(record.new_size)) {
        throw std::runtime_error("UnmaskMenuPatchRecord xor_payload length mismatch");
    }
    std::vector<std::uint8_t> unmasked =
        ApplyMenuPatchXorMask(record.xor_payload, orig_slice, record.orig_slice_hash);
    const std::uint32_t actual_new_hash = MurmurHash2(unmasked, 0u);
    if (actual_new_hash != record.new_raw_hash) {
        throw std::runtime_error("UnmaskMenuPatchRecord new_raw_hash mismatch");
    }
    return unmasked;
}

bool VerifyNoRawAssetSignatures(boost::span<const std::uint8_t> blob,
                                std::string* violation_out) {
    if (blob.size() <= kMenuPatchHeaderBytes) {
        if (violation_out != nullptr) {
            *violation_out = "Container smaller than 32-byte header";
        }
        return false;
    }

    const boost::span<const std::uint8_t> body(blob.data() + kMenuPatchHeaderBytes,
                                               blob.size() - kMenuPatchHeaderBytes);

    if (ContainsBytes(body, kPngMagic)) {
        if (violation_out != nullptr) {
            *violation_out = "Raw 8-byte PNG signature detected in container body";
        }
        return false;
    }

    const std::vector<std::uint8_t> gamename_utf16 = AsciiToUtf16LeBytes("#GAMENAME");
    if (ContainsBytes(body, gamename_utf16)) {
        if (violation_out != nullptr) {
            *violation_out = "Raw UTF-16LE #GAMENAME directive detected in container body";
        }
        return false;
    }

    const std::vector<std::uint8_t> discmark_utf16 = AsciiToUtf16LeBytes("#DISCMARK");
    if (ContainsBytes(body, discmark_utf16)) {
        if (violation_out != nullptr) {
            *violation_out = "Raw UTF-16LE #DISCMARK directive detected in container body";
        }
        return false;
    }

    std::size_t consecutive_zeros = 0u;
    for (const std::uint8_t byte : body) {
        if (byte == 0u) {
            ++consecutive_zeros;
            if (consecutive_zeros >= kForbiddenZeroRunBytes) {
                if (violation_out != nullptr) {
                    *violation_out = "Run of >= 64 consecutive zero bytes detected in container body";
                }
                return false;
            }
            continue;
        }
        consecutive_zeros = 0u;
    }
    return true;
}

void AssertNoRawAssetSignatures(boost::span<const std::uint8_t> blob) {
    std::string violation;
    if (!VerifyNoRawAssetSignatures(blob, &violation)) {
        throw std::runtime_error("ADIEUMEN raw-asset-signature check failed: " + violation);
    }
}

struct MenuPatchBuilder::Impl {
    std::vector<MenuPatchRecord> records;
    std::unordered_set<std::string> seen_targets;
};

MenuPatchBuilder::MenuPatchBuilder() : impl_(std::make_unique<Impl>()) {}
MenuPatchBuilder::MenuPatchBuilder(MenuPatchBuilder&& other) noexcept = default;
MenuPatchBuilder& MenuPatchBuilder::operator=(MenuPatchBuilder&& other) noexcept = default;
MenuPatchBuilder::~MenuPatchBuilder() = default;

void MenuPatchBuilder::AddRecord(MenuPatchRecordInput input) {
    if (impl_->records.size() >= kMaxMenuPatchRecords) {
        throw std::runtime_error("MenuPatchBuilder exceeded maximum record count (4096)");
    }
    ValidateRecordInput(input);

    std::string lower_target = LowercaseAsciiCopy(input.target_rel_path);
    if (impl_->seen_targets.find(lower_target) != impl_->seen_targets.end()) {
        throw std::invalid_argument("Duplicate target_rel_path in MenuPatchBuilder: " +
                                    input.target_rel_path);
    }

    const std::uint32_t orig_hash = MurmurHash2(input.orig_slice, 0u);
    const std::uint32_t new_hash = MurmurHash2(input.new_raw, 0u);
    std::vector<std::uint8_t> masked =
        ApplyMenuPatchXorMask(input.new_raw, input.orig_slice, orig_hash);

    MenuPatchRecord record;
    record.source_rel_path = std::move(input.source_rel_path);
    record.target_rel_path = std::move(input.target_rel_path);
    record.orig_offset = input.orig_offset;
    record.orig_size = static_cast<std::uint64_t>(input.orig_slice.size());
    record.orig_slice_hash = orig_hash;
    record.new_size = static_cast<std::uint64_t>(input.new_raw.size());
    record.new_raw_hash = new_hash;
    record.flags = input.flags;
    record.xor_payload = std::move(masked);

    impl_->seen_targets.insert(std::move(lower_target));
    impl_->records.push_back(std::move(record));
}

std::size_t MenuPatchBuilder::record_count() const noexcept { return impl_->records.size(); }

std::vector<std::uint8_t> MenuPatchBuilder::BuildBytes() const {
    if (impl_->records.size() < kMinMenuPatchRecords ||
        impl_->records.size() > kMaxMenuPatchRecords) {
        throw std::runtime_error("MenuPatchBuilder::BuildBytes requires 1..4096 records");
    }

    std::size_t body_bytes = 0u;
    for (const MenuPatchRecord& rec : impl_->records) {
        body_bytes += 2u + rec.source_rel_path.size() + 2u + rec.target_rel_path.size() +
                      kRecordFixedMetadataBytes + rec.xor_payload.size();
    }

    std::vector<std::uint8_t> out;
    out.reserve(kMenuPatchHeaderBytes + body_bytes);
    out.resize(kMenuPatchHeaderBytes, 0u);

    for (const MenuPatchRecord& rec : impl_->records) {
        AppendLe16(out, static_cast<std::uint16_t>(rec.source_rel_path.size()));
        out.insert(out.end(), rec.source_rel_path.begin(), rec.source_rel_path.end());
        AppendLe16(out, static_cast<std::uint16_t>(rec.target_rel_path.size()));
        out.insert(out.end(), rec.target_rel_path.begin(), rec.target_rel_path.end());
        AppendLe64(out, rec.orig_offset);
        AppendLe64(out, rec.orig_size);
        AppendLe32(out, rec.orig_slice_hash);
        AppendLe64(out, rec.new_size);
        AppendLe32(out, rec.new_raw_hash);
        AppendLe32(out, rec.flags);
        out.insert(out.end(), rec.xor_payload.begin(), rec.xor_payload.end());
    }

    const std::uint32_t manifest_hash =
        MurmurHash2(out.data() + kMenuPatchHeaderBytes, out.size() - kMenuPatchHeaderBytes, 0u);

    std::memcpy(out.data(), kMenuPatchMagic.data(), kMenuPatchMagic.size());
    std::vector<std::uint8_t> hdr_fields;
    hdr_fields.reserve(12u);
    AppendLe32(hdr_fields, kMenuPatchVersion);
    AppendLe32(hdr_fields, static_cast<std::uint32_t>(impl_->records.size()));
    AppendLe32(hdr_fields, manifest_hash);
    std::memcpy(out.data() + kMenuPatchMagic.size(), hdr_fields.data(), hdr_fields.size());

    AssertNoRawAssetSignatures(out);
    return out;
}

void MenuPatchBuilder::WriteToFile(const std::filesystem::path& output_path) const {
    const std::vector<std::uint8_t> blob = BuildBytes();
    if (output_path.has_parent_path() && !output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    const std::filesystem::path tmp_path = output_path.wstring() + L".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw std::runtime_error("MenuPatchBuilder::WriteToFile failed to open: " +
                                     tmp_path.u8string());
        }
        ofs.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        if (!ofs) {
            throw std::runtime_error("MenuPatchBuilder::WriteToFile write error: " +
                                     tmp_path.u8string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, output_path, ec);
    if (ec) {
        std::filesystem::remove(output_path, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, output_path, ec);
        if (ec) {
            throw std::runtime_error("MenuPatchBuilder::WriteToFile rename failed: " +
                                     output_path.u8string());
        }
    }
}

}  // namespace adi::menu
