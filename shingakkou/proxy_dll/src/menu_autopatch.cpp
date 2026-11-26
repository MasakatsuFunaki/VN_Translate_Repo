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
#include <utility>
#include <vector>

namespace menu_autopatch {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kDdp2MagicSize = 4u;
constexpr char kDdp2Magic[kDdp2MagicSize] = {'D', 'D', 'P', '2'};
constexpr std::size_t kDdp2HeaderSize = 32u;
constexpr std::size_t kDdp2CountOffset = 4u;
constexpr std::size_t kDdp2TailHeaderOffset = 8u;
constexpr std::size_t kDdp2IndexEntrySize = 16u;
constexpr std::size_t kDdp2EntryUnpackedOffset = 4u;
constexpr std::size_t kDdp2EntryPackedOffset = 8u;
constexpr std::size_t kShsWrapHeaderSize = 5u;
constexpr std::uint8_t kShsLiteral32Opcode = 0x1Fu;

constexpr std::size_t kPatchMagicSize = 8u;
constexpr char kPatchMagic[kPatchMagicSize] = {'S', 'H', 'I', 'N', 'M', 'E', 'N', 'U'};
constexpr std::uint32_t kPatchFormatVersion = 1u;
constexpr std::size_t kPatchHeaderSize = 36u;
constexpr std::size_t kPatchVersionOffset = 8u;
constexpr std::size_t kPatchSinSysdTotalOffset = 12u;
constexpr std::size_t kPatchSinSysdOrigSizeOffset = 16u;
constexpr std::size_t kPatchSinSysdCountOffset = 20u;
constexpr std::size_t kPatchBootupTotalOffset = 24u;
constexpr std::size_t kPatchBootupOrigSizeOffset = 28u;
constexpr std::size_t kPatchBootupCountOffset = 32u;

constexpr std::size_t kPatchRecordHeaderSize = 20u;
constexpr std::size_t kRecordArchiveIdOffset = 0u;
constexpr std::size_t kRecordEntryIndexOffset = 4u;
constexpr std::size_t kRecordOrigHashOffset = 8u;
constexpr std::size_t kRecordNewSizeOffset = 12u;
constexpr std::size_t kRecordNewHashOffset = 16u;
constexpr std::uint32_t kArchiveIdSinSysd = 0u;
constexpr std::uint32_t kArchiveIdBootup = 1u;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::size_t kWordBytes = 4u;
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
constexpr int kShiftHighDword = 32;

constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kXorStreamAddend = 0x37u;
constexpr std::uint32_t kHashByteIndexMask = 3u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::uint64_t kLowDwordMask = 0xFFFFFFFFULL;
constexpr std::size_t kBufferGrowthFactor = 2u;

constexpr std::size_t kSentinelMagicSize = 8u;
constexpr char kSentinelMagic[kSentinelMagicSize] = {
    'S', 'H', 'I', 'N', 'A', 'P', 'L', 'D'};
constexpr std::uint32_t kSentinelVersion = 1u;
constexpr std::size_t kSentinelSize = 48u;
constexpr std::size_t kSentinelVersionOffset = 8u;
constexpr std::size_t kSentinelMurmurOffset = 12u;
constexpr std::size_t kSentinelFnv64Offset = 16u;
constexpr std::size_t kSentinelPatchSizeOffset = 24u;
constexpr std::size_t kSentinelSinSysdSizeOffset = 32u;
constexpr std::size_t kSentinelBootupSizeOffset = 40u;

constexpr std::uint64_t kFnv64OffsetBasis = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnv64Prime = 0x100000001B3ULL;
constexpr std::size_t kHashChunkBytes = 16384u;
constexpr std::size_t kIoChunkBytes = 1u << 24;

struct Ddp2EntryMeta {
    std::uint32_t offset = 0;
    std::uint32_t unpacked = 0;
    std::uint32_t packed = 0;
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

static std::uint64_t rd_le_u64(const std::uint8_t* p) noexcept {
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

static void append_le_u32(Bytes& out, std::uint32_t v) {
    for (std::size_t i = 0; i < kWordBytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

static std::uint64_t compute_fnv64(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint64_t fnv = kFnv64OffsetBasis;
    for (std::size_t i = 0; i < len; ++i) {
        fnv ^= static_cast<std::uint64_t>(data[i]);
        fnv *= kFnv64Prime;
    }
    return fnv;
}

static std::uint32_t menu_murmur_hash2(const std::uint8_t* data, std::size_t len) {
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

static std::uint8_t compute_xor_mask_byte(const std::uint8_t* orig_stored,
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

static bool parse_ddp2_index(const Bytes& archive, std::vector<Ddp2EntryMeta>& out_entries) {
    out_entries.clear();
    if (archive.size() < kDdp2HeaderSize ||
        std::memcmp(archive.data(), kDdp2Magic, kDdp2MagicSize) != 0) {
        return false;
    }
    const std::uint32_t count = rd_le_u32(archive, kDdp2CountOffset);
    if (static_cast<std::size_t>(count) >
        (archive.size() - kDdp2HeaderSize) / kDdp2IndexEntrySize) {
        return false;
    }
    out_entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off =
            kDdp2HeaderSize + static_cast<std::size_t>(i) * kDdp2IndexEntrySize;
        Ddp2EntryMeta entry;
        entry.offset = rd_le_u32(archive, off);
        entry.unpacked = rd_le_u32(archive, off + kDdp2EntryUnpackedOffset);
        entry.packed = rd_le_u32(archive, off + kDdp2EntryPackedOffset);
        out_entries.push_back(entry);
    }
    return true;
}

static void rebuild_ddp2_in_memory(const Bytes& orig_archive,
                                   const std::vector<Ddp2EntryMeta>& entries,
                                   const std::map<std::size_t, Bytes>& replacements,
                                   Bytes& out_archive) {
    struct EntryPlan {
        std::uint32_t offset = 0;
        std::uint32_t unpacked = 0;
        std::uint32_t packed = 0;
        std::uint32_t stored = 0;
        const Bytes* replacement = nullptr;
    };

    const std::size_t count = entries.size();
    const std::uint32_t data_start =
        static_cast<std::uint32_t>(kDdp2HeaderSize + count * kDdp2IndexEntrySize);

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
            plan.packed = static_cast<std::uint32_t>(rep_it->second.size() + kShsWrapHeaderSize);
            plan.stored = plan.packed;
            cursor += plan.stored;
            plans.push_back(plan);
            continue;
        }
        const std::uint32_t raw_want =
            (entries[i].packed != 0) ? entries[i].packed : entries[i].unpacked;
        const std::uint32_t avail =
            (entries[i].offset >= orig_archive.size())
                ? 0u
                : static_cast<std::uint32_t>(std::min<std::size_t>(
                      raw_want, orig_archive.size() - static_cast<std::size_t>(entries[i].offset)));
        plan.stored = avail;
        cursor += plan.stored;
        plans.push_back(plan);
    }

    out_archive.clear();
    out_archive.reserve(cursor);
    out_archive.insert(out_archive.end(),
                       orig_archive.begin(),
                       orig_archive.begin() + static_cast<std::ptrdiff_t>(kDdp2MagicSize));
    append_le_u32(out_archive, static_cast<std::uint32_t>(count));
    out_archive.insert(out_archive.end(),
                       orig_archive.begin() + static_cast<std::ptrdiff_t>(kDdp2TailHeaderOffset),
                       orig_archive.begin() + static_cast<std::ptrdiff_t>(kDdp2HeaderSize));

    for (const EntryPlan& plan : plans) {
        append_le_u32(out_archive, plan.offset);
        append_le_u32(out_archive, plan.unpacked);
        append_le_u32(out_archive, plan.packed);
        append_le_u32(out_archive, 0u);
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (plans[i].replacement != nullptr) {
            const Bytes& rep = *plans[i].replacement;
            out_archive.push_back(kShsLiteral32Opcode);
            const std::uint32_t n = static_cast<std::uint32_t>(rep.size());
            out_archive.push_back(static_cast<std::uint8_t>((n >> kShiftByte3) & kLowByteMask));
            out_archive.push_back(static_cast<std::uint8_t>((n >> kShiftByte2) & kLowByteMask));
            out_archive.push_back(static_cast<std::uint8_t>((n >> kShiftByte1) & kLowByteMask));
            out_archive.push_back(static_cast<std::uint8_t>(n & kLowByteMask));
            out_archive.insert(out_archive.end(), rep.begin(), rep.end());
            continue;
        }
        const std::size_t src_off = entries[i].offset;
        const std::size_t src_len = plans[i].stored;
        out_archive.insert(out_archive.end(),
                           orig_archive.begin() + static_cast<std::ptrdiff_t>(src_off),
                           orig_archive.begin() + static_cast<std::ptrdiff_t>(src_off + src_len));
    }
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
    std::size_t written_total = 0;
    while (written_total < size) {
        const std::size_t rem = size - written_total;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(rem, kIoChunkBytes));
        DWORD put = 0;
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
                                 std::uint64_t expected_sin_sysd_size,
                                 std::uint64_t expected_bootup_size) noexcept {
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
           rd_le_u64(buf.data() + kSentinelFnv64Offset) == expected_fnv64 &&
           rd_le_u64(buf.data() + kSentinelPatchSizeOffset) == expected_patch_size &&
           rd_le_u64(buf.data() + kSentinelSinSysdSizeOffset) == expected_sin_sysd_size &&
           rd_le_u64(buf.data() + kSentinelBootupSizeOffset) == expected_bootup_size;
}

static bool write_sentinel_atomically(const std::wstring& sentinel_path,
                                      std::uint32_t murmur32,
                                      std::uint64_t fnv64,
                                      std::uint64_t patch_size,
                                      std::uint64_t sin_sysd_size,
                                      std::uint64_t bootup_size) noexcept {
    std::array<std::uint8_t, kSentinelSize> buf{};
    std::memcpy(buf.data(), kSentinelMagic, kSentinelMagicSize);
    wr_le_u32(buf.data() + kSentinelVersionOffset, kSentinelVersion);
    wr_le_u32(buf.data() + kSentinelMurmurOffset, murmur32);
    wr_le_u64(buf.data() + kSentinelFnv64Offset, fnv64);
    wr_le_u64(buf.data() + kSentinelPatchSizeOffset, patch_size);
    wr_le_u64(buf.data() + kSentinelSinSysdSizeOffset, sin_sysd_size);
    wr_le_u64(buf.data() + kSentinelBootupSizeOffset, bootup_size);
    return write_and_commit_atomically(sentinel_path, buf.data(), buf.size());
}

static std::wstring resolve_sin_sysd_path(const std::wstring& game_dir) {
    const std::wstring data_subdir_path = game_dir + L"\\Data\\sin_sysd.dat";
    std::uint64_t sz = 0;
    if (get_file_size_exact(data_subdir_path, sz) ||
        get_file_size_exact(data_subdir_path + L".bak", sz)) {
        return data_subdir_path;
    }
    const std::wstring direct_path = game_dir + L"\\sin_sysd.dat";
    if (get_file_size_exact(direct_path, sz) ||
        get_file_size_exact(direct_path + L".bak", sz)) {
        return direct_path;
    }
    return data_subdir_path;
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
    const std::vector<std::uint8_t>& orig_sin_sysd,
    const std::vector<std::uint8_t>& orig_bootup,
    const std::vector<std::uint8_t>& patch_blob,
    std::vector<std::uint8_t>& out_patched_sin_sysd,
    std::vector<std::uint8_t>& out_patched_bootup) {
    out_patched_sin_sysd.clear();
    out_patched_bootup.clear();

    if (patch_blob.size() < kPatchHeaderSize ||
        std::memcmp(patch_blob.data(), kPatchMagic, kPatchMagicSize) != 0) {
        return false;
    }
    const std::uint32_t version = rd_le_u32(patch_blob, kPatchVersionOffset);
    if (version != kPatchFormatVersion) {
        return false;
    }

    const std::uint32_t sin_sysd_total = rd_le_u32(patch_blob, kPatchSinSysdTotalOffset);
    const std::uint32_t sin_sysd_orig_size = rd_le_u32(patch_blob, kPatchSinSysdOrigSizeOffset);
    const std::uint32_t sin_sysd_patch_count = rd_le_u32(patch_blob, kPatchSinSysdCountOffset);
    const std::uint32_t bootup_total = rd_le_u32(patch_blob, kPatchBootupTotalOffset);
    const std::uint32_t bootup_orig_size = rd_le_u32(patch_blob, kPatchBootupOrigSizeOffset);
    const std::uint32_t bootup_patch_count = rd_le_u32(patch_blob, kPatchBootupCountOffset);

    if (sin_sysd_total == 0 || orig_sin_sysd.size() != sin_sysd_orig_size) {
        return false;
    }

    std::vector<Ddp2EntryMeta> sin_sysd_entries;
    if (!parse_ddp2_index(orig_sin_sysd, sin_sysd_entries) ||
        sin_sysd_entries.size() != sin_sysd_total) {
        return false;
    }

    const bool expects_bootup =
        (bootup_total > 0) || (bootup_orig_size > 0) || (bootup_patch_count > 0);
    std::vector<Ddp2EntryMeta> bootup_entries;
    if (expects_bootup) {
        if (bootup_total == 0 || orig_bootup.size() != bootup_orig_size) {
            return false;
        }
        if (!parse_ddp2_index(orig_bootup, bootup_entries) ||
            bootup_entries.size() != bootup_total) {
            return false;
        }
    }

    std::map<std::size_t, Bytes> sin_sysd_replacements;
    std::map<std::size_t, Bytes> bootup_replacements;
    const std::uint64_t total_records_u64 =
        static_cast<std::uint64_t>(sin_sysd_patch_count) +
        static_cast<std::uint64_t>(bootup_patch_count);
    const std::size_t max_records =
        (patch_blob.size() - kPatchHeaderSize) / kPatchRecordHeaderSize;
    if (total_records_u64 > static_cast<std::uint64_t>(max_records)) {
        return false;
    }
    const std::size_t total_records = static_cast<std::size_t>(total_records_u64);
    std::size_t off = kPatchHeaderSize;

    for (std::size_t rec = 0; rec < total_records; ++rec) {
        if (patch_blob.size() - off < kPatchRecordHeaderSize) {
            return false;
        }
        const std::uint32_t archive_id = rd_le_u32(patch_blob, off + kRecordArchiveIdOffset);
        const std::uint32_t entry_index = rd_le_u32(patch_blob, off + kRecordEntryIndexOffset);
        const std::uint32_t orig_stored_hash = rd_le_u32(patch_blob, off + kRecordOrigHashOffset);
        const std::uint32_t new_unpacked_size = rd_le_u32(patch_blob, off + kRecordNewSizeOffset);
        const std::uint32_t new_unpacked_hash = rd_le_u32(patch_blob, off + kRecordNewHashOffset);
        off += kPatchRecordHeaderSize;

        if (new_unpacked_size == 0 ||
            static_cast<std::size_t>(new_unpacked_size) > patch_blob.size() - off) {
            return false;
        }

        const bool is_sin_sysd_slot = (rec < sin_sysd_patch_count);
        const std::uint32_t expected_archive_id =
            is_sin_sysd_slot ? kArchiveIdSinSysd : kArchiveIdBootup;
        if (archive_id != expected_archive_id) {
            return false;
        }

        const Bytes& target_orig_archive = is_sin_sysd_slot ? orig_sin_sysd : orig_bootup;
        const std::vector<Ddp2EntryMeta>& target_entries =
            is_sin_sysd_slot ? sin_sysd_entries : bootup_entries;
        std::map<std::size_t, Bytes>& target_map =
            is_sin_sysd_slot ? sin_sysd_replacements : bootup_replacements;

        if (entry_index >= target_entries.size()) {
            return false;
        }
        const Ddp2EntryMeta& oe = target_entries[entry_index];
        if (oe.offset >= target_orig_archive.size()) {
            return false;
        }
        const std::uint32_t raw_want = (oe.packed != 0) ? oe.packed : oe.unpacked;
        const std::size_t stored_len = std::min<std::size_t>(
            raw_want, target_orig_archive.size() - static_cast<std::size_t>(oe.offset));
        if (stored_len == 0) {
            return false;
        }

        const std::uint8_t* orig_ptr = target_orig_archive.data() + oe.offset;
        const std::uint32_t actual_orig_hash = menu_murmur_hash2(orig_ptr, stored_len);
        if (actual_orig_hash != orig_stored_hash) {
            return false;
        }

        Bytes unmasked(new_unpacked_size);
        for (std::size_t k = 0; k < new_unpacked_size; ++k) {
            const std::uint8_t mask =
                compute_xor_mask_byte(orig_ptr, stored_len, orig_stored_hash, k);
            unmasked[k] = static_cast<std::uint8_t>(patch_blob[off + k] ^ mask);
        }
        off += new_unpacked_size;

        const std::uint32_t actual_new_hash =
            menu_murmur_hash2(unmasked.data(), unmasked.size());
        if (actual_new_hash != new_unpacked_hash) {
            return false;
        }

        const auto [it, inserted] =
            target_map.emplace(static_cast<std::size_t>(entry_index), std::move(unmasked));
        if (!inserted) {
            return false;
        }
    }

    if (off != patch_blob.size()) {
        return false;
    }

    rebuild_ddp2_in_memory(
        orig_sin_sysd, sin_sysd_entries, sin_sysd_replacements, out_patched_sin_sysd);
    if (expects_bootup) {
        rebuild_ddp2_in_memory(
            orig_bootup, bootup_entries, bootup_replacements, out_patched_bootup);
    }
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
        return {PatchStatus::kFailed, 0u, 0u};
    }
    ScopedNamedMutex lock(impl_->mutex_name.c_str(), impl_->mutex_timeout_ms);
    if (!lock.acquired()) {
        Log("MenuAutoPatch: failed to acquire mutex");
        return {PatchStatus::kFailed, 0u, 0u};
    }

    const std::wstring patch_path = game_dir + L"\\menu_patch.bin";
    const std::wstring sentinel_path = game_dir + L"\\menu_patch.applied";
    std::uint64_t patch_size = 0u;
    if (!get_file_size_exact(patch_path, patch_size)) {
        return {PatchStatus::kSkippedMissingInput, 0u, 0u};
    }

    const std::wstring sin_sysd_path = resolve_sin_sysd_path(game_dir);
    const std::wstring sin_sysd_bak_path = sin_sysd_path + L".bak";
    const std::wstring bootup_path = game_dir + L"\\Bootup.dat";
    const std::wstring bootup_bak_path = bootup_path + L".bak";

    std::uint64_t sin_sysd_size = 0u;
    const bool has_sin_sysd = get_file_size_exact(sin_sysd_path, sin_sysd_size);
    std::uint64_t sin_sysd_bak_size = 0u;
    bool has_sin_sysd_bak = get_file_size_exact(sin_sysd_bak_path, sin_sysd_bak_size);
    if (!has_sin_sysd && !has_sin_sysd_bak) {
        return {PatchStatus::kSkippedMissingInput, 0u, 0u};
    }

    if (patch_size < kPatchHeaderSize) {
        Log("MenuAutoPatch: menu_patch.bin truncated (%llu bytes)",
            static_cast<unsigned long long>(patch_size));
        return {PatchStatus::kFailed, 0u, 0u};
    }

    std::uint8_t header[kPatchHeaderSize] = {};
    if (!read_file_prefix(patch_path, header, static_cast<DWORD>(kPatchHeaderSize))) {
        Log("MenuAutoPatch: failed to read menu_patch.bin header");
        return {PatchStatus::kFailed, 0u, 0u};
    }
    if (std::memcmp(header, kPatchMagic, kPatchMagicSize) != 0 ||
        rd_le_u32(header + kPatchVersionOffset) != kPatchFormatVersion) {
        Log("MenuAutoPatch: invalid menu_patch.bin header");
        return {PatchStatus::kFailed, 0u, 0u};
    }

    const std::uint32_t expected_sin_sysd_orig_size =
        rd_le_u32(header + kPatchSinSysdOrigSizeOffset);
    const std::uint32_t expected_bootup_total =
        rd_le_u32(header + kPatchBootupTotalOffset);
    const std::uint32_t expected_bootup_orig_size =
        rd_le_u32(header + kPatchBootupOrigSizeOffset);
    const std::uint32_t expected_bootup_patch_count =
        rd_le_u32(header + kPatchBootupCountOffset);
    const bool expects_bootup =
        (expected_bootup_total > 0u) || (expected_bootup_orig_size > 0u) ||
        (expected_bootup_patch_count > 0u);

    std::uint64_t bootup_size = 0u;
    const bool has_bootup =
        expects_bootup && get_file_size_exact(bootup_path, bootup_size);
    std::uint64_t bootup_bak_size = 0u;
    bool has_bootup_bak =
        expects_bootup && get_file_size_exact(bootup_bak_path, bootup_bak_size);
    if (expects_bootup && !has_bootup && !has_bootup_bak) {
        return {PatchStatus::kSkippedMissingInput, 0u, 0u};
    }

    std::uint32_t patch_murmur32 = 0u;
    std::uint64_t patch_fnv64 = 0u;
    if (!hash_file_contents(patch_path, patch_size, patch_murmur32, patch_fnv64)) {
        Log("MenuAutoPatch: failed to hash menu_patch.bin");
        return {PatchStatus::kFailed, 0u, 0u};
    }

    // Launch 2+ fast-path: verified pristine .bak backups, present live archives,
    // and matching menu_patch.applied sentinel covering patch digest + sizes.
    const bool sin_sysd_ready =
        has_sin_sysd_bak && sin_sysd_bak_size == expected_sin_sysd_orig_size && has_sin_sysd;
    const bool bootup_ready =
        !expects_bootup ||
        (has_bootup_bak && bootup_bak_size == expected_bootup_orig_size && has_bootup);
    if (sin_sysd_ready && bootup_ready &&
        is_sentinel_matching(sentinel_path,
                             patch_murmur32,
                             patch_fnv64,
                             patch_size,
                             sin_sysd_size,
                             expects_bootup ? bootup_size : 0u)) {
        return {PatchStatus::kAlreadyUpToDate,
                sin_sysd_size,
                expects_bootup ? bootup_size : 0u};
    }

    const bool need_sin_sysd_bak = !has_sin_sysd_bak;
    const bool need_bootup_bak = expects_bootup && !has_bootup_bak;

    const std::wstring& sin_sysd_src_path =
        has_sin_sysd_bak ? sin_sysd_bak_path : sin_sysd_path;
    const std::uint64_t sin_sysd_src_size =
        has_sin_sysd_bak ? sin_sysd_bak_size : sin_sysd_size;
    if (sin_sysd_src_size != expected_sin_sysd_orig_size) {
        Log("MenuAutoPatch: sin_sysd source size mismatch (%llu != %u)",
            static_cast<unsigned long long>(sin_sysd_src_size),
            static_cast<unsigned>(expected_sin_sysd_orig_size));
        return {PatchStatus::kFailed, 0u, 0u};
    }

    const std::wstring& bootup_src_path =
        has_bootup_bak ? bootup_bak_path : bootup_path;
    const std::uint64_t bootup_src_size =
        has_bootup_bak ? bootup_bak_size : bootup_size;
    if (expects_bootup && bootup_src_size != expected_bootup_orig_size) {
        Log("MenuAutoPatch: Bootup source size mismatch (%llu != %u)",
            static_cast<unsigned long long>(bootup_src_size),
            static_cast<unsigned>(expected_bootup_orig_size));
        return {PatchStatus::kFailed, 0u, 0u};
    }

    Bytes orig_sin_sysd;
    Bytes orig_bootup;
    Bytes patch_blob;
    if (!read_entire_file(sin_sysd_src_path, orig_sin_sysd) ||
        !read_entire_file(patch_path, patch_blob)) {
        Log("MenuAutoPatch: failed to read sin_sysd source or menu_patch.bin");
        return {PatchStatus::kFailed, 0u, 0u};
    }
    if (expects_bootup && !read_entire_file(bootup_src_path, orig_bootup)) {
        Log("MenuAutoPatch: failed to read Bootup source");
        return {PatchStatus::kFailed, 0u, 0u};
    }

    // Validate all record hashes and reconstruct both patched archives in memory
    // BEFORE creating any .bak or mutating any live file on disk.
    Bytes patched_sin_sysd;
    Bytes patched_bootup;
    if (!ApplyXorMenuPatchInMemory(
            orig_sin_sysd, orig_bootup, patch_blob, patched_sin_sysd, patched_bootup)) {
        Log("MenuAutoPatch: ApplyXorMenuPatchInMemory failed");
        return {PatchStatus::kFailed, 0u, 0u};
    }

    // Compute sentinel digest directly from the applied in-memory patch_blob
    // to eliminate any TOCTOU drift between fast-path hashing and full read.
    if (static_cast<std::uint64_t>(patch_blob.size()) >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        Log("MenuAutoPatch: menu_patch.bin exceeds 4 GiB digest limit");
        return {PatchStatus::kFailed, 0u, 0u};
    }
    const std::uint32_t applied_murmur32 =
        menu_murmur_hash2(patch_blob.data(), patch_blob.size());
    const std::uint64_t applied_fnv64 =
        compute_fnv64(patch_blob.data(), patch_blob.size());
    const std::uint64_t applied_patch_size =
        static_cast<std::uint64_t>(patch_blob.size());

    // Phase 1 (staging): write and flush all .bak.tmp and .tmp files before
    // swapping any destination, so mid-stage I/O failure leaves disk untouched.
    const std::wstring sin_sysd_bak_tmp = sin_sysd_bak_path + L".tmp";
    const std::wstring bootup_bak_tmp = bootup_bak_path + L".tmp";
    const std::wstring sin_sysd_tmp_path = sin_sysd_path + L".tmp";
    const std::wstring bootup_tmp_path = bootup_path + L".tmp";

    auto cleanup_staged = [&]() noexcept {
        if (need_sin_sysd_bak) {
            DeleteFileW(sin_sysd_bak_tmp.c_str());
        }
        if (need_bootup_bak) {
            DeleteFileW(bootup_bak_tmp.c_str());
        }
        DeleteFileW(sin_sysd_tmp_path.c_str());
        if (expects_bootup) {
            DeleteFileW(bootup_tmp_path.c_str());
        }
    };

    if (need_sin_sysd_bak &&
        !write_and_flush_file(sin_sysd_bak_tmp, orig_sin_sysd.data(), orig_sin_sysd.size())) {
        const DWORD gle = GetLastError();
        cleanup_staged();
        Log("MenuAutoPatch: staging sin_sysd.dat.bak.tmp failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }
    if (need_bootup_bak &&
        !write_and_flush_file(bootup_bak_tmp, orig_bootup.data(), orig_bootup.size())) {
        const DWORD gle = GetLastError();
        cleanup_staged();
        Log("MenuAutoPatch: staging Bootup.dat.bak.tmp failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }
    if (!write_and_flush_file(sin_sysd_tmp_path,
                              patched_sin_sysd.data(),
                              patched_sin_sysd.size())) {
        const DWORD gle = GetLastError();
        cleanup_staged();
        Log("MenuAutoPatch: staging sin_sysd.dat.tmp failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }
    if (expects_bootup &&
        !write_and_flush_file(bootup_tmp_path,
                              patched_bootup.data(),
                              patched_bootup.size())) {
        const DWORD gle = GetLastError();
        cleanup_staged();
        Log("MenuAutoPatch: staging Bootup.dat.tmp failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }

    // Phase 2 (atomic commit): commit immutable .bak backups FIRST, invalidate
    // any stale .applied sentinel, then replace live archives.
    if (need_sin_sysd_bak &&
        !MoveFileExW(sin_sysd_bak_tmp.c_str(), sin_sysd_bak_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        cleanup_staged();
        Log("MenuAutoPatch: atomic commit of sin_sysd.dat.bak failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }
    if (need_bootup_bak &&
        !MoveFileExW(bootup_bak_tmp.c_str(), bootup_bak_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        cleanup_staged();
        Log("MenuAutoPatch: atomic commit of Bootup.dat.bak failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }
    DeleteFileW(sentinel_path.c_str());
    if (!MoveFileExW(sin_sysd_tmp_path.c_str(), sin_sysd_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        DeleteFileW(sin_sysd_tmp_path.c_str());
        if (expects_bootup) {
            DeleteFileW(bootup_tmp_path.c_str());
        }
        Log("MenuAutoPatch: atomic replace of sin_sysd.dat failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }
    if (expects_bootup &&
        !MoveFileExW(bootup_tmp_path.c_str(), bootup_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        DeleteFileW(bootup_tmp_path.c_str());
        Log("MenuAutoPatch: atomic replace of Bootup.dat failed (gle=%lu)", gle);
        return {PatchStatus::kFailed, 0u, 0u};
    }

    const std::uint64_t final_bootup_size =
        expects_bootup ? static_cast<std::uint64_t>(patched_bootup.size()) : 0u;
    if (!write_sentinel_atomically(sentinel_path,
                                   applied_murmur32,
                                   applied_fnv64,
                                   applied_patch_size,
                                   static_cast<std::uint64_t>(patched_sin_sysd.size()),
                                   final_bootup_size)) {
        const DWORD gle = GetLastError();
        Log("MenuAutoPatch: warning — failed to write menu_patch.applied sentinel (gle=%lu)", gle);
    }

    Log("MenuAutoPatch: patched sin_sysd.dat (%u -> %u bytes) and Bootup.dat (%u -> %u bytes)",
        static_cast<unsigned>(orig_sin_sysd.size()),
        static_cast<unsigned>(patched_sin_sysd.size()),
        static_cast<unsigned>(orig_bootup.size()),
        static_cast<unsigned>(patched_bootup.size()));
    return {PatchStatus::kApplied,
            static_cast<std::uint64_t>(patched_sin_sysd.size()),
            final_bootup_size};
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
        std::uint64_t sin_sysd_sz = 0u;
        if (!get_file_size_exact(dir + L"\\menu_patch.bin", patch_sz)) {
            continue;
        }
        const std::wstring sin_sysd_path = resolve_sin_sysd_path(dir);
        if (get_file_size_exact(sin_sysd_path, sin_sysd_sz) ||
            get_file_size_exact(sin_sysd_path + L".bak", sin_sysd_sz)) {
            EnsureMenuArchivePatchedInDir(dir);
            return;
        }
    }
}

}  // namespace menu_autopatch
