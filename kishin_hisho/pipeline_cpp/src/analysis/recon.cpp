// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Body of the 01_recon report -- see reports.h.
#include "reports.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "common/util.h"
#include "pe/disasm.h"
#include "pe/pe_image.h"

namespace ksh::analysis {

namespace {

// Section characteristic bits, in the order the report's Flags column lists them.
struct FlagBit {
    std::uint32_t mask;
    const char* name;
};
constexpr FlagBit kSectionFlags[] = {
    {0x20000000, "EXEC"},     {0x40000000, "READ"},     {0x80000000, "WRITE"},
    {0x00000020, "CODE"},     {0x00000040, "INITDATA"}, {0x00000080, "UNINITDATA"},
};

// Signatures scanned for at the end of the report.  The label is the byte-string
// literal the report prints, stored verbatim beside the hex it decodes to --
// deriving one from the other at compile time would be pure incidental
// complexity for thirteen fixed entries.
struct Signature {
    const char* label;
    const char* hex;
};
constexpr Signature kSignatures[] = {
    {"b'SETTEC'", "534554544543"},
    {"b'alpha'", "616c706861"},
    {"b'AlphaROM'", "416c706861524f4d"},
    {"b'\\\\\\\\.\\\\'", "5c5c2e5c"},
    {"b'NTICE'", "4e54494345"},
    {"b'SICE'", "53494345"},
    {"b'FILEM'", "46494c454d"},
    {"b'TRW'", "545257"},
    {"b'SoftIce'", "536f6674496365"},
    {"b'DBKKernel'", "44424b4b65726e656c"},
    {"b'Syser'", "5379736572"},
    {"b'OllyDbg'", "4f6c6c79446267"},
    {"b'DRM'", "44524d"},
};

constexpr std::size_t kAepDisasmBytes = 256;
constexpr std::size_t kMaxImportsListed = 8;
// The scan stops once the hit count passes this, so a signature lists at most 6.
constexpr std::size_t kMaxSigHits = 5;

Bytes from_hex(const char* hex) {
    Bytes out;
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        auto nib = [](char c) {
            return c >= 'a' ? c - 'a' + 10 : (c >= 'A' ? c - 'A' + 10 : c - '0');
        };
        out.push_back(static_cast<std::uint8_t>((nib(p[0]) << 4) | nib(p[1])));
    }
    return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

}  // namespace

std::string recon_report(const std::string& exe_path) {
    pe::Image pe(read_file(exe_path));
    const pe::Disassembler md;

    std::vector<std::string> out;
    auto p = [&](std::string s = {}) { out.push_back(std::move(s)); };

    p("=== PE summary ===");
    p("Machine:            0x" + hexu(pe.machine(), 4));
    p("NumberOfSections:   " + std::to_string(pe.number_of_sections()));
    p("TimeDateStamp:      0x" + hexu(pe.time_date_stamp(), 8));
    p("Characteristics:    0x" + hexu(pe.characteristics(), 4));
    p("ImageBase:          0x" + hexu(pe.image_base(), 8));
    p("SizeOfImage:        0x" + hexu(pe.size_of_image(), 8));
    p("AddressOfEntryPoint:0x" + hexu(pe.address_of_entry_point(), 8) + "  (VA 0x" +
      hexu(static_cast<std::uint64_t>(pe.image_base()) + pe.address_of_entry_point(), 8) + ")");
    p("DllCharacteristics: 0x" + hexu(pe.dll_characteristics(), 4) +
      "  (NX_COMPAT=0x100, DYNAMIC_BASE=0x40)");
    p();

    p("=== Sections ===");
    p(ljust("Name", 10) + " " + ljust("VA", 10) + " " + ljust("VSize", 10) + " " +
      ljust("RawOff", 10) + " " + ljust("RawSize", 10) + " " + ljust("Char", 12) + " Flags");
    for (const auto& s : pe.sections()) {
        const std::string name = s.name.empty() ? "(anon)" : s.name;
        std::vector<std::string> flags;
        for (const auto& f : kSectionFlags)
            if (s.characteristics & f.mask) flags.emplace_back(f.name);
        p(ljust(name, 10) + " 0x" +
          hexu(static_cast<std::uint64_t>(pe.image_base()) + s.virtual_address, 8) + " 0x" +
          hexu(s.virtual_size, 8) + " 0x" + hexu(s.pointer_to_raw_data, 8) + " 0x" +
          hexu(s.size_of_raw_data, 8) + " 0x" + hexu(s.characteristics, 8) + "   " +
          join(flags, ","));
    }
    p();

    p("=== Imports (resolved by Windows loader BEFORE entry point runs) ===");
    if (!pe.imports().empty()) {
        for (const auto& imp : pe.imports()) {
            p("  " + imp.dll + "  (" + std::to_string(imp.imports.size()) + " funcs)");
            for (std::size_t i = 0; i < imp.imports.size() && i < kMaxImportsListed; ++i) {
                const auto& f = imp.imports[i];
                p("      " + (f.name.empty() ? "ord_" + std::to_string(f.ordinal) : f.name));
            }
            if (imp.imports.size() > kMaxImportsListed)
                p("      ... +" + std::to_string(imp.imports.size() - kMaxImportsListed) + " more");
        }
    } else {
        p("  (no static imports \xE2\x80\x94 that's a strong AlphaROM signature; imports are "
          "resolved at runtime by the stub)");
    }
    p();

    p("=== TLS callbacks ===");
    if (const auto& tls = pe.tls()) {
        p("  StartAddressOfRawData: 0x" + hexu(tls->start_address_of_raw_data, 8));
        p("  EndAddressOfRawData:   0x" + hexu(tls->end_address_of_raw_data, 8));
        p("  AddressOfIndex:        0x" + hexu(tls->address_of_index, 8));
        p("  AddressOfCallBacks:    0x" + hexu(tls->address_of_callbacks, 8));
        if (tls->address_of_callbacks) {
            const Bytes& data = pe.memory_mapped_image();
            std::size_t idx = tls->address_of_callbacks - pe.image_base();
            std::vector<std::string> cbs;
            while (idx + 4 <= data.size()) {
                const std::uint32_t val = pe::rd_u32(data, idx);
                if (!val) break;
                cbs.emplace_back(hexl(val));
                idx += 4;
            }
            p("  Callbacks: [" + join(cbs, ", ") + "]");
        }
    } else {
        p("  (no TLS directory)");
    }
    p();

    p("=== Disassembly @ entry point (first 256 bytes) ===");
    {
        const std::uint32_t aep_rva = pe.address_of_entry_point();
        const std::uint64_t aep_va = static_cast<std::uint64_t>(pe.image_base()) + aep_rva;
        const Bytes& mapped = pe.memory_mapped_image();
        const std::size_t start = std::min<std::size_t>(aep_rva, mapped.size());
        const std::size_t end = std::min(start + kAepDisasmBytes, mapped.size());
        for (const auto& ins : md.disasm(mapped.data() + start, end - start, aep_va))
            p(pe::format_instruction(ins));
    }

    p();
    p("=== Strings of interest in the exe ===");
    {
        const Bytes& raw = pe.raw();
        for (const auto& sig : kSignatures) {
            const Bytes needle = from_hex(sig.hex);
            std::vector<std::string> hits;
            std::size_t idx = 0;
            while (idx < raw.size()) {
                const auto it = std::search(raw.begin() + static_cast<std::ptrdiff_t>(idx),
                                            raw.end(), needle.begin(), needle.end());
                if (it == raw.end()) break;
                const std::size_t j = static_cast<std::size_t>(it - raw.begin());
                hits.push_back("'0x" + hexu(j, 0) + "'");
                idx = j + 1;
                if (hits.size() > kMaxSigHits) break;
            }
            if (!hits.empty()) p("  " + ljust(sig.label, 25) + " @ [" + join(hits, ", ") + "]");
        }
    }

    return join(out, "\n");
}

}  // namespace ksh::analysis
