// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_patch_format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace menu_patch {

namespace {

constexpr std::array<char, kMagicSize> kMagic = {'A', 'M', 'K', '2',
                                                 'M', 'E', 'N', 'U'};
constexpr std::size_t kHeaderVersionOffset = 8;
constexpr std::size_t kHeaderRecordCountOffset = 12;
constexpr std::size_t kHeaderManifestHashOffset = 16;
constexpr std::size_t kHeaderReservedOffset = 20;

constexpr std::size_t kU8Bytes = 1;
constexpr std::size_t kU16Bytes = 2;
constexpr std::size_t kU32Bytes = 4;
constexpr std::size_t kU64Bytes = 8;
constexpr std::size_t kRecordReservedFlagBytes = 3;
constexpr std::size_t kRecordU32FieldCount = 4;
// Smallest record: both paths empty and no payload.
constexpr std::size_t kMinRecordBytes = kU16Bytes + kU16Bytes + kU8Bytes +
                                        kRecordReservedFlagBytes + kU64Bytes +
                                        kRecordU32FieldCount * kU32Bytes;

constexpr unsigned kBitsPerByte = 8;
constexpr std::uint32_t kLowByteMask = 0xFFu;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr unsigned kMurmurShift = 24;
constexpr unsigned kMurmurFinalShift1 = 13;
constexpr unsigned kMurmurFinalShift2 = 15;
constexpr std::size_t kTailThreeBytes = 3;
constexpr std::size_t kTailTwoBytes = 2;
constexpr std::size_t kTailOneByte = 1;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;
constexpr unsigned kShiftByte1 = 8;
constexpr unsigned kShiftByte2 = 16;
constexpr unsigned kShiftByte3 = 24;

constexpr std::uint32_t kHashByteSelectMask = 3u;
constexpr std::uint32_t kKeyStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kKeyStreamAddend = 0x37u;

constexpr char kPathSeparator = '\\';
constexpr unsigned char kFirstPrintableAscii = 0x20u;
constexpr unsigned char kAsciiDelete = 0x7Fu;
constexpr std::string_view kReservedPathChars = "<>\"|?*:/";
constexpr std::string_view kCurrentDirComponent = ".";
constexpr std::string_view kParentDirComponent = "..";

constexpr std::array<std::string_view, 5> kForbiddenTargetExtensions = {
    ".int", ".exe", ".dll", ".bak", ".tmp",
};
constexpr std::array<std::string_view, 2> kForbiddenTargetNames = {
    "menu_patch.bin", "menu_patch.applied",
};

constexpr unsigned char kUtf8MaxOneByte = 0x7Fu;
constexpr unsigned char kUtf8ContMask = 0xC0u;
constexpr unsigned char kUtf8ContPrefix = 0x80u;
constexpr unsigned char kUtf8MinTwoByteLead = 0xC2u;
constexpr unsigned char kUtf8MaxTwoByteLead = 0xDFu;
constexpr unsigned char kUtf8ThreeByteOverlongLead = 0xE0u;
constexpr unsigned char kUtf8MaxThreeByteLead = 0xEFu;
constexpr unsigned char kUtf8SurrogateLead = 0xEDu;
constexpr unsigned char kUtf8MinThreeByteNonOverlongCont = 0xA0u;
constexpr unsigned char kUtf8MaxNonSurrogateCont = 0x9Fu;
constexpr unsigned char kUtf8FourByteOverlongLead = 0xF0u;
constexpr unsigned char kUtf8MaxFourByteLead = 0xF4u;
constexpr unsigned char kUtf8MinFourByteNonOverlongCont = 0x90u;
constexpr unsigned char kUtf8MaxScalarCont = 0x8Fu;
constexpr std::size_t kUtf8TwoByteLen = 2;
constexpr std::size_t kUtf8ThreeByteLen = 3;
constexpr std::size_t kUtf8FourByteLen = 4;

constexpr char kAsciiCaseOffset = 'a' - 'A';

constexpr std::array<char, kMagicSize> kSentinelMagic = {'A', 'M', 'K', '2',
                                                         'A', 'P', 'L', 'D'};
constexpr std::uint32_t kSentinelVersion = 1;

std::uint16_t ReadLeU16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[kByteIndex1]) << kShiftByte1));
}

std::uint32_t ReadLeU32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[kByteIndex1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[kByteIndex2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteIndex3]) << kShiftByte3);
}

std::uint64_t ReadLeU64(const std::uint8_t* p) noexcept {
    const std::uint64_t low = ReadLeU32(p);
    const std::uint64_t high = ReadLeU32(p + kU32Bytes);
    return low | (high << (kU32Bytes * kBitsPerByte));
}

void AppendLe(std::vector<std::uint8_t>& out,
              std::uint64_t value,
              std::size_t byte_count) {
    for (std::size_t i = 0; i < byte_count; ++i) {
        out.push_back(static_cast<std::uint8_t>(
            (value >> (i * kBitsPerByte)) & kLowByteMask));
    }
}

void AppendBytes(std::vector<std::uint8_t>& out, std::string_view bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

bool AllZero(const std::uint8_t* p, std::size_t len) noexcept {
    return std::all_of(p, p + len, [](std::uint8_t b) { return b == 0; });
}

char ToAsciiLower(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + kAsciiCaseOffset);
    }
    return c;
}

bool EndsWithIgnoreAsciiCase(std::string_view s,
                             std::string_view suffix) noexcept {
    if (s.size() < suffix.size()) {
        return false;
    }
    return PathsEqualIgnoreAsciiCase(s.substr(s.size() - suffix.size()),
                                     suffix);
}

std::string_view FileNameOf(std::string_view path) noexcept {
    const std::size_t sep = path.rfind(kPathSeparator);
    if (sep == std::string_view::npos) {
        return path;
    }
    return path.substr(sep + kU8Bytes);
}

bool IsUtf8Continuation(unsigned char b) noexcept {
    return (b & kUtf8ContMask) == kUtf8ContPrefix;
}

// Returns the length of the strictly valid UTF-8 scalar at `s[pos]`, or 0.
std::size_t ValidUtf8ScalarLength(std::string_view s, std::size_t pos) noexcept {
    const auto b0 = static_cast<unsigned char>(s[pos]);
    if (b0 <= kUtf8MaxOneByte) {
        return kU8Bytes;
    }
    std::size_t len = 0;
    if (b0 >= kUtf8MinTwoByteLead && b0 <= kUtf8MaxTwoByteLead) {
        len = kUtf8TwoByteLen;
    } else if (b0 >= kUtf8ThreeByteOverlongLead && b0 <= kUtf8MaxThreeByteLead) {
        len = kUtf8ThreeByteLen;
    } else if (b0 >= kUtf8FourByteOverlongLead && b0 <= kUtf8MaxFourByteLead) {
        len = kUtf8FourByteLen;
    }
    if (len == 0 || pos + len > s.size()) {
        return 0;
    }
    for (std::size_t i = kByteIndex1; i < len; ++i) {
        if (!IsUtf8Continuation(static_cast<unsigned char>(s[pos + i]))) {
            return 0;
        }
    }
    const auto b1 = static_cast<unsigned char>(s[pos + kByteIndex1]);
    if (b0 == kUtf8ThreeByteOverlongLead && b1 < kUtf8MinThreeByteNonOverlongCont) {
        return 0;
    }
    if (b0 == kUtf8SurrogateLead && b1 > kUtf8MaxNonSurrogateCont) {
        return 0;
    }
    if (b0 == kUtf8FourByteOverlongLead && b1 < kUtf8MinFourByteNonOverlongCont) {
        return 0;
    }
    if (b0 == kUtf8MaxFourByteLead && b1 > kUtf8MaxScalarCont) {
        return 0;
    }
    return len;
}

bool IsSafeComponent(std::string_view component) noexcept {
    if (component.empty() || component == kCurrentDirComponent ||
        component == kParentDirComponent) {
        return false;
    }
    const char last = component.back();
    return last != '.' && last != ' ';
}

// Sequential bounds-checked cursor over a byte range.
class ByteCursor {
public:
    explicit ByteCursor(ByteView view) noexcept : view_(view) {}

    bool Has(std::size_t count) const noexcept {
        return count <= view_.size - pos_;
    }
    std::size_t Position() const noexcept { return pos_; }
    bool AtEnd() const noexcept { return pos_ == view_.size; }

    bool Skip(std::size_t count) noexcept {
        if (!Has(count)) {
            return false;
        }
        pos_ += count;
        return true;
    }
    bool ReadU8(std::uint8_t& out) noexcept {
        if (!Has(kU8Bytes)) {
            return false;
        }
        out = view_.data[pos_];
        pos_ += kU8Bytes;
        return true;
    }
    bool ReadU16(std::uint16_t& out) noexcept {
        if (!Has(kU16Bytes)) {
            return false;
        }
        out = ReadLeU16(view_.data + pos_);
        pos_ += kU16Bytes;
        return true;
    }
    bool ReadU32(std::uint32_t& out) noexcept {
        if (!Has(kU32Bytes)) {
            return false;
        }
        out = ReadLeU32(view_.data + pos_);
        pos_ += kU32Bytes;
        return true;
    }
    bool ReadU64(std::uint64_t& out) noexcept {
        if (!Has(kU64Bytes)) {
            return false;
        }
        out = ReadLeU64(view_.data + pos_);
        pos_ += kU64Bytes;
        return true;
    }
    bool ReadString(std::size_t len, std::string& out) {
        if (!Has(len)) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(view_.data + pos_), len);
        pos_ += len;
        return true;
    }
    bool ReadLengthPrefixedString(std::string& out) {
        std::uint16_t len = 0;
        return ReadU16(len) && ReadString(len, out);
    }
    bool ReadZeroBytes(std::size_t len, bool& all_zero) noexcept {
        if (!Has(len)) {
            return false;
        }
        all_zero = AllZero(view_.data + pos_, len);
        pos_ += len;
        return true;
    }

private:
    ByteView view_;
    std::size_t pos_ = 0;
};

void AppendRecordDescriptor(std::vector<std::uint8_t>& out,
                            const MenuPatchRecord& record) {
    AppendLe(out, record.source_rel_path.size(), kU16Bytes);
    AppendBytes(out, record.source_rel_path);
    AppendLe(out, record.target_rel_path.size(), kU16Bytes);
    AppendBytes(out, record.target_rel_path);
    AppendLe(out, record.flags, kU8Bytes);
    AppendLe(out, record.orig_offset, kU64Bytes);
    AppendLe(out, record.orig_size, kU32Bytes);
    AppendLe(out, record.orig_slice_hash, kU32Bytes);
    AppendLe(out, record.new_raw_size, kU32Bytes);
    AppendLe(out, record.new_raw_hash, kU32Bytes);
}

ParseStatus ReadRecord(ByteCursor& cursor, MenuPatchRecord& record) {
    bool reserved_zero = false;
    if (!cursor.ReadLengthPrefixedString(record.source_rel_path) ||
        !cursor.ReadLengthPrefixedString(record.target_rel_path) ||
        !cursor.ReadU8(record.flags) ||
        !cursor.ReadZeroBytes(kRecordReservedFlagBytes, reserved_zero) ||
        !cursor.ReadU64(record.orig_offset) ||
        !cursor.ReadU32(record.orig_size) ||
        !cursor.ReadU32(record.orig_slice_hash) ||
        !cursor.ReadU32(record.new_raw_size) ||
        !cursor.ReadU32(record.new_raw_hash)) {
        return ParseStatus::kTruncated;
    }
    record.payload_offset = cursor.Position();
    if (!cursor.Skip(record.new_raw_size)) {
        return ParseStatus::kTruncated;
    }
    if (!reserved_zero) {
        return ParseStatus::kReservedNotZero;
    }
    if ((record.flags & static_cast<std::uint8_t>(~kKnownFlagsMask)) != 0) {
        return ParseStatus::kUnknownFlags;
    }
    if (record.orig_size < kMinOrigSliceSize) {
        return ParseStatus::kOrigSliceTooSmall;
    }
    if (record.new_raw_size == 0) {
        return ParseStatus::kEmptyPayload;
    }
    if (!IsSafeRelativePath(record.source_rel_path) ||
        !IsSafeRelativePath(record.target_rel_path)) {
        return ParseStatus::kUnsafePath;
    }
    const bool same_path = PathsEqualIgnoreAsciiCase(record.source_rel_path,
                                                     record.target_rel_path);
    if (IsBackupStandalone(record) != same_path) {
        return ParseStatus::kFlagPathMismatch;
    }
    if (IsForbiddenTarget(record.target_rel_path)) {
        return ParseStatus::kForbiddenTarget;
    }
    return ParseStatus::kOk;
}

bool HasDuplicateTarget(const std::vector<MenuPatchRecord>& records,
                        const MenuPatchRecord& candidate) noexcept {
    return std::any_of(records.begin(), records.end(),
                       [&candidate](const MenuPatchRecord& r) {
                           return PathsEqualIgnoreAsciiCase(
                               r.target_rel_path, candidate.target_rel_path);
                       });
}

}  // namespace

std::uint32_t MurmurHash2Seeded(const std::uint8_t* data,
                                std::size_t len,
                                std::uint32_t seed) noexcept {
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);
    std::size_t i = 0;
    while (len - i >= kU32Bytes) {
        std::uint32_t k = ReadLeU32(data + i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShift;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kU32Bytes;
    }
    const std::size_t tail = len - i;
    if (tail == kTailThreeBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteIndex2]) << kShiftByte2;
    }
    if (tail >= kTailTwoBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteIndex1]) << kShiftByte1;
    }
    if (tail >= kTailOneByte) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

std::uint32_t MurmurHash2(const std::uint8_t* data, std::size_t len) noexcept {
    constexpr std::uint32_t kZeroSeed = 0;
    return MurmurHash2Seeded(data, len, kZeroSeed);
}

std::uint8_t XorMaskByte(ByteView orig_slice,
                         std::uint32_t orig_slice_hash,
                         std::size_t index) noexcept {
    const auto k = static_cast<std::uint32_t>(index);
    const std::uint32_t hash_shift = (k & kHashByteSelectMask) * kBitsPerByte;
    const auto hash_byte = static_cast<std::uint8_t>(
        (orig_slice_hash >> hash_shift) & kLowByteMask);
    const auto stream_byte = static_cast<std::uint8_t>(
        (k * kKeyStreamMultiplier + kKeyStreamAddend) & kLowByteMask);
    return static_cast<std::uint8_t>(orig_slice.data[index % orig_slice.size] ^
                                     hash_byte ^ stream_byte);
}

const char* ParseStatusName(ParseStatus status) noexcept {
    switch (status) {
    case ParseStatus::kOk:
        return "ok";
    case ParseStatus::kTooSmall:
        return "file smaller than header";
    case ParseStatus::kBadMagic:
        return "bad magic";
    case ParseStatus::kBadVersion:
        return "unsupported format version";
    case ParseStatus::kReservedNotZero:
        return "reserved bytes not zero";
    case ParseStatus::kTruncated:
        return "truncated record";
    case ParseStatus::kUnknownFlags:
        return "unknown record flags";
    case ParseStatus::kOrigSliceTooSmall:
        return "original slice smaller than minimum";
    case ParseStatus::kEmptyPayload:
        return "empty payload";
    case ParseStatus::kUnsafePath:
        return "unsafe relative path";
    case ParseStatus::kFlagPathMismatch:
        return "backup flag does not match source/target equality";
    case ParseStatus::kForbiddenTarget:
        return "forbidden target file";
    case ParseStatus::kDuplicateTarget:
        return "duplicate target path";
    case ParseStatus::kTrailingBytes:
        return "trailing bytes after last record";
    case ParseStatus::kManifestHashMismatch:
        return "manifest hash mismatch";
    }
    return "unknown";
}

bool IsBackupStandalone(const MenuPatchRecord& record) noexcept {
    return (record.flags & kFlagBackupStandalone) != 0;
}

ParseStatus ParseMenuPatchHeader(ByteView container, MenuPatchHeader& out) {
    if (container.data == nullptr || container.size < kFileHeaderSize) {
        return ParseStatus::kTooSmall;
    }
    if (std::memcmp(container.data, kMagic.data(), kMagic.size()) != 0) {
        return ParseStatus::kBadMagic;
    }
    const std::uint32_t version =
        ReadLeU32(container.data + kHeaderVersionOffset);
    if (version != kFormatVersion) {
        return ParseStatus::kBadVersion;
    }
    if (!AllZero(container.data + kHeaderReservedOffset,
                 kFileHeaderSize - kHeaderReservedOffset)) {
        return ParseStatus::kReservedNotZero;
    }
    out.format_version = version;
    out.record_count = ReadLeU32(container.data + kHeaderRecordCountOffset);
    out.manifest_hash = ReadLeU32(container.data + kHeaderManifestHashOffset);
    return ParseStatus::kOk;
}

ParseStatus ParseMenuPatchContainer(ByteView container,
                                    MenuPatchManifest& out) {
    MenuPatchManifest manifest;
    const ParseStatus header_status =
        ParseMenuPatchHeader(container, manifest.header);
    if (header_status != ParseStatus::kOk) {
        return header_status;
    }

    ByteCursor cursor(container);
    cursor.Skip(kFileHeaderSize);
    const std::size_t max_possible_records =
        (container.size - kFileHeaderSize) / kMinRecordBytes;
    manifest.records.reserve(std::min<std::size_t>(
        manifest.header.record_count, max_possible_records));

    std::vector<std::uint8_t> descriptors;
    for (std::uint32_t i = 0; i < manifest.header.record_count; ++i) {
        MenuPatchRecord record;
        const ParseStatus status = ReadRecord(cursor, record);
        if (status != ParseStatus::kOk) {
            return status;
        }
        if (HasDuplicateTarget(manifest.records, record)) {
            return ParseStatus::kDuplicateTarget;
        }
        AppendRecordDescriptor(descriptors, record);
        manifest.records.push_back(std::move(record));
    }
    if (!cursor.AtEnd()) {
        return ParseStatus::kTrailingBytes;
    }
    if (MurmurHash2(descriptors.data(), descriptors.size()) !=
        manifest.header.manifest_hash) {
        return ParseStatus::kManifestHashMismatch;
    }
    out = std::move(manifest);
    return ParseStatus::kOk;
}

bool IsSafeRelativePath(std::string_view utf8_path) noexcept {
    if (utf8_path.empty()) {
        return false;
    }
    std::size_t pos = 0;
    while (pos < utf8_path.size()) {
        const auto b = static_cast<unsigned char>(utf8_path[pos]);
        if (b < kFirstPrintableAscii || b == kAsciiDelete ||
            kReservedPathChars.find(static_cast<char>(b)) !=
                std::string_view::npos) {
            return false;
        }
        const std::size_t len = ValidUtf8ScalarLength(utf8_path, pos);
        if (len == 0) {
            return false;
        }
        pos += len;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t sep = utf8_path.find(kPathSeparator, start);
        const std::string_view component =
            utf8_path.substr(start, sep == std::string_view::npos
                                        ? std::string_view::npos
                                        : sep - start);
        if (!IsSafeComponent(component)) {
            return false;
        }
        if (sep == std::string_view::npos) {
            return true;
        }
        start = sep + kU8Bytes;
    }
}

bool IsForbiddenTarget(std::string_view utf8_path) noexcept {
    const bool forbidden_extension = std::any_of(
        kForbiddenTargetExtensions.begin(), kForbiddenTargetExtensions.end(),
        [utf8_path](std::string_view ext) {
            return EndsWithIgnoreAsciiCase(utf8_path, ext);
        });
    if (forbidden_extension) {
        return true;
    }
    const std::string_view name = FileNameOf(utf8_path);
    return std::any_of(kForbiddenTargetNames.begin(), kForbiddenTargetNames.end(),
                       [name](std::string_view forbidden) {
                           return PathsEqualIgnoreAsciiCase(name, forbidden);
                       });
}

bool PathsEqualIgnoreAsciiCase(std::string_view lhs,
                               std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (ToAsciiLower(lhs[i]) != ToAsciiLower(rhs[i])) {
            return false;
        }
    }
    return true;
}

const char* UnmaskStatusName(UnmaskStatus status) noexcept {
    switch (status) {
    case UnmaskStatus::kOk:
        return "ok";
    case UnmaskStatus::kPayloadOutOfRange:
        return "payload outside container";
    case UnmaskStatus::kOrigSliceSizeMismatch:
        return "original slice size mismatch";
    case UnmaskStatus::kOrigSliceHashMismatch:
        return "original slice hash mismatch";
    case UnmaskStatus::kNewRawHashMismatch:
        return "reconstructed hash mismatch";
    }
    return "unknown";
}

UnmaskStatus UnmaskRecordPayload(ByteView container,
                                 const MenuPatchRecord& record,
                                 ByteView orig_slice,
                                 std::vector<std::uint8_t>& new_raw_out) {
    if (container.data == nullptr ||
        record.payload_offset > container.size ||
        record.new_raw_size > container.size - record.payload_offset) {
        return UnmaskStatus::kPayloadOutOfRange;
    }
    if (orig_slice.data == nullptr || orig_slice.size != record.orig_size ||
        orig_slice.size == 0) {
        return UnmaskStatus::kOrigSliceSizeMismatch;
    }
    if (MurmurHash2(orig_slice.data, orig_slice.size) !=
        record.orig_slice_hash) {
        return UnmaskStatus::kOrigSliceHashMismatch;
    }
    std::vector<std::uint8_t> new_raw(record.new_raw_size);
    const std::uint8_t* const payload = container.data + record.payload_offset;
    for (std::size_t k = 0; k < new_raw.size(); ++k) {
        new_raw[k] = static_cast<std::uint8_t>(
            payload[k] ^ XorMaskByte(orig_slice, record.orig_slice_hash, k));
    }
    if (MurmurHash2(new_raw.data(), new_raw.size()) != record.new_raw_hash) {
        return UnmaskStatus::kNewRawHashMismatch;
    }
    new_raw_out = std::move(new_raw);
    return UnmaskStatus::kOk;
}

// Sentinel layout (little-endian):
//   char magic[8] = "AMK2APLD", u32 version, u32 manifest_hash,
//   u64 patch_file_size, u32 target_count,
//   target_count * ([u16 path_len][path][u64 size]),
//   u32 MurmurHash2 of every preceding byte.
std::vector<std::uint8_t> SerializeAppliedSentinel(
    const AppliedSentinel& sentinel) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kSentinelMagic.begin(), kSentinelMagic.end());
    AppendLe(out, kSentinelVersion, kU32Bytes);
    AppendLe(out, sentinel.manifest_hash, kU32Bytes);
    AppendLe(out, sentinel.patch_file_size, kU64Bytes);
    AppendLe(out, sentinel.targets.size(), kU32Bytes);
    for (const AppliedTarget& target : sentinel.targets) {
        AppendLe(out, target.target_rel_path.size(), kU16Bytes);
        AppendBytes(out, target.target_rel_path);
        AppendLe(out, target.size, kU64Bytes);
    }
    AppendLe(out, MurmurHash2(out.data(), out.size()), kU32Bytes);
    return out;
}

bool ParseAppliedSentinel(ByteView bytes, AppliedSentinel& out) {
    if (bytes.data == nullptr || bytes.size < kMagicSize + kU32Bytes) {
        return false;
    }
    const std::size_t body_size = bytes.size - kU32Bytes;
    if (MurmurHash2(bytes.data, body_size) !=
        ReadLeU32(bytes.data + body_size)) {
        return false;
    }
    if (std::memcmp(bytes.data, kSentinelMagic.data(), kSentinelMagic.size()) !=
        0) {
        return false;
    }

    ByteCursor cursor(ByteView{bytes.data, body_size});
    cursor.Skip(kMagicSize);
    std::uint32_t version = 0;
    std::uint32_t target_count = 0;
    AppliedSentinel sentinel;
    if (!cursor.ReadU32(version) || version != kSentinelVersion ||
        !cursor.ReadU32(sentinel.manifest_hash) ||
        !cursor.ReadU64(sentinel.patch_file_size) ||
        !cursor.ReadU32(target_count)) {
        return false;
    }
    for (std::uint32_t i = 0; i < target_count; ++i) {
        AppliedTarget target;
        if (!cursor.ReadLengthPrefixedString(target.target_rel_path) ||
            !cursor.ReadU64(target.size) ||
            !IsSafeRelativePath(target.target_rel_path)) {
            return false;
        }
        sentinel.targets.push_back(std::move(target));
    }
    if (!cursor.AtEnd()) {
        return false;
    }
    out = std::move(sentinel);
    return true;
}

}  // namespace menu_patch
