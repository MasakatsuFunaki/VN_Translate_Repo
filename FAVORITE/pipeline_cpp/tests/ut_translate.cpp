// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-2 tests: cache and glossary behaviour, the prompt-shape and
// response-parsing invariants the batch driver relies on, and the cast-roster
// gate the step opens with.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "apps/pipeline_steps.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

namespace fs = std::filesystem;
using namespace fav;
namespace tr = fav::translate;

TEST(Translate, needs_translation_detects_kana_and_kanji) {
    EXPECT_TRUE(tr::needs_translation("\xE3\x81\x82"));          // あ hiragana
    EXPECT_TRUE(tr::needs_translation("\xE3\x82\xA2"));          // ア katakana
    EXPECT_TRUE(tr::needs_translation("\xE6\x97\xA5"));          // 日 kanji
    EXPECT_FALSE(tr::needs_translation("plain ascii"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("!?..."));
}

TEST(Translate, strip_speaker_tags_removes_echoed_tags) {
    EXPECT_EQ(tr::strip_speaker_tags("[NARRATION] The night was long."),
              "The night was long.");
    EXPECT_EQ(tr::strip_speaker_tags("[Kuro] Good evening."), "Good evening.");
    // No tag -> unchanged (modulo strip).
    EXPECT_EQ(tr::strip_speaker_tags("  Good evening.  "), "Good evening.");
}

// strip() must also remove U+3000 IDEOGRAPHIC SPACE, which is what FVP pads
// name-window strings with.  An ASCII-only trim would key the TSV on a string
// the engine never emits.
TEST(Translate, strip_removes_unicode_whitespace) {
    EXPECT_EQ(tr::strip("\xE3\x80\x80Kuro\xE3\x80\x80"), "Kuro");
    EXPECT_EQ(tr::strip("  spaced \t\n"), "spaced");
    EXPECT_EQ(tr::strip("\xE3\x80\x80"), "");
    // U+200B ZERO WIDTH SPACE is a format character, not whitespace -- it must
    // survive, or the stripped key stops matching what the engine emits.
    EXPECT_EQ(tr::strip("\xE2\x80\x8B"), "\xE2\x80\x8B");
}

TEST(Translate, user_prompt_shape) {
    std::string p = tr::build_user_prompt({"\xE3\x81\x82", "\xE3\x81\x84"},
                                          {"Previous line."});
    EXPECT_NE(p.find("<previous_context>\nPrevious line.\n</previous_context>"),
              std::string::npos);
    EXPECT_NE(p.find("1. \xE3\x81\x82\n2. \xE3\x81\x84\n"), std::string::npos);
    EXPECT_NE(p.find("Output EXACTLY 2 numbered translations"), std::string::npos);
    // No [Speaker] tags: FVP has no per-line speaker info.
    EXPECT_EQ(p.find("1. ["), std::string::npos);
}

TEST(Translate, user_prompt_omits_empty_context_block) {
    std::string p = tr::build_user_prompt({"\xE3\x81\x82"}, {});
    // The closing tag only ever appears with the block; the opening tag is
    // also named in the trailing instruction, so it isn't a usable marker.
    EXPECT_EQ(p.find("</previous_context>"), std::string::npos);
    EXPECT_EQ(p.rfind("<lines_to_translate>", 0), 0u);
}

TEST(Translate, context_window_is_capped) {
    std::vector<std::string> ctx;
    for (int i = 0; i < tr::CONTEXT_WINDOW + 25; ++i)
        ctx.push_back("line" + std::to_string(i));
    std::string p = tr::build_user_prompt({"x"}, ctx);
    EXPECT_EQ(p.find("line0\n"), std::string::npos) << "oldest line leaked in";
    EXPECT_NE(p.find("line25\n"), std::string::npos) << "window start missing";
    const std::string newest = "line" + std::to_string(tr::CONTEXT_WINDOW + 24) + "\n";
    EXPECT_NE(p.find(newest), std::string::npos) << "newest line missing";
}

TEST(Translate, parses_numbered_response) {
    auto t = tr::parse_numbered_response(
        "1. First line.\n"
        "2) Second line.\n"
        "3: Third line.\n");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[1], "First line.");
    EXPECT_EQ(t[2], "Second line.");
    EXPECT_EQ(t[3], "Third line.");
}

// A wrapped answer must be re-joined with a single space, not dropped.
TEST(Translate, parses_multiline_continuation) {
    auto t = tr::parse_numbered_response("1. First part\ncontinued here\n2. Second\n");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1], "First part continued here");
    EXPECT_EQ(t[2], "Second");
}

TEST(Translate, parses_and_strips_leaked_speaker_tag) {
    auto t = tr::parse_numbered_response("1. [Kuro] Good evening.\n");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[1], "Good evening.");
}

TEST(Translate, cache_preserves_insertion_order) {
    tr::Cache c;
    c.set("b", "B");
    c.set("a", "A");
    c.set("b", "B2");  // update in place, order unchanged
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c.items()[0].first, "b");
    EXPECT_EQ(c.items()[0].second, "B2");
    EXPECT_EQ(c.items()[1].first, "a");
    c.set_default("a", "IGNORED");
    EXPECT_EQ(*c.get("a"), "A");
}

// The glossary seeds the cache, so every entry must be usable as-is.
TEST(Translate, glossary_entries_are_well_formed) {
    const auto& ordered = tr::name_translations_ordered();
    EXPECT_FALSE(ordered.empty());
    for (const auto& [jp, en] : ordered) {
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(tr::strip(en).empty()) << "empty EN for " << jp;
    }
    EXPECT_EQ(ordered.size(), tr::name_translations().size())
        << "duplicate JP key in the glossary";
}

// A mistyped cap must be refused before anything is loaded or deleted: a
// negative one used to read as "no limit" and start an unbounded paid run.
TEST(Translate, validate_options_refuses_caps_that_would_overspend) {
    EXPECT_FALSE(tr::validate_options(150, 0, 0).has_value());
    EXPECT_FALSE(tr::validate_options(1, 4, 0).has_value());
    EXPECT_FALSE(tr::validate_options(1, 0, 4).has_value());
    EXPECT_TRUE(tr::validate_options(0, 0, 0).has_value());
    EXPECT_TRUE(tr::validate_options(-1, 0, 0).has_value());
    EXPECT_TRUE(tr::validate_options(150, -1, 0).has_value());
    EXPECT_TRUE(tr::validate_options(150, 0, -1).has_value());
}

namespace {

// The gate reads the game's own bytecode, so its tests skip on a machine
// where the game is not installed.
std::string installed_hcb() {
    for (const char* p : {"C:\\Favorite\\Sakura.hcb"})
        if (fs::exists(fs::u8path(p))) return p;
    return {};
}

}  // namespace

// The walker is what makes check 1 mean anything: if it stops finding the
// name-window registrations the check passes over an empty set and gates
// nothing.
TEST(Translate, cast_registry_still_finds_names_in_the_bytecode) {
    const std::string hcb = installed_hcb();
    if (hcb.empty()) GTEST_SKIP() << "Sakura.hcb not installed";
    auto cast = tr::cast_registry(read_file(hcb));
    EXPECT_GT(cast.size(), 5u) << "the name-window walk found almost nothing";
    EXPECT_NE(cast.find("\xE3\x82\xAF\xE3\x83\xAD"), cast.end()) << "クロ missing";
}

// The gate the translation step opens with, over the real bytecode.
TEST(Translate, speaker_gate_passes_over_the_installed_game) {
    const std::string hcb = installed_hcb();
    if (hcb.empty()) GTEST_SKIP() << "Sakura.hcb not installed";
    tr::SpeakerCheckReport r = tr::run_speaker_checks(read_file(hcb));
    EXPECT_TRUE(r.passed) << "check " << r.failed_check << ": " << r.reason;
    EXPECT_EQ(r.failed_check, 0);
    EXPECT_GT(r.glossary_entries, 0u);
}

// The gate is the first thing the translation step does, before a key is read
// and before a file is removed.  A refused gate therefore has to leave the
// cache and the document exactly as it found them, even on a fresh run whose
// whole point is to delete them.
TEST(Translate, a_refused_gate_stops_the_step_before_it_spends_or_deletes) {
    const fs::path dir = fs::temp_directory_path() / "fav_ut_gate";
    fs::create_directories(dir);
    const std::string cache = (dir / "cache.json").u8string();
    const std::string out = (dir / "translated.json").u8string();
    write_file(cache, std::string("{}"));
    write_file(out, std::string("{}"));

    tr::TranslateOptions opt;
    opt.hcb_file = (dir / "there-is-no-Sakura.hcb").u8string();
    opt.input_file = (dir / "extracted.json").u8string();
    opt.cache_file = cache;
    opt.output_file = out;
    opt.tsv_file = (dir / "table.tsv").u8string();
    opt.last_translate_dir = (dir / "last").u8string();
    opt.fresh_run = true;

    EXPECT_EQ(tr::translate_all(opt), 2) << "a refused gate has to fail the step";
    EXPECT_TRUE(fs::exists(fs::u8path(cache))) << "the cache was wiped before the gate ran";
    EXPECT_TRUE(fs::exists(fs::u8path(out))) << "the document was wiped before the gate ran";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));

    fs::remove_all(dir);
}

// Four steps, and the two that were separate commands are inside the
// translation step now, because a translation the game never sees is not a
// translation.
TEST(Translate, the_pipeline_is_three_steps) {
    const auto& steps = apps::pipeline_steps();
    ASSERT_EQ(steps.size(), 4u);
    EXPECT_STREQ(steps[0].name, "01_extract");
    EXPECT_STREQ(steps[1].name, "02_translate");
    EXPECT_STREQ(steps[2].name, "03_patch_ui");
    EXPECT_STREQ(steps[3].name, "deploy");
    for (const auto& step : steps) {
        EXPECT_EQ(std::string(step.name).find("speaker_gate"), std::string::npos);
        EXPECT_EQ(std::string(step.name).find("build_translations"), std::string::npos);
    }
}

TEST(Translate, system_prompt_has_dossier_with_core_cast) {
    const std::string sp = tr::SYSTEM_PROMPT;
    ASSERT_NE(sp.find("<character_dossier>"), std::string::npos);
    ASSERT_NE(sp.find("</character_dossier>"), std::string::npos);
    for (const char* jp : {"\xE5\xA4\xA7\xE9\x9B\x85",              // 大雅
                           "\xE3\x82\xAF\xE3\x83\xAD",              // クロ
                           "\xE3\x83\x8F\xE3\x83\xAB"})             // ハル
        EXPECT_NE(sp.find(jp), std::string::npos) << "missing from prompt: " << jp;
}

// The guard refuses flags that would throw away a paid cache until
// --discard-cache confirms the intent.
TEST(Translate, a_flag_that_deletes_a_paid_cache_is_refused_until_it_is_meant) {
    // An empty or nearly empty cache is what a from-scratch run was written for.
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                      false).has_value());

    // One entry past the threshold, every flag is refused.
    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag,
                                        false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    // The count is in the message, because "some lines" is not a reason to
    // stop and "56103 lines" is.
    auto why = tr::refuse_cache_discard(56103, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    // --discard-cache is the caller saying they meant it.
    EXPECT_FALSE(tr::refuse_cache_discard(56103, "--test", true).has_value());
}

// An echo -- the model handing back the line it was given -- is a failure only
// when the line is Japanese.  An English string that survives translation
// unchanged is a correct answer, so pure ASCII is never a failure.
TEST(Translate, failed_entry_predicate_only_fires_on_japanese_identity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));  // non-ASCII, still an echo
    // A resource name carrying one stray Japanese byte is an echo like any
    // other: the run re-requests it and pays for it again.
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));          // ASCII identity is right
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));  // ASCII identifier
    EXPECT_FALSE(tr::is_failed_entry("", ""));
}

// The purge takes out exactly the echoes and leaves everything else where it
// was, key order included -- that order is the cache file's key order.
TEST(Translate, purge_removes_failed_entries_and_keeps_the_rest) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 2u);
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    // Nothing left to remove on a second pass.
    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 3u);
}

// A batch that fails leaves its lines out of the cache, so the next run asks
// for exactly those lines again.  Writing the Japanese in as its own English
// would mark them done for good.
TEST(Translate, AnAnswerThatNeverArrivedIsNotCached) {
    tr::Cache cache;
    std::vector<std::string> context;

    tr::apply_line_result(cache, context, "「おはよう」", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), 0u);
    // The context window still reads in sequence.
    ASSERT_EQ(context.size(), 1u);
    EXPECT_EQ(context.front(), "「おはよう」");

    tr::apply_line_result(cache, context, "朝が来た。", "Morning came.");
    ASSERT_TRUE(cache.contains("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_EQ(context.back(), "Morning came.");
}

TEST(Translate, needs_translation_rejects_sprite_tags_and_asset_paths) {
    EXPECT_FALSE(tr::needs_translation("_私服めがね_吹出R"));
    EXPECT_FALSE(tr::needs_translation("_制服_吹出L"));
    EXPECT_FALSE(tr::needs_translation("  _パジャマ  "));
    EXPECT_FALSE(tr::needs_translation("graph_bs/CHR_ましろ"));
    EXPECT_FALSE(tr::needs_translation("graph_bg/BG01_夜"));
}

TEST(Translate, purge_failed_entries_removes_sprite_tags_and_asset_paths) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("_私服めがね_吹出R", "_Casual glasses_Bubble R");
    cache.set("_制服_吹出L", "_Uniform_Bubble L");
    cache.set("graph_bs/CHR_ましろ", "graph_bs/CHR_Mashiro");
    cache.set("クロ", "Kuro");

    EXPECT_EQ(tr::purge_failed_entries(cache), 3u);
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_FALSE(cache.contains("_私服めがね_吹出R"));
    EXPECT_FALSE(cache.contains("_制服_吹出L"));
    EXPECT_FALSE(cache.contains("graph_bs/CHR_ましろ"));
    EXPECT_TRUE(cache.contains("朝が来た。"));
    EXPECT_TRUE(cache.contains("クロ"));
}

TEST(Translate, glossary_includes_mystery_speaker_and_ellipses) {
    const auto& dict = tr::name_translations();
    ASSERT_NE(dict.find("？？？"), dict.end());
    EXPECT_EQ(dict.at("？？？"), "???");
    ASSERT_NE(dict.find("…………"), dict.end());
    EXPECT_EQ(dict.at("…………"), "............");
    ASSERT_NE(dict.find("……"), dict.end());
    EXPECT_EQ(dict.at("……"), "......");
}

TEST(Translate, sanitize_ascii_normalizes_typographic_punctuation_and_ruby_tags) {
    EXPECT_EQ(tr::sanitize_ascii("“Hello”—‘world’…"), "\"Hello\"--'world'...");
    EXPECT_EQ(tr::sanitize_ascii("[・|俺][・|は]"), "俺は");
    EXPECT_EQ(tr::sanitize_ascii("[えが|描]く"), "描く");
    EXPECT_EQ(tr::strip_speaker_tags("[Chiwa] “Good morning…”"), "\"Good morning...\"");
}

TEST(Translate, user_prompt_includes_context_after_and_speaker_hints) {
    std::vector<std::string> after;
    for (int i = 0; i < tr::CONTEXT_AFTER + 5; ++i)
        after.push_back("after" + std::to_string(i));
    std::string p = tr::build_user_prompt({"[Chiwa] おはよう"}, {"Prev EN."}, after);
    EXPECT_NE(p.find("1. [Chiwa] おはよう\n"), std::string::npos);
    EXPECT_NE(p.find("<context_after>\nafter0\n"), std::string::npos);
    const std::string last_kept = "after" + std::to_string(tr::CONTEXT_AFTER - 1) + "\n";
    const std::string first_dropped = "after" + std::to_string(tr::CONTEXT_AFTER) + "\n";
    EXPECT_NE(p.find(last_kept), std::string::npos);
    EXPECT_EQ(p.find(first_dropped), std::string::npos);
}

TEST(Translate, map_line_speakers_resolves_bytecode_speakers) {
    const std::string hcb = installed_hcb();
    if (hcb.empty()) GTEST_SKIP() << "Sakura.hcb not installed";
    const auto speakers = tr::map_line_speakers(read_file(hcb));
    EXPECT_GT(speakers.size(), 20000u);
    // 0x0a1e99 is Kuro's opening ellipsis line; 0x0aa64e is Chiwa's onigiri line;
    // 0x0a3a56 is Taiga's spoken line.
    ASSERT_NE(speakers.find(0x0a1e99u), speakers.end());
    EXPECT_EQ(speakers.at(0x0a1e99u), "Kuro");
    ASSERT_NE(speakers.find(0x0aa64eu), speakers.end());
    EXPECT_EQ(speakers.at(0x0aa64eu), "Chiwa");
    ASSERT_NE(speakers.find(0x0a3a56u), speakers.end());
    EXPECT_EQ(speakers.at(0x0a3a56u), "Taiga");
}

TEST(Translate, real_api_three_moving_windows_with_argon_judge) {
    const std::string hcb_path = installed_hcb();
    if (hcb_path.empty()) GTEST_SKIP() << "Sakura.hcb not installed";
    const std::string extracted_path =
        std::string(FAV_PROJECT_DIR) + "/script_output/extracted_text.json";
    if (!fs::exists(fs::u8path(extracted_path)))
        GTEST_SKIP() << "extracted_text.json not present";
    if (gemini::load_api_key().empty())
        GTEST_SKIP() << "GEMINI_API_KEY not configured";

    namespace bj = boost::json;
    const Bytes hcb = read_file(hcb_path);
    const auto speakers = tr::map_line_speakers(hcb);

    bj::value root = json_parse_file(extracted_path);
    const bj::array& all_lines =
        root.get_object().at("Sakura.hcb").get_object().at("lines").get_array();

    auto format_line = [&](const bj::object& ln) -> std::string {
        std::string c(ln.at("content").get_string());
        std::uint32_t off = static_cast<std::uint32_t>(ln.at("offset").to_number<std::uint64_t>());
        auto it = speakers.find(off);
        if (it != speakers.end() && !it->second.empty() && it->second != "???")
            return "[" + it->second + "] " + c;
        return c;
    };

    // Collect 3 moving windows across early (idx >= 4900), middle (idx >= 25000),
    // and late (idx >= 45000) sections of the novel.
    struct WindowSpec {
        const char* label;
        std::size_t start_idx;
    };
    const WindowSpec specs[] = {
        {"early_novel", 4900},
        {"mid_novel", 25000},
        {"late_novel", 45000},
    };
    constexpr std::size_t WINDOW_LEN = 8;

    gemini::Client client;
    bj::array tsv_test_lines;
    bj::object cache_obj;
    std::string judge_transcript;

    for (const auto& spec : specs) {
        std::vector<const bj::object*> before_objs;
        std::vector<const bj::object*> target_objs;
        std::vector<const bj::object*> after_objs;

        for (std::size_t idx = 0; idx < all_lines.size(); ++idx) {
            const auto& ln = all_lines[idx].get_object();
            const auto* tp = ln.if_contains("type");
            if (!tp || !tp->is_string() || tp->get_string() != "STRING") continue;
            const std::string c(ln.at("content").get_string());
            if (!tr::needs_translation(c)) continue;

            if (idx < spec.start_idx) {
                before_objs.push_back(&ln);
                if (before_objs.size() > static_cast<std::size_t>(tr::CONTEXT_WINDOW))
                    before_objs.erase(before_objs.begin());
            } else if (target_objs.size() < WINDOW_LEN) {
                target_objs.push_back(&ln);
            } else if (after_objs.size() < static_cast<std::size_t>(tr::CONTEXT_AFTER)) {
                after_objs.push_back(&ln);
            } else {
                break;
            }
        }
        ASSERT_EQ(target_objs.size(), WINDOW_LEN) << spec.label;

        std::vector<std::string> ctx_before;
        ctx_before.reserve(before_objs.size());
        for (const auto* o : before_objs) ctx_before.push_back(format_line(*o));

        std::vector<std::string> prompt_lines;
        prompt_lines.reserve(target_objs.size());
        for (const auto* o : target_objs) prompt_lines.push_back(format_line(*o));

        std::vector<std::string> ctx_after;
        ctx_after.reserve(after_objs.size());
        for (const auto* o : after_objs) ctx_after.push_back(format_line(*o));

        auto translations = tr::call_gemini(client, prompt_lines, ctx_before, ctx_after);
        ASSERT_EQ(translations.size(), WINDOW_LEN)
            << "Incomplete window translation in " << spec.label;

        judge_transcript += std::string("=== Window: ") + spec.label + " ===\n";
        for (std::size_t j = 0; j < WINDOW_LEN; ++j) {
            const std::string jp(target_objs[j]->at("content").get_string());
            const std::string en = tr::strip(translations[static_cast<int>(j) + 1]);
            ASSERT_FALSE(en.empty()) << spec.label << " line " << (j + 1);
            EXPECT_NE(en, jp) << spec.label << " line " << (j + 1);
            EXPECT_NE(en.front(), '[') << "Leaked [Speaker] tag in " << en;
            EXPECT_EQ(en.find('|'), std::string::npos) << "Leaked ruby pipe in " << en;
            for (unsigned char ch : en) {
                EXPECT_LT(ch, 0x80u) << "Non-ASCII byte in translation: " << en;
            }

            cache_obj[jp] = en;
            bj::object row;
            row["content"] = jp;
            row["translated"] = en;
            tsv_test_lines.push_back(std::move(row));

            judge_transcript += "JP: " + prompt_lines[j] + "\nEN: " + en + "\n\n";
        }
    }

    // Verify end-to-end TSV build over all 3 translated windows.
    const fs::path tmp_dir = fs::temp_directory_path() / "fav_ut_moving_windows";
    fs::create_directories(tmp_dir);
    const std::string tmp_tr = (tmp_dir / "translated.json").u8string();
    const std::string tmp_ca = (tmp_dir / "cache.json").u8string();
    const std::string tmp_tsv = (tmp_dir / "table.tsv").u8string();

    bj::object script_obj;
    script_obj["lines"] = tsv_test_lines;
    bj::object root_obj;
    root_obj["Sakura.hcb"] = std::move(script_obj);
    write_file(tmp_tr, bj::serialize(bj::value(std::move(root_obj))));
    write_file(tmp_ca, bj::serialize(bj::value(cache_obj)));

    ASSERT_EQ(build_tsv::run_build(tmp_tr, tmp_ca, tmp_tsv), 0);
    const Bytes tsv_bytes = read_file(tmp_tsv);
    const std::string tsv_text(reinterpret_cast<const char*>(tsv_bytes.data()), tsv_bytes.size());
    for (const auto& kv : cache_obj) {
        const std::string flat_jp = build_tsv::flatten(std::string(kv.key()));
        EXPECT_NE(tsv_text.find(flat_jp + "\t"), std::string::npos)
            << "Missing JP key in TSV: " << flat_jp;
    }
    fs::remove_all(tmp_dir);

    // LLM-as-a-Judge verification call via gemini-4-argon across all 3 windows.
    bj::object judge_body;
    judge_body["model"] = tr::MODEL;
    judge_body["max_tokens"] = 4096;
    judge_body["system"] =
        "You are a senior Japanese-to-English Visual Novel translation auditor. "
        "Verify whether the English translations across the 3 sampled windows "
        "(early_novel, mid_novel, late_novel) accurately translate the Japanese lines, "
        "preserve speaker voice and pronouns, handle [・|...] emphasis and ruby text cleanly, "
        "and contain zero untranslated Japanese or formatting glitches. "
        "End your response with 'VERDICT: PASS' if the translations are publication-ready "
        "with zero critical errors, or 'VERDICT: FAIL' followed by the error details.";
    bj::object judge_msg;
    judge_msg["role"] = "user";
    judge_msg["content"] =
        "Audit these 3 moving windows of translated visual novel dialogue:\n\n" +
        judge_transcript;
    judge_body["messages"] = bj::array{std::move(judge_msg)};

    bj::value judge_res = client.messages(bj::value(std::move(judge_body)));
    std::string judge_text;
    if (const auto* content = judge_res.get_object().if_contains("content")) {
        for (const auto& block : content->get_array()) {
            if (const auto* txt = block.get_object().if_contains("text");
                txt && txt->is_string()) {
                judge_text += std::string(txt->get_string());
            }
        }
    }
    log_info("\n--- ARGON JUDGE AUDIT REPORT ---\n" + judge_text + "\n--------------------------------");
    EXPECT_NE(judge_text.find("VERDICT: PASS"), std::string::npos)
        << "Argon judge did not return VERDICT: PASS:\n" << judge_text;
}
