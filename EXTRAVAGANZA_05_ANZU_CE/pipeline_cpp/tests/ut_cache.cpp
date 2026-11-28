// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure standalone unit tests for Cache persistence, the 5-stage speaker gate,
// CP932 TSV generation, Gemini Carbon request/response contracts, zero-thought
// fail-fast cache protection, and <context_after> lookahead routing.
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;

namespace {

constexpr int kWrapColumns = 54;
constexpr int kMinSyntheticDialoguePadding = 50;
constexpr int kExpectedCoverageCheckId = 5;
constexpr std::size_t kExpectedReloadedEntries = 2;

constexpr std::int64_t kClampedMinTokens = 16384;
constexpr std::int64_t kFirstUnclampedTokens = 16500;
constexpr std::int64_t kLastUnclampedTokens = 65400;
constexpr std::int64_t kClampedMaxTokens = 65536;

constexpr std::size_t kZeroLines = 0;
constexpr std::size_t kSingleLineCount = 1;
constexpr std::size_t kTwoLineCount = 2;
constexpr std::size_t kBelowGuardMinLines = 3;
constexpr std::size_t kGuardMinLines = 4;
constexpr std::size_t kAboveGuardMinLines = 5;
constexpr std::size_t kLowerClampSeamLines = 54;
constexpr std::size_t kFirstUnclampedSeamLines = 55;
constexpr std::size_t kLastUnclampedSeamLines = 218;
constexpr std::size_t kUpperClampSeamLines = 219;

constexpr std::int64_t kDefaultPromptTokens = 500;
constexpr std::int64_t kDefaultCandidatesTokens = 40;
constexpr std::int64_t kDefaultCachedTokens = 100;
constexpr std::int64_t kDefaultTotalTokens = 640;

constexpr std::int64_t kInt64PromptTokens = 1200;
constexpr std::int64_t kInt64CandidatesTokens = 85;
constexpr std::int64_t kInt64ThoughtTokens = 430;
constexpr std::int64_t kInt64CachedTokens = 300;
constexpr std::int64_t kInt64TotalTokens = 2015;

constexpr std::uint64_t kUint64PromptTokens = 2100;
constexpr std::uint64_t kUint64CandidatesTokens = 140;
constexpr std::uint64_t kUint64ThoughtTokens = 980;
constexpr std::uint64_t kUint64CachedTokens = 650;
constexpr std::uint64_t kUint64TotalTokens = 3870;

constexpr std::int64_t kBlockReasonCheckThoughts = 50;
constexpr std::int64_t kZeroThoughtTokens = 0;
constexpr std::int64_t kNegativeThoughtTokens = -1;
constexpr std::int64_t kSingleThoughtToken = 1;
constexpr std::int64_t kLookaheadBatchThoughts = 96;

constexpr int kFirstLineId = 1;
constexpr int kSecondLineId = 2;

constexpr std::size_t kExpectedSafetyCategoryCount = 5;
constexpr std::size_t kPrevContextOverflowCount = 45;
constexpr std::size_t kPrevContextDroppedCount = 5;
constexpr std::size_t kAfterContextOverflowCount = 25;
constexpr std::size_t kAfterContextKeptCount = 20;
constexpr int kLookaheadWindowBatchSize = 10;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
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

struct CapturedGeminiCall {
    std::string model;
    bj::value body;
};

struct FakeGeminiState {
    std::vector<CapturedGeminiCall> calls;
    bj::value canned_response;
};

class FakeGeminiClient final : public gemini::Client {
public:
    explicit FakeGeminiClient(std::shared_ptr<FakeGeminiState> state)
        : state_(std::move(state)) {}

    bj::value generate_content(const std::string& model,
                               const bj::value& body) override {
        state_->calls.push_back(CapturedGeminiCall{model, body});
        return state_->canned_response;
    }

private:
    std::shared_ptr<FakeGeminiState> state_;
};

bj::object make_dialogue_entry(const std::string& text) {
    bj::object o;
    o["text"] = text;
    o["type"] = "dialogue";
    o["has_jp"] = true;
    return o;
}

bj::object make_synthetic_extracted() {
    bj::array strings;
    for (const auto& [jp, en] : translate::name_translations_ordered()) {
        (void)en;
        strings.push_back(make_dialogue_entry(jp + "\r\n「いらっしゃいませ」"));
    }
    while (static_cast<int>(strings.size()) < kMinSyntheticDialoguePadding) {
        strings.push_back(make_dialogue_entry("夢美\r\n「追加の台詞です」"));
    }

    bj::object tokuten_file;
    tokuten_file["strings"] = std::move(strings);
    bj::object root;
    root["tokuten.spt"] = std::move(tokuten_file);
    return root;
}

translate::Cache make_seeded_corpus_cache() {
    translate::Cache cache;
    for (const auto& [jp, en] : translate::name_translations_ordered()) {
        cache.set(jp + "\r\n「いらっしゃいませ」", en + "\r\n\"Welcome!\"");
    }
    cache.set("夢美\r\n「追加の台詞です」", "Yumemi\r\n\"Here is an extra line.\"");
    return cache;
}

std::string make_indexed_tag(std::string_view prefix, std::size_t index) {
    constexpr std::size_t kDecimalBase = 10;
    std::string out(prefix);
    out.push_back(static_cast<char>('0' + (index / kDecimalBase)));
    out.push_back(static_cast<char>('0' + (index % kDecimalBase)));
    return out;
}

std::optional<std::string_view> extract_tag_inner(std::string_view text,
                                                  std::string_view open_tag,
                                                  std::string_view close_tag) {
    const std::size_t open_pos = text.find(open_tag);
    if (open_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t inner_begin = open_pos + open_tag.size();
    const std::size_t close_pos = text.find(close_tag, inner_begin);
    if (close_pos == std::string_view::npos) {
        return std::nullopt;
    }
    return text.substr(inner_begin, close_pos - inner_begin);
}

std::string_view as_sv(const bj::value& v) {
    const bj::string& s = v.as_string();
    return std::string_view(s.data(), s.size());
}

bj::object make_gemini_response(bj::array parts,
                                std::int64_t thought_tokens,
                                std::string_view finish_reason = "STOP") {
    bj::object content;
    content["role"] = "model";
    content["parts"] = std::move(parts);

    bj::object candidate;
    candidate["finishReason"] =
        bj::string_view(finish_reason.data(), finish_reason.size());
    candidate["content"] = std::move(content);

    bj::object usage;
    usage["promptTokenCount"] = kDefaultPromptTokens;
    usage["candidatesTokenCount"] = kDefaultCandidatesTokens;
    usage["thoughtsTokenCount"] = thought_tokens;
    usage["cachedContentTokenCount"] = kDefaultCachedTokens;
    usage["totalTokenCount"] = kDefaultTotalTokens;

    bj::object resp;
    resp["candidates"] = bj::array{std::move(candidate)};
    resp["usageMetadata"] = std::move(usage);
    return resp;
}

std::vector<std::pair<std::string, std::string>> make_batch_lines(std::size_t count) {
    std::vector<std::pair<std::string, std::string>> lines;
    lines.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        lines.emplace_back("夢美", "「テスト行_" + std::to_string(i) + "」");
    }
    return lines;
}

}  // namespace

TEST(CacheUnit, CacheRoundTripAndPostprocessSpeakerPlate) {
    const ScopedTempDir tmp("anzu_ce_ut_cache");
    const fs::path tmp_cache = tmp.path() / "anzu_ce_ut_cache.json";

    translate::Cache cache;
    cache.set("夢美\r\n「いらっしゃいませ」", "Yumemi\r\n\"Welcome!\"");
    cache.set("アゲハ\r\n「ふん」", "Ageha\r\n\"Hmph.\"");
    translate::save_cache(cache, tmp_cache.u8string());

    const translate::Cache reloaded = translate::load_cache(tmp_cache.u8string());
    ASSERT_EQ(reloaded.size(), kExpectedReloadedEntries);
    ASSERT_NE(reloaded.get("夢美\r\n「いらっしゃいませ」"), nullptr);
    EXPECT_EQ(*reloaded.get("夢美\r\n「いらっしゃいませ」"), "Yumemi\r\n\"Welcome!\"");

    const std::string post =
        translate::postprocess("夢美\r\n「テスト」\r\n", "“Test…”");
    EXPECT_EQ(post, "Yumemi\r\n\"Test...\"\r\n");
}

TEST(CacheUnit, SpeakerGatePassesOnSyntheticCorpusAndRejectsUnknownSpeaker) {
    bj::object extracted = make_synthetic_extracted();
    const auto pass_report = translate::run_speaker_checks(extracted);
    EXPECT_TRUE(pass_report.passed) << pass_report.reason;
    EXPECT_EQ(pass_report.failed_check, 0);

    extracted["tokuten.spt"].as_object()["strings"].as_array().push_back(
        make_dialogue_entry("謎の来訪者\r\n「誰かしら」"));
    const auto fail_report = translate::run_speaker_checks(extracted);
    EXPECT_FALSE(fail_report.passed);
    EXPECT_EQ(fail_report.failed_check, kExpectedCoverageCheckId);
}

TEST(CacheUnit, BuildTsvFoldsSpeakerWrapsEscapesAndWritesExactCp932) {
    // Pure transformation seams: merge_speaker_line, sanitize_newlines, reflow_lines,
    // word_wrap, and escape_for_tsv (including CP932 0x5C-trail '―' before CRLF).
    EXPECT_EQ(build_tsv::merge_speaker_line("Yumemi\r\n\"Hi\""), "Yumemi: \"Hi\"");
    EXPECT_EQ(build_tsv::merge_speaker_line("\"Already quoted\"\r\nnext"),
              "\"Already quoted\"\r\nnext");
    EXPECT_EQ(build_tsv::merge_speaker_line("123456789012345678901\r\n\"Too long\""),
              "123456789012345678901\r\n\"Too long\"");
    EXPECT_EQ(build_tsv::sanitize_newlines("\n\nfirst\nsecond\r\n"), "first\r\nsecond\r\n");
    EXPECT_EQ(build_tsv::reflow_lines("line1 \r\n  line2\r\n\r\npara2\r\n"),
              "line1 line2\r\n\r\npara2\r\n");
    EXPECT_EQ(build_tsv::word_wrap(
                  "Yumemi: \"Welcome to Cafe AnzU, please make yourself right at home today!\"",
                  kWrapColumns),
              "Yumemi: \"Welcome to Cafe AnzU, please make yourself\r\nright at home today!\"");
    EXPECT_EQ(build_tsv::escape_for_tsv("そして――――\r\n\\a\tb"),
              "そして――――\\r\\n\\\\a\\tb");

    const ScopedTempDir tmp("anzu_ce_ut_build_tsv");
    const fs::path tmp_json = tmp.path() / "anzu_ce_ut_tr.json";
    const fs::path tmp_tsv = tmp.path() / "anzu_ce_ut_tr.tsv";

    bj::object row1;
    row1["text"] = "夢美\r\n「いらっしゃいませ」";
    row1["translated"] =
        "Yumemi\r\n\"Welcome to Cafe AnzU, please make yourself right at home today!\"";
    bj::object row2;
    row2["text"] = "だが――――\r\n";
    row2["translated"] = "However--\r\n";
    bj::object fdata;
    fdata["strings"] = bj::array{std::move(row1), std::move(row2)};
    bj::object root;
    root["tokuten.spt"] = std::move(fdata);
    write_file(tmp_json.u8string(), bj::serialize(bj::value(std::move(root))));

    ASSERT_EQ(build_tsv::run_build(tmp_json.u8string(), tmp_tsv.u8string()), 0);
    const Bytes raw = read_file(tmp_tsv.u8string());
    const auto utf8 = cp932_to_utf8_strict(raw);
    ASSERT_TRUE(utf8.has_value());
    EXPECT_EQ(
        *utf8,
        "夢美\\r\\n「いらっしゃいませ」\t"
        "Yumemi: \"Welcome to Cafe AnzU, please make yourself right at\\r\\nhome today!\"\n"
        "だが――――\\r\\n\tHowever--\\r\\n\n");
}

TEST(CacheUnit, ComputeMaxOutputTokensClampsAndPreventsOverflow) {
    static_assert(sizeof(std::size_t) >= sizeof(std::uint64_t),
                  "pipeline_cpp unit tests target 64-bit size_t");

    struct TokenBoundaryCase {
        const char* name;
        std::size_t line_count;
        std::int64_t expected_tokens;
    };

    // 61489146912365273 * 300 wraps mod 2^64 to 30284 (inside [16384, 65536]),
    // catching naive unsigned multiplication prior to clamping.
    constexpr std::size_t kUint64MulWrapCount = std::size_t{61489146912365273ULL};

    constexpr TokenBoundaryCase kCases[] = {
        {"zero_lines_clamped_to_min", kZeroLines, kClampedMinTokens},
        {"single_line_clamped_to_min", kSingleLineCount, kClampedMinTokens},
        {"exact_lower_clamp_seam_54", kLowerClampSeamLines, kClampedMinTokens},
        {"first_unclamped_seam_55", kFirstUnclampedSeamLines, kFirstUnclampedTokens},
        {"last_unclamped_seam_218", kLastUnclampedSeamLines, kLastUnclampedTokens},
        {"exact_upper_clamp_seam_219", kUpperClampSeamLines, kClampedMaxTokens},
        {"int32_max_overflow_guard",
         static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()),
         kClampedMaxTokens},
        {"uint64_mul_wrap_into_midrange_guard", kUint64MulWrapCount, kClampedMaxTokens},
        {"size_max_overflow_guard",
         std::numeric_limits<std::size_t>::max(), kClampedMaxTokens},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(translate::compute_max_output_tokens(tc.line_count), tc.expected_tokens);
    }
}

TEST(CacheUnit, BuildGeminiRequestEnforcesThinkingSafetyAndContextCaps) {
    const auto one_line = make_batch_lines(1);
    const bj::object req = translate::build_gemini_request(one_line);

    const bj::array& sys_parts =
        req.at("systemInstruction").as_object().at("parts").as_array();
    ASSERT_EQ(sys_parts.size(), 1u);
    EXPECT_EQ(as_sv(sys_parts.at(0).as_object().at("text")),
              std::string_view(translate::SYSTEM_PROMPT));

    const bj::array& contents = req.at("contents").as_array();
    ASSERT_EQ(contents.size(), 1u);
    EXPECT_EQ(as_sv(contents.at(0).as_object().at("role")), "user");
    const bj::array& user_parts = contents.at(0).as_object().at("parts").as_array();
    ASSERT_EQ(user_parts.size(), 1u);
    const std::string_view default_prompt =
        as_sv(user_parts.at(0).as_object().at("text"));
    EXPECT_EQ(default_prompt, translate::build_user_prompt(one_line, {}, {}));
    EXPECT_FALSE(
        extract_tag_inner(default_prompt, "<previous_context>", "</previous_context>")
            .has_value());
    EXPECT_FALSE(
        extract_tag_inner(default_prompt, "<context_after>", "</context_after>")
            .has_value());

    const bj::object& gen_cfg = req.at("generationConfig").as_object();
    EXPECT_FALSE(gen_cfg.contains("thinkingBudget"));
    EXPECT_EQ(gen_cfg.at("maxOutputTokens").to_number<std::int64_t>(), kClampedMinTokens);
    ASSERT_TRUE(gen_cfg.contains("thinkingConfig"));
    const bj::object& thinking_cfg = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_FALSE(thinking_cfg.contains("thinkingBudget"));
    EXPECT_EQ(thinking_cfg, (bj::object{{"thinkingLevel", "MEDIUM"}}));

    const bj::array& safety = req.at("safetySettings").as_array();
    ASSERT_EQ(safety.size(), kExpectedSafetyCategoryCount);
    std::map<std::string, std::string> actual_safety;
    for (const auto& entry : safety) {
        const bj::object& obj = entry.as_object();
        actual_safety.emplace(std::string(as_sv(obj.at("category"))),
                              std::string(as_sv(obj.at("threshold"))));
    }
    const std::map<std::string, std::string> expected_safety = {
        {"HARM_CATEGORY_HARASSMENT", "BLOCK_NONE"},
        {"HARM_CATEGORY_HATE_SPEECH", "BLOCK_NONE"},
        {"HARM_CATEGORY_SEXUALLY_EXPLICIT", "BLOCK_NONE"},
        {"HARM_CATEGORY_DANGEROUS_CONTENT", "BLOCK_NONE"},
        {"HARM_CATEGORY_CIVIC_INTEGRITY", "BLOCK_NONE"},
    };
    EXPECT_EQ(actual_safety, expected_safety);

    const bj::object mid_req = translate::build_gemini_request(
        make_batch_lines(kFirstUnclampedSeamLines), {}, {}, "LOW");
    const bj::object& mid_gen = mid_req.at("generationConfig").as_object();
    EXPECT_FALSE(mid_gen.contains("thinkingBudget"));
    EXPECT_EQ(mid_gen.at("maxOutputTokens").to_number<std::int64_t>(),
              kFirstUnclampedTokens);
    EXPECT_EQ(mid_gen.at("thinkingConfig").as_object(),
              (bj::object{{"thinkingLevel", "LOW"}}));

    const bj::object clamped_req = translate::build_gemini_request(
        make_batch_lines(kUpperClampSeamLines), {}, {}, "");
    const bj::object& clamped_gen = clamped_req.at("generationConfig").as_object();
    EXPECT_FALSE(clamped_gen.contains("thinkingBudget"));
    EXPECT_FALSE(clamped_gen.contains("thinkingConfig"));
    EXPECT_EQ(clamped_gen.at("maxOutputTokens").to_number<std::int64_t>(),
              kClampedMaxTokens);

    std::vector<std::pair<std::string, std::string>> prev_ctx;
    prev_ctx.reserve(kPrevContextOverflowCount);
    for (std::size_t i = 0; i < kPrevContextOverflowCount; ++i) {
        const std::string tag = make_indexed_tag("PrevCtx_", i);
        prev_ctx.emplace_back(tag + "_JP", tag + "_EN");
    }

    std::vector<std::pair<std::string, std::string>> after_ctx;
    after_ctx.reserve(kAfterContextOverflowCount);
    for (std::size_t i = 0; i < kAfterContextOverflowCount; ++i) {
        const std::string tag = make_indexed_tag("AfterCtx_", i);
        after_ctx.emplace_back(tag + "_JP", tag + "_EN");
    }

    const std::string direct_ctx_prompt =
        translate::build_user_prompt(one_line, prev_ctx, after_ctx);
    const bj::object ctx_req =
        translate::build_gemini_request(one_line, prev_ctx, after_ctx);
    const std::string_view wire_ctx_prompt = as_sv(ctx_req.at("contents")
                                                       .as_array()
                                                       .at(0)
                                                       .as_object()
                                                       .at("parts")
                                                       .as_array()
                                                       .at(0)
                                                       .as_object()
                                                       .at("text"));
    EXPECT_EQ(wire_ctx_prompt, direct_ctx_prompt);

    const auto prev_inner =
        extract_tag_inner(wire_ctx_prompt, "<previous_context>", "</previous_context>");
    ASSERT_TRUE(prev_inner.has_value());
    for (std::size_t i = 0; i < kPrevContextDroppedCount; ++i) {
        const std::string tag = make_indexed_tag("PrevCtx_", i);
        SCOPED_TRACE(tag);
        EXPECT_EQ(prev_inner->find(tag), std::string_view::npos);
    }
    for (std::size_t i = kPrevContextDroppedCount; i < kPrevContextOverflowCount; ++i) {
        const std::string tag = make_indexed_tag("PrevCtx_", i);
        SCOPED_TRACE(tag);
        EXPECT_NE(prev_inner->find(tag), std::string_view::npos);
    }

    const auto after_inner =
        extract_tag_inner(wire_ctx_prompt, "<context_after>", "</context_after>");
    ASSERT_TRUE(after_inner.has_value());
    for (std::size_t i = 0; i < kAfterContextKeptCount; ++i) {
        const std::string tag = make_indexed_tag("AfterCtx_", i);
        SCOPED_TRACE(tag);
        EXPECT_NE(after_inner->find(tag), std::string_view::npos);
    }
    for (std::size_t i = kAfterContextKeptCount; i < kAfterContextOverflowCount; ++i) {
        const std::string tag = make_indexed_tag("AfterCtx_", i);
        SCOPED_TRACE(tag);
        EXPECT_EQ(after_inner->find(tag), std::string_view::npos);
    }
}

TEST(CacheUnit, ParseGeminiResponseFiltersThoughtsAndExtractsUsageAndBlocks) {
    bj::array mixed_parts{
        bj::object{{"thought", true}, {"text", "[1] Internal draft translation\n"}},
        bj::object{{"text", "[1] First visible line"}},
        bj::object{{"thought", true}, {"text", "Checking register and speaker voice."}},
        bj::object{{"thought", false}, {"text", "\n[2] Second visible line"}},
        bj::object{{"thoughtSignature", "opaque_sig_payload"}},
    };
    bj::object int64_resp =
        make_gemini_response(std::move(mixed_parts), kInt64ThoughtTokens);
    int64_resp["usageMetadata"] = bj::object{
        {"promptTokenCount", kInt64PromptTokens},
        {"candidatesTokenCount", kInt64CandidatesTokens},
        {"thoughtsTokenCount", kInt64ThoughtTokens},
        {"cachedContentTokenCount", kInt64CachedTokens},
        {"totalTokenCount", kInt64TotalTokens},
    };

    const translate::GeminiParsedResponse parsed =
        translate::parse_gemini_response(int64_resp);
    EXPECT_FALSE(parsed.blocked);
    EXPECT_EQ(parsed.candidate_text, "[1] First visible line\n[2] Second visible line");
    EXPECT_EQ(parsed.usage.prompt_tokens, kInt64PromptTokens);
    EXPECT_EQ(parsed.usage.candidates_tokens, kInt64CandidatesTokens);
    EXPECT_EQ(parsed.usage.thought_tokens, kInt64ThoughtTokens);
    EXPECT_EQ(parsed.usage.cached_tokens, kInt64CachedTokens);
    EXPECT_EQ(parsed.usage.total_tokens, kInt64TotalTokens);

    auto state = std::make_shared<FakeGeminiState>();
    state->canned_response = int64_resp;
    FakeGeminiClient client(state);
    const translate::GeminiBatchResult detailed =
        translate::call_gemini_detailed(client, make_batch_lines(kTwoLineCount));
    EXPECT_FALSE(detailed.blocked);
    EXPECT_EQ(detailed.usage.thought_tokens, kInt64ThoughtTokens);
    const std::map<int, std::string> expected_translations = {
        {kFirstLineId, "First visible line"},
        {kSecondLineId, "Second visible line"},
    };
    EXPECT_EQ(detailed.translations, expected_translations);

    bj::object uint64_usage;
    uint64_usage["promptTokenCount"] = bj::value(kUint64PromptTokens);
    uint64_usage["candidatesTokenCount"] = bj::value(kUint64CandidatesTokens);
    uint64_usage["thoughtsTokenCount"] = bj::value(kUint64ThoughtTokens);
    uint64_usage["cachedContentTokenCount"] = bj::value(kUint64CachedTokens);
    uint64_usage["totalTokenCount"] = bj::value(kUint64TotalTokens);
    ASSERT_TRUE(uint64_usage.at("thoughtsTokenCount").is_uint64());
    bj::object uint64_resp;
    uint64_resp["usageMetadata"] = std::move(uint64_usage);

    const translate::GeminiParsedResponse parsed_uint64 =
        translate::parse_gemini_response(uint64_resp);
    EXPECT_EQ(parsed_uint64.usage.prompt_tokens,
              static_cast<std::int64_t>(kUint64PromptTokens));
    EXPECT_EQ(parsed_uint64.usage.candidates_tokens,
              static_cast<std::int64_t>(kUint64CandidatesTokens));
    EXPECT_EQ(parsed_uint64.usage.thought_tokens,
              static_cast<std::int64_t>(kUint64ThoughtTokens));
    EXPECT_EQ(parsed_uint64.usage.cached_tokens,
              static_cast<std::int64_t>(kUint64CachedTokens));
    EXPECT_EQ(parsed_uint64.usage.total_tokens,
              static_cast<std::int64_t>(kUint64TotalTokens));

    const translate::GeminiParsedResponse parsed_missing_usage =
        translate::parse_gemini_response(bj::object{{"candidates", bj::array{}}});
    EXPECT_EQ(parsed_missing_usage.usage.prompt_tokens, 0);
    EXPECT_EQ(parsed_missing_usage.usage.candidates_tokens, 0);
    EXPECT_EQ(parsed_missing_usage.usage.thought_tokens, 0);
    EXPECT_EQ(parsed_missing_usage.usage.cached_tokens, 0);
    EXPECT_EQ(parsed_missing_usage.usage.total_tokens, 0);

    const translate::GeminiParsedResponse parsed_malformed_usage =
        translate::parse_gemini_response(bj::object{
            {"usageMetadata",
             bj::object{{"thoughtsTokenCount", "invalid"},
                        {"promptTokenCount", nullptr}}},
        });
    EXPECT_EQ(parsed_malformed_usage.usage.prompt_tokens, 0);
    EXPECT_EQ(parsed_malformed_usage.usage.thought_tokens, 0);

    struct BlockReasonCase {
        const char* block_reason;
        bool expected_blocked;
    };
    constexpr BlockReasonCase kBlockReasonCases[] = {
        {"PROHIBITED_CONTENT", true},
        {"SAFETY", true},
        {"BLOCK_REASON_UNSPECIFIED", false},
        {"", false},
    };
    for (const auto& tc : kBlockReasonCases) {
        SCOPED_TRACE(tc.block_reason);
        bj::object r = make_gemini_response(
            bj::array{bj::object{{"text", "[1] Ok"}}},
            kBlockReasonCheckThoughts, "STOP");
        r["promptFeedback"] = bj::object{{"blockReason", tc.block_reason}};
        EXPECT_EQ(translate::parse_gemini_response(r).blocked, tc.expected_blocked);
    }

    struct FinishReasonCase {
        const char* finish_reason;
        bool expected_blocked;
    };
    constexpr FinishReasonCase kFinishReasonCases[] = {
        {"SAFETY", true},
        {"PROHIBITED_CONTENT", true},
        {"BLOCKLIST", true},
        {"SPII", true},
        {"RECITATION", true},
        {"IMAGE_SAFETY", true},
        {"STOP", false},
        {"MAX_TOKENS", false},
    };
    for (const auto& tc : kFinishReasonCases) {
        SCOPED_TRACE(tc.finish_reason);
        const bj::object r = make_gemini_response(
            bj::array{bj::object{{"text", "[1] Ok"}}},
            kBlockReasonCheckThoughts, tc.finish_reason);
        EXPECT_EQ(translate::parse_gemini_response(r).blocked, tc.expected_blocked);
    }

    bj::object second_candidate_blocked;
    second_candidate_blocked["candidates"] = bj::array{
        bj::object{{"finishReason", "STOP"}},
        bj::object{{"finishReason", "PROHIBITED_CONTENT"}},
    };
    EXPECT_TRUE(translate::parse_gemini_response(second_candidate_blocked).blocked);
}

TEST(CacheUnit, ZeroThoughtGuardEnforcesGuardedCarbonMediumHighAndBansArgon) {
    struct ThrowingGuardCase {
        const char* name;
        const char* model;
        const char* thinking_level;
        std::size_t line_count;
        std::int64_t thought_tokens;
    };
    constexpr ThrowingGuardCase kThrowingCases[] = {
        {"carbon_medium_min_lines_4_zero_thoughts", "gemini-4-carbon", "MEDIUM",
         kGuardMinLines, kZeroThoughtTokens},
        {"carbon_high_min_lines_4_negative_thoughts", "gemini-4-carbon", "HIGH",
         kGuardMinLines, kNegativeThoughtTokens},
        {"mixed_case_carbon_5_lines_zero_thoughts", "Gemini-4-Carbon", "MEDIUM",
         kAboveGuardMinLines, kZeroThoughtTokens},
        {"upper_neon_lower_medium_4_lines_zero_thoughts",
         "models/GEMINI-4-NEON-preview", "medium", kGuardMinLines,
         kZeroThoughtTokens},
        {"hydrogen_high_4_lines_zero_thoughts", "gemini-4-hydrogen", "HIGH",
         kGuardMinLines, kZeroThoughtTokens},
    };
    for (const auto& tc : kThrowingCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_THROW(translate::enforce_thinking_guard(
                         tc.model, tc.thinking_level, false, tc.line_count,
                         tc.thought_tokens),
                     translate::ZeroThoughtError);

        auto state = std::make_shared<FakeGeminiState>();
        state->canned_response = make_gemini_response(
            bj::array{bj::object{{"text", "[1] Line"}}}, tc.thought_tokens, "STOP");
        FakeGeminiClient client(state);
        EXPECT_THROW(translate::call_gemini_detailed(
                         client, make_batch_lines(tc.line_count), {}, {}, tc.model,
                         tc.thinking_level),
                     translate::ZeroThoughtError);
    }

    struct AllowedGuardCase {
        const char* name;
        const char* model;
        const char* thinking_level;
        const char* finish_reason;
        bool expected_blocked;
        std::size_t line_count;
        std::int64_t thought_tokens;
        std::size_t expected_translation_count;
    };
    constexpr AllowedGuardCase kAllowedCases[] = {
        {"carbon_3_lines_below_min_seam", "gemini-4-carbon", "MEDIUM", "STOP", false,
         kBelowGuardMinLines, kZeroThoughtTokens, kSingleLineCount},
        {"carbon_4_lines_one_thought_seam", "gemini-4-carbon", "MEDIUM", "STOP", false,
         kGuardMinLines, kSingleThoughtToken, kSingleLineCount},
        {"carbon_4_lines_blocked_exempt", "gemini-4-carbon", "MEDIUM",
         "PROHIBITED_CONTENT", true, kGuardMinLines, kZeroThoughtTokens, kZeroLines},
        {"non_gemini4_flash_4_lines_zero_thoughts", "gemini-2.5-flash", "MEDIUM",
         "STOP", false, kGuardMinLines, kZeroThoughtTokens, kSingleLineCount},
        {"carbon_low_thinking_4_lines_zero_thoughts", "gemini-4-carbon", "LOW",
         "STOP", false, kGuardMinLines, kZeroThoughtTokens, kSingleLineCount},
    };
    for (const auto& tc : kAllowedCases) {
        SCOPED_TRACE(tc.name);
        translate::enforce_thinking_guard(
            tc.model, tc.thinking_level, tc.expected_blocked, tc.line_count,
            tc.thought_tokens);

        auto state = std::make_shared<FakeGeminiState>();
        bj::array parts = tc.expected_blocked
                              ? bj::array{}
                              : bj::array{bj::object{{"text", "[1] Allowed line"}}};
        state->canned_response =
            make_gemini_response(std::move(parts), tc.thought_tokens, tc.finish_reason);
        FakeGeminiClient client(state);
        const translate::GeminiBatchResult result = translate::call_gemini_detailed(
            client, make_batch_lines(tc.line_count), {}, {}, tc.model,
            tc.thinking_level);
        EXPECT_EQ(result.blocked, tc.expected_blocked);
        EXPECT_EQ(result.usage.thought_tokens, tc.thought_tokens);
        EXPECT_EQ(result.translations.size(), tc.expected_translation_count);
    }

    constexpr std::string_view kEmbeddedNulModel("gemini-4-carbon\0bad", 19);
    const std::pair<const char*, std::string_view> kDisallowedModels[] = {
        {"canonical_argon", "gemini-4-argon"},
        {"uppercase_argon", "GEMINI-4-ARGON"},
        {"prefixed_mixed_argon", "models/Gemini-4-Argon-preview"},
        {"embedded_nul_model", kEmbeddedNulModel},
    };
    auto https_client = gemini::make_https_client("offline-dummy-key");
    auto state = std::make_shared<FakeGeminiState>();
    state->canned_response = make_gemini_response(
        bj::array{bj::object{{"text", "[1] Valid after reject"}}},
        kSingleThoughtToken, "STOP");
    FakeGeminiClient client(state);

    for (const auto& [name, disallowed_model] : kDisallowedModels) {
        SCOPED_TRACE(name);
        try {
            (void)translate::call_gemini_detailed(
                client, make_batch_lines(kSingleLineCount), {}, {}, disallowed_model,
                "MEDIUM");
            FAIL() << "Expected vn::llm::LlmError for disallowed model in call_gemini_detailed";
        } catch (const translate::ZeroThoughtError&) {
            FAIL() << "Disallowed model must fail model policy, not ZeroThoughtError";
        } catch (const vn::llm::LlmError& err) {
            EXPECT_NE(std::string_view(err.what()).find("disallowed"),
                      std::string_view::npos);
        }
        EXPECT_TRUE(state->calls.empty());

        try {
            (void)https_client->generate_content(std::string(disallowed_model), bj::object{});
            FAIL() << "Expected vn::llm::LlmError for disallowed model in HttpsClient";
        } catch (const vn::llm::LlmError& err) {
            EXPECT_NE(std::string_view(err.what()).find("disallowed"),
                      std::string_view::npos);
        }
    }

    const translate::GeminiBatchResult valid_after_reject =
        translate::call_gemini_detailed(
            client, make_batch_lines(kSingleLineCount), {}, {},
            "gemini-4-carbon", "MEDIUM");
    ASSERT_EQ(state->calls.size(), kSingleLineCount);
    EXPECT_EQ(state->calls[0].model, "gemini-4-carbon");
    ASSERT_EQ(valid_after_reject.translations.size(), kSingleLineCount);
    EXPECT_EQ(valid_after_reject.translations.at(kFirstLineId), "Valid after reject");
}

TEST(CacheUnit, TranslateAllAbortsOnZeroThoughtWithoutMutatingCacheOrOutput) {
    const ScopedTempDir tmp("anzu_ce_ut_zero_thought_abort");
    const fs::path input_path = tmp.path() / "extracted_text.json";
    const fs::path cache_path = tmp.path() / "translation_cache.json";
    const fs::path output_path = tmp.path() / "translated_text.json";
    const fs::path test_dir_path = tmp.path() / "test_out";
    const fs::path tsv_path = tmp.path() / "translation_table.tsv";

    const translate::Cache seeded_cache = make_seeded_corpus_cache();
    translate::save_cache(seeded_cache, cache_path.u8string());
    const auto items_before = seeded_cache.items();
    const Bytes raw_cache_before = read_file(cache_path.u8string());

    bj::object extracted = make_synthetic_extracted();
    bj::array& strings = extracted["tokuten.spt"].as_object()["strings"].as_array();
    strings.push_back(make_dialogue_entry("ザラッとした感触。ひどいサメ肌。"));
    strings.push_back(make_dialogue_entry("冷たい波が足元をさらっていく。"));
    strings.push_back(make_dialogue_entry("遠くでカモメの鳴き声が響いた。"));
    strings.push_back(make_dialogue_entry("誰もいない浜辺に、影がひとつ伸びていた。"));
    write_file(input_path.u8string(), bj::serialize(bj::value(std::move(extracted))));

    auto state = std::make_shared<FakeGeminiState>();
    state->canned_response = make_gemini_response(
        bj::array{bj::object{
            {"text",
             "[1] Rough texture. Terrible sharkskin.\n"
             "[2] Cold waves sweep past my feet.\n"
             "[3] A seagull cried in the distance.\n"
             "[4] A lone shadow stretched across the empty beach."}}},
        0, "STOP");

    translate::TranslateOptions opt;
    opt.input_file = input_path.u8string();
    opt.cache_file = cache_path.u8string();
    opt.output_file = output_path.u8string();
    opt.test_dir = test_dir_path.u8string();
    opt.tsv_file = tsv_path.u8string();
    opt.client_factory = [state] { return std::make_unique<FakeGeminiClient>(state); };

    EXPECT_THROW(translate::translate_all(opt), translate::ZeroThoughtError);
    ASSERT_EQ(state->calls.size(), 1u);
    EXPECT_EQ(state->calls[0].model, translate::MODEL);

    EXPECT_EQ(read_file(cache_path.u8string()), raw_cache_before);
    const translate::Cache reloaded = translate::load_cache(cache_path.u8string());
    EXPECT_EQ(reloaded.items(), items_before);
    EXPECT_FALSE(reloaded.contains("ザラッとした感触。ひどいサメ肌。"));
    EXPECT_FALSE(fs::exists(output_path));
}

TEST(CacheUnit, TranslateAllPlacesFollowingCachedLinesInContextAfterNotPreviousContext) {
    const ScopedTempDir tmp("anzu_ce_ut_context_after_seam");
    const fs::path input_path = tmp.path() / "extracted_text.json";
    const fs::path cache_path = tmp.path() / "translation_cache.json";
    const fs::path output_path = tmp.path() / "translated_text.json";
    const fs::path test_dir_path = tmp.path() / "test_out";
    const fs::path tsv_path = tmp.path() / "translation_table.tsv";

    constexpr std::string_view kCachedBeforeJp = "冷たい海風が頬を撫でた。";
    constexpr std::string_view kCachedBeforeEn = "A cold sea breeze brushed my cheek.";
    constexpr std::string_view kUncachedTargetJp = "ザラッとした感触。ひどいサメ肌。";
    constexpr std::string_view kUncachedTargetEn =
        "A rough sensation. Terrible sharkskin.";
    constexpr std::string_view kCachedAfterJp =
        "巨大なサメが、そこに口を開いていた。";
    constexpr std::string_view kCachedAfterEn =
        "A giant shark had its jaws wide open there.";

    translate::Cache seeded_cache = make_seeded_corpus_cache();
    seeded_cache.set(std::string(kCachedBeforeJp), std::string(kCachedBeforeEn));
    seeded_cache.set(std::string(kCachedAfterJp), std::string(kCachedAfterEn));
    translate::save_cache(seeded_cache, cache_path.u8string());

    bj::object base_corpus = make_synthetic_extracted();
    bj::array ordered_strings;
    ordered_strings.push_back(make_dialogue_entry(std::string(kCachedBeforeJp)));
    ordered_strings.push_back(make_dialogue_entry(std::string(kUncachedTargetJp)));
    ordered_strings.push_back(make_dialogue_entry(std::string(kCachedAfterJp)));
    for (auto& item : base_corpus["tokuten.spt"].as_object()["strings"].as_array()) {
        ordered_strings.push_back(std::move(item));
    }
    base_corpus["tokuten.spt"].as_object()["strings"] = std::move(ordered_strings);
    write_file(input_path.u8string(), bj::serialize(bj::value(std::move(base_corpus))));

    auto state = std::make_shared<FakeGeminiState>();
    state->canned_response = make_gemini_response(
        bj::array{
            bj::object{{"thought", true},
                       {"text", "Checking lookahead shark context."}},
            bj::object{{"text", "[1] A rough sensation. Terrible sharkskin."}},
        },
        kLookaheadBatchThoughts, "STOP");

    translate::TranslateOptions opt;
    opt.batch_size = kLookaheadWindowBatchSize;
    opt.input_file = input_path.u8string();
    opt.cache_file = cache_path.u8string();
    opt.output_file = output_path.u8string();
    opt.test_dir = test_dir_path.u8string();
    opt.tsv_file = tsv_path.u8string();
    opt.client_factory = [state] { return std::make_unique<FakeGeminiClient>(state); };

    ASSERT_EQ(translate::translate_all(opt), 0);
    ASSERT_EQ(state->calls.size(), 1u);

    const std::string_view wire_prompt = as_sv(state->calls[0]
                                                   .body.as_object()
                                                   .at("contents")
                                                   .as_array()
                                                   .at(0)
                                                   .as_object()
                                                   .at("parts")
                                                   .as_array()
                                                   .at(0)
                                                   .as_object()
                                                   .at("text"));

    const auto prev_section =
        extract_tag_inner(wire_prompt, "<previous_context>", "</previous_context>");
    ASSERT_TRUE(prev_section.has_value());
    EXPECT_TRUE(prev_section->find(kCachedBeforeJp) != std::string_view::npos ||
                prev_section->find(kCachedBeforeEn) != std::string_view::npos);
    EXPECT_EQ(prev_section->find(kCachedAfterJp), std::string_view::npos);
    EXPECT_EQ(prev_section->find(kCachedAfterEn), std::string_view::npos);

    const auto after_section =
        extract_tag_inner(wire_prompt, "<context_after>", "</context_after>");
    ASSERT_TRUE(after_section.has_value());
    EXPECT_NE(after_section->find(kCachedAfterJp), std::string_view::npos);
    EXPECT_EQ(after_section->find(kCachedBeforeJp), std::string_view::npos);
    EXPECT_EQ(after_section->find(kCachedBeforeEn), std::string_view::npos);

    const translate::Cache reloaded = translate::load_cache(cache_path.u8string());
    ASSERT_NE(reloaded.get(std::string(kUncachedTargetJp)), nullptr);
    EXPECT_EQ(*reloaded.get(std::string(kUncachedTargetJp)), kUncachedTargetEn);
    ASSERT_TRUE(fs::is_regular_file(output_path));
    const bj::value out_json = json_parse_file(output_path.u8string());
    const bj::array& out_strings =
        out_json.as_object().at("tokuten.spt").as_object().at("strings").as_array();
    ASSERT_GE(out_strings.size(), 2u);
    EXPECT_EQ(as_sv(out_strings.at(1).as_object().at("translated")), kUncachedTargetEn);
}
