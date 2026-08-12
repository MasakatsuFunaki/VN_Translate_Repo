// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// cp932_probe -- expose the CP932 decoder to a driver that sweeps the whole
// byte space and diffs the results against a reference codec.
//
// Protocol: one hex-encoded byte string per stdin line; prints the HEX of the
// UTF-8 decode, or the literal "INVALID", one line per input.  The output is
// hex because the inputs include NUL, LF and CR, which would otherwise break
// the line framing and produce spurious mismatches.
//
// This exists because the hand-written unit tests can only pin the values
// someone thought to write down -- the sweep pins all 48k of them.
#include <cstdio>
#include <iostream>
#include <string>

#include "common/util.h"

int main() {
    exm::setup_console_utf8();
    std::ios::sync_with_stdio(false);

    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        exm::Bytes raw;
        raw.reserve(line.size() / 2);
        bool ok = (line.size() % 2) == 0;
        for (std::size_t i = 0; ok && i + 1 < line.size(); i += 2) {
            auto nib = [&](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = nib(line[i]), lo = nib(line[i + 1]);
            if (hi < 0 || lo < 0) { ok = false; break; }
            raw.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        if (!ok) {
            std::fputs("INVALID\n", stdout);
            continue;
        }
        auto decoded = exm::cp932_to_utf8_strict(raw);
        if (!decoded) {
            std::fputs("INVALID\n", stdout);
            continue;
        }
        std::string hex;
        hex.reserve(decoded->size() * 2 + 1);
        for (unsigned char c : *decoded) {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02x", c);
            hex += buf;
        }
        hex += '\n';
        std::fputs(hex.c_str(), stdout);
    }
    std::fflush(stdout);
    return 0;
}
