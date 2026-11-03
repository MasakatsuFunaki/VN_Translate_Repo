// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// analysis/02_decrypt_devnames -- decrypt the AlphaROM anti-debug device-name
// table.
//
// The stub does `add byte 0x44` over 0x49 bytes at ebp+0x2C5, i.e. file offset
// 0x272C5 / RVA 0x282C5.  stdout only; no log file.
#include <algorithm>
#include <string>
#include <vector>

#include "common/util.h"
#include "pe/pe_image.h"

#include "apps/paths.h"

using namespace ksh;

namespace {

constexpr std::uint32_t kStubBaseRva = 0x28000;
constexpr std::uint32_t kBufferOffsetInStub = 0x2C5;
constexpr std::uint32_t kStubBaseFileOffset = 0x27000;
constexpr std::size_t kBufferLength = 0x49;
constexpr std::uint8_t kXorKey = 0x44;  // an ADD, not an XOR -- the stub's name

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = ksh::apps::po;

    std::string project, exe_override;
    po::options_description desc("02_decrypt_devnames -- AlphaROM device-name table");
    desc.add(ksh::apps::common_options(project));
    desc.add_options()("exe", po::value(&exe_override),
                       "read this exe instead of analysis/samples/KishinHisho_orig.exe");
    po::variables_map vm;
    if (auto rc = ksh::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string exe_path = exe_override.empty()
                                     ? project + "\\analysis\\samples\\KishinHisho_orig.exe"
                                     : exe_override;
    try {
        pe::Image image(read_file(exe_path));
        const Bytes& mapped = image.memory_mapped_image();

        const std::size_t buf_rva = kStubBaseRva + kBufferOffsetInStub;
        Bytes encrypted;
        if (buf_rva < mapped.size())
            encrypted.assign(mapped.begin() + static_cast<std::ptrdiff_t>(buf_rva),
                             mapped.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min(buf_rva + kBufferLength, mapped.size())));
        Bytes decrypted;
        decrypted.reserve(encrypted.size());
        for (std::uint8_t b : encrypted)
            decrypted.push_back(static_cast<std::uint8_t>((b + kXorKey) & 0xFF));

        print_line("=== Encrypted bytes (file offset 0x" +
                   hexu(kStubBaseFileOffset + kBufferOffsetInStub, 0) + ", len 0x" +
                   hexu(kBufferLength, 0) + ") ===");
        print_line(hex_bytes(encrypted));
        print_line("");
        print_line("=== Decrypted (add 0x44) ===");
        print_line(bytes_repr(decrypted));
        print_line("");
        print_line("=== As null-terminated strings ===");
        for (std::size_t i = 0; i < decrypted.size();) {
            const auto it = std::find(decrypted.begin() + static_cast<std::ptrdiff_t>(i),
                                      decrypted.end(), 0);
            if (it == decrypted.end()) break;  // trailing bytes with no NUL are not a string
            const auto end = static_cast<std::size_t>(it - decrypted.begin());
            if (end > i)
                print_line("  [" + hexu(i, 2) + "] " +
                           bytes_repr(decrypted.data() + i, end - i));
            i = end + 1;
        }
        return 0;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
