// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for translation core, cache semantics, glossary integrity, prompt
// construction, response parsing, and synthetic cast-roster bytecode gating.
#include "apps/pipeline_steps.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace fav;
namespace tr = fav::translate;

namespace {

constexpr std::size_t kPrefixStartOffset = 0u;
constexpr int kContextOverflowCount = 25;
constexpr int kNewestContextOffset = 24;
constexpr std::size_t kExpectedThreeLines = 3u;
constexpr std::size_t kExpectedTwoLines = 2u;
constexpr std::size_t kExpectedSingleLine = 1u;
constexpr std::size_t kExpectedEmptyCount = 0u;
constexpr int kFirstNumberedLine = 1;
constexpr int kSecondNumberedLine = 2;
constexpr int kThirdNumberedLine = 3;
constexpr int kValidBatchSize = 150;
constexpr int kMinBatchSize = 1;
constexpr int kSampleBatchCap = 4;
constexpr int kZeroLimit = 0;
constexpr int kNegativeValue = -1;
constexpr int kRefusedGateExitCode = 2;
constexpr std::size_t kPipelineStageCount = 4u;
constexpr std::size_t kThresholdExceedIncrement = 1u;
constexpr std::size_t kSamplePaidCacheLines = 56103u;
constexpr int kLookaheadOverflowCount = 5;
constexpr int kLastKeptOffset = 1;

constexpr std::uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;
constexpr std::size_t kScriptHeaderByteCount = 4u;
constexpr std::size_t kNulTerminatorByteCount = 1u;
constexpr std::uint8_t kNulByte = 0x00u;
constexpr std::uint8_t kFuncHeaderOpcode = 0x01u;
constexpr std::uint8_t kCallOpcode = 0x02u;
constexpr std::uint8_t kReturnOpcode = 0x04u;
constexpr std::uint8_t kPushStringOpcode = 0x0Eu;
constexpr std::uint8_t kFaceGraphicOpcode = 0x10u;
constexpr std::uint8_t kSampleByteOperand = 0x01u;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
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

void append_u32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kOneByteShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kTwoBytesShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kThreeBytesShift) & kByteMask));
}

void append_push_string(Bytes& out, const std::string& utf8) {
    const Bytes cp932 = utf8_to_cp932_replace(utf8);
    out.push_back(kPushStringOpcode);
    out.push_back(static_cast<std::uint8_t>(cp932.size() + kNulTerminatorByteCount));
    out.insert(out.end(), cp932.begin(), cp932.end());
    out.push_back(kNulByte);
}

Bytes build_synthetic_hcb(const Bytes& body) {
    Bytes hcb;
    append_u32(hcb, static_cast<std::uint32_t>(kScriptHeaderByteCount + body.size()));
    hcb.insert(hcb.end(), body.begin(), body.end());
    return hcb;
}

}  // namespace

TEST(Translate, needs_translation_detects_kana_and_kanji) {
    EXPECT_TRUE(tr::needs_translation("あ"));
    EXPECT_TRUE(tr::needs_translation("ア"));
    EXPECT_TRUE(tr::needs_translation("日"));
    EXPECT_FALSE(tr::needs_translation("plain ascii"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("!?..."));
}

TEST(Translate, strip_speaker_tags_removes_echoed_tags) {
    EXPECT_EQ(tr::strip_speaker_tags("[NARRATION] The night was long."),
              "The night was long.");
    EXPECT_EQ(tr::strip_speaker_tags("[Kuro] Good evening."), "Good evening.");
    EXPECT_EQ(tr::strip_speaker_tags("  Good evening.  "), "Good evening.");
}

// strip() must also remove U+3000 IDEOGRAPHIC SPACE, which is what FVP pads
// name-window strings with.  An ASCII-only trim would key the TSV on a string
// the engine never emits.
TEST(Translate, strip_removes_unicode_whitespace) {
    EXPECT_EQ(tr::strip("　Kuro　"), "Kuro");
    EXPECT_EQ(tr::strip("  spaced \t\n"), "spaced");
    EXPECT_EQ(tr::strip("　"), "");
    // U+200B ZERO WIDTH SPACE is a format character, not whitespace -- it must
    // survive, or the stripped key stops matching what the engine emits.
    EXPECT_EQ(tr::strip("\u200B"), "\u200B");
}

TEST(Translate, user_prompt_shape) {
    const std::string p = tr::build_user_prompt({"あ", "い"}, {"Previous line."});
    EXPECT_NE(p.find("<previous_context>\nPrevious line.\n</previous_context>"),
              std::string::npos);
    EXPECT_NE(p.find("1. あ\n2. い\n"), std::string::npos);
    EXPECT_NE(p.find("Output EXACTLY 2 numbered translations"), std::string::npos);
    EXPECT_EQ(p.find("1. ["), std::string::npos);
}

TEST(Translate, user_prompt_omits_empty_context_block) {
    const std::string p = tr::build_user_prompt({"あ"}, {});
    EXPECT_EQ(p.find("</previous_context>"), std::string::npos);
    EXPECT_EQ(p.rfind("<lines_to_translate>", kPrefixStartOffset), kPrefixStartOffset);
}

TEST(Translate, context_window_is_capped) {
    std::vector<std::string> ctx;
    for (int i = 0; i < tr::CONTEXT_WINDOW + kContextOverflowCount; ++i) {
        ctx.push_back("line" + std::to_string(i));
    }
    const std::string p = tr::build_user_prompt({"x"}, ctx);
    EXPECT_EQ(p.find("line0\n"), std::string::npos) << "oldest line leaked in";
    EXPECT_NE(p.find("line25\n"), std::string::npos) << "window start missing";
    const std::string newest =
        "line" + std::to_string(tr::CONTEXT_WINDOW + kNewestContextOffset) + "\n";
    EXPECT_NE(p.find(newest), std::string::npos) << "newest line missing";
}

TEST(Translate, parses_numbered_response) {
    auto t = tr::parse_numbered_response(
        "1. First line.\n"
        "2) Second line.\n"
        "3: Third line.\n");
    ASSERT_EQ(t.size(), kExpectedThreeLines);
    EXPECT_EQ(t[kFirstNumberedLine], "First line.");
    EXPECT_EQ(t[kSecondNumberedLine], "Second line.");
    EXPECT_EQ(t[kThirdNumberedLine], "Third line.");
}

// A wrapped answer must be re-joined with a single space, not dropped.
TEST(Translate, parses_multiline_continuation) {
    auto t = tr::parse_numbered_response("1. First part\ncontinued here\n2. Second\n");
    ASSERT_EQ(t.size(), kExpectedTwoLines);
    EXPECT_EQ(t[kFirstNumberedLine], "First part continued here");
    EXPECT_EQ(t[kSecondNumberedLine], "Second");
}

TEST(Translate, parses_and_strips_leaked_speaker_tag) {
    auto t = tr::parse_numbered_response("1. [Kuro] Good evening.\n");
    ASSERT_EQ(t.size(), kExpectedSingleLine);
    EXPECT_EQ(t[kFirstNumberedLine], "Good evening.");
}

TEST(Translate, cache_preserves_insertion_order) {
    tr::Cache c;
    c.set("b", "B");
    c.set("a", "A");
    c.set("b", "B2");
    ASSERT_EQ(c.size(), kExpectedTwoLines);
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
    EXPECT_FALSE(tr::validate_options(kValidBatchSize, kZeroLimit, kZeroLimit).has_value());
    EXPECT_FALSE(tr::validate_options(kMinBatchSize, kSampleBatchCap, kZeroLimit).has_value());
    EXPECT_FALSE(tr::validate_options(kMinBatchSize, kZeroLimit, kSampleBatchCap).has_value());
    EXPECT_TRUE(tr::validate_options(kZeroLimit, kZeroLimit, kZeroLimit).has_value());
    EXPECT_TRUE(tr::validate_options(kNegativeValue, kZeroLimit, kZeroLimit).has_value());
    EXPECT_TRUE(tr::validate_options(kValidBatchSize, kNegativeValue, kZeroLimit).has_value());
    EXPECT_TRUE(tr::validate_options(kValidBatchSize, kZeroLimit, kNegativeValue).has_value());
}

TEST(Translate, synthetic_cast_registry_and_speaker_gate_checks_pass) {
    Bytes body;
    body.push_back(kFaceGraphicOpcode);
    body.push_back(kSampleByteOperand);
    append_push_string(body, "　クロ　");
    body.push_back(kFaceGraphicOpcode);
    body.push_back(kSampleByteOperand);
    append_push_string(body, "　ハル　");
    body.push_back(kFaceGraphicOpcode);
    body.push_back(kSampleByteOperand);
    append_push_string(body, "_制服_吹出L");

    const Bytes hcb = build_synthetic_hcb(body);
    const auto cast = tr::cast_registry(hcb);
    ASSERT_EQ(cast.size(), kExpectedTwoLines);
    EXPECT_NE(cast.find("クロ"), cast.end());
    EXPECT_NE(cast.find("ハル"), cast.end());

    const tr::SpeakerCheckReport report = tr::run_speaker_checks(hcb);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.failed_check, kZeroLimit);
    EXPECT_GT(report.glossary_entries, kExpectedEmptyCount);
}

TEST(Translate, synthetic_map_line_speakers_resolves_subroutine_calls) {
    Bytes body;
    const std::uint32_t kuro_sub_offset =
        static_cast<std::uint32_t>(kScriptHeaderByteCount + body.size());
    body.push_back(kFuncHeaderOpcode);
    body.push_back(kSampleByteOperand);
    body.push_back(kSampleByteOperand);
    append_push_string(body, "　クロ　");
    body.push_back(kReturnOpcode);

    body.push_back(kFuncHeaderOpcode);
    body.push_back(kSampleByteOperand);
    body.push_back(kSampleByteOperand);
    body.push_back(kCallOpcode);
    append_u32(body, kuro_sub_offset);
    const std::uint32_t dialogue_offset =
        static_cast<std::uint32_t>(kScriptHeaderByteCount + body.size());
    append_push_string(body, "「さくら、もゆ。」");

    const auto speakers = tr::map_line_speakers(build_synthetic_hcb(body));
    ASSERT_NE(speakers.find(dialogue_offset), speakers.end());
    EXPECT_EQ(speakers.at(dialogue_offset), "Kuro");
}

// The gate is the first thing the translation stage does, before a key is read
// and before a file is removed.  A refused gate therefore has to leave the
// cache and the document exactly as it found them, even on a fresh run whose
// whole point is to delete them.
TEST(Translate, a_refused_gate_stops_before_it_spends_or_deletes) {
    const ScopedTempDir temp("fav_ut_gate");
    const fs::path& dir = temp.path();
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

    EXPECT_EQ(tr::translate_all(opt), kRefusedGateExitCode)
        << "a refused gate has to fail the run";
    EXPECT_TRUE(fs::exists(fs::u8path(cache))) << "the cache was wiped before the gate ran";
    EXPECT_TRUE(fs::exists(fs::u8path(out))) << "the document was wiped before the gate ran";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
}

TEST(Translate, pipeline_stages_match_expected_sequence) {
    const auto& stages = apps::pipeline_steps();
    ASSERT_EQ(stages.size(), kPipelineStageCount);
    EXPECT_STREQ(stages[0].name, "01_extract");
    EXPECT_STREQ(stages[1].name, "02_translate");
    EXPECT_STREQ(stages[2].name, "03_patch_ui");
    EXPECT_STREQ(stages[3].name, "deploy");
    for (const auto& stage : stages) {
        EXPECT_EQ(std::string(stage.name).find("speaker_gate"), std::string::npos);
        EXPECT_EQ(std::string(stage.name).find("build_translations"), std::string::npos);
    }
}

TEST(Translate, system_prompt_has_dossier_with_core_cast) {
    const std::string sp = tr::SYSTEM_PROMPT;
    ASSERT_NE(sp.find("<character_dossier>"), std::string::npos);
    ASSERT_NE(sp.find("</character_dossier>"), std::string::npos);
    for (const char* jp : {"大雅", "クロ", "ハル"}) {
        EXPECT_NE(sp.find(jp), std::string::npos) << "missing from prompt: " << jp;
    }
}

// The guard refuses flags that would throw away a paid cache until
// --discard-cache confirms the intent.
TEST(Translate, a_flag_that_deletes_a_paid_cache_is_refused_until_it_is_meant) {
    EXPECT_FALSE(tr::refuse_cache_discard(kExpectedEmptyCount, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                          false).has_value());

    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        const auto why = tr::refuse_cache_discard(
            tr::CACHE_DISCARD_THRESHOLD + kThresholdExceedIncrement, flag, false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    const auto why = tr::refuse_cache_discard(kSamplePaidCacheLines, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    EXPECT_FALSE(tr::refuse_cache_discard(kSamplePaidCacheLines, "--test", true).has_value());
}

// An echo -- the model handing back the line it was given -- is a failure only
// when the line is Japanese.  Pure ASCII identity is never a failure.
TEST(Translate, failed_entry_predicate_only_fires_on_japanese_identity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));
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

    EXPECT_EQ(tr::purge_failed_entries(cache), kExpectedTwoLines);
    EXPECT_EQ(cache.size(), kExpectedThreeLines);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), kExpectedThreeLines);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), kExpectedEmptyCount);
    EXPECT_EQ(cache.size(), kExpectedThreeLines);
}

TEST(Translate, AnAnswerThatNeverArrivedIsNotCached) {
    tr::Cache cache;
    std::vector<std::string> context;

    tr::apply_line_result(cache, context, "「おはよう」", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), kExpectedEmptyCount);
    ASSERT_EQ(context.size(), kExpectedSingleLine);
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

    EXPECT_EQ(tr::purge_failed_entries(cache), kExpectedThreeLines);
    EXPECT_EQ(cache.size(), kExpectedTwoLines);
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
    for (int i = 0; i < tr::CONTEXT_AFTER + kLookaheadOverflowCount; ++i) {
        after.push_back("after" + std::to_string(i));
    }
    const std::string p =
        tr::build_user_prompt({"[Chiwa] おはよう"}, {"Prev EN."}, after);
    EXPECT_NE(p.find("1. [Chiwa] おはよう\n"), std::string::npos);
    EXPECT_NE(p.find("<context_after>\nafter0\n"), std::string::npos);
    const std::string last_kept =
        "after" + std::to_string(tr::CONTEXT_AFTER - kLastKeptOffset) + "\n";
    const std::string first_dropped =
        "after" + std::to_string(tr::CONTEXT_AFTER) + "\n";
    EXPECT_NE(p.find(last_kept), std::string::npos);
    EXPECT_EQ(p.find(first_dropped), std::string::npos);
}
