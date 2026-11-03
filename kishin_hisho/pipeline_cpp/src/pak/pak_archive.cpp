// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "pak/pak_archive.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ksh::pak {

namespace {

constexpr std::size_t kPakHeaderSize = 0x114;
constexpr std::size_t kPakVersionOffset = 0x000;
constexpr std::size_t kPakMagicFieldOffset = 0x004;
constexpr std::size_t kPakMagicFieldSize = 0x100;
constexpr std::size_t kPakHeaderSeedOffset = 0x104;
constexpr std::size_t kPakTocUncompSizeOffset = 0x108;
constexpr std::size_t kPakFileCountOffset = 0x10C;
constexpr std::size_t kPakTocCompSizeOffset = 0x110;

constexpr std::uint32_t kPakHeaderXorSeed = 0x64u;
constexpr std::uint32_t kPakHashMultiplier = 0x89u;
constexpr std::uint32_t kLowByteMask = 0xFFu;

constexpr std::size_t kWordByteCount = 4;
constexpr std::size_t kWordAlignMask = 3;
constexpr std::size_t kByteIndex0 = 0;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;
constexpr unsigned int kShiftByte1 = 8;
constexpr unsigned int kShiftByte2 = 16;
constexpr unsigned int kShiftByte3 = 24;

constexpr std::size_t kTocEntryLengthFieldBytes = 4;
constexpr std::size_t kTocEntryTrailerBytes = 20;
constexpr std::size_t kTocDataOffsetFieldPos = 0;
constexpr std::size_t kTocUncompSizeFieldPos = 4;
constexpr std::size_t kTocNextOffsetFieldPos = 8;
constexpr std::size_t kTocIsCompressedFieldPos = 12;
constexpr std::size_t kTocCompSizeFieldPos = 16;

constexpr std::uint32_t kCompressedFlagValue = 1u;
constexpr std::uint32_t kUncompressedFlagValue = 0u;
constexpr int kDefaultZlibCompressionLevel = 6;
constexpr UINT kCp932CodePage = 932;

constexpr wchar_t kUnicodeNoBreakSpace = 0x00A0;
constexpr wchar_t kUnicodeAsciiSpace = 0x0020;
constexpr wchar_t kUnicodeAsciiHyphen = 0x002D;
constexpr wchar_t kUnicodeEnDash = 0x2013;
constexpr wchar_t kUnicodeEmDash = 0x2014;
constexpr wchar_t kUnicodeHorizontalBar = 0x2015;
constexpr wchar_t kUnicodeMinusSign = 0x2212;
constexpr wchar_t kUnicodeWaveDash = 0x301C;
constexpr wchar_t kUnicodeFullwidthHyphen = 0xFF0D;
constexpr wchar_t kUnicodeFullwidthTilde = 0xFF5E;

struct NecKanjiMapping {
    wchar_t codepoint;
    std::uint8_t lead_byte;
    std::uint8_t trail_byte;
};

// NEC-selected IBM extension Kanji present in Demonbane scripts that Python's
// cp932 codec encodes into 0xED40..0xEEFC rather than 0xFA40..0xFC4B.
constexpr NecKanjiMapping kNecKanjiTable[] = {
    {0x4F94, 0xED, 0x56},
    {0x5022, 0xED, 0x59},
    {0x5046, 0xED, 0x5C},
    {0x6111, 0xED, 0xA7},
    {0x7005, 0xED, 0xEF},
    {0x84B4, 0xEE, 0x7C},
    {0x891C, 0xED, 0x41},
    {0x8A12, 0xEE, 0x87},
};

static std::uint32_t ReadLeU32(const std::uint8_t* ptr) noexcept {
    return static_cast<std::uint32_t>(ptr[kByteIndex0]) |
           (static_cast<std::uint32_t>(ptr[kByteIndex1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(ptr[kByteIndex2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(ptr[kByteIndex3]) << kShiftByte3);
}

static void WriteLeU32(std::uint8_t* ptr, std::uint32_t value) noexcept {
    ptr[kByteIndex0] = static_cast<std::uint8_t>(value & kLowByteMask);
    ptr[kByteIndex1] = static_cast<std::uint8_t>((value >> kShiftByte1) & kLowByteMask);
    ptr[kByteIndex2] = static_cast<std::uint8_t>((value >> kShiftByte2) & kLowByteMask);
    ptr[kByteIndex3] = static_cast<std::uint8_t>((value >> kShiftByte3) & kLowByteMask);
}

static void AppendLeU32(Bytes& out, std::uint32_t value) {
    const std::size_t offset = out.size();
    out.resize(offset + kWordByteCount);
    WriteLeU32(out.data() + offset, value);
}

static Bytes ZlibCompress(const std::uint8_t* data, std::size_t size, int level) {
    uLongf dest_len = compressBound(static_cast<uLong>(size));
    Bytes out(static_cast<std::size_t>(dest_len));
    std::uint8_t dummy_input = 0;
    const Bytef* src_ptr = (size == 0) ? &dummy_input : reinterpret_cast<const Bytef*>(data);
    const int rc = compress2(reinterpret_cast<Bytef*>(out.data()), &dest_len, src_ptr,
                             static_cast<uLong>(size), level);
    if (rc != Z_OK) {
        throw std::runtime_error("zlib compress2 failed with code " + std::to_string(rc));
    }
    out.resize(static_cast<std::size_t>(dest_len));
    return out;
}

static Bytes ZlibDecompress(const std::uint8_t* data,
                            std::size_t comp_size,
                            std::size_t expected_uncomp_size) {
    Bytes out(expected_uncomp_size);
    std::uint8_t dummy_output = 0;
    Bytef* dst_ptr =
        (expected_uncomp_size == 0) ? &dummy_output : reinterpret_cast<Bytef*>(out.data());
    uLongf dest_len = static_cast<uLongf>(expected_uncomp_size);
    const int rc = uncompress(dst_ptr, &dest_len, reinterpret_cast<const Bytef*>(data),
                              static_cast<uLong>(comp_size));
    if (rc != Z_OK) {
        throw std::runtime_error("zlib uncompress failed with code " + std::to_string(rc));
    }
    if (static_cast<std::size_t>(dest_len) != expected_uncomp_size) {
        throw std::runtime_error("zlib decompressed size mismatch");
    }
    return out;
}

static wchar_t NormalizeForCp932(wchar_t ch) noexcept {
    switch (ch) {
    case kUnicodeEmDash:
        return kUnicodeHorizontalBar;
    case kUnicodeEnDash:
        return kUnicodeAsciiHyphen;
    case kUnicodeMinusSign:
        return kUnicodeFullwidthHyphen;
    case kUnicodeWaveDash:
        return kUnicodeFullwidthTilde;
    case kUnicodeNoBreakSpace:
        return kUnicodeAsciiSpace;
    default:
        return ch;
    }
}

static const NecKanjiMapping* FindNecKanji(wchar_t ch) noexcept {
    for (const auto& entry : kNecKanjiTable) {
        if (entry.codepoint == ch) {
            return &entry;
        }
    }
    return nullptr;
}

static void AppendWideRunAsCp932(const wchar_t* data, std::size_t length, Bytes& out) {
    if (length == 0) {
        return;
    }
    const int byte_len = WideCharToMultiByte(kCp932CodePage, 0, data, static_cast<int>(length),
                                             nullptr, 0, nullptr, nullptr);
    if (byte_len <= 0) {
        throw std::runtime_error("WideCharToMultiByte(932) failed");
    }
    const std::size_t start = out.size();
    out.resize(start + static_cast<std::size_t>(byte_len));
    WideCharToMultiByte(kCp932CodePage, 0, data, static_cast<int>(length),
                        reinterpret_cast<char*>(out.data() + start), byte_len, nullptr, nullptr);
}

static std::vector<PakEntry> ParseArchiveBytes(const Bytes& archive_bytes,
                                               std::uint32_t* out_version,
                                               std::string* out_magic_name) {
    if (archive_bytes.size() < kPakHeaderSize) {
        throw std::runtime_error("pak archive too small for v4 header");
    }

    const std::uint32_t version = ReadLeU32(archive_bytes.data() + kPakVersionOffset);
    if (version != kPakFormatVersion) {
        throw std::runtime_error("unsupported pak version: " + std::to_string(version));
    }

    const char* magic_begin =
        reinterpret_cast<const char*>(archive_bytes.data() + kPakMagicFieldOffset);
    const std::size_t magic_len = strnlen(magic_begin, kPakMagicFieldSize);
    std::string magic_name(magic_begin, magic_len);

    const std::uint32_t h_magic = ComputePakKey(magic_name);
    const std::uint32_t header_seed = ReadLeU32(archive_bytes.data() + kPakHeaderSeedOffset);
    const std::uint32_t toc_uncomp_size =
        ReadLeU32(archive_bytes.data() + kPakTocUncompSizeOffset) ^ h_magic;
    const std::uint32_t file_count =
        ReadLeU32(archive_bytes.data() + kPakFileCountOffset) ^ h_magic;
    const std::uint32_t toc_comp_size =
        ReadLeU32(archive_bytes.data() + kPakTocCompSizeOffset) ^ header_seed;

    if (kPakHeaderSize + static_cast<std::size_t>(toc_comp_size) > archive_bytes.size()) {
        throw std::runtime_error("pak TOC compressed size exceeds archive bounds");
    }

    const Bytes toc_raw = ZlibDecompress(archive_bytes.data() + kPakHeaderSize,
                                         static_cast<std::size_t>(toc_comp_size),
                                         static_cast<std::size_t>(toc_uncomp_size));

    const std::size_t data_start = kPakHeaderSize + static_cast<std::size_t>(toc_comp_size);
    std::size_t toc_pos = 0;
    std::vector<PakEntry> entries;
    entries.reserve(file_count);

    for (std::uint32_t i = 0; i < file_count; ++i) {
        if (toc_pos + kTocEntryLengthFieldBytes > toc_raw.size()) {
            throw std::runtime_error("pak TOC truncated at entry name length");
        }
        const std::uint32_t name_len = ReadLeU32(toc_raw.data() + toc_pos);
        toc_pos += kTocEntryLengthFieldBytes;

        if (toc_pos + static_cast<std::size_t>(name_len) + kTocEntryTrailerBytes > toc_raw.size()) {
            throw std::runtime_error("pak TOC truncated inside entry record");
        }
        const std::uint8_t* name_ptr = toc_raw.data() + toc_pos;
        const std::string rel_path = Cp932ToUtf8(name_ptr, static_cast<std::size_t>(name_len));
        const std::uint32_t entry_key = ComputePakKey(name_ptr, static_cast<std::size_t>(name_len));
        toc_pos += static_cast<std::size_t>(name_len);

        const std::uint8_t* fields_ptr = toc_raw.data() + toc_pos;
        const std::uint32_t rel_data_offset =
            ReadLeU32(fields_ptr + kTocDataOffsetFieldPos) ^ entry_key;
        const std::uint32_t uncomp_size =
            ReadLeU32(fields_ptr + kTocUncompSizeFieldPos) ^ entry_key;
        const std::uint32_t next_toc_offset =
            ReadLeU32(fields_ptr + kTocNextOffsetFieldPos) ^ entry_key;
        const std::uint32_t is_compressed =
            ReadLeU32(fields_ptr + kTocIsCompressedFieldPos) ^ entry_key;
        const std::uint32_t comp_size =
            ReadLeU32(fields_ptr + kTocCompSizeFieldPos) ^ entry_key;
        toc_pos += kTocEntryTrailerBytes;

        if (toc_pos != static_cast<std::size_t>(next_toc_offset)) {
            throw std::runtime_error("pak TOC next_toc_offset mismatch for " + rel_path);
        }

        const std::size_t chunk_offset = data_start + static_cast<std::size_t>(rel_data_offset);
        const std::size_t chunk_size = static_cast<std::size_t>(comp_size);
        if (chunk_offset + chunk_size > archive_bytes.size()) {
            throw std::runtime_error("pak payload out of bounds for " + rel_path);
        }

        Bytes xored_payload;
        if (is_compressed != kUncompressedFlagValue) {
            xored_payload = ZlibDecompress(archive_bytes.data() + chunk_offset, chunk_size,
                                           static_cast<std::size_t>(uncomp_size));
        } else {
            xored_payload.assign(archive_bytes.begin() + static_cast<std::ptrdiff_t>(chunk_offset),
                                 archive_bytes.begin() +
                                     static_cast<std::ptrdiff_t>(chunk_offset + chunk_size));
        }

        Bytes plain_payload = XorWordStream(xored_payload, entry_key);
        if (plain_payload.size() != static_cast<std::size_t>(uncomp_size)) {
            throw std::runtime_error("pak payload size mismatch after decryption for " + rel_path);
        }

        PakEntry entry;
        entry.relative_path = rel_path;
        entry.payload = std::move(plain_payload);
        entry.compress = (is_compressed != kUncompressedFlagValue);
        entries.push_back(std::move(entry));
    }

    if (out_version != nullptr) {
        *out_version = version;
    }
    if (out_magic_name != nullptr) {
        *out_magic_name = std::move(magic_name);
    }
    return entries;
}

}  // namespace

std::uint32_t ComputePakKey(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t hash = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const auto signed_byte = static_cast<std::int8_t>(data[i]);
        hash = hash * kPakHashMultiplier + static_cast<std::uint32_t>(static_cast<std::int32_t>(signed_byte));
    }
    return hash;
}

std::uint32_t ComputePakKey(std::string_view text) noexcept {
    return ComputePakKey(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

Bytes XorWordStream(const std::uint8_t* data, std::size_t size, std::uint32_t key) {
    Bytes out(size);
    if (size == 0) {
        return out;
    }

    std::uint8_t key_bytes[kWordByteCount];
    WriteLeU32(key_bytes, key);

    const std::size_t aligned_size = size & ~kWordAlignMask;
    for (std::size_t i = 0; i < aligned_size; i += kWordByteCount) {
        const std::uint32_t word = ReadLeU32(data + i) ^ key;
        WriteLeU32(out.data() + i, word);
    }
    for (std::size_t i = aligned_size; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>(data[i] ^ key_bytes[i & kWordAlignMask]);
    }
    return out;
}

Bytes XorWordStream(const Bytes& data, std::uint32_t key) {
    return XorWordStream(data.data(), data.size(), key);
}

Bytes Utf8ToCp932(std::string_view utf8_text) {
    if (utf8_text.empty()) {
        return {};
    }

    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
                                             static_cast<int>(utf8_text.size()), nullptr, 0);
    if (wide_len <= 0) {
        throw std::runtime_error("MultiByteToWideChar(CP_UTF8) failed");
    }

    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(), static_cast<int>(utf8_text.size()),
                        wide.data(), wide_len);

    for (wchar_t& ch : wide) {
        ch = NormalizeForCp932(ch);
    }

    Bytes out;
    out.reserve(utf8_text.size());
    std::size_t run_start = 0;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const NecKanjiMapping* nec = FindNecKanji(wide[i]);
        if (nec == nullptr) {
            continue;
        }
        AppendWideRunAsCp932(wide.data() + run_start, i - run_start, out);
        out.push_back(nec->lead_byte);
        out.push_back(nec->trail_byte);
        run_start = i + 1;
    }
    AppendWideRunAsCp932(wide.data() + run_start, wide.size() - run_start, out);
    return out;
}

std::string Cp932ToUtf8(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return {};
    }
    const int wide_len =
        MultiByteToWideChar(kCp932CodePage, 0, reinterpret_cast<const char*>(data),
                            static_cast<int>(size), nullptr, 0);
    if (wide_len <= 0) {
        throw std::runtime_error("MultiByteToWideChar(932) failed");
    }
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(kCp932CodePage, 0, reinterpret_cast<const char*>(data),
                        static_cast<int>(size), wide.data(), wide_len);

    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, nullptr, 0,
                                             nullptr, nullptr);
    if (utf8_len <= 0) {
        throw std::runtime_error("WideCharToMultiByte(CP_UTF8) failed");
    }
    std::string out(static_cast<std::size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, out.data(), utf8_len, nullptr, nullptr);
    return out;
}

std::string Cp932ToUtf8(const Bytes& cp932_bytes) {
    return Cp932ToUtf8(cp932_bytes.data(), cp932_bytes.size());
}

struct PakArchiveWriter::Impl {
    std::string magic_name;
    std::vector<PakEntry> entries;
};

PakArchiveWriter::PakArchiveWriter(std::string magic_name)
    : impl_(std::make_unique<Impl>()) {
    if (magic_name.size() >= kPakMagicFieldSize) {
        throw std::invalid_argument("pak magic_name exceeds 255 bytes");
    }
    impl_->magic_name = std::move(magic_name);
}

PakArchiveWriter::PakArchiveWriter(const PakArchiveWriter& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

PakArchiveWriter::PakArchiveWriter(PakArchiveWriter&& other) noexcept = default;

PakArchiveWriter& PakArchiveWriter::operator=(const PakArchiveWriter& other) {
    if (this != &other) {
        *impl_ = *other.impl_;
    }
    return *this;
}

PakArchiveWriter& PakArchiveWriter::operator=(PakArchiveWriter&& other) noexcept = default;

PakArchiveWriter::~PakArchiveWriter() = default;

void PakArchiveWriter::AddEntry(std::string relative_path, Bytes payload, bool compress) {
    PakEntry entry;
    entry.relative_path = std::move(relative_path);
    entry.payload = std::move(payload);
    entry.compress = compress;
    impl_->entries.push_back(std::move(entry));
}

void PakArchiveWriter::AddEntry(PakEntry entry) {
    impl_->entries.push_back(std::move(entry));
}

void PakArchiveWriter::Clear() {
    impl_->entries.clear();
}

std::size_t PakArchiveWriter::EntryCount() const noexcept {
    return impl_->entries.size();
}

const std::vector<PakEntry>& PakArchiveWriter::Entries() const noexcept {
    return impl_->entries;
}

Bytes PakArchiveWriter::BuildToBytes() const {
    const Bytes magic_bytes = Utf8ToCp932(impl_->magic_name);
    if (magic_bytes.size() >= kPakMagicFieldSize) {
        throw std::runtime_error("encoded pak magic_name exceeds header field");
    }
    const std::uint32_t h_magic = ComputePakKey(magic_bytes.data(), magic_bytes.size());

    Bytes toc_raw;
    std::vector<Bytes> payload_chunks;
    payload_chunks.reserve(impl_->entries.size());

    std::uint32_t cur_data_offset = 0;
    std::uint32_t cur_toc_offset = 0;
    std::size_t total_payload_bytes = 0;

    for (const auto& entry : impl_->entries) {
        const Bytes name_bytes = Utf8ToCp932(entry.relative_path);
        const std::uint32_t entry_key = ComputePakKey(name_bytes.data(), name_bytes.size());
        const auto uncomp_size = static_cast<std::uint32_t>(entry.payload.size());
        const std::uint32_t is_comp =
            entry.compress ? kCompressedFlagValue : kUncompressedFlagValue;

        const Bytes xored_raw = XorWordStream(entry.payload, entry_key);
        Bytes stored_bytes = entry.compress
                                 ? ZlibCompress(xored_raw.data(), xored_raw.size(),
                                                kDefaultZlibCompressionLevel)
                                 : xored_raw;
        const auto comp_size = static_cast<std::uint32_t>(stored_bytes.size());

        const auto entry_len = static_cast<std::uint32_t>(
            kTocEntryLengthFieldBytes + name_bytes.size() + kTocEntryTrailerBytes);
        const std::uint32_t next_toc_offset = cur_toc_offset + entry_len;

        AppendLeU32(toc_raw, static_cast<std::uint32_t>(name_bytes.size()));
        toc_raw.insert(toc_raw.end(), name_bytes.begin(), name_bytes.end());
        AppendLeU32(toc_raw, cur_data_offset ^ entry_key);
        AppendLeU32(toc_raw, uncomp_size ^ entry_key);
        AppendLeU32(toc_raw, next_toc_offset ^ entry_key);
        AppendLeU32(toc_raw, is_comp ^ entry_key);
        AppendLeU32(toc_raw, comp_size ^ entry_key);

        cur_toc_offset = next_toc_offset;
        cur_data_offset += comp_size;
        total_payload_bytes += stored_bytes.size();
        payload_chunks.push_back(std::move(stored_bytes));
    }

    const Bytes toc_comp =
        ZlibCompress(toc_raw.data(), toc_raw.size(), kDefaultZlibCompressionLevel);

    Bytes archive(kPakHeaderSize, 0);
    WriteLeU32(archive.data() + kPakVersionOffset, kPakFormatVersion);
    std::memcpy(archive.data() + kPakMagicFieldOffset, magic_bytes.data(), magic_bytes.size());
    WriteLeU32(archive.data() + kPakHeaderSeedOffset, kPakHeaderXorSeed);
    WriteLeU32(archive.data() + kPakTocUncompSizeOffset,
               static_cast<std::uint32_t>(toc_raw.size()) ^ h_magic);
    WriteLeU32(archive.data() + kPakFileCountOffset,
               static_cast<std::uint32_t>(impl_->entries.size()) ^ h_magic);
    WriteLeU32(archive.data() + kPakTocCompSizeOffset,
               static_cast<std::uint32_t>(toc_comp.size()) ^ kPakHeaderXorSeed);

    archive.reserve(kPakHeaderSize + toc_comp.size() + total_payload_bytes);
    archive.insert(archive.end(), toc_comp.begin(), toc_comp.end());
    for (const auto& chunk : payload_chunks) {
        archive.insert(archive.end(), chunk.begin(), chunk.end());
    }
    return archive;
}

void PakArchiveWriter::WriteToFile(const std::string& output_path) const {
    const Bytes archive = BuildToBytes();
    write_file(output_path, archive);
}

struct PakArchiveReader::Impl {
    std::uint32_t version = kPakFormatVersion;
    std::string magic_name;
    std::vector<PakEntry> entries;
};

PakArchiveReader::PakArchiveReader(const Bytes& archive_bytes)
    : impl_(std::make_unique<Impl>()) {
    impl_->entries =
        ParseArchiveBytes(archive_bytes, &impl_->version, &impl_->magic_name);
}

PakArchiveReader::PakArchiveReader(const PakArchiveReader& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

PakArchiveReader::PakArchiveReader(PakArchiveReader&& other) noexcept = default;

PakArchiveReader& PakArchiveReader::operator=(const PakArchiveReader& other) {
    if (this != &other) {
        *impl_ = *other.impl_;
    }
    return *this;
}

PakArchiveReader& PakArchiveReader::operator=(PakArchiveReader&& other) noexcept = default;

PakArchiveReader::~PakArchiveReader() = default;

PakArchiveReader PakArchiveReader::FromFile(const std::string& archive_path) {
    return PakArchiveReader(read_file(archive_path));
}

std::uint32_t PakArchiveReader::Version() const noexcept {
    return impl_->version;
}

const std::string& PakArchiveReader::MagicName() const noexcept {
    return impl_->magic_name;
}

const std::vector<PakEntry>& PakArchiveReader::Entries() const noexcept {
    return impl_->entries;
}

const PakEntry* PakArchiveReader::FindEntry(std::string_view relative_path) const noexcept {
    for (const auto& entry : impl_->entries) {
        if (entry.relative_path == relative_path) {
            return &entry;
        }
    }
    return nullptr;
}

void PakArchiveReader::VerifyAgainst(const std::vector<PakEntry>& expected_entries) const {
    if (impl_->entries.size() != expected_entries.size()) {
        throw std::runtime_error("pak entry count mismatch: got " +
                                 std::to_string(impl_->entries.size()) + ", expected " +
                                 std::to_string(expected_entries.size()));
    }
    for (std::size_t i = 0; i < expected_entries.size(); ++i) {
        if (impl_->entries[i].relative_path != expected_entries[i].relative_path) {
            throw std::runtime_error("pak entry path mismatch at index " + std::to_string(i));
        }
        if (impl_->entries[i].payload != expected_entries[i].payload) {
            throw std::runtime_error("pak entry payload mismatch for " +
                                     expected_entries[i].relative_path);
        }
    }
}

Bytes WritePakArchive(const std::vector<PakEntry>& entries, std::string_view magic_name) {
    PakArchiveWriter writer{std::string(magic_name)};
    for (const auto& entry : entries) {
        writer.AddEntry(entry);
    }
    return writer.BuildToBytes();
}

std::vector<PakEntry> ReadPakArchive(const Bytes& archive_bytes) {
    PakArchiveReader reader(archive_bytes);
    return reader.Entries();
}

void VerifyPakArchive(const Bytes& archive_bytes,
                      const std::vector<PakEntry>& expected_entries) {
    PakArchiveReader reader(archive_bytes);
    reader.VerifyAgainst(expected_entries);
}

}  // namespace ksh::pak
