// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// analysis/06_parse_recognizedisc -- read the export table of the dumped
// recognizedisc.dll and disassemble fnRecognizedisc, so the helper can hook it
//
// The input is the MAPPED image, not the on-disk file, so an RVA indexes the
// buffer directly -- no section translation.
#include <algorithm>
#include <string>
#include <vector>

#include "common/util.h"
#include "pe/disasm.h"
#include "pe/pe_image.h"

#include "apps/paths.h"

using namespace ksh;

namespace {

constexpr std::uint64_t kDllImageBase = 0x10000000;
constexpr std::size_t kElfanewOffset = 0x3C;
constexpr std::size_t kOptionalHeaderOffset = 24;  // from the PE signature
constexpr std::size_t kOptAddressOfEntryPoint = 16;
constexpr std::size_t kOptImageBase = 28;
constexpr std::size_t kOptExportDirRva = 96;   // DataDirectory[0].VirtualAddress
constexpr std::size_t kOptExportDirSize = 100;
// IMAGE_EXPORT_DIRECTORY field offsets.
constexpr std::size_t kExpNumberOfFunctions = 0x14;
constexpr std::size_t kExpNumberOfNames = 0x18;
constexpr std::size_t kExpAddressOfFunctions = 0x1C;
constexpr std::size_t kExpAddressOfNames = 0x20;
constexpr std::size_t kExpAddressOfNameOrdinals = 0x24;

constexpr const char* kTargetExport = "?fnRecognizedisc@@YAHXZ";
constexpr std::size_t kTargetDisasmBytes = 128;

// Export names in the dump are ASCII, so the bytes are taken as-is.
std::string read_c_string(const Bytes& data, std::size_t off) {
    const auto end = std::find(data.begin() + static_cast<std::ptrdiff_t>(off), data.end(), 0);
    if (end == data.end()) throw std::runtime_error("unterminated export name");
    return std::string(reinterpret_cast<const char*>(data.data() + off),
                       static_cast<std::size_t>(end - data.begin()) - off);
}

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = ksh::apps::po;

    std::string project, dump_override;
    po::options_description desc("06_parse_recognizedisc -- export table of recognizedisc.dll");
    desc.add(ksh::apps::common_options(project));
    desc.add_options()("dump", po::value(&dump_override),
                       "mapped image; default: analysis/dumps/recognizedisc_mapped.bin");
    po::variables_map vm;
    if (auto rc = ksh::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string path = dump_override.empty()
                                 ? project + "\\analysis\\dumps\\recognizedisc_mapped.bin"
                                 : dump_override;
    try {
        const Bytes data = read_file(path);
        using pe::rd_u16;
        using pe::rd_u32;

        const std::uint32_t pe_off = rd_u32(data, kElfanewOffset);
        const std::size_t opt = pe_off + kOptionalHeaderOffset;
        const std::uint32_t exp_rva = rd_u32(data, opt + kOptExportDirRva);
        const std::uint32_t exp_size = rd_u32(data, opt + kOptExportDirSize);

        print_line("PE @ 0x" + hexu(pe_off, 0) + ", OptHdr @ 0x" + hexu(opt, 0));
        print_line("Export Dir RVA 0x" + hexu(exp_rva, 0) + ", size 0x" + hexu(exp_size, 0));
        print_line("ImageBase 0x" + hexu(rd_u32(data, opt + kOptImageBase), 0));
        print_line("AEP RVA 0x" + hexu(rd_u32(data, opt + kOptAddressOfEntryPoint), 0));
        print_line("");

        const std::uint32_t n_funcs = rd_u32(data, exp_rva + kExpNumberOfFunctions);
        const std::uint32_t n_names = rd_u32(data, exp_rva + kExpNumberOfNames);
        const std::uint32_t funcs_rva = rd_u32(data, exp_rva + kExpAddressOfFunctions);
        const std::uint32_t names_rva = rd_u32(data, exp_rva + kExpAddressOfNames);
        const std::uint32_t ords_rva = rd_u32(data, exp_rva + kExpAddressOfNameOrdinals);

        print_line("# functions = " + std::to_string(n_funcs) + ", # names = " +
                   std::to_string(n_names));
        print_line("AddressOfFunctions    @ RVA 0x" + hexu(funcs_rva, 0));
        print_line("AddressOfNames        @ RVA 0x" + hexu(names_rva, 0));
        print_line("AddressOfNameOrdinals @ RVA 0x" + hexu(ords_rva, 0));
        print_line("");

        struct Export {
            std::string name;
            std::uint16_t ordinal_index;
            std::uint32_t rva;
        };
        std::vector<Export> exports;
        exports.reserve(n_names);
        for (std::uint32_t i = 0; i < n_names; ++i) {
            const std::uint32_t name_rva = rd_u32(data, names_rva + i * 4);
            const std::uint16_t ord_idx = rd_u16(data, ords_rva + i * 2);
            exports.push_back({read_c_string(data, name_rva), ord_idx,
                               rd_u32(data, funcs_rva + ord_idx * 4)});
        }

        print_line("Exports:");
        for (const auto& e : exports)
            print_line("  [" + std::to_string(e.ordinal_index) + "] RVA 0x" + hexu(e.rva, 8) +
                       " (VA 0x" + hexu(kDllImageBase + e.rva, 8) + ")  " + e.name);

        print_line("");
        const pe::Disassembler dis;
        for (const auto& e : exports) {
            if (e.name != kTargetExport) continue;
            const std::uint64_t va = kDllImageBase + e.rva;
            print_line("--- Disasm fnRecognizedisc @ RVA 0x" + hexu(e.rva, 0) + " (VA 0x" +
                       hexu(va, 0) + ") ---");
            const std::size_t n =
                e.rva < data.size() ? std::min(kTargetDisasmBytes, data.size() - e.rva) : 0;
            for (const auto& ins : dis.disasm(data.data() + e.rva, n, va))
                print_line(pe::format_instruction(ins));
            break;
        }
        return 0;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
