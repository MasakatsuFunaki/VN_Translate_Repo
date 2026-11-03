// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal PE32 reader for the kishin_hisho RE tooling.
//
// Only what the six analysis tools actually touch: the file/optional headers,
// the section table, the import and TLS directories, and -- critically -- the
// memory-mapped image view, whose exact layout the tools index into by RVA.
//
// Parsed state and the lazily-built caches live behind a Pimpl so the header
// stays a pure interface.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/util.h"

namespace ksh::pe {

// Behaviourless aggregates -- public data is the point (item 41).
struct Section {
    std::string name;  // trailing NULs stripped; empty for an anonymous section
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t pointer_to_raw_data = 0;
    std::uint32_t size_of_raw_data = 0;
    std::uint32_t characteristics = 0;
};

struct ImportedSymbol {
    std::string name;  // empty when imported by ordinal
    std::uint16_t ordinal = 0;
};

struct ImportedDll {
    std::string dll;
    std::vector<ImportedSymbol> imports;
};

struct TlsDirectory {
    std::uint32_t start_address_of_raw_data = 0;
    std::uint32_t end_address_of_raw_data = 0;
    std::uint32_t address_of_index = 0;
    std::uint32_t address_of_callbacks = 0;
};

// Data-directory indices used by the tools.
enum class Directory { Export = 0, Import = 1, Tls = 9 };

class Image {
public:
    // Throws std::runtime_error when `raw` is not a PE32 image.
    explicit Image(Bytes raw);
    ~Image();
    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    std::uint16_t machine() const;
    std::uint16_t number_of_sections() const;
    std::uint32_t time_date_stamp() const;
    std::uint16_t characteristics() const;
    std::uint32_t image_base() const;
    std::uint32_t size_of_image() const;
    std::uint32_t address_of_entry_point() const;
    std::uint16_t dll_characteristics() const;
    std::uint32_t section_alignment() const;
    std::uint32_t file_alignment() const;

    const std::vector<Section>& sections() const;
    const Bytes& raw() const;

    std::uint32_t directory_rva(Directory d) const;
    std::uint32_t directory_size(Directory d) const;

    // The image laid out as the loader would map it.  The analysis tools index
    // this buffer directly by RVA, so any deviation from the layout built in
    // pe_image.cpp shifts every read they make.
    const Bytes& memory_mapped_image() const;

    // Empty when the PE has no import directory at all -- which is itself the
    // AlphaROM tell that 01_recon reports on.
    const std::vector<ImportedDll>& imports() const;

    // nullopt when there is no TLS directory.
    const std::optional<TlsDirectory>& tls() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Section-alignment adjustment applied when mapping.  The rule is exactly two
// steps: a sub-page SectionAlignment falls back to FileAlignment, then the value
// is rounded DOWN to a multiple of it.  There is deliberately NO
// "FileAlignment < 0x200" early-out -- adding one shifts the mapped layout, and
// with it every RVA the tools read.
std::uint32_t adjust_section_alignment(std::uint32_t val, std::uint32_t section_alignment,
                                       std::uint32_t file_alignment);

// Little-endian scalar reads that yield 0 past the end rather than throwing: the
// tools walk truncated dumps and hostile PE headers, where a short read means
// "field absent", not "abort the report".
std::uint32_t rd_u32(const Bytes& b, std::size_t off);
std::uint16_t rd_u16(const Bytes& b, std::size_t off);
std::uint64_t rd_u64(const Bytes& b, std::size_t off);

// NUL-terminated ASCII at `off`, clipped at the buffer end.
std::string rd_cstr(const Bytes& b, std::size_t off);

}  // namespace ksh::pe
