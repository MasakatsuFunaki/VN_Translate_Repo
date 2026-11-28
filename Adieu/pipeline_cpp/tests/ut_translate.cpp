// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Contract-driven unit tests for Adieu translation core: translatable filtering,
// cast speaker resolution, chronological story ordering, global max_batches cap,
// forward uncached batch packing + within-batch deduplication, default glossary
// seeding, cache I/O, and live progress snapshots.
#include "translate/translate_core.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace adi;
using namespace adi::translate;

namespace {

constexpr const char* kHiraA = "あ";
constexpr const char* kKagiOpen = "「";
constexpr const char* kHitomiSurname = "人見";
constexpr const char* kKousukeGiven = "広介";
constexpr const char* kMutsukiPlate = "睦　月";
constexpr const char* kMahiruPlate = "まひる";

constexpr std::size_t kIdx0 = 0;
constexpr std::size_t kIdx1 = 1;
constexpr std::size_t kIdx2 = 2;
constexpr std::size_t kIdx3 = 3;
constexpr int kLine1 = 1;
constexpr int kLine2 = 2;
constexpr int kLine3 = 3;
constexpr int kLine4 = 4;
constexpr int kLine5 = 5;
constexpr int kLine6 = 6;
constexpr int kLine7 = 7;
constexpr int kLine8 = 8;
constexpr std::size_t kParsedLineCount = 8;
constexpr int kExtraContextLines = 5;
constexpr std::size_t kShortFlatCap = 100;
constexpr int kRepeatedHiraCount = 200;
constexpr std::size_t kContextLineCharCap = 120;
constexpr std::size_t kExpectedCutBytes = 360;
constexpr std::size_t kExpectedCacheEntries = 2;
constexpr int kDefaultBatchSize = vn::llm::kDefaultBatchSize;
constexpr int kMinBatchSize = 1;
constexpr int kNoLimit = 0;
constexpr int kZeroBatchSize = 0;
constexpr int kSmallCap = 3;
constexpr int kNegativeCap = -1;
constexpr std::size_t kEmptyCacheSize = 0;
constexpr std::size_t kThresholdIncrement = 1;
constexpr std::size_t kPaidCacheSize = 56103;
constexpr std::size_t kExpectedPurgedCount = 2;
constexpr std::size_t kRemainingCacheCount = 3;
constexpr std::size_t kZeroPurgedCount = 0;

constexpr int kSingleGlobalBatchCap = 1;
constexpr int kBatchSizeTwo = 2;
constexpr int kBatchSizeThree = 3;
constexpr int kPreCachedPrefixCount = 50;
constexpr int kTranslateExitOk = 0;
constexpr std::size_t kExpectedOneCall = 1;
constexpr std::size_t kExpectedZeroCalls = 0;
constexpr std::size_t kExpectedSnapshotItemsTwo = 2;
constexpr std::size_t kExpectedSnapshotItemsThree = 3;
constexpr std::int64_t kFakeInputTokens = 100;
constexpr std::int64_t kFakeOutputTokens = 350;
constexpr std::int64_t kFakeThoughtTokens = 250;
constexpr std::int64_t kFakeZeroTokens = 0;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        dir_ = fs::temp_directory_path() /
               fs::u8path("adi_ut_translate_" + std::to_string(++seq));
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

static_assert(!std::is_copy_constructible_v<gemini::IMessageClient>);
static_assert(!std::is_copy_assignable_v<gemini::IMessageClient>);
static_assert(!std::is_move_constructible_v<gemini::IMessageClient>);
static_assert(!std::is_move_assignable_v<gemini::IMessageClient>);
static_assert(std::is_nothrow_move_constructible_v<gemini::Client>);
static_assert(std::is_nothrow_move_assignable_v<gemini::Client>);
static_assert(std::is_nothrow_move_constructible_v<gemini::LazyClient>);
static_assert(std::is_nothrow_move_assignable_v<gemini::LazyClient>);

// Deterministic fake IMessageClient that inspects the numbered prompt lines in
// `body["messages"][0]["content"]` and synthesizes `"1. EN_1\n2. EN_2\n..."`
// inside the `{"content": [{"type": "text", "text": ...}]}` envelope expected
// by `call_gemini`, recording every request with zero network sockets.
class FakeMessageClient final : public gemini::IMessageClient {
public:
    bj::value messages(const bj::value& body, int /*timeout_sec*/) override {
        requests.push_back(body);
        const std::string prompt(
            body.at("messages").as_array().at(0).at("content").as_string());

        const std::string open_tag = "<lines_to_translate>\n";
        const std::string close_tag = "</lines_to_translate>";
        const std::size_t start = prompt.find(open_tag);
        const std::size_t end = prompt.find(close_tag);
        int line_count = 0;
        if (start != std::string::npos && end != std::string::npos && end > start) {
            const std::string block =
                prompt.substr(start + open_tag.size(), end - (start + open_tag.size()));
            for (char c : block) {
                if (c == '\n') {
                    ++line_count;
                }
            }
        }
        line_counts_per_call.push_back(line_count);
        prompts.push_back(prompt);

        std::string reply;
        for (int i = 1; i <= line_count; ++i) {
            reply += std::to_string(i) + ". Translated_c" +
                     std::to_string(requests.size()) + "_l" + std::to_string(i) + "\n";
        }

        bj::object text_block;
        text_block["type"] = "text";
        text_block["text"] = reply;
        bj::array content_arr;
        content_arr.push_back(std::move(text_block));
        bj::object resp;
        resp["content"] = std::move(content_arr);
        resp["usage"] = bj::object{
            {"input_tokens", kFakeInputTokens},
            {"output_tokens", kFakeOutputTokens},
            {"thought_tokens", kFakeThoughtTokens},
            {"cache_read_input_tokens", kFakeZeroTokens},
            {"cache_creation_input_tokens", kFakeZeroTokens},
        };
        return resp;
    }

    std::vector<bj::value> requests;
    std::vector<std::string> prompts;
    std::vector<int> line_counts_per_call;
};

bj::object entry(const std::string& text, const std::string& type,
                 const std::string& speaker = "") {
    bj::object o;
    o["text"] = text;
    o["type"] = type;
    o["speaker"] = speaker;
    return o;
}

bj::object scene_of(std::initializer_list<bj::object> entries) {
    bj::array arr;
    for (const auto& e : entries) {
        arr.push_back(e);
    }
    bj::object s;
    s["strings"] = std::move(arr);
    return s;
}

TranslateOptions make_sandbox_options(const ScopedTempDir& scratch) {
    fs::create_directories(scratch.path() / "script_output");
    TranslateOptions opt;
    opt.input_file = (scratch.path() / "script_output" / "extracted_text.json").u8string();
    opt.cache_file =
        (scratch.path() / "script_output" / "translation_cache_gemini.json").u8string();
    opt.output_file =
        (scratch.path() / "script_output" / "translated_text.json").u8string();
    opt.tsv_file = (scratch.path() / "translation_table.tsv").u8string();
    opt.test_dir = (scratch.path() / "test").u8string();
    return opt;
}

}  // namespace

TEST(Translate, NeedsTranslationThresholds) {
    EXPECT_FALSE(needs_translation(""));
    EXPECT_FALSE(needs_translation("   "));
    EXPECT_FALSE(needs_translation("　"));
    EXPECT_FALSE(needs_translation("ab"));
    EXPECT_FALSE(needs_translation("a long ascii sentence"));
    EXPECT_FALSE(needs_translation("＊Ａ"));
    EXPECT_FALSE(needs_translation("＊Ｂ"));
    EXPECT_TRUE(needs_translation(kHitomiSurname));
    EXPECT_TRUE(needs_translation(kHiraA));
}

TEST(Translate, TranslatableTypesIncludesNameAndExcludesOther) {
    const bj::object scene = scene_of({
        entry(kMutsukiPlate, "name"),
        entry("「おはよう」", "dialogue", kMutsukiPlate),
        entry("　夢だ。", "narrative"),
        entry("教室", "menu"),
        entry("＊Ａ", "other"),
        entry("plain ascii", "narrative"),
    });
    const auto idx = translatable_indices(scene, false);
    EXPECT_EQ(idx, (std::vector<std::size_t>{kIdx0, kIdx1, kIdx2, kIdx3}));
}

TEST(Translate, SkipToDialogueTrimsAtFirstQuoteBracket) {
    const bj::object scene = scene_of({
        entry(kMutsukiPlate, "name"),
        entry("　夢だ。", "narrative"),
        entry(std::string(kKagiOpen) + kHiraA + kHiraA, "narrative"),
        entry("　朝だ。", "narrative"),
    });
    EXPECT_EQ(translatable_indices(scene, true),
              (std::vector<std::size_t>{kIdx2, kIdx3}));

    const bj::object no_quote =
        scene_of({entry(kMutsukiPlate, "name"), entry("　夢だ。", "narrative")});
    EXPECT_EQ(translatable_indices(no_quote, true),
              (std::vector<std::size_t>{kIdx0, kIdx1}));
}

TEST(Translate, MatchSpeakerNameExactAndLongestSuffixOnAdieuCast) {
    ASSERT_TRUE(match_speaker_name(kHitomiSurname).has_value());
    EXPECT_EQ(*match_speaker_name(kHitomiSurname), "Hitomi");
    ASSERT_TRUE(match_speaker_name(kMutsukiPlate).has_value());
    EXPECT_EQ(*match_speaker_name(kMutsukiPlate), "Mutsuki");
    ASSERT_TRUE(match_speaker_name(kMahiruPlate).has_value());
    EXPECT_EQ(*match_speaker_name(kMahiruPlate), "Mahiru");
    ASSERT_TRUE(match_speaker_name("謎の" + std::string(kMahiruPlate)).has_value());
    EXPECT_EQ(*match_speaker_name("謎の" + std::string(kMahiruPlate)), "Mahiru");
    EXPECT_FALSE(match_speaker_name("Bob").has_value());
}

TEST(Translate, ExtractSpeakerNameVsNarration) {
    EXPECT_EQ(extract_speaker(entry(kMutsukiPlate, "name", "")), "NAME");
    EXPECT_EQ(extract_speaker(entry("x", "dialogue", "")), "NARRATION");
    EXPECT_EQ(extract_speaker(entry("x", "dialogue", kMutsukiPlate)), "Mutsuki");
    EXPECT_EQ(extract_speaker(entry("x", "dialogue", kHitomiSurname)), "Hitomi");
    EXPECT_EQ(extract_speaker(entry("x", "dialogue", "Bob")), "Bob");
}

TEST(Translate, SanitizeAsciiAllReplacements) {
    EXPECT_EQ(sanitize_ascii("a—b"), "a--b");
    EXPECT_EQ(sanitize_ascii("a–b"), "a--b");
    EXPECT_EQ(sanitize_ascii("a…"), "a...");
    EXPECT_EQ(sanitize_ascii("‘x’"), "'x'");
    EXPECT_EQ(sanitize_ascii("“x”"), "\"x\"");
    EXPECT_EQ(sanitize_ascii("a　b"), "a b");
}

TEST(Translate, PostprocessDoesNotTouchSpeakerPlate) {
    EXPECT_EQ(postprocess("jp", ""), "");
    EXPECT_EQ(postprocess("same", "same"), "same");
    EXPECT_EQ(postprocess(std::string(kMutsukiPlate) + "\r\n" + kKagiOpen, "Hello\r\n"),
              "Hello\r\n");
    EXPECT_EQ(postprocess("jp\r\n", "Hi"), "Hi");
}

TEST(Translate, ParseNumberedResponseVariants) {
    const std::string resp =
        "1. one\n"
        "2) two\n"
        "3: three\n"
        "4 four\n"
        "\n"
        "[NARRATION] 5. five\n"
        "[NAME] 6. six\n"
        "not a numbered line\n"
        "7. a\\r\\nb\n"
        "8. c\\nd\n";
    const auto t = parse_numbered_response(resp);
    ASSERT_EQ(t.size(), kParsedLineCount);
    EXPECT_EQ(t.at(kLine1), "one");
    EXPECT_EQ(t.at(kLine2), "two");
    EXPECT_EQ(t.at(kLine3), "three");
    EXPECT_EQ(t.at(kLine4), "four");
    EXPECT_EQ(t.at(kLine5), "five");
    EXPECT_EQ(t.at(kLine6), "six");
    EXPECT_EQ(t.at(kLine7), "a\r\nb");
    EXPECT_EQ(t.at(kLine8), "c\nd");
}

TEST(Translate, BuildUserPromptShape) {
    const std::vector<std::pair<std::string, std::string>> batch = {
        {"NAME", kMutsukiPlate}, {"Hitomi", "line\r\nbreak"}};
    const std::string bare = build_user_prompt(batch, {});
    EXPECT_EQ(bare.rfind("<lines_to_translate>\n", kIdx0), kIdx0);
    EXPECT_EQ(bare.find("</previous_context>"), std::string::npos);
    EXPECT_EQ(bare.find("</context_after>"), std::string::npos);
    EXPECT_NE(bare.find("1. [NAME] " + std::string(kMutsukiPlate) + "\n"), std::string::npos);
    EXPECT_NE(bare.find("2. [Hitomi] line\\r\\nbreak\n"), std::string::npos);
    EXPECT_NE(bare.find("Translate the 2 lines inside <lines_to_translate>"),
              std::string::npos);
    EXPECT_NE(bare.find("Output EXACTLY 2 numbered translations"), std::string::npos);

    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < CONTEXT_WINDOW + kExtraContextLines; ++i) {
        ctx.emplace_back("NARRATION", "ctx" + std::to_string(i));
    }
    std::vector<std::pair<std::string, std::string>> after;
    for (int i = 0; i < CONTEXT_AFTER_WINDOW + kExtraContextLines; ++i) {
        after.emplace_back("NARRATION", "after" + std::to_string(i));
    }
    const std::string with_ctx = build_user_prompt(batch, ctx, after);
    EXPECT_NE(with_ctx.find("<previous_context>\n[NARRATION] ctx5\n"), std::string::npos);
    EXPECT_EQ(with_ctx.find("ctx4\n"), std::string::npos);
    EXPECT_NE(with_ctx.find("<context_after>\n[NARRATION] after0\n"), std::string::npos);
    EXPECT_NE(with_ctx.find("[NARRATION] after14\n"), std::string::npos);
    EXPECT_EQ(with_ctx.find("after15\n"), std::string::npos);
}

TEST(Translate, FlattenForLogTruncatesCodepointsNotUtf8Bytes) {
    EXPECT_EQ(flatten_for_log("a\r\nb\nc", kShortFlatCap), "a|b|c");
    std::string jp;
    for (int i = 0; i < kRepeatedHiraCount; ++i) {
        jp += kHiraA;
    }
    const std::string cut = flatten_for_log(jp, kContextLineCharCap);
    EXPECT_EQ(char_len(cut), kContextLineCharCap);
    EXPECT_EQ(cut.size(), kExpectedCutBytes);
}

TEST(Translate, CacheSerialisesInsertionOrderIndentTwoCrlf) {
    Cache c;
    c.set("b", "B");
    c.set("a", "A");
    c.set("b", "B2");
    ASSERT_EQ(c.size(), kExpectedCacheEntries);
    EXPECT_EQ(c.items()[kIdx0].first, "b");
    EXPECT_EQ(*c.get("b"), "B2");

    ScopedTempDir scratch;
    const std::string path = (scratch.path() / "cache.json").u8string();
    save_cache(c, path);
    const Bytes raw = read_file(path);
    const std::string s(raw.begin(), raw.end());
    EXPECT_EQ(s, "{\r\n  \"b\": \"B2\",\r\n  \"a\": \"A\"\r\n}");
    EXPECT_FALSE(fs::exists(fs::u8path(path + ".tmp")));

    const Cache back = load_cache(path);
    ASSERT_EQ(back.size(), kExpectedCacheEntries);
    EXPECT_EQ(back.items()[kIdx0].first, "b");
}

// Contract 1: `story_order()` places utility/boot/prologue/opening scenes
// (`_start`, `_menu`, `_manual`, `seen900`, `seen020`) ahead of later routes,
// and `translate_all()` processes unknown tail scenes deterministically.
TEST(Translate, StoryOrderPlacesOpeningScenesAheadOfRoutesAndSortsTailDeterministically) {
    const auto& order = story_order();
    const auto idx_of = [&](const std::string& name) -> std::ptrdiff_t {
        const auto it = std::find(order.begin(), order.end(), name);
        return it == order.end() ? -1 : std::distance(order.begin(), it);
    };

    const auto pos_start = idx_of("_start");
    const auto pos_menu = idx_of("_menu");
    const auto pos_manual = idx_of("_manual");
    const auto pos_seen900 = idx_of("seen900");
    const auto pos_seen020 = idx_of("seen020");
    const auto pos_seen031 = idx_of("seen031");
    const auto pos_seen021 = idx_of("seen021");

    ASSERT_GE(pos_start, 0);
    ASSERT_GT(pos_menu, pos_start);
    ASSERT_GT(pos_manual, pos_menu);
    ASSERT_GT(pos_seen900, pos_manual);
    ASSERT_GT(pos_seen020, pos_seen900);
    ASSERT_GT(pos_seen031, pos_seen020);
    ASSERT_GT(pos_seen021, pos_seen031);
}

// Contract 2: `translate_all()` with `max_batches = 1` across 3 uncached scenes
// executes strictly 1 `messages()` call globally (not 1 per scene!) and writes
// `script_output/last_gemini_translate/last_translation.json`.
TEST(Translate, GlobalMaxBatchesCapStopsAcrossMultipleUncachedScenesAndWritesProgressSnapshot) {
    ScopedTempDir scratch;
    TranslateOptions opt = make_sandbox_options(scratch);
    opt.batch_size = kBatchSizeTwo;
    opt.max_batches = kSingleGlobalBatchCap;

    bj::object extracted;
    // Deliberately insert scenes out of chronological order to prove `story_order()`
    // runs `_start` first and stops immediately after 1 global API request.
    extracted["seen031"] = scene_of({
        entry("　三日目の朝だ。", "narrative"),
        entry("　教室へ向かう。", "narrative"),
    });
    extracted["seen020"] = scene_of({
        entry("　夢だ。", "narrative"),
        entry("　暗い廊下を歩いている。", "narrative"),
    });
    extracted["_start"] = scene_of({
        entry("　起動テスト一行目。", "narrative"),
        entry("　起動テスト二行目。", "narrative"),
    });
    write_file_text(opt.input_file, json_pretty(bj::value(std::move(extracted))));

    FakeMessageClient fake_client;
    ASSERT_EQ(translate_all(opt, fake_client), kTranslateExitOk);

    // Strictly 1 API call across all 3 scenes (the first uncached scene `_start`).
    ASSERT_EQ(fake_client.requests.size(), kExpectedOneCall);
    ASSERT_EQ(fake_client.line_counts_per_call.size(), kExpectedOneCall);
    EXPECT_EQ(fake_client.line_counts_per_call[0], kBatchSizeTwo);

    const Cache persisted = load_cache(opt.cache_file);
    ASSERT_NE(persisted.get("　起動テスト一行目。"), nullptr);
    EXPECT_EQ(*persisted.get("　起動テスト一行目。"), "Translated_c1_l1");
    EXPECT_EQ(persisted.get("　夢だ。"), nullptr);
    EXPECT_EQ(persisted.get("　三日目の朝だ。"), nullptr);

    // Live progress snapshot `last_gemini_translate/last_translation.json` written.
    const fs::path snapshot_path =
        scratch.path() / "script_output" / "last_gemini_translate" / "last_translation.json";
    ASSERT_TRUE(fs::exists(snapshot_path));
    const bj::object snap = json_parse_file(snapshot_path.u8string()).get_object();
    EXPECT_EQ(std::string(snap.at("scene").get_string()), "_start");
    EXPECT_EQ(snap.at("api_batches_done").get_int64(), kSingleGlobalBatchCap);
    const bj::array items = snap.at("items").get_array();
    ASSERT_EQ(items.size(), kExpectedSnapshotItemsTwo);
    EXPECT_EQ(std::string(items[0].get_object().at("jp").get_string()),
              "　起動テスト一行目。");
    EXPECT_EQ(std::string(items[0].get_object().at("en").get_string()),
              "Translated_c1_l1");
}

// Contract 3: Forward uncached batch packing (Lesson 1.7) skips 50 pre-cached
// entries in a scene, packs the next `batch_size` uncached unique entries into
// a single API call while providing cached context, deduplicates identical
// Japanese lines within the same batch from one response, and persists cache.
TEST(Translate, ForwardUncachedScanSkipsCachedPrefixAndDeduplicatesWithinBatch) {
    ScopedTempDir scratch;
    TranslateOptions opt = make_sandbox_options(scratch);
    opt.batch_size = kBatchSizeThree;
    opt.max_batches = kNoLimit;

    Cache pre_cache;
    seed_default_glossary(pre_cache);
    bj::array scene_entries;
    for (int i = 0; i < kPreCachedPrefixCount; ++i) {
        const std::string jp = "　既訳の文" + std::to_string(i) + "。";
        pre_cache.set(jp, "Cached line " + std::to_string(i) + ".");
        scene_entries.push_back(entry(jp, "narrative"));
    }
    // Append 3 unique uncached lines, with the middle line repeated 3 times in-batch.
    const std::string u1 = "　未訳の一番目。";
    const std::string u2_dup = "　僕は真っ白い便器を見つめている。";
    const std::string u3 = "「おはようございます、先生」";
    scene_entries.push_back(entry(u1, "narrative"));
    scene_entries.push_back(entry(u2_dup, "narrative"));
    scene_entries.push_back(entry(u2_dup, "narrative"));
    scene_entries.push_back(entry(u2_dup, "narrative"));
    scene_entries.push_back(entry(kMutsukiPlate, "name"));
    scene_entries.push_back(entry(u3, "dialogue", kMutsukiPlate));

    save_cache(pre_cache, opt.cache_file);

    bj::object scene_obj;
    scene_obj["strings"] = std::move(scene_entries);
    bj::object extracted;
    extracted["seen020"] = std::move(scene_obj);
    write_file_text(opt.input_file, json_pretty(bj::value(std::move(extracted))));

    FakeMessageClient fake_client;
    ASSERT_EQ(translate_all(opt, fake_client), kTranslateExitOk);

    // All 3 unique uncached lines (despite 50 cached prefix lines + 3x duplicate u2 +
    // pre-seeded name plate "睦　月") must pack into STRICTLY 1 API request of 3 lines!
    ASSERT_EQ(fake_client.requests.size(), kExpectedOneCall);
    ASSERT_EQ(fake_client.line_counts_per_call.size(), kExpectedOneCall);
    EXPECT_EQ(fake_client.line_counts_per_call[0], kBatchSizeThree);
    // Rolling context from the 50 cached prefix lines must be present in the prompt.
    EXPECT_NE(fake_client.prompts[0].find("Cached line 49."), std::string::npos);

    const Cache after = load_cache(opt.cache_file);
    ASSERT_NE(after.get(u1), nullptr);
    ASSERT_NE(after.get(u2_dup), nullptr);
    ASSERT_NE(after.get(u3), nullptr);
    EXPECT_EQ(*after.get(u1), "Translated_c1_l1");
    EXPECT_EQ(*after.get(u2_dup), "Translated_c1_l2");
    EXPECT_EQ(*after.get(u3), "Translated_c1_l3");

    // Verify all 3 duplicate occurrences in `translated_text.json` received the translation.
    const bj::object out_doc = json_parse_file(opt.output_file).get_object();
    const bj::array out_strings = out_doc.at("seen020").get_object().at("strings").get_array();
    int dup_filled = 0;
    ASSERT_GT(out_strings.size(), static_cast<std::size_t>(kPreCachedPrefixCount));
    for (const auto& sv : out_strings) {
        const auto& so = sv.get_object();
        if (std::string(so.at("text").get_string()) == u2_dup) {
            EXPECT_EQ(std::string(so.at("translated").get_string()), "Translated_c1_l2");
            ++dup_filled;
        }
    }
    EXPECT_EQ(dup_filled, kBatchSizeThree);

    const fs::path snapshot_path =
        scratch.path() / "script_output" / "last_gemini_translate" / "last_translation.json";
    ASSERT_TRUE(fs::exists(snapshot_path));
    const bj::object snap = json_parse_file(snapshot_path.u8string()).get_object();
    EXPECT_EQ(snap.at("items").get_array().size(), kExpectedSnapshotItemsThree);
}

// Contract 4: Default cache seeding populates both `name_translations_ordered()`
// (`"人見" -> "Hitomi"`, `"睦　月" -> "Mutsuki"`) and `default_seed_translations()`
// (`"広介" -> "Kousuke"`), while `"＊Ａ"` / `"＊Ｂ"` (`type: "other"`) are never
// translated or written into `translation_table.tsv`.
TEST(Translate, DefaultGlossarySeedsProtagonistAndCastWhilePreservingNamaeMacros) {
    ScopedTempDir scratch;
    TranslateOptions opt = make_sandbox_options(scratch);

    bj::object extracted;
    extracted["_start"] = scene_of({
        entry("＊Ａ", "other"),
        entry(kHitomiSurname, "name"),
        entry("＊Ｂ", "other"),
        entry(kKousukeGiven, "name"),
        entry(kMutsukiPlate, "name"),
    });
    write_file_text(opt.input_file, json_pretty(bj::value(std::move(extracted))));

    FakeMessageClient fake_client;
    ASSERT_EQ(translate_all(opt, fake_client), kTranslateExitOk);
    EXPECT_EQ(fake_client.requests.size(), kExpectedZeroCalls);

    const Cache persisted = load_cache(opt.cache_file);
    ASSERT_NE(persisted.get(kHitomiSurname), nullptr);
    EXPECT_EQ(*persisted.get(kHitomiSurname), "Hitomi");
    ASSERT_NE(persisted.get(kKousukeGiven), nullptr);
    EXPECT_EQ(*persisted.get(kKousukeGiven), "Kousuke");
    ASSERT_NE(persisted.get(kMutsukiPlate), nullptr);
    EXPECT_EQ(*persisted.get(kMutsukiPlate), "Mutsuki");
    EXPECT_EQ(persisted.get("＊Ａ"), nullptr);
    EXPECT_EQ(persisted.get("＊Ｂ"), nullptr);

    const Bytes tsv_bytes = read_file(opt.tsv_file);
    const std::string tsv(tsv_bytes.begin(), tsv_bytes.end());
    EXPECT_NE(tsv.find("人見\tHitomi\n"), std::string::npos);
    EXPECT_NE(tsv.find("広介\tKousuke\n"), std::string::npos);
    EXPECT_NE(tsv.find("睦　月\tMutsuki\n"), std::string::npos);
    EXPECT_EQ(tsv.find("＊Ａ"), std::string::npos);
    EXPECT_EQ(tsv.find("＊Ｂ"), std::string::npos);
}

TEST(Translate, ValidateOptionsRejectsUnboundedAndNonsenseCaps) {
    EXPECT_FALSE(validate_options(kDefaultBatchSize, kNoLimit, kNoLimit).has_value());
    EXPECT_FALSE(validate_options(kMinBatchSize, kSmallCap, kNoLimit).has_value());
    EXPECT_FALSE(validate_options(kMinBatchSize, kNoLimit, kSmallCap).has_value());

    EXPECT_TRUE(validate_options(kZeroBatchSize, kNoLimit, kNoLimit).has_value());
    EXPECT_TRUE(validate_options(kDefaultBatchSize, kNoLimit, kNegativeCap).has_value());
    EXPECT_TRUE(validate_options(kDefaultBatchSize, kNegativeCap, kNoLimit).has_value());
}

TEST(Translate, AFlagThatDeletesAPaidCacheIsRefusedUntilItIsMeant) {
    EXPECT_FALSE(refuse_cache_discard(kEmptyCacheSize, "--test", false).has_value());
    EXPECT_FALSE(refuse_cache_discard(CACHE_DISCARD_THRESHOLD, "--test", false).has_value());

    const char* const kDestructiveFlags[] = {"--test", "--retranslate", "--clean"};
    for (const char* flag : kDestructiveFlags) {
        auto why = refuse_cache_discard(
            CACHE_DISCARD_THRESHOLD + kThresholdIncrement, flag, false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    auto why = refuse_cache_discard(kPaidCacheSize, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    EXPECT_FALSE(refuse_cache_discard(kPaidCacheSize, "--test", true).has_value());
}

TEST(Translate, FailedEntryPredicateOnlyFiresOnJapaneseIdentity) {
    EXPECT_TRUE(is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(is_failed_entry("……", "……"));
    EXPECT_TRUE(is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(is_failed_entry("OK", "OK"));
    EXPECT_FALSE(is_failed_entry("bg152n", "bg152n"));
    EXPECT_FALSE(is_failed_entry("", ""));
}

TEST(Translate, PurgeRemovesFailedEntriesAndKeepsTheRest) {
    Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(purge_failed_entries(cache), kExpectedPurgedCount);
    EXPECT_EQ(cache.size(), kRemainingCacheCount);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_NE(cache.get("朝が来た。"), nullptr);
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), kRemainingCacheCount);
    EXPECT_EQ(items[kIdx0].first, "朝が来た。");
    EXPECT_EQ(items[kIdx1].first, "OK");
    EXPECT_EQ(items[kIdx2].first, "bg152n");

    EXPECT_EQ(purge_failed_entries(cache), kZeroPurgedCount);
    EXPECT_EQ(cache.size(), kRemainingCacheCount);
}
