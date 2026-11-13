// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System-tier checks against the committed AlphaROM sample executable, crash
// dump, and golden forensic logs under analysis/.
#include "analysis/reports.h"
#include "common/util.h"
#include "minidump/minidump.h"
#include "pe/disasm.h"
#include "pe/pe_image.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace ksh;

namespace {

constexpr std::uint16_t kExpectedSampleMachine = 0x014Cu;
constexpr std::uint16_t kExpectedSampleSectionCount = 5u;
constexpr std::uint32_t kExpectedSampleTimestamp = 0x4458CC07u;
constexpr std::uint16_t kExpectedSampleCharacteristics = 0x010Fu;
constexpr std::uint32_t kExpectedSampleImageBase = 0x00400000u;
constexpr std::uint32_t kExpectedSampleSizeOfImage = 0x0004E000u;
constexpr std::uint32_t kExpectedSampleEntryPointRva = 0x00028000u;
constexpr std::uint64_t kExpectedSampleEntryPointVa = 0x00428000ull;
constexpr std::uint16_t kExpectedSampleDllCharacteristics = 0x0000u;
constexpr std::uint32_t kExpectedTextRawOffset = 0x00027000u;
constexpr std::size_t kExpectedStubImportCount = 4u;
constexpr std::size_t kDeviceTableRvaOffset = 0x2C5u;
constexpr std::size_t kDeviceTableLength = 0x49u;
constexpr std::size_t kMappedVerifySpan = 0x100u;
constexpr std::uint8_t kDeviceTableAddKey = 0x44u;
constexpr std::uint8_t kLowByteMask = 0xFFu;
constexpr std::size_t kDisasmEntrySpanBytes = 256u;

constexpr std::uint32_t kExpectedDumpStreamCount = 12u;
constexpr std::size_t kExpectedNonEmptyStreamCount = 8u;
constexpr std::uint32_t kStreamModuleList = 4u;
constexpr std::uint32_t kStreamMemoryList = 5u;
constexpr std::uint32_t kStreamException = 6u;
constexpr std::size_t kMinidumpDirEntryBytes = 12u;
constexpr std::size_t kMinidumpDirSizeFieldOffset = 4u;
constexpr std::size_t kMinidumpDirRvaFieldOffset = 8u;
constexpr std::uint32_t kAccessViolationCode = 0xC0000005u;
constexpr std::uint32_t kExpectedAvParamCount = 2u;
constexpr std::uint32_t kExpectedContext32Bytes = 0x2CCu;
constexpr std::uint32_t kExpectedCrashEip = 0x001AFEC7u;
constexpr std::uint32_t kExpectedCrashEbp = 0x0042830Eu;
constexpr std::uint64_t kStage2JumpDeltaOffset = 0x65Dull;
constexpr std::size_t kJumpDeltaBytes = 4u;
constexpr std::uint64_t kUnmappedProbeAddress = 0xF0000000ull;

std::string project_dir() { return KSH_PROJECT_DIR; }

std::string sample_exe_path() {
    return project_dir() + "\\analysis\\samples\\KishinHisho_orig.exe";
}

std::string sample_dump_path() {
    return project_dir() + "\\analysis\\dumps\\KishinHisho.exe.45764.dmp";
}

bool have_sample_exe() { return fs::exists(fs::u8path(sample_exe_path())); }

bool have_sample_dump() { return fs::exists(fs::u8path(sample_dump_path())); }

dump::MiniDump load_sample_dump() { return dump::MiniDump(read_file(sample_dump_path())); }

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// The committed logs hold CRLF (the apps convert on the way out) while the
// report builders emit LF, so compare line by line rather than byte by byte.
void expect_matches_log(const std::string& produced, const std::string& log_name) {
    const std::string log_path = project_dir() + "\\analysis\\logs\\" + log_name;
    if (!fs::exists(fs::u8path(log_path))) {
        GTEST_SKIP() << log_name << " not present";
    }
    const Bytes raw = read_file(log_path);
    const auto want = split_lines(std::string(raw.begin(), raw.end()));
    const auto got = split_lines(produced);

    for (std::size_t i = 0; i < std::min(want.size(), got.size()); ++i) {
        ASSERT_EQ(want[i], got[i]) << log_name << " diverges at line " << (i + 1);
    }
    ASSERT_EQ(want.size(), got.size()) << log_name << " has a different number of lines";
}

}  // namespace

TEST(GoldenLogs, recon_report_reproduces_01_recon_txt) {
    const std::string exe = sample_exe_path();
    if (!fs::exists(fs::u8path(exe))) {
        GTEST_SKIP() << "sample exe not present";
    }
    expect_matches_log(analysis::recon_report(exe), "01_recon.txt");
}

TEST(GoldenLogs, dump_report_reproduces_03_parse_dump_txt) {
    const std::string dump = sample_dump_path();
    if (!fs::exists(fs::u8path(dump))) {
        GTEST_SKIP() << "crash dump not present";
    }
    expect_matches_log(analysis::dump_report(dump) + "\n", "03_parse_dump.txt");
}

TEST(PeImage, reads_headers_of_the_sample) {
    if (!have_sample_exe()) {
        GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present";
    }
    const pe::Image pe(read_file(sample_exe_path()));
    EXPECT_EQ(pe.machine(), kExpectedSampleMachine);
    EXPECT_EQ(pe.number_of_sections(), kExpectedSampleSectionCount);
    EXPECT_EQ(pe.time_date_stamp(), kExpectedSampleTimestamp);
    EXPECT_EQ(pe.characteristics(), kExpectedSampleCharacteristics);
    EXPECT_EQ(pe.image_base(), kExpectedSampleImageBase);
    EXPECT_EQ(pe.size_of_image(), kExpectedSampleSizeOfImage);
    EXPECT_EQ(pe.address_of_entry_point(), kExpectedSampleEntryPointRva);
    EXPECT_EQ(pe.dll_characteristics(), kExpectedSampleDllCharacteristics);
}

TEST(PeImage, section_table_shows_the_alpharom_layout) {
    if (!have_sample_exe()) {
        GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present";
    }
    const pe::Image pe(read_file(sample_exe_path()));
    const auto& s = pe.sections();
    ASSERT_EQ(s.size(), static_cast<std::size_t>(kExpectedSampleSectionCount));
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        EXPECT_TRUE(s[i].name.empty()) << "section " << i << " should be anonymous";
    }
    EXPECT_EQ(s.back().name, ".text");
    EXPECT_EQ(s.back().virtual_address, kExpectedSampleEntryPointRva);
    EXPECT_EQ(s.back().pointer_to_raw_data, kExpectedTextRawOffset);
}

TEST(PeImage, imports_are_the_four_the_stub_leaves_behind) {
    if (!have_sample_exe()) {
        GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present";
    }
    const pe::Image pe(read_file(sample_exe_path()));
    std::size_t total = 0;
    for (const auto& d : pe.imports()) {
        total += d.imports.size();
    }
    EXPECT_EQ(total, kExpectedStubImportCount);
    bool saw_lopen = false;
    for (const auto& d : pe.imports()) {
        for (const auto& f : d.imports) {
            if (f.name == "_lopen") {
                saw_lopen = true;
            }
        }
    }
    EXPECT_TRUE(saw_lopen);
}

TEST(PeImage, sample_has_no_tls_directory) {
    if (!have_sample_exe()) {
        GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present";
    }
    const pe::Image pe(read_file(sample_exe_path()));
    EXPECT_FALSE(pe.tls().has_value());
}

TEST(PeImage, memory_mapped_image_is_rva_indexable) {
    if (!have_sample_exe()) {
        GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present";
    }
    const pe::Image pe(read_file(sample_exe_path()));
    const Bytes& mapped = pe.memory_mapped_image();
    const Bytes& raw = pe.raw();
    ASSERT_EQ(mapped.size(), static_cast<std::size_t>(kExpectedSampleSizeOfImage))
        << "mapped size must equal SizeOfImage";
    const std::size_t raw_dev_table = kExpectedTextRawOffset + kDeviceTableRvaOffset;
    const std::size_t mapped_dev_table = kExpectedSampleEntryPointRva + kDeviceTableRvaOffset;
    ASSERT_GT(raw.size(), raw_dev_table + kDeviceTableLength);
    for (std::size_t i = 0; i < kDeviceTableLength; ++i) {
        EXPECT_EQ(mapped[mapped_dev_table + i], raw[raw_dev_table + i]) << "at " << i;
    }
    for (std::size_t i = 0; i < kMappedVerifySpan; ++i) {
        EXPECT_EQ(mapped[kExpectedSampleEntryPointRva + i], raw[kExpectedTextRawOffset + i])
            << "at " << i;
    }
}

TEST(PeImage, device_name_table_decrypts_to_the_known_strings) {
    if (!have_sample_exe()) {
        GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present";
    }
    const pe::Image pe(read_file(sample_exe_path()));
    const Bytes& mapped = pe.memory_mapped_image();
    std::string decoded;
    const std::size_t table_rva = kExpectedSampleEntryPointRva + kDeviceTableRvaOffset;
    for (std::size_t i = 0; i < kDeviceTableLength; ++i) {
        decoded += static_cast<char>((mapped[table_rva + i] + kDeviceTableAddKey) & kLowByteMask);
    }
    EXPECT_NE(decoded.find("\\\\.\\TRW"), std::string::npos);
    EXPECT_NE(decoded.find("\\\\.\\SICE"), std::string::npos);
    EXPECT_NE(decoded.find("\\\\.\\NTICE"), std::string::npos);
    EXPECT_NE(decoded.find("\\\\.\\REGMON"), std::string::npos);
}

TEST(PeImage, disassembles_the_entry_point) {
    if (!have_sample_exe()) {
        GTEST_SKIP() << "analysis/samples/KishinHisho_orig.exe not present";
    }
    const pe::Image pe(read_file(sample_exe_path()));
    const pe::Disassembler md;
    const Bytes& mapped = pe.memory_mapped_image();
    const auto ins = md.disasm(mapped.data() + kExpectedSampleEntryPointRva,
                               kDisasmEntrySpanBytes, kExpectedSampleEntryPointVa);
    ASSERT_FALSE(ins.empty());
    EXPECT_EQ(ins.front().address, kExpectedSampleEntryPointVa);
    for (const auto& i : ins) {
        EXPECT_FALSE(i.mnemonic.empty());
    }
}

TEST(MiniDump, reads_the_stream_directory) {
    if (!have_sample_dump()) {
        GTEST_SKIP() << "analysis/dumps/*.dmp not present";
    }
    const auto md = load_sample_dump();
    ASSERT_TRUE(md.is_minidump());
    EXPECT_EQ(md.number_of_streams(), kExpectedDumpStreamCount);
    EXPECT_EQ(md.streams().size(), kExpectedNonEmptyStreamCount);
    EXPECT_EQ(md.streams().size(), md.streams_in_order().size());
    EXPECT_EQ(md.streams().count(kStreamException), 1u) << "ExceptionStream";
    EXPECT_EQ(md.streams().count(kStreamMemoryList), 1u) << "MemoryListStream";
    EXPECT_EQ(md.streams().count(kStreamModuleList), 1u) << "ModuleListStream";
}

TEST(MiniDump, keeps_the_directory_order_the_report_prints) {
    if (!have_sample_dump()) {
        GTEST_SKIP() << "analysis/dumps/*.dmp not present";
    }
    const Bytes raw = read_file(sample_dump_path());
    const dump::MiniDump md(raw);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> want;
    for (std::uint32_t i = 0; i < md.number_of_streams(); ++i) {
        const std::size_t off = md.stream_directory_rva() + i * kMinidumpDirEntryBytes;
        const std::uint32_t type = pe::rd_u32(raw, off);
        const std::uint32_t size = pe::rd_u32(raw, off + kMinidumpDirSizeFieldOffset);
        const std::uint32_t rva = pe::rd_u32(raw, off + kMinidumpDirRvaFieldOffset);
        if (size != 0) {
            want.emplace_back(type, rva);
        }
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> got;
    for (const auto& [type, ref] : md.streams_in_order()) {
        got.emplace_back(type, ref.rva);
    }
    EXPECT_EQ(want, got);
}

TEST(MiniDump, exception_record_is_the_alpharom_access_violation) {
    if (!have_sample_dump()) {
        GTEST_SKIP() << "analysis/dumps/*.dmp not present";
    }
    const auto exc = load_sample_dump().exception();
    ASSERT_TRUE(exc.has_value());
    EXPECT_EQ(exc->code, kAccessViolationCode);
    EXPECT_EQ(dump::exception_name(exc->code), "EXCEPTION_ACCESS_VIOLATION");
    EXPECT_EQ(exc->number_parameters, kExpectedAvParamCount);
    ASSERT_GE(exc->parameters.size(), static_cast<std::size_t>(kExpectedAvParamCount));
    EXPECT_EQ(exc->parameters[0], 0u) << "READ";
    EXPECT_EQ(exc->context_size, kExpectedContext32Bytes);
}

TEST(MiniDump, context_holds_the_crash_registers) {
    if (!have_sample_dump()) {
        GTEST_SKIP() << "analysis/dumps/*.dmp not present";
    }
    const auto exc = load_sample_dump().exception();
    ASSERT_TRUE(exc.has_value());
    ASSERT_TRUE(exc->context.has_value());
    EXPECT_EQ(exc->context->eip, kExpectedCrashEip);
    EXPECT_EQ(exc->context->ebp, kExpectedCrashEbp);
    EXPECT_EQ(exc->context->eip, static_cast<std::uint32_t>(exc->address));
}

TEST(MiniDump, module_list_resolves_the_faulting_address) {
    if (!have_sample_dump()) {
        GTEST_SKIP() << "analysis/dumps/*.dmp not present";
    }
    const auto md = load_sample_dump();
    const auto modules = md.modules();
    ASSERT_FALSE(modules.empty());
    bool saw_game = false;
    for (const auto& m : modules) {
        if (m.name.find("KishinHisho.exe") != std::string::npos) {
            saw_game = true;
        }
    }
    EXPECT_TRUE(saw_game);

    const auto exc = md.exception();
    ASSERT_TRUE(exc && exc->context);
    for (const auto& m : modules) {
        EXPECT_FALSE(exc->context->eip >= m.base && exc->context->eip < m.base + m.size)
            << "EIP unexpectedly inside " << m.name;
    }
}

TEST(MiniDump, memory_ranges_are_sorted_by_start) {
    if (!have_sample_dump()) {
        GTEST_SKIP() << "analysis/dumps/*.dmp not present";
    }
    const auto ranges = load_sample_dump().memory_ranges();
    ASSERT_FALSE(ranges.empty());
    EXPECT_TRUE(std::is_sorted(ranges.begin(), ranges.end()));
}

TEST(MiniDump, read_mem_walks_ranges_and_reports_gaps) {
    if (!have_sample_dump()) {
        GTEST_SKIP() << "analysis/dumps/*.dmp not present";
    }
    const auto md = load_sample_dump();
    const auto ranges = md.memory_ranges();

    const Bytes field =
        md.read_mem(ranges, kExpectedCrashEbp + kStage2JumpDeltaOffset, kJumpDeltaBytes);
    ASSERT_EQ(field.size(), kJumpDeltaBytes);
    EXPECT_EQ(hex_lower(field.data(), field.size()), "353c0000");

    EXPECT_TRUE(md.read_mem(ranges, kUnmappedProbeAddress, kJumpDeltaBytes).empty());
    EXPECT_EQ(dump::MiniDump::find_range(ranges, kUnmappedProbeAddress), nullptr);
}
