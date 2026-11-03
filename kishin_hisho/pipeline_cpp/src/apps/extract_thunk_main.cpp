// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// analysis/04_extract_thunk -- find the stack page AlphaROM jumped into and
// disassemble around the faulting EIP.
//
// stdout only: this one writes no log, so nothing here is a committed fixture.
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/util.h"
#include "minidump/minidump.h"
#include "pe/disasm.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace ksh;

namespace {

constexpr std::size_t kContextBytes = 32;    // shown before EIP
constexpr std::size_t kDisasmBytes = 128;    // shown after EIP
constexpr std::size_t kStage2DisasmBytes = 256;
constexpr std::size_t kRawDumpBytes = 64;
constexpr std::size_t kRawDumpStride = 16;

// A memory range prints as a bare tuple of DECIMAL ints --
// "(4360192, 4096, 262524)" -- or "None" when no range covers the address.
std::string range_repr(const dump::MemoryRange* r) {
    if (!r) return "None";
    return "(" + std::to_string(r->start) + ", " + std::to_string(r->size) + ", " +
           std::to_string(r->rva) + ")";
}

std::string first_dump_in(const std::string& dir) {
    std::vector<std::string> found;
    for (const auto& e : fs::directory_iterator(fs::u8path(dir))) {
        const std::string name = e.path().filename().u8string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".dmp") == 0)
            found.push_back(e.path().u8string());
    }
    if (found.empty()) throw std::runtime_error("no .dmp files in " + dir);
    std::sort(found.begin(), found.end());
    return found.front();
}

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = ksh::apps::po;

    std::string project, dump_override;
    po::options_description desc("04_extract_thunk -- disassemble the stack thunk");
    desc.add(ksh::apps::common_options(project));
    desc.add_options()("dump", po::value(&dump_override),
                       "dump file; default: the first analysis/dumps/*.dmp, sorted");
    po::variables_map vm;
    if (auto rc = ksh::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string root = project + "\\analysis";
    try {
        const std::string dump_path =
            dump_override.empty() ? first_dump_in(root + "\\dumps") : dump_override;

        dump::MiniDump md(read_file(dump_path));
        const pe::Disassembler dis;

        const auto exc = md.exception();
        if (!exc || !exc->context) throw std::runtime_error("dump has no 32-bit CONTEXT");
        const dump::Context32& c = *exc->context;

        print_line("EIP=" + hexl(c.eip) + "  ESP=" + hexl(c.esp) + "  EBP=" + hexl(c.ebp) +
                   "  EBX=" + hexl(c.ebx) + "  ECX=" + hexl(c.ecx));

        const std::vector<dump::MemoryRange> ranges = md.memory_ranges();
        print_line("\nMemoryListStream: " + std::to_string(ranges.size()) + " ranges");

        const dump::MemoryRange* hit = dump::MiniDump::find_range(ranges, c.eip);
        print_line("\nEIP=" + hexl(c.eip) + " page: " + range_repr(hit));

        if (hit) {
            const Bytes& data = md.raw();
            const std::size_t page_off = hit->rva;
            const std::size_t rsize = hit->size;
            // A descriptor can claim more bytes than the dump file actually
            // stores, so the page is clamped at EOF and every read below is
            // bounded by that clamped length, not by the declared rsize.
            const std::size_t page_len =
                page_off < data.size() ? std::min<std::size_t>(rsize, data.size() - page_off) : 0;
            print_line("Page covers " + hexl(hit->start) + "-" + hexl(hit->start + rsize) +
                       ", size 0x" + hexu(rsize, 0));

            const std::size_t eip_off = static_cast<std::size_t>(c.eip - hit->start);
            const std::size_t pre = eip_off > kContextBytes ? eip_off - kContextBytes : 0;
            const std::size_t post = std::min({rsize, eip_off + kDisasmBytes, page_len});
            print_line("\n--- Disasm from " + hexl(hit->start + pre) + " (EIP-" +
                       std::to_string(eip_off - pre) + " bytes) ---");
            if (post > pre)
                for (const auto& ins : dis.disasm(data.data() + page_off + pre, post - pre,
                                                  hit->start + pre))
                    print_line(pe::format_instruction(ins) +
                               (ins.address == c.eip ? "  >>> CRASH HERE" : ""));

            print_line("\n--- Raw bytes around EIP (" + hexl(c.eip) + ") ---");
            for (std::size_t i = 0; i < kRawDumpBytes; i += kRawDumpStride) {
                // `off` is signed on purpose: when EIP sits within 32 bytes of
                // the page start it goes negative, and the line is skipped
                // instead of wrapping round to a huge index.
                const std::ptrdiff_t off = static_cast<std::ptrdiff_t>(eip_off) -
                                           static_cast<std::ptrdiff_t>(kContextBytes) +
                                           static_cast<std::ptrdiff_t>(i);
                if (off < 0 || static_cast<std::size_t>(off) + kRawDumpStride > rsize) continue;
                const std::size_t start = static_cast<std::size_t>(off);
                const std::size_t n =
                    start < page_len ? std::min(kRawDumpStride, page_len - start) : 0;
                print_line("  " + hexu(hit->start + static_cast<std::uint64_t>(off), 8) + "  " +
                           hex_bytes(data.data() + page_off + start, n));
            }
        }

        // The pointer the byte-walker was reading when the AV fired -- usually a
        // kernel32 page, which minidumps do not capture.
        print_line("\n--- ECX=" + hexl(c.ecx) + " (the read pointer when AV happened) page lookup ---");
        print_line("  page: " + range_repr(dump::MiniDump::find_range(ranges, c.ecx)));

        print_line("\n--- EBP=" + hexl(c.ebp) +
                   " (next-stage entry from static analysis) page lookup ---");
        hit = dump::MiniDump::find_range(ranges, c.ebp);
        print_line("  page: " + range_repr(hit));
        if (hit) {
            const Bytes& data = md.raw();
            const std::size_t page_len =
                hit->rva < data.size() ? std::min<std::size_t>(hit->size, data.size() - hit->rva) : 0;
            const std::size_t in_page = static_cast<std::size_t>(c.ebp - hit->start);
            const std::size_t n =
                in_page < page_len ? std::min(kStage2DisasmBytes, page_len - in_page) : 0;
            print_line("\n--- Disasm at " + hexl(c.ebp) + " (stage-2 stub, post-decryption) ---");
            for (const auto& ins : dis.disasm(data.data() + hit->rva + in_page, n, c.ebp))
                print_line(pe::format_instruction(ins));
        }
        return 0;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
