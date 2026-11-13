// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Extractor unit tests covering the .ast Lua line walker's capture rules and
// multi-archive script override precedence.
#include "common/util.h"
#include "extract/extractor.h"
#include "pfs_fixture.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;

constexpr std::size_t kExpectedSampleCaptureCount = 4;
constexpr int kExpectedNamePlateLineNumber = 9;
constexpr std::size_t kTwoEntries = 2;
constexpr std::size_t kThreeEntries = 3;
constexpr std::size_t kFourEntries = 4;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

snt::Bytes as_bytes(const std::string& s) {
    return snt::Bytes(s.begin(), s.end());
}

// One .ast body wrapping `body` in the ja block the walker looks for.
std::string ast_with(const std::string& body) {
    return "astver = 2.0\nast = {\n\tblock_00001 = {\n\t\ttext = {\n\t\t\tja = {\n"
           "\t\t\t\t{\n" +
           body + "\t\t\t\t},\n\t\t\t},\n\t\t},\n\t}\n}\n";
}

// A minimal but structurally faithful .ast body.
constexpr const char* kSampleAst = R"(astver = 2.0
ast = {
	block_00001 = {
		{"text"},
		text = {
			vo = { {"vo", file="a01"} },
			ja = {
				{
					name = {"直哉"},
					"「おはよう」",
					"continuation 続き",
					{"rt2"},
				},
			},
		},
		{"savetitle", text="第一章"},
		{"bg", file="背景.png"},
	},
}
)";

}  // namespace

TEST(Extract, rejects_non_ast_payload) {
    EXPECT_FALSE(snt::extract::extract_one(as_bytes("not an ast file")).has_value());
    EXPECT_FALSE(snt::extract::extract_one(snt::Bytes{}).has_value());
}

TEST(Extract, captures_name_dialogue_narrative_and_title) {
    const auto entries = snt::extract::extract_one(as_bytes(kSampleAst));
    ASSERT_TRUE(entries.has_value());

    std::vector<std::pair<std::string, std::string>> got;
    for (const auto& e : *entries) {
        got.emplace_back(e.type, e.text);
    }

    // name, dialogue (bracketed), narrative (bare), title -- in line order.
    ASSERT_EQ(got.size(), kExpectedSampleCaptureCount) << "unexpected capture count";
    EXPECT_EQ(got[0].first, "name");
    EXPECT_EQ(got[0].second, "直哉");
    EXPECT_EQ(got[1].first, "dialogue");
    EXPECT_EQ(got[2].first, "narrative");
    EXPECT_EQ(got[3].first, "title");
    EXPECT_EQ(got[3].second, "第一章");
}

// {"bg", file="背景.png"} carries JP but it's an asset path, not story text.
TEST(Extract, skips_engine_commands) {
    const auto entries = snt::extract::extract_one(as_bytes(kSampleAst));
    ASSERT_TRUE(entries.has_value());
    for (const auto& e : *entries) {
        EXPECT_EQ(e.text.find("背景"), std::string::npos)
            << "asset path leaked in as " << e.type;
    }
}

// A bare JP string outside any ja={} block is engine data, not dialogue.
TEST(Extract, bare_strings_outside_ja_block_are_ignored) {
    const auto entries = snt::extract::extract_one(as_bytes(
        "astver = 2.0\n"
        "ast = {\n"
        "\tmisc = {\n"
        "\t\t\"外側\",\n"
        "\t},\n"
        "}\n"));
    ASSERT_TRUE(entries.has_value());
    EXPECT_TRUE(entries->empty());
}

TEST(Extract, line_numbers_are_one_based) {
    const auto entries = snt::extract::extract_one(as_bytes(kSampleAst));
    ASSERT_TRUE(entries.has_value());
    ASSERT_FALSE(entries->empty());
    EXPECT_EQ((*entries)[0].offset, kExpectedNamePlateLineNumber);
}

TEST(Extract, ascii_only_strings_are_dropped) {
    const auto entries = snt::extract::extract_one(as_bytes(
        "astver = 2.0\n"
        "ja = {\n"
        "\t\"plain ascii\",\n"
        "}\n"));
    ASSERT_TRUE(entries.has_value());
    EXPECT_TRUE(entries->empty());
}

// `name = {"藍", "？？？"}`: the script knows Ai, the box shows nothing.  The
// plate carries the identity and the box text rides along as `display`.
TEST(Extract, two_element_name_plate_takes_the_first_element) {
    const auto entries = snt::extract::extract_one(as_bytes(ast_with(
        "name = {\"藍\", \"？？？\"},\n"
        "\"「おはよう」\",\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), kTwoEntries);
    EXPECT_EQ((*entries)[0].type, "name");
    EXPECT_EQ((*entries)[0].text, "藍");
    EXPECT_EQ((*entries)[0].display, "？？？");
    EXPECT_EQ((*entries)[1].type, "dialogue");
    ASSERT_TRUE((*entries)[1].speaker.has_value());
    EXPECT_EQ(*(*entries)[1].speaker, "藍");
}

// The brace list is split rather than matched element by element, so a plate
// with more than two elements is read the same way: the first element is the
// speaker and the second is the box text.
TEST(Extract, a_three_element_name_plate_is_read_like_any_other) {
    const auto entries = snt::extract::extract_one(as_bytes(ast_with(
        "name = {\"藍\", \"？？？\", \"教師\"},\n"
        "\"「おはよう」\",\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), kTwoEntries);
    EXPECT_EQ((*entries)[0].type, "name");
    EXPECT_EQ((*entries)[0].text, "藍");
    EXPECT_EQ((*entries)[0].display, "？？？");
    ASSERT_TRUE((*entries)[1].speaker.has_value());
    EXPECT_EQ(*(*entries)[1].speaker, "藍");
}

// A plate whose two elements agree carries no separate display.
TEST(Extract, name_plate_display_is_omitted_when_it_repeats_the_speaker) {
    const auto entries = snt::extract::extract_one(as_bytes(
        ast_with("name = {\"藍\", \"藍\"},\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_TRUE((*entries)[0].display.empty());
}

// `？？？` holds no Japanese character but is still a plate: dropping it would
// leave the previous speaker standing over lines nobody has been named for.
TEST(Extract, name_plate_without_japanese_is_kept_and_ends_the_previous_speaker) {
    const auto entries = snt::extract::extract_one(as_bytes(ast_with(
        "name = {\"藍\"},\n"
        "\"「あ」\",\n"
        "name = {\"？？？\"},\n"
        "\"「い」\",\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), kFourEntries);
    EXPECT_EQ((*entries)[2].type, "name");
    EXPECT_EQ((*entries)[2].text, "？？？");
    ASSERT_TRUE((*entries)[3].speaker.has_value());
    EXPECT_EQ(*(*entries)[3].speaker, "？？？");
}

TEST(Extract, a_title_ends_the_scene_the_speaker_belongs_to) {
    const auto entries = snt::extract::extract_one(as_bytes(
        "astver = 2.0\nast = {\n\tblock = {\n\t\ttext = {\n\t\t\tja = {\n\t\t\t\t{\n"
        "name = {\"藍\"},\n"
        "\t\t\t\t},\n\t\t\t},\n\t\t},\n"
        "\t\t{\"savetitle\", text=\"第一章\"},\n"
        "\t\ttext = {\n\t\t\tja = {\n\t\t\t\t{\n"
        "\"「あ」\",\n"
        "\t\t\t\t},\n\t\t\t},\n\t\t},\n\t}\n}\n"));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), kThreeEntries);
    EXPECT_EQ((*entries)[1].type, "title");
    EXPECT_EQ((*entries)[2].type, "dialogue");
    EXPECT_FALSE((*entries)[2].speaker.has_value()) << "a title resets the speaker";
}

// A ruby base is the middle of the sentence before it, so it keeps that
// sentence's voice instead of reading as narration between two spoken halves.
TEST(Extract, ruby_base_fragment_keeps_the_voice_of_its_sentence) {
    const auto entries = snt::extract::extract_one(as_bytes(ast_with(
        "name = {\"静流\"},\n"
        "\"「そうで\",\n"
        "{\"ruby\", text=\"つぶ\"},\n"
        "\"円木\",\n"
        "{\"/ruby\"},\n"
        "\"すね」\",\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), kFourEntries);
    EXPECT_EQ((*entries)[2].type, "narrative");
    EXPECT_EQ((*entries)[2].text, "円木");
    ASSERT_TRUE((*entries)[2].speaker.has_value()) << "the fragment lost its voice";
    EXPECT_EQ(*(*entries)[2].speaker, "静流");
    // The reading itself is an engine command and never becomes an entry.
    for (const auto& e : *entries) {
        EXPECT_NE(e.text, "つぶ");
    }
}

// Narration around a fragment stays narration: the rule can only repeat a
// speaker an adjacent entry already carries, never invent one.
TEST(Extract, ruby_base_fragment_in_narration_stays_narration) {
    const auto entries = snt::extract::extract_one(as_bytes(ast_with(
        "\"弓張の\",\n"
        "{\"ruby\", text=\"つぶ\"},\n"
        "\"円木\",\n"
        "{\"/ruby\"},\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), kTwoEntries);
    EXPECT_FALSE((*entries)[1].speaker.has_value());
}

// Two archives holding the same script name: the extractor keeps one copy and
// says on the log which copy lost.
TEST(Extract, the_later_archive_supplies_a_script_both_archives_hold) {
    using snt::test_fixture::make_pf8;
    ScopedTempDir temp("snt_ut_extract_game");
    const fs::path& game = temp.path();

    const std::string base_line = ast_with("\"「原文」\",\n");
    const std::string patch_line = ast_with("\"「修正」\",\n");
    snt::write_file((game / fs::u8path("sakuranotoki.pfs")).u8string(),
                    make_pf8({{"script\\dup.ast", as_bytes(base_line)}}));
    snt::write_file((game / fs::u8path("sakuranotoki.pfs.010")).u8string(),
                    make_pf8({{"script\\dup.ast", as_bytes(patch_line)}}));

    const std::string out = (game / fs::u8path("extracted.json")).u8string();
    ASSERT_EQ(snt::extract::run_extract(game.u8string(), out), 0);

    const bj::value root = snt::json_parse_file(out);
    ASSERT_TRUE(root.is_object());
    const auto& script = root.get_object().at("script\\dup.ast").get_object();
    EXPECT_EQ(script.at("arc").as_string(), "sakuranotoki.pfs.010");
    const auto& strings = script.at("strings").get_array();
    ASSERT_EQ(strings.size(), 1U);
    EXPECT_EQ(strings[0].get_object().at("text").as_string(), "「修正」");
}
