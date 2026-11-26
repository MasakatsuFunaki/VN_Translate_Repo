// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_autopatch.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace menu_autopatch {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kYpfMagicSize = 4u;
constexpr char kYpfMagic[kYpfMagicSize] = {'Y', 'P', 'F', '\0'};
constexpr std::size_t kYpfHeaderSize = 32u;
constexpr std::size_t kYpfEntryPrefixSize = 5u;
constexpr std::size_t kYpfEntryFooterSize = 22u;
constexpr std::size_t kFileCountOffset = 8u;
constexpr std::size_t kIdxSizeOffset = 12u;
constexpr std::size_t kNameSizeRawOffset = 4u;
constexpr std::size_t kFooterCompressedOffset = 1u;
constexpr std::size_t kFooterRawSizeOffset = 2u;
constexpr std::size_t kFooterPackedSizeOffset = 6u;
constexpr std::size_t kFooterDataOffsetOffset = 10u;
constexpr std::size_t kFooterDataHashOffset = 18u;
constexpr std::uint8_t kYpfNameXorKey = 0xFFu;

constexpr std::uint8_t kMaxEntryType = 20u;
constexpr std::uint8_t kMaxCompressedFlag = 1u;
constexpr std::uint8_t kAsciiPrintMin = 0x20u;
constexpr std::uint8_t kAsciiPrintMax = 0x7Eu;
constexpr std::uint8_t kHalfwidthKanaMin = 0xA1u;
constexpr std::uint8_t kHalfwidthKanaMax = 0xDFu;
constexpr std::uint8_t kCp932LeadMin1 = 0x81u;
constexpr std::uint8_t kCp932LeadMax1 = 0x9Fu;
constexpr std::uint8_t kCp932LeadMin2 = 0xE0u;
constexpr std::uint8_t kCp932LeadMax2 = 0xFCu;
constexpr std::uint8_t kCp932TrailMin1 = 0x40u;
constexpr std::uint8_t kCp932TrailMax1 = 0x7Eu;
constexpr std::uint8_t kCp932TrailMin2 = 0x80u;
constexpr std::uint8_t kCp932TrailMax2 = 0xFCu;
constexpr std::uint8_t kExtensionDotByte = 0x2Eu;
constexpr int kHintLookaheadMargin = 8;
constexpr int kMaxProbedNameLength = 255;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::size_t kWordBytes = 4u;
constexpr std::size_t kU64Bytes = 8u;
constexpr std::size_t kDoubleByteCharLen = 2u;
constexpr std::size_t kRemThreeBytes = 3u;
constexpr std::size_t kRemTwoBytes = 2u;
constexpr std::size_t kRemOneByte = 1u;
constexpr std::size_t kByteOffset1 = 1u;
constexpr std::size_t kByteOffset2 = 2u;
constexpr std::size_t kByteOffset3 = 3u;
constexpr int kBitsPerByte = 8;
constexpr int kShiftByte1 = 8;
constexpr int kShiftByte2 = 16;
constexpr int kShiftByte3 = 24;

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
constexpr std::uint64_t kLowDwordMask = 0xFFFFFFFFULL;
constexpr std::size_t kBufferGrowthFactor = 2u;
constexpr std::size_t kMinYpfIndexEntrySize =
    kYpfEntryPrefixSize + 1u + kYpfEntryFooterSize;

constexpr std::size_t kSentinelMagicSize = 8u;
constexpr char kSentinelMagic[kSentinelMagicSize] = {
    'F', 'R', 'A', 'T', 'A', 'P', 'L', 'D'};
constexpr std::uint32_t kSentinelVersion = 1u;
constexpr std::size_t kSentinelSize = 40u;
constexpr std::size_t kSentinelVersionOffset = 8u;
constexpr std::size_t kSentinelMurmurOffset = 12u;
constexpr std::size_t kSentinelFnv64Offset = 16u;
constexpr std::size_t kSentinelPatchSizeOffset = 24u;
constexpr std::size_t kSentinelPatchedYpfSizeOffset = 32u;

constexpr std::uint64_t kFnv64OffsetBasis = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnv64Prime = 0x100000001B3ULL;
constexpr std::size_t kHashChunkBytes = 16384u;
constexpr std::size_t kIoChunkBytes = 1u << 24;
constexpr int kShiftHighDword = 32;

struct YpfIndexEntry {
    std::uint32_t name_hash = 0;
    std::uint8_t name_size_raw = 0;
    Bytes name_bytes_xor;
    std::uint8_t type = 0;
    std::uint8_t compressed = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t packed_size = 0;
    std::uint64_t data_offset = 0;
    std::uint32_t data_hash = 0;
};

struct PngHeaderInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
};

struct PayloadBlock {
    std::uint64_t orig_offset = 0;
    std::uint32_t orig_packed_size = 0;
    std::vector<std::size_t> entry_indices;
    Bytes trailing_bytes;
};

class ScopedNamedMutex {
public:
    ScopedNamedMutex(const wchar_t* name, DWORD timeout_ms) noexcept
        : handle_(CreateMutexW(nullptr, FALSE, name)) {
        if (handle_ != nullptr) {
            const DWORD rc = WaitForSingleObject(handle_, timeout_ms);
            acquired_ = (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED);
        }
    }
    ScopedNamedMutex(const ScopedNamedMutex&) = delete;
    ScopedNamedMutex& operator=(const ScopedNamedMutex&) = delete;
    ~ScopedNamedMutex() {
        if (acquired_ && handle_ != nullptr) {
            ReleaseMutex(handle_);
        }
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    bool acquired() const noexcept { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

class ScopedFileHandle {
public:
    explicit ScopedFileHandle(HANDLE h) noexcept : handle_(h) {}
    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;
    ~ScopedFileHandle() { reset(); }

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const noexcept { return handle_; }
    void reset() noexcept {
        if (valid()) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

}  // namespace

static std::uint32_t rd_le_u32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[kByteOffset1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[kByteOffset2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteOffset3]) << kShiftByte3);
}

static std::uint32_t rd_le_u32(const Bytes& b, std::size_t off) noexcept {
    return rd_le_u32(b.data() + off);
}

static std::uint64_t rd_le_u64_ptr(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(rd_le_u32(p)) |
           (static_cast<std::uint64_t>(rd_le_u32(p + kWordBytes)) << kShiftHighDword);
}

static void wr_le_u32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & kLowByteMask);
    p[kByteOffset1] = static_cast<std::uint8_t>((v >> kShiftByte1) & kLowByteMask);
    p[kByteOffset2] = static_cast<std::uint8_t>((v >> kShiftByte2) & kLowByteMask);
    p[kByteOffset3] = static_cast<std::uint8_t>((v >> kShiftByte3) & kLowByteMask);
}

static void wr_le_u64(std::uint8_t* p, std::uint64_t v) noexcept {
    wr_le_u32(p, static_cast<std::uint32_t>(v & kLowDwordMask));
    wr_le_u32(p + kWordBytes, static_cast<std::uint32_t>((v >> kShiftHighDword) & kLowDwordMask));
}

static std::uint64_t rd_le_u64(const Bytes& b, std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = kU64Bytes; i > 0; --i) {
        v = (v << kBitsPerByte) | b[off + i - 1];
    }
    return v;
}

static std::uint32_t rd_be_u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << kShiftByte3) |
           (static_cast<std::uint32_t>(p[kByteOffset1]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteOffset2]) << kShiftByte1) |
           static_cast<std::uint32_t>(p[kByteOffset3]);
}

static void append_le_u32(Bytes& out, std::uint32_t v) {
    for (std::size_t i = 0; i < kWordBytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

static void append_le_u64(Bytes& out, std::uint64_t v) {
    for (std::size_t i = 0; i < kU64Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

static bool is_cp932_lead(std::uint8_t b) {
    return (b >= kCp932LeadMin1 && b <= kCp932LeadMax1) ||
           (b >= kCp932LeadMin2 && b <= kCp932LeadMax2);
}

static bool is_cp932_trail(std::uint8_t b) {
    return (b >= kCp932TrailMin1 && b <= kCp932TrailMax1) ||
           (b >= kCp932TrailMin2 && b <= kCp932TrailMax2);
}

static std::uint64_t compute_fnv64(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint64_t fnv = kFnv64OffsetBasis;
    for (std::size_t i = 0; i < len; ++i) {
        fnv ^= static_cast<std::uint64_t>(data[i]);
        fnv *= kFnv64Prime;
    }
    return fnv;
}

static std::uint32_t ypf_murmur_hash2(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h = static_cast<std::uint32_t>(len);
    std::size_t i = 0;
    while (len - i >= kWordBytes) {
        std::uint32_t k = rd_le_u32(data + i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }
    const std::size_t rem = len - i;
    if (rem == kRemThreeBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset2]) << kShiftByte2;
    }
    if (rem >= kRemTwoBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset1]) << kShiftByte1;
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

static bool is_valid_cp932_entry_name(const Bytes& idx, std::size_t off, std::size_t len,
                                      bool require_dot) {
    std::size_t j = 0;
    bool has_dot = false;
    while (j < len) {
        const std::uint8_t d = static_cast<std::uint8_t>(idx[off + j] ^ kYpfNameXorKey);
        if (d == kExtensionDotByte) {
            has_dot = true;
        }
        if ((d >= kAsciiPrintMin && d <= kAsciiPrintMax) ||
            (d >= kHalfwidthKanaMin && d <= kHalfwidthKanaMax)) {
            ++j;
            continue;
        }
        if (is_cp932_lead(d)) {
            if (j + kDoubleByteCharLen > len) {
                return false;
            }
            const std::uint8_t trail =
                static_cast<std::uint8_t>(idx[off + j + kByteOffset1] ^ kYpfNameXorKey);
            if (!is_cp932_trail(trail)) {
                return false;
            }
            j += kDoubleByteCharLen;
            continue;
        }
        return false;
    }
    return !require_dot || has_dot;
}

static bool is_valid_ascii_entry_name(const Bytes& idx, std::size_t off, std::size_t len) {
    for (std::size_t j = 0; j < len; ++j) {
        const std::uint8_t d = static_cast<std::uint8_t>(idx[off + j] ^ kYpfNameXorKey);
        if (d < kAsciiPrintMin || d > kAsciiPrintMax) {
            return false;
        }
    }
    return true;
}

static bool is_valid_entry_footer(const Bytes& idx, std::size_t tail) {
    if (tail > idx.size() || idx.size() - tail < kYpfEntryFooterSize) {
        return false;
    }
    const std::uint8_t t = idx[tail];
    const std::uint8_t c = idx[tail + kFooterCompressedOffset];
    if (t > kMaxEntryType || c > kMaxCompressedFlag) {
        return false;
    }
    const std::uint32_t raw = rd_le_u32(idx, tail + kFooterRawSizeOffset);
    const std::uint32_t packed = rd_le_u32(idx, tail + kFooterPackedSizeOffset);
    return (c == 0 && raw == packed) || (c == 1 && raw > 0);
}

static int probe_entry_name_length(const Bytes& idx, std::size_t off, int hinted) {
    if (off >= idx.size() || idx.size() - off <= kYpfEntryFooterSize) {
        return 0;
    }
    const std::size_t room_sz = idx.size() - off - kYpfEntryFooterSize;
    const int room = static_cast<int>(
        std::min<std::size_t>(room_sz, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int max_cp932_try =
        std::min(std::max(hinted + kHintLookaheadMargin, kMaxProbedNameLength), room);
    for (int n = 1; n <= max_cp932_try; ++n) {
        const std::size_t len = static_cast<std::size_t>(n);
        if (!is_valid_entry_footer(idx, off + len)) {
            continue;
        }
        if (is_valid_cp932_entry_name(idx, off, len, true)) {
            return n;
        }
    }
    const int max_ascii_try = std::min(hinted + kHintLookaheadMargin, room);
    for (int n = 1; n <= max_ascii_try; ++n) {
        const std::size_t len = static_cast<std::size_t>(n);
        if (!is_valid_entry_footer(idx, off + len)) {
            continue;
        }
        if (is_valid_ascii_entry_name(idx, off, len)) {
            return n;
        }
    }
    return 0;
}

static bool parse_ypf_index(const Bytes& file, std::vector<YpfIndexEntry>& out_entries) {
    out_entries.clear();
    if (file.size() < kYpfHeaderSize ||
        std::memcmp(file.data(), kYpfMagic, kYpfMagicSize) != 0) {
        return false;
    }
    const std::uint32_t file_count = rd_le_u32(file, kFileCountOffset);
    const std::uint32_t idx_size = rd_le_u32(file, kIdxSizeOffset);
    if (static_cast<std::size_t>(idx_size) > file.size()) {
        return false;
    }
    const std::size_t idx_begin = kYpfHeaderSize;
    const std::size_t idx_len =
        std::min<std::size_t>(file.size() - idx_begin, static_cast<std::size_t>(idx_size));
    const std::size_t idx_end = idx_begin + idx_len;
    const Bytes idx(file.begin() + static_cast<std::ptrdiff_t>(idx_begin),
                    file.begin() + static_cast<std::ptrdiff_t>(idx_end));

    if (static_cast<std::size_t>(file_count) > idx.size() / kMinYpfIndexEntrySize) {
        return false;
    }
    out_entries.reserve(file_count);
    std::size_t off = 0;
    for (std::uint32_t k = 0; k < file_count; ++k) {
        if (idx.size() - off < kYpfEntryPrefixSize) {
            return false;
        }
        const std::uint32_t name_hash = rd_le_u32(idx, off);
        const std::uint8_t name_size_raw = idx[off + kNameSizeRawOffset];
        off += kYpfEntryPrefixSize;

        const int hinted = static_cast<int>(name_size_raw ^ kYpfNameXorKey);
        int name_len = probe_entry_name_length(idx, off, hinted);
        if (name_len == 0) {
            name_len = hinted;
        }
        if (name_len <= 0 ||
            static_cast<std::size_t>(name_len) > idx.size() - off ||
            idx.size() - off - static_cast<std::size_t>(name_len) < kYpfEntryFooterSize) {
            return false;
        }

        const std::size_t nlen = static_cast<std::size_t>(name_len);
        YpfIndexEntry e;
        e.name_hash = name_hash;
        e.name_size_raw = name_size_raw;
        e.name_bytes_xor.assign(idx.begin() + static_cast<std::ptrdiff_t>(off),
                                idx.begin() + static_cast<std::ptrdiff_t>(off + nlen));
        off += nlen;

        e.type = idx[off];
        e.compressed = idx[off + kFooterCompressedOffset];
        e.raw_size = rd_le_u32(idx, off + kFooterRawSizeOffset);
        e.packed_size = rd_le_u32(idx, off + kFooterPackedSizeOffset);
        e.data_offset = rd_le_u64(idx, off + kFooterDataOffsetOffset);
        e.data_hash = rd_le_u32(idx, off + kFooterDataHashOffset);
        off += kYpfEntryFooterSize;
        out_entries.push_back(std::move(e));
    }
    return out_entries.size() == file_count;
}

static bool parse_png_ihdr_ptr(const std::uint8_t* data, std::size_t size, PngHeaderInfo& out) {
    if (size < kMinPngHeaderBytes ||
        std::memcmp(data, kPngSignature, kPngSignatureSize) != 0) {
        return false;
    }
    const std::uint32_t ihdr_len = rd_be_u32(data + kPngIhdrLengthOffset);
    if (ihdr_len != kExpectedIhdrDataLength ||
        std::memcmp(data + kPngIhdrTypeOffset, "IHDR", kChunkTypeLength) != 0) {
        return false;
    }
    out.width = rd_be_u32(data + kPngIhdrWidthOffset);
    out.height = rd_be_u32(data + kPngIhdrHeightOffset);
    out.bit_depth = data[kPngIhdrBitDepthOffset];
    out.color_type = data[kPngIhdrColorTypeOffset];
    return out.width > 0 && out.height > 0;
}

static std::uint8_t compute_xor_mask_byte(const std::uint8_t* orig_payload,
                                          std::size_t orig_size,
                                          std::uint32_t orig_data_hash,
                                          std::size_t byte_index) {
    const std::uint32_t idx32 = static_cast<std::uint32_t>(byte_index);
    const std::uint32_t shift =
        (idx32 & kHashByteIndexMask) * static_cast<std::uint32_t>(kBitsPerByte);
    const std::uint8_t hash_byte =
        static_cast<std::uint8_t>((orig_data_hash >> shift) & kLowByteMask);
    const std::uint8_t stream_byte =
        static_cast<std::uint8_t>((idx32 * kXorStreamMultiplier + kXorStreamAddend) & kLowByteMask);
    return static_cast<std::uint8_t>(
        orig_payload[byte_index % orig_size] ^ hash_byte ^ stream_byte);
}

static bool get_file_size_exact(const std::wstring& path, std::uint64_t& out_size) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    out_size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << kShiftHighDword) |
               static_cast<std::uint64_t>(fad.nFileSizeLow);
    return true;
}

static bool read_file_prefix(const std::wstring& path, std::uint8_t* dst, DWORD bytes_to_read) noexcept {
    ScopedFileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }
    DWORD got = 0;
    return ReadFile(file.get(), dst, bytes_to_read, &got, nullptr) && got == bytes_to_read;
}

static bool read_entire_file(const std::wstring& path, Bytes& out) {
    out.clear();
    ScopedFileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        return false;
    }
    LARGE_INTEGER li = {};
    if (!GetFileSizeEx(file.get(), &li) || li.QuadPart <= 0 ||
        static_cast<std::uint64_t>(li.QuadPart) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    out.resize(static_cast<std::size_t>(li.QuadPart));
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t rem = out.size() - done;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(rem, kIoChunkBytes));
        DWORD got = 0;
        if (!ReadFile(file.get(), out.data() + done, chunk, &got, nullptr) || got == 0u) {
            out.clear();
            return false;
        }
        done += got;
    }
    return true;
}

static bool write_and_flush_file(const std::wstring& path,
                                 const std::uint8_t* data,
                                 std::size_t size) noexcept {
    DeleteFileW(path.c_str());
    ScopedFileHandle out(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!out.valid()) {
        return false;
    }
    std::size_t written_total = 0u;
    while (written_total < size) {
        const std::size_t rem = size - written_total;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(rem, kIoChunkBytes));
        DWORD put = 0u;
        if (!WriteFile(out.get(), data + written_total, chunk, &put, nullptr) || put != chunk) {
            const DWORD gle = GetLastError();
            out.reset();
            DeleteFileW(path.c_str());
            SetLastError(gle);
            return false;
        }
        written_total += put;
    }
    if (!FlushFileBuffers(out.get())) {
        const DWORD gle = GetLastError();
        out.reset();
        DeleteFileW(path.c_str());
        SetLastError(gle);
        return false;
    }
    return true;
}

static bool write_and_commit_atomically(const std::wstring& target_path,
                                        const std::uint8_t* data,
                                        std::size_t size) noexcept {
    const std::wstring tmp_path = target_path + L".tmp";
    if (!write_and_flush_file(tmp_path, data, size)) {
        return false;
    }
    if (!MoveFileExW(tmp_path.c_str(), target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        DeleteFileW(tmp_path.c_str());
        SetLastError(gle);
        return false;
    }
    return true;
}

static bool hash_file_contents(const std::wstring& path,
                               std::uint64_t expected_size,
                               std::uint32_t& out_murmur32,
                               std::uint64_t& out_fnv64) noexcept {
    out_murmur32 = 0u;
    out_fnv64 = 0u;
    if (expected_size == 0u ||
        expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    ScopedFileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        return false;
    }
    LARGE_INTEGER li = {};
    if (!GetFileSizeEx(file.get(), &li) || li.QuadPart < 0 ||
        static_cast<std::uint64_t>(li.QuadPart) != expected_size) {
        return false;
    }

    std::uint32_t h = static_cast<std::uint32_t>(expected_size);
    std::uint64_t fnv = kFnv64OffsetBasis;
    std::array<std::uint8_t, kWordBytes> tail{};
    std::size_t tail_len = 0u;
    std::array<std::uint8_t, kHashChunkBytes> chunk{};
    std::uint64_t total_read = 0u;

    while (total_read < expected_size) {
        const std::uint64_t rem = expected_size - total_read;
        const DWORD want = static_cast<DWORD>(
            std::min<std::uint64_t>(rem, static_cast<std::uint64_t>(chunk.size())));
        DWORD got = 0u;
        if (!ReadFile(file.get(), chunk.data(), want, &got, nullptr) || got == 0u) {
            return false;
        }
        total_read += got;

        for (DWORD i = 0u; i < got; ++i) {
            fnv ^= static_cast<std::uint64_t>(chunk[i]);
            fnv *= kFnv64Prime;
        }

        std::size_t pos = 0u;
        const std::size_t avail = static_cast<std::size_t>(got);
        if (tail_len > 0u) {
            const std::size_t need = kWordBytes - tail_len;
            const std::size_t take = std::min(need, avail);
            std::memcpy(tail.data() + tail_len, chunk.data(), take);
            tail_len += take;
            pos += take;
            if (tail_len == kWordBytes) {
                std::uint32_t k = rd_le_u32(tail.data());
                k *= kMurmurMultiplier;
                k ^= k >> kMurmurShiftR;
                k *= kMurmurMultiplier;
                h *= kMurmurMultiplier;
                h ^= k;
                tail_len = 0u;
            }
        }

        while (avail - pos >= kWordBytes) {
            std::uint32_t k = rd_le_u32(chunk.data() + pos);
            k *= kMurmurMultiplier;
            k ^= k >> kMurmurShiftR;
            k *= kMurmurMultiplier;
            h *= kMurmurMultiplier;
            h ^= k;
            pos += kWordBytes;
        }

        if (pos < avail) {
            tail_len = avail - pos;
            std::memcpy(tail.data(), chunk.data() + pos, tail_len);
        }
    }

    if (tail_len == kRemThreeBytes) {
        h ^= static_cast<std::uint32_t>(tail[kByteOffset2]) << kShiftByte2;
    }
    if (tail_len >= kRemTwoBytes) {
        h ^= static_cast<std::uint32_t>(tail[kByteOffset1]) << kShiftByte1;
    }
    if (tail_len >= kRemOneByte) {
        h ^= static_cast<std::uint32_t>(tail[0]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;

    out_murmur32 = h;
    out_fnv64 = fnv;
    return true;
}

static bool is_sentinel_matching(const std::wstring& sentinel_path,
                                 std::uint32_t expected_murmur32,
                                 std::uint64_t expected_fnv64,
                                 std::uint64_t expected_patch_size,
                                 std::uint64_t expected_patched_ypf_size) noexcept {
    std::uint64_t sent_size = 0u;
    if (!get_file_size_exact(sentinel_path, sent_size) || sent_size != kSentinelSize) {
        return false;
    }
    std::array<std::uint8_t, kSentinelSize> buf{};
    if (!read_file_prefix(sentinel_path, buf.data(), static_cast<DWORD>(buf.size()))) {
        return false;
    }
    if (std::memcmp(buf.data(), kSentinelMagic, kSentinelMagicSize) != 0 ||
        rd_le_u32(buf.data() + kSentinelVersionOffset) != kSentinelVersion) {
        return false;
    }
    return rd_le_u32(buf.data() + kSentinelMurmurOffset) == expected_murmur32 &&
           rd_le_u64_ptr(buf.data() + kSentinelFnv64Offset) == expected_fnv64 &&
           rd_le_u64_ptr(buf.data() + kSentinelPatchSizeOffset) == expected_patch_size &&
           rd_le_u64_ptr(buf.data() + kSentinelPatchedYpfSizeOffset) == expected_patched_ypf_size;
}

static bool write_sentinel_atomically(const std::wstring& sentinel_path,
                                      std::uint32_t murmur32,
                                      std::uint64_t fnv64,
                                      std::uint64_t patch_size,
                                      std::uint64_t patched_ypf_size) noexcept {
    std::array<std::uint8_t, kSentinelSize> buf{};
    std::memcpy(buf.data(), kSentinelMagic, kSentinelMagicSize);
    wr_le_u32(buf.data() + kSentinelVersionOffset, kSentinelVersion);
    wr_le_u32(buf.data() + kSentinelMurmurOffset, murmur32);
    wr_le_u64(buf.data() + kSentinelFnv64Offset, fnv64);
    wr_le_u64(buf.data() + kSentinelPatchSizeOffset, patch_size);
    wr_le_u64(buf.data() + kSentinelPatchedYpfSizeOffset, patched_ypf_size);
    return write_and_commit_atomically(sentinel_path, buf.data(), buf.size());
}

static std::wstring parent_dir_of_module(HMODULE mod) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0u) {
            return {};
        }
        if (len < buf.size() - 1u) {
            buf.resize(len);
            const std::size_t slash = buf.find_last_of(L"\\/");
            return (slash == std::wstring::npos) ? std::wstring{} : buf.substr(0, slash);
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
}

bool ApplyXorMenuPatchInMemory(
    const std::vector<std::uint8_t>& orig_ypf,
    const std::vector<std::uint8_t>& patch_blob,
    std::vector<std::uint8_t>& out_patched_ypf) {
    out_patched_ypf.clear();
    if (patch_blob.size() < kPatchHeaderSize ||
        std::memcmp(patch_blob.data(), kPatchMagic, kPatchMagicSize) != 0) {
        return false;
    }
    const std::uint32_t version = rd_le_u32(patch_blob, kPatchVersionOffset);
    if (version != kPatchFormatVersion) {
        return false;
    }
    const std::uint32_t expected_entries =
        rd_le_u32(patch_blob, kPatchTotalEntriesOffset);
    const std::uint32_t expected_orig_size =
        rd_le_u32(patch_blob, kPatchOrigSizeOffset);
    const std::uint32_t patch_entry_count =
        rd_le_u32(patch_blob, kPatchEntryCountOffset);

    if (expected_entries == 0 || orig_ypf.size() != expected_orig_size) {
        return false;
    }

    std::vector<YpfIndexEntry> entries;
    if (!parse_ypf_index(orig_ypf, entries) || entries.size() != expected_entries) {
        return false;
    }

    const std::size_t max_patch_records =
        (patch_blob.size() - kPatchHeaderSize) / kPatchRecordHeaderSize;
    if (static_cast<std::size_t>(patch_entry_count) > max_patch_records) {
        return false;
    }

    std::unordered_map<std::size_t, Bytes> replacements;
    replacements.reserve(patch_entry_count);
    std::size_t off = kPatchHeaderSize;

    for (std::uint32_t r = 0; r < patch_entry_count; ++r) {
        if (patch_blob.size() - off < kPatchRecordHeaderSize) {
            return false;
        }
        const std::uint32_t entry_index = rd_le_u32(patch_blob, off);
        const std::uint32_t orig_data_hash =
            rd_le_u32(patch_blob, off + kRecordOrigHashOffset);
        const std::uint32_t new_raw_size =
            rd_le_u32(patch_blob, off + kRecordNewSizeOffset);
        const std::uint32_t new_data_hash =
            rd_le_u32(patch_blob, off + kRecordNewHashOffset);
        off += kPatchRecordHeaderSize;

        if (entry_index >= entries.size() || new_raw_size == 0 ||
            static_cast<std::size_t>(new_raw_size) > patch_blob.size() - off) {
            return false;
        }

        const YpfIndexEntry& oe = entries[entry_index];
        if (oe.compressed != 0 || oe.raw_size != oe.packed_size || oe.packed_size == 0 ||
            oe.data_offset > orig_ypf.size() ||
            static_cast<std::uint64_t>(oe.packed_size) > orig_ypf.size() - oe.data_offset) {
            return false;
        }

        const std::uint8_t* orig_ptr =
            orig_ypf.data() + static_cast<std::size_t>(oe.data_offset);
        const std::size_t orig_len = oe.packed_size;
        const std::uint32_t actual_orig_hash = ypf_murmur_hash2(orig_ptr, orig_len);
        if (actual_orig_hash != orig_data_hash ||
            (oe.data_hash != 0 && oe.data_hash != orig_data_hash)) {
            return false;
        }

        Bytes unmasked(new_raw_size);
        for (std::size_t k = 0; k < new_raw_size; ++k) {
            const std::uint8_t mask =
                compute_xor_mask_byte(orig_ptr, orig_len, orig_data_hash, k);
            unmasked[k] = static_cast<std::uint8_t>(patch_blob[off + k] ^ mask);
        }
        off += new_raw_size;

        const std::uint32_t actual_new_hash =
            ypf_murmur_hash2(unmasked.data(), unmasked.size());
        if (actual_new_hash != new_data_hash) {
            return false;
        }

        PngHeaderInfo orig_ihdr;
        PngHeaderInfo new_ihdr;
        if (!parse_png_ihdr_ptr(orig_ptr, orig_len, orig_ihdr) ||
            !parse_png_ihdr_ptr(unmasked.data(), unmasked.size(), new_ihdr)) {
            return false;
        }
        if (orig_ihdr.width != new_ihdr.width ||
            orig_ihdr.height != new_ihdr.height ||
            orig_ihdr.bit_depth != new_ihdr.bit_depth ||
            orig_ihdr.color_type != new_ihdr.color_type) {
            return false;
        }

        const auto [it, inserted] =
            replacements.emplace(static_cast<std::size_t>(entry_index), std::move(unmasked));
        if (!inserted) {
            return false;
        }
    }

    if (off != patch_blob.size()) {
        return false;
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
    if (first_payload_offset < kYpfHeaderSize || first_payload_offset > orig_ypf.size()) {
        return false;
    }

    for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
        if (blocks[bi].orig_offset > orig_ypf.size() ||
            static_cast<std::uint64_t>(blocks[bi].orig_packed_size) >
                orig_ypf.size() - blocks[bi].orig_offset) {
            return false;
        }
        const std::uint64_t orig_end =
            blocks[bi].orig_offset + static_cast<std::uint64_t>(blocks[bi].orig_packed_size);
        const std::uint64_t next_start =
            (bi + 1 < blocks.size()) ? blocks[bi + 1].orig_offset
                                     : static_cast<std::uint64_t>(orig_ypf.size());
        if (orig_end > next_start || next_start > orig_ypf.size()) {
            return false;
        }
        blocks[bi].trailing_bytes.assign(
            orig_ypf.begin() + static_cast<std::ptrdiff_t>(orig_end),
            orig_ypf.begin() + static_cast<std::ptrdiff_t>(next_start));
    }

    Bytes payload_buf;
    payload_buf.reserve(orig_ypf.size() - static_cast<std::size_t>(first_payload_offset));

    for (const PayloadBlock& block : blocks) {
        std::vector<std::size_t> unreplaced;
        std::vector<std::size_t> replaced;
        for (const std::size_t idx : block.entry_indices) {
            if (replacements.find(idx) != replacements.end()) {
                replaced.push_back(idx);
                continue;
            }
            unreplaced.push_back(idx);
        }

        if (!unreplaced.empty()) {
            const std::uint64_t new_off = first_payload_offset + payload_buf.size();
            payload_buf.insert(
                payload_buf.end(),
                orig_ypf.begin() + static_cast<std::ptrdiff_t>(block.orig_offset),
                orig_ypf.begin() +
                    static_cast<std::ptrdiff_t>(block.orig_offset + block.orig_packed_size));
            payload_buf.insert(payload_buf.end(), block.trailing_bytes.begin(),
                               block.trailing_bytes.end());
            for (const std::size_t idx : unreplaced) {
                entries[idx].data_offset = new_off;
            }
        }

        for (const std::size_t idx : replaced) {
            YpfIndexEntry& e = entries[idx];
            const Bytes& repl_raw = replacements.at(idx);
            const std::uint64_t new_off = first_payload_offset + payload_buf.size();

            e.raw_size = static_cast<std::uint32_t>(repl_raw.size());
            e.packed_size = static_cast<std::uint32_t>(repl_raw.size());
            e.data_offset = new_off;
            if (e.data_hash != 0) {
                e.data_hash = ypf_murmur_hash2(repl_raw.data(), repl_raw.size());
            }

            payload_buf.insert(payload_buf.end(), repl_raw.begin(), repl_raw.end());
            payload_buf.insert(payload_buf.end(), block.trailing_bytes.begin(),
                               block.trailing_bytes.end());
        }
    }

    out_patched_ypf.reserve(
        static_cast<std::size_t>(first_payload_offset) + payload_buf.size());
    out_patched_ypf.insert(
        out_patched_ypf.end(), orig_ypf.begin(),
        orig_ypf.begin() + static_cast<std::ptrdiff_t>(kYpfHeaderSize));

    for (const YpfIndexEntry& e : entries) {
        append_le_u32(out_patched_ypf, e.name_hash);
        out_patched_ypf.push_back(e.name_size_raw);
        out_patched_ypf.insert(out_patched_ypf.end(), e.name_bytes_xor.begin(),
                               e.name_bytes_xor.end());
        out_patched_ypf.push_back(e.type);
        out_patched_ypf.push_back(e.compressed);
        append_le_u32(out_patched_ypf, e.raw_size);
        append_le_u32(out_patched_ypf, e.packed_size);
        append_le_u64(out_patched_ypf, e.data_offset);
        append_le_u32(out_patched_ypf, e.data_hash);
    }

    if (out_patched_ypf.size() < first_payload_offset) {
        out_patched_ypf.insert(
            out_patched_ypf.end(),
            orig_ypf.begin() + static_cast<std::ptrdiff_t>(out_patched_ypf.size()),
            orig_ypf.begin() + static_cast<std::ptrdiff_t>(first_payload_offset));
    }
    out_patched_ypf.insert(out_patched_ypf.end(), payload_buf.begin(), payload_buf.end());
    return true;
}

struct MenuAutoPatcher::Impl {
    std::wstring mutex_name;
    std::uint32_t mutex_timeout_ms = kDefaultMutexTimeoutMs;
};

MenuAutoPatcher::MenuAutoPatcher(std::wstring mutex_name,
                                 std::uint32_t mutex_timeout_ms)
    : impl_(std::make_unique<Impl>()) {
    impl_->mutex_name = std::move(mutex_name);
    impl_->mutex_timeout_ms = mutex_timeout_ms;
}

MenuAutoPatcher::~MenuAutoPatcher() = default;
MenuAutoPatcher::MenuAutoPatcher(MenuAutoPatcher&&) noexcept = default;
MenuAutoPatcher& MenuAutoPatcher::operator=(MenuAutoPatcher&&) noexcept = default;

PatchResult MenuAutoPatcher::Apply(const std::wstring& game_dir) const {
    if (game_dir.empty() || !impl_) {
        return {PatchStatus::kFailed, 0u};
    }
    ScopedNamedMutex lock(impl_->mutex_name.c_str(), impl_->mutex_timeout_ms);
    if (!lock.acquired()) {
        Log("MenuAutoPatch: failed to acquire mutex");
        return {PatchStatus::kFailed, 0u};
    }

    const std::wstring patch_path = game_dir + L"\\cgsys_patch.bin";
    const std::wstring ypf_path = game_dir + L"\\pac\\cgsys.ypf";
    const std::wstring bak_path = ypf_path + L".bak";
    const std::wstring sentinel_path = ypf_path + L".applied";

    std::uint64_t patch_size = 0u;
    if (!get_file_size_exact(patch_path, patch_size)) {
        return {PatchStatus::kSkippedMissingInput, 0u};
    }

    std::uint64_t ypf_size = 0u;
    const bool has_ypf = get_file_size_exact(ypf_path, ypf_size);
    std::uint64_t bak_size = 0u;
    bool has_bak = get_file_size_exact(bak_path, bak_size);
    if (!has_ypf && !has_bak) {
        return {PatchStatus::kSkippedMissingInput, 0u};
    }

    if (patch_size < kPatchHeaderSize) {
        Log("MenuAutoPatch: cgsys_patch.bin truncated (%llu bytes)",
            static_cast<unsigned long long>(patch_size));
        return {PatchStatus::kFailed, 0u};
    }

    std::uint8_t header[kPatchHeaderSize] = {};
    if (!read_file_prefix(patch_path, header, static_cast<DWORD>(kPatchHeaderSize))) {
        Log("MenuAutoPatch: failed to read cgsys_patch.bin header");
        return {PatchStatus::kFailed, 0u};
    }
    if (std::memcmp(header, kPatchMagic, kPatchMagicSize) != 0 ||
        rd_le_u32(header + kPatchVersionOffset) != kPatchFormatVersion) {
        Log("MenuAutoPatch: invalid cgsys_patch.bin header");
        return {PatchStatus::kFailed, 0u};
    }
    const std::uint32_t expected_orig_size = rd_le_u32(header + kPatchOrigSizeOffset);

    std::uint32_t patch_murmur32 = 0u;
    std::uint64_t patch_fnv64 = 0u;
    if (!hash_file_contents(patch_path, patch_size, patch_murmur32, patch_fnv64)) {
        Log("MenuAutoPatch: failed to hash cgsys_patch.bin");
        return {PatchStatus::kFailed, 0u};
    }

    // Launch 2+ fast-path: verified pristine .bak backup, present live cgsys.ypf,
    // and matching pac\cgsys.ypf.applied sentinel covering patch digest + sizes.
    if (has_bak && bak_size == expected_orig_size && has_ypf &&
        is_sentinel_matching(sentinel_path,
                             patch_murmur32,
                             patch_fnv64,
                             patch_size,
                             ypf_size)) {
        return {PatchStatus::kAlreadyUpToDate, ypf_size};
    }

    const bool need_bak = !has_bak;
    const std::wstring& src_path = has_bak ? bak_path : ypf_path;
    const std::uint64_t src_size = has_bak ? bak_size : ypf_size;
    if (src_size != expected_orig_size) {
        Log("MenuAutoPatch: pac\\cgsys.ypf source size mismatch (%llu != %u)",
            static_cast<unsigned long long>(src_size),
            static_cast<unsigned>(expected_orig_size));
        return {PatchStatus::kFailed, 0u};
    }

    Bytes orig_ypf;
    Bytes patch_blob;
    if (!read_entire_file(src_path, orig_ypf) ||
        !read_entire_file(patch_path, patch_blob)) {
        Log("MenuAutoPatch: failed to read pac\\cgsys.ypf source or cgsys_patch.bin");
        return {PatchStatus::kFailed, 0u};
    }

    // Validate all hashes + PNG IHDR parity and reconstruct patched YPF in memory
    // BEFORE creating .bak or mutating live pac\cgsys.ypf.
    Bytes patched_ypf;
    if (!ApplyXorMenuPatchInMemory(orig_ypf, patch_blob, patched_ypf)) {
        Log("MenuAutoPatch: ApplyXorMenuPatchInMemory failed");
        return {PatchStatus::kFailed, 0u};
    }

    // Compute sentinel digest directly from the applied in-memory patch_blob
    // to eliminate any TOCTOU drift between fast-path hashing and full read.
    if (static_cast<std::uint64_t>(patch_blob.size()) >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        Log("MenuAutoPatch: cgsys_patch.bin exceeds 4 GiB digest limit");
        return {PatchStatus::kFailed, 0u};
    }
    const std::uint32_t applied_murmur32 =
        ypf_murmur_hash2(patch_blob.data(), patch_blob.size());
    const std::uint64_t applied_fnv64 =
        compute_fnv64(patch_blob.data(), patch_blob.size());
    const std::uint64_t applied_patch_size =
        static_cast<std::uint64_t>(patch_blob.size());

    // Phase 1 (staging): write & flush .bak.tmp (if needed) and .tmp before swapping.
    const std::wstring bak_tmp_path = bak_path + L".tmp";
    const std::wstring ypf_tmp_path = ypf_path + L".tmp";
    if (need_bak && !write_and_flush_file(bak_tmp_path, orig_ypf.data(), orig_ypf.size())) {
        const DWORD gle = GetLastError();
        Log("MenuAutoPatch: staging pac\\cgsys.ypf.bak.tmp failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u};
    }
    if (!write_and_flush_file(ypf_tmp_path, patched_ypf.data(), patched_ypf.size())) {
        const DWORD gle = GetLastError();
        if (need_bak) {
            DeleteFileW(bak_tmp_path.c_str());
        }
        Log("MenuAutoPatch: staging pac\\cgsys.ypf.tmp failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u};
    }

    // Phase 2 (atomic commit): commit immutable .bak FIRST, invalidate any stale
    // .applied sentinel, then commit live pac\cgsys.ypf.
    if (need_bak &&
        !MoveFileExW(bak_tmp_path.c_str(), bak_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        DeleteFileW(bak_tmp_path.c_str());
        DeleteFileW(ypf_tmp_path.c_str());
        Log("MenuAutoPatch: atomic commit to pac\\cgsys.ypf.bak failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u};
    }
    DeleteFileW(sentinel_path.c_str());
    if (!MoveFileExW(ypf_tmp_path.c_str(), ypf_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        DeleteFileW(ypf_tmp_path.c_str());
        Log("MenuAutoPatch: atomic commit to pac\\cgsys.ypf failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u};
    }

    if (!write_sentinel_atomically(sentinel_path,
                                   applied_murmur32,
                                   applied_fnv64,
                                   applied_patch_size,
                                   static_cast<std::uint64_t>(patched_ypf.size()))) {
        const DWORD gle = GetLastError();
        Log("MenuAutoPatch: warning — failed to write pac\\cgsys.ypf.applied sentinel (gle=%lu)",
            gle);
    }

    Log("MenuAutoPatch: patched pac\\cgsys.ypf (%u -> %u bytes)",
        static_cast<unsigned>(orig_ypf.size()),
        static_cast<unsigned>(patched_ypf.size()));
    return {PatchStatus::kApplied, static_cast<std::uint64_t>(patched_ypf.size())};
}

bool EnsureMenuArchivePatchedInDir(const std::wstring& game_dir) {
    const MenuAutoPatcher patcher;
    const PatchResult res = patcher.Apply(game_dir);
    return res.status == PatchStatus::kApplied ||
           res.status == PatchStatus::kAlreadyUpToDate;
}

void EnsureMenuArchivePatched() {
    std::vector<std::wstring> candidate_dirs;
    const std::wstring exe_dir = parent_dir_of_module(nullptr);
    if (!exe_dir.empty()) {
        candidate_dirs.push_back(exe_dir);
    }

    HMODULE self_mod = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&EnsureMenuArchivePatched), &self_mod) &&
        self_mod != nullptr) {
        const std::wstring dll_dir = parent_dir_of_module(self_mod);
        if (!dll_dir.empty() &&
            std::find(candidate_dirs.begin(), candidate_dirs.end(), dll_dir) ==
                candidate_dirs.end()) {
            candidate_dirs.push_back(dll_dir);
        }
    }

    const DWORD cwd_needed = GetCurrentDirectoryW(0u, nullptr);
    if (cwd_needed > 1u) {
        std::wstring cwd_dir(cwd_needed, L'\0');
        const DWORD cwd_len = GetCurrentDirectoryW(cwd_needed, cwd_dir.data());
        if (cwd_len > 0u && cwd_len < cwd_needed) {
            cwd_dir.resize(cwd_len);
            if (std::find(candidate_dirs.begin(), candidate_dirs.end(), cwd_dir) ==
                candidate_dirs.end()) {
                candidate_dirs.push_back(std::move(cwd_dir));
            }
        }
    }

    for (const std::wstring& dir : candidate_dirs) {
        std::uint64_t patch_sz = 0u;
        std::uint64_t ypf_sz = 0u;
        const std::wstring ypf_path = dir + L"\\pac\\cgsys.ypf";
        if (get_file_size_exact(dir + L"\\cgsys_patch.bin", patch_sz) &&
            (get_file_size_exact(ypf_path, ypf_sz) ||
             get_file_size_exact(ypf_path + L".bak", ypf_sz))) {
            EnsureMenuArchivePatchedInDir(dir);
            return;
        }
    }
}

}  // namespace menu_autopatch
