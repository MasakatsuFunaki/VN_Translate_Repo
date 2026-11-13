// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pure synthetic unit tests for the Windows minidump reader.
#include "common/util.h"
#include "minidump/minidump.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

using namespace ksh;

namespace {

constexpr std::size_t kNonDumpHeaderBytes = 16;
constexpr std::size_t kSyntheticDumpBytes = 0x2C0;

constexpr std::uint32_t kStreamThreadList = 3;
constexpr std::uint32_t kStreamModuleList = 4;
constexpr std::uint32_t kStreamMemoryList = 5;
constexpr std::uint32_t kStreamException = 6;
constexpr std::uint32_t kStreamSystemInfo = 7;
constexpr std::uint32_t kStreamMemoryInfoList = 16;
constexpr std::uint32_t kStreamUnknown99 = 99;
constexpr std::uint32_t kUnknownExceptionCode = 0x1234u;

constexpr std::uint32_t kSyntheticVersion = 0x0000A793u;
constexpr std::uint32_t kSyntheticTotalStreams = 4;
constexpr std::size_t kExpectedNonEmptyStreams = 3;
constexpr std::uint32_t kDirectoryRva = 0x20u;
constexpr std::size_t kDirectoryEntryBytes = 12;

constexpr std::uint32_t kExceptionStreamRva = 0x60u;
constexpr std::uint32_t kExceptionStreamSize = 168u;
constexpr std::uint32_t kContextRva = 0x110u;
constexpr std::uint32_t kContextSize = 204u;
constexpr std::uint32_t kModuleListRva = 0x1E0u;
constexpr std::uint32_t kModuleListSize = 112u;
constexpr std::uint32_t kModuleNameStringRva = 0x250u;
constexpr std::uint32_t kMemoryListRva = 0x280u;
constexpr std::uint32_t kMemoryListSize = 36u;
constexpr std::uint32_t kMemSlice0Rva = 0x2B0u;
constexpr std::uint32_t kMemSlice1Rva = 0x2B4u;

constexpr std::size_t kHdrVersionOff = 4;
constexpr std::size_t kHdrStreamCountOff = 8;
constexpr std::size_t kHdrDirectoryRvaOff = 12;

constexpr std::size_t kExcThreadIdOff = 0;
constexpr std::size_t kExcCodeOff = 8;
constexpr std::size_t kExcAddressOff = 24;
constexpr std::size_t kExcNumParamsOff = 32;
constexpr std::size_t kExcParam0Off = 40;
constexpr std::size_t kExcParam1Off = 48;
constexpr std::size_t kExcContextSizeOff = 160;
constexpr std::size_t kExcContextRvaOff = 164;

constexpr std::size_t kCtxFlagsOff = 0;
constexpr std::size_t kCtxDr0Off = 4;
constexpr std::size_t kCtxGpBlockOff = 156;
constexpr std::size_t kCtxEaxInGp = 20;
constexpr std::size_t kCtxEbpInGp = 24;
constexpr std::size_t kCtxEipInGp = 28;

constexpr std::size_t kModCountOff = 0;
constexpr std::size_t kModFirstOff = 4;
constexpr std::size_t kModBaseOff = 0;
constexpr std::size_t kModSizeOff = 8;
constexpr std::size_t kModTimestampOff = 16;
constexpr std::size_t kModNameRvaOff = 20;

constexpr std::size_t kMemCountOff = 0;
constexpr std::size_t kMemFirstOff = 4;
constexpr std::size_t kMemDescBytes = 16;
constexpr std::size_t kMemSizeOff = 8;
constexpr std::size_t kMemRvaOff = 12;

constexpr std::uint32_t kExpectedThreadId = 0x1234u;
constexpr std::uint32_t kAccessViolationCode = 0xC0000005u;
constexpr std::uint64_t kExpectedFaultAddress = 0x001AFEC7ull;
constexpr std::uint32_t kExpectedParamCount = 2u;
constexpr std::uint64_t kExpectedFaultTarget = 0xDEADBEEFull;
constexpr std::uint32_t kExpectedContextFlags = 0x0001003Fu;
constexpr std::uint32_t kExpectedDr0 = 0x11111111u;
constexpr std::uint32_t kExpectedEax = 0xCAFEBABEu;
constexpr std::uint32_t kExpectedEbp = 0x0042830Eu;
constexpr std::uint32_t kExpectedEip = 0x001AFEC7u;

constexpr std::uint64_t kExpectedModuleBase = 0x00400000ull;
constexpr std::uint32_t kExpectedModuleSize = 0x00010000u;
constexpr std::uint32_t kExpectedModuleTimestamp = 0x4458CC07u;

constexpr std::uint64_t kMemRange0Start = 0x00500000ull;
constexpr std::uint64_t kMemRange1Start = 0x00500004ull;
constexpr std::uint32_t kMemRangeSliceBytes = 4u;
constexpr std::uint64_t kStraddlingReadStart = 0x00500002ull;
constexpr std::uint64_t kUnmappedAddress = 0x00900000ull;

constexpr unsigned int kShift8 = 8;
constexpr unsigned int kShift16 = 16;
constexpr unsigned int kShift24 = 24;
constexpr unsigned int kShift32 = 32;
constexpr std::uint32_t kByteMask = 0xFFu;

void wr_u16(Bytes& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v & kByteMask);
    b[off + 1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
}

void wr_u32(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v & kByteMask);
    b[off + 1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    b[off + 2] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    b[off + 3] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

void wr_u64(Bytes& b, std::size_t off, std::uint64_t v) {
    wr_u32(b, off, static_cast<std::uint32_t>(v));
    wr_u32(b, off + sizeof(std::uint32_t), static_cast<std::uint32_t>(v >> kShift32));
}

void wr_dir_entry(Bytes& b, std::size_t index, std::uint32_t type, std::uint32_t size,
                  std::uint32_t rva) {
    const std::size_t off = kDirectoryRva + index * kDirectoryEntryBytes;
    wr_u32(b, off, type);
    wr_u32(b, off + sizeof(std::uint32_t), size);
    wr_u32(b, off + 2 * sizeof(std::uint32_t), rva);
}

Bytes build_synthetic_minidump() {
    Bytes b(kSyntheticDumpBytes, 0);
    std::memcpy(b.data(), "MDMP", 4);
    wr_u32(b, kHdrVersionOff, kSyntheticVersion);
    wr_u32(b, kHdrStreamCountOff, kSyntheticTotalStreams);
    wr_u32(b, kHdrDirectoryRvaOff, kDirectoryRva);

    // Deliberately unsorted directory order with one zero-length stream to pin
    // both zero-size filtering and directory-order preservation.
    wr_dir_entry(b, 0, kStreamException, kExceptionStreamSize, kExceptionStreamRva);
    wr_dir_entry(b, 1, kStreamSystemInfo, 0, 0);
    wr_dir_entry(b, 2, kStreamModuleList, kModuleListSize, kModuleListRva);
    wr_dir_entry(b, 3, kStreamMemoryList, kMemoryListSize, kMemoryListRva);

    wr_u32(b, kExceptionStreamRva + kExcThreadIdOff, kExpectedThreadId);
    wr_u32(b, kExceptionStreamRva + kExcCodeOff, kAccessViolationCode);
    wr_u64(b, kExceptionStreamRva + kExcAddressOff, kExpectedFaultAddress);
    wr_u32(b, kExceptionStreamRva + kExcNumParamsOff, kExpectedParamCount);
    wr_u64(b, kExceptionStreamRva + kExcParam0Off, 0);
    wr_u64(b, kExceptionStreamRva + kExcParam1Off, kExpectedFaultTarget);
    wr_u32(b, kExceptionStreamRva + kExcContextSizeOff, kContextSize);
    wr_u32(b, kExceptionStreamRva + kExcContextRvaOff, kContextRva);

    wr_u32(b, kContextRva + kCtxFlagsOff, kExpectedContextFlags);
    wr_u32(b, kContextRva + kCtxDr0Off, kExpectedDr0);
    wr_u32(b, kContextRva + kCtxGpBlockOff + kCtxEaxInGp, kExpectedEax);
    wr_u32(b, kContextRva + kCtxGpBlockOff + kCtxEbpInGp, kExpectedEbp);
    wr_u32(b, kContextRva + kCtxGpBlockOff + kCtxEipInGp, kExpectedEip);

    wr_u32(b, kModuleListRva + kModCountOff, 1);
    const std::size_t mod0 = kModuleListRva + kModFirstOff;
    wr_u64(b, mod0 + kModBaseOff, kExpectedModuleBase);
    wr_u32(b, mod0 + kModSizeOff, kExpectedModuleSize);
    wr_u32(b, mod0 + kModTimestampOff, kExpectedModuleTimestamp);
    wr_u32(b, mod0 + kModNameRvaOff, kModuleNameStringRva);

    constexpr std::wstring_view kModName = L"synthetic_game.exe";
    wr_u32(b, kModuleNameStringRva,
           static_cast<std::uint32_t>(kModName.size() * sizeof(wchar_t)));
    for (std::size_t i = 0; i < kModName.size(); ++i) {
        wr_u16(b, kModuleNameStringRva + sizeof(std::uint32_t) + i * sizeof( std::uint16_t),
               static_cast<std::uint16_t>(kModName[i]));
    }

    // Emit the higher address first so memory_ranges() exercises its sort.
    wr_u32(b, kMemoryListRva + kMemCountOff, 2);
    const std::size_t desc0 = kMemoryListRva + kMemFirstOff;
    wr_u64(b, desc0, kMemRange1Start);
    wr_u32(b, desc0 + kMemSizeOff, kMemRangeSliceBytes);
    wr_u32(b, desc0 + kMemRvaOff, kMemSlice1Rva);

    const std::size_t desc1 = desc0 + kMemDescBytes;
    wr_u64(b, desc1, kMemRange0Start);
    wr_u32(b, desc1 + kMemSizeOff, kMemRangeSliceBytes);
    wr_u32(b, desc1 + kMemRvaOff, kMemSlice0Rva);

    const std::uint8_t slice0[] = {0x11u, 0x22u, 0x33u, 0x44u};
    const std::uint8_t slice1[] = {0x55u, 0x66u, 0x77u, 0x88u};
    std::memcpy(b.data() + kMemSlice0Rva, slice0, sizeof(slice0));
    std::memcpy(b.data() + kMemSlice1Rva, slice1, sizeof(slice1));
    return b;
}

}  // namespace

TEST(MiniDump, rejects_non_dump_input) {
    Bytes bad(kNonDumpHeaderBytes, 0);
    bad[0] = 'N';
    bad[1] = 'O';
    bad[2] = 'P';
    bad[3] = 'E';
    const dump::MiniDump md(std::move(bad));
    EXPECT_FALSE(md.is_minidump());
    EXPECT_FALSE(dump::MiniDump(Bytes{}).is_minidump());
}

TEST(MiniDump, names_the_streams_the_report_prints) {
    EXPECT_EQ(dump::stream_type_name(kStreamThreadList), "ThreadListStream");
    EXPECT_EQ(dump::stream_type_name(kStreamMemoryInfoList), "MemoryInfoListStream");
    EXPECT_EQ(dump::stream_type_name(kStreamUnknown99), "?99")
        << "an unknown type keeps the '?<n>' form";
    EXPECT_EQ(dump::exception_name(kUnknownExceptionCode), "?");
}

TEST(MiniDump, parses_synthetic_mdmp_streams_exception_modules_and_straddling_memory) {
    const dump::MiniDump md(build_synthetic_minidump());
    ASSERT_TRUE(md.is_minidump());
    EXPECT_EQ(md.version(), kSyntheticVersion);
    EXPECT_EQ(md.number_of_streams(), kSyntheticTotalStreams);
    EXPECT_EQ(md.stream_directory_rva(), kDirectoryRva);

    ASSERT_EQ(md.streams().size(), kExpectedNonEmptyStreams);
    ASSERT_EQ(md.streams_in_order().size(), kExpectedNonEmptyStreams);
    EXPECT_EQ(md.streams_in_order()[0].first, kStreamException);
    EXPECT_EQ(md.streams_in_order()[1].first, kStreamModuleList);
    EXPECT_EQ(md.streams_in_order()[2].first, kStreamMemoryList);

    const auto exc = md.exception();
    ASSERT_TRUE(exc.has_value());
    EXPECT_EQ(exc->thread_id, kExpectedThreadId);
    EXPECT_EQ(exc->code, kAccessViolationCode);
    EXPECT_EQ(dump::exception_name(exc->code), "EXCEPTION_ACCESS_VIOLATION");
    EXPECT_EQ(exc->address, kExpectedFaultAddress);
    ASSERT_EQ(exc->parameters.size(), static_cast<std::size_t>(kExpectedParamCount));
    EXPECT_EQ(exc->parameters[0], 0u);
    EXPECT_EQ(exc->parameters[1], kExpectedFaultTarget);
    ASSERT_TRUE(exc->context.has_value());
    EXPECT_EQ(exc->context->context_flags, kExpectedContextFlags);
    EXPECT_EQ(exc->context->dr0, kExpectedDr0);
    EXPECT_EQ(exc->context->eax, kExpectedEax);
    EXPECT_EQ(exc->context->ebp, kExpectedEbp);
    EXPECT_EQ(exc->context->eip, kExpectedEip);

    const auto modules = md.modules();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].base, kExpectedModuleBase);
    EXPECT_EQ(modules[0].size, kExpectedModuleSize);
    EXPECT_EQ(modules[0].time_date_stamp, kExpectedModuleTimestamp);
    EXPECT_EQ(modules[0].name, "synthetic_game.exe");

    const auto ranges = md.memory_ranges();
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_TRUE(std::is_sorted(ranges.begin(), ranges.end()));
    EXPECT_EQ(ranges[0].start, kMemRange0Start);
    EXPECT_EQ(ranges[1].start, kMemRange1Start);

    const Bytes straddling = md.read_mem(ranges, kStraddlingReadStart, kMemRangeSliceBytes);
    const Bytes expected_straddling{0x33u, 0x44u, 0x55u, 0x66u};
    EXPECT_EQ(straddling, expected_straddling);

    EXPECT_TRUE(md.read_mem(ranges, kUnmappedAddress, kMemRangeSliceBytes).empty());
    EXPECT_EQ(dump::MiniDump::find_range(ranges, kUnmappedAddress), nullptr);
}
