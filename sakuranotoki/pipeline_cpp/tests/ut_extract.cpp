// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-1 tests: the .ast Lua line walker's capture rules, plus the universal
// cross-game gate on the deployed extracted_text.json.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "common/util.h"
#include "extract/extractor.h"
#include "pfs_fixture.h"

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace snt;

namespace {

Bytes as_bytes(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

// One .ast body wrapping `body` in the ja block the walker looks for.
std::string ast_with(const std::string& body) {
    return "astver = 2.0\nast = {\n\tblock_00001 = {\n\t\ttext = {\n\t\t\tja = {\n"
           "\t\t\t\t{\n" +
           body + "\t\t\t\t},\n\t\t\t},\n\t\t},\n\t}\n}\n";
}

// A minimal but structurally faithful .ast body.
const char* AST = R"(astver = 2.0
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
    EXPECT_FALSE(extract::extract_one(as_bytes("not an ast file")).has_value());
    EXPECT_FALSE(extract::extract_one(Bytes{}).has_value());
}

TEST(Extract, captures_name_dialogue_narrative_and_title) {
    auto entries = extract::extract_one(as_bytes(AST));
    ASSERT_TRUE(entries.has_value());

    std::vector<std::pair<std::string, std::string>> got;
    for (const auto& e : *entries) got.emplace_back(e.type, e.text);

    // name, dialogue (bracketed), narrative (bare), title -- in line order.
    ASSERT_EQ(got.size(), 4u) << "unexpected capture count";
    EXPECT_EQ(got[0].first, "name");
    EXPECT_EQ(got[0].second, "\xE7\x9B\xB4\xE5\x93\x89");            // 直哉
    EXPECT_EQ(got[1].first, "dialogue");
    EXPECT_EQ(got[2].first, "narrative");
    EXPECT_EQ(got[3].first, "title");
    EXPECT_EQ(got[3].second, "\xE7\xAC\xAC\xE4\xB8\x80\xE7\xAB\xA0");  // 第一章
}

// {"bg", file="背景.png"} carries JP but it's an asset path, not story text.
TEST(Extract, skips_engine_commands) {
    auto entries = extract::extract_one(as_bytes(AST));
    ASSERT_TRUE(entries.has_value());
    for (const auto& e : *entries)
        EXPECT_EQ(e.text.find("\xE8\x83\x8C\xE6\x99\xAF"), std::string::npos)  // 背景
            << "asset path leaked in as " << e.type;
}

// A bare JP string outside any ja={} block is engine data, not dialogue.
TEST(Extract, bare_strings_outside_ja_block_are_ignored) {
    auto entries = extract::extract_one(as_bytes(
        "astver = 2.0\n"
        "ast = {\n"
        "\tmisc = {\n"
        "\t\t\"\xE5\xA4\x96\xE5\x81\xB4\",\n"  // 外側
        "\t},\n"
        "}\n"));
    ASSERT_TRUE(entries.has_value());
    EXPECT_TRUE(entries->empty());
}

TEST(Extract, line_numbers_are_one_based) {
    auto entries = extract::extract_one(as_bytes(AST));
    ASSERT_TRUE(entries.has_value());
    ASSERT_FALSE(entries->empty());
    // `name = {"直哉"}` is on line 9 of AST.
    EXPECT_EQ((*entries)[0].offset, 9);
}

TEST(Extract, ascii_only_strings_are_dropped) {
    auto entries = extract::extract_one(as_bytes(
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
    auto entries = extract::extract_one(as_bytes(ast_with(
        "name = {\"\xE8\x97\x8D\", \"\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F\"},\n"
        "\"\xE3\x80\x8C\xE3\x81\x8A\xE3\x81\xAF\xE3\x82\x88\xE3\x81\x86\xE3\x80\x8D\",\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_EQ((*entries)[0].type, "name");
    EXPECT_EQ((*entries)[0].text, "\xE8\x97\x8D");                              // 藍
    EXPECT_EQ((*entries)[0].display, "\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F");   // ？？？
    EXPECT_EQ((*entries)[1].type, "dialogue");
    ASSERT_TRUE((*entries)[1].speaker.has_value());
    EXPECT_EQ(*(*entries)[1].speaker, "\xE8\x97\x8D");
}

// The brace list is split rather than matched element by element, so a plate
// with more than two elements is read the same way: the first element is the
// speaker and the second is the box text.  No plate in the corpus has three,
// which is exactly why the path needs a case of its own.
TEST(Extract, a_three_element_name_plate_is_read_like_any_other) {
    auto entries = extract::extract_one(as_bytes(ast_with(
        "name = {\"\xE8\x97\x8D\", \"\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F\", "
        "\"\xE6\x95\x99\xE5\xB8\xAB\"},\n"
        "\"\xE3\x80\x8C\xE3\x81\x8A\xE3\x81\xAF\xE3\x82\x88\xE3\x81\x86\xE3\x80\x8D\",\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_EQ((*entries)[0].type, "name");
    EXPECT_EQ((*entries)[0].text, "\xE8\x97\x8D");                             // 藍
    EXPECT_EQ((*entries)[0].display, "\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F");  // ？？？
    ASSERT_TRUE((*entries)[1].speaker.has_value());
    EXPECT_EQ(*(*entries)[1].speaker, "\xE8\x97\x8D");
}

// A plate whose two elements agree carries no separate display.
TEST(Extract, name_plate_display_is_omitted_when_it_repeats_the_speaker) {
    auto entries = extract::extract_one(as_bytes(
        ast_with("name = {\"\xE8\x97\x8D\", \"\xE8\x97\x8D\"},\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 1u);
    EXPECT_TRUE((*entries)[0].display.empty());
}

// `？？？` holds no Japanese character but is still a plate: dropping it would
// leave the previous speaker standing over lines nobody has been named for.
TEST(Extract, name_plate_without_japanese_is_kept_and_ends_the_previous_speaker) {
    auto entries = extract::extract_one(as_bytes(ast_with(
        "name = {\"\xE8\x97\x8D\"},\n"
        "\"\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D\",\n"
        "name = {\"\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F\"},\n"
        "\"\xE3\x80\x8C\xE3\x81\x84\xE3\x80\x8D\",\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 4u);
    EXPECT_EQ((*entries)[2].type, "name");
    EXPECT_EQ((*entries)[2].text, "\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F");
    ASSERT_TRUE((*entries)[3].speaker.has_value());
    EXPECT_EQ(*(*entries)[3].speaker, "\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F");
}

TEST(Extract, a_title_ends_the_scene_the_speaker_belongs_to) {
    auto entries = extract::extract_one(as_bytes(
        "astver = 2.0\nast = {\n\tblock = {\n\t\ttext = {\n\t\t\tja = {\n\t\t\t\t{\n"
        "name = {\"\xE8\x97\x8D\"},\n"
        "\t\t\t\t},\n\t\t\t},\n\t\t},\n"
        "\t\t{\"savetitle\", text=\"\xE7\xAC\xAC\xE4\xB8\x80\xE7\xAB\xA0\"},\n"
        "\t\ttext = {\n\t\t\tja = {\n\t\t\t\t{\n"
        "\"\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D\",\n"
        "\t\t\t\t},\n\t\t\t},\n\t\t},\n\t}\n}\n"));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 3u);
    EXPECT_EQ((*entries)[1].type, "title");
    EXPECT_EQ((*entries)[2].type, "dialogue");
    EXPECT_FALSE((*entries)[2].speaker.has_value()) << "a title resets the speaker";
}

// A ruby base is the middle of the sentence before it, so it keeps that
// sentence's voice instead of reading as narration between two spoken halves.
TEST(Extract, ruby_base_fragment_keeps_the_voice_of_its_sentence) {
    auto entries = extract::extract_one(as_bytes(ast_with(
        "name = {\"\xE9\x9D\x99\xE6\xB5\x81\"},\n"                  // 静流
        "\"\xE3\x80\x8C\xE3\x81\x9D\xE3\x81\x86\xE3\x81\xA7\",\n"   // 「そうで
        "{\"ruby\", text=\"\xE3\x81\xA4\xE3\x81\xB6\"},\n"
        "\"\xE5\x86\x86\xE6\x9C\xA8\",\n"                           // 円木
        "{\"/ruby\"},\n"
        "\"\xE3\x81\x99\xE3\x81\xAD\xE3\x80\x8D\",\n")));           // すね」
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 4u);
    EXPECT_EQ((*entries)[2].type, "narrative");
    EXPECT_EQ((*entries)[2].text, "\xE5\x86\x86\xE6\x9C\xA8");
    ASSERT_TRUE((*entries)[2].speaker.has_value()) << "the fragment lost its voice";
    EXPECT_EQ(*(*entries)[2].speaker, "\xE9\x9D\x99\xE6\xB5\x81");
    // The reading itself is an engine command and never becomes an entry.
    for (const auto& e : *entries)
        EXPECT_NE(e.text, "\xE3\x81\xA4\xE3\x81\xB6");
}

// Narration around a fragment stays narration: the rule can only repeat a
// speaker an adjacent entry already carries, never invent one.
TEST(Extract, ruby_base_fragment_in_narration_stays_narration) {
    auto entries = extract::extract_one(as_bytes(ast_with(
        "\"\xE5\xBC\x93\xE5\xBC\xB5\xE3\x81\xAE\",\n"  // 弓張の
        "{\"ruby\", text=\"\xE3\x81\xA4\xE3\x81\xB6\"},\n"
        "\"\xE5\x86\x86\xE6\x9C\xA8\",\n"
        "{\"/ruby\"},\n")));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_FALSE((*entries)[1].speaker.has_value());
}

// Two archives holding the same script name: the extractor keeps one copy and
// says on the log which copy lost.
TEST(Extract, the_later_archive_supplies_a_script_both_archives_hold) {
    using snt::test_fixture::make_pf8;
    const fs::path game = fs::temp_directory_path() / "snt_ut_extract_game";
    fs::remove_all(game);
    fs::create_directories(game);

    const std::string base_line =
        ast_with("\"\xE3\x80\x8C\xE5\x8E\x9F\xE6\x96\x87\xE3\x80\x8D\",\n");   // 「原文」
    const std::string patch_line =
        ast_with("\"\xE3\x80\x8C\xE4\xBF\xAE\xE6\xAD\xA3\xE3\x80\x8D\",\n");   // 「修正」
    write_file((game / "sakuranotoki.pfs").u8string(),
               make_pf8({{"script\\dup.ast", as_bytes(base_line)}}));
    write_file((game / "sakuranotoki.pfs.010").u8string(),
               make_pf8({{"script\\dup.ast", as_bytes(patch_line)}}));

    const std::string out = (game / "extracted.json").u8string();
    ASSERT_EQ(extract::run_extract(game.u8string(), out), 0);

    bj::value root = json_parse_file(out);
    ASSERT_TRUE(root.is_object());
    const auto& script = root.get_object().at("script\\dup.ast").get_object();
    EXPECT_EQ(script.at("arc").as_string(), "sakuranotoki.pfs.010");
    const auto& strings = script.at("strings").get_array();
    ASSERT_EQ(strings.size(), 1u);
    EXPECT_EQ(strings[0].get_object().at("text").as_string(),
              "\xE3\x80\x8C\xE4\xBF\xAE\xE6\xAD\xA3\xE3\x80\x8D");

    fs::remove_all(game);
}

// Universal cross-game gate: whatever the capture rules are, a deployed
// extraction that produced no strings at all is a broken pipeline.
TEST(Extract, deployed_extracted_json_has_entries) {
    fs::path p = fs::u8path(std::string(SNT_PROJECT_DIR) +
                            "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) GTEST_SKIP() << "extracted_text.json not built yet";
    bj::value root = json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    bool has_entries = false;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) continue;
        if (auto* s = kv.value().get_object().if_contains("strings"))
            if (s->is_array() && !s->get_array().empty()) has_entries = true;
    }
    EXPECT_TRUE(has_entries) << "no archives with a non-empty strings container";
}
