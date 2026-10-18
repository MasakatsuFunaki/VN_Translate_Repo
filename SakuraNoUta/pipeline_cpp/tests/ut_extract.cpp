// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BGI bytecode walker on a synthetic script, plus schema gate over extracted_text.json.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

#include "common/util.h"
#include "extract/extractor.h"

#include "arc_fixture.h"

using snu::Bytes;
namespace ext = snu::extract;
namespace fs = std::filesystem;

namespace {

struct ScriptBuilder {
    std::vector<std::uint32_t> code;
    Bytes text;
    std::vector<std::size_t> addrs;

    // Append a CP932 string to the text section; returns its address.
    std::size_t add_string(const std::string& utf8) {
        Bytes cp = snu::utf8_to_cp932_replace(utf8);
        std::size_t addr = text.size();
        text.insert(text.end(), cp.begin(), cp.end());
        text.push_back(0);
        addrs.push_back(addr);
        return addr;
    }

    Bytes build() const {
        // Header: 27-byte magic + NUL (0x1C bytes), then hdr_extra dword = 4
        // so hdr_size = 0x20.
        Bytes data;
        const char magic[] = "BurikoCompiledScriptVer1.00";
        data.insert(data.end(), magic, magic + 27);
        data.push_back(0);
        for (std::uint8_t b : {4, 0, 0, 0}) data.push_back(b);
        // Code section (dwords), ending with the 1B 00 00 00 boundary marker.
        for (std::uint32_t d : code)
            for (int i = 0; i < 4; ++i)
                data.push_back(static_cast<std::uint8_t>((d >> (8 * i)) & 0xFF));
        data.insert(data.end(), text.begin(), text.end());
        return data;
    }
};

}  // namespace

TEST(Extract, non_bgi_returns_nullopt) {
    Bytes junk(64, 0x42);
    EXPECT_FALSE(ext::extract_one(junk).has_value());
}

TEST(Extract, walker_classifies_name_dialogue_narrative) {
    ScriptBuilder sb;
    // Code will be 26 dwords = 104 bytes; string refs are code_size + addr.
    const std::uint32_t code_size = 26 * 4;
    std::size_t a_name = sb.add_string("直哉");
    std::size_t a_dlg = sb.add_string("「テスト」");
    std::size_t a_nar = sb.add_string("ある夜");
    std::size_t a_ruby = sb.add_string("ルビ");
    std::size_t a_lbl = sb.add_string("label");
    auto ref = [&](std::size_t addr) { return code_size + static_cast<std::uint32_t>(addr); };

    sb.code = {
        0,                    // d0
        3, ref(a_name),       // d1 marker, d2 name ref (pos 8)
        0, 0, 0x140,          // d3 d4, d5 = TEXT_FCN at pos+0x0C -> name
        ref(a_name),          // d6: inline speaker ptr for the dialogue
        3, ref(a_dlg),        // d7 marker, d8 dialogue ref (pos 32)
        0x140,                // d9 = TEXT_FCN at pos+4 -> dialogue
        0,                    // d10: null speaker ptr for the narration
        3, ref(a_nar),        // d11 marker, d12 narrative ref (pos 48)
        0x140,                // d13
        3, ref(a_ruby),       // d14 marker, d15 ruby ref (pos 60)
        0x14B,                // d16 = RUBY_FCN -> skipped
        3, ref(a_lbl),        // d17 marker, d18 ascii ref (pos 72) -> 'other', no JP
        0,                    // d19
        3, ref(a_name),       // d20 marker, d21 duplicate name ref (pos 84)
        0, 0, 0x140,          // d22 d23, d24 -> name pattern, dup-suppressed
        0x1B,                 // d25: section boundary marker
    };
    ASSERT_EQ(sb.code.size(), 26u);

    auto result = ext::extract_one(sb.build());
    ASSERT_TRUE(result.has_value());
    const auto& entries = *result;
    ASSERT_EQ(entries.size(), 3u);

    const std::int64_t text_base = 0x20 + code_size;

    EXPECT_EQ(entries[0].type, "name");
    EXPECT_EQ(entries[0].text, "直哉");
    EXPECT_EQ(entries[0].offset, text_base + static_cast<std::int64_t>(a_name));
    EXPECT_EQ(entries[0].byte_len, 4);
    EXPECT_TRUE(entries[0].has_jp);
    EXPECT_FALSE(entries[0].speaker.has_value());

    EXPECT_EQ(entries[1].type, "dialogue");
    EXPECT_EQ(entries[1].text, "「テスト」");
    EXPECT_EQ(entries[1].offset, text_base + static_cast<std::int64_t>(a_dlg));
    EXPECT_EQ(entries[1].byte_len, 10);
    ASSERT_TRUE(entries[1].speaker.has_value());
    EXPECT_EQ(*entries[1].speaker, "直哉");

    EXPECT_EQ(entries[2].type, "narrative");
    EXPECT_EQ(entries[2].text, "ある夜");
    EXPECT_EQ(entries[2].offset, text_base + static_cast<std::int64_t>(a_nar));
    EXPECT_FALSE(entries[2].speaker.has_value());
}

// Backlog-only strings survive the walk; echoes of message-function lines stay suppressed.
TEST(Extract, walker_keeps_backlog_only_strings_and_drops_echoes) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 20 * 4;
    std::size_t a_only = sb.add_string("『春と修羅』序");
    std::size_t a_shown = sb.add_string("ある夜");
    auto ref = [&](std::size_t addr) { return code_size + static_cast<std::uint32_t>(addr); };

    sb.code = {
        0,                    // d0
        3, ref(a_only),       // d1 marker, d2 backlog-only ref (pos 8)
        0, 0, 0x143,          // d3 d4, d5 = BKLG_FCN at pos+0x0C
        0,                    // d6
        3, ref(a_shown),      // d7 marker, d8 backlog echo (pos 32), seen first
        0, 0, 0x143,          // d9 d10, d11 = BKLG_FCN at pos+0x0C
        0,                    // d12: null speaker ptr
        3, ref(a_shown),      // d13 marker, d14 display ref (pos 56)
        0x140,                // d15 = TEXT_FCN at pos+4 -> narrative
        0, 0, 0,              // d16 d17 d18
        0x1B,                 // d19: section boundary marker
    };
    ASSERT_EQ(sb.code.size(), 20u);

    auto result = ext::extract_one(sb.build());
    ASSERT_TRUE(result.has_value());
    const auto& entries = *result;
    ASSERT_EQ(entries.size(), 2u);

    const std::int64_t text_base = 0x20 + code_size;
    EXPECT_EQ(entries[0].text, "『春と修羅』序");
    EXPECT_EQ(entries[0].type, "narrative");
    EXPECT_EQ(entries[0].offset, text_base + static_cast<std::int64_t>(a_only));
    EXPECT_FALSE(entries[0].speaker.has_value());

    EXPECT_EQ(entries[1].text, "ある夜");
    EXPECT_EQ(entries[1].type, "narrative");
    EXPECT_EQ(entries[1].offset, text_base + static_cast<std::int64_t>(a_shown));
}

TEST(Extract, script_to_json_speaker_field_rules) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 26 * 4;
    std::size_t a_name = sb.add_string("直哉");
    std::size_t a_dlg = sb.add_string("「テスト」");
    std::size_t a_nar = sb.add_string("ある夜");
    sb.add_string("ルビ");
    sb.add_string("label");
    auto ref = [&](std::size_t addr) { return code_size + static_cast<std::uint32_t>(addr); };
    sb.code = {0, 3, ref(a_name), 0, 0, 0x140,
               ref(a_name), 3, ref(a_dlg), 0x140,
               0, 3, ref(a_nar), 0x140,
               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
               0x1B};
    ASSERT_EQ(sb.code.size(), 26u);
    auto entries = ext::extract_one(sb.build());
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 3u);

    auto obj = ext::script_to_json("sc01", "data01099.arc", 123, *entries);
    EXPECT_EQ(obj.at("file").get_string(), "sc01");
    EXPECT_EQ(obj.at("jp_strings").get_int64(), 3);
    const auto& strings = obj.at("strings").get_array();
    // name entries carry NO speaker key; dialogue = JP name; narrative = null.
    EXPECT_FALSE(strings[0].get_object().if_contains("speaker"));
    EXPECT_EQ(strings[1].get_object().at("speaker").get_string(), "直哉");
    EXPECT_TRUE(strings[2].get_object().at("speaker").is_null());
}

TEST(Extract, jp_and_clean_text_helpers) {
    EXPECT_EQ(snu::jp_char_count("直哉とテスト"), 6);
    EXPECT_TRUE(snu::is_jp("こんにちは"));
    EXPECT_FALSE(snu::is_jp("Hello 123"));
    EXPECT_TRUE(snu::is_clean_text("line\nwith\ttabs\r"));
    EXPECT_FALSE(snu::is_clean_text(std::string("bad\x01byte", 8)));
}

// extracted_text.json must be loadable, non-empty, with at least one archive
// holding entries -- everything downstream keys off this file.
TEST(Extract, extracted_json_has_archives_with_entries) {
    fs::path p = fs::u8path(std::string(SNU_PROJECT_DIR) +
                            "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) {
        GTEST_SKIP() << "extracted_text.json missing — run 01_extract first";
    }
    auto root = snu::json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    EXPECT_FALSE(root.get_object().empty());
    bool has_entries = false;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) continue;
        auto* s = kv.value().get_object().if_contains("strings");
        if (s && s->is_array() && !s->get_array().empty()) {
            has_entries = true;
            break;
        }
    }
    EXPECT_TRUE(has_entries) << "no archives with strings container";

    // Deeper sanity: per-script schema, jp_strings consistency, plausible
    // corpus size, first chapter present.
    EXPECT_GT(root.get_object().size(), 50u);
    std::int64_t total = 0;
    bool chapter1 = false;
    for (const auto& kv : root.get_object()) {
        const auto& sc = kv.value().get_object();
        for (const char* field : {"file", "arc", "file_size", "jp_strings"})
            EXPECT_TRUE(sc.if_contains(field))
                << std::string(kv.key()) << " missing " << field;
        const auto& strings = sc.at("strings").get_array();
        EXPECT_EQ(sc.at("jp_strings").get_int64(),
                  static_cast<std::int64_t>(strings.size()));
        total += sc.at("jp_strings").get_int64();
        if (std::string(kv.key()).rfind("01_fruhlingsbeginn", 0) == 0)
            chapter1 = true;
        for (std::size_t i = 0; i < strings.size() && i < 3; ++i) {
            const auto& e = strings[i].get_object();
            for (const char* field : {"offset", "byte_len", "text", "has_jp", "type"})
                EXPECT_TRUE(e.if_contains(field));
        }
    }
    EXPECT_GE(total, 30'000);
    EXPECT_LE(total, 200'000);
    EXPECT_TRUE(chapter1) << "01_fruhlingsbeginn chapter missing";
}

namespace {

// A one-line narrative script whose only text is `line`.
Bytes one_line_script(const std::string& line) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 8 * 4;
    std::size_t a_nar = sb.add_string(line);
    sb.code = {
        0,                                 // d0: null speaker ptr
        3, code_size + static_cast<std::uint32_t>(a_nar),  // d1 marker, d2 ref (pos 8)
        0x140,                             // d3 = TEXT_FCN at pos+4 -> narrative
        0, 0, 0,                           // d4 d5 d6
        0x1B,                              // d7: section boundary marker
    };
    return sb.build();
}

}  // namespace

// The engine takes the first arc that holds a name, so the update patch
// data01099.arc shadows the original.  The extractor once let the later arc
// win and the patch's new lines drew in Japanese.
TEST(Extract, first_arc_in_search_order_wins_a_duplicated_script) {
    using snu::test_fixture::make_arc;
    const fs::path game = fs::temp_directory_path() / "snu_ut_extract_game";
    fs::create_directories(game);
    snu::write_file((game / "data01099.arc").u8string(),
                    make_arc({{"dup", one_line_script("パッチの行")}}));
    snu::write_file((game / "data01103.arc").u8string(),
                    make_arc({{"dup", one_line_script("元の行")}}));
    const std::string out = (game / "extracted.json").u8string();

    ASSERT_EQ(ext::run_extract(game.u8string(), out), 0);

    auto root = snu::json_parse_file(out);
    ASSERT_TRUE(root.is_object());
    const auto& sc = root.get_object().at("dup").get_object();
    EXPECT_EQ(sc.at("arc").as_string(), "data01099.arc");
    const auto& strings = sc.at("strings").get_array();
    ASSERT_EQ(strings.size(), 1u);
    EXPECT_EQ(strings[0].get_object().at("text").as_string(), "パッチの行");
    fs::remove_all(game);
}
