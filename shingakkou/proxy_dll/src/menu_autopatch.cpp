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

constexpr DWORD kMutexWaitTimeoutMs = 10000u;
constexpr const wchar_t* kPatchMutexName = L"Local\\ShingakkouMenuPatchMutex";

struct Ddp2EntryMeta {
    std::uint32_t offset = 0;
    std::uint32_t unpacked = 0;
    std::uint32_t packed = 0;
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

static std::uint32_t rd_le_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[kByteOffset1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[kByteOffset2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteOffset3]) << kShiftByte3);
}

static std::uint32_t rd_le_u32(const Bytes& b, std::size_t off) {
    return rd_le_u32(b.data() + off);
}

static void append_le_u32(Bytes& out, std::uint32_t v) {
    for (std::size_t i = 0; i < kWordBytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

static std::uint32_t menu_murmur_hash2(const std::uint8_t* data, std::size_t len) {
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
    const std::size_t index_end =
        kDdp2HeaderSize + static_cast<std::size_t>(count) * kDdp2IndexEntrySize;
    if (index_end > archive.size()) {
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

static bool get_file_size_exact(const std::wstring& path, std::uint64_t& out_size) {
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

static bool read_file_prefix(const std::wstring& path, std::uint8_t* dst, DWORD bytes_to_read) {
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
    std::uint64_t size64 = 0;
    if (!get_file_size_exact(path, size64) || size64 == 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(size64));
    return read_file_prefix(path, out.data(), static_cast<DWORD>(out.size()));
}

static bool write_entire_file(const std::wstring& path, const Bytes& data) {
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

static bool replace_file_atomically(const std::wstring& target_path, const Bytes& data) {
    const std::wstring tmp_path = target_path + L".tmp";
    if (!write_entire_file(tmp_path, data)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    if (!MoveFileExW(tmp_path.c_str(), target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
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
    const std::size_t total_records =
        static_cast<std::size_t>(sin_sysd_patch_count) +
        static_cast<std::size_t>(bootup_patch_count);
    std::size_t off = kPatchHeaderSize;

    for (std::size_t rec = 0; rec < total_records; ++rec) {
        if (off + kPatchRecordHeaderSize > patch_blob.size()) {
            return false;
        }
        const std::uint32_t archive_id = rd_le_u32(patch_blob, off + kRecordArchiveIdOffset);
        const std::uint32_t entry_index = rd_le_u32(patch_blob, off + kRecordEntryIndexOffset);
        const std::uint32_t orig_stored_hash = rd_le_u32(patch_blob, off + kRecordOrigHashOffset);
        const std::uint32_t new_unpacked_size = rd_le_u32(patch_blob, off + kRecordNewSizeOffset);
        const std::uint32_t new_unpacked_hash = rd_le_u32(patch_blob, off + kRecordNewHashOffset);
        off += kPatchRecordHeaderSize;

        if (new_unpacked_size == 0 || off + new_unpacked_size > patch_blob.size()) {
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

bool EnsureMenuArchivePatchedInDir(const std::wstring& game_dir) {
    if (game_dir.empty()) {
        return false;
    }
    ScopedNamedMutex lock(kPatchMutexName, kMutexWaitTimeoutMs);
    if (!lock.acquired()) {
        Log("MenuAutoPatch: failed to acquire mutex");
        return false;
    }

    const std::wstring patch_path = game_dir + L"\\menu_patch.bin";
    std::uint64_t patch_size = 0;
    if (!get_file_size_exact(patch_path, patch_size) || patch_size < kPatchHeaderSize) {
        return false;
    }

    std::uint8_t header[kPatchHeaderSize] = {};
    if (!read_file_prefix(patch_path, header, static_cast<DWORD>(kPatchHeaderSize))) {
        return false;
    }
    if (std::memcmp(header, kPatchMagic, kPatchMagicSize) != 0 ||
        rd_le_u32(header + kPatchVersionOffset) != kPatchFormatVersion) {
        Log("MenuAutoPatch: invalid menu_patch.bin header");
        return false;
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
        (expected_bootup_total > 0) || (expected_bootup_orig_size > 0) ||
        (expected_bootup_patch_count > 0);

    const std::wstring sin_sysd_path = resolve_sin_sysd_path(game_dir);
    const std::wstring sin_sysd_bak_path = sin_sysd_path + L".bak";
    const std::wstring bootup_path = game_dir + L"\\Bootup.dat";
    const std::wstring bootup_bak_path = bootup_path + L".bak";

    std::uint64_t sin_sysd_size = 0;
    if (!get_file_size_exact(sin_sysd_path, sin_sysd_size)) {
        return false;
    }

    std::uint64_t bootup_size = 0;
    if (expects_bootup && !get_file_size_exact(bootup_path, bootup_size)) {
        return false;
    }

    std::uint64_t sin_sysd_bak_size = 0;
    bool has_sin_sysd_bak = get_file_size_exact(sin_sysd_bak_path, sin_sysd_bak_size);
    if (!has_sin_sysd_bak && sin_sysd_size == expected_sin_sysd_orig_size) {
        if (!CopyFileW(sin_sysd_path.c_str(), sin_sysd_bak_path.c_str(), FALSE)) {
            Log("MenuAutoPatch: CopyFileW to sin_sysd.dat.bak failed (gle=%lu)",
                GetLastError());
            return false;
        }
        has_sin_sysd_bak = get_file_size_exact(sin_sysd_bak_path, sin_sysd_bak_size);
    }
    if (!has_sin_sysd_bak) {
        return false;
    }

    std::uint64_t bootup_bak_size = 0;
    bool has_bootup_bak = false;
    if (expects_bootup) {
        has_bootup_bak = get_file_size_exact(bootup_bak_path, bootup_bak_size);
        if (!has_bootup_bak && bootup_size == expected_bootup_orig_size) {
            if (!CopyFileW(bootup_path.c_str(), bootup_bak_path.c_str(), FALSE)) {
                Log("MenuAutoPatch: CopyFileW to Bootup.dat.bak failed (gle=%lu)",
                    GetLastError());
                return false;
            }
            has_bootup_bak = get_file_size_exact(bootup_bak_path, bootup_bak_size);
        }
        if (!has_bootup_bak) {
            return false;
        }
    }

    const bool sin_sysd_patched = (sin_sysd_size != expected_sin_sysd_orig_size);
    const bool bootup_patched = !expects_bootup || (bootup_size != expected_bootup_orig_size);
    if (sin_sysd_patched && bootup_patched) {
        return true;
    }

    if (sin_sysd_bak_size != expected_sin_sysd_orig_size) {
        Log("MenuAutoPatch: sin_sysd.dat.bak size mismatch (%llu != %u)",
            static_cast<unsigned long long>(sin_sysd_bak_size),
            static_cast<unsigned>(expected_sin_sysd_orig_size));
        return false;
    }
    if (expects_bootup && bootup_bak_size != expected_bootup_orig_size) {
        Log("MenuAutoPatch: Bootup.dat.bak size mismatch (%llu != %u)",
            static_cast<unsigned long long>(bootup_bak_size),
            static_cast<unsigned>(expected_bootup_orig_size));
        return false;
    }

    Bytes orig_sin_sysd;
    Bytes orig_bootup;
    Bytes patch_blob;
    if (!read_entire_file(sin_sysd_bak_path, orig_sin_sysd) ||
        !read_entire_file(patch_path, patch_blob)) {
        Log("MenuAutoPatch: failed to read sin_sysd.dat.bak or menu_patch.bin");
        return false;
    }
    if (expects_bootup && !read_entire_file(bootup_bak_path, orig_bootup)) {
        Log("MenuAutoPatch: failed to read Bootup.dat.bak");
        return false;
    }

    Bytes patched_sin_sysd;
    Bytes patched_bootup;
    if (!ApplyXorMenuPatchInMemory(
            orig_sin_sysd, orig_bootup, patch_blob, patched_sin_sysd, patched_bootup)) {
        Log("MenuAutoPatch: ApplyXorMenuPatchInMemory failed");
        return false;
    }

    if (!replace_file_atomically(sin_sysd_path, patched_sin_sysd)) {
        Log("MenuAutoPatch: atomic replace of sin_sysd.dat failed (gle=%lu)",
            GetLastError());
        return false;
    }
    if (expects_bootup && !replace_file_atomically(bootup_path, patched_bootup)) {
        Log("MenuAutoPatch: atomic replace of Bootup.dat failed (gle=%lu)",
            GetLastError());
        return false;
    }

    Log("MenuAutoPatch: patched sin_sysd.dat (%u -> %u bytes) and Bootup.dat (%u -> %u bytes)",
        static_cast<unsigned>(orig_sin_sysd.size()),
        static_cast<unsigned>(patched_sin_sysd.size()),
        static_cast<unsigned>(orig_bootup.size()),
        static_cast<unsigned>(patched_bootup.size()));
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
        std::uint64_t sin_sysd_sz = 0;
        if (!get_file_size_exact(dir + L"\\menu_patch.bin", patch_sz)) {
            continue;
        }
        const std::wstring sin_sysd_path = resolve_sin_sysd_path(dir);
        if (get_file_size_exact(sin_sysd_path, sin_sysd_sz)) {
            EnsureMenuArchivePatchedInDir(dir);
            return;
        }
    }
}

}  // namespace menu_autopatch
