// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// PE reader tests, pinned against the committed sample.
//
// The values below are the ones analysis/logs/01_recon.txt records, so if the
// parser drifts these fail before the log comparison does -- and they name the
// field, which a whole-file diff does not.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/util.h"
#include "pe/disasm.h"
#include "pe/pe_image.h"

namespace fs = std::filesystem;
using namespace ksh;

namespace {

std::string sample_path() {
    return std::string(KSH_PROJECT_DIR) + "\\analysis\\samples\\KishinHisho_orig.exe";
}

bool have_sample() { return fs::exists(fs::u8path(sample_path())); }

#define REQUIRE_SAMPLE() \
    if (!have_sample()) GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present"

}  // namespace

// The rule is the sub-page fallback plus the round-down, and nothing else: an
// extra "FileAlignment < 0x200" early-out would shift the whole mapped layout.
TEST(PeImage, adjust_section_alignment_is_fallback_then_round_down) {
    // Page-aligned SectionAlignment: value already aligned -> unchanged.
    EXPECT_EQ(pe::adjust_section_alignment(0x1000, 0x1000, 0x200), 0x1000u);
    // Not aligned -> rounded DOWN to a multiple.
    EXPECT_EQ(pe::adjust_section_alignment(0x1234, 0x1000, 0x200), 0x1000u);
    // Sub-page SectionAlignment falls back to FileAlignment.
    EXPECT_EQ(pe::adjust_section_alignment(0x250, 0x200, 0x200), 0x200u);
    // A tiny FileAlignment is honoured, NOT short-circuited.
    EXPECT_EQ(pe::adjust_section_alignment(0x1FF, 0x100, 0x100), 0x100u);
    // Zero alignment cannot divide -- pass through rather than fault.
    EXPECT_EQ(pe::adjust_section_alignment(0x1234, 0, 0), 0x1234u);
}

TEST(PeImage, reads_headers_of_the_sample) {
    REQUIRE_SAMPLE();
    pe::Image pe(read_file(sample_path()));
    EXPECT_EQ(pe.machine(), 0x014C);
    EXPECT_EQ(pe.number_of_sections(), 5);
    EXPECT_EQ(pe.time_date_stamp(), 0x4458CC07u);
    EXPECT_EQ(pe.characteristics(), 0x010F);
    EXPECT_EQ(pe.image_base(), 0x00400000u);
    EXPECT_EQ(pe.size_of_image(), 0x0004E000u);
    EXPECT_EQ(pe.address_of_entry_point(), 0x00028000u);
    EXPECT_EQ(pe.dll_characteristics(), 0x0000);
}

// The AlphaROM repack signature: four anonymous RW sections holding the
// encrypted original, with the stub's .text appended LAST.
TEST(PeImage, section_table_shows_the_alpharom_layout) {
    REQUIRE_SAMPLE();
    pe::Image pe(read_file(sample_path()));
    const auto& s = pe.sections();
    ASSERT_EQ(s.size(), 5u);
    for (std::size_t i = 0; i + 1 < s.size(); ++i)
        EXPECT_TRUE(s[i].name.empty()) << "section " << i << " should be anonymous";
    EXPECT_EQ(s.back().name, ".text");
    EXPECT_EQ(s.back().virtual_address, 0x00028000u);
    EXPECT_EQ(s.back().pointer_to_raw_data, 0x00027000u);
}

// Only four imports -- everything else is resolved at runtime by the stub.
TEST(PeImage, imports_are_the_four_the_stub_leaves_behind) {
    REQUIRE_SAMPLE();
    pe::Image pe(read_file(sample_path()));
    std::size_t total = 0;
    for (const auto& d : pe.imports()) total += d.imports.size();
    EXPECT_EQ(total, 4u);
    bool saw_lopen = false;
    for (const auto& d : pe.imports())
        for (const auto& f : d.imports)
            if (f.name == "_lopen") saw_lopen = true;
    EXPECT_TRUE(saw_lopen);
}

TEST(PeImage, sample_has_no_tls_directory) {
    REQUIRE_SAMPLE();
    pe::Image pe(read_file(sample_path()));
    EXPECT_FALSE(pe.tls().has_value());
}

// The whole point of the mapped view: the tools index the result by RVA, so
// mapped[0x282C5] must be file[0x272C5].
TEST(PeImage, memory_mapped_image_is_rva_indexable) {
    REQUIRE_SAMPLE();
    pe::Image pe(read_file(sample_path()));
    const Bytes& mapped = pe.memory_mapped_image();
    const Bytes& raw = pe.raw();
    ASSERT_EQ(mapped.size(), 0x4E000u) << "mapped size must equal SizeOfImage";
    ASSERT_GT(raw.size(), 0x272C5u + 0x49);
    for (std::size_t i = 0; i < 0x49; ++i)
        EXPECT_EQ(mapped[0x282C5 + i], raw[0x272C5 + i]) << "at " << i;
    for (std::size_t i = 0; i < 0x100; ++i)
        EXPECT_EQ(mapped[0x28000 + i], raw[0x27000 + i]) << "at " << i;
}

// The anti-debug device-name table, decrypted with the stub's `add 0x44`.
TEST(PeImage, device_name_table_decrypts_to_the_known_strings) {
    REQUIRE_SAMPLE();
    pe::Image pe(read_file(sample_path()));
    const Bytes& mapped = pe.memory_mapped_image();
    std::string decoded;
    for (std::size_t i = 0; i < 0x49; ++i)
        decoded += static_cast<char>((mapped[0x28000 + 0x2C5 + i] + 0x44) & 0xFF);
    EXPECT_NE(decoded.find("\\\\.\\TRW"), std::string::npos);
    EXPECT_NE(decoded.find("\\\\.\\SICE"), std::string::npos);
    EXPECT_NE(decoded.find("\\\\.\\NTICE"), std::string::npos);
    EXPECT_NE(decoded.find("\\\\.\\REGMON"), std::string::npos);
}

TEST(PeImage, disassembles_the_entry_point) {
    REQUIRE_SAMPLE();
    pe::Image pe(read_file(sample_path()));
    const pe::Disassembler md;
    const Bytes& mapped = pe.memory_mapped_image();
    const auto ins = md.disasm(mapped.data() + 0x28000, 256, 0x00428000);
    ASSERT_FALSE(ins.empty());
    EXPECT_EQ(ins.front().address, 0x00428000u);
    for (const auto& i : ins) EXPECT_FALSE(i.mnemonic.empty());
}

TEST(PeImage, rejects_non_pe_input) {
    EXPECT_THROW(pe::Image(Bytes(64, 0)), std::runtime_error);
    EXPECT_THROW(pe::Image(Bytes{}), std::runtime_error);
}
