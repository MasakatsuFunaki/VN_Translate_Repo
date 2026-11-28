// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box component tests spanning bytecode extraction (`fav::extract`),
// cast-roster gating, batched LLM translation (`fav::translate::translate_all`),
// persistent resume cache & TSV generation (`fav::build_tsv`), and the proxy
// DLL's runtime CP932 lookup + 58-column word-wrap consumer (`translator_logic`).
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "translate/gemini_client.h"
#include "translate/translate_core.h"
#include "translator_logic.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace bj = boost::json;
namespace gm = fav::gemini;
namespace tr = fav::translate;

namespace {

constexpr int kSuccessExitCode = 0;
constexpr int kFatalRuntimeExitCode = 1;

constexpr std::uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;
constexpr std::size_t kScriptHeaderBytes = 4u;
constexpr std::size_t kNulTerminatorBytes = 1u;
constexpr std::uint8_t kNulTerminatorByte = 0x00u;
constexpr std::uint8_t kFuncHeaderOp = 0x01u;
constexpr std::uint8_t kCallSubOp = 0x02u;
constexpr std::uint8_t kReturnOp = 0x04u;
constexpr std::uint8_t kPushStringOp = 0x0Eu;
constexpr std::uint8_t kFaceGraphicOp = 0x10u;
constexpr std::uint8_t kFaceGraphicSlot = 0x01u;
constexpr std::uint8_t kZeroOperand = 0x00u;
constexpr std::size_t kMaxShortPushPayloadBytes = 254u;

constexpr std::int64_t kPromptTokens = 1200;
constexpr std::int64_t kCandidateTokens = 160;
constexpr std::int64_t kHealthyThoughts = 320;
constexpr std::int64_t kInitialRetryAttemptThoughts = 280;
constexpr std::int64_t kZeroTokens = 0;
constexpr unsigned char kAsciiCeiling = 0x80u;
constexpr unsigned char kNullByte = 0x00u;
constexpr std::size_t kExpectedTabsPerTsvRow = 1u;
constexpr std::size_t kNewlineCharBytes = 1u;

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

void append_hcb_u32(fav::Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kOneByteShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kTwoBytesShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kThreeBytesShift) & kByteMask));
}

void append_hcb_push_string(fav::Bytes& out, const std::string& utf8) {
    const fav::Bytes cp932 = fav::utf8_to_cp932_replace(utf8);
    if (cp932.size() > kMaxShortPushPayloadBytes) {
        throw std::runtime_error("Synthetic FVP string exceeds 1-byte push length");
    }
    const auto roundtrip = fav::cp932_to_utf8_strict(cp932);
    if (!roundtrip.has_value() || *roundtrip != utf8) {
        throw std::runtime_error("Synthetic FVP string is not CP932-clean: " + utf8);
    }
    out.push_back(kPushStringOp);
    out.push_back(static_cast<std::uint8_t>(cp932.size() + kNulTerminatorBytes));
    out.insert(out.end(), cp932.begin(), cp932.end());
    out.push_back(kNulTerminatorByte);
}

std::uint32_t append_speaker_subroutine(fav::Bytes& body, const std::string& padded_jp_name) {
    const std::uint32_t sub_offset =
        static_cast<std::uint32_t>(kScriptHeaderBytes + body.size());
    body.push_back(kFuncHeaderOp);
    body.push_back(kFaceGraphicSlot);
    body.push_back(kFaceGraphicSlot);
    body.push_back(kFaceGraphicOp);
    body.push_back(kFaceGraphicSlot);
    append_hcb_push_string(body, padded_jp_name);
    body.push_back(kReturnOp);
    return sub_offset;
}

void append_spoken_line(fav::Bytes& body, std::uint32_t speaker_sub, const std::string& jp_line) {
    body.push_back(kCallSubOp);
    append_hcb_u32(body, speaker_sub);
    append_hcb_push_string(body, jp_line);
}

fav::Bytes finalize_hcb(const fav::Bytes& body) {
    fav::Bytes hcb;
    append_hcb_u32(hcb, static_cast<std::uint32_t>(kScriptHeaderBytes + body.size()));
    hcb.insert(hcb.end(), body.begin(), body.end());
    return hcb;
}

bj::object make_wire_response(std::string_view finish_reason,
                              std::int64_t prompt_tokens,
                              std::int64_t candidates_tokens,
                              std::int64_t thoughts_tokens,
                              std::string_view visible_text) {
    bj::object part;
    part["text"] = visible_text;
    bj::array parts;
    parts.emplace_back(std::move(part));

    bj::object content;
    content["role"] = "model";
    content["parts"] = std::move(parts);

    bj::object candidate;
    candidate["content"] = std::move(content);
    candidate["finishReason"] = finish_reason;

    bj::array candidates;
    candidates.emplace_back(std::move(candidate));

    bj::object usage;
    usage["promptTokenCount"] = prompt_tokens;
    usage["candidatesTokenCount"] = candidates_tokens;
    usage["thoughtsTokenCount"] = thoughts_tokens;
    usage["cachedContentTokenCount"] = kZeroTokens;

    bj::object response;
    response["candidates"] = std::move(candidates);
    response["usageMetadata"] = std::move(usage);
    return response;
}

std::string make_numbered_block(const char* prefix, int count) {
    std::string out;
    for (int i = 1; i <= count; ++i) {
        out += std::to_string(i) + ". " + prefix + " line " + std::to_string(i) + ".\n";
    }
    return out;
}

class QueuedEndpoint final : public gm::Endpoint {
public:
    void enqueue(bj::value response) { responses_.push_back(std::move(response)); }

    bj::value generate_content(std::string_view /*model*/,
                               const bj::object& wire_request,
                               int /*timeout_sec*/) override {
        recorded_requests_.push_back(wire_request);
        const std::size_t idx = call_count_++;
        return responses_.at(idx);
    }

    std::size_t call_count() const noexcept { return call_count_; }
    const std::vector<bj::object>& recorded_requests() const noexcept {
        return recorded_requests_;
    }

private:
    std::vector<bj::value> responses_;
    std::vector<bj::object> recorded_requests_;
    std::size_t call_count_ = 0u;
};

bool has_any_tmp_files(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().extension() == ".tmp") {
            return true;
        }
    }
    return false;
}

std::string to_cp932_str(const std::string& utf8) {
    const fav::Bytes b = fav::utf8_to_cp932_replace(utf8);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

}  // namespace

TEST(TranslatePipelineCt,
     NoContextRetryZeroThoughtMaskingRefusedAndAbortsOnSecondStrikeWithCleanResume) {
    constexpr int kBatchSize = 16;
    constexpr int kTotalLines = 48;
    constexpr std::size_t kExpectedPass1Calls = 4u;
    constexpr std::size_t kExpectedPass2TotalCalls = 6u;

    const ScopedTempDir sandbox("fav_ct_retry_zero_thought_masking");
    const fs::path& dir = sandbox.path();
    const std::string hcb_path = (dir / "Sakura.hcb").u8string();
    const std::string extracted_path = (dir / "extracted_text.json").u8string();
    const std::string cache_path = (dir / "translation_cache_gemini.json").u8string();
    const std::string output_path = (dir / "translated_text.json").u8string();
    const std::string tsv_path = (dir / "translation_table.tsv").u8string();
    const std::string log_dir = (dir / "last_gemini_translate").u8string();

    std::vector<std::string> jp_lines;
    jp_lines.reserve(static_cast<std::size_t>(kTotalLines));
    {
        fav::Bytes body;
        const std::uint32_t kuro_sub = append_speaker_subroutine(body, "　クロ　");
        const std::uint32_t haru_sub = append_speaker_subroutine(body, "　ハル　");
        body.push_back(kFuncHeaderOp);
        body.push_back(kZeroOperand);
        body.push_back(kZeroOperand);
        for (int i = 0; i < kTotalLines; ++i) {
            const std::string jp =
                "「夜の桜が静かに舞い落ちる場面その" + std::to_string(i + 1) + "」";
            jp_lines.push_back(jp);
            append_spoken_line(body, (i % 2 == 0) ? kuro_sub : haru_sub, jp);
        }
        body.push_back(kReturnOp);
        fav::write_file(hcb_path, finalize_hcb(body));
    }

    const fav::Bytes original_hcb_bytes = fav::read_file(hcb_path);
    ASSERT_EQ(fav::extract::run_extract(hcb_path, extracted_path), kSuccessExitCode);
    const fav::Bytes original_extracted_bytes = fav::read_file(extracted_path);

    QueuedEndpoint endpoint;
    // Call 1 (Batch 1, 16 lines): healthy reasoning -> cached.
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kHealthyThoughts,
        make_numbered_block("BatchOne", kBatchSize)));
    // Call 2 (Batch 2 initial attempt with context): thoughts > 0, but unnumbered
    // commentary produces empty parsed translations -> triggers no-context retry.
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kInitialRetryAttemptThoughts,
        "Unnumbered commentary without any line prefixes."));
    // Call 3 (Batch 2 no-context retry): thoughts == 0 (Strike 1/2) with numbered
    // lines. Must NOT be masked by Call 2's 280 thoughts; must refuse to cache!
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kZeroTokens,
        make_numbered_block("DegradedZeroThoughtRetry", kBatchSize)));
    // Call 4 (Batch 3, 16 lines): thoughts == 0 (Strike 2/2) -> aborts run!
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kZeroTokens,
        make_numbered_block("DegradedStrikeTwo", kBatchSize)));

    tr::TranslateOptions opt;
    opt.batch_size = kBatchSize;
    opt.model = "gemini-4-carbon";
    opt.thinking_level = "MEDIUM";
    opt.endpoint = &endpoint;
    opt.hcb_file = hcb_path;
    opt.input_file = extracted_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.tsv_file = tsv_path;
    opt.last_translate_dir = log_dir;

    ASSERT_EQ(tr::translate_all(opt), kFatalRuntimeExitCode);
    ASSERT_EQ(endpoint.call_count(), kExpectedPass1Calls);

    // Read-only inputs must remain byte-for-byte untouched and zero .tmp files left.
    EXPECT_EQ(fav::read_file(hcb_path), original_hcb_bytes);
    EXPECT_EQ(fav::read_file(extracted_path), original_extracted_bytes);
    EXPECT_FALSE(has_any_tmp_files(dir));

    // Cumulative billing telemetry on Batch 2 must preserve Call 2's 280 thought tokens,
    // while winning_thoughts == 0 prevented Batch 2 from entering cache.json.
    const fs::path telemetry_path = fs::u8path(log_dir) / "last_batch_telemetry.json";
    ASSERT_TRUE(fs::exists(telemetry_path));
    const bj::object telemetry = fav::json_parse_file(telemetry_path.u8string()).as_object();
    EXPECT_EQ(telemetry.at("thought_tokens").as_int64(), kInitialRetryAttemptThoughts);
    EXPECT_EQ(telemetry.at("cumulative_thought_tokens").as_int64(),
              kHealthyThoughts + kInitialRetryAttemptThoughts);

    const tr::Cache pass1_cache = tr::load_cache(cache_path);
    ASSERT_EQ(jp_lines.size(), static_cast<std::size_t>(kTotalLines));
    for (int i = 0; i < kBatchSize; ++i) {
        const std::string* cached = pass1_cache.get(jp_lines[static_cast<std::size_t>(i)]);
        ASSERT_NE(cached, nullptr) << "Batch 1 line " << i << " missing from flushed cache";
        EXPECT_EQ(*cached, "BatchOne line " + std::to_string(i + 1) + ".");
    }
    for (int i = kBatchSize; i < kTotalLines; ++i) {
        EXPECT_FALSE(pass1_cache.contains(jp_lines[static_cast<std::size_t>(i)]))
            << "Zero-thought line " << i << " leaked into cache despite winning_thoughts == 0";
    }

    // Pass 2 (Stateful Resume): healthy model translates exact remaining 32 lines.
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kHealthyThoughts,
        make_numbered_block("RecoveredBatchTwo", kBatchSize)));
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kHealthyThoughts,
        make_numbered_block("RecoveredBatchThree", kBatchSize)));

    ASSERT_EQ(tr::translate_all(opt), kSuccessExitCode);
    ASSERT_EQ(endpoint.call_count(), kExpectedPass2TotalCalls);

    const tr::Cache pass2_cache = tr::load_cache(cache_path);
    for (int i = 0; i < kBatchSize; ++i) {
        ASSERT_NE(pass2_cache.get(jp_lines[static_cast<std::size_t>(i)]), nullptr);
        EXPECT_EQ(*pass2_cache.get(jp_lines[static_cast<std::size_t>(i)]),
                  "BatchOne line " + std::to_string(i + 1) + ".");
    }
    for (int i = 0; i < kBatchSize; ++i) {
        const std::size_t b2 = static_cast<std::size_t>(kBatchSize + i);
        const std::size_t b3 = static_cast<std::size_t>(2 * kBatchSize + i);
        ASSERT_NE(pass2_cache.get(jp_lines[b2]), nullptr);
        EXPECT_EQ(*pass2_cache.get(jp_lines[b2]),
                  "RecoveredBatchTwo line " + std::to_string(i + 1) + ".");
        ASSERT_NE(pass2_cache.get(jp_lines[b3]), nullptr);
        EXPECT_EQ(*pass2_cache.get(jp_lines[b3]),
                  "RecoveredBatchThree line " + std::to_string(i + 1) + ".");
    }

    // Pass 3 (Idempotent Replay): 0 additional API calls and byte-identical outputs.
    const fav::Bytes pass2_cache_bytes = fav::read_file(cache_path);
    const fav::Bytes pass2_tsv_bytes = fav::read_file(tsv_path);
    ASSERT_EQ(tr::translate_all(opt), kSuccessExitCode);
    EXPECT_EQ(endpoint.call_count(), kExpectedPass2TotalCalls);
    EXPECT_EQ(fav::read_file(cache_path), pass2_cache_bytes);
    EXPECT_EQ(fav::read_file(tsv_path), pass2_tsv_bytes);
}

TEST(TranslatePipelineCt, SubsequentBatchBelowSixteenLineSeamAllowsZeroThoughtCompletion) {
    constexpr int kBelowSeamBatchSize = 15;
    constexpr int kTotalSeamLines = 30;
    constexpr std::size_t kExpectedTwoCalls = 2u;

    const ScopedTempDir sandbox("fav_ct_fifteen_line_seam");
    const fs::path& dir = sandbox.path();
    const std::string hcb_path = (dir / "Sakura.hcb").u8string();
    const std::string extracted_path = (dir / "extracted_text.json").u8string();
    const std::string cache_path = (dir / "translation_cache_gemini.json").u8string();
    const std::string output_path = (dir / "translated_text.json").u8string();
    const std::string tsv_path = (dir / "translation_table.tsv").u8string();

    std::vector<std::string> jp_lines;
    jp_lines.reserve(static_cast<std::size_t>(kTotalSeamLines));
    {
        fav::Bytes body;
        const std::uint32_t kuro_sub = append_speaker_subroutine(body, "　クロ　");
        const std::uint32_t haru_sub = append_speaker_subroutine(body, "　ハル　");
        body.push_back(kFuncHeaderOp);
        body.push_back(kZeroOperand);
        body.push_back(kZeroOperand);
        for (int i = 0; i < kTotalSeamLines; ++i) {
            const std::string jp =
                "「境界十五行の静かな台詞その" + std::to_string(i + 1) + "」";
            jp_lines.push_back(jp);
            append_spoken_line(body, (i % 2 == 0) ? kuro_sub : haru_sub, jp);
        }
        body.push_back(kReturnOp);
        fav::write_file(hcb_path, finalize_hcb(body));
    }

    ASSERT_EQ(fav::extract::run_extract(hcb_path, extracted_path), kSuccessExitCode);

    QueuedEndpoint endpoint;
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kHealthyThoughts,
        make_numbered_block("SeamBatchOne", kBelowSeamBatchSize)));
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kZeroTokens,
        make_numbered_block("SeamBatchTwo", kBelowSeamBatchSize)));

    tr::TranslateOptions opt;
    opt.batch_size = kBelowSeamBatchSize;
    opt.model = "gemini-4-carbon";
    opt.thinking_level = "MEDIUM";
    opt.endpoint = &endpoint;
    opt.hcb_file = hcb_path;
    opt.input_file = extracted_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.tsv_file = tsv_path;
    opt.last_translate_dir = (dir / "last_gemini_translate").u8string();

    ASSERT_EQ(tr::translate_all(opt), kSuccessExitCode);
    ASSERT_EQ(endpoint.call_count(), kExpectedTwoCalls);

    const tr::Cache saved = tr::load_cache(cache_path);
    ASSERT_EQ(jp_lines.size(), static_cast<std::size_t>(kTotalSeamLines));
    for (int i = 0; i < kBelowSeamBatchSize; ++i) {
        const std::size_t second_idx = static_cast<std::size_t>(kBelowSeamBatchSize + i);
        ASSERT_NE(saved.get(jp_lines[second_idx]), nullptr);
        EXPECT_EQ(*saved.get(jp_lines[second_idx]),
                  "SeamBatchTwo line " + std::to_string(i + 1) + ".");
    }
}

TEST(TranslatePipelineCt,
     RejectsNonAsciiAndNulTranslationsBeforeCachingAndFeedsCleanTsvIntoProxyConsumer) {
    constexpr int kSixLineBatch = 6;
    constexpr std::size_t kExpectedSingleCall = 1u;
    constexpr std::size_t kExpectedTwoCalls = 2u;
    constexpr std::size_t kFvpGridColumns = 58u;

    const ScopedTempDir sandbox("fav_ct_ascii_gate_and_proxy_consumer");
    const fs::path& dir = sandbox.path();
    const std::string hcb_path = (dir / "Sakura.hcb").u8string();
    const std::string extracted_path = (dir / "extracted_text.json").u8string();
    const std::string cache_path = (dir / "translation_cache_gemini.json").u8string();
    const std::string output_path = (dir / "translated_text.json").u8string();
    const std::string tsv_path = (dir / "translation_table.tsv").u8string();

    constexpr const char* kJpLine1KuroPromise =
        "「大丈夫。約束する、約束するよ。決してきみを独りにはしない」";
    constexpr const char* kJpLine2NarrationCafe =
        "　<桜|さくら>の花びらがカフェの窓辺を静かに舞い落ちた。";
    constexpr const char* kJpLine3ToyaGate =
        "「十夜はずっと校門の前で待っていたんだよ」";
    constexpr const char* kJpLine4TicketPrice =
        "「今夜の乗車券は百ユーロだって書いてあるね」";
    constexpr const char* kJpLine5NightPetals =
        "　夜風に乗って薄紅色の花びらがふわりと揺れた。";
    constexpr const char* kJpLine6QuietNight =
        "「静かな夜だね、大雅」";

    {
        fav::Bytes body;
        const std::uint32_t kuro_sub = append_speaker_subroutine(body, "　クロ　");
        const std::uint32_t chiwa_sub = append_speaker_subroutine(body, "　千和　");
        const std::uint32_t taiga_sub = append_speaker_subroutine(body, "　大雅　");

        body.push_back(kFuncHeaderOp);
        body.push_back(kZeroOperand);
        body.push_back(kZeroOperand);
        append_spoken_line(body, kuro_sub, kJpLine1KuroPromise);
        append_hcb_push_string(body, kJpLine2NarrationCafe);
        append_spoken_line(body, chiwa_sub, kJpLine3ToyaGate);
        append_spoken_line(body, taiga_sub, kJpLine4TicketPrice);
        append_hcb_push_string(body, kJpLine5NightPetals);
        append_spoken_line(body, kuro_sub, kJpLine6QuietNight);
        body.push_back(kReturnOp);
        fav::write_file(hcb_path, finalize_hcb(body));
    }

    ASSERT_EQ(fav::extract::run_extract(hcb_path, extracted_path), kSuccessExitCode);

    // Pass 1 response: Lines 1-2 fold cleanly to 7-bit ASCII; Lines 3-6 contain
    // post-sanitize_ascii non-ASCII UTF-8 (`ō`, `€`, emoji) or leaked kanji and
    // must be rejected before entering cache.json or translation_table.tsv.
    const std::string pass1_visible =
        "1. [Kuro] It’s okay. I promise, I promise. I won’t ever let you be alone.\n"
        "2. <Cherry blossoms|さくら> drifted quietly past the café window.\n"
        "3. Tōya was waiting in front of the school gate all along.\n"
        "4. It says tonight's train ticket costs 100€.\n"
        "5. Pale pink petals swayed gently on the night breeze 🌸\n"
        "6. It is a quiet 夜, Taiga.\n";

    QueuedEndpoint endpoint;
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kHealthyThoughts, pass1_visible));

    tr::TranslateOptions opt;
    opt.batch_size = kSixLineBatch;
    opt.model = "gemini-4-carbon";
    opt.thinking_level = "MEDIUM";
    opt.endpoint = &endpoint;
    opt.hcb_file = hcb_path;
    opt.input_file = extracted_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.tsv_file = tsv_path;
    opt.last_translate_dir = (dir / "last_gemini_translate").u8string();

    ASSERT_EQ(tr::translate_all(opt), kSuccessExitCode);
    ASSERT_EQ(endpoint.call_count(), kExpectedSingleCall);
    EXPECT_FALSE(has_any_tmp_files(dir));

    const tr::Cache pass1_cache = tr::load_cache(cache_path);
    ASSERT_NE(pass1_cache.get(kJpLine1KuroPromise), nullptr);
    EXPECT_EQ(*pass1_cache.get(kJpLine1KuroPromise),
              "It's okay. I promise, I promise. I won't ever let you be alone.");
    ASSERT_NE(pass1_cache.get(kJpLine2NarrationCafe), nullptr);
    EXPECT_EQ(*pass1_cache.get(kJpLine2NarrationCafe),
              "Cherry blossoms drifted quietly past the cafe window.");
    EXPECT_FALSE(pass1_cache.contains(kJpLine3ToyaGate));
    EXPECT_FALSE(pass1_cache.contains(kJpLine4TicketPrice));
    EXPECT_FALSE(pass1_cache.contains(kJpLine5NightPetals));
    EXPECT_FALSE(pass1_cache.contains(kJpLine6QuietNight));

    // Verify every English column emitted in Pass 1 TSV is strictly 7-bit ASCII.
    const fav::Bytes pass1_tsv_raw = fav::read_file(tsv_path);
    const std::string pass1_tsv(
        reinterpret_cast<const char*>(pass1_tsv_raw.data()), pass1_tsv_raw.size());
    EXPECT_EQ(pass1_tsv.find(kJpLine3ToyaGate), std::string::npos);
    EXPECT_EQ(pass1_tsv.find(kJpLine4TicketPrice), std::string::npos);
    EXPECT_EQ(pass1_tsv.find(kJpLine5NightPetals), std::string::npos);
    EXPECT_EQ(pass1_tsv.find(kJpLine6QuietNight), std::string::npos);

    std::size_t pos = 0u;
    while (pos < pass1_tsv.size()) {
        const std::size_t nl = pass1_tsv.find('\n', pos);
        std::string row = pass1_tsv.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? pass1_tsv.size() : (nl + kNewlineCharBytes);
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (row.empty()) {
            continue;
        }
        const std::size_t tab = row.find('\t');
        ASSERT_NE(tab, std::string::npos) << row;
        ASSERT_EQ(static_cast<std::size_t>(std::count(row.begin(), row.end(), '\t')),
                  kExpectedTabsPerTsvRow) << row;
        const std::string en_col = row.substr(tab + 1u);
        for (const char ch : en_col) {
            const auto b = static_cast<unsigned char>(ch);
            EXPECT_NE(b, kNullByte) << en_col;
            EXPECT_LT(b, kAsciiCeiling) << "Non-ASCII byte in TSV English column: " << en_col;
        }
    }

    // Pass 2 (Resume): only the 4 rejected lines must be re-requested, and clean
    // ASCII replacements complete the runtime TSV.
    const std::string pass2_visible =
        "1. Toya was waiting in front of the school gate all along.\n"
        "2. It says tonight's train ticket costs 100 EUR.\n"
        "3. Pale pink petals swayed gently on the night breeze.\n"
        "4. It is a quiet night, Taiga.\n";
    endpoint.enqueue(make_wire_response(
        "STOP", kPromptTokens, kCandidateTokens, kHealthyThoughts, pass2_visible));

    ASSERT_EQ(tr::translate_all(opt), kSuccessExitCode);
    ASSERT_EQ(endpoint.call_count(), kExpectedTwoCalls);

    const bj::object& second_wire = endpoint.recorded_requests().at(1u);
    const std::string second_prompt(
        second_wire.at("contents").as_array().at(0u).as_object()
            .at("parts").as_array().at(0u).as_object()
            .at("text").as_string());
    EXPECT_NE(second_prompt.find("Output EXACTLY 4 numbered translations"), std::string::npos)
        << second_prompt;
    EXPECT_NE(second_prompt.find(kJpLine3ToyaGate), std::string::npos);
    EXPECT_EQ(second_prompt.find("1. [Kuro] 「大丈夫。"), std::string::npos);

    // Producer -> Real Proxy Consumer (`translator_logic`) verification:
    // Load the generated TSV, re-key JP UTF-8 -> CP932 just like `LoadTranslations()`
    // in `translator_favorite.cpp`, and verify exact/normalized lookup + 58-column
    // space-padded wrapping on FVP runtime strings.
    translator_logic::TranslationMap utf8_map;
    ASSERT_TRUE(translator_logic::LoadTsvFile(tsv_path, utf8_map));
    ASSERT_FALSE(utf8_map.empty());

    translator_logic::TranslationMap cp932_map;
    for (const auto& [jp_utf8, en_ascii] : utf8_map) {
        for (const char ch : en_ascii) {
            const auto b = static_cast<unsigned char>(ch);
            ASSERT_NE(b, kNullByte);
            ASSERT_LT(b, kAsciiCeiling) << en_ascii;
        }
        cp932_map.emplace(to_cp932_str(jp_utf8), en_ascii);
    }

    // 1. Padded speaker nameplate `　クロ　` normalizes to `クロ` -> `"Kuro"`.
    const std::string runtime_kuro_plate = to_cp932_str("　クロ　");
    const std::string norm_kuro = translator_logic::NormalizeCp932LookupKey(
        runtime_kuro_plate.data(), runtime_kuro_plate.size());
    const std::string* kuro_en = translator_logic::FindTranslation(cp932_map, norm_kuro);
    ASSERT_NE(kuro_en, nullptr);
    EXPECT_EQ(*kuro_en, "Kuro");

    // 2. Spoken line hits exact CP932 key and wraps cleanly on FVP's 58-col grid
    //    (56 chars + 2 spaces to reach col 58, then "alone." at col 0 of Row 2).
    const std::string runtime_spoken = to_cp932_str(kJpLine1KuroPromise);
    const std::string* promise_en =
        translator_logic::FindTranslation(cp932_map, runtime_spoken);
    ASSERT_NE(promise_en, nullptr);
    EXPECT_EQ(translator_logic::WrapAsciiToColumnGrid(*promise_en, kFvpGridColumns),
              "It's okay. I promise, I promise. I won't ever let you be  alone.");

    // 3. Narration with leading `　` (`0x81 0x40`) normalizes to trimmed TSV key.
    const std::string runtime_narration = to_cp932_str(kJpLine2NarrationCafe);
    const std::string norm_narration = translator_logic::NormalizeCp932LookupKey(
        runtime_narration.data(), runtime_narration.size());
    const std::string* narration_en =
        translator_logic::FindTranslation(cp932_map, norm_narration);
    ASSERT_NE(narration_en, nullptr);
    EXPECT_EQ(*narration_en, "Cherry blossoms drifted quietly past the cafe window.");

    // 4. Recovered Pass 2 line (`Toya`) is now present in the consumer table.
    const std::string runtime_toya = to_cp932_str(kJpLine3ToyaGate);
    const std::string* toya_en = translator_logic::FindTranslation(cp932_map, runtime_toya);
    ASSERT_NE(toya_en, nullptr);
    EXPECT_EQ(*toya_en, "Toya was waiting in front of the school gate all along.");
}
