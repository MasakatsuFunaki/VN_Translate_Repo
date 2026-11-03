// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Body of the 03_parse_dump report -- see reports.h.
#include "reports.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "common/util.h"
#include "minidump/minidump.h"

namespace fs = std::filesystem;

namespace ksh::analysis {

namespace {

constexpr std::uint32_t kAccessViolation = 0xC0000005;

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::string basename_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

std::string dump_report(const std::string& path) {
    std::vector<std::string> out;
    auto p = [&](std::string s = {}) { out.push_back(std::move(s)); };

    const Bytes data = read_file(path);
    p("=== " + fs::u8path(path).filename().u8string() + " (" + std::to_string(data.size()) +
      " bytes) ===");

    dump::MiniDump md(data);
    if (!md.is_minidump()) {
        p("!! not a minidump (sig=" +
          std::string(reinterpret_cast<const char*>(data.data()),
                      std::min<std::size_t>(4, data.size())) +
          ")");
        return join(out, "\n");
    }

    p("Version=0x" + hexu(md.version(), 8) + "  NumStreams=" +
      std::to_string(md.number_of_streams()) + "  StreamDir@0x" +
      hexu(md.stream_directory_rva(), 0));

    // A list of tuples in stream-DIRECTORY order, not sorted by stream type.
    {
        std::vector<std::string> items;
        for (const auto& [type, ref] : md.streams_in_order())
            items.push_back("('" + dump::stream_type_name(type) + "', " +
                            std::to_string(ref.size) + ", '" + hexl(ref.rva) + "')");
        p("Streams: [" + join(items, ", ") + "]");
    }
    p();

    const auto exc = md.exception();
    if (exc) {
        p("=== ExceptionStream ===");
        p("ThreadId:        0x" + hexu(exc->thread_id, 0));
        p("ExceptionCode:   0x" + hexu(exc->code, 8) + "  (" + dump::exception_name(exc->code) +
          ")");
        p("ExceptionFlags:  0x" + hexu(exc->flags, 8));
        p("ExceptionAddress: 0x" + hexu(exc->address, 16));
        p("NumberParameters: " + std::to_string(exc->number_parameters));
        for (std::size_t i = 0; i < exc->parameters.size(); ++i) {
            // For an AV: param[0] is the access type, param[1] the fault address.
            std::string extra;
            if (exc->code == kAccessViolation && exc->number_parameters >= 2) {
                if (i == 0) {
                    switch (exc->parameters[0]) {
                    case 0: extra = "(READ)"; break;
                    case 1: extra = "(WRITE)"; break;
                    case 8: extra = "(DEP/EXEC)"; break;
                    default: break;
                    }
                } else if (i == 1) {
                    extra = "(fault address)";
                }
            }
            p("  param[" + std::to_string(i) + "]: 0x" + hexu(exc->parameters[i], 16) + "  " +
              extra);
        }
        p("ThreadContext: size=0x" + hexu(exc->context_size, 0) + " @ 0x" +
          hexu(exc->context_rva, 0));

        if (const auto& c = exc->context) {
            p("--- 32-bit CONTEXT ---");
            p("  ContextFlags=0x" + hexu(c->context_flags, 8));
            p("  EAX=0x" + hexu(c->eax, 8) + "  EBX=0x" + hexu(c->ebx, 8) + "  ECX=0x" +
              hexu(c->ecx, 8) + "  EDX=0x" + hexu(c->edx, 8));
            p("  ESI=0x" + hexu(c->esi, 8) + "  EDI=0x" + hexu(c->edi, 8) + "  EBP=0x" +
              hexu(c->ebp, 8) + "  ESP=0x" + hexu(c->esp, 8));
            p("  EIP=0x" + hexu(c->eip, 8) + "  CS=0x" + hexu(c->cs, 4) + "  SS=0x" +
              hexu(c->ss, 4) + "  EFLAGS=0x" + hexu(c->eflags, 8));
            p("  DR0=0x" + hexu(c->dr0, 8) + "  DR1=0x" + hexu(c->dr1, 8) + "  DR2=0x" +
              hexu(c->dr2, 8) + "  DR3=0x" + hexu(c->dr3, 8));
            p("  DR6=0x" + hexu(c->dr6, 8) + "  DR7=0x" + hexu(c->dr7, 8));
        }
        p();
    }

    const std::vector<dump::Module> modules = md.modules();
    if (!modules.empty()) {
        p("=== Loaded modules ===");
        for (const auto& m : modules)
            p("  0x" + hexu(m.base, 16) + "-0x" + hexu(m.base + m.size, 16) + " TS=0x" +
              hexu(m.time_date_stamp, 8) + "  " + m.name);
        p();
    }

    // All three are required: the exception stream and the module list to have
    // something to resolve, and the register context because EIP comes from it.
    // A dump with an exception stream but no context has nothing to print here.
    if (exc && !modules.empty() && exc->context) {
        const std::pair<const char*, std::uint64_t> targets[] = {
            {"ExceptionAddress", exc->address},
            {"EIP", exc->context->eip},
        };
        for (const auto& [label, addr] : targets) {
            bool found = false;
            for (const auto& m : modules) {
                if (addr >= m.base && addr < m.base + m.size) {
                    p(std::string(label) + " 0x" + hexu(addr, 0) + "  \xE2\x86\x92  " +
                      basename_of(m.name) + "+0x" + hexu(addr - m.base, 0) +
                      "  (in module range 0x" + hexu(m.base, 0) + "-0x" +
                      hexu(m.base + m.size, 0) + ")");
                    found = true;
                    break;
                }
            }
            if (!found)
                p(std::string(label) + " 0x" + hexu(addr, 0) + "  \xE2\x86\x92  NOT in any loaded module");
        }
    }

    return join(out, "\n");
}

}  // namespace ksh::analysis
