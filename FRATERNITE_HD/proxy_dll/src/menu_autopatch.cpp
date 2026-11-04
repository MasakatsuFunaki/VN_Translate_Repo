// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_autopatch.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

constexpr DWORD kMutexWaitTimeoutMs = 10000u;
constexpr const wchar_t* kPatchMutexName = L"Local\\FraterniteHdMenuPatchMutex";

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
    ScopedNamedMutex(const wchar_t* name, DWORD timeout_ms)
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

    bool acquired() const { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

class ScopedFileHandle {
public:
    explicit ScopedFileHandle(HANDLE h) : handle_(h) {}
    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;
    ~ScopedFileHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    bool valid() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::uint32_t rd_le_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[kByteOffset1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[kByteOffset2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteOffset3]) << kShiftByte3);
}

std::uint32_t rd_le_u32(const Bytes& b, std::size_t off) {
    return rd_le_u32(b.data() + off);
}

std::uint64_t rd_le_u64(const Bytes& b, std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = kU64Bytes; i > 0; --i) {
        v = (v << kBitsPerByte) | b[off + i - 1];
    }
    return v;
}

std::uint32_t rd_be_u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << kShiftByte3) |
           (static_cast<std::uint32_t>(p[kByteOffset1]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteOffset2]) << kShiftByte1) |
           static_cast<std::uint32_t>(p[kByteOffset3]);
}

void append_le_u32(Bytes& out, std::uint32_t v) {
    for (std::size_t i = 0; i < kWordBytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

void append_le_u64(Bytes& out, std::uint64_t v) {
    for (std::size_t i = 0; i < kU64Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

bool is_cp932_lead(std::uint8_t b) {
    return (b >= kCp932LeadMin1 && b <= kCp932LeadMax1) ||
           (b >= kCp932LeadMin2 && b <= kCp932LeadMax2);
}

bool is_cp932_trail(std::uint8_t b) {
    return (b >= kCp932TrailMin1 && b <= kCp932TrailMax1) ||
           (b >= kCp932TrailMin2 && b <= kCp932TrailMax2);
}

std::uint32_t ypf_murmur_hash2(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h = static_cast<std::uint32_t>(len);
    std::size_t i = 0;
    while (i + kWordBytes <= len) {
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

bool is_valid_cp932_entry_name(const Bytes& idx, std::size_t off, std::size_t len,
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

bool is_valid_ascii_entry_name(const Bytes& idx, std::size_t off, std::size_t len) {
    for (std::size_t j = 0; j < len; ++j) {
        const std::uint8_t d = static_cast<std::uint8_t>(idx[off + j] ^ kYpfNameXorKey);
        if (d < kAsciiPrintMin || d > kAsciiPrintMax) {
            return false;
        }
    }
    return true;
}

bool is_valid_entry_footer(const Bytes& idx, std::size_t tail) {
    if (tail + kYpfEntryFooterSize > idx.size()) {
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

int probe_entry_name_length(const Bytes& idx, std::size_t off, int hinted) {
    const int room = static_cast<int>(idx.size()) - static_cast<int>(off) -
                     static_cast<int>(kYpfEntryFooterSize);
    if (room <= 0) {
        return 0;
    }
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

bool parse_ypf_index(const Bytes& file, std::vector<YpfIndexEntry>& out_entries) {
    out_entries.clear();
    if (file.size() < kYpfHeaderSize ||
        std::memcmp(file.data(), kYpfMagic, kYpfMagicSize) != 0) {
        return false;
    }
    const std::uint32_t file_count = rd_le_u32(file, kFileCountOffset);
    const std::uint32_t idx_size = rd_le_u32(file, kIdxSizeOffset);
    const std::size_t idx_begin = kYpfHeaderSize;
    const std::size_t idx_end = std::min<std::size_t>(file.size(), idx_begin + idx_size);
    const Bytes idx(file.begin() + static_cast<std::ptrdiff_t>(idx_begin),
                    file.begin() + static_cast<std::ptrdiff_t>(idx_end));

    out_entries.reserve(file_count);
    std::size_t off = 0;
    for (std::uint32_t k = 0; k < file_count; ++k) {
        if (off + kYpfEntryPrefixSize > idx.size()) {
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
            off + static_cast<std::size_t>(name_len) + kYpfEntryFooterSize > idx.size()) {
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

bool parse_png_ihdr_ptr(const std::uint8_t* data, std::size_t size, PngHeaderInfo& out) {
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

std::uint8_t compute_xor_mask_byte(const std::uint8_t* orig_payload,
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

bool get_file_size_exact(const std::wstring& path, std::uint64_t& out_size) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    out_size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32u) |
               static_cast<std::uint64_t>(fad.nFileSizeLow);
    return true;
}

bool read_file_prefix(const std::wstring& path, std::uint8_t* dst, DWORD bytes_to_read) {
    ScopedFileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }
    DWORD got = 0;
    return ReadFile(file.get(), dst, bytes_to_read, &got, nullptr) && got == bytes_to_read;
}

bool read_entire_file(const std::wstring& path, Bytes& out) {
    std::uint64_t size64 = 0;
    if (!get_file_size_exact(path, size64) || size64 == 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(size64));
    return read_file_prefix(path, out.data(), static_cast<DWORD>(out.size()));
}

bool write_entire_file(const std::wstring& path, const Bytes& data) {
    ScopedFileHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }
    DWORD written = 0;
    return WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()),
                     &written, nullptr) &&
           written == static_cast<DWORD>(data.size());
}

std::wstring parent_dir_of_module(HMODULE mod) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(mod, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    std::wstring path(buf, len);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return path.substr(0, slash);
}

}  // namespace

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

    std::unordered_map<std::size_t, Bytes> replacements;
    replacements.reserve(patch_entry_count);
    std::size_t off = kPatchHeaderSize;

    for (std::uint32_t r = 0; r < patch_entry_count; ++r) {
        if (off + kPatchRecordHeaderSize > patch_blob.size()) {
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
            off + new_raw_size > patch_blob.size()) {
            return false;
        }

        const YpfIndexEntry& oe = entries[entry_index];
        if (oe.compressed != 0 || oe.raw_size != oe.packed_size || oe.packed_size == 0 ||
            oe.data_offset + oe.packed_size > orig_ypf.size()) {
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
        const std::uint64_t orig_end =
            blocks[bi].orig_offset + blocks[bi].orig_packed_size;
        const std::uint64_t next_start =
            (bi + 1 < blocks.size()) ? blocks[bi + 1].orig_offset : orig_ypf.size();
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

bool EnsureMenuArchivePatchedInDir(const std::wstring& game_dir) {
    if (game_dir.empty()) {
        return false;
    }
    ScopedNamedMutex lock(kPatchMutexName, kMutexWaitTimeoutMs);
    if (!lock.acquired()) {
        Log("MenuAutoPatch: failed to acquire mutex");
        return false;
    }

    const std::wstring patch_path = game_dir + L"\\cgsys_patch.bin";
    const std::wstring ypf_path = game_dir + L"\\pac\\cgsys.ypf";
    const std::wstring bak_path = game_dir + L"\\pac\\cgsys.ypf.bak";
    const std::wstring tmp_path = game_dir + L"\\pac\\cgsys.ypf.tmp";

    std::uint64_t patch_size = 0;
    std::uint64_t ypf_size = 0;
    if (!get_file_size_exact(patch_path, patch_size) ||
        patch_size < kPatchHeaderSize ||
        !get_file_size_exact(ypf_path, ypf_size)) {
        return false;
    }

    std::uint8_t header[kPatchHeaderSize] = {};
    if (!read_file_prefix(patch_path, header, static_cast<DWORD>(kPatchHeaderSize))) {
        return false;
    }
    if (std::memcmp(header, kPatchMagic, kPatchMagicSize) != 0 ||
        rd_le_u32(header + kPatchVersionOffset) != kPatchFormatVersion) {
        Log("MenuAutoPatch: invalid cgsys_patch.bin header");
        return false;
    }
    const std::uint32_t expected_orig_size = rd_le_u32(header + kPatchOrigSizeOffset);

    std::uint64_t bak_size = 0;
    bool has_bak = get_file_size_exact(bak_path, bak_size);
    if (!has_bak && ypf_size == expected_orig_size) {
        if (!CopyFileW(ypf_path.c_str(), bak_path.c_str(), FALSE)) {
            Log("MenuAutoPatch: CopyFileW to pac\\cgsys.ypf.bak failed (gle=%lu)",
                GetLastError());
            return false;
        }
        has_bak = get_file_size_exact(bak_path, bak_size);
    }

    if (!has_bak) {
        return false;
    }
    if (ypf_size != expected_orig_size) {
        return true;
    }
    if (bak_size != expected_orig_size) {
        Log("MenuAutoPatch: pac\\cgsys.ypf.bak size mismatch (%llu != %u)",
            static_cast<unsigned long long>(bak_size),
            static_cast<unsigned>(expected_orig_size));
        return false;
    }

    Bytes orig_ypf;
    Bytes patch_blob;
    if (!read_entire_file(bak_path, orig_ypf) ||
        !read_entire_file(patch_path, patch_blob)) {
        Log("MenuAutoPatch: failed to read pac\\cgsys.ypf.bak or cgsys_patch.bin");
        return false;
    }

    Bytes patched_ypf;
    if (!ApplyXorMenuPatchInMemory(orig_ypf, patch_blob, patched_ypf)) {
        Log("MenuAutoPatch: ApplyXorMenuPatchInMemory failed");
        return false;
    }

    if (!write_entire_file(tmp_path, patched_ypf)) {
        DeleteFileW(tmp_path.c_str());
        Log("MenuAutoPatch: failed to write pac\\cgsys.ypf.tmp");
        return false;
    }

    if (!MoveFileExW(tmp_path.c_str(), ypf_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        Log("MenuAutoPatch: MoveFileExW to pac\\cgsys.ypf failed (gle=%lu)",
            GetLastError());
        return false;
    }

    Log("MenuAutoPatch: patched pac\\cgsys.ypf (%u -> %u bytes)",
        static_cast<unsigned>(orig_ypf.size()),
        static_cast<unsigned>(patched_ypf.size()));
    return true;
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

    wchar_t cwd_buf[MAX_PATH] = {};
    const DWORD cwd_len = GetCurrentDirectoryW(MAX_PATH, cwd_buf);
    if (cwd_len > 0 && cwd_len < MAX_PATH) {
        const std::wstring cwd_dir(cwd_buf, cwd_len);
        if (std::find(candidate_dirs.begin(), candidate_dirs.end(), cwd_dir) ==
            candidate_dirs.end()) {
            candidate_dirs.push_back(cwd_dir);
        }
    }

    for (const std::wstring& dir : candidate_dirs) {
        std::uint64_t patch_sz = 0;
        std::uint64_t ypf_sz = 0;
        if (get_file_size_exact(dir + L"\\cgsys_patch.bin", patch_sz) &&
            get_file_size_exact(dir + L"\\pac\\cgsys.ypf", ypf_sz)) {
            EnsureMenuArchivePatchedInDir(dir);
            return;
        }
    }
}

}  // namespace menu_autopatch
