// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// cp932_probe -- expose the CP932 decoder to a driver that sweeps the whole
// byte space and diffs the results against a reference codec.
#include "common/util.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

namespace {

constexpr int kHexBaseOffset = 10;
constexpr unsigned int kNibbleShift = 4;
constexpr std::size_t kHexCharsPerByte = 2;
constexpr std::size_t kHexByteBufSize = 3;

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + kHexBaseOffset;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + kHexBaseOffset;
    }
    return -1;
}

}  // namespace

int main() {
    try {
        exm::setup_console_utf8();
        std::ios::sync_with_stdio(false);

        std::string line;
        while (std::getline(std::cin, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            exm::Bytes raw;
            raw.reserve(line.size() / kHexCharsPerByte);
            bool ok = (line.size() % kHexCharsPerByte) == 0;
            for (std::size_t i = 0; ok && i + 1 < line.size(); i += kHexCharsPerByte) {
                const int hi = hex_nibble(line[i]);
                const int lo = hex_nibble(line[i + 1]);
                if (hi < 0 || lo < 0) {
                    ok = false;
                    break;
                }
                raw.push_back(static_cast<std::uint8_t>(
                    (static_cast<unsigned int>(hi) << kNibbleShift) |
                    static_cast<unsigned int>(lo)));
            }
            if (!ok) {
                std::fputs("INVALID\n", stdout);
                continue;
            }
            const auto decoded = exm::cp932_to_utf8_strict(raw);
            if (!decoded) {
                std::fputs("INVALID\n", stdout);
                continue;
            }
            std::string hex;
            hex.reserve(decoded->size() * kHexCharsPerByte + 1);
            for (unsigned char c : *decoded) {
                char buf[kHexByteBufSize];
                std::snprintf(buf, sizeof(buf), "%02x", c);
                hex += buf;
            }
            hex += '\n';
            std::fputs(hex.c_str(), stdout);
        }
        std::fflush(stdout);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }
}
