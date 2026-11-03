// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minidump reader tests, pinned against the committed crash dump.
//
// The values are the ones analysis/logs/03_parse_dump.txt records; naming the
// field here makes a parser regression readable, which a whole-log diff is not.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/util.h"
#include "minidump/minidump.h"
#include "pe/pe_image.h"

namespace fs = std::filesystem;
using namespace ksh;

namespace {

std::string dump_path() {
    return std::string(KSH_PROJECT_DIR) + "\\analysis\\dumps\\KishinHisho.exe.45764.dmp";
}

bool have_dump() { return fs::exists(fs::u8path(dump_path())); }

#define REQUIRE_DUMP() \
    if (!have_dump()) GTEST_SKIP() << "analysis/dumps/*.dmp not present"

dump::MiniDump load() { return dump::MiniDump(read_file(dump_path())); }

}  // namespace

TEST(MiniDump, rejects_non_dump_input) {
    dump::MiniDump md(Bytes{'N', 'O', 'P', 'E', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(md.is_minidump());
    EXPECT_FALSE(dump::MiniDump(Bytes{}).is_minidump());
}

TEST(MiniDump, reads_the_stream_directory) {
    REQUIRE_DUMP();
    const auto md = load();
    ASSERT_TRUE(md.is_minidump());
    EXPECT_EQ(md.number_of_streams(), 12u);
    // Four of the twelve are zero-length and get dropped.
    EXPECT_EQ(md.streams().size(), 8u);
    EXPECT_EQ(md.streams().size(), md.streams_in_order().size());
    EXPECT_EQ(md.streams().count(6), 1u) << "ExceptionStream";
    EXPECT_EQ(md.streams().count(5), 1u) << "MemoryListStream";
    EXPECT_EQ(md.streams().count(4), 1u) << "ModuleListStream";
}

// The report prints the streams in DIRECTORY order, which is not necessarily the
// std::map's ascending-type order -- this dump's directory happens to be sorted,
// so pin the order against the file rather than against the map.
TEST(MiniDump, keeps_the_directory_order_the_report_prints) {
    REQUIRE_DUMP();
    const Bytes raw = read_file(dump_path());
    const dump::MiniDump md(raw);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> want;  // (type, rva)
    for (std::uint32_t i = 0; i < md.number_of_streams(); ++i) {
        const std::size_t off = md.stream_directory_rva() + i * 12u;
        const std::uint32_t type = pe::rd_u32(raw, off);
        const std::uint32_t size = pe::rd_u32(raw, off + 4);
        const std::uint32_t rva = pe::rd_u32(raw, off + 8);
        if (size) want.emplace_back(type, rva);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> got;
    for (const auto& [type, ref] : md.streams_in_order()) got.emplace_back(type, ref.rva);
    EXPECT_EQ(want, got);
}

TEST(MiniDump, exception_record_is_the_alpharom_access_violation) {
    REQUIRE_DUMP();
    const auto exc = load().exception();
    ASSERT_TRUE(exc.has_value());
    EXPECT_EQ(exc->code, 0xC0000005u);
    EXPECT_EQ(dump::exception_name(exc->code), "EXCEPTION_ACCESS_VIOLATION");
    EXPECT_EQ(exc->number_parameters, 2u);
    ASSERT_GE(exc->parameters.size(), 2u);
    EXPECT_EQ(exc->parameters[0], 0u) << "READ";
    EXPECT_EQ(exc->context_size, 0x2CCu);
}

// EIP in the stack (0x1AF...) is the whole finding: AlphaROM jumped into a
// thunk it built on the stack.
TEST(MiniDump, context_holds_the_crash_registers) {
    REQUIRE_DUMP();
    const auto exc = load().exception();
    ASSERT_TRUE(exc.has_value());
    ASSERT_TRUE(exc->context.has_value());
    EXPECT_EQ(exc->context->eip, 0x001AFEC7u);
    EXPECT_EQ(exc->context->ebp, 0x0042830Eu);
    EXPECT_EQ(exc->context->eip, static_cast<std::uint32_t>(exc->address));
}

TEST(MiniDump, module_list_resolves_the_faulting_address) {
    REQUIRE_DUMP();
    const auto md = load();
    const auto modules = md.modules();
    ASSERT_FALSE(modules.empty());
    bool saw_game = false;
    for (const auto& m : modules)
        if (m.name.find("KishinHisho.exe") != std::string::npos) saw_game = true;
    EXPECT_TRUE(saw_game);
    // EIP is on the stack, so it must resolve to no module at all.
    const auto exc = md.exception();
    ASSERT_TRUE(exc && exc->context);
    for (const auto& m : modules)
        EXPECT_FALSE(exc->context->eip >= m.base && exc->context->eip < m.base + m.size)
            << "EIP unexpectedly inside " << m.name;
}

TEST(MiniDump, memory_ranges_are_sorted_by_start) {
    REQUIRE_DUMP();
    const auto ranges = load().memory_ranges();
    ASSERT_FALSE(ranges.empty());
    EXPECT_TRUE(std::is_sorted(ranges.begin(), ranges.end()));
}

TEST(MiniDump, read_mem_walks_ranges_and_reports_gaps) {
    REQUIRE_DUMP();
    const auto md = load();
    const auto ranges = md.memory_ranges();

    // The stage-2 jump-delta field 05_trace_stage3 reads.
    const Bytes field = md.read_mem(ranges, 0x42830E + 0x65D, 4);
    ASSERT_EQ(field.size(), 4u);
    EXPECT_EQ(hex_lower(field.data(), field.size()), "353c0000");

    // An address no range covers yields nothing, rather than zeros.
    EXPECT_TRUE(md.read_mem(ranges, 0xF0000000ull, 4).empty());
    EXPECT_EQ(dump::MiniDump::find_range(ranges, 0xF0000000ull), nullptr);
}

TEST(MiniDump, names_the_streams_the_report_prints) {
    EXPECT_EQ(dump::stream_type_name(3), "ThreadListStream");
    EXPECT_EQ(dump::stream_type_name(16), "MemoryInfoListStream");
    EXPECT_EQ(dump::stream_type_name(99), "?99") << "an unknown type keeps the '?<n>' form";
    EXPECT_EQ(dump::exception_name(0x1234), "?");
}
