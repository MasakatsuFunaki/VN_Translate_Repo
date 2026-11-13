// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/menu_patcher.h"

#include "common/util.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace mgi::menu {

namespace {

constexpr std::size_t kByteIndex0 = 0;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;
constexpr std::size_t kByteIndex4 = 4;
constexpr std::size_t kByteIndex5 = 5;
constexpr std::size_t kByteIndex6 = 6;
constexpr std::size_t kByteIndex7 = 7;

constexpr std::uint32_t kBitsPerByte = 8u;
constexpr std::uint32_t kBitsPerU64 = 64u;
constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;
constexpr std::uint32_t kByteShift4 = 32u;
constexpr std::uint32_t kByteShift5 = 40u;
constexpr std::uint32_t kByteShift6 = 48u;
constexpr std::uint32_t kByteShift7 = 56u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::uint64_t kLowU32Mask = 0xFFFFFFFFull;

constexpr std::size_t kU16ByteSize = 2;
constexpr std::size_t kU32ByteSize = 4;
constexpr std::size_t kU64ByteSize = 8;

constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::size_t kMurmurBlockBytes = 4;
constexpr std::size_t kMurmurTailBytes3 = 3;
constexpr std::size_t kMurmurTailBytes2 = 2;
constexpr std::size_t kMurmurTailBytes1 = 1;

constexpr std::uint8_t kFxfBitwiseNotMask = 0xFFu;

constexpr std::size_t kHeaderVersionOffset = 0x08;
constexpr std::size_t kHeaderStandaloneCountOffset = 0x0C;
constexpr std::size_t kHeaderArchiveCountOffset = 0x10;
constexpr std::size_t kHeaderFlagsOffset = 0x14;
constexpr std::size_t kHeaderReservedOffset = 0x18;

constexpr std::size_t kStandaloneVariantHeaderBytes = 16;
constexpr std::size_t kVariantOrigSizeOffset = 0;
constexpr std::size_t kVariantOrigHashOffset = 4;
constexpr std::size_t kVariantNewSizeOffset = 8;
constexpr std::size_t kVariantNewHashOffset = 12;

constexpr std::size_t kArchiveFixedFieldsBytes = 36;
constexpr std::size_t kArchiveOrigGpkSizeOffset = 0;
constexpr std::size_t kArchiveNewGpkSizeOffset = 8;
constexpr std::size_t kArchiveOrigGtbSizeOffset = 16;
constexpr std::size_t kArchiveOrigGtbHashOffset = 20;
constexpr std::size_t kArchiveNewGtbSizeOffset = 24;
constexpr std::size_t kArchiveNewGtbHashOffset = 28;
constexpr std::size_t kArchiveEntryCountOffset = 32;

constexpr std::size_t kArchiveEntryHeaderBytes = 28;
constexpr std::size_t kEntryIndexOffset = 0;
constexpr std::size_t kEntryOrigOffsetOffset = 4;
constexpr std::size_t kEntryOrigSizeOffset = 12;
constexpr std::size_t kEntryOrigHashOffset = 16;
constexpr std::size_t kEntryNewSizeOffset = 20;
constexpr std::size_t kEntryNewHashOffset = 24;

constexpr std::size_t kCropTopIndex = 0;
constexpr std::size_t kCropLeftIndex = 1;
constexpr std::size_t kCropRightIndex = 2;
constexpr std::size_t kCropBottomIndex = 3;

constexpr int kRgbaChannels = 4;
constexpr int kRgbChannels = 3;
constexpr std::size_t kRIndex = 0;
constexpr std::size_t kGIndex = 1;
constexpr std::size_t kBIndex = 2;
constexpr std::size_t kAIndex = 3;
constexpr std::uint8_t kFullAlpha = 255u;
constexpr int kMaxChannelInt = 255;
constexpr long kMaxChannelLong = 255L;
constexpr float kMaxChannelFloat = 255.0f;

constexpr int kBt601RedWeight = 299;
constexpr int kBt601GreenWeight = 587;
constexpr int kBt601BlueWeight = 114;
constexpr int kBt601WeightSum = 1000;

constexpr std::size_t kPngSignatureBytes = 8;
constexpr std::size_t kPngIhdrTotalAfterSig = 25;
constexpr std::size_t kPngInsertChunkOffset = kPngSignatureBytes + kPngIhdrTotalAfterSig;
constexpr std::size_t kPngMinValidBytes = 33;
constexpr std::uint8_t kPngSignature[kPngSignatureBytes] = {
    0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
constexpr std::uint32_t kCrc32Init = 0xFFFFFFFFu;
constexpr std::uint32_t kCrc32Polynomial = 0xEDB88320u;
constexpr int kBitsPerByteInt = 8;
constexpr int kMaxSaltAttempts = 512;

int compute_bt601_luma(int r, int g, int b) noexcept {
    return (kBt601RedWeight * r + kBt601GreenWeight * g + kBt601BlueWeight * b) /
           kBt601WeightSum;
}

std::uint16_t read_u16_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[kByteIndex0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[kByteIndex1]) << kBitsPerByte);
}

std::uint32_t read_u32_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[kByteIndex0]) |
           (static_cast<std::uint32_t>(p[kByteIndex1]) << kByteShift1) |
           (static_cast<std::uint32_t>(p[kByteIndex2]) << kByteShift2) |
           (static_cast<std::uint32_t>(p[kByteIndex3]) << kByteShift3);
}

std::uint64_t read_u64_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(p[kByteIndex0]) |
           (static_cast<std::uint64_t>(p[kByteIndex1]) << kByteShift1) |
           (static_cast<std::uint64_t>(p[kByteIndex2]) << kByteShift2) |
           (static_cast<std::uint64_t>(p[kByteIndex3]) << kByteShift3) |
           (static_cast<std::uint64_t>(p[kByteIndex4]) << kByteShift4) |
           (static_cast<std::uint64_t>(p[kByteIndex5]) << kByteShift5) |
           (static_cast<std::uint64_t>(p[kByteIndex6]) << kByteShift6) |
           (static_cast<std::uint64_t>(p[kByteIndex7]) << kByteShift7);
}

void write_u32_le(std::uint8_t* p, std::uint32_t v) noexcept {
    p[kByteIndex0] = static_cast<std::uint8_t>(v & kLowByteMask);
    p[kByteIndex1] = static_cast<std::uint8_t>((v >> kByteShift1) & kLowByteMask);
    p[kByteIndex2] = static_cast<std::uint8_t>((v >> kByteShift2) & kLowByteMask);
    p[kByteIndex3] = static_cast<std::uint8_t>((v >> kByteShift3) & kLowByteMask);
}

void write_i32_le(std::uint8_t* p, std::int32_t v) noexcept {
    write_u32_le(p, static_cast<std::uint32_t>(v));
}

void write_u64_le(std::uint8_t* p, std::uint64_t v) noexcept {
    p[kByteIndex0] = static_cast<std::uint8_t>(v & kLowByteMask);
    p[kByteIndex1] = static_cast<std::uint8_t>((v >> kByteShift1) & kLowByteMask);
    p[kByteIndex2] = static_cast<std::uint8_t>((v >> kByteShift2) & kLowByteMask);
    p[kByteIndex3] = static_cast<std::uint8_t>((v >> kByteShift3) & kLowByteMask);
    p[kByteIndex4] = static_cast<std::uint8_t>((v >> kByteShift4) & kLowByteMask);
    p[kByteIndex5] = static_cast<std::uint8_t>((v >> kByteShift5) & kLowByteMask);
    p[kByteIndex6] = static_cast<std::uint8_t>((v >> kByteShift6) & kLowByteMask);
    p[kByteIndex7] = static_cast<std::uint8_t>((v >> kByteShift7) & kLowByteMask);
}

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kBitsPerByte) & kLowByteMask));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift1) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift2) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift3) & kLowByteMask));
}

void append_u64_le(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (std::uint32_t shift = 0; shift < kBitsPerU64; shift += kBitsPerByte) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & kLowByteMask));
    }
}

void append_u32_be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift3) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift2) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift1) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
}

std::uint32_t compute_crc32(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint32_t crc = kCrc32Init;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < kBitsPerByteInt; ++b) {
            const std::uint32_t mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1u));
            crc = (crc >> 1u) ^ (kCrc32Polynomial & mask);
        }
    }
    return ~crc;
}

bool contains_bytes(const std::uint8_t* hay,
                    std::size_t hay_len,
                    const std::uint8_t* needle,
                    std::size_t needle_len) noexcept {
    if (needle_len == 0 || hay_len < needle_len) {
        return false;
    }
    const auto* end = hay + hay_len;
    const auto* it = std::search(hay, end, needle, needle + needle_len);
    return it != end;
}

bool contains_bmp_file_header(const std::uint8_t* data, std::size_t len) noexcept {
    constexpr std::size_t kBitmapFileHeaderSize = 14;
    constexpr std::size_t kBmpMagicOffset0 = 0;
    constexpr std::size_t kBmpMagicOffset1 = 1;
    constexpr std::size_t kBmpReservedOffset0 = 6;
    constexpr std::size_t kBmpReservedOffset1 = 7;
    constexpr std::size_t kBmpReservedOffset2 = 8;
    constexpr std::size_t kBmpReservedOffset3 = 9;
    if (len < kBitmapFileHeaderSize) {
        return false;
    }
    for (std::size_t i = 0; i + kBitmapFileHeaderSize <= len; ++i) {
        if (data[i + kBmpMagicOffset0] == 'B' &&
            data[i + kBmpMagicOffset1] == 'M' &&
            data[i + kBmpReservedOffset0] == 0u &&
            data[i + kBmpReservedOffset1] == 0u &&
            data[i + kBmpReservedOffset2] == 0u &&
            data[i + kBmpReservedOffset3] == 0u) {
            return true;
        }
    }
    return false;
}

bool contains_forbidden_raw_signature(const std::uint8_t* data, std::size_t len) noexcept {
    static constexpr std::uint8_t kSigPackBmp[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    static constexpr std::uint8_t kSigPackType[] = {'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E', '='};
    static constexpr std::uint8_t kSigPng[] = {0x89u, 'P', 'N', 'G'};
    static constexpr std::uint8_t kSigDwqBmp[] = {'B', 'M', 'P', ' '};
    static constexpr std::uint8_t kSigOver2G[] = {'o', 'v', 'e', 'r', '2', 'G', '!'};

    return contains_bytes(data, len, kSigPackBmp, sizeof(kSigPackBmp)) ||
           contains_bytes(data, len, kSigPackType, sizeof(kSigPackType)) ||
           contains_bytes(data, len, kSigPng, sizeof(kSigPng)) ||
           contains_bytes(data, len, kSigDwqBmp, sizeof(kSigDwqBmp)) ||
           contains_bytes(data, len, kSigOver2G, sizeof(kSigOver2G)) ||
           contains_bmp_file_header(data, len);
}

std::vector<std::uint8_t> insert_png_text_salt_chunk(
    const std::vector<std::uint8_t>& png_bytes,
    int salt_index) {
    if (salt_index <= 0 || png_bytes.size() <= kPngInsertChunkOffset) {
        return png_bytes;
    }

    const std::string payload = std::string("Software") + '\0' + "mgi_" + std::to_string(salt_index);
    std::vector<std::uint8_t> chunk_type_and_data;
    chunk_type_and_data.reserve(kU32ByteSize + payload.size());
    chunk_type_and_data.push_back('t');
    chunk_type_and_data.push_back('E');
    chunk_type_and_data.push_back('X');
    chunk_type_and_data.push_back('t');
    chunk_type_and_data.insert(chunk_type_and_data.end(),
                               reinterpret_cast<const std::uint8_t*>(payload.data()),
                               reinterpret_cast<const std::uint8_t*>(payload.data()) + payload.size());

    const std::uint32_t crc = compute_crc32(chunk_type_and_data.data(), chunk_type_and_data.size());

    std::vector<std::uint8_t> out;
    out.reserve(png_bytes.size() + kU32ByteSize * 2 + chunk_type_and_data.size());
    out.insert(out.end(), png_bytes.begin(),
               png_bytes.begin() + static_cast<std::ptrdiff_t>(kPngInsertChunkOffset));
    append_u32_be(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), chunk_type_and_data.begin(), chunk_type_and_data.end());
    append_u32_be(out, crc);
    out.insert(out.end(),
               png_bytes.begin() + static_cast<std::ptrdiff_t>(kPngInsertChunkOffset),
               png_bytes.end());
    return out;
}

struct RawLine {
    std::string body;
    std::string ending;
};

std::vector<RawLine> split_preserving_line_endings(const std::string& text) {
    std::vector<RawLine> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back({text.substr(pos), {}});
            break;
        }
        if (nl > pos && text[nl - 1] == '\r') {
            lines.push_back({text.substr(pos, nl - 1 - pos), "\r\n"});
        } else {
            lines.push_back({text.substr(pos, nl - pos), "\n"});
        }
        pos = nl + 1;
    }
    return lines;
}

std::string join_raw_lines(const std::vector<RawLine>& lines) {
    std::size_t total = 0;
    for (const auto& ln : lines) {
        total += ln.body.size() + ln.ending.size();
    }
    std::string out;
    out.reserve(total);
    for (const auto& ln : lines) {
        out.append(ln.body);
        out.append(ln.ending);
    }
    return out;
}

std::string utf8_literal_to_cp932(std::string_view utf8) {
    const auto encoded = mgi::utf8_to_cp932_strict(std::string(utf8));
    if (!encoded) {
        throw std::runtime_error("Failed strict UTF-8 to CP932 conversion for menu literal");
    }
    return std::string(reinterpret_cast<const char*>(encoded->data()), encoded->size());
}

class ScopedComInit {
public:
    ScopedComInit() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedComInit() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

private:
    HRESULT hr_ = E_FAIL;
};

ComPtr<IWICImagingFactory> create_wic_factory() {
    ComPtr<IWICImagingFactory> factory;
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        throw std::runtime_error("Failed to create IWICImagingFactory");
    }
    return factory;
}

class ScopedMemoryDc {
public:
    ScopedMemoryDc() : hdc_(CreateCompatibleDC(nullptr)) {
        if (!hdc_) {
            throw std::runtime_error("CreateCompatibleDC failed");
        }
    }
    ~ScopedMemoryDc() {
        if (hdc_) {
            DeleteDC(hdc_);
        }
    }
    ScopedMemoryDc(const ScopedMemoryDc&) = delete;
    ScopedMemoryDc& operator=(const ScopedMemoryDc&) = delete;

    HDC get() const noexcept { return hdc_; }

private:
    HDC hdc_ = nullptr;
};

class ScopedGdiFont {
public:
    explicit ScopedGdiFont(HFONT hfont) noexcept : hfont_(hfont) {}
    ~ScopedGdiFont() {
        if (hfont_) {
            DeleteObject(hfont_);
        }
    }
    ScopedGdiFont(const ScopedGdiFont&) = delete;
    ScopedGdiFont& operator=(const ScopedGdiFont&) = delete;
    ScopedGdiFont(ScopedGdiFont&& other) noexcept : hfont_(other.hfont_) {
        other.hfont_ = nullptr;
    }
    ScopedGdiFont& operator=(ScopedGdiFont&& other) noexcept {
        if (this != &other) {
            if (hfont_) {
                DeleteObject(hfont_);
            }
            hfont_ = other.hfont_;
            other.hfont_ = nullptr;
        }
        return *this;
    }

    HFONT get() const noexcept { return hfont_; }

private:
    HFONT hfont_ = nullptr;
};

class ScopedDibSection {
public:
    ScopedDibSection(HDC hdc, int width, int height) : width_(width), height_(height) {
        constexpr WORD kDibPlanes = 1;
        constexpr WORD kDibBitCount32 = 32;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // top-down
        bmi.bmiHeader.biPlanes = kDibPlanes;
        bmi.bmiHeader.biBitCount = kDibBitCount32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hbmp_ = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!hbmp_ || !bits_) {
            throw std::runtime_error("CreateDIBSection failed");
        }
    }
    ~ScopedDibSection() {
        if (hbmp_) {
            DeleteObject(hbmp_);
        }
    }
    ScopedDibSection(const ScopedDibSection&) = delete;
    ScopedDibSection& operator=(const ScopedDibSection&) = delete;

    HBITMAP bitmap() const noexcept { return hbmp_; }
    std::uint8_t* bytes() const noexcept { return static_cast<std::uint8_t*>(bits_); }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

private:
    HBITMAP hbmp_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

std::vector<std::uint8_t> read_binary_file(const fs::path& p) {
    return mgi::read_file(p.u8string());
}

void write_binary_file(const fs::path& p, const std::vector<std::uint8_t>& data) {
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    mgi::write_file(p.u8string(), data);
}

std::vector<std::uint8_t> read_file_slice(std::ifstream& in,
                                          std::uint64_t offset,
                                          std::uint32_t size) {
    std::vector<std::uint8_t> buf(size, 0u);
    if (size == 0) {
        return buf;
    }
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in.good()) {
        throw std::runtime_error("Failed to seek to GPK entry slice");
    }
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error("Short read on GPK entry slice");
    }
    return buf;
}

void serialize_archive_section_into(std::vector<std::uint8_t>& out,
                                    const ArchiveSectionRecord& sec) {
    if (sec.archive_stem.empty() ||
        sec.archive_stem.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("Invalid archive_stem length during serialization");
    }
    if (sec.masked_gtb.size() != sec.new_gtb_size) {
        throw std::runtime_error("Archive masked_gtb length mismatch");
    }
    append_u16_le(out, static_cast<std::uint16_t>(sec.archive_stem.size()));
    out.insert(out.end(), sec.archive_stem.begin(), sec.archive_stem.end());
    append_u64_le(out, sec.orig_gpk_size);
    append_u64_le(out, sec.new_gpk_size);
    append_u32_le(out, sec.orig_gtb_size);
    append_u32_le(out, sec.orig_gtb_hash);
    append_u32_le(out, sec.new_gtb_size);
    append_u32_le(out, sec.new_gtb_hash);
    append_u32_le(out, static_cast<std::uint32_t>(sec.entries.size()));
    out.insert(out.end(), sec.masked_gtb.begin(), sec.masked_gtb.end());

    for (const auto& entry : sec.entries) {
        if (entry.masked_entry.size() != entry.new_size) {
            throw std::runtime_error("Archive entry masked_entry length mismatch");
        }
        append_u32_le(out, entry.entry_index);
        append_u64_le(out, entry.orig_offset);
        append_u32_le(out, entry.orig_size);
        append_u32_le(out, entry.orig_data_hash);
        append_u32_le(out, entry.new_size);
        append_u32_le(out, entry.new_data_hash);
        out.insert(out.end(), entry.masked_entry.begin(), entry.masked_entry.end());
    }
}

struct RgbaColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = kFullAlpha;
};

struct RectI {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    int width() const noexcept { return x1 - x0; }
    int height() const noexcept { return y1 - y0; }
};

void blend_pixel_porter_duff_over(std::uint8_t* dst,
                                  std::uint8_t sr,
                                  std::uint8_t sg,
                                  std::uint8_t sb,
                                  float src_alpha_norm) noexcept {
    constexpr float kOutAlphaEpsilon = 1.0e-6f;
    if (src_alpha_norm <= 0.0f) {
        return;
    }
    const float sa = std::clamp(src_alpha_norm, 0.0f, 1.0f);
    const float da = static_cast<float>(dst[kAIndex]) / kMaxChannelFloat;
    const float out_a = sa + da * (1.0f - sa);
    if (out_a <= kOutAlphaEpsilon) {
        dst[kRIndex] = 0;
        dst[kGIndex] = 0;
        dst[kBIndex] = 0;
        dst[kAIndex] = 0;
        return;
    }
    const float inv_out_a = 1.0f / out_a;
    const float dr = static_cast<float>(dst[kRIndex]);
    const float dg = static_cast<float>(dst[kGIndex]);
    const float db = static_cast<float>(dst[kBIndex]);

    const float out_r = (static_cast<float>(sr) * sa + dr * da * (1.0f - sa)) * inv_out_a;
    const float out_g = (static_cast<float>(sg) * sa + dg * da * (1.0f - sa)) * inv_out_a;
    const float out_b = (static_cast<float>(sb) * sa + db * da * (1.0f - sa)) * inv_out_a;

    dst[kRIndex] = static_cast<std::uint8_t>(std::clamp(std::lround(out_r), 0L, kMaxChannelLong));
    dst[kGIndex] = static_cast<std::uint8_t>(std::clamp(std::lround(out_g), 0L, kMaxChannelLong));
    dst[kBIndex] = static_cast<std::uint8_t>(std::clamp(std::lround(out_b), 0L, kMaxChannelLong));
    dst[kAIndex] = static_cast<std::uint8_t>(
        std::clamp(std::lround(out_a * kMaxChannelFloat), 0L, kMaxChannelLong));
}

void overwrite_rect_rgba(RgbaImage& img, const RectI& r, RgbaColor c) noexcept {
    const int x0 = std::max(0, r.x0);
    const int y0 = std::max(0, r.y0);
    const int x1 = std::min(img.width, r.x1);
    const int y1 = std::min(img.height, r.y1);
    for (int y = y0; y < y1; ++y) {
        std::uint8_t* row =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * kRgbaChannels;
        for (int x = x0; x < x1; ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * kRgbaChannels;
            px[kRIndex] = c.r;
            px[kGIndex] = c.g;
            px[kBIndex] = c.b;
            px[kAIndex] = c.a;
        }
    }
}

void copy_rect_from_clean_plate(RgbaImage& dst,
                                const RgbaImage& clean_src,
                                const RectI& r) {
    if (dst.width != clean_src.width || dst.height != clean_src.height) {
        throw std::runtime_error("Clean plate dimensions do not match target sprite");
    }
    const int x0 = std::max(0, r.x0);
    const int y0 = std::max(0, r.y0);
    const int x1 = std::min(dst.width, r.x1);
    const int y1 = std::min(dst.height, r.y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    const std::size_t copy_bytes = static_cast<std::size_t>(x1 - x0) * kRgbaChannels;
    for (int y = y0; y < y1; ++y) {
        const std::size_t off =
            (static_cast<std::size_t>(y) * dst.width + static_cast<std::size_t>(x0)) *
            kRgbaChannels;
        std::memcpy(dst.pixels.data() + off, clean_src.pixels.data() + off, copy_bytes);
    }
}

void tile_rect_from_patch(RgbaImage& img, const RectI& dst_rect, const RectI& src_patch) {
    const int sw = src_patch.width();
    const int sh = src_patch.height();
    if (sw <= 0 || sh <= 0) {
        return;
    }
    RgbaImage patch;
    patch.width = sw;
    patch.height = sh;
    patch.pixels.resize(static_cast<std::size_t>(sw) * sh * kRgbaChannels);
    for (int sy = 0; sy < sh; ++sy) {
        const int py = std::clamp(src_patch.y0 + sy, 0, img.height - 1);
        for (int sx = 0; sx < sw; ++sx) {
            const int px = std::clamp(src_patch.x0 + sx, 0, img.width - 1);
            const std::uint8_t* s =
                img.pixels.data() +
                (static_cast<std::size_t>(py) * img.width + static_cast<std::size_t>(px)) *
                    kRgbaChannels;
            std::uint8_t* d =
                patch.pixels.data() +
                (static_cast<std::size_t>(sy) * sw + static_cast<std::size_t>(sx)) *
                    kRgbaChannels;
            std::memcpy(d, s, kRgbaChannels);
        }
    }

    const int x0 = std::max(0, dst_rect.x0);
    const int y0 = std::max(0, dst_rect.y0);
    const int x1 = std::min(img.width, dst_rect.x1);
    const int y1 = std::min(img.height, dst_rect.y1);
    for (int y = y0; y < y1; ++y) {
        const int sy = (y - y0) % sh;
        for (int x = x0; x < x1; ++x) {
            const int sx = (x - x0) % sw;
            const std::uint8_t* s =
                patch.pixels.data() +
                (static_cast<std::size_t>(sy) * sw + static_cast<std::size_t>(sx)) *
                    kRgbaChannels;
            std::uint8_t* d =
                img.pixels.data() +
                (static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) *
                    kRgbaChannels;
            std::memcpy(d, s, kRgbaChannels);
        }
    }
}

void inpaint_masked_region_onion_peel(RgbaImage& img,
                                      std::vector<std::uint8_t>& mask,
                                      const RectI& bounds,
                                      int sample_radius,
                                      int min_sample_luma) {
    constexpr std::size_t kInitialInpaintLayerCapacity = 4096;
    const int x0 = std::max(0, bounds.x0);
    const int y0 = std::max(0, bounds.y0);
    const int x1 = std::min(img.width, bounds.x1);
    const int y1 = std::min(img.height, bounds.y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    struct PendingFill {
        int x = 0;
        int y = 0;
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
    };
    std::vector<PendingFill> layer;
    layer.reserve(kInitialInpaintLayerCapacity);

    while (true) {
        layer.clear();
        std::size_t remaining = 0;

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * img.width + x;
                if (mask[idx] == 0) {
                    continue;
                }
                ++remaining;

                bool touches_unmasked = false;
                for (int ny = std::max(0, y - 1); ny <= std::min(img.height - 1, y + 1); ++ny) {
                    for (int nx = std::max(0, x - 1); nx <= std::min(img.width - 1, x + 1); ++nx) {
                        if (mask[static_cast<std::size_t>(ny) * img.width + nx] == 0) {
                            touches_unmasked = true;
                            break;
                        }
                    }
                    if (touches_unmasked) {
                        break;
                    }
                }
                if (!touches_unmasked) {
                    continue;
                }

                double wr = 0.0;
                double wg = 0.0;
                double wb = 0.0;
                double wsum = 0.0;
                double fallback_r = 0.0;
                double fallback_g = 0.0;
                double fallback_b = 0.0;
                double fallback_wsum = 0.0;

                for (int dy = -sample_radius; dy <= sample_radius; ++dy) {
                    const int sy = y + dy;
                    if (sy < 0 || sy >= img.height) {
                        continue;
                    }
                    for (int dx = -sample_radius; dx <= sample_radius; ++dx) {
                        const int sx = x + dx;
                        if (sx < 0 || sx >= img.width) {
                            continue;
                        }
                        const std::size_t sidx = static_cast<std::size_t>(sy) * img.width + sx;
                        if (mask[sidx] != 0) {
                            continue;
                        }
                        const double dist2 = static_cast<double>(dx * dx + dy * dy);
                        const double w = 1.0 / (1.0 + dist2);
                        const std::uint8_t* sp = img.pixels.data() + sidx * kRgbaChannels;
                        fallback_r += w * sp[kRIndex];
                        fallback_g += w * sp[kGIndex];
                        fallback_b += w * sp[kBIndex];
                        fallback_wsum += w;
                        if (compute_bt601_luma(sp[kRIndex], sp[kGIndex], sp[kBIndex]) >=
                            min_sample_luma) {
                            wr += w * sp[kRIndex];
                            wg += w * sp[kGIndex];
                            wb += w * sp[kBIndex];
                            wsum += w;
                        }
                    }
                }

                if (wsum > 0.0) {
                    layer.push_back({
                        x,
                        y,
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(wr / wsum), 0L, kMaxChannelLong)),
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(wg / wsum), 0L, kMaxChannelLong)),
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(wb / wsum), 0L, kMaxChannelLong)),
                    });
                    continue;
                }
                if (fallback_wsum > 0.0) {
                    layer.push_back({
                        x,
                        y,
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(fallback_r / fallback_wsum), 0L, kMaxChannelLong)),
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(fallback_g / fallback_wsum), 0L, kMaxChannelLong)),
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(fallback_b / fallback_wsum), 0L, kMaxChannelLong)),
                    });
                }
            }
        }

        if (remaining == 0 || layer.empty()) {
            break;
        }
        for (const auto& f : layer) {
            const std::size_t idx = static_cast<std::size_t>(f.y) * img.width + f.x;
            std::uint8_t* px = img.pixels.data() + idx * kRgbaChannels;
            px[kRIndex] = f.r;
            px[kGIndex] = f.g;
            px[kBIndex] = f.b;
            px[kAIndex] = kFullAlpha;
            mask[idx] = 0;
        }
    }
}

enum class HAlign { kLeft, kCenter, kRight };

struct TextEffectStyle {
    RgbaColor fill{255, 255, 255, 255};
    bool has_glow = false;
    RgbaColor glow{0, 0, 0, 0};
    int glow_radius_px = 1;
    bool has_outline = false;
    RgbaColor outline{0, 0, 0, 255};
    int outline_radius_px = 2;
    bool has_drop_shadow = false;
    RgbaColor shadow{0, 0, 0, 180};
    int shadow_dx = 1;
    int shadow_dy = 1;
};

constexpr int kSupersampleFactor = 3;
constexpr int kRotatedSupersampleFactor = 2;
constexpr int kMinAutoFitFontPx = 9;
constexpr WORD kMissingGlyphIndex = 0xFFFFu;
constexpr float kUnpremultiplyAlphaEpsilon = 1.0e-5f;

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8,
                                           0,
                                           utf8.data(),
                                           static_cast<int>(utf8.size()),
                                           nullptr,
                                           0);
    if (needed <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed for UTF-8 label");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        utf8.data(),
                        static_cast<int>(utf8.size()),
                        out.data(),
                        needed);
    return out;
}

ScopedGdiFont create_scaled_font(const wchar_t* family, int pixel_height, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -pixel_height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(lf.lfFaceName, family, _TRUNCATE);

    HFONT hf = CreateFontIndirectW(&lf);
    if (!hf) {
        throw std::runtime_error("CreateFontIndirectW failed");
    }
    return ScopedGdiFont(hf);
}

struct FontRun {
    std::wstring text;
    const wchar_t* family = L"Segoe UI";
    SIZE extent{};
    int ascent = 0;
    int descent = 0;
};

bool font_supports_wchar(HDC hdc, HFONT hfont, wchar_t ch) {
    if (ch == L' ') {
        return true;
    }
    HGDIOBJ prev = SelectObject(hdc, hfont);
    WORD gi = 0;
    const DWORD res = GetGlyphIndicesW(hdc, &ch, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS);
    SelectObject(hdc, prev);
    return res != GDI_ERROR && gi != kMissingGlyphIndex;
}

struct MeasuredLineRuns {
    std::vector<FontRun> runs;
    int total_width = 0;
    int max_ascent = 0;
    int max_descent = 0;
    int total_height() const noexcept { return max_ascent + max_descent; }
};

MeasuredLineRuns layout_text_runs(HDC hdc,
                                  const std::wstring& wtext,
                                  const wchar_t* primary_family,
                                  int scaled_px,
                                  int weight) {
    static constexpr std::array<const wchar_t*, 4> kFallbackFamilies = {
        L"Yu Gothic UI", L"MS Gothic", L"Segoe UI Symbol", L"Meiryo"};

    ScopedGdiFont primary_font = create_scaled_font(primary_family, scaled_px, weight);
    std::vector<ScopedGdiFont> fallback_fonts;
    fallback_fonts.reserve(kFallbackFamilies.size());
    for (const wchar_t* fam : kFallbackFamilies) {
        fallback_fonts.push_back(create_scaled_font(fam, scaled_px, weight));
    }

    auto choose_family = [&](wchar_t ch) -> const wchar_t* {
        if (font_supports_wchar(hdc, primary_font.get(), ch)) {
            return primary_family;
        }
        for (std::size_t i = 0; i < kFallbackFamilies.size(); ++i) {
            if (font_supports_wchar(hdc, fallback_fonts[i].get(), ch)) {
                return kFallbackFamilies[i];
            }
        }
        return primary_family;
    };

    MeasuredLineRuns out;
    for (wchar_t ch : wtext) {
        const wchar_t* fam = choose_family(ch);
        if (out.runs.empty() || std::wcscmp(out.runs.back().family, fam) != 0) {
            FontRun r;
            r.family = fam;
            r.text.push_back(ch);
            out.runs.push_back(std::move(r));
        } else {
            out.runs.back().text.push_back(ch);
        }
    }

    for (auto& run : out.runs) {
        ScopedGdiFont rf = create_scaled_font(run.family, scaled_px, weight);
        HGDIOBJ prev = SelectObject(hdc, rf.get());
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, run.text.c_str(), static_cast<int>(run.text.size()), &sz);
        SelectObject(hdc, prev);

        run.extent = sz;
        run.ascent = tm.tmAscent;
        run.descent = tm.tmDescent;
        out.total_width += sz.cx;
        out.max_ascent = std::max(out.max_ascent, static_cast<int>(tm.tmAscent));
        out.max_descent = std::max(out.max_descent, static_cast<int>(tm.tmDescent));
    }
    return out;
}

// Rasterizes a single line at `scale` supersampling and returns a 1x-resolution
// straight-RGBA sprite (when downsample_to_1x == true) or keeps `scale`x
// resolution (when downsample_to_1x == false, used for smooth -45 deg rotation).
RgbaImage rasterize_styled_line_sprite(std::string_view utf8_text,
                                       const wchar_t* primary_family,
                                       int font_px,
                                       int weight,
                                       const TextEffectStyle& style,
                                       int max_width_1x,
                                       int scale,
                                       bool downsample_to_1x) {
    const std::wstring wtext = utf8_to_wide(utf8_text);
    if (wtext.empty()) {
        return {};
    }

    ScopedMemoryDc mem_dc;
    HDC hdc = mem_dc.get();

    int chosen_px = font_px;
    MeasuredLineRuns measured =
        layout_text_runs(hdc, wtext, primary_family, chosen_px * scale, weight);
    if (max_width_1x > 0) {
        while (chosen_px > kMinAutoFitFontPx &&
               measured.total_width > max_width_1x * scale) {
            --chosen_px;
            measured = layout_text_runs(hdc, wtext, primary_family, chosen_px * scale, weight);
        }
    }

    const int effect_pad_1x = std::max({
        style.has_outline ? style.outline_radius_px + 2 : 1,
        style.has_glow ? style.glow_radius_px + 2 : 1,
        style.has_drop_shadow ? std::max(std::abs(style.shadow_dx), std::abs(style.shadow_dy)) + 2
                              : 1,
    });
    const int pad_hi = (effect_pad_1x + 2) * scale;
    const int raw_w_hi = measured.total_width + pad_hi * 2;
    const int raw_h_hi = measured.total_height() + pad_hi * 2;
    const int w_1x = (raw_w_hi + scale - 1) / scale;
    const int h_1x = (raw_h_hi + scale - 1) / scale;
    const int w_hi = w_1x * scale;
    const int h_hi = h_1x * scale;

    ScopedDibSection dib(hdc, w_hi, h_hi);
    std::memset(dib.bytes(), 0, static_cast<std::size_t>(w_hi) * h_hi * kRgbaChannels);

    HGDIOBJ prev_bmp = SelectObject(hdc, dib.bitmap());
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(kFullAlpha, kFullAlpha, kFullAlpha));
    SetTextAlign(hdc, TA_LEFT | TA_BASELINE);

    int pen_x = pad_hi;
    const int baseline_y = pad_hi + measured.max_ascent;
    for (const auto& run : measured.runs) {
        ScopedGdiFont rf = create_scaled_font(run.family, chosen_px * scale, weight);
        HGDIOBJ prev_font = SelectObject(hdc, rf.get());
        TextOutW(hdc, pen_x, baseline_y, run.text.c_str(), static_cast<int>(run.text.size()));
        SelectObject(hdc, prev_font);
        pen_x += run.extent.cx;
    }
    GdiFlush();
    SelectObject(hdc, prev_bmp);

    // Extract glyph coverage in [0..255] at hi-res.
    std::vector<std::uint8_t> glyph_cov(static_cast<std::size_t>(w_hi) * h_hi, 0u);
    const std::uint8_t* dib_px = dib.bytes();
    for (std::size_t i = 0; i < glyph_cov.size(); ++i) {
        const std::size_t b_off = i * kRgbaChannels;
        const int lum =
            (static_cast<int>(dib_px[b_off + kByteIndex0]) +
             static_cast<int>(dib_px[b_off + kByteIndex1]) +
             static_cast<int>(dib_px[b_off + kByteIndex2])) /
            kRgbChannels;
        glyph_cov[i] = static_cast<std::uint8_t>(std::clamp(lum, 0, kMaxChannelInt));
    }

    // Build optional circular dilated coverage for outline or glow at hi-res.
    auto build_dilated_coverage = [&](int radius_hi) {
        std::vector<std::uint8_t> out(glyph_cov.size(), 0u);
        if (radius_hi <= 0) {
            return out;
        }
        std::vector<std::pair<int, int>> offsets;
        const int r2 = radius_hi * radius_hi;
        for (int dy = -radius_hi; dy <= radius_hi; ++dy) {
            for (int dx = -radius_hi; dx <= radius_hi; ++dx) {
                if (dx * dx + dy * dy <= r2) {
                    offsets.emplace_back(dx, dy);
                }
            }
        }
        for (int y = 0; y < h_hi; ++y) {
            for (int x = 0; x < w_hi; ++x) {
                const std::uint8_t src_v = glyph_cov[static_cast<std::size_t>(y) * w_hi + x];
                if (src_v == 0) {
                    continue;
                }
                for (const auto& [dx, dy] : offsets) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx >= 0 && nx < w_hi && ny >= 0 && ny < h_hi) {
                        std::uint8_t& dst_v = out[static_cast<std::size_t>(ny) * w_hi + nx];
                        if (src_v > dst_v) {
                            dst_v = src_v;
                        }
                    }
                }
            }
        }
        return out;
    };

    std::vector<std::uint8_t> outline_cov;
    if (style.has_outline && style.outline_radius_px > 0) {
        outline_cov = build_dilated_coverage(style.outline_radius_px * scale);
    }

    std::vector<std::uint8_t> glow_cov;
    if (style.has_glow && style.glow_radius_px > 0) {
        std::vector<std::uint8_t> dilated = build_dilated_coverage(style.glow_radius_px * scale);
        // Box-blur dilated coverage once for soft glow halo.
        glow_cov.assign(dilated.size(), 0u);
        const int blur_r = std::max(1, scale);
        for (int y = 0; y < h_hi; ++y) {
            for (int x = 0; x < w_hi; ++x) {
                int sum = 0;
                int cnt = 0;
                for (int dy = -blur_r; dy <= blur_r; ++dy) {
                    const int sy = std::clamp(y + dy, 0, h_hi - 1);
                    for (int dx = -blur_r; dx <= blur_r; ++dx) {
                        const int sx = std::clamp(x + dx, 0, w_hi - 1);
                        sum += dilated[static_cast<std::size_t>(sy) * w_hi + sx];
                        ++cnt;
                    }
                }
                glow_cov[static_cast<std::size_t>(y) * w_hi + x] =
                    static_cast<std::uint8_t>(sum / cnt);
            }
        }
    }

    // Composite hi-res premultiplied RGBA buffer, then either downsample or return hi-res.
    std::vector<float> pm_hi(static_cast<std::size_t>(w_hi) * h_hi * kRgbaChannels, 0.0f);
    auto over_pm = [&](std::size_t idx, RgbaColor col, float cov_01) {
        const float sa = (static_cast<float>(col.a) / kMaxChannelFloat) * cov_01;
        if (sa <= 0.0f) {
            return;
        }
        float* dst = pm_hi.data() + idx * kRgbaChannels;
        const float inv = 1.0f - sa;
        dst[kRIndex] = static_cast<float>(col.r) * sa + dst[kRIndex] * inv;
        dst[kGIndex] = static_cast<float>(col.g) * sa + dst[kGIndex] * inv;
        dst[kBIndex] = static_cast<float>(col.b) * sa + dst[kBIndex] * inv;
        dst[kAIndex] = sa + dst[kAIndex] * inv;
    };

    const int sh_dx_hi = style.shadow_dx * scale;
    const int sh_dy_hi = style.shadow_dy * scale;

    for (int y = 0; y < h_hi; ++y) {
        for (int x = 0; x < w_hi; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * w_hi + x;
            if (style.has_drop_shadow) {
                const int sx = x - sh_dx_hi;
                const int sy = y - sh_dy_hi;
                if (sx >= 0 && sx < w_hi && sy >= 0 && sy < h_hi) {
                    const float sc =
                        static_cast<float>(glyph_cov[static_cast<std::size_t>(sy) * w_hi + sx]) /
                        kMaxChannelFloat;
                    over_pm(idx, style.shadow, sc);
                }
            }
            if (!glow_cov.empty()) {
                over_pm(idx, style.glow, static_cast<float>(glow_cov[idx]) / kMaxChannelFloat);
            }
            if (!outline_cov.empty()) {
                over_pm(idx,
                        style.outline,
                        static_cast<float>(outline_cov[idx]) / kMaxChannelFloat);
            }
            over_pm(idx, style.fill, static_cast<float>(glyph_cov[idx]) / kMaxChannelFloat);
        }
    }

    if (!downsample_to_1x) {
        RgbaImage hi_img;
        hi_img.width = w_hi;
        hi_img.height = h_hi;
        hi_img.pixels.resize(static_cast<std::size_t>(w_hi) * h_hi * kRgbaChannels, 0u);
        for (std::size_t i = 0; i < static_cast<std::size_t>(w_hi) * h_hi; ++i) {
            const float* pm = pm_hi.data() + i * kRgbaChannels;
            std::uint8_t* out = hi_img.pixels.data() + i * kRgbaChannels;
            const float a = pm[kAIndex];
            if (a > kUnpremultiplyAlphaEpsilon) {
                out[kRIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(pm[kRIndex] / a), 0L, kMaxChannelLong));
                out[kGIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(pm[kGIndex] / a), 0L, kMaxChannelLong));
                out[kBIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(pm[kBIndex] / a), 0L, kMaxChannelLong));
                out[kAIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(a * kMaxChannelFloat), 0L, kMaxChannelLong));
            }
        }
        return hi_img;
    }

    RgbaImage out_1x;
    out_1x.width = w_1x;
    out_1x.height = h_1x;
    out_1x.pixels.resize(static_cast<std::size_t>(w_1x) * h_1x * kRgbaChannels, 0u);

    const float inv_area = 1.0f / static_cast<float>(scale * scale);
    for (int y = 0; y < h_1x; ++y) {
        for (int x = 0; x < w_1x; ++x) {
            float sr = 0.0f;
            float sg = 0.0f;
            float sb = 0.0f;
            float sa = 0.0f;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    const std::size_t hi_idx =
                        (static_cast<std::size_t>(y * scale + sy) * w_hi + (x * scale + sx)) *
                        kRgbaChannels;
                    sr += pm_hi[hi_idx + kRIndex];
                    sg += pm_hi[hi_idx + kGIndex];
                    sb += pm_hi[hi_idx + kBIndex];
                    sa += pm_hi[hi_idx + kAIndex];
                }
            }
            sr *= inv_area;
            sg *= inv_area;
            sb *= inv_area;
            sa *= inv_area;

            std::uint8_t* dst =
                out_1x.pixels.data() +
                (static_cast<std::size_t>(y) * w_1x + static_cast<std::size_t>(x)) *
                    kRgbaChannels;
            if (sa > kUnpremultiplyAlphaEpsilon) {
                dst[kRIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(sr / sa), 0L, kMaxChannelLong));
                dst[kGIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(sg / sa), 0L, kMaxChannelLong));
                dst[kBIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(sb / sa), 0L, kMaxChannelLong));
                dst[kAIndex] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(sa * kMaxChannelFloat), 0L, kMaxChannelLong));
            }
        }
    }
    return out_1x;
}

int compute_sprite_padding_1x(const TextEffectStyle& style) noexcept {
    const int effect_pad_1x = std::max({
        style.has_outline ? style.outline_radius_px + 2 : 1,
        style.has_glow ? style.glow_radius_px + 2 : 1,
        style.has_drop_shadow ? std::max(std::abs(style.shadow_dx), std::abs(style.shadow_dy)) + 2
                              : 1,
    });
    return effect_pad_1x + 2;
}

void draw_styled_text_in_rect(RgbaImage& canvas,
                              std::string_view utf8_text,
                              const RectI& text_rect,
                              const wchar_t* font_family,
                              int font_px,
                              int weight,
                              HAlign halign,
                              const TextEffectStyle& style,
                              const RectI* optional_clip = nullptr) {
    const int max_w = std::max(1, text_rect.width());
    const RgbaImage sprite = rasterize_styled_line_sprite(
        utf8_text, font_family, font_px, weight, style, max_w, kSupersampleFactor, true);
    if (sprite.width <= 0 || sprite.height <= 0) {
        return;
    }

    const int pad_1x = compute_sprite_padding_1x(style);
    const int glyph_w = std::max(0, sprite.width - pad_1x * 2);

    int dst_x0 = text_rect.x0 - pad_1x;
    if (halign == HAlign::kCenter) {
        const int center_x = (text_rect.x0 + text_rect.x1) / 2;
        dst_x0 = center_x - sprite.width / 2;
    } else if (halign == HAlign::kRight) {
        dst_x0 = text_rect.x1 - glyph_w - pad_1x;
    }

    const int center_y = (text_rect.y0 + text_rect.y1) / 2;
    const int dst_y0 = center_y - sprite.height / 2;

    const int clip_x0 = optional_clip ? std::max(0, optional_clip->x0) : 0;
    const int clip_y0 = optional_clip ? std::max(0, optional_clip->y0) : 0;
    const int clip_x1 = optional_clip ? std::min(canvas.width, optional_clip->x1) : canvas.width;
    const int clip_y1 = optional_clip ? std::min(canvas.height, optional_clip->y1) : canvas.height;

    for (int sy = 0; sy < sprite.height; ++sy) {
        const int dy = dst_y0 + sy;
        if (dy < clip_y0 || dy >= clip_y1) {
            continue;
        }
        for (int sx = 0; sx < sprite.width; ++sx) {
            const int dx = dst_x0 + sx;
            if (dx < clip_x0 || dx >= clip_x1) {
                continue;
            }
            const std::uint8_t* sp =
                sprite.pixels.data() +
                (static_cast<std::size_t>(sy) * sprite.width + static_cast<std::size_t>(sx)) *
                    kRgbaChannels;
            if (sp[kAIndex] == 0) {
                continue;
            }
            std::uint8_t* dp =
                canvas.pixels.data() +
                (static_cast<std::size_t>(dy) * canvas.width + static_cast<std::size_t>(dx)) *
                    kRgbaChannels;
            blend_pixel_porter_duff_over(dp,
                                         sp[kRIndex],
                                         sp[kGIndex],
                                         sp[kBIndex],
                                         static_cast<float>(sp[kAIndex]) / kMaxChannelFloat);
        }
    }
}

RgbaImage rasterize_styled_callout_sprite(std::string_view utf8_text,
                                          const wchar_t* primary_family,
                                          int font_px,
                                          int weight,
                                          const TextEffectStyle& style,
                                          int scale) {
    if (utf8_text.find('\n') == std::string_view::npos) {
        return rasterize_styled_line_sprite(
            utf8_text, primary_family, font_px, weight, style, 0, scale, false);
    }

    std::vector<RgbaImage> line_sprites;
    std::size_t pos = 0;
    while (pos <= utf8_text.size()) {
        const std::size_t nl = utf8_text.find('\n', pos);
        const std::string_view part = (nl == std::string_view::npos)
                                          ? utf8_text.substr(pos)
                                          : utf8_text.substr(pos, nl - pos);
        if (!part.empty()) {
            RgbaImage sp = rasterize_styled_line_sprite(
                part, primary_family, font_px, weight, style, 0, scale, false);
            if (sp.width > 0 && sp.height > 0) {
                line_sprites.push_back(std::move(sp));
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }

    if (line_sprites.empty()) {
        return {};
    }
    if (line_sprites.size() == 1) {
        return std::move(line_sprites.front());
    }

    constexpr int kInterLineClearGap1x = 2;
    const int pad_1x = compute_sprite_padding_1x(style);
    const int overlap_hi = std::max(0, (pad_1x * 2 - kInterLineClearGap1x) * scale);

    int max_w = 0;
    int total_h = 0;
    for (std::size_t i = 0; i < line_sprites.size(); ++i) {
        max_w = std::max(max_w, line_sprites[i].width);
        total_h += line_sprites[i].height;
        if (i > 0) {
            total_h -= std::min(overlap_hi, line_sprites[i].height / 2);
        }
    }

    RgbaImage combined;
    combined.width = max_w;
    combined.height = std::max(1, total_h);
    combined.pixels.assign(
        static_cast<std::size_t>(combined.width) * combined.height * kRgbaChannels, 0u);

    int cur_y = 0;
    for (std::size_t i = 0; i < line_sprites.size(); ++i) {
        const RgbaImage& sp = line_sprites[i];
        if (i > 0) {
            cur_y -= std::min(overlap_hi, sp.height / 2);
        }
        const int dst_x0 = (combined.width - sp.width) / 2;
        for (int sy = 0; sy < sp.height; ++sy) {
            const int dy = cur_y + sy;
            if (dy < 0 || dy >= combined.height) {
                continue;
            }
            for (int sx = 0; sx < sp.width; ++sx) {
                const int dx = dst_x0 + sx;
                if (dx < 0 || dx >= combined.width) {
                    continue;
                }
                const std::uint8_t* src_px =
                    sp.pixels.data() +
                    (static_cast<std::size_t>(sy) * sp.width + static_cast<std::size_t>(sx)) *
                        kRgbaChannels;
                if (src_px[kAIndex] == 0) {
                    continue;
                }
                std::uint8_t* dst_px =
                    combined.pixels.data() +
                    (static_cast<std::size_t>(dy) * combined.width + static_cast<std::size_t>(dx)) *
                        kRgbaChannels;
                blend_pixel_porter_duff_over(dst_px,
                                             src_px[kRIndex],
                                             src_px[kGIndex],
                                             src_px[kBIndex],
                                             static_cast<float>(src_px[kAIndex]) / kMaxChannelFloat);
            }
        }
        cur_y += sp.height;
    }
    return combined;
}

// Renders `-45 deg` rotated callout text (sloping bottom-left to top-right),
// auto-fitting `font_px` and sliding along the invariant `-45 deg` lane
// (`cx + cy == const`) so no glyph or outline ever clips `clip_bounds`.
void draw_rotated_minus45_callout(RgbaImage& canvas,
                                  std::string_view utf8_text,
                                  int target_cx,
                                  int target_cy,
                                  int initial_font_px,
                                  int weight,
                                  const TextEffectStyle& style,
                                  const RectI& clip_bounds,
                                  int edge_margin_px = 4) {
    constexpr double kInvSqrt2 = 0.70710678118654752440;

    int font_px = initial_font_px;
    RgbaImage hi_sprite;
    double cx = static_cast<double>(target_cx);
    double cy = static_cast<double>(target_cy);
    const double lane_sum = cx + cy;

    double half_ext = 0.0;
    while (font_px >= kMinAutoFitFontPx) {
        hi_sprite = rasterize_styled_callout_sprite(utf8_text,
                                                    L"Segoe UI",
                                                    font_px,
                                                    weight,
                                                    style,
                                                    kRotatedSupersampleFactor);
        const double w_1x =
            static_cast<double>(hi_sprite.width) / static_cast<double>(kRotatedSupersampleFactor);
        const double h_1x =
            static_cast<double>(hi_sprite.height) / static_cast<double>(kRotatedSupersampleFactor);
        half_ext = 0.5 * kInvSqrt2 * (w_1x + h_1x);

        const double x_lo =
            std::max(static_cast<double>(clip_bounds.x0 + edge_margin_px),
                     lane_sum - static_cast<double>(clip_bounds.y1 - edge_margin_px)) +
            half_ext;
        const double x_hi =
            std::min(static_cast<double>(clip_bounds.x1 - edge_margin_px),
                     lane_sum - static_cast<double>(clip_bounds.y0 + edge_margin_px)) -
            half_ext;

        if (x_lo <= x_hi) {
            cx = std::clamp(static_cast<double>(target_cx), x_lo, x_hi);
            cy = lane_sum - cx;
            break;
        }

        // If the target lane near a corner is shorter than the full string, also allow
        // general 2D box fit when font_px is already compact.
        const double box_w = static_cast<double>(clip_bounds.width() - 2 * edge_margin_px);
        const double box_h = static_cast<double>(clip_bounds.height() - 2 * edge_margin_px);
        if (font_px <= kMinAutoFitFontPx + 2 && 2.0 * half_ext <= std::min(box_w, box_h)) {
            cx = std::clamp(static_cast<double>(target_cx),
                            static_cast<double>(clip_bounds.x0 + edge_margin_px) + half_ext,
                            static_cast<double>(clip_bounds.x1 - edge_margin_px) - half_ext);
            cy = std::clamp(static_cast<double>(target_cy),
                            static_cast<double>(clip_bounds.y0 + edge_margin_px) + half_ext,
                            static_cast<double>(clip_bounds.y1 - edge_margin_px) - half_ext);
            break;
        }
        --font_px;
    }

    if (hi_sprite.width <= 0 || hi_sprite.height <= 0) {
        return;
    }

    const int bx0 = std::max(clip_bounds.x0, static_cast<int>(std::floor(cx - half_ext - 2.0)));
    const int by0 = std::max(clip_bounds.y0, static_cast<int>(std::floor(cy - half_ext - 2.0)));
    const int bx1 = std::min(clip_bounds.x1, static_cast<int>(std::ceil(cx + half_ext + 2.0)));
    const int by1 = std::min(clip_bounds.y1, static_cast<int>(std::ceil(cy + half_ext + 2.0)));

    const double hi_cx = 0.5 * static_cast<double>(hi_sprite.width - 1);
    const double hi_cy = 0.5 * static_cast<double>(hi_sprite.height - 1);
    const double scale_d = static_cast<double>(kRotatedSupersampleFactor);

    for (int y = std::max(0, by0); y < std::min(canvas.height, by1); ++y) {
        for (int x = std::max(0, bx0); x < std::min(canvas.width, bx1); ++x) {
            const double dx = static_cast<double>(x) - cx;
            const double dy = static_cast<double>(y) - cy;
            const double u_hi = kInvSqrt2 * (dx - dy) * scale_d + hi_cx;
            const double v_hi = kInvSqrt2 * (dx + dy) * scale_d + hi_cy;

            if (u_hi < 0.0 || v_hi < 0.0 ||
                u_hi >= static_cast<double>(hi_sprite.width - 1) ||
                v_hi >= static_cast<double>(hi_sprite.height - 1)) {
                continue;
            }

            const int x0 = static_cast<int>(std::floor(u_hi));
            const int y0 = static_cast<int>(std::floor(v_hi));
            const float fx = static_cast<float>(u_hi - static_cast<double>(x0));
            const float fy = static_cast<float>(v_hi - static_cast<double>(y0));

            auto sample_pm = [&](int sx, int sy, float w, float& pr, float& pg, float& pb, float& pa) {
                const std::uint8_t* p =
                    hi_sprite.pixels.data() +
                    (static_cast<std::size_t>(sy) * hi_sprite.width + static_cast<std::size_t>(sx)) *
                        kRgbaChannels;
                const float a = (static_cast<float>(p[kAIndex]) / kMaxChannelFloat) * w;
                pr += static_cast<float>(p[kRIndex]) * a;
                pg += static_cast<float>(p[kGIndex]) * a;
                pb += static_cast<float>(p[kBIndex]) * a;
                pa += a;
            };

            float pr = 0.0f;
            float pg = 0.0f;
            float pb = 0.0f;
            float pa = 0.0f;
            sample_pm(x0, y0, (1.0f - fx) * (1.0f - fy), pr, pg, pb, pa);
            sample_pm(x0 + 1, y0, fx * (1.0f - fy), pr, pg, pb, pa);
            sample_pm(x0, y0 + 1, (1.0f - fx) * fy, pr, pg, pb, pa);
            sample_pm(x0 + 1, y0 + 1, fx * fy, pr, pg, pb, pa);

            if (pa <= kUnpremultiplyAlphaEpsilon) {
                continue;
            }
            const std::uint8_t sr = static_cast<std::uint8_t>(
                std::clamp(std::lround(pr / pa), 0L, kMaxChannelLong));
            const std::uint8_t sg = static_cast<std::uint8_t>(
                std::clamp(std::lround(pg / pa), 0L, kMaxChannelLong));
            const std::uint8_t sb = static_cast<std::uint8_t>(
                std::clamp(std::lround(pb / pa), 0L, kMaxChannelLong));

            std::uint8_t* dp =
                canvas.pixels.data() +
                (static_cast<std::size_t>(y) * canvas.width + static_cast<std::size_t>(x)) *
                    kRgbaChannels;
            blend_pixel_porter_duff_over(dp, sr, sg, sb, pa);
        }
    }
}

constexpr std::size_t kFourButtonStates = 4;

std::array<TextEffectStyle, kFourButtonStates> standard_four_row_button_styles() noexcept {
    TextEffectStyle s0;
    s0.fill = {182, 242, 236, 235};
    s0.has_glow = true;
    s0.glow = {0, 72, 68, 140};
    s0.glow_radius_px = 1;

    TextEffectStyle s1;
    s1.fill = {214, 176, 126, 230};
    s1.has_glow = true;
    s1.glow = {96, 62, 18, 140};
    s1.glow_radius_px = 1;

    TextEffectStyle s2;
    s2.fill = {255, 248, 156, 255};
    s2.has_glow = true;
    s2.glow = {218, 154, 32, 210};
    s2.glow_radius_px = 2;

    TextEffectStyle s3;
    s3.fill = {210, 210, 210, 205};
    s3.has_glow = true;
    s3.glow = {48, 48, 48, 120};
    s3.glow_radius_px = 1;

    return {s0, s1, s2, s3};
}

constexpr std::array<std::uint32_t, kExpectedSys0PatchedCount> kSys0PatchedIndices = {
    2,   187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 202, 203,
    204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218,
    219, 221, 222, 223, 224, 226, 227, 228, 229, 246, 247, 248, 249, 251, 252,
    253, 254, 255, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266};

constexpr std::uint32_t kSys0CleanLoadBaseIndex = 234u;

void render_sys0_warning(RgbaImage& img) {
    overwrite_rect_rgba(img, {0, 0, img.width, img.height}, {0, 0, 0, 255});

    struct WarningLine {
        int y0;
        int y1;
        const char* text;
    };
    static constexpr std::array<WarningLine, 14> kLines = {{
        {70, 94, "This work may not be purchased by anyone under the age of 18."},
        {131, 154, "This work is a work of fiction."},
        {160, 184, "All character names, organizations, place names, and settings are fictitious,"},
        {190, 214, "and bear no relation whatsoever to any real persons or entities."},
        {250, 276, "All contents of this work are the copyrighted property of BLACK Cyc."},
        {280, 304, "Unauthorized reproduction, modification, recording, reprinting,"},
        {310, 334, "rental, pseudo-rental, distribution, or streaming of any part of this work is prohibited."},
        {370, 394, "This product is intended for sale within Japan only."},
        {401, 423, "JAPAN SALES ONLY"},
        {460, 484, "This work contains extreme depictions including violence, cruelty, and criminal acts."},
        {490, 514, "Viewer discretion is strongly advised, as some content may cause distress."},
        {520, 544, "Never imitate any criminal or dangerous acts depicted herein."},
        {580, 604, "Use of this software is strictly limited to individuals capable of handling"},
        {610, 634, "any resulting circumstances with sound judgment and personal responsibility."},
    }};

    TextEffectStyle style;
    style.fill = {205, 194, 0, 255};

    constexpr int kHorizontalMargin = 40;
    for (const auto& ln : kLines) {
        draw_styled_text_in_rect(img,
                                 ln.text,
                                 {kHorizontalMargin, ln.y0, img.width - kHorizontalMargin, ln.y1},
                                 L"Segoe UI",
                                 18,
                                 FW_SEMIBOLD,
                                 HAlign::kCenter,
                                 style);
    }
}

void render_sys0_radio_strip(RgbaImage& img, const char* label) {
    static constexpr std::array<RectI, kFourButtonStates> kEraseRel = {{
        {26, 5, 195, 23},
        {26, 4, 193, 22},
        {27, 6, 195, 24},
        {26, 5, 195, 23},
    }};
    static constexpr std::array<RgbaColor, kFourButtonStates> kCellBg = {{
        {2, 135, 127, 127},
        {158, 110, 42, 191},
        {158, 110, 42, 191},
        {69, 68, 68, 127},
    }};
    static constexpr std::array<RectI, kFourButtonStates> kTextRel = {{
        {30, 5, 193, 23},
        {30, 4, 191, 22},
        {30, 6, 193, 24},
        {30, 5, 193, 23},
    }};
    const auto styles = standard_four_row_button_styles();
    constexpr int kRowHeight = 30;

    for (std::size_t r = 0; r < kFourButtonStates; ++r) {
        const int y_off = static_cast<int>(r) * kRowHeight;
        const RectI er = {kEraseRel[r].x0, kEraseRel[r].y0 + y_off,
                          kEraseRel[r].x1, kEraseRel[r].y1 + y_off};
        const RectI tr = {kTextRel[r].x0, kTextRel[r].y0 + y_off,
                          kTextRel[r].x1, kTextRel[r].y1 + y_off};
        overwrite_rect_rgba(img, er, kCellBg[r]);
        draw_styled_text_in_rect(img, label, tr, L"Segoe UI", 15, FW_SEMIBOLD, HAlign::kCenter, styles[r]);
    }
}

struct Sys0HelperPlates {
    RgbaImage clean_load_base;
    RgbaImage kyou_donor_01;
    RgbaImage kyou_donor_02;
};

void render_sys0_voice_strip(RgbaImage& img,
                             const char* name,
                             int jp_x0,
                             int font_px,
                             const RgbaImage* optional_hair_donor = nullptr) {
    constexpr int kColWidth = 210;
    constexpr int kActiveCols = 3;
    constexpr int kDonorX0 = 146;
    constexpr int kDonorX1 = 178;
    constexpr int kGlyphBandY0 = 43;
    constexpr int kGlyphBandY1 = 65;
    constexpr int kGlyphBandX1 = 199;
    constexpr int kCellInnerX0 = 5;
    constexpr int kCellInnerX1 = 200;
    constexpr int kCellInnerY0 = 6;
    constexpr int kCellInnerY1 = 65;
    constexpr int kCell1TealBlueBias = 10;
    constexpr int kCell1TealMinScore = 3;
    constexpr std::size_t kMaxTinyPocketPixels = 8;
    constexpr int kOutlineRadiusPx = 2;
    constexpr int kTextRelX0 = 92;
    constexpr int kTextRelX1 = 198;
    constexpr int kTextRelY0 = 44;
    constexpr int kTextRelY1 = 65;

    static constexpr std::array<int, kActiveCols> kColShift = {0, -1, +1};
    static constexpr std::array<RgbaColor, kActiveCols> kFill = {{
        {225, 252, 248, 255},
        {245, 225, 190, 255},
        {255, 250, 175, 255},
    }};
    static constexpr std::array<RgbaColor, kActiveCols> kOutline = {{
        {2, 52, 48, 255},
        {72, 44, 10, 255},
        {92, 56, 8, 255},
    }};

    if (optional_hair_donor != nullptr &&
        optional_hair_donor->width == img.width &&
        optional_hair_donor->height == img.height) {
        for (int c = 0; c < kActiveCols; ++c) {
            const int x_off = c * kColWidth;
            const int sh = kColShift[static_cast<std::size_t>(c)];
            for (int y = kGlyphBandY0 + sh; y < kGlyphBandY1 + sh; ++y) {
                for (int rx = kDonorX0 + sh; rx < kDonorX1 + sh; ++rx) {
                    const std::size_t off =
                        (static_cast<std::size_t>(y) * img.width +
                         static_cast<std::size_t>(x_off + rx)) *
                        kRgbaChannels;
                    std::memcpy(img.pixels.data() + off,
                                optional_hair_donor->pixels.data() + off,
                                kRgbaChannels);
                }
            }
        }
        jp_x0 = kDonorX1;
    }

    const int h_box = kGlyphBandY1 - kGlyphBandY0;
    const int w_box = std::max(1, kGlyphBandX1 - jp_x0);
    std::vector<std::uint8_t> seed(static_cast<std::size_t>(h_box) * w_box, 0u);

    for (int ly = 0; ly < h_box; ++ly) {
        const int y1 = kGlyphBandY0 + ly - 1;
        if (y1 <= kCellInnerY0 + 2 || y1 >= kCellInnerY1 - 1) {
            continue;
        }
        for (int lx = 0; lx < w_box; ++lx) {
            const int x1 = kColWidth + jp_x0 + lx - 1;
            if (x1 >= kColWidth + kGlyphBandX1) {
                continue;
            }
            const std::uint8_t* px =
                img.pixels.data() +
                (static_cast<std::size_t>(y1) * img.width + static_cast<std::size_t>(x1)) *
                    kRgbaChannels;
            const int r1 = px[kRIndex];
            const int g1 = px[kGIndex];
            const int b1 = px[kBIndex];
            if (std::min(g1 - r1, g1 - b1 + kCell1TealBlueBias) >= kCell1TealMinScore) {
                seed[static_cast<std::size_t>(ly) * w_box + lx] = 1u;
            }
        }
    }

    std::vector<std::uint8_t> mask = seed;
    for (int ly = 0; ly < h_box; ++ly) {
        for (int lx = 0; lx < w_box; ++lx) {
            if (seed[static_cast<std::size_t>(ly) * w_box + lx] == 0) {
                continue;
            }
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int ny = ly + dy;
                    const int nx = lx + dx;
                    if (ny >= 0 && ny < h_box && nx >= 0 && nx < w_box &&
                        kGlyphBandY0 + ny <= kCellInnerY1 - 1 &&
                        jp_x0 + nx <= kGlyphBandX1 - 1) {
                        mask[static_cast<std::size_t>(ny) * w_box + nx] = 1u;
                    }
                }
            }
        }
    }

    std::vector<std::uint8_t> visited = mask;
    static constexpr std::array<std::pair<int, int>, 4> kSteps4 = {{
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
    }};
    for (int sy = 0; sy < h_box; ++sy) {
        for (int sx = 0; sx < w_box; ++sx) {
            const std::size_t start_i = static_cast<std::size_t>(sy) * w_box + sx;
            if (visited[start_i] != 0) {
                continue;
            }
            std::vector<std::size_t> comp;
            bool touches_border = false;
            std::deque<std::pair<int, int>> q;
            visited[start_i] = 1u;
            q.emplace_back(sx, sy);
            while (!q.empty()) {
                const auto [cx, cy] = q.front();
                q.pop_front();
                comp.push_back(static_cast<std::size_t>(cy) * w_box + cx);
                if (cx == 0 || cx == w_box - 1 || cy == 0 || cy == h_box - 1) {
                    touches_border = true;
                }
                for (const auto& [dx, dy] : kSteps4) {
                    const int nx = cx + dx;
                    const int ny = cy + dy;
                    if (nx < 0 || nx >= w_box || ny < 0 || ny >= h_box) {
                        continue;
                    }
                    const std::size_t ni = static_cast<std::size_t>(ny) * w_box + nx;
                    if (visited[ni] == 0) {
                        visited[ni] = 1u;
                        q.emplace_back(nx, ny);
                    }
                }
            }
            if (!touches_border && comp.size() <= kMaxTinyPocketPixels) {
                for (std::size_t idx : comp) {
                    mask[idx] = 1u;
                }
            }
        }
    }

    for (int c = 0; c < kActiveCols; ++c) {
        const int x_off = c * kColWidth;
        const int sh = kColShift[static_cast<std::size_t>(c)];
        const int cell_y0 = kCellInnerY0 + sh;
        const int cell_y1 = kCellInnerY1 + sh;
        const int cell_x0 = kCellInnerX0 + sh;
        const int cell_x1 = kCellInnerX1 + sh;

        std::vector<std::uint8_t> cell_mask(static_cast<std::size_t>(img.height) * kColWidth, 0u);
        for (int ly = 0; ly < h_box; ++ly) {
            for (int lx = 0; lx < w_box; ++lx) {
                if (mask[static_cast<std::size_t>(ly) * w_box + lx] == 0) {
                    continue;
                }
                const int cy = kGlyphBandY0 + ly + sh;
                const int cx = jp_x0 + lx + sh;
                if (cy >= cell_y0 && cy < cell_y1 && cx >= cell_x0 && cx < cell_x1) {
                    cell_mask[static_cast<std::size_t>(cy) * kColWidth + cx] = 1u;
                }
            }
        }

        for (int cx = std::max(cell_x0, jp_x0 - 1 + sh); cx < cell_x1; ++cx) {
            int cy = std::max(cell_y0, kGlyphBandY0 - 3 + sh);
            while (cy < cell_y1) {
                if (cell_mask[static_cast<std::size_t>(cy) * kColWidth + cx] == 0) {
                    ++cy;
                    continue;
                }
                const int run_y0 = cy;
                while (cy < cell_y1 &&
                       cell_mask[static_cast<std::size_t>(cy) * kColWidth + cx] != 0) {
                    ++cy;
                }
                const int run_y1 = cy - 1;
                const int top_y = (run_y0 - 1 >= cell_y0) ? (run_y0 - 1) : -1;
                const int bot_y = (run_y1 + 1 < cell_y1) ? (run_y1 + 1) : -1;

                auto px_at = [&](int py) -> std::uint8_t* {
                    return img.pixels.data() +
                           (static_cast<std::size_t>(py) * img.width +
                            static_cast<std::size_t>(x_off + cx)) *
                               kRgbaChannels;
                };

                for (int y_fill = run_y0; y_fill <= run_y1; ++y_fill) {
                    std::uint8_t* dst = px_at(y_fill);
                    if (top_y >= 0 && bot_y >= 0) {
                        const float span = static_cast<float>(bot_y - top_y);
                        const float t = static_cast<float>(y_fill - top_y) / span;
                        const std::uint8_t* p0 = px_at(top_y);
                        const std::uint8_t* p1 = px_at(bot_y);
                        const float a0 = static_cast<float>(p0[kAIndex]) / kMaxChannelFloat;
                        const float a1 = static_cast<float>(p1[kAIndex]) / kMaxChannelFloat;
                        const float a_mix = (1.0f - t) * a0 + t * a1;
                        if (a_mix > kUnpremultiplyAlphaEpsilon) {
                            const float w0 = (1.0f - t) * a0;
                            const float w1 = t * a1;
                            dst[kRIndex] = static_cast<std::uint8_t>(std::clamp(
                                std::lround((w0 * p0[kRIndex] + w1 * p1[kRIndex]) / a_mix),
                                0L,
                                kMaxChannelLong));
                            dst[kGIndex] = static_cast<std::uint8_t>(std::clamp(
                                std::lround((w0 * p0[kGIndex] + w1 * p1[kGIndex]) / a_mix),
                                0L,
                                kMaxChannelLong));
                            dst[kBIndex] = static_cast<std::uint8_t>(std::clamp(
                                std::lround((w0 * p0[kBIndex] + w1 * p1[kBIndex]) / a_mix),
                                0L,
                                kMaxChannelLong));
                            dst[kAIndex] = static_cast<std::uint8_t>(std::clamp(
                                std::lround(a_mix * kMaxChannelFloat), 0L, kMaxChannelLong));
                            continue;
                        }
                        std::memset(dst, 0, kRgbaChannels);
                        continue;
                    }
                    if (top_y >= 0) {
                        std::memcpy(dst, px_at(top_y), kRgbaChannels);
                        continue;
                    }
                    if (bot_y >= 0) {
                        std::memcpy(dst, px_at(bot_y), kRgbaChannels);
                    }
                }
            }
        }

        TextEffectStyle st;
        st.fill = kFill[static_cast<std::size_t>(c)];
        st.has_outline = true;
        st.outline = kOutline[static_cast<std::size_t>(c)];
        st.outline_radius_px = kOutlineRadiusPx;

        const RectI text_box = {
            x_off + kTextRelX0 + sh,
            kTextRelY0 + sh,
            x_off + kTextRelX1 + sh,
            kTextRelY1 + sh,
        };
        draw_styled_text_in_rect(
            img, name, text_box, L"Segoe UI", font_px, FW_BOLD, HAlign::kRight, st);
    }
}

void render_sys0_dialog_yes_no(RgbaImage& img, const char* label) {
    static constexpr std::array<RectI, kFourButtonStates> kRectRel = {{
        {14, 9, 146, 33},
        {14, 8, 144, 32},
        {14, 9, 146, 34},
        {14, 9, 146, 33},
    }};
    static constexpr std::array<RgbaColor, kFourButtonStates> kCellBg = {{
        {2, 135, 128, 127},
        {158, 110, 42, 191},
        {158, 110, 42, 191},
        {2, 135, 128, 127},
    }};
    auto styles = standard_four_row_button_styles();
    styles[3] = styles[0];  // Row 3 duplicates normal row on dialog Yes/No strips.

    constexpr int kRowHeight = 40;
    for (std::size_t r = 0; r < kFourButtonStates; ++r) {
        const int y_off = static_cast<int>(r) * kRowHeight;
        const RectI box = {kRectRel[r].x0, kRectRel[r].y0 + y_off,
                           kRectRel[r].x1, kRectRel[r].y1 + y_off};
        overwrite_rect_rgba(img, box, kCellBg[r]);
        draw_styled_text_in_rect(img, label, box, L"Segoe UI", 17, FW_SEMIBOLD, HAlign::kCenter, styles[r]);
    }
}

void render_sys0_dialog_prompt(RgbaImage& img, const char* line1, const char* line2) {
    constexpr int kSampleRowY = 145;
    tile_rect_from_patch(img, {34, 56, 364, 134}, {34, kSampleRowY, 364, kSampleRowY + 1});

    TextEffectStyle st;
    st.fill = {155, 232, 226, 255};
    st.has_glow = true;
    st.glow = {0, 48, 44, 150};
    st.glow_radius_px = 1;

    draw_styled_text_in_rect(img, line1, {34, 60, 364, 88}, L"Segoe UI", 18, FW_SEMIBOLD, HAlign::kCenter, st);
    draw_styled_text_in_rect(img, line2, {34, 100, 364, 128}, L"Segoe UI", 18, FW_SEMIBOLD, HAlign::kCenter, st);
}

void render_sys0_hex_button_strip(RgbaImage& img, const char* label) {
    static constexpr std::array<RectI, kFourButtonStates> kRectRel = {{
        {34, 6, 166, 34},
        {32, 5, 164, 31},
        {34, 7, 166, 33},
        {34, 6, 166, 34},
    }};
    static constexpr std::array<RgbaColor, kFourButtonStates> kCellBg = {{
        {2, 167, 159, 103},
        {168, 117, 45, 179},
        {168, 117, 45, 179},
        {84, 84, 84, 103},
    }};
    const auto styles = standard_four_row_button_styles();
    constexpr int kRowHeight = 40;

    for (std::size_t r = 0; r < kFourButtonStates; ++r) {
        const int y_off = static_cast<int>(r) * kRowHeight;
        const RectI box = {kRectRel[r].x0, kRectRel[r].y0 + y_off,
                           kRectRel[r].x1, kRectRel[r].y1 + y_off};
        overwrite_rect_rgba(img, box, kCellBg[r]);
        draw_styled_text_in_rect(img, label, box, L"Segoe UI", 16, FW_SEMIBOLD, HAlign::kCenter, styles[r]);
    }
}

void render_sys0_quick_notice(RgbaImage& img) {
    constexpr RgbaColor kBg{0, 166, 156, 243};
    TextEffectStyle st;
    st.fill = {242, 255, 254, 255};
    st.has_glow = true;
    st.glow = {0, 68, 64, 140};
    st.glow_radius_px = 1;

    overwrite_rect_rgba(img, {5, 7, 133, 29}, kBg);
    draw_styled_text_in_rect(img, "Quick Loaded", {6, 7, 132, 29}, L"Segoe UI", 15, FW_SEMIBOLD, HAlign::kCenter, st);

    overwrite_rect_rgba(img, {5, 47, 133, 69}, kBg);
    draw_styled_text_in_rect(img, "Quick Saved", {6, 47, 132, 69}, L"Segoe UI", 15, FW_SEMIBOLD, HAlign::kCenter, st);
}

void render_sys0_hud_slide(RgbaImage& img, const char* label) {
    constexpr int kRowHeight = 60;
    constexpr int kFirstBarRow = 1;
    constexpr int kFullRevealRow = 7;
    constexpr int kActiveHighlightRow = 9;
    constexpr int kBarFillX0 = 4;
    constexpr int kBarFillX1 = 121;
    constexpr int kBarFillRelY0 = 20;
    constexpr int kBarFillRelY1 = 42;
    constexpr int kTextX0 = 6;
    constexpr int kTextX1 = 120;
    constexpr int kTextRelY0 = 19;
    constexpr int kTextRelY1 = 43;
    constexpr int kInitialHudFontPx = 14;
    constexpr std::size_t kInitialRevealChars = 2;
    constexpr RgbaColor kBarBg{0, 166, 156, 243};

    const std::string_view full_sv(label);
    if (full_sv.empty()) {
        return;
    }

    auto make_row_style = [](bool active) noexcept {
        TextEffectStyle st;
        st.fill = active ? RgbaColor{255, 250, 220, 255} : RgbaColor{235, 255, 252, 255};
        st.has_glow = true;
        st.glow = active ? RgbaColor{140, 96, 16, 160} : RgbaColor{0, 68, 64, 140};
        st.glow_radius_px = 1;
        return st;
    };

    const TextEffectStyle measure_st = make_row_style(false);
    const int pad_1x = compute_sprite_padding_1x(measure_st);
    const int max_glyph_w = kTextX1 - kTextX0;
    int hud_font_px = kInitialHudFontPx;
    RgbaImage full_sprite = rasterize_styled_line_sprite(
        full_sv, L"Segoe UI", hud_font_px, FW_BOLD, measure_st, 0, kSupersampleFactor, true);
    while (hud_font_px > kMinAutoFitFontPx &&
           full_sprite.width - pad_1x * 2 > max_glyph_w) {
        --hud_font_px;
        full_sprite = rasterize_styled_line_sprite(
            full_sv, L"Segoe UI", hud_font_px, FW_BOLD, measure_st, 0, kSupersampleFactor, true);
    }
    if (full_sprite.width <= 0 || full_sprite.height <= 0) {
        return;
    }

    const int dst_x0 = (kTextX0 + kTextX1) / 2 - full_sprite.width / 2;
    const int dst_y_rel = (kTextRelY0 + kTextRelY1) / 2 - full_sprite.height / 2;

    std::vector<std::size_t> prefix_lens;
    const std::size_t first_len = std::min(kInitialRevealChars, full_sv.size());
    for (std::size_t len = first_len; len <= full_sv.size(); ++len) {
        if (full_sv[len - 1] != ' ') {
            prefix_lens.push_back(len);
        }
    }
    if (prefix_lens.empty()) {
        prefix_lens.push_back(full_sv.size());
    }

    for (int row = kFirstBarRow; row <= kActiveHighlightRow; ++row) {
        const int y_off = row * kRowHeight;
        overwrite_rect_rgba(
            img, {kBarFillX0, y_off + kBarFillRelY0, kBarFillX1, y_off + kBarFillRelY1}, kBarBg);

        std::size_t chosen_len = full_sv.size();
        if (row < kFullRevealRow && prefix_lens.size() >= 2) {
            const int partial_steps = static_cast<int>(prefix_lens.size()) - 2;
            const int step_idx = ((row - kFirstBarRow) * partial_steps + 2) / 5;
            const int clamped_idx =
                std::clamp(step_idx, 0, static_cast<int>(prefix_lens.size()) - 2);
            chosen_len = prefix_lens[static_cast<std::size_t>(clamped_idx)];
        }

        const TextEffectStyle st = make_row_style(row == kActiveHighlightRow);
        const RgbaImage row_sprite = rasterize_styled_line_sprite(full_sv.substr(0, chosen_len),
                                                                  L"Segoe UI",
                                                                  hud_font_px,
                                                                  FW_BOLD,
                                                                  st,
                                                                  0,
                                                                  kSupersampleFactor,
                                                                  true);
        const int dst_y0 = y_off + dst_y_rel;
        const int clip_y0 = std::max(0, y_off + kTextRelY0);
        const int clip_y1 = std::min(img.height, y_off + kTextRelY1);
        const int clip_x0 = kBarFillX0;
        const int clip_x1 = std::min(img.width, kBarFillX1);

        for (int sy = 0; sy < row_sprite.height; ++sy) {
            const int dy = dst_y0 + sy;
            if (dy < clip_y0 || dy >= clip_y1) {
                continue;
            }
            for (int sx = 0; sx < row_sprite.width; ++sx) {
                const int dx = dst_x0 + sx;
                if (dx < clip_x0 || dx >= clip_x1) {
                    continue;
                }
                const std::uint8_t* sp =
                    row_sprite.pixels.data() +
                    (static_cast<std::size_t>(sy) * row_sprite.width +
                     static_cast<std::size_t>(sx)) *
                        kRgbaChannels;
                if (sp[kAIndex] == 0) {
                    continue;
                }
                std::uint8_t* dp =
                    img.pixels.data() +
                    (static_cast<std::size_t>(dy) * img.width + static_cast<std::size_t>(dx)) *
                        kRgbaChannels;
                blend_pixel_porter_duff_over(dp,
                                             sp[kRIndex],
                                             sp[kGIndex],
                                             sp[kBIndex],
                                             static_cast<float>(sp[kAIndex]) / kMaxChannelFloat);
            }
        }
    }
}

void render_sys0_quick_slot_base(RgbaImage& img, const RgbaImage& clean_load_base) {
    img = clean_load_base;

    TextEffectStyle st;
    st.fill = {210, 250, 246, 255};
    st.has_drop_shadow = true;
    st.shadow = {0, 58, 54, 200};
    st.shadow_dx = 1;
    st.shadow_dy = 1;

    draw_styled_text_in_rect(
        img, "▽ Quick Save Slot", {14, 1, 240, 18}, L"Segoe UI", 12, FW_SEMIBOLD, HAlign::kLeft, st);
}

void render_sys0_menu_button(RgbaImage& img,
                             const char* label,
                             int erase_x1_r023,
                             int text_x1_r023,
                             std::uint8_t r3_bg_red) {
    const std::array<RectI, kFourButtonStates> erase_rel = {{
        {10, 8, erase_x1_r023, 33},
        {8, 7, erase_x1_r023 - 2, 32},
        {10, 7, erase_x1_r023, 33},
        {10, 8, erase_x1_r023, 33},
    }};
    const std::array<RectI, kFourButtonStates> text_rel = {{
        {18, 8, text_x1_r023, 33},
        {16, 7, text_x1_r023 - 2, 32},
        {18, 7, text_x1_r023, 33},
        {18, 8, text_x1_r023, 33},
    }};
    const std::array<RgbaColor, kFourButtonStates> cell_bg = {{
        {2, 135, 128, 127},
        {158, 110, 42, 191},
        {158, 110, 42, 191},
        {r3_bg_red, 68, 68, 127},
    }};
    const auto styles = standard_four_row_button_styles();
    constexpr int kRowHeight = 40;

    for (std::size_t r = 0; r < kFourButtonStates; ++r) {
        const int y_off = static_cast<int>(r) * kRowHeight;
        const RectI er = {erase_rel[r].x0, erase_rel[r].y0 + y_off,
                          erase_rel[r].x1, erase_rel[r].y1 + y_off};
        const RectI tr = {text_rel[r].x0, text_rel[r].y0 + y_off,
                          text_rel[r].x1, text_rel[r].y1 + y_off};
        overwrite_rect_rgba(img, er, cell_bg[r]);
        draw_styled_text_in_rect(img, label, tr, L"Segoe UI", 16, FW_SEMIBOLD, HAlign::kLeft, styles[r]);
    }
}

bool render_sys0_entry(std::uint32_t entry_index,
                       RgbaImage& img,
                       const Sys0HelperPlates& helpers) {
    switch (entry_index) {
        case 2:
            render_sys0_warning(img);
            return false;
        case 187:
        case 188:
            render_sys0_radio_strip(img, "Windowed");
            return false;
        case 189:
        case 190:
            render_sys0_radio_strip(img, "Fullscreen");
            return false;
        case 191:
        case 192:
            render_sys0_radio_strip(img, "Read Only");
            return false;
        case 193:
        case 194:
            render_sys0_radio_strip(img, "All Text");
            return false;
        case 195:
        case 196:
            render_sys0_radio_strip(img, "Manual");
            return false;
        case 197:
        case 198:
            render_sys0_radio_strip(img, "Auto");
            return false;
        case 202:
        case 203:
            render_sys0_voice_strip(img, "Miyaka", 147, 14);
            return true;
        case 204:
        case 205:
            render_sys0_voice_strip(img, "Ren", 164, 14);
            return true;
        case 206:
        case 207:
            render_sys0_voice_strip(img, "Ayaka", 162, 14);
            return true;
        case 208:
        case 209:
            render_sys0_voice_strip(img, "Yuria", 134, 14);
            return true;
        case 210:
        case 211:
            render_sys0_voice_strip(img, "Shio", 164, 14);
            return true;
        case 212:
        case 213:
            render_sys0_voice_strip(img, "Yumemi", 163, 14);
            return true;
        case 214:
        case 215:
            render_sys0_voice_strip(img, "Kazenomiya", 147, 12);
            return true;
        case 216:
            render_sys0_voice_strip(img, "Kyou", 148, 14, &helpers.kyou_donor_01);
            return true;
        case 217:
            render_sys0_voice_strip(img, "Kyou", 178, 14);
            return true;
        case 218:
            render_sys0_voice_strip(img, "Kyou", 148, 14, &helpers.kyou_donor_02);
            return true;
        case 219:
            render_sys0_voice_strip(img, "Kyou", 178, 14);
            return true;
        case 221:
            render_sys0_dialog_yes_no(img, "No");
            return false;
        case 222:
            render_sys0_dialog_prompt(img, "Return to the title screen?", "Are you sure?");
            return false;
        case 223:
            render_sys0_dialog_prompt(img, "Overwrite this save data?", "Are you sure?");
            return false;
        case 224:
            render_sys0_dialog_yes_no(img, "Yes");
            return false;
        case 226:
            render_sys0_hex_button_strip(img, "CG Gallery");
            return true;
        case 227:
            render_sys0_hex_button_strip(img, "Scene Replay");
            return true;
        case 228:
            render_sys0_hex_button_strip(img, "Music");
            return true;
        case 229:
            render_sys0_hex_button_strip(img, "Back");
            return true;
        case 246:
            render_sys0_quick_notice(img);
            return true;
        case 247:
            render_sys0_hud_slide(img, "Quick Load");
            return true;
        case 248:
            render_sys0_hud_slide(img, "Quick Save");
            return true;
        case 249:
            render_sys0_quick_slot_base(img, helpers.clean_load_base);
            return true;
        case 251:
            render_sys0_hud_slide(img, "System Menu");
            return true;
        case 252:
            render_sys0_menu_button(img, "Save", 228, 224, 68);
            return false;
        case 253:
            render_sys0_menu_button(img, "Load", 228, 224, 68);
            return false;
        case 254:
            render_sys0_menu_button(img, "Settings", 228, 224, 69);
            return false;
        case 255:
            render_sys0_menu_button(img, "Hide Text", 202, 198, 69);
            return false;
        case 256:
            render_sys0_menu_button(img, "Skip", 203, 199, 69);
            return false;
        case 257:
            render_sys0_menu_button(img, "Backlog", 227, 223, 69);
            return false;
        case 258:
            render_sys0_menu_button(img, "Auto", 228, 224, 69);
            return false;
        case 259:
            render_sys0_menu_button(img, "Close", 194, 190, 69);
            return false;
        case 260:
            render_sys0_menu_button(img, "Title / Quit", 256, 252, 69);
            return false;
        case 261:
            render_sys0_menu_button(img, "Exit Scene", 256, 252, 69);
            return true;
        case 262:
            render_sys0_hex_button_strip(img, "New Game");
            return true;
        case 263:
            render_sys0_hex_button_strip(img, "Load Game");
            return true;
        case 264:
            render_sys0_hex_button_strip(img, "Settings");
            return true;
        case 265:
            render_sys0_hex_button_strip(img, "Gallery");
            return true;
        case 266:
            render_sys0_hex_button_strip(img, "Exit");
            return true;
        default:
            throw std::runtime_error("Unexpected sys0 entry index in menu renderer");
    }
}

constexpr std::array<std::uint32_t, kExpectedTa0PatchedCount> kTa0PatchedIndices = {
    401, 402, 403, 404, 409, 411, 412, 413, 414, 415, 416, 417};

constexpr std::uint32_t kTa0CleanTitlePlateIndex = 410u;

struct BarrageItem {
    int cx;
    int cy;
    int font_px;
    const char* text;
};

void render_ta0_barrage_overlay(RgbaImage& img,
                                const BarrageItem* items,
                                std::size_t count,
                                RgbaColor fill_color) {
    constexpr int kBarrageOutlineRadiusPx = 4;
    constexpr int kBarrageEdgeMarginPx = 4;
    std::fill(img.pixels.begin(), img.pixels.end(), 0u);

    TextEffectStyle st;
    st.fill = fill_color;
    st.has_outline = true;
    st.outline = {0, 0, 0, kFullAlpha};
    st.outline_radius_px = kBarrageOutlineRadiusPx;

    const RectI canvas_bounds = {0, 0, img.width, img.height};
    for (std::size_t i = 0; i < count; ++i) {
        draw_rotated_minus45_callout(img,
                                     items[i].text,
                                     items[i].cx,
                                     items[i].cy,
                                     items[i].font_px,
                                     FW_EXTRABOLD,
                                     st,
                                     canvas_bounds,
                                     kBarrageEdgeMarginPx);
    }
}

void render_ta0_legacy_config_plate(RgbaImage& img) {
    tile_rect_from_patch(img, {42, 26, 232, 65}, {24, 95, 440, 140});
    tile_rect_from_patch(img, {42, 154, 430, 204}, {432, 154, 580, 204});

    TextEffectStyle title_st;
    title_st.fill = {255, 255, 255, 245};
    title_st.has_drop_shadow = true;
    title_st.shadow = {0, 48, 72, 170};
    title_st.shadow_dx = 1;
    title_st.shadow_dy = 1;

    draw_styled_text_in_rect(
        img, "—Settings—", {45, 28, 260, 62}, L"Segoe UI", 26, FW_BOLD, HAlign::kLeft, title_st);
    draw_styled_text_in_rect(img,
                             "◇ Message Window Opacity",
                             {45, 154, 560, 202},
                             L"Segoe UI",
                             22,
                             FW_SEMIBOLD,
                             HAlign::kLeft,
                             title_st);
}

void render_ta0_mkri_header_plate(RgbaImage& img,
                                  const RgbaImage& clean_title_plate,
                                  int header_erase_x1,
                                  const char* header_text,
                                  bool with_config_p1_sections) {
    copy_rect_from_clean_plate(img, clean_title_plate, {76, 33, header_erase_x1, 57});

    TextEffectStyle st;
    st.fill = {150, 195, 192, 255};
    st.has_glow = true;
    st.glow = {4, 34, 32, 185};
    st.glow_radius_px = 1;

    draw_styled_text_in_rect(
        img, header_text, {80, 33, 360, 57}, L"Segoe UI", 20, FW_SEMIBOLD, HAlign::kLeft, st);

    if (!with_config_p1_sections) {
        return;
    }

    struct SectionLabel {
        RectI erase_rect;
        RectI text_rect;
        const char* label;
    };
    static constexpr std::array<SectionLabel, 10> kSections = {{
        {{172, 137, 256, 161}, {176, 137, 560, 161}, "BGM Volume"},
        {{172, 218, 263, 241}, {176, 218, 560, 241}, "SE Volume"},
        {{172, 298, 273, 321}, {176, 298, 560, 321}, "Voice Volume"},
        {{172, 378, 381, 401}, {176, 378, 560, 401}, "Voice SE Volume"},
        {{172, 457, 465, 481}, {176, 457, 560, 481}, "Master Volume (BGM / SE / Voice)"},
        {{732, 137, 885, 161}, {736, 137, 1120, 161}, "Display Mode"},
        {{732, 218, 901, 241}, {736, 218, 1120, 241}, "Text Speed"},
        {{732, 297, 904, 321}, {736, 297, 1120, 321}, "Skip Mode"},
        {{732, 378, 904, 401}, {736, 378, 1120, 401}, "Auto-Read Mode"},
        {{732, 458, 975, 482}, {736, 458, 1120, 482}, "Auto-Read Speed"},
    }};

    for (const auto& sec : kSections) {
        copy_rect_from_clean_plate(img, clean_title_plate, sec.erase_rect);
        draw_styled_text_in_rect(
            img, sec.label, sec.text_rect, L"Segoe UI", 18, FW_SEMIBOLD, HAlign::kLeft, st);
    }
}

bool render_ta0_entry(std::uint32_t entry_index,
                      RgbaImage& img,
                      const RgbaImage& clean_title_plate) {
    switch (entry_index) {
        case 401: {
            static constexpr std::array<BarrageItem, 1> kItems = {{
                {114, 110, 22, "Go Back to Big-Boob Land!"},
            }};
            render_ta0_barrage_overlay(img, kItems.data(), kItems.size(), {255, 255, 0, 255});
            return false;
        }
        case 402: {
            static constexpr std::array<BarrageItem, 7> kItems = {{
                {1120, 141, 24, "Gotta respect owning it that hard, though..."},
                {1436, 133, 24, "Go home, hack whose only gimmick is tits!"},
                {1028, 358, 28, "<<++ C H A N G E ++>>"},
                {409, 597, 24, "HAAAAAAAAAAAG-----"},
                {92, 987, 22, "No way with those tits lol"},
                {532, 880, 24, "Yep, next please"},
                {897, 912, 24, "Even as a MILF lover, I declare this a total miscast"},
            }};
            render_ta0_barrage_overlay(img, kItems.data(), kItems.size(), {255, 255, 255, 255});
            return false;
        }
        case 403: {
            static constexpr std::array<BarrageItem, 9> kItems = {{
                {765, 121, 23, "Another piece of trash born in JP cinema"},
                {780, 265, 21, "Did they cast her by hair color alone?"},
                {412, 590, 21, "Both her image and my sides just exploded"},
                {678, 498, 23, "Overflowing 'this ain't it' vibes"},
                {796, 643, 20, "IT'S HERE----(°∀°)----"},
                {74, 1034, 20, "Who's this for?"},
                {234, 990, 24, "Her body isn't this sloppy!"},
                {505, 949, 24, "Bring out whoever's in charge!"},
                {250, 1374, 24, "Way off the mark!"},
            }};
            render_ta0_barrage_overlay(img, kItems.data(), kItems.size(), {255, 255, 255, 255});
            return false;
        }
        case 404: {
            static constexpr std::array<BarrageItem, 6> kItems = {{
                {832, 67, 24, "Just stop already"},
                {243, 588, 24, "Don't push yourself, hag"},
                {528, 516, 23, "What was going through her head putting on a miniskirt?"},
                {703, 438, 21, "How many casting couches did this take?"},
                {728, 482, 21, "Whoever cast this clearly hates the original work"},
                {96, 1170, 22, "Way too much of a stretch"},
            }};
            render_ta0_barrage_overlay(img, kItems.data(), kItems.size(), {255, 255, 255, 255});
            return false;
        }
        case 409:
            render_ta0_legacy_config_plate(img);
            return true;
        case 411:
            render_ta0_mkri_header_plate(img, clean_title_plate, 160, "Settings", true);
            return false;
        case 412:
            render_ta0_mkri_header_plate(img, clean_title_plate, 160, "Settings", false);
            return false;
        case 413:
            render_ta0_mkri_header_plate(img, clean_title_plate, 160, "Music Gallery", false);
            return false;
        case 414:
            render_ta0_mkri_header_plate(img, clean_title_plate, 160, "CG Gallery", false);
            return false;
        case 415:
            render_ta0_mkri_header_plate(img, clean_title_plate, 180, "Scene Gallery", false);
            return false;
        case 416:
            render_ta0_mkri_header_plate(img, clean_title_plate, 195, "Load Game", false);
            return false;
        case 417:
            render_ta0_mkri_header_plate(img, clean_title_plate, 195, "Save Game", false);
            return false;
        default:
            throw std::runtime_error("Unexpected ta0 entry index in menu renderer");
    }
}

constexpr std::array<std::uint32_t, kExpectedEv0PatchedCount> kEv0PatchedIndices = {
    70, 71, 76};

void erase_dark_handwriting_in_subrects(RgbaImage& img,
                                        const RectI* subrects,
                                        std::size_t subrect_count,
                                        int dilate_radius) {
    constexpr int kHandwritingLumaThreshold = 198;
    constexpr int kHandwritingInpaintSampleRadius = 5;
    constexpr int kHandwritingParchmentMinLuma = 200;

    auto inside_any_subrect = [&](int x, int y) {
        for (std::size_t i = 0; i < subrect_count; ++i) {
            if (x >= subrects[i].x0 && x < subrects[i].x1 &&
                y >= subrects[i].y0 && y < subrects[i].y1) {
                return true;
            }
        }
        return false;
    };

    std::vector<std::uint8_t> seed(static_cast<std::size_t>(img.width) * img.height, 0u);
    RectI union_bounds{img.width, img.height, 0, 0};

    for (std::size_t i = 0; i < subrect_count; ++i) {
        const RectI r = subrects[i];
        union_bounds.x0 = std::min(union_bounds.x0, r.x0);
        union_bounds.y0 = std::min(union_bounds.y0, r.y0);
        union_bounds.x1 = std::max(union_bounds.x1, r.x1);
        union_bounds.y1 = std::max(union_bounds.y1, r.y1);

        for (int y = std::max(0, r.y0); y < std::min(img.height, r.y1); ++y) {
            for (int x = std::max(0, r.x0); x < std::min(img.width, r.x1); ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * img.width + x;
                const std::uint8_t* px = img.pixels.data() + idx * kRgbaChannels;
                if (compute_bt601_luma(px[kRIndex], px[kGIndex], px[kBIndex]) <
                    kHandwritingLumaThreshold) {
                    seed[idx] = 1u;
                }
            }
        }
    }

    std::vector<std::uint8_t> dilated = seed;
    const int r2 = dilate_radius * dilate_radius;
    for (int y = std::max(0, union_bounds.y0); y < std::min(img.height, union_bounds.y1); ++y) {
        for (int x = std::max(0, union_bounds.x0); x < std::min(img.width, union_bounds.x1); ++x) {
            if (seed[static_cast<std::size_t>(y) * img.width + x] == 0) {
                continue;
            }
            for (int dy = -dilate_radius; dy <= dilate_radius; ++dy) {
                for (int dx = -dilate_radius; dx <= dilate_radius; ++dx) {
                    if (dx * dx + dy * dy > r2) {
                        continue;
                    }
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx >= 0 && nx < img.width && ny >= 0 && ny < img.height &&
                        inside_any_subrect(nx, ny)) {
                        dilated[static_cast<std::size_t>(ny) * img.width + nx] = 1u;
                    }
                }
            }
        }
    }

    inpaint_masked_region_onion_peel(img,
                                     dilated,
                                     union_bounds,
                                     kHandwritingInpaintSampleRadius,
                                     kHandwritingParchmentMinLuma);
}

void render_ev0_denky_comment(RgbaImage& img) {
    constexpr int kHandwritingDilateRadius = 3;
    static constexpr std::array<RectI, 2> kSubrects = {{
        {165, 20, 1155, 526},
        {165, 526, 615, 552},
    }};
    erase_dark_handwriting_in_subrects(
        img, kSubrects.data(), kSubrects.size(), kHandwritingDilateRadius);

    struct CommentLine {
        int y_top;
        const char* text;
    };
    static constexpr std::array<CommentLine, 6> kLines = {{
        {52, "Teaming up with Black Cyc and Ueda Metawo-san again"},
        {132, "after so long got me super hyped up!!!"},
        {212, "I put so much spirit into it that the track"},
        {292, "ended up running long, but we poured everything"},
        {372, "from Denkare × Black Cyc's history into this song,"},
        {452, "so please listen to every last second of it!!"},
    }};

    TextEffectStyle st;
    st.fill = {30, 28, 26, 255};

    for (const auto& ln : kLines) {
        draw_styled_text_in_rect(img,
                                 ln.text,
                                 {185, ln.y_top, 1135, ln.y_top + 48},
                                 L"Segoe Print",
                                 24,
                                 FW_BOLD,
                                 HAlign::kLeft,
                                 st);
    }
}

void render_ev0_karen_comment(RgbaImage& img) {
    constexpr int kHandwritingDilateRadius = 3;
    static constexpr std::array<RectI, 4> kSubrects = {{
        {150, 20, 1165, 122},
        {150, 122, 692, 304},
        {150, 304, 672, 512},
        {150, 512, 720, 675},
    }};
    erase_dark_handwriting_in_subrects(
        img, kSubrects.data(), kSubrects.size(), kHandwritingDilateRadius);

    struct SteppedLine {
        int y_top;
        int max_w;
        const char* text;
    };
    static constexpr std::array<SteppedLine, 7> kLines = {{
        {52, 960, "I'm so happy to work alongside Metawo-san again."},
        {148, 485, "I read deep into the scenario"},
        {228, 485, "before writing the lyrics."},
        {315, 475, "During recording, I got so"},
        {400, 475, "swept up in emotion that I"},
        {488, 510, "nearly cried. I'm really proud of it!"},
        {576, 510, "Please be sure to give it a listen!!"},
    }};

    TextEffectStyle st;
    st.fill = {30, 28, 26, 255};

    for (const auto& ln : kLines) {
        draw_styled_text_in_rect(img,
                                 ln.text,
                                 {170, ln.y_top, 170 + ln.max_w, ln.y_top + 44},
                                 L"Segoe Print",
                                 22,
                                 FW_BOLD,
                                 HAlign::kLeft,
                                 st);
    }
}

void render_ev0_metawo_callout(RgbaImage& img) {
    constexpr int kWorkX0 = 800;
    constexpr int kWorkY0 = 270;
    constexpr int kWorkW = 320;
    constexpr int kWorkH = 320;
    constexpr RectI kCalloutBox = {845, 311, 1073, 544};
    constexpr int kLocalCalloutX0 = kCalloutBox.x0 - kWorkX0;
    constexpr int kLocalCalloutY0 = kCalloutBox.y0 - kWorkY0;
    constexpr int kLocalCalloutX1 = kCalloutBox.x1 - kWorkX0;
    constexpr int kLocalCalloutY1 = kCalloutBox.y1 - kWorkY0;

    constexpr int kYellowChromaMin = 38;
    constexpr int kYellowRedMin = 90;
    constexpr int kDistSearchRadius = 15;
    constexpr int kDistInfSq = 999999;

    constexpr int kCleanPaperMargin = 20;
    constexpr int kCleanPaperMinDistSq = 96;
    constexpr int kCleanPaperMinLuma = 185;
    constexpr int kMedianFilterMinLuma = 182;
    constexpr int kMedianWindowRadius = 4;
    constexpr std::size_t kMinNonDarkWindowCount = 25;
    constexpr double kMaxMedianDeviation = 11.0;

    constexpr int kPaperFillX0 = 36;
    constexpr int kPaperFillX1 = 284;
    constexpr int kPaperFillY0 = 32;
    constexpr int kPaperFillY1 = 284;
    constexpr int kPaperOnionRadius = 9;
    constexpr int kPaperBlurPasses = 3;
    constexpr int kPaperBlurMaxDistSq = 105;
    constexpr int kPaperBlurRadius = 2;

    constexpr int kBilateralMinDistSq = 24;
    constexpr double kBilateralMinInkDrop = 20.0;
    constexpr double kBilateralFlankMaxDrop = 13.0;
    constexpr int kStrongInkMinDistSq = 36;
    constexpr int kStrongInkMaxLuma = 130;
    constexpr int kAnchorMinDistSq = 93;
    constexpr int kAnchorMaxDistSq = 250;
    constexpr int kSoftHaloMinDistSq = 30;
    constexpr double kSoftHaloMinInkDrop = 12.0;

    constexpr int kEraseX0 = 42;
    constexpr int kEraseX1 = 276;
    constexpr int kEraseY0 = 38;
    constexpr int kEraseY1 = 276;
    constexpr int kCoreEraseDistSq = 45;
    constexpr int kOuterArcEraseDistSq = 95;
    constexpr double kOuterArcMinInkDrop = 4.0;

    constexpr int kGrainCellSize = 2;
    constexpr std::uint32_t kGrainHashMulX = 0x045D9F3Bu;
    constexpr std::uint32_t kGrainHashMulY = 0x27D4EB2Du;
    constexpr std::uint32_t kGrainHashShift = 16u;
    constexpr std::uint32_t kGrainSpan = 5u;
    constexpr int kGrainHalfSpan = 2;

    constexpr int kRayAngleCount = 120;
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr double kStubTipMinDrop = 16.0;
    constexpr double kStubBackMinDrop = 14.0;
    constexpr int kStubBackMaxSteps = 15;
    constexpr double kStubPerpSampleOffset = 5.5;
    constexpr double kStubPerpCheckBackDist = 6.0;
    constexpr double kStubPerpMaxDrop = 26.0;
    constexpr double kStubNmsDist = 4.5;
    constexpr double kStubNmsDirDot = 0.85;
    constexpr double kBridgeMinDist = 6.0;
    constexpr double kBridgeMaxDist = 90.0;
    constexpr double kBridgeMinDot = 0.92;
    constexpr double kBridgeBaseMaxPerp = 3.5;
    constexpr double kBridgeSlopeMaxPerp = 0.11;
    constexpr double kBridgeMinEraseFraction = 0.55;
    constexpr double kBridgeDropScale = 0.88;

    constexpr int kCalloutCenterX = 959;
    constexpr int kCalloutCenterY = 424;
    constexpr int kCalloutInitialFontPx = 24;
    constexpr int kCalloutOutlineRadiusPx = 3;
    constexpr int kCalloutEdgeMarginPx = 4;

    if (img.width < kWorkX0 + kWorkW || img.height < kWorkY0 + kWorkH) {
        return;
    }

    const std::size_t work_count = static_cast<std::size_t>(kWorkW) * kWorkH;
    auto widx = [&](int lx, int ly) noexcept -> std::size_t {
        return static_cast<std::size_t>(ly) * kWorkW + static_cast<std::size_t>(lx);
    };

    std::vector<int> luma(work_count, 0);
    std::vector<std::uint8_t> seed(work_count, 0u);
    for (int ly = 0; ly < kWorkH; ++ly) {
        for (int lx = 0; lx < kWorkW; ++lx) {
            const std::uint8_t* px =
                img.pixels.data() +
                (static_cast<std::size_t>(kWorkY0 + ly) * img.width +
                 static_cast<std::size_t>(kWorkX0 + lx)) *
                    kRgbaChannels;
            const int r = px[kRIndex];
            const int g = px[kGIndex];
            const int b = px[kBIndex];
            const std::size_t idx = widx(lx, ly);
            luma[idx] = compute_bt601_luma(r, g, b);
            if (lx >= kLocalCalloutX0 && lx < kLocalCalloutX1 &&
                ly >= kLocalCalloutY0 && ly < kLocalCalloutY1 &&
                std::min(r - b, g - b) > kYellowChromaMin && r > kYellowRedMin) {
                seed[idx] = 1u;
            }
        }
    }

    std::vector<int> dist_sq(work_count, kDistInfSq);
    for (int ly = kLocalCalloutY0; ly < kLocalCalloutY1; ++ly) {
        for (int lx = kLocalCalloutX0; lx < kLocalCalloutX1; ++lx) {
            if (seed[widx(lx, ly)] == 0) {
                continue;
            }
            const int y0 = std::max(0, ly - kDistSearchRadius);
            const int y1 = std::min(kWorkH - 1, ly + kDistSearchRadius);
            const int x0 = std::max(0, lx - kDistSearchRadius);
            const int x1 = std::min(kWorkW - 1, lx + kDistSearchRadius);
            for (int ny = y0; ny <= y1; ++ny) {
                const int dy = ny - ly;
                for (int nx = x0; nx <= x1; ++nx) {
                    const int dx = nx - lx;
                    int& slot = dist_sq[widx(nx, ny)];
                    slot = std::min(slot, dx * dx + dy * dy);
                }
            }
        }
    }

    std::vector<std::uint8_t> paper_known(work_count, 0u);
    std::vector<int> window_vals;
    window_vals.reserve(81);
    for (int ly = kCleanPaperMargin; ly < kWorkH - kCleanPaperMargin; ++ly) {
        for (int lx = kCleanPaperMargin; lx < kWorkW - kCleanPaperMargin; ++lx) {
            const std::size_t idx = widx(lx, ly);
            if (dist_sq[idx] < kCleanPaperMinDistSq || luma[idx] < kCleanPaperMinLuma) {
                continue;
            }
            window_vals.clear();
            for (int dy = -kMedianWindowRadius; dy <= kMedianWindowRadius; ++dy) {
                for (int dx = -kMedianWindowRadius; dx <= kMedianWindowRadius; ++dx) {
                    const int v = luma[widx(lx + dx, ly + dy)];
                    if (v >= kMedianFilterMinLuma) {
                        window_vals.push_back(v);
                    }
                }
            }
            if (window_vals.size() < kMinNonDarkWindowCount) {
                continue;
            }
            const std::size_t mid = window_vals.size() / 2;
            std::nth_element(window_vals.begin(),
                             window_vals.begin() + static_cast<std::ptrdiff_t>(mid),
                             window_vals.end());
            const double med = static_cast<double>(window_vals[mid]);
            if (std::abs(static_cast<double>(luma[idx]) - med) <= kMaxMedianDeviation) {
                paper_known[idx] = 1u;
            }
        }
    }

    std::vector<std::array<double, kRgbChannels>> paper_rgb(work_count);
    for (int ly = 0; ly < kWorkH; ++ly) {
        for (int lx = 0; lx < kWorkW; ++lx) {
            const std::uint8_t* px =
                img.pixels.data() +
                (static_cast<std::size_t>(kWorkY0 + ly) * img.width +
                 static_cast<std::size_t>(kWorkX0 + lx)) *
                    kRgbaChannels;
            paper_rgb[widx(lx, ly)] = {
                static_cast<double>(px[kRIndex]),
                static_cast<double>(px[kGIndex]),
                static_cast<double>(px[kBIndex]),
            };
        }
    }

    struct PaperUpdate {
        std::size_t idx;
        std::array<double, kRgbChannels> rgb;
    };
    std::vector<PaperUpdate> paper_updates;
    while (true) {
        paper_updates.clear();
        for (int ly = kPaperFillY0; ly < kPaperFillY1; ++ly) {
            for (int lx = kPaperFillX0; lx < kPaperFillX1; ++lx) {
                const std::size_t idx = widx(lx, ly);
                if (paper_known[idx] != 0) {
                    continue;
                }
                bool touches_known = false;
                for (int dy = -1; dy <= 1 && !touches_known; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (paper_known[widx(lx + dx, ly + dy)] != 0) {
                            touches_known = true;
                            break;
                        }
                    }
                }
                if (!touches_known) {
                    continue;
                }
                double wr = 0.0;
                double wg = 0.0;
                double wb = 0.0;
                double wsum = 0.0;
                for (int dy = -kPaperOnionRadius; dy <= kPaperOnionRadius; ++dy) {
                    const int sy = ly + dy;
                    if (sy < 0 || sy >= kWorkH) {
                        continue;
                    }
                    for (int dx = -kPaperOnionRadius; dx <= kPaperOnionRadius; ++dx) {
                        const int sx = lx + dx;
                        if (sx < 0 || sx >= kWorkW) {
                            continue;
                        }
                        const std::size_t sidx = widx(sx, sy);
                        if (paper_known[sidx] == 0) {
                            continue;
                        }
                        const double w = 1.0 / (1.0 + static_cast<double>(dx * dx + dy * dy));
                        wr += w * paper_rgb[sidx][kRIndex];
                        wg += w * paper_rgb[sidx][kGIndex];
                        wb += w * paper_rgb[sidx][kBIndex];
                        wsum += w;
                    }
                }
                if (wsum > 0.0) {
                    paper_updates.push_back({idx, {wr / wsum, wg / wsum, wb / wsum}});
                }
            }
        }
        if (paper_updates.empty()) {
            break;
        }
        for (const auto& u : paper_updates) {
            paper_rgb[u.idx] = u.rgb;
            paper_known[u.idx] = 1u;
        }
    }

    for (int pass = 0; pass < kPaperBlurPasses; ++pass) {
        auto next_rgb = paper_rgb;
        for (int ly = kPaperFillY0 + 2; ly < kPaperFillY1 - 2; ++ly) {
            for (int lx = kPaperFillX0 + 2; lx < kPaperFillX1 - 2; ++lx) {
                const std::size_t idx = widx(lx, ly);
                if (dist_sq[idx] > kPaperBlurMaxDistSq) {
                    continue;
                }
                double sr = 0.0;
                double sg = 0.0;
                double sb = 0.0;
                int count = 0;
                for (int dy = -kPaperBlurRadius; dy <= kPaperBlurRadius; ++dy) {
                    for (int dx = -kPaperBlurRadius; dx <= kPaperBlurRadius; ++dx) {
                        const auto& c = paper_rgb[widx(lx + dx, ly + dy)];
                        sr += c[kRIndex];
                        sg += c[kGIndex];
                        sb += c[kBIndex];
                        ++count;
                    }
                }
                const double inv = 1.0 / static_cast<double>(count);
                next_rgb[idx] = {sr * inv, sg * inv, sb * inv};
            }
        }
        paper_rgb = std::move(next_rgb);
    }

    std::vector<double> ink_drop(work_count, 0.0);
    for (std::size_t i = 0; i < work_count; ++i) {
        const double p_luma = (299.0 * paper_rgb[i][kRIndex] +
                               587.0 * paper_rgb[i][kGIndex] +
                               114.0 * paper_rgb[i][kBIndex]) /
                              1000.0;
        ink_drop[i] = p_luma - static_cast<double>(luma[i]);
    }

    struct OffsetPair {
        int dy1;
        int dx1;
        int dy2;
        int dx2;
    };
    static constexpr std::array<OffsetPair, 12> kOppositePairs = {{
        {0, 3, 0, -3},   {0, 4, 0, -4},   {3, 0, -3, 0},   {4, 0, -4, 0},
        {3, 3, -3, -3},  {4, 4, -4, -4},  {3, -3, -3, 3},  {4, -4, -4, 4},
        {4, 2, -4, -2},  {2, 4, -2, -4},  {4, -2, -4, 2},  {2, -4, -2, 4},
    }};

    auto is_bilateral_thin_stroke = [&](int lx, int ly) noexcept -> bool {
        const std::size_t idx = widx(lx, ly);
        const int nd2 = dist_sq[idx];
        if (nd2 < kBilateralMinDistSq || ink_drop[idx] < kBilateralMinInkDrop) {
            return false;
        }
        for (const auto& op : kOppositePairs) {
            const std::size_t i1 = widx(lx + op.dx1, ly + op.dy1);
            const std::size_t i2 = widx(lx + op.dx2, ly + op.dy2);
            if (dist_sq[i1] >= kBilateralMinDistSq &&
                dist_sq[i2] >= kBilateralMinDistSq &&
                ink_drop[i1] < kBilateralFlankMaxDrop &&
                ink_drop[i2] < kBilateralFlankMaxDrop) {
                return true;
            }
        }
        return nd2 >= kStrongInkMinDistSq && luma[idx] < kStrongInkMaxLuma;
    };

    std::vector<std::uint8_t> keep_stroke(work_count, 0u);
    std::deque<std::pair<int, int>> stroke_q;
    for (int ly = kPaperFillY0 - 2; ly < kPaperFillY1 + 1; ++ly) {
        for (int lx = kPaperFillX0 - 1; lx < kPaperFillX1 + 1; ++lx) {
            const std::size_t idx = widx(lx, ly);
            if (dist_sq[idx] >= kAnchorMinDistSq &&
                dist_sq[idx] <= kAnchorMaxDistSq &&
                is_bilateral_thin_stroke(lx, ly)) {
                keep_stroke[idx] = 1u;
                stroke_q.emplace_back(lx, ly);
            }
        }
    }

    while (!stroke_q.empty()) {
        const auto [cx, cy] = stroke_q.front();
        stroke_q.pop_front();
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const int nx = cx + dx;
                const int ny = cy + dy;
                if (nx < kPaperFillX0 + 2 || nx >= kPaperFillX1 - 2 ||
                    ny < kPaperFillY0 + 2 || ny >= kPaperFillY1 - 2) {
                    continue;
                }
                const std::size_t nidx = widx(nx, ny);
                if (keep_stroke[nidx] != 0 || dist_sq[nidx] >= kAnchorMinDistSq) {
                    continue;
                }
                if (is_bilateral_thin_stroke(nx, ny)) {
                    keep_stroke[nidx] = 1u;
                    stroke_q.emplace_back(nx, ny);
                }
            }
        }
    }

    std::vector<std::uint8_t> keep_soft = keep_stroke;
    for (int ly = kPaperFillY0 + 3; ly < kPaperFillY1 - 3; ++ly) {
        for (int lx = kPaperFillX0 + 3; lx < kPaperFillX1 - 3; ++lx) {
            const std::size_t idx = widx(lx, ly);
            if (keep_stroke[idx] != 0 ||
                dist_sq[idx] < kSoftHaloMinDistSq ||
                ink_drop[idx] < kSoftHaloMinInkDrop) {
                continue;
            }
            bool adj = false;
            for (int dy = -1; dy <= 1 && !adj; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (keep_stroke[widx(lx + dx, ly + dy)] != 0) {
                        adj = true;
                        break;
                    }
                }
            }
            if (adj) {
                keep_soft[idx] = 1u;
            }
        }
    }

    std::vector<std::uint8_t> erase(work_count, 0u);
    for (int ly = kEraseY0; ly < kEraseY1; ++ly) {
        for (int lx = kEraseX0; lx < kEraseX1; ++lx) {
            const std::size_t idx = widx(lx, ly);
            if (keep_soft[idx] != 0) {
                continue;
            }
            const int d2 = dist_sq[idx];
            if (d2 <= kCoreEraseDistSq ||
                (d2 <= kOuterArcEraseDistSq && ink_drop[idx] >= kOuterArcMinInkDrop)) {
                erase[idx] = 1u;
            }
        }
    }

    std::vector<std::uint8_t> ext(work_count, 0u);
    std::deque<std::pair<int, int>> ext_q;
    auto try_push_ext = [&](int lx, int ly) {
        const std::size_t idx = widx(lx, ly);
        if (ext[idx] != 0 || erase[idx] != 0) {
            return;
        }
        ext[idx] = 1u;
        ext_q.emplace_back(lx, ly);
    };
    for (int lx = kEraseX0; lx < kEraseX1; ++lx) {
        try_push_ext(lx, kEraseY0);
        try_push_ext(lx, kEraseY1 - 1);
    }
    for (int ly = kEraseY0; ly < kEraseY1; ++ly) {
        try_push_ext(kEraseX0, ly);
        try_push_ext(kEraseX1 - 1, ly);
    }
    static constexpr std::array<std::pair<int, int>, 4> kCardinalSteps = {{
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
    }};
    while (!ext_q.empty()) {
        const auto [cx, cy] = ext_q.front();
        ext_q.pop_front();
        for (const auto& [dx, dy] : kCardinalSteps) {
            const int nx = cx + dx;
            const int ny = cy + dy;
            if (nx >= kEraseX0 && nx < kEraseX1 && ny >= kEraseY0 && ny < kEraseY1) {
                try_push_ext(nx, ny);
            }
        }
    }
    for (int ly = kEraseY0; ly < kEraseY1; ++ly) {
        for (int lx = kEraseX0; lx < kEraseX1; ++lx) {
            const std::size_t idx = widx(lx, ly);
            if (ext[idx] == 0 && keep_soft[idx] == 0) {
                erase[idx] = 1u;
            }
        }
    }

    for (int ly = kEraseY0; ly < kEraseY1; ++ly) {
        for (int lx = kEraseX0; lx < kEraseX1; ++lx) {
            const std::size_t idx = widx(lx, ly);
            if (erase[idx] == 0) {
                continue;
            }
            const int gx = kWorkX0 + lx;
            const int gy = kWorkY0 + ly;
            std::uint32_t h0 =
                (static_cast<std::uint32_t>(gx / kGrainCellSize) * kGrainHashMulX) ^
                (static_cast<std::uint32_t>(gy / kGrainCellSize) * kGrainHashMulY);
            h0 = ((h0 >> kGrainHashShift) ^ h0) * kGrainHashMulX;
            const int delta = static_cast<int>(h0 % kGrainSpan) - kGrainHalfSpan;

            std::uint8_t* px =
                img.pixels.data() +
                (static_cast<std::size_t>(gy) * img.width + static_cast<std::size_t>(gx)) *
                    kRgbaChannels;
            px[kRIndex] = static_cast<std::uint8_t>(std::clamp(
                static_cast<int>(std::lround(paper_rgb[idx][kRIndex])) + delta,
                0,
                kMaxChannelInt));
            px[kGIndex] = static_cast<std::uint8_t>(std::clamp(
                static_cast<int>(std::lround(paper_rgb[idx][kGIndex])) + delta,
                0,
                kMaxChannelInt));
            px[kBIndex] = static_cast<std::uint8_t>(std::clamp(
                static_cast<int>(std::lround(paper_rgb[idx][kBIndex])) + delta,
                0,
                kMaxChannelInt));
        }
    }

    struct StrokeStub {
        double x;
        double y;
        double ux;
        double uy;
        double drop;
    };
    std::vector<StrokeStub> raw_stubs;
    for (int ly = kPaperFillY0; ly < kEraseY1; ++ly) {
        for (int lx = kPaperFillX0 + 2; lx < kEraseX1; ++lx) {
            const std::size_t idx = widx(lx, ly);
            if (erase[idx] != 0 || ink_drop[idx] < kStubTipMinDrop) {
                continue;
            }
            bool touches_erase = false;
            for (int dy = -1; dy <= 1 && !touches_erase; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (erase[widx(lx + dx, ly + dy)] != 0) {
                        touches_erase = true;
                        break;
                    }
                }
            }
            if (!touches_erase) {
                continue;
            }

            double best_score = -1.0;
            double best_ux = 0.0;
            double best_uy = 0.0;
            double best_med_drop = 0.0;
            std::array<double, kStubBackMaxSteps> step_drops{};

            for (int k_ang = 0; k_ang < kRayAngleCount; ++k_ang) {
                const double ang = static_cast<double>(k_ang) * (kTwoPi / kRayAngleCount);
                const double ux = std::cos(ang);
                const double uy = std::sin(ang);

                int in_erase_cnt = 0;
                for (int tf : {2, 4, 6}) {
                    const int fx = static_cast<int>(std::lround(lx + tf * ux));
                    const int fy = static_cast<int>(std::lround(ly + tf * uy));
                    if (fx >= 0 && fx < kWorkW && fy >= 0 && fy < kWorkH &&
                        erase[widx(fx, fy)] != 0) {
                        ++in_erase_cnt;
                    }
                }
                if (in_erase_cnt < 2) {
                    continue;
                }

                bool ok = true;
                double sum_drop = 0.0;
                for (int tb = 1; tb <= kStubBackMaxSteps; ++tb) {
                    const double bxf = static_cast<double>(lx) - tb * ux;
                    const double byf = static_cast<double>(ly) - tb * uy;
                    double d_max = 0.0;
                    for (const auto& [sp_off, sp_w] :
                         std::array<std::pair<double, double>, 3>{{{0.0, 1.0},
                                                                   {-1.0, 0.9},
                                                                   {1.0, 0.9}}}) {
                        const int qx = static_cast<int>(std::lround(bxf - sp_off * uy));
                        const int qy = static_cast<int>(std::lround(byf + sp_off * ux));
                        if (qx >= 10 && qx < kWorkW - 10 && qy >= 10 && qy < kWorkH - 10 &&
                            erase[widx(qx, qy)] == 0) {
                            d_max = std::max(d_max, sp_w * ink_drop[widx(qx, qy)]);
                        }
                    }
                    if (d_max < kStubBackMinDrop) {
                        ok = false;
                        break;
                    }
                    step_drops[static_cast<std::size_t>(tb - 1)] = d_max;
                    sum_drop += d_max;
                }
                if (!ok) {
                    continue;
                }

                const double mx = static_cast<double>(lx) - kStubPerpCheckBackDist * ux;
                const double my = static_cast<double>(ly) - kStubPerpCheckBackDist * uy;
                const int p1x = static_cast<int>(std::lround(mx - kStubPerpSampleOffset * uy));
                const int p1y = static_cast<int>(std::lround(my + kStubPerpSampleOffset * ux));
                const int p2x = static_cast<int>(std::lround(mx + kStubPerpSampleOffset * uy));
                const int p2y = static_cast<int>(std::lround(my - kStubPerpSampleOffset * ux));
                const double dp1 =
                    (erase[widx(p1x, p1y)] != 0) ? 0.0 : ink_drop[widx(p1x, p1y)];
                const double dp2 =
                    (erase[widx(p2x, p2y)] != 0) ? 0.0 : ink_drop[widx(p2x, p2y)];
                if (dp1 > kStubPerpMaxDrop || dp2 > kStubPerpMaxDrop) {
                    continue;
                }

                const double mean_drop = sum_drop / kStubBackMaxSteps;
                if (mean_drop > best_score) {
                    best_score = mean_drop;
                    best_ux = ux;
                    best_uy = uy;
                    auto sorted_drops = step_drops;
                    std::nth_element(sorted_drops.begin(),
                                     sorted_drops.begin() + kStubBackMaxSteps / 2,
                                     sorted_drops.end());
                    best_med_drop = sorted_drops[kStubBackMaxSteps / 2];
                }
            }

            if (best_score > 0.0) {
                raw_stubs.push_back({static_cast<double>(lx),
                                     static_cast<double>(ly),
                                     best_ux,
                                     best_uy,
                                     best_med_drop});
            }
        }
    }

    std::sort(raw_stubs.begin(), raw_stubs.end(), [](const StrokeStub& a, const StrokeStub& b) {
        return a.drop > b.drop;
    });
    std::vector<StrokeStub> stubs;
    for (const auto& cand : raw_stubs) {
        bool duplicate = false;
        for (const auto& kept : stubs) {
            if (std::hypot(cand.x - kept.x, cand.y - kept.y) < kStubNmsDist &&
                (cand.ux * kept.ux + cand.uy * kept.uy) > kStubNmsDirDot) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            stubs.push_back(cand);
        }
    }

    std::vector<std::uint8_t> stub_used(stubs.size(), 0u);
    for (std::size_t i = 0; i < stubs.size(); ++i) {
        if (stub_used[i] != 0) {
            continue;
        }
        const auto& s1 = stubs[i];
        std::size_t best_j = stubs.size();
        double best_pair_score = std::numeric_limits<double>::infinity();

        for (std::size_t j = i + 1; j < stubs.size(); ++j) {
            if (stub_used[j] != 0) {
                continue;
            }
            const auto& s2 = stubs[j];
            const double dx = s2.x - s1.x;
            const double dy = s2.y - s1.y;
            const double dist = std::hypot(dx, dy);
            if (dist < kBridgeMinDist || dist > kBridgeMaxDist) {
                continue;
            }
            const double cx_u = dx / dist;
            const double cy_u = dy / dist;
            if ((s1.ux * cx_u + s1.uy * cy_u) < kBridgeMinDot ||
                (s2.ux * (-cx_u) + s2.uy * (-cy_u)) < kBridgeMinDot) {
                continue;
            }
            const double perp1 = std::abs(dx * s1.uy - dy * s1.ux);
            const double perp2 = std::abs(dx * s2.uy - dy * s2.ux);
            const double max_perp = kBridgeBaseMaxPerp + kBridgeSlopeMaxPerp * dist;
            if (std::max(perp1, perp2) > max_perp) {
                continue;
            }
            const int steps = std::max(4, static_cast<int>(dist));
            int er_cnt = 0;
            for (int si = 1; si < steps; ++si) {
                const double t = static_cast<double>(si) / static_cast<double>(steps);
                const int px = static_cast<int>(std::lround(s1.x + t * dx));
                const int py = static_cast<int>(std::lround(s1.y + t * dy));
                if (erase[widx(px, py)] != 0) {
                    ++er_cnt;
                }
            }
            if (static_cast<double>(er_cnt) < kBridgeMinEraseFraction * (steps - 1)) {
                continue;
            }
            const double pair_score = dist + 8.0 * std::max(perp1, perp2);
            if (pair_score < best_pair_score) {
                best_pair_score = pair_score;
                best_j = j;
            }
        }

        if (best_j >= stubs.size()) {
            continue;
        }
        stub_used[i] = 1u;
        stub_used[best_j] = 1u;
        const auto& s2 = stubs[best_j];
        const double dx = s2.x - s1.x;
        const double dy = s2.y - s1.y;
        const double seg_len = std::hypot(dx, dy);
        const double sx_u = dx / seg_len;
        const double sy_u = dy / seg_len;
        const double stroke_drop = std::min(s1.drop, s2.drop) * kBridgeDropScale;
        double stroke_hw = 1.45;
        if (stroke_drop < 45.0) {
            stroke_hw = 0.75;
        } else if (stroke_drop < 80.0) {
            stroke_hw = 1.05;
        }

        const int bx0 = std::max(kEraseX0, static_cast<int>(std::min(s1.x, s2.x)) - 3);
        const int bx1 = std::min(kEraseX1, static_cast<int>(std::max(s1.x, s2.x)) + 4);
        const int by0 = std::max(kEraseY0, static_cast<int>(std::min(s1.y, s2.y)) - 3);
        const int by1 = std::min(kEraseY1, static_cast<int>(std::max(s1.y, s2.y)) + 4);
        for (int py = by0; py < by1; ++py) {
            for (int px = bx0; px < bx1; ++px) {
                if (erase[widx(px, py)] == 0) {
                    continue;
                }
                const double rx = static_cast<double>(px) - s1.x;
                const double ry = static_cast<double>(py) - s1.y;
                const double along = rx * sx_u + ry * sy_u;
                if (along < -0.5 || along > seg_len + 0.5) {
                    continue;
                }
                const double perp = std::abs(rx * sy_u - ry * sx_u);
                if (perp > stroke_hw + 1.0) {
                    continue;
                }
                const double cov_a =
                    std::clamp(1.0 - std::max(0.0, perp - (stroke_hw - 0.45)) / 1.15, 0.0, 1.0);
                const int darken = static_cast<int>(std::lround(stroke_drop * cov_a));
                std::uint8_t* dst =
                    img.pixels.data() +
                    (static_cast<std::size_t>(kWorkY0 + py) * img.width +
                     static_cast<std::size_t>(kWorkX0 + px)) *
                        kRgbaChannels;
                dst[kRIndex] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(dst[kRIndex]) - darken, 0, kMaxChannelInt));
                dst[kGIndex] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(dst[kGIndex]) - darken, 0, kMaxChannelInt));
                dst[kBIndex] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(dst[kBIndex]) - darken, 0, kMaxChannelInt));
            }
        }
    }

    TextEffectStyle st;
    st.fill = {232, 216, 16, kFullAlpha};
    st.has_outline = true;
    st.outline = {25, 22, 18, kFullAlpha};
    st.outline_radius_px = kCalloutOutlineRadiusPx;

    draw_rotated_minus45_callout(img,
                                 "Go Back\nto Big-Boob Land!",
                                 kCalloutCenterX,
                                 kCalloutCenterY,
                                 kCalloutInitialFontPx,
                                 FW_EXTRABOLD,
                                 st,
                                 kCalloutBox,
                                 kCalloutEdgeMarginPx);
}

void render_ev0_entry(std::uint32_t entry_index, RgbaImage& img) {
    switch (entry_index) {
        case 70:
            render_ev0_denky_comment(img);
            return;
        case 71:
            render_ev0_karen_comment(img);
            return;
        case 76:
            render_ev0_metawo_callout(img);
            return;
        default:
            throw std::runtime_error("Unexpected ev0 entry index in menu renderer");
    }
}

}  // namespace

std::uint32_t ComputeMurmurHash2(const std::uint8_t* data,
                                 std::size_t len,
                                 std::uint32_t seed) {
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);
    const std::uint8_t* cur = data;
    std::size_t rem = len;

    while (rem >= kMurmurBlockBytes) {
        std::uint32_t k = read_u32_le(cur);
        k *= kMurmurHash2Multiplier;
        k ^= k >> kMurmurHash2Shift;
        k *= kMurmurHash2Multiplier;

        h *= kMurmurHash2Multiplier;
        h ^= k;

        cur += kMurmurBlockBytes;
        rem -= kMurmurBlockBytes;
    }

    switch (rem) {
        case kMurmurTailBytes3:
            h ^= static_cast<std::uint32_t>(cur[kByteIndex2]) << kByteShift2;
            [[fallthrough]];
        case kMurmurTailBytes2:
            h ^= static_cast<std::uint32_t>(cur[kByteIndex1]) << kByteShift1;
            [[fallthrough]];
        case kMurmurTailBytes1:
            h ^= static_cast<std::uint32_t>(cur[kByteIndex0]);
            h *= kMurmurHash2Multiplier;
            break;
        default:
            break;
    }

    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurHash2Multiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

void XorMaskPayload(std::uint8_t* buf,
                    std::size_t len,
                    const std::uint8_t* orig_slice,
                    std::size_t orig_len,
                    std::uint32_t orig_data_hash) {
    if (!buf || len == 0) {
        return;
    }
    const bool has_orig = (orig_slice != nullptr && orig_len > 0);
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint32_t shift =
            (static_cast<std::uint32_t>(i) & kXorHashByteIndexMask) * kXorBitsPerByte;
        const std::uint8_t hash_byte =
            static_cast<std::uint8_t>((orig_data_hash >> shift) & kXorByteMask);
        const std::uint8_t pos_stream = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(i) * kXorPositionStride + kXorPositionBias) & kXorByteMask);
        const std::uint8_t pos_mix = static_cast<std::uint8_t>(hash_byte ^ pos_stream);
        const std::uint8_t orig_byte = has_orig ? orig_slice[i % orig_len] : 0u;
        buf[i] ^= static_cast<std::uint8_t>(orig_byte ^ pos_mix);
    }
}

std::vector<std::uint8_t> encrypt_fxf(const std::string& plain_cp932) {
    std::vector<std::uint8_t> out(plain_cp932.size(), 0u);
    for (std::size_t i = 0; i < plain_cp932.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(plain_cp932[i]) ^
                                           kFxfBitwiseNotMask);
    }
    return out;
}

std::string decrypt_fxf(const std::vector<std::uint8_t>& raw) {
    std::string out(raw.size(), '\0');
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<char>(raw[i] ^ kFxfBitwiseNotMask);
    }
    return out;
}

std::string patch_charaname_xtx_cp932(const std::string& orig_cp932) {
    struct CharaEntry {
        const char* code;
        const char* fullwidth_romaji_utf8;
    };
    static constexpr std::array<CharaEntry, 9> kCharaMap = {{
        {"aa", "Ｍｉｙａｋａ"},
        {"bb", "Ｒｅｎ"},
        {"cc", "Ａｙａｋａ"},
        {"dd", "Ｙｕｒｉａ"},
        {"ee", "Ｓｈｉｏ"},
        {"ff", "Ｙｕｍｅｍｉ"},
        {"gg", "Ｋａｚｅｎｏｍｉｙａ"},
        {"hh", "Ｋｙｏｕ"},
        {"ii", "Ｋｙｏｕ"},
    }};

    auto lines = split_preserving_line_endings(orig_cp932);
    for (auto& ln : lines) {
        const std::size_t comma = ln.body.find(',');
        if (comma == std::string::npos) {
            continue;
        }
        const std::string key = ln.body.substr(0, comma);
        for (const auto& entry : kCharaMap) {
            if (key == entry.code) {
                ln.body = key + "," + utf8_literal_to_cp932(entry.fullwidth_romaji_utf8);
                break;
            }
        }
    }
    return join_raw_lines(lines);
}

std::string patch_backlog_fxf_cp932(const std::string& orig_cp932) {
    constexpr std::string_view kKeyPrefix = "firstMessage1 ";
    constexpr std::size_t kTabColorTagBytes = 4;
    constexpr std::size_t kBareColorTagBytes = 3;
    constexpr std::size_t kLeadingFullwidthSpaceCount = 8;

    const std::string fw_space_cp932 = utf8_literal_to_cp932("　");
    const std::string fw_backlog_cp932 = utf8_literal_to_cp932("Ｂａｃｋｌｏｇ");
    const std::string default_color_tag_cp932 = utf8_literal_to_cp932("\t#白");

    std::string spaces;
    spaces.reserve(fw_space_cp932.size() * kLeadingFullwidthSpaceCount);
    for (std::size_t i = 0; i < kLeadingFullwidthSpaceCount; ++i) {
        spaces.append(fw_space_cp932);
    }

    auto lines = split_preserving_line_endings(orig_cp932);
    for (auto& ln : lines) {
        if (ln.body.rfind(kKeyPrefix, 0) != 0) {
            continue;
        }
        const std::string rest = ln.body.substr(kKeyPrefix.size());
        std::string color_tag = default_color_tag_cp932;
        if (rest.size() >= kTabColorTagBytes && rest[0] == '\t' && rest[1] == '#') {
            color_tag = rest.substr(0, kTabColorTagBytes);
        } else if (rest.size() >= kBareColorTagBytes && rest[0] == '#') {
            color_tag = rest.substr(0, kBareColorTagBytes);
        }
        ln.body = std::string(kKeyPrefix) + color_tag + spaces + fw_backlog_cp932;
    }
    return join_raw_lines(lines);
}

std::string patch_main_control_fxf_cp932(const std::string& orig_cp932) {
    const bool is_hd10 =
        (orig_cp932.find("bcycmushikuruhd10class") != std::string::npos) ||
        (orig_cp932.find("MUSHIKURUHD10") != std::string::npos) ||
        (orig_cp932.find("HD10") != std::string::npos);

    const std::string win_title =
        is_hd10 ? "EXTRAVAGANZA ~Mushigurui~ HD10" : "EXTRAVAGANZA ~Mushigurui~ CE";
    const std::string trial_title =
        is_hd10 ? "EXTRAVAGANZA ~Mushigurui~ HD10 Trial" : "EXTRAVAGANZA ~Mushigurui~ CE Trial";

    auto lines = split_preserving_line_endings(orig_cp932);
    for (auto& ln : lines) {
        if (ln.body.rfind("windowTitle ", 0) == 0) {
            ln.body = "windowTitle " + win_title;
        } else if (ln.body.rfind("windowTitleTaiken ", 0) == 0) {
            ln.body = "windowTitleTaiken " + trial_title;
        } else if (ln.body.rfind("bppErrorMessage ", 0) == 0) {
            ln.body = "bppErrorMessage Please set display to 16-bit color or higher.";
        }
    }
    return join_raw_lines(lines);
}

ParsedGtbTable parse_gtb_offsets(const std::vector<std::uint8_t>& gtb_bytes,
                                 std::uint64_t gpk_size) {
    if (gtb_bytes.size() < kGtbCountFieldSize) {
        throw std::runtime_error("GTB file too small for entry count");
    }
    const std::uint32_t count = read_u32_le(gtb_bytes.data());
    const std::size_t base_tables_end =
        kGtbCountFieldSize + static_cast<std::size_t>(count) * kGtbU32EntryStride * 2u;
    if (gtb_bytes.size() < base_tables_end) {
        throw std::runtime_error("GTB file truncated inside 32-bit offset table");
    }

    ParsedGtbTable out;
    out.entry_count = count;
    out.offsets.resize(static_cast<std::size_t>(count) + 1u, 0u);

    const std::size_t u32_off_pos =
        kGtbCountFieldSize + static_cast<std::size_t>(count) * kGtbU32EntryStride;
    const std::size_t min_over2g_size =
        base_tables_end + static_cast<std::size_t>(count) * kGtbU64EntryStride +
        kGtbOver2GFooterPaddingBytes;

    if (gtb_bytes.size() >= min_over2g_size &&
        std::memcmp(gtb_bytes.data() + (gtb_bytes.size() - kGtbOver2GTrailerSize),
                    kGtbOver2GTrailer,
                    kGtbOver2GTrailerSize) == 0) {
        out.has_over2g_u64_table = true;
        const std::size_t u64_off_pos =
            gtb_bytes.size() - kGtbOver2GFooterPaddingBytes -
            static_cast<std::size_t>(count) * kGtbU64EntryStride;

        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t off32 =
                read_u32_le(gtb_bytes.data() + u32_off_pos + static_cast<std::size_t>(i) * kGtbU32EntryStride);
            const std::uint64_t off64 =
                read_u64_le(gtb_bytes.data() + u64_off_pos + static_cast<std::size_t>(i) * kGtbU64EntryStride);
            if (static_cast<std::uint32_t>(off64 & kLowU32Mask) != off32) {
                throw std::runtime_error("GTB 32-bit and 64-bit offset tables disagree");
            }
            out.offsets[i] = off64;
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) {
            out.offsets[i] = read_u32_le(
                gtb_bytes.data() + u32_off_pos + static_cast<std::size_t>(i) * kGtbU32EntryStride);
        }
    }

    out.offsets[count] = gpk_size;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (out.offsets[i] > out.offsets[i + 1]) {
            throw std::runtime_error("GTB entry offsets are not monotonically non-decreasing");
        }
    }
    return out;
}

std::vector<std::uint8_t> update_gtb_offsets(
    const std::vector<std::uint8_t>& orig_gtb,
    const std::vector<std::uint64_t>& new_offsets) {
    if (orig_gtb.size() < kGtbCountFieldSize) {
        throw std::runtime_error("orig_gtb too small in update_gtb_offsets");
    }
    const std::uint32_t count = read_u32_le(orig_gtb.data());
    if (new_offsets.size() < count) {
        throw std::runtime_error("new_offsets has fewer elements than GTB count");
    }
    const std::size_t base_tables_end =
        kGtbCountFieldSize + static_cast<std::size_t>(count) * kGtbU32EntryStride * 2u;
    if (orig_gtb.size() < base_tables_end) {
        throw std::runtime_error("orig_gtb truncated in update_gtb_offsets");
    }

    std::vector<std::uint8_t> out = orig_gtb;
    const std::size_t u32_off_pos =
        kGtbCountFieldSize + static_cast<std::size_t>(count) * kGtbU32EntryStride;

    const std::size_t min_over2g_size =
        base_tables_end + static_cast<std::size_t>(count) * kGtbU64EntryStride +
        kGtbOver2GFooterPaddingBytes;
    const bool has_over2g =
        orig_gtb.size() >= min_over2g_size &&
        std::memcmp(orig_gtb.data() + (orig_gtb.size() - kGtbOver2GTrailerSize),
                    kGtbOver2GTrailer,
                    kGtbOver2GTrailerSize) == 0;

    if (!has_over2g) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (new_offsets[i] > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("64-bit GPK offset requires over2G! GTB table");
            }
        }
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        write_u32_le(out.data() + u32_off_pos + static_cast<std::size_t>(i) * kGtbU32EntryStride,
                     static_cast<std::uint32_t>(new_offsets[i] & kLowU32Mask));
    }

    if (has_over2g) {
        const std::size_t u64_off_pos =
            out.size() - kGtbOver2GFooterPaddingBytes -
            static_cast<std::size_t>(count) * kGtbU64EntryStride;
        for (std::uint32_t i = 0; i < count; ++i) {
            write_u64_le(
                out.data() + u64_off_pos + static_cast<std::size_t>(i) * kGtbU64EntryStride,
                new_offsets[i]);
        }
    }
    return out;
}

std::array<std::int32_t, kDwqCropMarginCount> compute_rgba_crop_margins(
    const RgbaImage& img) {
    if (img.width <= 0 || img.height <= 0 ||
        img.pixels.size() < static_cast<std::size_t>(img.width) * img.height * kRgbaChannels) {
        return {0, 0, 0, 0};
    }

    int min_x = img.width;
    int max_x = -1;
    int min_y = img.height;
    int max_y = -1;

    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t* row =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * kRgbaChannels;
        for (int x = 0; x < img.width; ++x) {
            if (row[static_cast<std::size_t>(x) * kRgbaChannels + kAIndex] != 0) {
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }

    if (max_x < 0 || max_y < 0) {
        return {0, 0, 0, 0};
    }
    return {
        static_cast<std::int32_t>(min_y),
        static_cast<std::int32_t>(min_x),
        static_cast<std::int32_t>(img.width - 1 - max_x),
        static_cast<std::int32_t>(img.height - 1 - max_y),
    };
}

std::vector<std::uint8_t> wrap_dwq_png_preserving_header(
    const std::vector<std::uint8_t>& orig_dwq_raw,
    const std::vector<std::uint8_t>& encoded_png,
    bool recompute_crop_margins,
    const RgbaImage* decoded_for_crop) {
    if (orig_dwq_raw.size() < kDwqHeaderSize) {
        throw std::runtime_error("DWQ entry is smaller than 64-byte header");
    }
    if (encoded_png.size() < kPngMinValidBytes ||
        std::memcmp(encoded_png.data(), kPngSignature, kPngSignatureBytes) != 0) {
        throw std::runtime_error("Invalid PNG payload passed to wrap_dwq_png_preserving_header");
    }

    std::vector<std::uint8_t> out;
    out.resize(kDwqHeaderSize + encoded_png.size());
    std::memcpy(out.data(), orig_dwq_raw.data(), kDwqHeaderSize);
    write_u32_le(out.data() + kDwqDataSizeOffset, static_cast<std::uint32_t>(encoded_png.size()));

    if (recompute_crop_margins) {
        RgbaImage local_decoded;
        const RgbaImage* src_img = decoded_for_crop;
        if (!src_img) {
            local_decoded = decode_png_rgba(encoded_png.data(), encoded_png.size());
            src_img = &local_decoded;
        }
        const auto margins = compute_rgba_crop_margins(*src_img);
        write_i32_le(out.data() + kDwqCropMarginsOffset + kCropTopIndex * kU32ByteSize,
                     margins[kCropTopIndex]);
        write_i32_le(out.data() + kDwqCropMarginsOffset + kCropLeftIndex * kU32ByteSize,
                     margins[kCropLeftIndex]);
        write_i32_le(out.data() + kDwqCropMarginsOffset + kCropRightIndex * kU32ByteSize,
                     margins[kCropRightIndex]);
        write_i32_le(out.data() + kDwqCropMarginsOffset + kCropBottomIndex * kU32ByteSize,
                     margins[kCropBottomIndex]);
    }

    std::memcpy(out.data() + kDwqHeaderSize, encoded_png.data(), encoded_png.size());
    return out;
}

RgbaImage decode_png_rgba(const std::uint8_t* png_data, std::size_t png_size) {
    if (!png_data || png_size < kPngMinValidBytes) {
        throw std::runtime_error("Invalid PNG buffer passed to decode_png_rgba");
    }

    ScopedComInit com;
    auto factory = create_wic_factory();

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        throw std::runtime_error("IWICImagingFactory::CreateStream failed");
    }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(png_data), static_cast<DWORD>(png_size));
    if (FAILED(hr)) {
        throw std::runtime_error("IWICStream::InitializeFromMemory failed");
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        throw std::runtime_error("CreateDecoderFromStream failed for PNG");
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        throw std::runtime_error("PNG GetFrame(0) failed");
    }

    UINT w = 0;
    UINT h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        throw std::runtime_error("PNG GetSize failed");
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        throw std::runtime_error("CreateFormatConverter failed");
    }
    hr = converter->Initialize(frame.Get(),
                               GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone,
                               nullptr,
                               0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICFormatConverter::Initialize(32bppRGBA) failed");
    }

    RgbaImage img;
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.channels = kRgbaChannels;
    const UINT stride = w * static_cast<UINT>(kRgbaChannels);
    const UINT buf_size = stride * h;
    img.pixels.resize(buf_size);

    hr = converter->CopyPixels(nullptr, stride, buf_size, img.pixels.data());
    if (FAILED(hr)) {
        throw std::runtime_error("IWICFormatConverter::CopyPixels failed");
    }
    return img;
}

std::vector<std::uint8_t> encode_png(const RgbaImage& img, bool write_24bpp_rgb) {
    if (img.width <= 0 || img.height <= 0 ||
        img.pixels.size() < static_cast<std::size_t>(img.width) * img.height * kRgbaChannels) {
        throw std::runtime_error("Invalid RgbaImage passed to encode_png");
    }

    ScopedComInit com;
    auto factory = create_wic_factory();

    ComPtr<IStream> mem_stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &mem_stream);
    if (FAILED(hr) || !mem_stream) {
        throw std::runtime_error("CreateStreamOnHGlobal failed");
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        throw std::runtime_error("CreateEncoder(GUID_ContainerFormatPng) failed");
    }
    hr = encoder->Initialize(mem_stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapEncoder::Initialize failed");
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || !frame) {
        throw std::runtime_error("IWICBitmapEncoder::CreateNewFrame failed");
    }
    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::Initialize failed");
    }
    hr = frame->SetSize(static_cast<UINT>(img.width), static_cast<UINT>(img.height));
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::SetSize failed");
    }

    WICPixelFormatGUID fmt =
        write_24bpp_rgb ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::SetPixelFormat failed");
    }

    if (write_24bpp_rgb) {
        const UINT stride = static_cast<UINT>(img.width * kRgbChannels);
        std::vector<std::uint8_t> bgr(static_cast<std::size_t>(stride) * img.height);
        for (int y = 0; y < img.height; ++y) {
            const std::uint8_t* src_row =
                img.pixels.data() + static_cast<std::size_t>(y) * img.width * kRgbaChannels;
            std::uint8_t* dst_row = bgr.data() + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < img.width; ++x) {
                const std::size_t dst_off = static_cast<std::size_t>(x) * kRgbChannels;
                const std::size_t src_off = static_cast<std::size_t>(x) * kRgbaChannels;
                dst_row[dst_off + kByteIndex0] = src_row[src_off + kBIndex];
                dst_row[dst_off + kByteIndex1] = src_row[src_off + kGIndex];
                dst_row[dst_off + kByteIndex2] = src_row[src_off + kRIndex];
            }
        }
        hr = frame->WritePixels(
            static_cast<UINT>(img.height), stride, static_cast<UINT>(bgr.size()), bgr.data());
    } else {
        const UINT stride = static_cast<UINT>(img.width * kRgbaChannels);
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(stride) * img.height);
        for (int y = 0; y < img.height; ++y) {
            const std::uint8_t* src_row =
                img.pixels.data() + static_cast<std::size_t>(y) * img.width * kRgbaChannels;
            std::uint8_t* dst_row = bgra.data() + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < img.width; ++x) {
                const std::size_t px_off = static_cast<std::size_t>(x) * kRgbaChannels;
                dst_row[px_off + kByteIndex0] = src_row[px_off + kBIndex];
                dst_row[px_off + kByteIndex1] = src_row[px_off + kGIndex];
                dst_row[px_off + kByteIndex2] = src_row[px_off + kRIndex];
                dst_row[px_off + kByteIndex3] = src_row[px_off + kAIndex];
            }
        }
        hr = frame->WritePixels(
            static_cast<UINT>(img.height), stride, static_cast<UINT>(bgra.size()), bgra.data());
    }
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::WritePixels failed");
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::Commit failed");
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapEncoder::Commit failed");
    }

    STATSTG stat{};
    hr = mem_stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) {
        throw std::runtime_error("IStream::Stat failed after PNG encode");
    }

    const std::size_t total_bytes = static_cast<std::size_t>(stat.cbSize.QuadPart);
    std::vector<std::uint8_t> out(total_bytes);
    LARGE_INTEGER zero{};
    mem_stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG bytes_read = 0;
    hr = mem_stream->Read(out.data(), static_cast<ULONG>(total_bytes), &bytes_read);
    if (FAILED(hr) || bytes_read != total_bytes) {
        throw std::runtime_error("IStream::Read failed after PNG encode");
    }
    return out;
}

std::vector<std::uint8_t> serialize_menu_patch_bin(const ParsedMenuPatchBin& patch) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kMenuPatchMagic, kMenuPatchMagic + kMenuPatchMagicSize);
    append_u32_le(out, patch.version);
    append_u32_le(out, static_cast<std::uint32_t>(patch.standalone_groups.size()));
    append_u32_le(out, static_cast<std::uint32_t>(patch.archives.size()));
    append_u32_le(out, patch.flags);
    append_u64_le(out, patch.reserved);

    for (const auto& grp : patch.standalone_groups) {
        if (grp.rel_path.empty() ||
            grp.rel_path.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("Invalid standalone rel_path length");
        }
        if (grp.variants.empty() ||
            grp.variants.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("Invalid standalone variant_count");
        }
        append_u16_le(out, static_cast<std::uint16_t>(grp.rel_path.size()));
        out.insert(out.end(), grp.rel_path.begin(), grp.rel_path.end());
        append_u16_le(out, static_cast<std::uint16_t>(grp.variants.size()));
        for (const auto& var : grp.variants) {
            if (var.masked_payload.size() != var.new_file_size) {
                throw std::runtime_error("Standalone masked_payload size mismatch");
            }
            append_u32_le(out, var.orig_file_size);
            append_u32_le(out, var.orig_data_hash);
            append_u32_le(out, var.new_file_size);
            append_u32_le(out, var.new_data_hash);
            out.insert(out.end(), var.masked_payload.begin(), var.masked_payload.end());
        }
    }

    for (const auto& sec : patch.archives) {
        serialize_archive_section_into(out, sec);
    }
    return out;
}

ParsedMenuPatchBin parse_menu_patch_bin(const std::vector<std::uint8_t>& patch_bytes) {
    if (patch_bytes.size() < kMenuPatchHeaderSize) {
        throw std::runtime_error("menu_patch.bin truncated inside 32-byte header");
    }
    if (std::memcmp(patch_bytes.data(), kMenuPatchMagic, kMenuPatchMagicSize) != 0) {
        throw std::runtime_error("menu_patch.bin magic mismatch (expected MSGMENU1)");
    }

    ParsedMenuPatchBin out;
    out.version = read_u32_le(patch_bytes.data() + kHeaderVersionOffset);
    if (out.version != kMenuPatchFormatVersion) {
        throw std::runtime_error("Unsupported menu_patch.bin format version");
    }
    const std::uint32_t standalone_count =
        read_u32_le(patch_bytes.data() + kHeaderStandaloneCountOffset);
    const std::uint32_t archive_count =
        read_u32_le(patch_bytes.data() + kHeaderArchiveCountOffset);
    out.flags = read_u32_le(patch_bytes.data() + kHeaderFlagsOffset);
    out.reserved = read_u64_le(patch_bytes.data() + kHeaderReservedOffset);

    std::size_t off = kMenuPatchHeaderSize;
    auto require_bytes = [&](std::size_t n, const char* ctx) {
        if (n > patch_bytes.size() - off) {
            throw std::runtime_error(std::string("Truncated menu_patch.bin at ") + ctx);
        }
    };

    out.standalone_groups.reserve(standalone_count);
    for (std::uint32_t g = 0; g < standalone_count; ++g) {
        require_bytes(kU16ByteSize, "standalone path_len");
        const std::uint16_t path_len = read_u16_le(patch_bytes.data() + off);
        off += kU16ByteSize;
        if (path_len == 0) {
            throw std::runtime_error("Empty standalone rel_path in menu_patch.bin");
        }
        require_bytes(path_len + kU16ByteSize, "standalone rel_path + variant_count");

        StandaloneGroupRecord grp;
        grp.rel_path.assign(reinterpret_cast<const char*>(patch_bytes.data() + off), path_len);
        off += path_len;

        const std::uint16_t variant_count = read_u16_le(patch_bytes.data() + off);
        off += kU16ByteSize;
        if (variant_count == 0) {
            throw std::runtime_error("Zero variant_count in standalone group");
        }

        grp.variants.reserve(variant_count);
        for (std::uint16_t v = 0; v < variant_count; ++v) {
            require_bytes(kStandaloneVariantHeaderBytes, "standalone variant header");
            StandaloneVariantRecord var;
            var.orig_file_size = read_u32_le(patch_bytes.data() + off + kVariantOrigSizeOffset);
            var.orig_data_hash = read_u32_le(patch_bytes.data() + off + kVariantOrigHashOffset);
            var.new_file_size = read_u32_le(patch_bytes.data() + off + kVariantNewSizeOffset);
            var.new_data_hash = read_u32_le(patch_bytes.data() + off + kVariantNewHashOffset);
            off += kStandaloneVariantHeaderBytes;

            require_bytes(var.new_file_size, "standalone variant payload");
            var.masked_payload.assign(
                patch_bytes.begin() + static_cast<std::ptrdiff_t>(off),
                patch_bytes.begin() + static_cast<std::ptrdiff_t>(off + var.new_file_size));
            off += var.new_file_size;
            grp.variants.push_back(std::move(var));
        }
        out.standalone_groups.push_back(std::move(grp));
    }

    out.archives.reserve(archive_count);
    for (std::uint32_t a = 0; a < archive_count; ++a) {
        require_bytes(kU16ByteSize, "archive stem_len");
        const std::uint16_t stem_len = read_u16_le(patch_bytes.data() + off);
        off += kU16ByteSize;
        if (stem_len == 0) {
            throw std::runtime_error("Empty archive_stem in menu_patch.bin");
        }
        require_bytes(stem_len + kArchiveFixedFieldsBytes, "archive section header");

        ArchiveSectionRecord sec;
        sec.archive_stem.assign(reinterpret_cast<const char*>(patch_bytes.data() + off), stem_len);
        off += stem_len;

        sec.orig_gpk_size = read_u64_le(patch_bytes.data() + off + kArchiveOrigGpkSizeOffset);
        sec.new_gpk_size = read_u64_le(patch_bytes.data() + off + kArchiveNewGpkSizeOffset);
        sec.orig_gtb_size = read_u32_le(patch_bytes.data() + off + kArchiveOrigGtbSizeOffset);
        sec.orig_gtb_hash = read_u32_le(patch_bytes.data() + off + kArchiveOrigGtbHashOffset);
        sec.new_gtb_size = read_u32_le(patch_bytes.data() + off + kArchiveNewGtbSizeOffset);
        sec.new_gtb_hash = read_u32_le(patch_bytes.data() + off + kArchiveNewGtbHashOffset);
        const std::uint32_t entry_count =
            read_u32_le(patch_bytes.data() + off + kArchiveEntryCountOffset);
        off += kArchiveFixedFieldsBytes;

        if (sec.new_gtb_size == 0 || sec.new_gtb_size != sec.orig_gtb_size) {
            throw std::runtime_error("Invalid GTB size in archive section");
        }
        require_bytes(sec.new_gtb_size, "masked_gtb");
        sec.masked_gtb.assign(
            patch_bytes.begin() + static_cast<std::ptrdiff_t>(off),
            patch_bytes.begin() + static_cast<std::ptrdiff_t>(off + sec.new_gtb_size));
        off += sec.new_gtb_size;

        sec.entries.reserve(entry_count);
        for (std::uint32_t e = 0; e < entry_count; ++e) {
            require_bytes(kArchiveEntryHeaderBytes, "archive entry header");
            ArchiveEntryRecord rec;
            rec.entry_index = read_u32_le(patch_bytes.data() + off + kEntryIndexOffset);
            rec.orig_offset = read_u64_le(patch_bytes.data() + off + kEntryOrigOffsetOffset);
            rec.orig_size = read_u32_le(patch_bytes.data() + off + kEntryOrigSizeOffset);
            rec.orig_data_hash = read_u32_le(patch_bytes.data() + off + kEntryOrigHashOffset);
            rec.new_size = read_u32_le(patch_bytes.data() + off + kEntryNewSizeOffset);
            rec.new_data_hash = read_u32_le(patch_bytes.data() + off + kEntryNewHashOffset);
            off += kArchiveEntryHeaderBytes;

            if (e > 0 && rec.entry_index <= sec.entries.back().entry_index) {
                throw std::runtime_error("Archive entry_index values are not strictly ascending");
            }
            if (rec.orig_offset + rec.orig_size > sec.orig_gpk_size) {
                throw std::runtime_error("Archive entry slice exceeds orig_gpk_size");
            }

            require_bytes(rec.new_size, "masked_entry");
            rec.masked_entry.assign(
                patch_bytes.begin() + static_cast<std::ptrdiff_t>(off),
                patch_bytes.begin() + static_cast<std::ptrdiff_t>(off + rec.new_size));
            off += rec.new_size;
            sec.entries.push_back(std::move(rec));
        }
        out.archives.push_back(std::move(sec));
    }

    if (off != patch_bytes.size()) {
        throw std::runtime_error("Trailing bytes after end of menu_patch.bin");
    }
    return out;
}

std::vector<std::uint8_t> reconstruct_standalone_file(
    const StandaloneGroupRecord& group,
    const std::vector<std::uint8_t>& orig_bytes) {
    const std::uint32_t orig_hash = ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size());
    for (const auto& var : group.variants) {
        if (orig_bytes.size() == var.orig_file_size && orig_hash == var.orig_data_hash) {
            std::vector<std::uint8_t> unmasked = var.masked_payload;
            XorMaskPayload(unmasked.data(),
                           unmasked.size(),
                           orig_bytes.data(),
                           orig_bytes.size(),
                           var.orig_data_hash);
            const std::uint32_t new_hash = ComputeMurmurHash2(unmasked.data(), unmasked.size());
            if (new_hash != var.new_data_hash) {
                throw std::runtime_error("Standalone file post-unmask hash mismatch for " +
                                         group.rel_path);
            }
            return unmasked;
        }
    }
    throw std::runtime_error("No matching standalone variant for " + group.rel_path);
}

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
reconstruct_archive_pair(const ArchiveSectionRecord& section,
                         const std::vector<std::uint8_t>& orig_gtb,
                         const std::vector<std::uint8_t>& orig_gpk) {
    if (orig_gtb.size() != section.orig_gtb_size) {
        throw std::runtime_error("Original GTB size mismatch for " + section.archive_stem);
    }
    if (orig_gpk.size() != section.orig_gpk_size) {
        throw std::runtime_error("Original GPK size mismatch for " + section.archive_stem);
    }
    const std::uint32_t gtb_hash = ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size());
    if (gtb_hash != section.orig_gtb_hash) {
        throw std::runtime_error("Original GTB hash mismatch for " + section.archive_stem);
    }

    std::vector<std::uint8_t> new_gtb = section.masked_gtb;
    XorMaskPayload(
        new_gtb.data(), new_gtb.size(), orig_gtb.data(), orig_gtb.size(), section.orig_gtb_hash);
    if (ComputeMurmurHash2(new_gtb.data(), new_gtb.size()) != section.new_gtb_hash) {
        throw std::runtime_error("Reconstructed GTB hash mismatch for " + section.archive_stem);
    }

    const ParsedGtbTable orig_table = parse_gtb_offsets(orig_gtb, section.orig_gpk_size);
    const ParsedGtbTable new_table = parse_gtb_offsets(new_gtb, section.new_gpk_size);
    if (orig_table.entry_count != new_table.entry_count) {
        throw std::runtime_error("GTB entry count changed during archive reconstruction");
    }

    std::vector<std::uint8_t> new_gpk;
    new_gpk.reserve(static_cast<std::size_t>(section.new_gpk_size));

    std::size_t patch_cursor = 0;
    for (std::uint32_t idx = 0; idx < orig_table.entry_count; ++idx) {
        if (new_gpk.size() != new_table.offsets[idx]) {
            throw std::runtime_error("Reconstructed GPK offset disagrees with new GTB table");
        }
        const std::uint64_t orig_off = orig_table.offsets[idx];
        const std::uint64_t orig_sz = orig_table.offsets[idx + 1] - orig_off;

        if (patch_cursor < section.entries.size() &&
            section.entries[patch_cursor].entry_index == idx) {
            const auto& rec = section.entries[patch_cursor++];
            if (rec.orig_offset != orig_off || rec.orig_size != orig_sz) {
                throw std::runtime_error("Patched entry orig_offset/orig_size mismatch");
            }
            const std::uint8_t* orig_ptr = orig_gpk.data() + static_cast<std::size_t>(orig_off);
            if (ComputeMurmurHash2(orig_ptr, static_cast<std::size_t>(orig_sz)) !=
                rec.orig_data_hash) {
                throw std::runtime_error("Patched entry original slice hash mismatch");
            }

            std::vector<std::uint8_t> unmasked = rec.masked_entry;
            XorMaskPayload(unmasked.data(),
                           unmasked.size(),
                           orig_ptr,
                           static_cast<std::size_t>(orig_sz),
                           rec.orig_data_hash);
            if (ComputeMurmurHash2(unmasked.data(), unmasked.size()) != rec.new_data_hash) {
                throw std::runtime_error("Patched entry post-unmask hash mismatch");
            }
            new_gpk.insert(new_gpk.end(), unmasked.begin(), unmasked.end());
        } else {
            new_gpk.insert(new_gpk.end(),
                           orig_gpk.begin() + static_cast<std::ptrdiff_t>(orig_off),
                           orig_gpk.begin() + static_cast<std::ptrdiff_t>(orig_off + orig_sz));
        }
    }

    if (patch_cursor != section.entries.size() || new_gpk.size() != section.new_gpk_size) {
        throw std::runtime_error("Reconstructed GPK total size mismatch for " +
                                 section.archive_stem);
    }
    return {std::move(new_gtb), std::move(new_gpk)};
}

VerifiedArchiveStreamResult verify_and_unmask_archive_from_disk(
    const ArchiveSectionRecord& section,
    const fs::path& orig_gtb_path,
    const fs::path& orig_gpk_path) {
    const std::vector<std::uint8_t> orig_gtb = read_binary_file(orig_gtb_path);
    if (orig_gtb.size() != section.orig_gtb_size) {
        throw std::runtime_error("On-disk GTB size mismatch for " + section.archive_stem);
    }
    if (ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size()) != section.orig_gtb_hash) {
        throw std::runtime_error("On-disk GTB hash mismatch for " + section.archive_stem);
    }

    std::error_code ec;
    const std::uintmax_t disk_gpk_size = fs::file_size(orig_gpk_path, ec);
    if (ec || disk_gpk_size != section.orig_gpk_size) {
        throw std::runtime_error("On-disk GPK size mismatch for " + section.archive_stem);
    }

    VerifiedArchiveStreamResult result;
    result.new_gtb = section.masked_gtb;
    XorMaskPayload(result.new_gtb.data(),
                   result.new_gtb.size(),
                   orig_gtb.data(),
                   orig_gtb.size(),
                   section.orig_gtb_hash);
    if (ComputeMurmurHash2(result.new_gtb.data(), result.new_gtb.size()) != section.new_gtb_hash) {
        throw std::runtime_error("Unmasked GTB hash mismatch for " + section.archive_stem);
    }

    const ParsedGtbTable orig_table = parse_gtb_offsets(orig_gtb, section.orig_gpk_size);
    const ParsedGtbTable new_table = parse_gtb_offsets(result.new_gtb, section.new_gpk_size);
    if (orig_table.entry_count != new_table.entry_count) {
        throw std::runtime_error("GTB entry count mismatch for " + section.archive_stem);
    }

    std::ifstream gpk_in(orig_gpk_path, std::ios::binary);
    if (!gpk_in) {
        throw std::runtime_error("Failed to open on-disk GPK: " + orig_gpk_path.u8string());
    }

    std::uint64_t running_new_off = 0;
    std::size_t cursor = 0;
    result.patched_entries.reserve(section.entries.size());

    for (std::uint32_t idx = 0; idx < orig_table.entry_count; ++idx) {
        if (new_table.offsets[idx] != running_new_off) {
            throw std::runtime_error("GTB offset mismatch at entry " + std::to_string(idx));
        }
        const std::uint64_t orig_off = orig_table.offsets[idx];
        const std::uint64_t orig_sz = orig_table.offsets[idx + 1] - orig_off;

        if (cursor < section.entries.size() && section.entries[cursor].entry_index == idx) {
            const auto& rec = section.entries[cursor++];
            if (rec.orig_offset != orig_off || rec.orig_size != orig_sz) {
                throw std::runtime_error("Entry slice metadata mismatch in " +
                                         section.archive_stem);
            }
            const auto orig_slice = read_file_slice(gpk_in, rec.orig_offset, rec.orig_size);
            if (ComputeMurmurHash2(orig_slice.data(), orig_slice.size()) != rec.orig_data_hash) {
                throw std::runtime_error("On-disk GPK slice hash mismatch in " +
                                         section.archive_stem);
            }

            std::vector<std::uint8_t> unmasked = rec.masked_entry;
            XorMaskPayload(unmasked.data(),
                           unmasked.size(),
                           orig_slice.data(),
                           orig_slice.size(),
                           rec.orig_data_hash);
            if (ComputeMurmurHash2(unmasked.data(), unmasked.size()) != rec.new_data_hash) {
                throw std::runtime_error("Unmasked DWQ hash mismatch in " + section.archive_stem);
            }

            result.patched_entries.push_back(
                {idx, running_new_off, rec.new_size, std::move(unmasked)});
            running_new_off += rec.new_size;
        } else {
            running_new_off += orig_sz;
        }
    }

    if (cursor != section.entries.size() || running_new_off != section.new_gpk_size) {
        throw std::runtime_error("Computed new_gpk_size mismatch for " + section.archive_stem);
    }
    result.computed_new_gpk_size = running_new_off;
    return result;
}

struct MenuPatchBuilder::Impl {
    explicit Impl(MenuPatchOptions opts) : options(std::move(opts)) {}

    MenuPatchOptions options;

    static StandaloneVariantRecord make_standalone_variant(
        const std::vector<std::uint8_t>& orig_bytes,
        const std::vector<std::uint8_t>& new_bytes) {
        StandaloneVariantRecord var;
        var.orig_file_size = static_cast<std::uint32_t>(orig_bytes.size());
        var.orig_data_hash = ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size());
        var.new_file_size = static_cast<std::uint32_t>(new_bytes.size());
        var.new_data_hash = ComputeMurmurHash2(new_bytes.data(), new_bytes.size());
        var.masked_payload = new_bytes;
        XorMaskPayload(var.masked_payload.data(),
                       var.masked_payload.size(),
                       orig_bytes.data(),
                       orig_bytes.size(),
                       var.orig_data_hash);
        return var;
    }

    std::vector<StandaloneGroupRecord> build_standalone_groups() const {
        std::vector<StandaloneGroupRecord> groups;
        groups.reserve(kExpectedStandaloneGroupCount);

        // 1. spt\charaname.xtx (plain CP932, shared HD10 + CE)
        {
            const fs::path rel = fs::u8path("spt") / fs::u8path("charaname.xtx");
            const auto hd10_raw = read_binary_file(options.hd10_game_dir / rel);
            const auto ce_raw = read_binary_file(options.ce_game_dir / rel);
            if (hd10_raw != ce_raw) {
                throw std::runtime_error("spt\\charaname.xtx differs between HD10 and CE inputs");
            }
            const std::string orig_cp932(reinterpret_cast<const char*>(hd10_raw.data()),
                                         hd10_raw.size());
            const std::string patched_cp932 = patch_charaname_xtx_cp932(orig_cp932);
            const std::vector<std::uint8_t> new_bytes(
                reinterpret_cast<const std::uint8_t*>(patched_cp932.data()),
                reinterpret_cast<const std::uint8_t*>(patched_cp932.data()) + patched_cp932.size());

            StandaloneGroupRecord grp;
            grp.rel_path = "spt\\charaname.xtx";
            grp.variants.push_back(make_standalone_variant(hd10_raw, new_bytes));
            groups.push_back(std::move(grp));
        }

        // 2. init2\backlog.fxf (bitwise-NOT CP932, shared HD10 + CE)
        {
            const fs::path rel = fs::u8path("init2") / fs::u8path("backlog.fxf");
            const auto hd10_raw = read_binary_file(options.hd10_game_dir / rel);
            const auto ce_raw = read_binary_file(options.ce_game_dir / rel);
            if (hd10_raw != ce_raw) {
                throw std::runtime_error("init2\\backlog.fxf differs between HD10 and CE inputs");
            }
            const std::string patched_cp932 = patch_backlog_fxf_cp932(decrypt_fxf(hd10_raw));
            const auto new_bytes = encrypt_fxf(patched_cp932);

            StandaloneGroupRecord grp;
            grp.rel_path = "init2\\backlog.fxf";
            grp.variants.push_back(make_standalone_variant(hd10_raw, new_bytes));
            groups.push_back(std::move(grp));
        }

        // 3. init2\mainControl.fxf (2 variants: HD10 + CE)
        {
            const fs::path rel = fs::u8path("init2") / fs::u8path("mainControl.fxf");
            const auto hd10_raw = read_binary_file(options.hd10_game_dir / rel);
            const auto ce_raw = read_binary_file(options.ce_game_dir / rel);
            if (hd10_raw == ce_raw) {
                throw std::runtime_error(
                    "HD10 and CE init2\\mainControl.fxf are identical; check --game-dir / --ce-game-dir");
            }

            const auto hd10_new = encrypt_fxf(patch_main_control_fxf_cp932(decrypt_fxf(hd10_raw)));
            const auto ce_new = encrypt_fxf(patch_main_control_fxf_cp932(decrypt_fxf(ce_raw)));

            StandaloneGroupRecord grp;
            grp.rel_path = "init2\\mainControl.fxf";
            grp.variants.push_back(make_standalone_variant(hd10_raw, hd10_new));
            grp.variants.push_back(make_standalone_variant(ce_raw, ce_new));
            groups.push_back(std::move(grp));
        }

        return groups;
    }

    struct PreparedSprite {
        std::uint32_t entry_index = 0;
        std::uint64_t orig_offset = 0;
        std::vector<std::uint8_t> orig_dwq;
        std::uint32_t orig_hash = 0;
        RgbaImage rendered_rgba;
        std::vector<std::uint8_t> base_png;
        bool recompute_crop = false;
        int chosen_salt = 0;
    };

    static ArchiveEntryRecord build_sanitized_entry_record(const PreparedSprite& sp,
                                                           int min_salt) {
        for (int salt = min_salt; salt < kMaxSaltAttempts; ++salt) {
            const auto png = insert_png_text_salt_chunk(sp.base_png, salt);
            const auto new_dwq = wrap_dwq_png_preserving_header(
                sp.orig_dwq, png, sp.recompute_crop, &sp.rendered_rgba);

            ArchiveEntryRecord rec;
            rec.entry_index = sp.entry_index;
            rec.orig_offset = sp.orig_offset;
            rec.orig_size = static_cast<std::uint32_t>(sp.orig_dwq.size());
            rec.orig_data_hash = sp.orig_hash;
            rec.new_size = static_cast<std::uint32_t>(new_dwq.size());
            rec.new_data_hash = ComputeMurmurHash2(new_dwq.data(), new_dwq.size());
            rec.masked_entry = new_dwq;
            XorMaskPayload(rec.masked_entry.data(),
                           rec.masked_entry.size(),
                           sp.orig_dwq.data(),
                           sp.orig_dwq.size(),
                           rec.orig_data_hash);

            std::vector<std::uint8_t> wire_entry;
            wire_entry.reserve(kArchiveEntryHeaderBytes + rec.masked_entry.size());
            append_u32_le(wire_entry, rec.entry_index);
            append_u64_le(wire_entry, rec.orig_offset);
            append_u32_le(wire_entry, rec.orig_size);
            append_u32_le(wire_entry, rec.orig_data_hash);
            append_u32_le(wire_entry, rec.new_size);
            append_u32_le(wire_entry, rec.new_data_hash);
            wire_entry.insert(wire_entry.end(), rec.masked_entry.begin(), rec.masked_entry.end());

            if (!contains_forbidden_raw_signature(wire_entry.data(), wire_entry.size())) {
                return rec;
            }
        }
        throw std::runtime_error("Failed to find clean salt for entry " +
                                 std::to_string(sp.entry_index));
    }

    template <std::size_t N, typename RenderFn>
    ArchiveSectionRecord build_archive_section(const char* archive_stem,
                                               const std::array<std::uint32_t, N>& target_indices,
                                               bool write_24bpp_rgb,
                                               RenderFn&& render_fn) const {
        const fs::path rel_stem = fs::u8path(archive_stem);
        const fs::path hd10_gtb_path =
            options.hd10_game_dir / fs::u8path(rel_stem.u8string() + ".gtb");
        const fs::path ce_gtb_path =
            options.ce_game_dir / fs::u8path(rel_stem.u8string() + ".gtb");
        const fs::path hd10_gpk_path =
            options.hd10_game_dir / fs::u8path(rel_stem.u8string() + ".gpk");
        const fs::path ce_gpk_path =
            options.ce_game_dir / fs::u8path(rel_stem.u8string() + ".gpk");

        const auto orig_gtb = read_binary_file(hd10_gtb_path);
        const auto ce_gtb = read_binary_file(ce_gtb_path);
        if (orig_gtb != ce_gtb) {
            throw std::runtime_error(std::string("GTB differs between HD10 and CE for ") +
                                     archive_stem);
        }

        const std::uint64_t orig_gpk_size = fs::file_size(hd10_gpk_path);
        if (fs::file_size(ce_gpk_path) != orig_gpk_size) {
            throw std::runtime_error(std::string("GPK size differs between HD10 and CE for ") +
                                     archive_stem);
        }

        const ParsedGtbTable orig_table = parse_gtb_offsets(orig_gtb, orig_gpk_size);
        std::ifstream gpk_in(hd10_gpk_path, std::ios::binary);
        if (!gpk_in) {
            throw std::runtime_error("Failed to open GPK archive: " + hd10_gpk_path.u8string());
        }

        auto load_decoded_dwq = [&](std::uint32_t idx) -> std::pair<std::vector<std::uint8_t>, RgbaImage> {
            if (idx >= orig_table.entry_count) {
                throw std::runtime_error("GPK entry index out of range");
            }
            const std::uint64_t off = orig_table.offsets[idx];
            const std::uint32_t sz =
                static_cast<std::uint32_t>(orig_table.offsets[idx + 1] - off);
            auto raw = read_file_slice(gpk_in, off, sz);
            if (raw.size() <= kDwqHeaderSize) {
                throw std::runtime_error("DWQ slice too small to hold embedded PNG");
            }
            const std::uint32_t png_sz = read_u32_le(raw.data() + kDwqDataSizeOffset);
            if (kDwqHeaderSize + png_sz > raw.size()) {
                throw std::runtime_error("DWQ embedded PNG size overflows entry slice");
            }
            RgbaImage img = decode_png_rgba(raw.data() + kDwqHeaderSize, png_sz);
            return {std::move(raw), std::move(img)};
        };

        auto load_plate_image = [&](std::uint32_t idx) -> RgbaImage {
            return load_decoded_dwq(idx).second;
        };

        std::vector<PreparedSprite> prepared;
        prepared.reserve(N);
        for (std::uint32_t idx : target_indices) {
            auto [raw_dwq, decoded] = load_decoded_dwq(idx);
            const bool recompute_crop = render_fn(idx, decoded, load_plate_image);
            auto encoded_png = encode_png(decoded, write_24bpp_rgb);

            PreparedSprite sp;
            sp.entry_index = idx;
            sp.orig_offset = orig_table.offsets[idx];
            sp.orig_hash = ComputeMurmurHash2(raw_dwq.data(), raw_dwq.size());
            sp.orig_dwq = std::move(raw_dwq);
            sp.rendered_rgba = std::move(decoded);
            sp.base_png = std::move(encoded_png);
            sp.recompute_crop = recompute_crop;
            prepared.push_back(std::move(sp));
        }

        // Assemble section and verify zero forbidden raw signatures across section header,
        // masked_gtb, and all masked entries (including record boundaries).
        for (int outer_salt = 0; outer_salt < kMaxSaltAttempts; ++outer_salt) {
            ArchiveSectionRecord sec;
            sec.archive_stem = archive_stem;
            sec.orig_gpk_size = orig_gpk_size;
            sec.orig_gtb_size = static_cast<std::uint32_t>(orig_gtb.size());
            sec.orig_gtb_hash = ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size());
            sec.entries.reserve(prepared.size());

            for (std::size_t i = 0; i < prepared.size(); ++i) {
                const int min_salt = (i == 0) ? outer_salt : 0;
                sec.entries.push_back(build_sanitized_entry_record(prepared[i], min_salt));
            }

            std::vector<std::uint64_t> new_offsets(orig_table.entry_count, 0u);
            std::uint64_t running_off = 0;
            std::size_t cursor = 0;
            for (std::uint32_t idx = 0; idx < orig_table.entry_count; ++idx) {
                new_offsets[idx] = running_off;
                if (cursor < sec.entries.size() && sec.entries[cursor].entry_index == idx) {
                    running_off += sec.entries[cursor++].new_size;
                    continue;
                }
                running_off += (orig_table.offsets[idx + 1] - orig_table.offsets[idx]);
            }
            sec.new_gpk_size = running_off;

            auto new_gtb = update_gtb_offsets(orig_gtb, new_offsets);
            sec.new_gtb_size = static_cast<std::uint32_t>(new_gtb.size());
            sec.new_gtb_hash = ComputeMurmurHash2(new_gtb.data(), new_gtb.size());
            sec.masked_gtb = std::move(new_gtb);
            XorMaskPayload(sec.masked_gtb.data(),
                           sec.masked_gtb.size(),
                           orig_gtb.data(),
                           orig_gtb.size(),
                           sec.orig_gtb_hash);

            std::vector<std::uint8_t> serialized_sec;
            serialize_archive_section_into(serialized_sec, sec);
            if (!contains_forbidden_raw_signature(serialized_sec.data(), serialized_sec.size())) {
                return sec;
            }
        }
        throw std::runtime_error(std::string("Failed to sanitize archive section ") + archive_stem);
    }

    MenuPatchSummary build() const {
        ParsedMenuPatchBin patch;
        patch.version = kMenuPatchFormatVersion;
        patch.flags = 0u;
        patch.reserved = 0u;
        patch.standalone_groups = build_standalone_groups();

        constexpr std::uint32_t kSys0KyouDonor01Index = 217u;
        constexpr std::uint32_t kSys0KyouDonor02Index = 219u;
        std::optional<Sys0HelperPlates> sys0_helpers;
        patch.archives.push_back(build_archive_section(
            "dwq\\sys0",
            kSys0PatchedIndices,
            false,
            [&](std::uint32_t idx, RgbaImage& img, auto&& load_plate) {
                if (!sys0_helpers.has_value()) {
                    Sys0HelperPlates hp;
                    hp.clean_load_base = load_plate(kSys0CleanLoadBaseIndex);
                    hp.kyou_donor_01 = load_plate(kSys0KyouDonor01Index);
                    hp.kyou_donor_02 = load_plate(kSys0KyouDonor02Index);
                    sys0_helpers = std::move(hp);
                }
                return render_sys0_entry(idx, img, *sys0_helpers);
            }));

        std::optional<RgbaImage> ta0_clean_plate;
        patch.archives.push_back(build_archive_section(
            "dwq\\ta0",
            kTa0PatchedIndices,
            false,
            [&](std::uint32_t idx, RgbaImage& img, auto&& load_plate) {
                if (!ta0_clean_plate.has_value()) {
                    ta0_clean_plate = load_plate(kTa0CleanTitlePlateIndex);
                }
                return render_ta0_entry(idx, img, *ta0_clean_plate);
            }));

        patch.archives.push_back(build_archive_section(
            "dwq\\ev0",
            kEv0PatchedIndices,
            true,
            [](std::uint32_t idx, RgbaImage& img, auto&&) {
                render_ev0_entry(idx, img);
                return false;
            }));

        const auto wire_bytes = serialize_menu_patch_bin(patch);
        if (contains_forbidden_raw_signature(wire_bytes.data(), wire_bytes.size())) {
            throw std::runtime_error(
                "Serialized menu_patch.bin unexpectedly contains a raw header signature");
        }

        // Verify parse round-trip before writing output file.
        const auto roundtrip = parse_menu_patch_bin(wire_bytes);
        if (roundtrip.standalone_groups.size() != kExpectedStandaloneGroupCount ||
            roundtrip.archives.size() != kExpectedArchiveCount) {
            throw std::runtime_error("menu_patch.bin self-validation failed");
        }

        write_binary_file(options.output_patch_bin, wire_bytes);

        MenuPatchSummary summary;
        summary.output_patch_bin = options.output_patch_bin;
        summary.patch_bin_bytes = wire_bytes.size();
        summary.standalone_group_count =
            static_cast<std::uint32_t>(patch.standalone_groups.size());
        for (const auto& grp : patch.standalone_groups) {
            summary.standalone_variant_count += static_cast<std::uint32_t>(grp.variants.size());
        }
        summary.archive_count = static_cast<std::uint32_t>(patch.archives.size());
        for (const auto& sec : patch.archives) {
            summary.patched_entry_count += static_cast<std::uint32_t>(sec.entries.size());
        }
        return summary;
    }
};

MenuPatchBuilder::MenuPatchBuilder(MenuPatchOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

MenuPatchBuilder::~MenuPatchBuilder() = default;

MenuPatchBuilder::MenuPatchBuilder(MenuPatchBuilder&&) noexcept = default;

MenuPatchBuilder& MenuPatchBuilder::operator=(MenuPatchBuilder&&) noexcept = default;

MenuPatchSummary MenuPatchBuilder::build() {
    return impl_->build();
}

}  // namespace mgi::menu
