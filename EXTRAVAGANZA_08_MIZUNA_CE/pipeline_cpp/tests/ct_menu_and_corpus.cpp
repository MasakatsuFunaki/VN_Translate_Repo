// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests verifying repository-resident artifacts for
// EXTRA_VA_MIZUNA_CE: MIZMENU1 menu_patch.bin format invariants and
// script_output/translated_text.json speaker coverage.
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "menu/menu_patcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kMaxWalkLevels = 8;
constexpr int kMemcmpEqual = 0;
constexpr std::size_t kBufferGrowthFactor = 2;
constexpr std::size_t kExpectedDistinctDialogueSpeakers = 29;
constexpr std::size_t kExpectedDialogueLineCount = 971;

fs::path CurrentTestExecutableDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path();
}

fs::path FindGameCheckoutRootFrom(fs::path start) {
    std::error_code ec;
    const fs::path menu_patch_name = fs::u8path("menu_patch.bin");
    const fs::path build_py_name = fs::u8path("build.py");
    const fs::path game_dir_name = fs::u8path("EXTRAVAGANZA_08_MIZUNA_CE");
    for (int i = 0; i < kMaxWalkLevels && !start.empty(); ++i) {
        if (fs::is_regular_file(start / menu_patch_name, ec) &&
            fs::is_regular_file(start / build_py_name, ec)) {
            return start;
        }
        const fs::path nested = start / game_dir_name;
        if (fs::is_regular_file(nested / menu_patch_name, ec)) {
            return nested;
        }
        if (!start.has_parent_path() || start.parent_path() == start) {
            break;
        }
        start = start.parent_path();
    }
    return {};
}

fs::path FindGameCheckoutRoot() {
    const fs::path from_exe = FindGameCheckoutRootFrom(CurrentTestExecutableDir());
    if (!from_exe.empty()) {
        return from_exe;
    }
    return FindGameCheckoutRootFrom(fs::current_path());
}

bool ContainsAsciiSubsequence(const std::vector<std::uint8_t>& haystack,
                              const std::uint8_t* needle,
                              std::size_t needle_len) {
    const auto it =
        std::search(haystack.begin(), haystack.end(), needle, needle + needle_len);
    return it != haystack.end();
}

TEST(MenuPatchBinContract, CommittedMenuPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path game_root = FindGameCheckoutRoot();
    ASSERT_FALSE(game_root.empty()) << "EXTRAVAGANZA_08_MIZUNA_CE checkout root not found.";
    const fs::path patch_bin_path = game_root / fs::u8path("menu_patch.bin");
    ASSERT_TRUE(fs::exists(patch_bin_path));

    const auto bytes = exm::read_file(patch_bin_path.u8string());
    ASSERT_GT(bytes.size(), exm::menu::kPatchMagicSize);
    EXPECT_EQ(
        std::memcmp(bytes.data(), exm::menu::kPatchMagic, exm::menu::kPatchMagicSize),
        kMemcmpEqual);

    const auto parsed = exm::menu::parse_patch_bin(bytes);
    EXPECT_EQ(parsed.version, exm::menu::kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_records.size(), exm::menu::kExpectedStandaloneFileCount);
    EXPECT_GT(parsed.gpk_records.size(), 0u);

    constexpr std::uint8_t kPackBmpNeedle[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    constexpr std::uint8_t kPngNeedle[] = {0x89u, 'P', 'N', 'G'};
    EXPECT_FALSE(ContainsAsciiSubsequence(bytes, kPackBmpNeedle, sizeof(kPackBmpNeedle)));
    EXPECT_FALSE(ContainsAsciiSubsequence(bytes, kPngNeedle, sizeof(kPngNeedle)));
}

TEST(CorpusSpeakerContract, ExtractedCorpusSpeakerCoverageAndGatePass) {
    const fs::path game_root = FindGameCheckoutRoot();
    ASSERT_FALSE(game_root.empty()) << "EXTRAVAGANZA_08_MIZUNA_CE checkout root not found.";
    const fs::path corpus_path =
        game_root / fs::u8path("script_output") / fs::u8path("translated_text.json");
    ASSERT_TRUE(fs::exists(corpus_path))
        << "script_output/translated_text.json not found at " << corpus_path.u8string();

    const auto& map = exm::translate::character_names();
    const auto root_val = exm::json_parse_file(corpus_path.u8string());
    ASSERT_TRUE(root_val.is_object());
    const auto& root = root_val.get_object();

    std::size_t dialogue_count = 0;
    std::set<std::string> dialogue_speakers;
    std::vector<std::string> unmapped_speakers;

    for (const auto& fkv : root) {
        ASSERT_TRUE(fkv.value().is_object());
        const auto& fobj = fkv.value().get_object();
        const auto* strings = fobj.if_contains("strings");
        ASSERT_NE(strings, nullptr);
        ASSERT_TRUE(strings->is_array());
        for (const auto& sv : strings->get_array()) {
            ASSERT_TRUE(sv.is_object());
            const auto& s = sv.get_object();
            if (std::string(s.at("type").get_string()) != "dialogue") {
                continue;
            }
            ++dialogue_count;
            const std::string text(s.at("text").get_string());
            const std::size_t crlf = text.find("\r\n");
            ASSERT_NE(crlf, std::string::npos) << "Dialogue entry missing CRLF: " << text;
            const std::string jp_speaker = exm::trim(text.substr(0, crlf));
            ASSERT_FALSE(jp_speaker.empty());
            dialogue_speakers.insert(jp_speaker);
            if (map.find(jp_speaker) == map.end()) {
                unmapped_speakers.push_back(jp_speaker);
            }
            const std::string resolved = exm::translate::extract_speaker(text);
            EXPECT_NE(resolved, "NARRATION") << "Failed to extract speaker from: " << text;
            EXPECT_EQ(resolved, map.at(jp_speaker));
        }
    }

    EXPECT_EQ(dialogue_count, kExpectedDialogueLineCount);
    ASSERT_EQ(dialogue_speakers.size(), kExpectedDistinctDialogueSpeakers);
    EXPECT_TRUE(unmapped_speakers.empty());

    const auto report = exm::translate::run_speaker_checks(root);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.dialogue_total, kExpectedDialogueLineCount);
    EXPECT_EQ(report.speakers_in_data, kExpectedDistinctDialogueSpeakers);
}

}  // namespace
