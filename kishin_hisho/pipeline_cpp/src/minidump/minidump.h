// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Windows minidump reader for the crash-analysis tools.
//
// Only the streams those tools read: Exception (plus the embedded 32-bit
// CONTEXT), ModuleList, MemoryList, and the stream directory itself.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "common/util.h"

namespace ksh::dump {

// MINIDUMP_STREAM_TYPE values the tool names; anything else prints as "?<n>".
std::string stream_type_name(std::uint32_t type);

// Human name for an NTSTATUS exception code, or "?".
std::string exception_name(std::uint32_t code);

struct StreamRef {
    std::uint32_t size = 0;
    std::uint32_t rva = 0;
};

// The 32-bit CONTEXT the crash was captured with.
struct Context32 {
    std::uint32_t context_flags = 0;
    std::uint32_t edi = 0, esi = 0, ebx = 0, edx = 0, ecx = 0, eax = 0;
    std::uint32_t ebp = 0, eip = 0, cs = 0, eflags = 0, esp = 0, ss = 0;
    std::uint32_t dr0 = 0, dr1 = 0, dr2 = 0, dr3 = 0, dr6 = 0, dr7 = 0;
};

struct ExceptionInfo {
    std::uint32_t thread_id = 0;
    std::uint32_t code = 0;
    std::uint32_t flags = 0;
    std::uint64_t address = 0;
    std::uint32_t number_parameters = 0;
    std::vector<std::uint64_t> parameters;  // capped at 15, as the format allows
    std::uint32_t context_size = 0;
    std::uint32_t context_rva = 0;
    std::optional<Context32> context;  // absent when size or rva is 0
};

// One MINIDUMP_MEMORY_DESCRIPTOR: a slice of process memory saved in the dump.
struct MemoryRange {
    std::uint64_t start = 0;
    std::uint32_t size = 0;
    std::uint32_t rva = 0;  // where the bytes live inside the dump file

    // Ordered by start address; size and rva only break ties, so the sort is
    // total and the report's range list is stable across runs.
    friend bool operator<(const MemoryRange& a, const MemoryRange& b) {
        return std::tie(a.start, a.size, a.rva) < std::tie(b.start, b.size, b.rva);
    }
};

struct Module {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    std::uint32_t time_date_stamp = 0;
    std::string name;  // UTF-8; "?" when the UTF-16 decode fails
};

class MiniDump {
public:
    // Throws std::runtime_error only for unreadable input; a bad signature is
    // reported via is_minidump() so the tool can print its own message.
    explicit MiniDump(Bytes data);

    bool is_minidump() const { return is_minidump_; }
    const Bytes& raw() const { return data_; }
    std::uint32_t version() const { return version_; }
    std::uint32_t number_of_streams() const { return number_of_streams_; }
    std::uint32_t stream_directory_rva() const { return stream_directory_rva_; }

    // Streams with a zero DataSize are dropped -- they carry no data and the
    // report does not list them.  std::map orders by ascending stream type,
    // which is NOT the order the report prints; use streams_in_order() there.
    const std::map<std::uint32_t, StreamRef>& streams() const { return streams_; }
    // Directory order, which IS what the report prints.
    const std::vector<std::pair<std::uint32_t, StreamRef>>& streams_in_order() const {
        return streams_in_order_;
    }

    std::optional<ExceptionInfo> exception() const;
    std::vector<Module> modules() const;

    // MemoryListStream, sorted by start address (the tools all want it sorted).
    std::vector<MemoryRange> memory_ranges() const;

    // The range covering `addr`, or nullptr.  Ranges must come from
    // memory_ranges(); passing an unsorted vector still works, first hit wins.
    static const MemoryRange* find_range(const std::vector<MemoryRange>& ranges,
                                         std::uint64_t addr);

    // Read `length` bytes of process memory, walking across adjacent ranges --
    // 05_trace_stage3 reads fields that can straddle two.  Empty when any part
    // of the span is missing from the dump.
    Bytes read_mem(const std::vector<MemoryRange>& ranges, std::uint64_t addr,
                   std::size_t length) const;

private:
    Bytes data_;
    bool is_minidump_ = false;
    std::uint32_t version_ = 0;
    std::uint32_t number_of_streams_ = 0;
    std::uint32_t stream_directory_rva_ = 0;
    std::map<std::uint32_t, StreamRef> streams_;
    std::vector<std::pair<std::uint32_t, StreamRef>> streams_in_order_;
};

}  // namespace ksh::dump
