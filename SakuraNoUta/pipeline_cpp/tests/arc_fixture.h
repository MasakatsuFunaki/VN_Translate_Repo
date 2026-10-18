// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Synthetic BURIKO ARC20 containers for tests that need an archive on disk
// without touching game data.  Payloads go in raw: `read_entry` only
// decompresses a blob that carries the DSC magic.
#pragma once

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace snu::test_fixture {

inline Bytes make_arc(const std::vector<std::pair<std::string, Bytes>>& entries) {
    Bytes arc;
    const char magic[] = "BURIKO ARC20";
    arc.insert(arc.end(), magic, magic + 12);
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) arc.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    put32(static_cast<std::uint32_t>(entries.size()));
    std::uint32_t cursor = 0;
    for (const auto& [name, payload] : entries) {
        Bytes field(96, 0);
        std::memcpy(field.data(), name.data(), std::min<std::size_t>(name.size(), 95));
        arc.insert(arc.end(), field.begin(), field.end());
        put32(cursor);
        put32(static_cast<std::uint32_t>(payload.size()));
        for (int i = 0; i < 24; ++i) arc.push_back(0xEE);  // index-extra bytes
        cursor += static_cast<std::uint32_t>(payload.size());
    }
    for (const auto& [name, payload] : entries)
        arc.insert(arc.end(), payload.begin(), payload.end());
    return arc;
}

inline std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).u8string();
}

}  // namespace snu::test_fixture
