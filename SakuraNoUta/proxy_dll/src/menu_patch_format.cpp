// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_patch_format.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace menu_patch_format {
namespace {

constexpr std::size_t kU16Bytes = 2u;
constexpr std::size_t kU32Bytes = 4u;
constexpr std::size_t kU64Bytes = 8u;
constexpr std::size_t kWordBytes = 4u;
constexpr std::size_t kRecordFixedTailBytes =
    kU64Bytes + kU64Bytes + kU32Bytes + kU64Bytes + kU32Bytes + kU32Bytes;

constexpr unsigned int kBitsPerByte = 8u;
constexpr unsigned int kShift8 = 8u;
constexpr unsigned int kShift16 = 16u;
constexpr unsigned int kShift24 = 24u;
constexpr int kMurmurAvalancheShift1 = 13;
constexpr int kMurmurAvalancheShift2 = 15;
constexpr std::size_t kHashByteCycleMask = 3u;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr std::uint64_t kMaxU32Value = 0xFFFFFFFFULL;

constexpr std::size_t kHeaderVersionOffset = 0x08u;
constexpr std::size_t kHeaderRecordCountOffset = 0x0Cu;
constexpr std::size_t kHeaderManifestHashOffset = 0x10u;
constexpr std::size_t kHeaderReservedOffset = 0x14u;

constexpr std::size_t kSentinelVersionOffset = 0x08u;
constexpr std::size_t kSentinelManifestHashOffset = 0x0Cu;
constexpr std::size_t kSentinelPatchSizeOffset = 0x10u;
constexpr std::size_t kSentinelEntryCountOffset = 0x18u;

constexpr std::size_t kArc20EntryCountOffset = 12u;
constexpr std::size_t kArc20RelOffsetFieldOffset = 96u;
constexpr std::size_t kArc20SizeFieldOffset = 100u;

constexpr std::uint8_t kMinPrintableAscii = 0x20u;
constexpr std::uint8_t kAsciiMax = 0x7Fu;
constexpr std::uint8_t kUtf8ContMask = 0xC0u;
constexpr std::uint8_t kUtf8ContTag = 0x80u;
constexpr std::uint8_t kUtf8ContValueMask = 0x3Fu;
constexpr unsigned int kUtf8ContShift = 6u;
constexpr std::size_t kUtf8TwoBytes = 2u;
constexpr std::size_t kUtf8ThreeBytes = 3u;
constexpr std::size_t kUtf8FourBytes = 4u;

constexpr std::uint8_t kTwoByteMinLead = 0xC2u;
constexpr std::uint8_t kTwoByteMaxLead = 0xDFu;
constexpr std::uint8_t kTwoByteValueMask = 0x1Fu;

constexpr std::uint8_t kThreeByteMinLead = 0xE0u;
constexpr std::uint8_t kThreeByteMaxLead = 0xEFu;
constexpr std::uint8_t kThreeByteValueMask = 0x0Fu;
constexpr std::uint8_t kOverlongThreeLead = 0xE0u;
constexpr std::uint8_t kOverlongThreeMinTrail = 0xA0u;
constexpr std::uint8_t kSurrogateThreeLead = 0xEDu;
constexpr std::uint8_t kSurrogateThreeMinTrail = 0xA0u;

constexpr std::uint8_t kFourByteMinLead = 0xF0u;
constexpr std::uint8_t kFourByteMaxLead = 0xF4u;
constexpr std::uint8_t kFourByteValueMask = 0x07u;
constexpr std::uint8_t kOverlongFourLead = 0xF0u;
constexpr std::uint8_t kOverlongFourMinTrail = 0x90u;
constexpr std::uint8_t kMaxUnicodeFourLead = 0xF4u;
constexpr std::uint8_t kMaxUnicodeFourMaxTrail = 0x8Fu;

constexpr std::uint32_t kBmpMaxCodePoint = 0xFFFFu;
constexpr std::uint32_t kSupplementalBase = 0x10000u;
constexpr unsigned int kSurrogateShift = 10u;
constexpr std::uint32_t kLowSurrogateMask = 0x3FFu;
constexpr std::uint16_t kHighSurrogateBase = 0xD800u;
constexpr std::uint16_t kLowSurrogateBase = 0xDC00u;

constexpr std::size_t kDosDeviceShortLen = 3u;
constexpr std::size_t kDosDevicePortLen = 4u;
constexpr std::size_t kDosDeviceDigitIndex = 3u;

std::uint16_t ReadLe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1])
                                   << kShift8));
}

std::uint32_t ReadLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << kShift8) |
           (static_cast<std::uint32_t>(p[2]) << kShift16) |
           (static_cast<std::uint32_t>(p[3]) << kShift24);
}

std::uint64_t ReadLe64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0u;
    for (unsigned int i = 0u; i < kU64Bytes; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (i * kBitsPerByte);
    }
    return value;
}

void WriteLe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kByteMask));
}

void WriteLe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (unsigned int i = 0u; i < kU32Bytes; ++i) {
        out.push_back(
            static_cast<std::uint8_t>((v >> (i * kBitsPerByte)) & kByteMask));
    }
}

void WriteLe32At(std::uint8_t* dst, std::uint32_t v) noexcept {
    for (unsigned int i = 0u; i < kU32Bytes; ++i) {
        dst[i] =
            static_cast<std::uint8_t>((v >> (i * kBitsPerByte)) & kByteMask);
    }
}

void WriteLe64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (unsigned int i = 0u; i < kU64Bytes; ++i) {
        out.push_back(
            static_cast<std::uint8_t>((v >> (i * kBitsPerByte)) & kByteMask));
    }
}

char ToAsciiLower(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

std::string LowercaseAsciiCopy(std::string_view sv) {
    std::string lowered(sv.size(), '\0');
    for (std::size_t i = 0u; i < sv.size(); ++i) {
        lowered[i] = ToAsciiLower(sv[i]);
    }
    return lowered;
}

bool EqualsIgnoreAsciiCase(std::string_view lhs,
                           std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0u; i < lhs.size(); ++i) {
        if (ToAsciiLower(lhs[i]) != ToAsciiLower(rhs[i])) {
            return false;
        }
    }
    return true;
}

bool EndsWithIgnoreAsciiCase(std::string_view text,
                             std::string_view suffix) noexcept {
    if (suffix.size() > text.size()) {
        return false;
    }
    return EqualsIgnoreAsciiCase(text.substr(text.size() - suffix.size()),
                                 suffix);
}

bool IsContinuationByte(std::uint8_t b) noexcept {
    return (b & kUtf8ContMask) == kUtf8ContTag;
}

bool DecodeNextUtf8CodePoint(std::string_view utf8,
                             std::size_t& pos,
                             std::uint32_t& cp_out) noexcept {
    const auto b0 = static_cast<std::uint8_t>(utf8[pos]);
    if (b0 <= kAsciiMax) {
        cp_out = b0;
        ++pos;
        return true;
    }

    const std::size_t rem = utf8.size() - pos;
    if (b0 >= kTwoByteMinLead && b0 <= kTwoByteMaxLead) {
        if (rem < kUtf8TwoBytes) {
            return false;
        }
        const auto b1 = static_cast<std::uint8_t>(utf8[pos + 1u]);
        if (!IsContinuationByte(b1)) {
            return false;
        }
        cp_out = (static_cast<std::uint32_t>(b0 & kTwoByteValueMask)
                  << kUtf8ContShift) |
                 static_cast<std::uint32_t>(b1 & kUtf8ContValueMask);
        pos += kUtf8TwoBytes;
        return true;
    }

    if (b0 >= kThreeByteMinLead && b0 <= kThreeByteMaxLead) {
        if (rem < kUtf8ThreeBytes) {
            return false;
        }
        const auto b1 = static_cast<std::uint8_t>(utf8[pos + 1u]);
        const auto b2 = static_cast<std::uint8_t>(utf8[pos + 2u]);
        if (!IsContinuationByte(b1) || !IsContinuationByte(b2)) {
            return false;
        }
        if (b0 == kOverlongThreeLead && b1 < kOverlongThreeMinTrail) {
            return false;
        }
        if (b0 == kSurrogateThreeLead && b1 >= kSurrogateThreeMinTrail) {
            return false;
        }
        cp_out = (static_cast<std::uint32_t>(b0 & kThreeByteValueMask)
                  << (2u * kUtf8ContShift)) |
                 (static_cast<std::uint32_t>(b1 & kUtf8ContValueMask)
                  << kUtf8ContShift) |
                 static_cast<std::uint32_t>(b2 & kUtf8ContValueMask);
        pos += kUtf8ThreeBytes;
        return true;
    }

    if (b0 >= kFourByteMinLead && b0 <= kFourByteMaxLead) {
        if (rem < kUtf8FourBytes) {
            return false;
        }
        const auto b1 = static_cast<std::uint8_t>(utf8[pos + 1u]);
        const auto b2 = static_cast<std::uint8_t>(utf8[pos + 2u]);
        const auto b3 = static_cast<std::uint8_t>(utf8[pos + 3u]);
        if (!IsContinuationByte(b1) || !IsContinuationByte(b2) ||
            !IsContinuationByte(b3)) {
            return false;
        }
        if (b0 == kOverlongFourLead && b1 < kOverlongFourMinTrail) {
            return false;
        }
        if (b0 == kMaxUnicodeFourLead && b1 > kMaxUnicodeFourMaxTrail) {
            return false;
        }
        cp_out = (static_cast<std::uint32_t>(b0 & kFourByteValueMask)
                  << (3u * kUtf8ContShift)) |
                 (static_cast<std::uint32_t>(b1 & kUtf8ContValueMask)
                  << (2u * kUtf8ContShift)) |
                 (static_cast<std::uint32_t>(b2 & kUtf8ContValueMask)
                  << kUtf8ContShift) |
                 static_cast<std::uint32_t>(b3 & kUtf8ContValueMask);
        pos += kUtf8FourBytes;
        return true;
    }

    return false;
}

bool IsValidUtf8(std::string_view sv) noexcept {
    std::size_t pos = 0u;
    std::uint32_t cp = 0u;
    while (pos < sv.size()) {
        if (!DecodeNextUtf8CodePoint(sv, pos, cp)) {
            return false;
        }
    }
    return true;
}

bool IsControlByte(char ch) noexcept {
    const auto u = static_cast<std::uint8_t>(ch);
    return u < kMinPrintableAscii || u == kAsciiMax;
}

bool IsForbiddenPathByte(char ch) noexcept {
    if (IsControlByte(ch)) {
        return true;
    }
    switch (ch) {
    case '/':
    case ':':
    case '<':
    case '>':
    case '"':
    case '|':
    case '?':
    case '*':
        return true;
    default:
        return false;
    }
}

bool IsDosReservedDeviceStem(std::string_view stem) noexcept {
    if (stem.size() == kDosDeviceShortLen) {
        return EqualsIgnoreAsciiCase(stem, "con") ||
               EqualsIgnoreAsciiCase(stem, "prn") ||
               EqualsIgnoreAsciiCase(stem, "aux") ||
               EqualsIgnoreAsciiCase(stem, "nul");
    }
    if (stem.size() == kDosDevicePortLen &&
        stem[kDosDeviceDigitIndex] >= '1' &&
        stem[kDosDeviceDigitIndex] <= '9') {
        const std::string_view prefix = stem.substr(0u, kDosDeviceShortLen);
        return EqualsIgnoreAsciiCase(prefix, "com") ||
               EqualsIgnoreAsciiCase(prefix, "lpt");
    }
    return false;
}

bool IsValidPathSegment(std::string_view segment) noexcept {
    if (segment.empty() || segment == "." || segment == "..") {
        return false;
    }
    const char last = segment.back();
    if (last == ' ' || last == '.') {
        return false;
    }
    const std::size_t dot = segment.find('.');
    const std::string_view stem =
        (dot == std::string_view::npos) ? segment : segment.substr(0u, dot);
    return !IsDosReservedDeviceStem(stem);
}

bool ReadLengthPrefixedField(ByteSpan blob,
                             std::size_t& cursor,
                             std::size_t min_len,
                             std::size_t max_len,
                             std::string& field_out,
                             bool& out_truncated) {
    out_truncated = false;
    if (kU16Bytes > blob.size() - cursor) {
        out_truncated = true;
        return false;
    }
    const std::uint16_t len = ReadLe16(blob.data() + cursor);
    cursor += kU16Bytes;
    if (static_cast<std::size_t>(len) < min_len ||
        static_cast<std::size_t>(len) > max_len) {
        return false;
    }
    if (static_cast<std::size_t>(len) > blob.size() - cursor) {
        out_truncated = true;
        return false;
    }
    field_out.assign(reinterpret_cast<const char*>(blob.data() + cursor),
                     static_cast<std::size_t>(len));
    cursor += static_cast<std::size_t>(len);
    return true;
}

struct TargetUniquenessTracker {
    std::unordered_set<std::string> standalone_targets;
    std::unordered_set<std::string> archive_targets;
    std::unordered_set<std::string> archive_entries;
};

ManifestStatus ValidateFlagsAndTargetUniqueness(
    std::string_view source_rel_path,
    std::string_view target_rel_path,
    std::string_view entry_name,
    std::uint32_t flags,
    TargetUniquenessTracker& tracker) {
    if ((flags & ~kKnownFlagsMask) != 0u) {
        return ManifestStatus::kUnsupportedFlags;
    }
    const bool is_archive = (flags & kFlagArchiveEntry) != 0u;
    const bool is_standalone_backup = (flags & kFlagBackupStandalone) != 0u;
    if (is_archive && is_standalone_backup) {
        return ManifestStatus::kUnsupportedFlags;
    }

    const std::string target_lower = LowercaseAsciiCopy(target_rel_path);
    if (is_archive) {
        if (!EqualsIgnoreAsciiCase(source_rel_path, target_rel_path) ||
            !IsValidArchiveEntryName(entry_name)) {
            return ManifestStatus::kInvalidPath;
        }
        if (tracker.standalone_targets.find(target_lower) !=
            tracker.standalone_targets.end()) {
            return ManifestStatus::kDuplicateTargetPath;
        }
        tracker.archive_targets.insert(target_lower);
        std::string composite_key = target_lower;
        composite_key.append("::");
        composite_key.append(LowercaseAsciiCopy(entry_name));
        if (!tracker.archive_entries.insert(std::move(composite_key)).second) {
            return ManifestStatus::kDuplicateTargetPath;
        }
        return ManifestStatus::kOk;
    }

    if (!entry_name.empty() && !IsValidArchiveEntryName(entry_name)) {
        return ManifestStatus::kInvalidPath;
    }
    if (tracker.archive_targets.find(target_lower) !=
        tracker.archive_targets.end()) {
        return ManifestStatus::kDuplicateTargetPath;
    }
    if (!tracker.standalone_targets.insert(target_lower).second) {
        return ManifestStatus::kDuplicateTargetPath;
    }
    return ManifestStatus::kOk;
}

std::string_view ExtractArc20EntryName(const std::uint8_t* entry_ptr) noexcept {
    std::size_t name_len = 0u;
    while (name_len < kArc20NameFieldBytes && entry_ptr[name_len] != 0u) {
        ++name_len;
    }
    return std::string_view(reinterpret_cast<const char*>(entry_ptr), name_len);
}

ArcStatus ValidateArc20Structure(ByteSpan arc_bytes,
                                 std::uint32_t& count_out,
                                 std::uint64_t& data_start_out) noexcept {
    count_out = 0u;
    data_start_out = 0u;
    if (arc_bytes.size() < kArc20HeaderBytes || arc_bytes.data() == nullptr) {
        return ArcStatus::kTooSmall;
    }

    const std::uint8_t* raw = arc_bytes.data();
    if (std::memcmp(raw, kArc20Magic, kArc20MagicBytes) != 0) {
        return ArcStatus::kBadMagic;
    }

    const std::uint32_t count = ReadLe32(raw + kArc20EntryCountOffset);
    const std::uint64_t data_start =
        static_cast<std::uint64_t>(kArc20HeaderBytes) +
        static_cast<std::uint64_t>(count) *
            static_cast<std::uint64_t>(kArc20IndexEntryBytes);
    const auto total_bytes = static_cast<std::uint64_t>(arc_bytes.size());
    if (data_start > total_bytes) {
        return ArcStatus::kTruncatedIndex;
    }

    const std::uint64_t data_section_bytes = total_bytes - data_start;
    for (std::uint32_t i = 0u; i < count; ++i) {
        const std::uint8_t* entry =
            raw + kArc20HeaderBytes +
            static_cast<std::size_t>(i) * kArc20IndexEntryBytes;
        const std::uint32_t rel_offset =
            ReadLe32(entry + kArc20RelOffsetFieldOffset);
        const std::uint32_t size = ReadLe32(entry + kArc20SizeFieldOffset);
        const std::uint64_t end_rel =
            static_cast<std::uint64_t>(rel_offset) +
            static_cast<std::uint64_t>(size);
        if (end_rel > data_section_bytes) {
            return ArcStatus::kEntryOutOfBounds;
        }
    }

    count_out = count;
    data_start_out = data_start;
    return ArcStatus::kOk;
}

}  // namespace

std::uint32_t MurmurHash2(const std::uint8_t* data,
                          std::size_t len,
                          std::uint32_t seed) noexcept {
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);
    const std::uint8_t* p = data;

    while (len >= kWordBytes) {
        std::uint32_t k = ReadLe32(p);
        k *= kMurmur2Multiplier;
        k ^= k >> kMurmur2Shift;
        k *= kMurmur2Multiplier;

        h *= kMurmur2Multiplier;
        h ^= k;

        p += kWordBytes;
        len -= kWordBytes;
    }

    switch (len) {
    case 3u:
        h ^= static_cast<std::uint32_t>(p[2]) << kShift16;
        [[fallthrough]];
    case 2u:
        h ^= static_cast<std::uint32_t>(p[1]) << kShift8;
        [[fallthrough]];
    case 1u:
        h ^= static_cast<std::uint32_t>(p[0]);
        h *= kMurmur2Multiplier;
        break;
    default:
        break;
    }

    h ^= h >> kMurmurAvalancheShift1;
    h *= kMurmur2Multiplier;
    h ^= h >> kMurmurAvalancheShift2;
    return h;
}

std::uint32_t MurmurHash2(ByteSpan bytes, std::uint32_t seed) noexcept {
    return MurmurHash2(bytes.data(), bytes.size(), seed);
}

std::uint8_t ComputeXorMaskByte(std::uint8_t orig_byte,
                                std::uint32_t orig_slice_hash,
                                std::size_t index) noexcept {
    const unsigned int shift =
        static_cast<unsigned int>((index & kHashByteCycleMask) * kBitsPerByte);
    const auto hash_byte =
        static_cast<std::uint8_t>((orig_slice_hash >> shift) & kByteMask);
    const auto salt_byte = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(index) * kXorSaltStride + kXorSaltOffset) &
        kByteMask);
    return static_cast<std::uint8_t>(orig_byte ^ hash_byte ^ salt_byte);
}

bool IsValidRelativePath(std::string_view rel_path) noexcept {
    if (rel_path.size() < kMinRelativePathBytes ||
        rel_path.size() > kMaxRelativePathBytes) {
        return false;
    }
    if (rel_path.front() == '\\' || rel_path.back() == '\\') {
        return false;
    }
    for (const char ch : rel_path) {
        if (IsForbiddenPathByte(ch)) {
            return false;
        }
    }
    if (!IsValidUtf8(rel_path)) {
        return false;
    }

    std::size_t seg_start = 0u;
    while (seg_start <= rel_path.size()) {
        const std::size_t sep = rel_path.find('\\', seg_start);
        const std::size_t seg_end =
            (sep == std::string_view::npos) ? rel_path.size() : sep;
        if (!IsValidPathSegment(
                rel_path.substr(seg_start, seg_end - seg_start))) {
            return false;
        }
        if (sep == std::string_view::npos) {
            break;
        }
        seg_start = sep + 1u;
    }
    return true;
}

bool IsAllowedTargetPath(std::string_view target_rel_path) noexcept {
    if (!IsValidRelativePath(target_rel_path)) {
        return false;
    }
    const std::size_t sep = target_rel_path.rfind('\\');
    const std::string_view leaf = (sep == std::string_view::npos)
                                      ? target_rel_path
                                      : target_rel_path.substr(sep + 1u);

    if (EqualsIgnoreAsciiCase(leaf, "menu_patch.bin") ||
        EqualsIgnoreAsciiCase(leaf, "menu_patch.applied") ||
        EqualsIgnoreAsciiCase(leaf, "translations.tsv")) {
        return false;
    }
    if (EndsWithIgnoreAsciiCase(leaf, ".exe") ||
        EndsWithIgnoreAsciiCase(leaf, ".dll") ||
        EndsWithIgnoreAsciiCase(leaf, ".bak") ||
        EndsWithIgnoreAsciiCase(leaf, ".backup") ||
        EndsWithIgnoreAsciiCase(leaf, ".tmp")) {
        return false;
    }
    return true;
}

bool IsValidArchiveEntryName(std::string_view entry_name) noexcept {
    if (entry_name.size() < kMinArchiveEntryNameBytes ||
        entry_name.size() > kMaxArchiveEntryNameBytes) {
        return false;
    }
    if (entry_name == "." || entry_name == "..") {
        return false;
    }
    for (const char ch : entry_name) {
        if (IsControlByte(ch) || ch == '/' || ch == '\\') {
            return false;
        }
    }
    return IsValidUtf8(entry_name);
}

bool Utf8PathToWide(std::string_view utf8_path, std::wstring& wide_out) {
    wide_out.clear();
    if (utf8_path.empty()) {
        return false;
    }
    wide_out.reserve(utf8_path.size());

    std::size_t pos = 0u;
    std::uint32_t cp = 0u;
    while (pos < utf8_path.size()) {
        if (!DecodeNextUtf8CodePoint(utf8_path, pos, cp)) {
            wide_out.clear();
            return false;
        }
        if (cp <= kBmpMaxCodePoint) {
            wide_out.push_back(static_cast<wchar_t>(cp));
            continue;
        }
        const std::uint32_t offset = cp - kSupplementalBase;
        const auto high = static_cast<wchar_t>(
            kHighSurrogateBase + (offset >> kSurrogateShift));
        const auto low = static_cast<wchar_t>(
            kLowSurrogateBase + (offset & kLowSurrogateMask));
        wide_out.push_back(high);
        wide_out.push_back(low);
    }
    return true;
}

const char* ManifestStatusName(ManifestStatus status) noexcept {
    switch (status) {
    case ManifestStatus::kOk:
        return "kOk";
    case ManifestStatus::kTooSmall:
        return "kTooSmall";
    case ManifestStatus::kBadMagic:
        return "kBadMagic";
    case ManifestStatus::kUnsupportedVersion:
        return "kUnsupportedVersion";
    case ManifestStatus::kInvalidRecordCount:
        return "kInvalidRecordCount";
    case ManifestStatus::kNonZeroReserved:
        return "kNonZeroReserved";
    case ManifestStatus::kManifestHashMismatch:
        return "kManifestHashMismatch";
    case ManifestStatus::kTruncatedRecord:
        return "kTruncatedRecord";
    case ManifestStatus::kInvalidPath:
        return "kInvalidPath";
    case ManifestStatus::kDuplicateTargetPath:
        return "kDuplicateTargetPath";
    case ManifestStatus::kOrigSliceSizeOutOfRange:
        return "kOrigSliceSizeOutOfRange";
    case ManifestStatus::kPayloadSizeOutOfRange:
        return "kPayloadSizeOutOfRange";
    case ManifestStatus::kUnsupportedFlags:
        return "kUnsupportedFlags";
    case ManifestStatus::kTrailingBytes:
        return "kTrailingBytes";
    }
    return "kUnknown";
}

ManifestStatus ParseMenuPatchHeader(ByteSpan header_bytes,
                                    MenuPatchHeader& out) noexcept {
    out = {};
    if (header_bytes.size() < kPatchHeaderBytes ||
        header_bytes.data() == nullptr) {
        return ManifestStatus::kTooSmall;
    }

    const std::uint8_t* raw = header_bytes.data();
    if (std::memcmp(raw, kPatchMagic, kMagicBytes) != 0) {
        return ManifestStatus::kBadMagic;
    }

    const std::uint32_t version = ReadLe32(raw + kHeaderVersionOffset);
    if (version != kFormatVersion) {
        return ManifestStatus::kUnsupportedVersion;
    }

    const std::uint32_t record_count = ReadLe32(raw + kHeaderRecordCountOffset);
    if (record_count < kMinRecordCount || record_count > kMaxRecordCount) {
        return ManifestStatus::kInvalidRecordCount;
    }

    for (std::size_t i = 0u; i < kPatchReservedBytes; ++i) {
        if (raw[kHeaderReservedOffset + i] != 0u) {
            return ManifestStatus::kNonZeroReserved;
        }
    }

    out.version = version;
    out.record_count = record_count;
    out.manifest_hash = ReadLe32(raw + kHeaderManifestHashOffset);
    return ManifestStatus::kOk;
}

ManifestStatus ParseMenuPatchManifest(ByteSpan blob, MenuPatchManifest& out) {
    out = {};
    MenuPatchHeader header{};
    const ManifestStatus hdr_status = ParseMenuPatchHeader(blob, header);
    if (hdr_status != ManifestStatus::kOk) {
        return hdr_status;
    }

    const ByteSpan body = blob.subspan(kPatchHeaderBytes);
    if (MurmurHash2(body, kDefaultSeed) != header.manifest_hash) {
        return ManifestStatus::kManifestHashMismatch;
    }

    std::vector<MenuPatchRecord> records;
    records.reserve(header.record_count);
    TargetUniquenessTracker tracker{};

    std::size_t cursor = kPatchHeaderBytes;
    for (std::uint32_t i = 0u; i < header.record_count; ++i) {
        MenuPatchRecord rec{};
        bool truncated = false;
        if (!ReadLengthPrefixedField(blob, cursor, kMinRelativePathBytes,
                                     kMaxRelativePathBytes, rec.source_rel_path,
                                     truncated)) {
            return truncated ? ManifestStatus::kTruncatedRecord
                             : ManifestStatus::kInvalidPath;
        }
        if (!IsValidRelativePath(rec.source_rel_path)) {
            return ManifestStatus::kInvalidPath;
        }

        if (!ReadLengthPrefixedField(blob, cursor, kMinRelativePathBytes,
                                     kMaxRelativePathBytes, rec.target_rel_path,
                                     truncated)) {
            return truncated ? ManifestStatus::kTruncatedRecord
                             : ManifestStatus::kInvalidPath;
        }
        if (!IsAllowedTargetPath(rec.target_rel_path)) {
            return ManifestStatus::kInvalidPath;
        }

        if (!ReadLengthPrefixedField(blob, cursor, 0u,
                                     kMaxArchiveEntryNameBytes, rec.entry_name,
                                     truncated)) {
            return truncated ? ManifestStatus::kTruncatedRecord
                             : ManifestStatus::kInvalidPath;
        }

        if (kRecordFixedTailBytes > blob.size() - cursor) {
            return ManifestStatus::kTruncatedRecord;
        }

        const std::uint8_t* meta = blob.data() + cursor;
        rec.orig_offset = ReadLe64(meta);
        meta += kU64Bytes;
        rec.orig_size = ReadLe64(meta);
        meta += kU64Bytes;
        rec.orig_slice_hash = ReadLe32(meta);
        meta += kU32Bytes;
        rec.new_size = ReadLe64(meta);
        meta += kU64Bytes;
        rec.new_raw_hash = ReadLe32(meta);
        meta += kU32Bytes;
        rec.flags = ReadLe32(meta);
        cursor += kRecordFixedTailBytes;

        const ManifestStatus target_rc = ValidateFlagsAndTargetUniqueness(
            rec.source_rel_path, rec.target_rel_path, rec.entry_name,
            rec.flags, tracker);
        if (target_rc != ManifestStatus::kOk) {
            return target_rc;
        }

        if (rec.orig_size < kMinOrigSliceBytes ||
            rec.orig_size > kMaxAssetPayloadBytes) {
            return ManifestStatus::kOrigSliceSizeOutOfRange;
        }
        if (rec.new_size == 0u || rec.new_size > kMaxAssetPayloadBytes) {
            return ManifestStatus::kPayloadSizeOutOfRange;
        }

        const auto payload_len = static_cast<std::size_t>(rec.new_size);
        if (payload_len > blob.size() - cursor) {
            return ManifestStatus::kTruncatedRecord;
        }

        rec.payload_offset = cursor;
        cursor += payload_len;
        records.push_back(std::move(rec));
    }

    if (cursor != blob.size()) {
        return ManifestStatus::kTrailingBytes;
    }

    out.header = header;
    out.records = std::move(records);
    return ManifestStatus::kOk;
}

ManifestStatus SerializeMenuPatchManifest(
    const std::vector<MenuPatchRecordInput>& inputs,
    std::vector<std::uint8_t>& blob_out) {
    blob_out.clear();
    if (inputs.size() < kMinRecordCount || inputs.size() > kMaxRecordCount) {
        return ManifestStatus::kInvalidRecordCount;
    }

    TargetUniquenessTracker tracker{};
    std::vector<std::uint8_t> out;
    out.resize(kPatchHeaderBytes, 0u);
    std::memcpy(out.data(), kPatchMagic, kMagicBytes);
    WriteLe32At(out.data() + kHeaderVersionOffset, kFormatVersion);
    WriteLe32At(out.data() + kHeaderRecordCountOffset,
                static_cast<std::uint32_t>(inputs.size()));

    for (const auto& rec : inputs) {
        if (!IsValidRelativePath(rec.source_rel_path) ||
            !IsAllowedTargetPath(rec.target_rel_path)) {
            return ManifestStatus::kInvalidPath;
        }

        const ManifestStatus target_rc = ValidateFlagsAndTargetUniqueness(
            rec.source_rel_path, rec.target_rel_path, rec.entry_name,
            rec.flags, tracker);
        if (target_rc != ManifestStatus::kOk) {
            return target_rc;
        }

        if (rec.orig_slice.size() < kMinOrigSliceBytes ||
            rec.orig_slice.size() > kMaxAssetPayloadBytes) {
            return ManifestStatus::kOrigSliceSizeOutOfRange;
        }
        if (rec.new_raw.empty() ||
            rec.new_raw.size() > kMaxAssetPayloadBytes) {
            return ManifestStatus::kPayloadSizeOutOfRange;
        }

        const std::uint32_t orig_hash =
            MurmurHash2(ByteSpan(rec.orig_slice), kDefaultSeed);
        const std::uint32_t new_hash =
            MurmurHash2(ByteSpan(rec.new_raw), kDefaultSeed);

        WriteLe16(out, static_cast<std::uint16_t>(rec.source_rel_path.size()));
        out.insert(out.end(), rec.source_rel_path.begin(),
                   rec.source_rel_path.end());

        WriteLe16(out, static_cast<std::uint16_t>(rec.target_rel_path.size()));
        out.insert(out.end(), rec.target_rel_path.begin(),
                   rec.target_rel_path.end());

        WriteLe16(out, static_cast<std::uint16_t>(rec.entry_name.size()));
        out.insert(out.end(), rec.entry_name.begin(), rec.entry_name.end());

        WriteLe64(out, rec.orig_offset);
        WriteLe64(out, static_cast<std::uint64_t>(rec.orig_slice.size()));
        WriteLe32(out, orig_hash);
        WriteLe64(out, static_cast<std::uint64_t>(rec.new_raw.size()));
        WriteLe32(out, new_hash);
        WriteLe32(out, rec.flags);

        const std::size_t orig_len = rec.orig_slice.size();
        for (std::size_t k = 0u; k < rec.new_raw.size(); ++k) {
            const std::uint8_t mask = ComputeXorMaskByte(
                rec.orig_slice[k % orig_len], orig_hash, k);
            out.push_back(static_cast<std::uint8_t>(rec.new_raw[k] ^ mask));
        }
    }

    const std::uint32_t manifest_hash =
        MurmurHash2(ByteSpan(out).subspan(kPatchHeaderBytes), kDefaultSeed);
    WriteLe32At(out.data() + kHeaderManifestHashOffset, manifest_hash);

    blob_out = std::move(out);
    return ManifestStatus::kOk;
}

const char* UnmaskStatusName(UnmaskStatus status) noexcept {
    switch (status) {
    case UnmaskStatus::kOk:
        return "kOk";
    case UnmaskStatus::kOrigSliceSizeOutOfRange:
        return "kOrigSliceSizeOutOfRange";
    case UnmaskStatus::kPayloadSizeOutOfRange:
        return "kPayloadSizeOutOfRange";
    case UnmaskStatus::kOrigHashMismatch:
        return "kOrigHashMismatch";
    case UnmaskStatus::kNewHashMismatch:
        return "kNewHashMismatch";
    }
    return "kUnknown";
}

UnmaskStatus UnmaskRecordPayload(ByteSpan xor_payload,
                                 ByteSpan orig_slice,
                                 std::uint32_t expected_orig_hash,
                                 std::uint32_t expected_new_hash,
                                 std::vector<std::uint8_t>& new_raw_out) {
    new_raw_out.clear();
    if (orig_slice.size() < kMinOrigSliceBytes ||
        orig_slice.size() > kMaxAssetPayloadBytes ||
        orig_slice.data() == nullptr) {
        return UnmaskStatus::kOrigSliceSizeOutOfRange;
    }
    if (xor_payload.empty() || xor_payload.size() > kMaxAssetPayloadBytes ||
        xor_payload.data() == nullptr) {
        return UnmaskStatus::kPayloadSizeOutOfRange;
    }

    if (MurmurHash2(orig_slice, kDefaultSeed) != expected_orig_hash) {
        return UnmaskStatus::kOrigHashMismatch;
    }

    std::vector<std::uint8_t> decoded(xor_payload.size());
    const std::size_t orig_len = orig_slice.size();
    for (std::size_t k = 0u; k < xor_payload.size(); ++k) {
        const std::uint8_t mask =
            ComputeXorMaskByte(orig_slice[k % orig_len], expected_orig_hash, k);
        decoded[k] = static_cast<std::uint8_t>(xor_payload[k] ^ mask);
    }

    if (MurmurHash2(ByteSpan(decoded), kDefaultSeed) != expected_new_hash) {
        return UnmaskStatus::kNewHashMismatch;
    }

    new_raw_out = std::move(decoded);
    return UnmaskStatus::kOk;
}

const char* SentinelStatusName(SentinelStatus status) noexcept {
    switch (status) {
    case SentinelStatus::kOk:
        return "kOk";
    case SentinelStatus::kTooSmall:
        return "kTooSmall";
    case SentinelStatus::kBadMagic:
        return "kBadMagic";
    case SentinelStatus::kUnsupportedVersion:
        return "kUnsupportedVersion";
    case SentinelStatus::kInvalidEntryCount:
        return "kInvalidEntryCount";
    case SentinelStatus::kTruncatedEntry:
        return "kTruncatedEntry";
    case SentinelStatus::kInvalidPath:
        return "kInvalidPath";
    case SentinelStatus::kInvalidExpectedSize:
        return "kInvalidExpectedSize";
    case SentinelStatus::kTrailingBytes:
        return "kTrailingBytes";
    case SentinelStatus::kHashMismatch:
        return "kHashMismatch";
    }
    return "kUnknown";
}

SentinelStatus SerializeAppliedSentinel(const AppliedSentinel& sentinel,
                                        std::vector<std::uint8_t>& bytes_out) {
    bytes_out.clear();
    if (sentinel.entries.size() < kMinRecordCount ||
        sentinel.entries.size() > kMaxRecordCount) {
        return SentinelStatus::kInvalidEntryCount;
    }

    std::vector<std::uint8_t> out;
    out.insert(out.end(), kSentinelMagic, kSentinelMagic + kMagicBytes);
    WriteLe32(out, kFormatVersion);
    WriteLe32(out, sentinel.manifest_hash);
    WriteLe64(out, sentinel.patch_file_size);
    WriteLe32(out, static_cast<std::uint32_t>(sentinel.entries.size()));

    for (const auto& entry : sentinel.entries) {
        if (!IsAllowedTargetPath(entry.rel_path)) {
            return SentinelStatus::kInvalidPath;
        }
        if (entry.expected_new_size == 0u ||
            entry.expected_new_size > kMaxAssetPayloadBytes) {
            return SentinelStatus::kInvalidExpectedSize;
        }
        WriteLe16(out, static_cast<std::uint16_t>(entry.rel_path.size()));
        out.insert(out.end(), entry.rel_path.begin(), entry.rel_path.end());
        WriteLe64(out, entry.expected_new_size);
    }

    const std::uint32_t digest = MurmurHash2(ByteSpan(out), kDefaultSeed);
    WriteLe32(out, digest);
    bytes_out = std::move(out);
    return SentinelStatus::kOk;
}

SentinelStatus ParseAppliedSentinel(ByteSpan blob, AppliedSentinel& out) {
    out = {};
    if (blob.size() < kSentinelHeaderBytes + kSentinelTrailerHashBytes ||
        blob.data() == nullptr) {
        return SentinelStatus::kTooSmall;
    }

    const std::uint8_t* raw = blob.data();
    if (std::memcmp(raw, kSentinelMagic, kMagicBytes) != 0) {
        return SentinelStatus::kBadMagic;
    }

    const std::size_t body_end = blob.size() - kSentinelTrailerHashBytes;
    const std::uint32_t expected_digest = ReadLe32(raw + body_end);
    if (MurmurHash2(ByteSpan(raw, body_end), kDefaultSeed) != expected_digest) {
        return SentinelStatus::kHashMismatch;
    }

    const std::uint32_t version = ReadLe32(raw + kSentinelVersionOffset);
    if (version != kFormatVersion) {
        return SentinelStatus::kUnsupportedVersion;
    }

    const std::uint32_t count = ReadLe32(raw + kSentinelEntryCountOffset);
    if (count < kMinRecordCount || count > kMaxRecordCount) {
        return SentinelStatus::kInvalidEntryCount;
    }

    AppliedSentinel parsed{};
    parsed.manifest_hash = ReadLe32(raw + kSentinelManifestHashOffset);
    parsed.patch_file_size = ReadLe64(raw + kSentinelPatchSizeOffset);
    parsed.entries.reserve(count);

    const ByteSpan prefix_span(raw, body_end);
    std::size_t cursor = kSentinelHeaderBytes;
    for (std::uint32_t i = 0u; i < count; ++i) {
        AppliedSentinelEntry entry{};
        bool truncated = false;
        if (!ReadLengthPrefixedField(prefix_span, cursor, kMinRelativePathBytes,
                                     kMaxRelativePathBytes, entry.rel_path,
                                     truncated)) {
            return truncated ? SentinelStatus::kTruncatedEntry
                             : SentinelStatus::kInvalidPath;
        }
        if (!IsAllowedTargetPath(entry.rel_path)) {
            return SentinelStatus::kInvalidPath;
        }
        if (kU64Bytes > prefix_span.size() - cursor) {
            return SentinelStatus::kTruncatedEntry;
        }
        entry.expected_new_size = ReadLe64(prefix_span.data() + cursor);
        cursor += kU64Bytes;
        if (entry.expected_new_size == 0u ||
            entry.expected_new_size > kMaxAssetPayloadBytes) {
            return SentinelStatus::kInvalidExpectedSize;
        }
        parsed.entries.push_back(std::move(entry));
    }

    if (cursor != body_end) {
        return SentinelStatus::kTrailingBytes;
    }

    out = std::move(parsed);
    return SentinelStatus::kOk;
}

const char* ArcStatusName(ArcStatus status) noexcept {
    switch (status) {
    case ArcStatus::kOk:
        return "kOk";
    case ArcStatus::kTooSmall:
        return "kTooSmall";
    case ArcStatus::kBadMagic:
        return "kBadMagic";
    case ArcStatus::kTruncatedIndex:
        return "kTruncatedIndex";
    case ArcStatus::kEntryOutOfBounds:
        return "kEntryOutOfBounds";
    case ArcStatus::kEntryNotFound:
        return "kEntryNotFound";
    case ArcStatus::kDuplicateReplacement:
        return "kDuplicateReplacement";
    }
    return "kUnknown";
}

ArcStatus FindArc20EntrySlice(ByteSpan arc_bytes,
                              std::string_view entry_name,
                              std::uint64_t& out_file_offset,
                              std::uint64_t& out_size) noexcept {
    out_file_offset = 0u;
    out_size = 0u;

    std::uint32_t count = 0u;
    std::uint64_t data_start = 0u;
    const ArcStatus struct_rc =
        ValidateArc20Structure(arc_bytes, count, data_start);
    if (struct_rc != ArcStatus::kOk) {
        return struct_rc;
    }
    if (entry_name.empty()) {
        return ArcStatus::kEntryNotFound;
    }

    const std::uint8_t* raw = arc_bytes.data();
    for (std::uint32_t i = 0u; i < count; ++i) {
        const std::uint8_t* entry =
            raw + kArc20HeaderBytes +
            static_cast<std::size_t>(i) * kArc20IndexEntryBytes;
        const std::string_view cur_name = ExtractArc20EntryName(entry);
        if (!EqualsIgnoreAsciiCase(cur_name, entry_name)) {
            continue;
        }
        const std::uint32_t rel_offset =
            ReadLe32(entry + kArc20RelOffsetFieldOffset);
        const std::uint32_t size = ReadLe32(entry + kArc20SizeFieldOffset);
        out_file_offset = data_start + static_cast<std::uint64_t>(rel_offset);
        out_size = static_cast<std::uint64_t>(size);
        return ArcStatus::kOk;
    }

    return ArcStatus::kEntryNotFound;
}

ArcStatus RepackArc20Entries(
    ByteSpan orig_arc,
    const std::vector<ArcEntryReplacement>& replacements,
    std::vector<std::uint8_t>& out_arc) {
    out_arc.clear();

    std::uint32_t count = 0u;
    std::uint64_t data_start = 0u;
    const ArcStatus struct_rc =
        ValidateArc20Structure(orig_arc, count, data_start);
    if (struct_rc != ArcStatus::kOk) {
        return struct_rc;
    }

    std::unordered_set<std::string> seen_rep_names;
    seen_rep_names.reserve(replacements.size());
    for (const auto& rep : replacements) {
        if (!rep.payload.empty() && rep.payload.data() == nullptr) {
            return ArcStatus::kEntryOutOfBounds;
        }
        if (!seen_rep_names.insert(LowercaseAsciiCopy(rep.entry_name)).second) {
            return ArcStatus::kDuplicateReplacement;
        }
    }
    for (const auto& rep : replacements) {
        if (rep.entry_name.empty()) {
            return ArcStatus::kEntryNotFound;
        }
    }

    std::vector<bool> matched(replacements.size(), false);
    const auto index_total_bytes =
        static_cast<std::size_t>(count) * kArc20IndexEntryBytes;

    std::vector<std::uint8_t> out_idx;
    out_idx.reserve(index_total_bytes);
    std::vector<std::uint8_t> out_data;
    out_data.reserve(orig_arc.size() - static_cast<std::size_t>(data_start));

    const std::uint8_t* raw = orig_arc.data();
    std::uint64_t cursor = 0u;

    for (std::uint32_t i = 0u; i < count; ++i) {
        const std::uint8_t* entry =
            raw + kArc20HeaderBytes +
            static_cast<std::size_t>(i) * kArc20IndexEntryBytes;
        const std::string_view cur_name = ExtractArc20EntryName(entry);

        ByteSpan chosen_payload{};
        bool replaced = false;
        for (std::size_t r = 0u; r < replacements.size(); ++r) {
            if (EqualsIgnoreAsciiCase(cur_name, replacements[r].entry_name)) {
                chosen_payload = replacements[r].payload;
                matched[r] = true;
                replaced = true;
                break;
            }
        }

        if (!replaced) {
            const std::uint32_t rel_offset =
                ReadLe32(entry + kArc20RelOffsetFieldOffset);
            const std::uint32_t size = ReadLe32(entry + kArc20SizeFieldOffset);
            chosen_payload = ByteSpan(
                raw + static_cast<std::size_t>(data_start) +
                    static_cast<std::size_t>(rel_offset),
                static_cast<std::size_t>(size));
        }

        if (chosen_payload.size() > kMaxU32Value ||
            cursor > kMaxU32Value - chosen_payload.size()) {
            return ArcStatus::kEntryOutOfBounds;
        }

        out_idx.insert(out_idx.end(), entry, entry + kArc20NameFieldBytes);
        WriteLe32(out_idx, static_cast<std::uint32_t>(cursor));
        WriteLe32(out_idx, static_cast<std::uint32_t>(chosen_payload.size()));
        out_idx.insert(
            out_idx.end(), entry + kArc20ExtraFieldOffset,
            entry + kArc20ExtraFieldOffset + kArc20ExtraFieldBytes);

        if (!chosen_payload.empty()) {
            out_data.insert(out_data.end(), chosen_payload.begin(),
                            chosen_payload.end());
        }
        cursor += static_cast<std::uint64_t>(chosen_payload.size());
    }

    for (const bool was_matched : matched) {
        if (!was_matched) {
            return ArcStatus::kEntryNotFound;
        }
    }

    std::vector<std::uint8_t> built;
    built.reserve(kArc20HeaderBytes + out_idx.size() + out_data.size());
    built.insert(built.end(), kArc20Magic, kArc20Magic + kArc20MagicBytes);
    WriteLe32(built, count);
    built.insert(built.end(), out_idx.begin(), out_idx.end());
    built.insert(built.end(), out_data.begin(), out_data.end());

    out_arc = std::move(built);
    return ArcStatus::kOk;
}

}  // namespace menu_patch_format
