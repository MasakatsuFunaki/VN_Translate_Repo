// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "minidump.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "pe/pe_image.h"

namespace ksh::dump {

namespace {

using pe::rd_u32;
using pe::rd_u64;

// MINIDUMP_HEADER
constexpr std::size_t kHdrVersion = 4;
constexpr std::size_t kHdrNumberOfStreams = 8;
constexpr std::size_t kHdrStreamDirectoryRva = 12;
constexpr std::size_t kDirEntrySize = 12;

// MINIDUMP_EXCEPTION_STREAM / EXCEPTION_RECORD, offsets from the stream RVA.
constexpr std::size_t kExcThreadId = 0;
constexpr std::size_t kExcCode = 8;
constexpr std::size_t kExcFlags = 12;
constexpr std::size_t kExcAddress = 24;
constexpr std::size_t kExcNumberParameters = 32;
constexpr std::size_t kExcParameters = 40;
constexpr std::size_t kMaxExceptionParameters = 15;
constexpr std::size_t kExcContextSize = kExcParameters + kMaxExceptionParameters * 8;
constexpr std::size_t kExcContextRva = kExcContextSize + 4;

// 32-bit CONTEXT, offsets from the context RVA.
constexpr std::size_t kCtxFlags = 0;
constexpr std::size_t kCtxDr0 = 4;
constexpr std::size_t kCtxGpBlock = 156;  // after Dr regs (24), FloatSave (112), Seg (16)

// MINIDUMP_MODULE_LIST
constexpr std::size_t kModListCount = 0;
constexpr std::size_t kModListFirst = 4;
constexpr std::size_t kModuleRecordSize = 108;
constexpr std::size_t kModBaseOfImage = 0;
constexpr std::size_t kModSizeOfImage = 8;
constexpr std::size_t kModTimeDateStamp = 16;
constexpr std::size_t kModNameRva = 20;

// MINIDUMP_MEMORY_LIST
constexpr std::size_t kMemListCount = 0;
constexpr std::size_t kMemListFirst = 4;
constexpr std::size_t kMemDescriptorSize = 16;
constexpr std::size_t kMemStart = 0;
constexpr std::size_t kMemSize = 8;
constexpr std::size_t kMemRva = 12;

constexpr std::uint32_t kStreamException = 6;
constexpr std::uint32_t kStreamModuleList = 4;
constexpr std::uint32_t kStreamMemoryList = 5;

// MINIDUMP_STRING: u32 byte-length then UTF-16LE, NOT NUL-terminated.
std::string read_minidump_string(const Bytes& data, std::uint32_t rva) {
    const std::uint32_t byte_len = rd_u32(data, rva);
    const std::size_t start = static_cast<std::size_t>(rva) + 4;
    if (start > data.size()) return "?";
    const std::size_t end = std::min(start + byte_len, data.size());
    if (end <= start) return {};
    // An odd length or a lone surrogate makes the decode fail; substitute "?"
    // rather than throwing -- one unreadable module name must not sink the
    // whole report.
    if ((end - start) % 2 != 0) return "?";
    const int wchars = static_cast<int>((end - start) / 2);
    const auto* w = reinterpret_cast<const wchar_t*>(data.data() + start);
    const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, wchars, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0) return "?";
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, wchars, out.data(), len, nullptr,
                        nullptr);
    return out;
}

}  // namespace

std::string stream_type_name(std::uint32_t type) {
    switch (type) {
    case 3: return "ThreadListStream";
    case 4: return "ModuleListStream";
    case 5: return "MemoryListStream";
    case 6: return "ExceptionStream";
    case 7: return "SystemInfoStream";
    case 9: return "Memory64ListStream";
    case 15: return "MiscInfoStream";
    case 16: return "MemoryInfoListStream";
    case 17: return "ThreadInfoListStream";
    default: return "?" + std::to_string(type);
    }
}

std::string exception_name(std::uint32_t code) {
    switch (code) {
    case 0xC0000005: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC000001D: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC0000096: return "EXCEPTION_PRIV_INSTRUCTION";
    case 0x80000003: return "EXCEPTION_BREAKPOINT";
    case 0x80000004: return "EXCEPTION_SINGLE_STEP";
    case 0xC00000FD: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000417: return "STATUS_INVALID_CRUNTIME_PARAMETER";
    default: return "?";
    }
}

MiniDump::MiniDump(Bytes data) : data_(std::move(data)) {
    if (data_.size() < 16 || std::memcmp(data_.data(), "MDMP", 4) != 0) return;
    is_minidump_ = true;
    version_ = rd_u32(data_, kHdrVersion);
    number_of_streams_ = rd_u32(data_, kHdrNumberOfStreams);
    stream_directory_rva_ = rd_u32(data_, kHdrStreamDirectoryRva);

    for (std::uint32_t i = 0; i < number_of_streams_; ++i) {
        const std::size_t off =
            static_cast<std::size_t>(stream_directory_rva_) + i * kDirEntrySize;
        const std::uint32_t type = rd_u32(data_, off);
        StreamRef ref{rd_u32(data_, off + 4), rd_u32(data_, off + 8)};
        if (!ref.size) continue;  // a zero-length stream carries no data
        streams_[type] = ref;
        streams_in_order_.emplace_back(type, ref);
    }
}

std::optional<ExceptionInfo> MiniDump::exception() const {
    auto it = streams_.find(kStreamException);
    if (it == streams_.end()) return std::nullopt;
    const std::uint32_t rva = it->second.rva;

    ExceptionInfo e;
    e.thread_id = rd_u32(data_, rva + kExcThreadId);
    e.code = rd_u32(data_, rva + kExcCode);
    e.flags = rd_u32(data_, rva + kExcFlags);
    e.address = rd_u64(data_, rva + kExcAddress);
    e.number_parameters = rd_u32(data_, rva + kExcNumberParameters);
    const std::size_t n = std::min<std::size_t>(e.number_parameters, kMaxExceptionParameters);
    for (std::size_t i = 0; i < n; ++i)
        e.parameters.push_back(rd_u64(data_, rva + kExcParameters + i * 8));
    e.context_size = rd_u32(data_, rva + kExcContextSize);
    e.context_rva = rd_u32(data_, rva + kExcContextRva);

    if (e.context_size && e.context_rva) {
        const std::uint32_t base = e.context_rva;
        const std::size_t gp = base + kCtxGpBlock;
        Context32 c;
        c.context_flags = rd_u32(data_, base + kCtxFlags);
        c.edi = rd_u32(data_, gp + 0);
        c.esi = rd_u32(data_, gp + 4);
        c.ebx = rd_u32(data_, gp + 8);
        c.edx = rd_u32(data_, gp + 12);
        c.ecx = rd_u32(data_, gp + 16);
        c.eax = rd_u32(data_, gp + 20);
        c.ebp = rd_u32(data_, gp + 24);
        c.eip = rd_u32(data_, gp + 28);
        c.cs = rd_u32(data_, gp + 32);
        c.eflags = rd_u32(data_, gp + 36);
        c.esp = rd_u32(data_, gp + 40);
        c.ss = rd_u32(data_, gp + 44);
        c.dr0 = rd_u32(data_, base + kCtxDr0 + 0);
        c.dr1 = rd_u32(data_, base + kCtxDr0 + 4);
        c.dr2 = rd_u32(data_, base + kCtxDr0 + 8);
        c.dr3 = rd_u32(data_, base + kCtxDr0 + 12);
        c.dr6 = rd_u32(data_, base + kCtxDr0 + 16);
        c.dr7 = rd_u32(data_, base + kCtxDr0 + 20);
        e.context = c;
    }
    return e;
}

std::vector<Module> MiniDump::modules() const {
    std::vector<Module> out;
    auto it = streams_.find(kStreamModuleList);
    if (it == streams_.end()) return out;
    const std::uint32_t rva = it->second.rva;

    const std::uint32_t count = rd_u32(data_, rva + kModListCount);
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off =
            static_cast<std::size_t>(rva) + kModListFirst + i * kModuleRecordSize;
        Module m;
        m.base = rd_u64(data_, off + kModBaseOfImage);
        m.size = rd_u32(data_, off + kModSizeOfImage);
        m.time_date_stamp = rd_u32(data_, off + kModTimeDateStamp);
        m.name = read_minidump_string(data_, rd_u32(data_, off + kModNameRva));
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<MemoryRange> MiniDump::memory_ranges() const {
    std::vector<MemoryRange> out;
    auto it = streams_.find(kStreamMemoryList);
    if (it == streams_.end()) return out;
    const std::uint32_t rva = it->second.rva;

    const std::uint32_t count = rd_u32(data_, rva + kMemListCount);
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off =
            static_cast<std::size_t>(rva) + kMemListFirst + i * kMemDescriptorSize;
        out.push_back({rd_u64(data_, off + kMemStart), rd_u32(data_, off + kMemSize),
                       rd_u32(data_, off + kMemRva)});
    }
    std::sort(out.begin(), out.end());
    return out;
}

const MemoryRange* MiniDump::find_range(const std::vector<MemoryRange>& ranges,
                                        std::uint64_t addr) {
    for (const auto& r : ranges)
        if (addr >= r.start && addr < r.start + r.size) return &r;
    return nullptr;
}

Bytes MiniDump::read_mem(const std::vector<MemoryRange>& ranges, std::uint64_t addr,
                         std::size_t length) const {
    Bytes out;
    out.reserve(length);
    while (out.size() < length) {
        const MemoryRange* r = find_range(ranges, addr);
        if (!r) return {};  // a hole anywhere means the whole read is missing
        const std::size_t avail =
            std::min<std::size_t>(length - out.size(), r->start + r->size - addr);
        const std::size_t off = r->rva + static_cast<std::size_t>(addr - r->start);
        // A descriptor can point past the end of the dump file, so clamp to what
        // is actually stored.  This is also what keeps the loop finite: once the
        // clamp leaves nothing to copy the read fails outright.
        const std::size_t end = std::min(off + avail, data_.size());
        if (end <= off) return {};
        out.insert(out.end(), data_.begin() + static_cast<std::ptrdiff_t>(off),
                   data_.begin() + static_cast<std::ptrdiff_t>(end));
        addr += avail;
    }
    return out;
}

}  // namespace ksh::dump
