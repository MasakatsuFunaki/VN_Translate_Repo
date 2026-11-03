// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "pe_image.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ksh::pe {

namespace {

// ---- PE layout constants ---------------------------------------------------
// Offsets are from the start of the structure named in the comment, so each
// read below states what it is reading rather than a bare number.

constexpr std::size_t kDosLfanewOff = 0x3C;   // IMAGE_DOS_HEADER::e_lfanew
constexpr std::size_t kPeSigSize = 4;         // "PE\0\0"

// IMAGE_FILE_HEADER
constexpr std::size_t kFhMachine = 0;
constexpr std::size_t kFhNumberOfSections = 2;
constexpr std::size_t kFhTimeDateStamp = 4;
constexpr std::size_t kFhSizeOfOptionalHeader = 16;
constexpr std::size_t kFhCharacteristics = 18;
constexpr std::size_t kFileHeaderSize = 20;

// IMAGE_OPTIONAL_HEADER32
constexpr std::size_t kOhMagic = 0;
constexpr std::size_t kOhAddressOfEntryPoint = 16;
constexpr std::size_t kOhImageBase = 28;
constexpr std::size_t kOhSectionAlignment = 32;
constexpr std::size_t kOhFileAlignment = 36;
constexpr std::size_t kOhSizeOfImage = 56;
constexpr std::size_t kOhDllCharacteristics = 70;
constexpr std::size_t kOhNumberOfRvaAndSizes = 92;
constexpr std::size_t kOhDataDirectory = 96;
constexpr std::size_t kDataDirEntrySize = 8;
constexpr std::uint16_t kPe32Magic = 0x10B;

// IMAGE_SECTION_HEADER
constexpr std::size_t kSecNameSize = 8;
constexpr std::size_t kSecVirtualSize = 8;
constexpr std::size_t kSecVirtualAddress = 12;
constexpr std::size_t kSecSizeOfRawData = 16;
constexpr std::size_t kSecPointerToRawData = 20;
constexpr std::size_t kSecCharacteristics = 36;
constexpr std::size_t kSectionHeaderSize = 40;

// IMAGE_IMPORT_DESCRIPTOR
constexpr std::size_t kImpOriginalFirstThunk = 0;
constexpr std::size_t kImpName = 12;
constexpr std::size_t kImpFirstThunk = 16;
constexpr std::size_t kImportDescriptorSize = 20;
constexpr std::uint32_t kOrdinalFlag32 = 0x80000000u;
constexpr std::size_t kHintNameSkipHint = 2;

// IMAGE_TLS_DIRECTORY32
constexpr std::size_t kTlsStartAddressOfRawData = 0;
constexpr std::size_t kTlsEndAddressOfRawData = 4;
constexpr std::size_t kTlsAddressOfIndex = 8;
constexpr std::size_t kTlsAddressOfCallBacks = 12;

// A section whose adjusted VA reaches this is skipped when mapping.
constexpr std::uint32_t kMappedVaCeiling = 0x10000000;
constexpr std::uint32_t kPageAlignment = 0x1000;

constexpr std::size_t kNoOffset = static_cast<std::size_t>(-1);

// RVA -> file offset, using the section table.
std::size_t rva_to_offset(const std::vector<Section>& sections, std::uint32_t rva) {
    for (const auto& s : sections) {
        const std::uint32_t size = s.size_of_raw_data ? s.size_of_raw_data : s.virtual_size;
        if (rva >= s.virtual_address && rva < s.virtual_address + size)
            return s.pointer_to_raw_data + (rva - s.virtual_address);
    }
    return kNoOffset;
}

}  // namespace

// ---- free helpers ----------------------------------------------------------

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::uint16_t rd_u16(const Bytes& b, std::size_t off) {
    if (off + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

std::uint64_t rd_u64(const Bytes& b, std::size_t off) {
    return static_cast<std::uint64_t>(rd_u32(b, off)) |
           (static_cast<std::uint64_t>(rd_u32(b, off + 4)) << 32);
}

std::string rd_cstr(const Bytes& b, std::size_t off) {
    std::string out;
    while (off < b.size() && b[off] != 0) out += static_cast<char>(b[off++]);
    return out;
}

std::uint32_t adjust_section_alignment(std::uint32_t val, std::uint32_t section_alignment,
                                       std::uint32_t file_alignment) {
    // A sub-page SectionAlignment falls back to FileAlignment, then the value is
    // rounded DOWN to a multiple of it.  Those two steps and nothing else.
    std::uint32_t sa = section_alignment;
    if (sa < kPageAlignment) sa = file_alignment;
    if (sa && (val % sa)) return sa * (val / sa);
    return val;
}

// ---- Image::Impl -----------------------------------------------------------

struct Image::Impl {
    Bytes raw;
    std::size_t pe_offset = 0;
    std::size_t opt_offset = 0;
    std::uint16_t machine = 0;
    std::uint16_t number_of_sections = 0;
    std::uint16_t characteristics = 0;
    std::uint16_t dll_characteristics = 0;
    std::uint16_t size_of_optional_header = 0;
    std::uint32_t time_date_stamp = 0;
    std::uint32_t image_base = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t address_of_entry_point = 0;
    std::uint32_t section_alignment = 0;
    std::uint32_t file_alignment = 0;
    std::uint32_t number_of_rva_and_sizes = 0;
    std::vector<Section> sections;

    // Built on first use; const accessors populate them.
    mutable std::optional<Bytes> mapped;
    mutable std::optional<std::vector<ImportedDll>> imports;
    mutable bool tls_built = false;
    mutable std::optional<TlsDirectory> tls;

    explicit Impl(Bytes bytes);

    std::uint32_t directory_field(Directory d, std::size_t field_off) const {
        const auto i = static_cast<std::uint32_t>(d);
        if (i >= number_of_rva_and_sizes) return 0;
        return rd_u32(raw, opt_offset + kOhDataDirectory + i * kDataDirEntrySize + field_off);
    }
};

Image::Impl::Impl(Bytes bytes) : raw(std::move(bytes)) {
    if (raw.size() < 0x40 || raw[0] != 'M' || raw[1] != 'Z')
        throw std::runtime_error("not a PE file (no MZ)");
    pe_offset = rd_u32(raw, kDosLfanewOff);
    if (pe_offset + kPeSigSize + kFileHeaderSize > raw.size() ||
        std::memcmp(raw.data() + pe_offset, "PE\0\0", kPeSigSize) != 0)
        throw std::runtime_error("not a PE file (no PE signature)");

    const std::size_t fh = pe_offset + kPeSigSize;
    machine = rd_u16(raw, fh + kFhMachine);
    number_of_sections = rd_u16(raw, fh + kFhNumberOfSections);
    time_date_stamp = rd_u32(raw, fh + kFhTimeDateStamp);
    size_of_optional_header = rd_u16(raw, fh + kFhSizeOfOptionalHeader);
    characteristics = rd_u16(raw, fh + kFhCharacteristics);

    opt_offset = fh + kFileHeaderSize;
    if (rd_u16(raw, opt_offset + kOhMagic) != kPe32Magic)
        throw std::runtime_error("only PE32 is supported");

    address_of_entry_point = rd_u32(raw, opt_offset + kOhAddressOfEntryPoint);
    image_base = rd_u32(raw, opt_offset + kOhImageBase);
    section_alignment = rd_u32(raw, opt_offset + kOhSectionAlignment);
    file_alignment = rd_u32(raw, opt_offset + kOhFileAlignment);
    size_of_image = rd_u32(raw, opt_offset + kOhSizeOfImage);
    dll_characteristics = rd_u16(raw, opt_offset + kOhDllCharacteristics);
    number_of_rva_and_sizes = rd_u32(raw, opt_offset + kOhNumberOfRvaAndSizes);

    const std::size_t sec_off = opt_offset + size_of_optional_header;
    sections.reserve(number_of_sections);
    for (std::uint16_t i = 0; i < number_of_sections; ++i) {
        const std::size_t o = sec_off + static_cast<std::size_t>(i) * kSectionHeaderSize;
        if (o + kSectionHeaderSize > raw.size()) break;
        Section s;
        // The name field is 8 bytes, NUL-padded and NOT necessarily terminated.
        std::size_t n = 0;
        while (n < kSecNameSize && raw[o + n] != 0) ++n;
        s.name.assign(reinterpret_cast<const char*>(raw.data() + o), n);
        s.virtual_size = rd_u32(raw, o + kSecVirtualSize);
        s.virtual_address = rd_u32(raw, o + kSecVirtualAddress);
        s.size_of_raw_data = rd_u32(raw, o + kSecSizeOfRawData);
        s.pointer_to_raw_data = rd_u32(raw, o + kSecPointerToRawData);
        s.characteristics = rd_u32(raw, o + kSecCharacteristics);
        sections.push_back(std::move(s));
    }
}

// ---- Image -----------------------------------------------------------------

Image::Image(Bytes raw) : impl_(std::make_unique<Impl>(std::move(raw))) {}
Image::~Image() = default;
Image::Image(Image&&) noexcept = default;
Image& Image::operator=(Image&&) noexcept = default;

std::uint16_t Image::machine() const { return impl_->machine; }
std::uint16_t Image::number_of_sections() const { return impl_->number_of_sections; }
std::uint32_t Image::time_date_stamp() const { return impl_->time_date_stamp; }
std::uint16_t Image::characteristics() const { return impl_->characteristics; }
std::uint32_t Image::image_base() const { return impl_->image_base; }
std::uint32_t Image::size_of_image() const { return impl_->size_of_image; }
std::uint32_t Image::address_of_entry_point() const { return impl_->address_of_entry_point; }
std::uint16_t Image::dll_characteristics() const { return impl_->dll_characteristics; }
std::uint32_t Image::section_alignment() const { return impl_->section_alignment; }
std::uint32_t Image::file_alignment() const { return impl_->file_alignment; }
const std::vector<Section>& Image::sections() const { return impl_->sections; }
const Bytes& Image::raw() const { return impl_->raw; }

std::uint32_t Image::directory_rva(Directory d) const { return impl_->directory_field(d, 0); }
std::uint32_t Image::directory_size(Directory d) const { return impl_->directory_field(d, 4); }

const Bytes& Image::memory_mapped_image() const {
    if (impl_->mapped) return *impl_->mapped;

    // Start from the WHOLE FILE, then for each section in order, pad-or-truncate
    // to its adjusted VirtualAddress and append its raw data.  For a normal PE
    // the net effect is the header page followed by each section at its VA --
    // which is why the tools can index this buffer directly by RVA.
    Bytes mapped = impl_->raw;
    for (const auto& s : impl_->sections) {
        const std::uint32_t va = adjust_section_alignment(
            s.virtual_address, impl_->section_alignment, impl_->file_alignment);
        if (va >= kMappedVaCeiling) continue;
        mapped.resize(va, 0);  // pads when short, truncates when long

        std::size_t start = std::min<std::size_t>(s.pointer_to_raw_data, impl_->raw.size());
        std::size_t end = std::min<std::size_t>(
            static_cast<std::size_t>(s.pointer_to_raw_data) + s.size_of_raw_data,
            impl_->raw.size());
        if (end > start)
            mapped.insert(mapped.end(),
                          impl_->raw.begin() + static_cast<std::ptrdiff_t>(start),
                          impl_->raw.begin() + static_cast<std::ptrdiff_t>(end));
    }
    impl_->mapped = std::move(mapped);
    return *impl_->mapped;
}

const std::vector<ImportedDll>& Image::imports() const {
    if (impl_->imports) return *impl_->imports;
    std::vector<ImportedDll> out;

    const std::uint32_t dir = directory_rva(Directory::Import);
    std::size_t off = dir ? rva_to_offset(impl_->sections, dir) : kNoOffset;
    // No import directory at all is the AlphaROM tell 01_recon reports on.
    for (; off != kNoOffset; off += kImportDescriptorSize) {
        const std::uint32_t original_first_thunk = rd_u32(impl_->raw, off + kImpOriginalFirstThunk);
        const std::uint32_t name_rva = rd_u32(impl_->raw, off + kImpName);
        const std::uint32_t first_thunk = rd_u32(impl_->raw, off + kImpFirstThunk);
        if (!original_first_thunk && !name_rva && !first_thunk) break;

        ImportedDll d;
        const std::size_t name_off = rva_to_offset(impl_->sections, name_rva);
        if (name_off != kNoOffset) d.dll = rd_cstr(impl_->raw, name_off);

        const std::uint32_t thunk_rva = original_first_thunk ? original_first_thunk : first_thunk;
        for (std::size_t t = rva_to_offset(impl_->sections, thunk_rva); t != kNoOffset; t += 4) {
            const std::uint32_t entry = rd_u32(impl_->raw, t);
            if (!entry) break;
            ImportedSymbol sym;
            if (entry & kOrdinalFlag32) {
                sym.ordinal = static_cast<std::uint16_t>(entry & 0xFFFF);
            } else {
                // IMAGE_IMPORT_BY_NAME: u16 hint then the ASCII name.
                const std::size_t h = rva_to_offset(impl_->sections, entry);
                if (h != kNoOffset) sym.name = rd_cstr(impl_->raw, h + kHintNameSkipHint);
            }
            d.imports.push_back(std::move(sym));
        }
        out.push_back(std::move(d));
    }

    impl_->imports = std::move(out);
    return *impl_->imports;
}

const std::optional<TlsDirectory>& Image::tls() const {
    if (impl_->tls_built) return impl_->tls;
    impl_->tls_built = true;

    const std::uint32_t dir = directory_rva(Directory::Tls);
    const std::size_t off = dir ? rva_to_offset(impl_->sections, dir) : kNoOffset;
    if (off == kNoOffset) return impl_->tls;

    TlsDirectory t;
    t.start_address_of_raw_data = rd_u32(impl_->raw, off + kTlsStartAddressOfRawData);
    t.end_address_of_raw_data = rd_u32(impl_->raw, off + kTlsEndAddressOfRawData);
    t.address_of_index = rd_u32(impl_->raw, off + kTlsAddressOfIndex);
    t.address_of_callbacks = rd_u32(impl_->raw, off + kTlsAddressOfCallBacks);
    impl_->tls = t;
    return impl_->tls;
}

}  // namespace ksh::pe
