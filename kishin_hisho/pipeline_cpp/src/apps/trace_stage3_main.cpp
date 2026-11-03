// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// analysis/05_trace_stage3 -- follow stage-2's jump into stage 3 by reading
// [ebp+0x65D] out of the dump.
//
// stdout only.  Note the deliberate asymmetry: the stage-3 disasm header prints
// only when that memory was captured in the dump, while the other two headers
// print unconditionally.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "common/util.h"
#include "minidump/minidump.h"
#include "pe/disasm.h"
#include "pe/pe_image.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace ksh;

namespace {

// Stage-2 pops its own entry point into ebp; these are offsets within it.
constexpr std::uint32_t kStage2Ebp = 0x42830E;
constexpr std::uint32_t kJumpDeltaField = 0x65D;  // [ebp+0x65D] holds the jump delta
constexpr std::uint32_t kJumpBase = 0x67B;        // and the jump is relative to ebp+0x67B
constexpr std::size_t kDisasmBytes = 256;
constexpr std::size_t kFieldDumpBytes = 32;

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
    po::options_description desc("05_trace_stage3 -- follow stage-2 into stage 3");
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
        const std::vector<dump::MemoryRange> ranges = md.memory_ranges();

        auto disasm_at = [&](const Bytes& code, std::uint64_t addr) {
            for (const auto& ins : dis.disasm(code, addr)) print_line(pe::format_instruction(ins));
        };

        const std::uint32_t field_addr = kStage2Ebp + kJumpDeltaField;
        const Bytes field = md.read_mem(ranges, field_addr, 4);
        print_line("[ebp_stage2 + 0x65D] = [" + hexl(field_addr) +
                   "] = " + (field.empty() ? "NOT IN DUMP" : hex_lower(field.data(), field.size())));

        if (!field.empty()) {
            const std::uint32_t uval = pe::rd_u32(field, 0);
            const std::int32_t val = static_cast<std::int32_t>(uval);
            print_line("  signed=" + std::to_string(val) + " (0x" + hexu_signed(val, 8) +
                       ")  unsigned=0x" + hexu(uval, 8));

            // Wraps at 32 bits, exactly as the stub's `add ecx, [ebp+0x65D]` does.
            const std::uint32_t target = kStage2Ebp + kJumpBase + uval;
            print_line("  jmp target = 0x" + hexu(kStage2Ebp, 0) + " + 0x67B + 0x" +
                       hexu(uval, 0) + " = 0x" + hexu(target, 0));

            const Bytes chunk = md.read_mem(ranges, target, kDisasmBytes);
            if (!chunk.empty()) {
                print_line("\n--- Disasm at stage-3 entry 0x" + hexu(target, 0) + " ---");
                disasm_at(chunk, target);
            }
        }

        // The buffer stage-2 "copies" 0x5149 bytes into -- is it code?
        const std::uint32_t buf_start = kStage2Ebp + kJumpBase;
        print_line("\n--- 'Copied' buffer at 0x" + hexu(buf_start, 0) + " (first 256 bytes) ---");
        const Bytes buf = md.read_mem(ranges, buf_start, kDisasmBytes);
        if (!buf.empty()) disasm_at(buf, buf_start);

        print_line("\n--- Bytes at ebp_stage2+0x65D (length 32) ---");
        const Bytes raw = md.read_mem(ranges, field_addr, kFieldDumpBytes);
        if (!raw.empty()) print_line("  " + hex_bytes(raw));
        return 0;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
