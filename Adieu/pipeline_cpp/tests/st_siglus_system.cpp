// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end system verification of the SiglusEngine Scene.pck reader,
// unconditional SS_V2 string decoder, and contextual speaker attribution
// against the shipped game archive when VN_GAME_DIR is configured.
#include "common/util.h"
#include "extract/extractor.h"
#include "siglus/scene_pck.h"
#include "siglus/siglus_decoder.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace bj = boost::json;
using namespace adi;
using namespace adi::siglus;

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kExpectedScenePckBytes = 2182337u;
constexpr std::uint32_t kExpectedHeaderSize = 0x5Cu;
constexpr std::size_t kExpectedMacroCount = 24u;
constexpr std::size_t kExpectedSceneCount = 194u;
constexpr std::uint64_t kExpectedFirstChunkOffset = 0x1B46u;
constexpr std::uint32_t kExpectedScene0RelOffset = 0u;
constexpr std::uint32_t kExpectedScene0CompSize = 2725u;
constexpr std::size_t kExpectedScene0DecompSize = 13907u;
constexpr std::uint32_t kExpectedScene1CompSize = 8818u;
constexpr std::size_t kExpectedScene1DecompSize = 42777u;
constexpr std::size_t kExpectedLargestDecompSize = 127491u;
constexpr int kExtractExitOk = 0;
constexpr std::size_t kMinExpectedDialogueLines = 1000u;

constexpr std::array<std::uint32_t, SECTION_COUNT> kExpectedSectionOffsets = {
    0x5Cu,  0x5Cu,  0x5Cu,   0x5Cu,   0x11Cu, 0x1DCu,
    0x434u, 0xA44u, 0x1536u, 0x1B46u, 0x1u};

constexpr std::array<std::uint32_t, SECTION_COUNT> kExpectedSectionCounts = {
    0u, 0u, 0u, 24u, 24u, 24u, 194u, 194u, 194u, 194u, 614u};

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        dir_ = fs::temp_directory_path() /
               fs::u8path("adi_st_siglus_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return dir_; }

private:
    fs::path dir_;
};

std::string wide_env_to_utf8(const wchar_t* name) {
    const wchar_t* val = _wgetenv(name);
    if (val == nullptr || *val == L'\0') {
        return {};
    }
    const int wlen = static_cast<int>(std::wcslen(val));
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, val, wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error(
            "WideCharToMultiByte failed converting VN_GAME_DIR to UTF-8");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, val, wlen, out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error(
            "WideCharToMultiByte produced an unexpected UTF-8 byte count");
    }
    return out;
}

std::optional<std::string> find_scene_pck_path() {
    const std::string env_dir = wide_env_to_utf8(L"VN_GAME_DIR");
    if (env_dir.empty()) {
        return std::nullopt;
    }
    const fs::path base = fs::u8path(env_dir);
    std::error_code ec;
    for (const fs::path& candidate : {
             base / "StartData" / "GameData" / "Scene.pck",
             base / "GameData" / "Scene.pck",
             base / "Scene.pck",
         }) {
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.u8string();
        }
    }
    return std::nullopt;
}

bool contains_string(const std::vector<std::string>& pool, const std::string& needle) {
    return std::find(pool.begin(), pool.end(), needle) != pool.end();
}

}  // namespace

TEST(ScenePck, HeaderAndSectionLayout) {
    const auto pck_path = find_scene_pck_path();
    if (!pck_path) {
        GTEST_SKIP() << "VN_GAME_DIR unset or Scene.pck not found";
    }
    const Bytes data = read_file(*pck_path);
    EXPECT_EQ(data.size(), kExpectedScenePckBytes);
    const Header h = parse_header(data);
    EXPECT_EQ(h.header_size, kExpectedHeaderSize);

    for (int i = 0; i < SECTION_COUNT; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        EXPECT_EQ(h.sections[i].offset, kExpectedSectionOffsets[idx]) << "section " << i;
        EXPECT_EQ(h.sections[i].count, kExpectedSectionCounts[idx]) << "section " << i;
    }

    const auto macros = read_name_table(data, h.sections[SEC_MACRO_NAME_IDX].offset,
                                        h.sections[SEC_MACRO_NAME_IDX].count,
                                        h.sections[SEC_MACRO_NAME_BLOB].offset);
    EXPECT_EQ(macros.size(), kExpectedMacroCount);
    const auto names = read_name_table(data, h.sections[SEC_SCRIPT_NAME_IDX].offset,
                                       h.sections[SEC_SCRIPT_NAME_IDX].count,
                                       h.sections[SEC_SCRIPT_NAME_BLOB].offset);
    ASSERT_EQ(names.size(), kExpectedSceneCount);
    EXPECT_EQ(names[0], "_cg_mode");

    const auto meta = read_script_meta(data, h, names);
    EXPECT_EQ(meta[0].abs_offset, kExpectedFirstChunkOffset);
}

TEST(SiglusDecoder, RealScenePckScene0) {
    const auto pck_path = find_scene_pck_path();
    if (!pck_path) {
        GTEST_SKIP() << "VN_GAME_DIR unset or Scene.pck not found";
    }
    const Bytes data = read_file(*pck_path);
    const Header h = parse_header(data);
    const auto names = read_name_table(data, h.sections[SEC_SCRIPT_NAME_IDX].offset,
                                       h.sections[SEC_SCRIPT_NAME_IDX].count,
                                       h.sections[SEC_SCRIPT_NAME_BLOB].offset);
    const auto meta = read_script_meta(data, h, names);
    ASSERT_EQ(meta.size(), kExpectedSceneCount);
    EXPECT_EQ(meta[0].name, "_cg_mode");
    EXPECT_EQ(meta[0].rel_offset, kExpectedScene0RelOffset);
    EXPECT_EQ(meta[0].size, kExpectedScene0CompSize);

    const Bytes chunk0 = slice_clamped(data, meta[0].abs_offset, meta[0].size);
    EXPECT_EQ(decode_scene_chunk(chunk0, ADIEU_PRIVATE_KEY.data()).size(),
              kExpectedScene0DecompSize);

    EXPECT_EQ(meta[1].size, kExpectedScene1CompSize);
    const Bytes chunk1 = slice_clamped(data, meta[1].abs_offset, meta[1].size);
    EXPECT_EQ(decode_scene_chunk(chunk1, ADIEU_PRIVATE_KEY.data()).size(),
              kExpectedScene1DecompSize);

    std::size_t largest = 0;
    for (const auto& m : meta) {
        largest = std::max(largest,
                           decode_scene_chunk(slice_clamped(data, m.abs_offset, m.size),
                                              ADIEU_PRIVATE_KEY.data())
                               .size());
    }
    EXPECT_EQ(largest, kExpectedLargestDecompSize);
}

TEST(SiglusDecoder, RealScenePckSsV2Verdicts) {
    const auto pck_path = find_scene_pck_path();
    if (!pck_path) {
        GTEST_SKIP() << "VN_GAME_DIR unset or Scene.pck not found";
    }
    const Bytes data = read_file(*pck_path);
    const Header h = parse_header(data);
    const auto names = read_name_table(data, h.sections[SEC_SCRIPT_NAME_IDX].offset,
                                       h.sections[SEC_SCRIPT_NAME_IDX].count,
                                       h.sections[SEC_SCRIPT_NAME_BLOB].offset);
    const auto meta = read_script_meta(data, h, names);
    ASSERT_EQ(meta.size(), kExpectedSceneCount);

    std::size_t encrypted_ascii_leaks = 0;
    bool start_has_protagonist_defaults = false;
    bool menu_has_protagonist_defaults = false;

    for (const auto& m : meta) {
        const Bytes d = decode_scene_chunk(slice_clamped(data, m.abs_offset, m.size),
                                           ADIEU_PRIVATE_KEY.data());
        const auto strings = parse_scene_strings(d);
        EXPECT_FALSE(strings.empty()) << m.name;

        for (const auto& s : strings) {
            // "white" XORed with 0x7087 produces "烰烯烮烳烢" when SS_V2 is skipped.
            if (s.find("烰烯") != std::string::npos ||
                s.find("烿烜烝") != std::string::npos) {
                ++encrypted_ascii_leaks;
            }
        }

        if (m.name == "_start") {
            start_has_protagonist_defaults =
                contains_string(strings, "人見") && contains_string(strings, "広介");
        }
        if (m.name == "_menu") {
            menu_has_protagonist_defaults =
                contains_string(strings, "人見") && contains_string(strings, "広介");
        }
    }

    EXPECT_EQ(encrypted_ascii_leaks, 0u);
    EXPECT_TRUE(start_has_protagonist_defaults);
    EXPECT_TRUE(menu_has_protagonist_defaults);
}

TEST(SiglusDecoder, RealScenePckExtractionAndSpeakerAttribution) {
    const auto pck_path = find_scene_pck_path();
    if (!pck_path) {
        GTEST_SKIP() << "VN_GAME_DIR unset or Scene.pck not found";
    }

    // Read retail Scene.pck directly from the read-only game directory while
    // writing extracted_text.json exclusively inside an ephemeral ScopedTempDir.
    const std::string game_dir = fs::u8path(*pck_path).parent_path().u8string();
    ScopedTempDir scratch;
    const std::string out_json = (scratch.path() / "extracted_text.json").u8string();

    ASSERT_EQ(extract::run_extract(game_dir, out_json), kExtractExitOk);
    const bj::object all_data = json_parse_file(out_json).get_object();
    ASSERT_EQ(all_data.size(), kExpectedSceneCount);

    std::size_t dialogue_count = 0;
    std::size_t unattributed_dialogues = 0;
    std::set<std::string> speakers;

    for (const auto& kv : all_data) {
        const auto& strings = kv.value().get_object().at("strings").get_array();
        for (const auto& sv : strings) {
            const auto& entry = sv.get_object();
            const std::string text(entry.at("text").get_string());
            const std::string type(entry.at("type").get_string());
            const std::string speaker(entry.at("speaker").get_string());

            if (text == "＊Ａ" || text == "＊Ｂ") {
                EXPECT_EQ(type, "other");
                EXPECT_FALSE(entry.at("has_jp").get_bool());
            }
            if (type == "dialogue") {
                ++dialogue_count;
                if (speaker.empty()) {
                    ++unattributed_dialogues;
                } else {
                    speakers.insert(speaker);
                }
            }
        }
    }

    EXPECT_GT(dialogue_count, kMinExpectedDialogueLines);
    EXPECT_EQ(unattributed_dialogues, 0u);
    EXPECT_GT(speakers.count("人見"), 0u);
    EXPECT_GT(speakers.count("睦　月"), 0u);
    EXPECT_GT(speakers.count("まひる"), 0u);
    EXPECT_GT(speakers.count("こより"), 0u);
    EXPECT_GT(speakers.count("となえ"), 0u);
}
