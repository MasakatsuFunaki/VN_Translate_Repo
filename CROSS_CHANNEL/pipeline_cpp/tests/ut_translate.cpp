// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for CROSS_CHANNEL's Gemini translation subsystem:
// v1beta wire codec, dotenv/API-key resolution, 75-speaker glossary, 5-check
// speaker gate, text postprocessing & strict 1..N response parsing, offline
// cache-only table generation, and live BatchTranslator resilience cascades.
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = crc::translate;

namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpForbidden = 403;
constexpr int kHttpTooManyRequests = 429;
constexpr int kHttpInternalError = 500;
constexpr int kHttpBadGateway = 502;
constexpr int kHttpServiceUnavailable = 503;

constexpr std::size_t kSingleElementCount = 1u;
constexpr std::size_t kTwoElements = 2u;
constexpr std::size_t kThreeElements = 3u;
constexpr std::size_t kFourElements = 4u;
constexpr std::size_t kFiveElements = 5u;
constexpr std::size_t kSafetySettingFieldCount = 2u;
constexpr std::size_t kExpectedSafetyCategoryCount = 5u;
constexpr std::size_t kExpectedGlossarySize = 75u;

constexpr std::int64_t kZeroThinkingBudget = 0;
constexpr std::int64_t kDisabledThinkingBudget = -1;
constexpr std::int64_t kMinClampedOutputTokens = 1;
constexpr std::int64_t kTokensPerBatchLine = 300;
constexpr int kSmallClampBatchSize = 54;
constexpr int kFirstUnclampedBatchSize = 55;
constexpr int kMaxUnclampedBatchSize = 218;
constexpr int kClampedHighBatchSize = 219;
constexpr int kHugeBatchSize = 500;

constexpr std::int64_t kLargePromptTokens = 5'000'000'000LL;
constexpr std::int64_t kLargeCachedTokens = 3'000'000'000LL;
constexpr std::int64_t kSampleOutputTokens = 420;
constexpr std::int64_t kSampleThoughtTokens = 1337;
constexpr std::int64_t kPromptBlockedTokens = 812;
constexpr std::int64_t kPromptBlockedCachedTokens = 512;
constexpr std::int64_t kEmptyCandidatesPromptTokens = 250;
constexpr std::int64_t kCandidateBlockedPromptTokens = 140;
constexpr std::int64_t kCandidateBlockedThoughtTokens = 15;
constexpr std::int64_t kDefaultFakePromptTokens = 240;
constexpr std::int64_t kDefaultFakeOutputTokens = 60;
constexpr std::int64_t kDefaultFakeThoughtTokens = 30;
constexpr std::int64_t kExpensiveOutputTokens = 100'000;

constexpr std::int64_t kOneMillionTokens = 1'000'000;
constexpr std::int64_t kHalfMillionTokens = 500'000;
constexpr double kExpectedArgonUncachedPlusOutputUsd = 12.0;
constexpr double kExpectedPro31UncachedPlusOutputUsd = 14.0;
constexpr double kEurUsdMultiplier = 0.95;
constexpr double kCostEpsilon = 1e-6;
constexpr double kTightBudgetEur = 0.50;
constexpr double kGenerousBudgetEur = 50.0;

constexpr int kMaxSpeakerCodepoints = 20;
constexpr int kTooLongSpeakerCodepoints = 21;
constexpr int kMinSpeakerCheckId = 1;
constexpr int kMaxSpeakerCheckId = 5;
constexpr int kExtractedJsonIndent = 1;
constexpr std::int64_t kOffsetStep = 64;
constexpr std::size_t kPaidCacheOverThreshold = 105u;
constexpr std::size_t kLargePaidCacheCount = 56103u;
constexpr char kPrintableAsciiMin = 0x20;
constexpr char kPrintableAsciiMax = 0x7E;

constexpr const wchar_t* kGeminiEnvVarWide = L"GEMINI_API_KEY";
constexpr const char* kSharedCoverageJp = "「了解。」";
constexpr const char* kSharedCoverageEn = "\"Understood.\"";

enum class ExpectedErrorKind {
    BaseApi,
    RateLimit,
    Transient,
};

struct MaxTokensClampCase {
    std::int64_t requested;
    std::int64_t expected;
};

struct ErrorClassificationCase {
    std::string_view label;
    int http_status;
    std::string_view body;
    ExpectedErrorKind kind;
    std::string_view message_substr;
};

struct DotenvCase {
    std::string_view label;
    std::string_view contents;
    std::string_view key;
    std::string_view expected;
};

struct TextTransformCase {
    std::string_view label;
    std::string_view input;
    std::string_view expected;
};

struct LinePairTransformCase {
    std::string_view label;
    std::string_view jp;
    std::string_view raw_en;
    std::string_view expected;
};

struct ValidationCase {
    std::string_view label;
    std::string_view jp;
    std::string_view en;
    bool expected_valid;
};

std::optional<std::wstring> read_process_env(const wchar_t* name) {
    wchar_t* raw = nullptr;
    std::size_t len = 0;
    if (::_wdupenv_s(&raw, &len, name) != 0 || raw == nullptr) {
        return std::nullopt;
    }
    std::wstring value(raw);
    std::free(raw);
    return value;
}

class ScopedGeminiEnvGuard {
public:
    ScopedGeminiEnvGuard() : saved_(read_process_env(kGeminiEnvVarWide)) {
        ::_wputenv_s(kGeminiEnvVarWide, L"");
    }

    ~ScopedGeminiEnvGuard() {
        if (saved_.has_value()) {
            ::_wputenv_s(kGeminiEnvVarWide, saved_->c_str());
            return;
        }
        ::_wputenv_s(kGeminiEnvVarWide, L"");
    }

    ScopedGeminiEnvGuard(const ScopedGeminiEnvGuard&) = delete;
    ScopedGeminiEnvGuard& operator=(const ScopedGeminiEnvGuard&) = delete;

private:
    std::optional<std::wstring> saved_;
};

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name = "crc_ut_translate_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(name);
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

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

void write_binary_file(const fs::path& path, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good());
}

std::string read_file_string(const fs::path& path) {
    const crc::Bytes raw = crc::read_file(path.u8string());
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
}

std::string_view json_sv(const bj::value& v) {
    const bj::string& s = v.as_string();
    return std::string_view(s.data(), s.size());
}

std::int64_t json_i64(const bj::value& v) {
    if (v.is_int64()) {
        return v.as_int64();
    }
    if (v.is_uint64()) {
        return static_cast<std::int64_t>(v.as_uint64());
    }
    ADD_FAILURE() << "Expected integer JSON value, got: " << bj::serialize(v);
    return 0;
}

void expect_error_classification(const ErrorClassificationCase& tc) {
    try {
        (void)tr::parse_gemini_response_json(tc.body, tc.http_status);
        FAIL() << "Expected exception for case: " << tc.label;
    } catch (const tr::RateLimitError& err) {
        ASSERT_EQ(tc.kind, ExpectedErrorKind::RateLimit);
        EXPECT_EQ(err.http_status(), tc.http_status);
        if (!tc.message_substr.empty()) {
            EXPECT_NE(std::string_view(err.what()).find(tc.message_substr),
                      std::string_view::npos)
                << err.what();
        }
    } catch (const tr::TransientApiError& err) {
        ASSERT_EQ(tc.kind, ExpectedErrorKind::Transient);
        EXPECT_EQ(err.http_status(), tc.http_status);
        if (!tc.message_substr.empty()) {
            EXPECT_NE(std::string_view(err.what()).find(tc.message_substr),
                      std::string_view::npos)
                << err.what();
        }
    } catch (const tr::ApiError& err) {
        ASSERT_EQ(tc.kind, ExpectedErrorKind::BaseApi);
        EXPECT_EQ(err.http_status(), tc.http_status);
        if (!tc.message_substr.empty()) {
            EXPECT_NE(std::string_view(err.what()).find(tc.message_substr),
                      std::string_view::npos)
                << err.what();
        }
    }
}

bj::object make_entry(std::int64_t offset, std::string_view type,
                      std::string_view speaker, std::string_view text) {
    bj::object o;
    o["offset"] = offset;
    o["type"] = type;
    o["speaker"] = speaker;
    o["text"] = text;
    return o;
}

// Builds an extracted corpus starting with 1 dialogue row per glossary speaker
// (all sharing `kSharedCoverageJp` so the 5-check Speaker Gate sees 75/75
// bidirectional speaker coverage while adding only 1 deduplicated JP key),
// followed by `extra_rows`.
bj::object make_corpus_with_all_speakers(const std::vector<bj::object>& extra_rows) {
    bj::array strings;
    std::int64_t offset = static_cast<std::int64_t>(crc::extract::GAME_START_OFFSET);
    for (const auto& [jp_speaker, en_speaker] : tr::name_translations_ordered()) {
        (void)en_speaker;
        strings.push_back(make_entry(offset, "dialogue", jp_speaker, kSharedCoverageJp));
        offset += kOffsetStep;
    }
    for (bj::object row : extra_rows) {
        if (!row.contains("offset") || json_i64(row.at("offset")) == 0) {
            row["offset"] = offset;
            offset += kOffsetStep;
        }
        strings.push_back(std::move(row));
    }
    bj::object sn_block;
    sn_block["strings"] = std::move(strings);
    bj::object root;
    root["sn.bin"] = std::move(sn_block);
    return root;
}

bj::object make_corpus_with_prepended_rows(const std::vector<bj::object>& front_rows) {
    bj::array strings;
    std::int64_t offset = static_cast<std::int64_t>(crc::extract::GAME_START_OFFSET);
    for (bj::object row : front_rows) {
        if (!row.contains("offset") || json_i64(row.at("offset")) == 0) {
            row["offset"] = offset;
            offset += kOffsetStep;
        }
        strings.push_back(std::move(row));
    }
    for (const auto& [jp_speaker, en_speaker] : tr::name_translations_ordered()) {
        (void)en_speaker;
        strings.push_back(make_entry(offset, "dialogue", jp_speaker, kSharedCoverageJp));
        offset += kOffsetStep;
    }
    bj::object sn_block;
    sn_block["strings"] = std::move(strings);
    bj::object root;
    root["sn.bin"] = std::move(sn_block);
    return root;
}

void seed_glossary_and_coverage_line(tr::Cache& cache) {
    for (const auto& [jp, en] : tr::name_translations_ordered()) {
        cache.set_default(jp, en);
    }
    cache.set(kSharedCoverageJp, kSharedCoverageEn);
}

bool is_printable_ascii_only(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch >= static_cast<unsigned char>(kPrintableAsciiMin) &&
               ch <= static_cast<unsigned char>(kPrintableAsciiMax);
    });
}

// Extracts the numbered JP lines (still prompt-escaped) from `<lines_to_translate>`
// inside `user_prompt` so FakeTranslationTransport can respond to any batch or
// recursive bisection slice strictly from public prompt framing.
std::vector<std::string> parse_requested_escaped_jps(
    std::string_view user_prompt,
    const std::unordered_map<std::string, std::string>& escaped_jp_to_en) {
    std::string_view section = user_prompt;
    constexpr std::string_view kOpenTag = "<lines_to_translate>";
    constexpr std::string_view kCloseTag = "</lines_to_translate>";
    constexpr std::string_view kAfterTag = "<context_after>";

    const std::size_t open_pos = section.find(kOpenTag);
    if (open_pos != std::string_view::npos) {
        section.remove_prefix(open_pos + kOpenTag.size());
    }
    const std::size_t close_pos = section.find(kCloseTag);
    if (close_pos != std::string_view::npos) {
        section = section.substr(0, close_pos);
    } else {
        const std::size_t after_pos = section.find(kAfterTag);
        if (after_pos != std::string_view::npos) {
            section = section.substr(0, after_pos);
        }
    }

    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos < section.size()) {
        const std::size_t nl = section.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
                                    ? section.substr(pos)
                                    : section.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? section.size() : (nl + 1u);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1u);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1u);
        }
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line.front()))) {
            continue;
        }
        std::size_t i = 0;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= line.size() || (line[i] != '.' && line[i] != ')' && line[i] != ':')) {
            continue;
        }
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        std::string_view payload = line.substr(i);
        if (escaped_jp_to_en.count(std::string(payload)) == 0 &&
            !payload.empty() && payload.front() == '[') {
            const std::size_t tag_end = payload.find("] ");
            if (tag_end != std::string_view::npos) {
                payload.remove_prefix(tag_end + 2u);
            }
        }
        result.emplace_back(payload);
    }
    return result;
}

class FakeTranslationTransport final : public tr::ITranslationTransport {
public:
    void register_translation(const std::string& jp, const std::string& en) {
        escaped_jp_to_en_[tr::escape_prompt_line(jp)] = tr::escape_prompt_line(en);
    }

    void block_jp(const std::string& jp) {
        blocked_escaped_jps_.insert(tr::escape_prompt_line(jp));
    }

    void unblock_jp(const std::string& jp) {
        blocked_escaped_jps_.erase(tr::escape_prompt_line(jp));
    }

    void set_output_tokens_per_call(std::int64_t tokens) noexcept {
        output_tokens_per_call_ = tokens;
    }

    void set_custom_hook(
        std::function<std::optional<tr::GeminiResponse>(
            std::size_t call_index, const tr::GeminiRequest& req,
            const std::vector<std::string>& requested_jps)> hook) {
        custom_hook_ = std::move(hook);
    }

    tr::GeminiResponse generate(const tr::GeminiRequest& request) override {
        requests_.push_back(request);
        const std::size_t call_idx = requests_.size() - 1u;
        const std::vector<std::string> jps =
            parse_requested_escaped_jps(request.user_prompt, escaped_jp_to_en_);

        if (custom_hook_) {
            const auto custom = custom_hook_(call_idx, request, jps);
            if (custom.has_value()) {
                return *custom;
            }
        }

        for (const std::string& jp : jps) {
            if (blocked_escaped_jps_.count(jp) != 0) {
                tr::GeminiResponse blocked;
                blocked.content_blocked = true;
                blocked.finish_reason = "PROHIBITED_CONTENT";
                blocked.prompt_tokens = kDefaultFakePromptTokens;
                return blocked;
            }
        }

        std::string body;
        for (std::size_t i = 0; i < jps.size(); ++i) {
            const auto it = escaped_jp_to_en_.find(jps[i]);
            const std::string en =
                (it != escaped_jp_to_en_.end()) ? it->second : "\"Fallback line.\"";
            body += std::to_string(i + 1u) + ". " + en + "\n";
        }

        tr::GeminiResponse ok;
        ok.text = std::move(body);
        ok.finish_reason = "STOP";
        ok.content_blocked = false;
        ok.prompt_tokens = kDefaultFakePromptTokens;
        ok.output_tokens = output_tokens_per_call_;
        ok.thought_tokens = kDefaultFakeThoughtTokens;
        return ok;
    }

    std::size_t call_count() const noexcept { return requests_.size(); }
    const std::vector<tr::GeminiRequest>& requests() const noexcept { return requests_; }

private:
    std::unordered_map<std::string, std::string> escaped_jp_to_en_;
    std::unordered_set<std::string> blocked_escaped_jps_;
    std::vector<tr::GeminiRequest> requests_;
    std::int64_t output_tokens_per_call_ = kDefaultFakeOutputTokens;
    std::function<std::optional<tr::GeminiResponse>(
        std::size_t, const tr::GeminiRequest&, const std::vector<std::string>&)>
        custom_hook_;
};

const bj::array& extract_blocked_array(const bj::value& root) {
    if (root.is_array()) {
        return root.as_array();
    }
    const bj::object& obj = root.as_object();
    for (const auto& kv : obj) {
        if (kv.value().is_array()) {
            return kv.value().as_array();
        }
    }
    static const bj::array kEmpty;
    return kEmpty;
}

tr::GeminiTranslateOptions make_live_options(const ScopedTempDir& scratch) {
    tr::GeminiTranslateOptions opt;
    opt.input_file = (scratch.path() / "script_output" / "extracted_text.json").u8string();
    opt.cache_file =
        (scratch.path() / "script_output" / "translation_cache_gemini.json").u8string();
    opt.output_file = (scratch.path() / "script_output" / "translated_text.json").u8string();
    opt.tsv_file = (scratch.path() / "build" / "install" / "game" / "translations.tsv").u8string();
    opt.game_tsv_file = (scratch.path() / "uninstalled_game" / "translations.tsv").u8string();
    opt.blocked_file = (scratch.path() / "script_output" / "blocked_lines.json").u8string();
    opt.raw_log_file = (scratch.path() / "script_output" / "gemini_raw_answers.log").u8string();
    opt.last_batch_file = (scratch.path() / "script_output" / "last_gemini_translate" /
                           "last_translation.json")
                              .u8string();
    opt.max_budget_eur = kGenerousBudgetEur;
    return opt;
}

}  // namespace

TEST(GeminiWireCodec, BuildRequestSerializesPromptsSafetyAndDefaultThinkingBudget) {
    tr::GeminiRequest req;
    req.system_prompt = "Translate visual novel lines accurately.";
    req.user_prompt = "「今、何時だ？」";
    req.safety_block_none = true;

    const bj::value parsed = bj::parse(tr::build_gemini_request_json(req));
    ASSERT_TRUE(parsed.is_object());
    const bj::object& root = parsed.as_object();

    ASSERT_TRUE(root.contains("systemInstruction"));
    const bj::array& sys_parts =
        root.at("systemInstruction").as_object().at("parts").as_array();
    ASSERT_EQ(sys_parts.size(), kSingleElementCount);
    EXPECT_EQ(json_sv(sys_parts[0].as_object().at("text")),
              "Translate visual novel lines accurately.");

    ASSERT_TRUE(root.contains("contents"));
    const bj::array& contents = root.at("contents").as_array();
    ASSERT_EQ(contents.size(), kSingleElementCount);
    const bj::object& user_turn = contents[0].as_object();
    EXPECT_EQ(json_sv(user_turn.at("role")), "user");
    const bj::array& user_parts = user_turn.at("parts").as_array();
    ASSERT_EQ(user_parts.size(), kSingleElementCount);
    EXPECT_EQ(json_sv(user_parts[0].as_object().at("text")), "「今、何時だ？」");

    ASSERT_TRUE(root.contains("safetySettings"));
    const bj::array& safety = root.at("safetySettings").as_array();
    ASSERT_EQ(safety.size(), kExpectedSafetyCategoryCount);
    std::set<std::string> categories;
    for (const bj::value& item : safety) {
        ASSERT_TRUE(item.is_object());
        const bj::object& obj = item.as_object();
        EXPECT_EQ(obj.size(), kSafetySettingFieldCount);
        EXPECT_EQ(json_sv(obj.at("threshold")), "BLOCK_NONE");
        categories.emplace(json_sv(obj.at("category")));
    }
    const std::set<std::string> expected_categories = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
    };
    EXPECT_EQ(categories, expected_categories);

    ASSERT_TRUE(root.contains("generationConfig"));
    const bj::object& gen_cfg = root.at("generationConfig").as_object();
    EXPECT_EQ(json_i64(gen_cfg.at("maxOutputTokens")), tr::kDefaultMaxOutputTokens);
    ASSERT_TRUE(gen_cfg.contains("thinkingConfig"));
    const bj::object& thinking_cfg = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_EQ(json_i64(thinking_cfg.at("thinkingBudget")), tr::kDefaultThinkingBudget);
    EXPECT_FALSE(thinking_cfg.contains("thinkingLevel"));
}

TEST(GeminiWireCodec, BuildRequestHandlesOptionalFieldsAndThinkingModes) {
    {
        tr::GeminiRequest minimal;
        minimal.user_prompt = "「おはよう」";
        minimal.safety_block_none = false;
        minimal.thinking_budget = kDisabledThinkingBudget;

        const bj::object root =
            bj::parse(tr::build_gemini_request_json(minimal)).as_object();
        EXPECT_FALSE(root.contains("systemInstruction"));
        EXPECT_FALSE(root.contains("safetySettings"));
        const bj::object& gen_cfg = root.at("generationConfig").as_object();
        EXPECT_FALSE(gen_cfg.contains("thinkingConfig"));
    }

    {
        tr::GeminiRequest zero_budget;
        zero_budget.user_prompt = "「テスト」";
        zero_budget.thinking_budget = kZeroThinkingBudget;

        const bj::object root =
            bj::parse(tr::build_gemini_request_json(zero_budget)).as_object();
        const bj::object& thinking_cfg =
            root.at("generationConfig").as_object().at("thinkingConfig").as_object();
        EXPECT_EQ(json_i64(thinking_cfg.at("thinkingBudget")), kZeroThinkingBudget);
        EXPECT_FALSE(thinking_cfg.contains("thinkingLevel"));
    }

    {
        tr::GeminiRequest discrete_level;
        discrete_level.user_prompt = "「テスト」";
        discrete_level.thinking_budget = tr::kDefaultThinkingBudget;
        discrete_level.thinking_level = "  HIGH \t\r\n";

        const bj::object root =
            bj::parse(tr::build_gemini_request_json(discrete_level)).as_object();
        const bj::object& thinking_cfg =
            root.at("generationConfig").as_object().at("thinkingConfig").as_object();
        EXPECT_EQ(json_sv(thinking_cfg.at("thinkingLevel")), "HIGH");
        EXPECT_FALSE(thinking_cfg.contains("thinkingBudget"));
    }
}

TEST(GeminiWireCodec, BuildRequestClampsMaxOutputTokensToSupportedBounds) {
    constexpr MaxTokensClampCase kCases[] = {
        {-100, kMinClampedOutputTokens},
        {0, kMinClampedOutputTokens},
        {1, kMinClampedOutputTokens},
        {2, 2},
        {tr::kDefaultMaxOutputTokens, tr::kDefaultMaxOutputTokens},
        {tr::kGeminiMaxOutputTokens - 1, tr::kGeminiMaxOutputTokens - 1},
        {tr::kGeminiMaxOutputTokens, tr::kGeminiMaxOutputTokens},
        {tr::kGeminiMaxOutputTokens + 1, tr::kGeminiMaxOutputTokens},
        {200'000, tr::kGeminiMaxOutputTokens},
    };

    for (const MaxTokensClampCase& tc : kCases) {
        SCOPED_TRACE(tc.requested);
        tr::GeminiRequest req;
        req.user_prompt = "line";
        req.max_output_tokens = tc.requested;

        const bj::object root =
            bj::parse(tr::build_gemini_request_json(req)).as_object();
        const bj::object& gen_cfg = root.at("generationConfig").as_object();
        EXPECT_EQ(json_i64(gen_cfg.at("maxOutputTokens")), tc.expected);
    }
}

TEST(GeminiWireCodec, ParseResponseConcatenatesNonThoughtPartsAndExtractsUsage) {
    constexpr std::string_view kMultiPartJson = R"json({
        "candidates": [{
            "content": {
                "role": "model",
                "parts": [
                    {"thought": true, "text": "Internal reasoning step 1."},
                    {"text": "\"What time is it?\"\n"},
                    {"thought": true, "text": "Internal reasoning step 2."},
                    {"thought": false, "text": "Taichi squinted at the clock."}
                ]
            },
            "finishReason": "STOP"
        }],
        "usageMetadata": {
            "promptTokenCount": 5000000000,
            "cachedContentTokenCount": 3000000000,
            "candidatesTokenCount": 420,
            "thoughtsTokenCount": 1337
        }
    })json";

    const tr::GeminiResponse resp =
        tr::parse_gemini_response_json(kMultiPartJson, kHttpOk);
    EXPECT_EQ(resp.text, "\"What time is it?\"\nTaichi squinted at the clock.");
    EXPECT_EQ(resp.finish_reason, "STOP");
    EXPECT_FALSE(resp.content_blocked);
    EXPECT_EQ(resp.prompt_tokens, kLargePromptTokens);
    EXPECT_EQ(resp.cached_tokens, kLargeCachedTokens);
    EXPECT_EQ(resp.output_tokens, kSampleOutputTokens);
    EXPECT_EQ(resp.thought_tokens, kSampleThoughtTokens);

    constexpr std::string_view kUint64UsageJson = R"json({
        "candidates": [{
            "content": {
                "role": "model",
                "parts": [{"text": "Truncated output"}]
            },
            "finishReason": "MAX_TOKENS"
        }],
        "usageMetadata": {
            "promptTokenCount": 9223372036854775808
        }
    })json";

    const tr::GeminiResponse uint64_resp =
        tr::parse_gemini_response_json(kUint64UsageJson, kHttpOk);
    EXPECT_EQ(uint64_resp.text, "Truncated output");
    EXPECT_EQ(uint64_resp.finish_reason, "MAX_TOKENS");
    EXPECT_FALSE(uint64_resp.content_blocked);
    EXPECT_EQ(uint64_resp.prompt_tokens, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(uint64_resp.cached_tokens, 0);
    EXPECT_EQ(uint64_resp.output_tokens, 0);
    EXPECT_EQ(uint64_resp.thought_tokens, 0);
}

TEST(GeminiWireCodec, ParseResponseClassifiesHttpAndRpcErrors) {
    constexpr ErrorClassificationCase kCases[] = {
        {
            "http_429_json_quota",
            kHttpTooManyRequests,
            R"json({"error":{"code":429,"message":"Quota exceeded for metric.","status":"RESOURCE_EXHAUSTED"}})json",
            ExpectedErrorKind::RateLimit,
            "Quota exceeded for metric.",
        },
        {
            "http_429_plain_text",
            kHttpTooManyRequests,
            "Too Many Requests",
            ExpectedErrorKind::RateLimit,
            {},
        },
        {
            "http_200_resource_exhausted",
            kHttpOk,
            R"json({"error":{"code":429,"message":"Stream quota exhausted.","status":"RESOURCE_EXHAUSTED"}})json",
            ExpectedErrorKind::RateLimit,
            "Stream quota exhausted.",
        },
        {
            "http_400_resource_exhausted",
            kHttpBadRequest,
            R"json({"error":{"code":429,"message":"Per-minute token limit.","status":"RESOURCE_EXHAUSTED"}})json",
            ExpectedErrorKind::RateLimit,
            "Per-minute token limit.",
        },
        {
            "http_500_internal",
            kHttpInternalError,
            R"json({"error":{"code":500,"message":"Internal backend error.","status":"INTERNAL"}})json",
            ExpectedErrorKind::Transient,
            "Internal backend error.",
        },
        {
            "http_502_bad_gateway",
            kHttpBadGateway,
            "<html>502 Bad Gateway</html>",
            ExpectedErrorKind::Transient,
            {},
        },
        {
            "http_503_unavailable",
            kHttpServiceUnavailable,
            R"json({"error":{"code":503,"message":"The model is overloaded.","status":"UNAVAILABLE"}})json",
            ExpectedErrorKind::Transient,
            "The model is overloaded.",
        },
        {
            "http_400_invalid_argument",
            kHttpBadRequest,
            R"json({"error":{"code":400,"message":"API key not valid.","status":"INVALID_ARGUMENT"}})json",
            ExpectedErrorKind::BaseApi,
            "API key not valid.",
        },
        {
            "http_403_permission_denied",
            kHttpForbidden,
            R"json({"error":{"code":403,"message":"Permission denied on model.","status":"PERMISSION_DENIED"}})json",
            ExpectedErrorKind::BaseApi,
            "Permission denied on model.",
        },
    };

    for (const ErrorClassificationCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        expect_error_classification(tc);
    }
}

TEST(GeminiWireCodec, ParseResponseFlagsPromptAndCandidateSafetyBlocksWithoutThrowing) {
    constexpr std::string_view kMissingCandidatesBlocked = R"json({
        "promptFeedback": {
            "blockReason": "PROHIBITED_CONTENT"
        },
        "usageMetadata": {
            "promptTokenCount": 812,
            "cachedContentTokenCount": 512,
            "candidatesTokenCount": 0,
            "thoughtsTokenCount": 0
        }
    })json";

    const tr::GeminiResponse missing_cand =
        tr::parse_gemini_response_json(kMissingCandidatesBlocked, kHttpOk);
    EXPECT_TRUE(missing_cand.content_blocked);
    EXPECT_EQ(missing_cand.text, "");
    EXPECT_EQ(missing_cand.prompt_tokens, kPromptBlockedTokens);
    EXPECT_EQ(missing_cand.cached_tokens, kPromptBlockedCachedTokens);
    EXPECT_EQ(missing_cand.output_tokens, 0);
    EXPECT_EQ(missing_cand.thought_tokens, 0);

    constexpr std::string_view kEmptyCandidatesBlocked = R"json({
        "candidates": [],
        "promptFeedback": {
            "blockReason": "PROHIBITED_CONTENT"
        },
        "usageMetadata": {
            "promptTokenCount": 250
        }
    })json";

    const tr::GeminiResponse empty_cand =
        tr::parse_gemini_response_json(kEmptyCandidatesBlocked, kHttpOk);
    EXPECT_TRUE(empty_cand.content_blocked);
    EXPECT_EQ(empty_cand.text, "");
    EXPECT_EQ(empty_cand.prompt_tokens, kEmptyCandidatesPromptTokens);

    constexpr std::string_view kBlockedFinishReasons[] = {
        "SAFETY",
        "PROHIBITED_CONTENT",
        "BLOCKLIST",
        "RECITATION",
        "SPII",
        "IMAGE_SAFETY",
    };

    for (std::string_view reason : kBlockedFinishReasons) {
        SCOPED_TRACE(reason);
        const std::string payload =
            std::string(R"json({"candidates":[{"finishReason":")json") +
            std::string(reason) +
            R"json("}],"usageMetadata":{"promptTokenCount":140,"thoughtsTokenCount":15}})json";

        const tr::GeminiResponse blocked =
            tr::parse_gemini_response_json(payload, kHttpOk);
        EXPECT_TRUE(blocked.content_blocked);
        EXPECT_EQ(blocked.finish_reason, reason);
        EXPECT_EQ(blocked.text, "");
        EXPECT_EQ(blocked.prompt_tokens, kCandidateBlockedPromptTokens);
        EXPECT_EQ(blocked.thought_tokens, kCandidateBlockedThoughtTokens);
    }
}

TEST(GeminiWireCodec, ParseResponseRejectsMalformedOrEmptyUnblockedPayloads) {
    constexpr ErrorClassificationCase kInvalidPayloads[] = {
        {"empty_body", kHttpOk, "", ExpectedErrorKind::BaseApi, {}},
        {"whitespace_body", kHttpOk, "  \r\n\t ", ExpectedErrorKind::BaseApi, {}},
        {"malformed_json", kHttpOk, "{\"candidates\": [", ExpectedErrorKind::BaseApi, {}},
        {"non_object_root", kHttpOk, "[]", ExpectedErrorKind::BaseApi, {}},
        {
            "missing_candidates_without_block_reason",
            kHttpOk,
            R"json({"usageMetadata":{"promptTokenCount":10}})json",
            ExpectedErrorKind::BaseApi,
            {},
        },
        {
            "empty_candidates_without_block_reason",
            kHttpOk,
            R"json({"candidates":[],"promptFeedback":{}})json",
            ExpectedErrorKind::BaseApi,
            {},
        },
        {
            "empty_parts_on_stop",
            kHttpOk,
            R"json({"candidates":[{"content":{"role":"model","parts":[]},"finishReason":"STOP"}]})json",
            ExpectedErrorKind::BaseApi,
            {},
        },
        {
            "thought_only_parts_on_stop",
            kHttpOk,
            R"json({"candidates":[{"content":{"role":"model","parts":[{"thought":true,"text":"reasoning only"}]},"finishReason":"STOP"}]})json",
            ExpectedErrorKind::BaseApi,
            {},
        },
    };

    for (const ErrorClassificationCase& tc : kInvalidPayloads) {
        SCOPED_TRACE(tc.label);
        expect_error_classification(tc);
    }
}

TEST(GeminiWireCodec, DotenvParserHandlesQuotesCommentsBomAndLastAssignmentWins) {
    ScopedGeminiEnvGuard env_guard;

    constexpr DotenvCase kCases[] = {
        {
            "plain_key_value",
            "GEMINI_API_KEY=plain\n",
            "GEMINI_API_KEY",
            "plain",
        },
        {
            "export_escaped_double_quotes",
            R"env(export GEMINI_API_KEY="double\"quoted")env",
            "GEMINI_API_KEY",
            "double\"quoted",
        },
        {
            "single_quoted_preserves_hash",
            "GEMINI_API_KEY='raw#hash'\r\n",
            "GEMINI_API_KEY",
            "raw#hash",
        },
        {
            "inline_comment_on_unquoted_value",
            "GEMINI_API_KEY=unquoted_secret # inline comment\n",
            "GEMINI_API_KEY",
            "unquoted_secret",
        },
        {
            "utf8_bom_on_first_line",
            "\xEF\xBB\xBF" "GEMINI_API_KEY=bom_value\r\n",
            "GEMINI_API_KEY",
            "bom_value",
        },
        {
            "comments_blanks_and_last_assignment_wins",
            "# Header comment\r\n"
            "   # Indented comment\r\n"
            "\r\n"
            "OTHER_KEY=ignored\r\n"
            "GEMINI_API_KEY=first_stale_value\r\n"
            "NOT_AN_ASSIGNMENT\r\n"
            "export GEMINI_API_KEY = 'second#winner' \r\n",
            "GEMINI_API_KEY",
            "second#winner",
        },
        {
            "non_matching_superstring_keys",
            "OTHER_GEMINI_API_KEY=wrong1\n"
            "GEMINI_API_KEY_EXTRA=wrong2\n",
            "GEMINI_API_KEY",
            "",
        },
        {
            "empty_contents",
            "",
            "GEMINI_API_KEY",
            "",
        },
    };

    for (const DotenvCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(tr::parse_dotenv_value(tc.contents, tc.key), tc.expected);
    }
    EXPECT_FALSE(read_process_env(kGeminiEnvVarWide).has_value());
}

TEST(GeminiWireCodec, ResolveApiKeyReadsDotenvOverrideAndHandlesMissingFileCleanly) {
    ScopedGeminiEnvGuard env_guard;
    ScopedTempDir scratch;

    const fs::path missing_dotenv = scratch.path() / "absent_subdir" / ".env";
    EXPECT_EQ(tr::GeminiClient::resolve_gemini_api_key(missing_dotenv), "");
    EXPECT_FALSE(read_process_env(kGeminiEnvVarWide).has_value());

    const fs::path dotenv_path = scratch.path() / ".env";
    constexpr std::string_view kDotenvPayload =
        "\xEF\xBB\xBF"
        "# Test dotenv file\r\n"
        "\r\n"
        "GEMINI_API_KEY=overwritten_key # inline comment\r\n"
        "export GEMINI_API_KEY=\"resolved\\\"key\"\r\n";
    write_binary_file(dotenv_path, kDotenvPayload);

    EXPECT_EQ(tr::GeminiClient::resolve_gemini_api_key(dotenv_path), "resolved\"key");
    EXPECT_FALSE(read_process_env(kGeminiEnvVarWide).has_value());

    ASSERT_EQ(::_wputenv_s(kGeminiEnvVarWide, L"env_priority_key"), 0);
    EXPECT_EQ(tr::GeminiClient::resolve_gemini_api_key(), "env_priority_key");
}

TEST(GlossaryAndSpeakerGate, GlossaryHasSeventyFiveUniqueValidCp932PairsAndStoryOrder) {
    const auto& ordered = tr::name_translations_ordered();
    const auto& lookup = tr::name_translations();
    ASSERT_EQ(ordered.size(), kExpectedGlossarySize);
    ASSERT_EQ(lookup.size(), kExpectedGlossarySize);

    EXPECT_EQ(ordered.front().first, "太一");
    EXPECT_EQ(ordered.front().second, "Taichi");
    EXPECT_EQ(lookup.at("桜庭・友貴"), "Sakuraba & Tomoki");
    EXPECT_EQ(lookup.at("太一（妄想）"), "Taichi (Delusion)");
    EXPECT_EQ(lookup.at("＊＊"), "**");

    std::set<std::string> unique_jp;
    for (const auto& [jp, en] : ordered) {
        SCOPED_TRACE(jp);
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(en.empty());
        EXPECT_TRUE(unique_jp.insert(jp).second);
        EXPECT_TRUE(crc::utf8_to_cp932_strict(jp).has_value());
        EXPECT_EQ(jp.find("「"), std::string::npos);
        EXPECT_TRUE(is_printable_ascii_only(en));
        ASSERT_NE(lookup.find(jp), lookup.end());
        EXPECT_EQ(lookup.at(jp), en);
    }
    EXPECT_EQ(unique_jp.size(), kExpectedGlossarySize);

    const auto& order = tr::story_order();
    ASSERT_EQ(order.size(), kSingleElementCount);
    EXPECT_EQ(order[0], "sn.bin");
    const std::set<std::string> unique_files(order.begin(), order.end());
    EXPECT_EQ(unique_files.size(), order.size());
}

TEST(GlossaryAndSpeakerGate, SystemAndNeutralPromptsPreserveGlossaryAndLineBreakRules) {
    const std::string& primary = tr::system_prompt();
    const std::string& neutral = tr::neutral_system_prompt();

    ASSERT_FALSE(primary.empty());
    ASSERT_FALSE(neutral.empty());
    EXPECT_NE(primary, neutral);

    EXPECT_NE(primary.find("%N"), std::string::npos);
    EXPECT_NE(neutral.find("%N"), std::string::npos);

    for (std::string_view pinned_name :
         {"Taichi", "Misato", "Miki", "Sakuraba & Tomoki", "Taichi (Delusion)"}) {
        SCOPED_TRACE(pinned_name);
        EXPECT_NE(primary.find(pinned_name), std::string::npos);
        EXPECT_NE(neutral.find(pinned_name), std::string::npos);
    }
}

TEST(GlossaryAndSpeakerGate, SpeakerGatePassesValidCorpusAndDetectsInjectedFailures) {
    const bj::object valid = make_corpus_with_all_speakers({
        make_entry(0, "narration", "", "時計の針が午前零時を指す。"),
        make_entry(0, "dialogue", "太一", "「今、何時だ？」"),
    });
    const tr::SpeakerCheckReport pass_report = tr::run_speaker_checks(valid);
    EXPECT_TRUE(pass_report.passed) << pass_report.reason;
    EXPECT_EQ(pass_report.failed_check, 0);
    EXPECT_EQ(pass_report.reason, "");
    EXPECT_EQ(pass_report.speakers_in_data, kExpectedGlossarySize);
    EXPECT_EQ(pass_report.glossary_entries, kExpectedGlossarySize);
    EXPECT_EQ(pass_report.dialogue_total, kExpectedGlossarySize + kSingleElementCount);
    EXPECT_GT(pass_report.dialogue_sampled, 0u);
    EXPECT_LE(pass_report.dialogue_sampled, pass_report.dialogue_total);

    auto expect_gate_failure = [](std::string_view label, const bj::object& broken) {
        SCOPED_TRACE(label);
        const tr::SpeakerCheckReport r = tr::run_speaker_checks(broken);
        EXPECT_FALSE(r.passed);
        EXPECT_GE(r.failed_check, kMinSpeakerCheckId);
        EXPECT_LE(r.failed_check, kMaxSpeakerCheckId);
        EXPECT_FALSE(r.reason.empty());
    };

    expect_gate_failure("empty_document", bj::object{});

    bj::object narration_only;
    narration_only["sn.bin"] = bj::object{
        {"strings",
         bj::array{make_entry(static_cast<std::int64_t>(crc::extract::GAME_START_OFFSET),
                              "narration", "", "誰もいない教室。")}}};
    expect_gate_failure("zero_dialogue_entries", narration_only);

    std::vector<bj::object> five_missing_speakers;
    for (std::size_t i = 0; i < kFiveElements; ++i) {
        five_missing_speakers.push_back(make_entry(0, "dialogue", "", "「誰の声だ？」"));
    }
    {
        const tr::SpeakerCheckReport r1 =
            tr::run_speaker_checks(make_corpus_with_prepended_rows(five_missing_speakers));
        EXPECT_FALSE(r1.passed);
        EXPECT_EQ(r1.failed_check, 1);
        EXPECT_FALSE(r1.reason.empty());
    }

    {
        const tr::SpeakerCheckReport r3 = tr::run_speaker_checks(
            make_corpus_with_prepended_rows({make_entry(0, "dialogue", "", "「誰の声だ？」")}));
        EXPECT_FALSE(r3.passed);
        EXPECT_EQ(r3.failed_check, 3);
        EXPECT_FALSE(r3.reason.empty());
    }

    expect_gate_failure(
        "speaker_holding_dialogue_quote",
        make_corpus_with_all_speakers({make_entry(0, "dialogue", "「太一」", "「おい」")}));

    expect_gate_failure(
        "unknown_unmapped_speaker",
        make_corpus_with_all_speakers({make_entry(0, "dialogue", "謎の通行人", "「よお」")}));
}

TEST(TranslateCoreHelpers, JapaneseDetectionAndTranslatableIndicesFilterCorrectly) {
    EXPECT_TRUE(tr::has_real_japanese("こんにちは"));
    EXPECT_TRUE(tr::has_real_japanese("カタカナ"));
    EXPECT_TRUE(tr::has_real_japanese("漢字"));
    EXPECT_TRUE(tr::needs_translation("Eスbg152n"));

    EXPECT_FALSE(tr::has_real_japanese("……"));
    EXPECT_FALSE(tr::has_real_japanese("「」！？"));
    EXPECT_FALSE(tr::needs_translation("Hello"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("……"));
    EXPECT_FALSE(tr::needs_translation("ABC123"));

    bj::object fdata;
    fdata["strings"] = bj::array{
        make_entry(100, "narration", "", "……"),
        make_entry(200, "dialogue", "太一", "「おはよう」"),
        make_entry(300, "narration", "", "CASE 01"),
        make_entry(400, "narration", "", "風が吹いた。"),
    };
    const std::vector<std::size_t> idx = tr::translatable_indices(fdata);
    ASSERT_EQ(idx.size(), kTwoElements);
    EXPECT_EQ(idx[0], 1u);
    EXPECT_EQ(idx[1], 3u);
}

TEST(TranslateCoreHelpers, ExtractSpeakerResolvesFieldLegacyPrefixAndLengthSeam) {
    EXPECT_EQ(tr::extract_speaker(make_entry(1, "dialogue", "太一", "「…」")), "Taichi");
    EXPECT_EQ(tr::extract_speaker(make_entry(2, "dialogue", "", "太一\r\n「…」")), "Taichi");
    EXPECT_EQ(tr::extract_speaker(make_entry(3, "dialogue", "未知", "x")), "未知");
    EXPECT_EQ(tr::extract_speaker(make_entry(4, "narration", "", "no crlf here")), "NARRATION");

    std::string boundary_valid;
    for (int i = 0; i < kMaxSpeakerCodepoints; ++i) {
        boundary_valid += "あ";
    }
    EXPECT_EQ(tr::extract_speaker(make_entry(5, "dialogue", boundary_valid, "x")),
              boundary_valid);

    std::string boundary_too_long = boundary_valid + "あ";
    ASSERT_EQ(kTooLongSpeakerCodepoints, kMaxSpeakerCodepoints + 1);
    EXPECT_EQ(tr::extract_speaker(make_entry(6, "dialogue", boundary_too_long, "x")),
              "NARRATION");
}

TEST(TranslateCoreHelpers, SanitizeAsciiPostprocessAndCleanTranslatedLine) {
    constexpr TextTransformCase kSanitizeCases[] = {
        {"em_and_en_dashes", "a—b–c", "a--b--c"},
        {"curly_single_and_double_quotes", "‘x’ and “y”", "'x' and \"y\""},
        {"ellipsis_and_ideographic_space", "Wait…　now", "Wait... now"},
        {"preserves_percent_n_and_crlf", "Line 1%NLine 2\r\n", "Line 1%NLine 2\r\n"},
    };
    for (const TextTransformCase& tc : kSanitizeCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(tr::sanitize_ascii(std::string(tc.input)), tc.expected);
    }

    constexpr LinePairTransformCase kPostprocessCases[] = {
        {"reattaches_legacy_speaker_plate", "太一\r\n「ふん」", "Hmph.", "Taichi\r\nHmph."},
        {"deduplicates_jp_speaker_plate", "太一\r\n「ふん」", "太一\r\nHmph.", "Taichi\r\nHmph."},
        {"deduplicates_en_speaker_plate", "太一\r\n「ふん」", "Taichi\r\nHmph.", "Taichi\r\nHmph."},
        {"preserves_percent_n", "「一%N二」", "“One%NTwo”", "\"One%NTwo\""},
        {"restores_trailing_crlf", "日本語\r\n", "Japanese  ", "Japanese\r\n"},
        {"identity_passthrough", "same", "same", "same"},
        {"empty_translation_stays_empty", "x", "", ""},
    };
    for (const LinePairTransformCase& tc : kPostprocessCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(tr::postprocess(std::string(tc.jp), std::string(tc.raw_en)), tc.expected);
    }

    constexpr LinePairTransformCase kCleanCases[] = {
        {
            "strips_echoed_speaker_tag_and_unescapes_crlf",
            "「おはよう」",
            "[Taichi] \"Good morning.\\r\\nWake up.\"",
            "\"Good morning.\r\nWake up.\"",
        },
        {
            "preserves_percent_n_and_normalizes_extended_punctuation",
            "「カフェ〜%N行くぞ」",
            "「Café〜%NLet’s―go⋯」",
            "\"Cafe~%NLet's--go...\"",
        },
        {
            "collapses_ruby_markup_to_reading",
            "先輩を呼ぶ。",
            "Calling [senpai|upperclassman].",
            "Calling upperclassman.",
        },
    };
    for (const LinePairTransformCase& tc : kCleanCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(tr::clean_translated_line(std::string(tc.jp), std::string(tc.raw_en)),
                  tc.expected);
    }
}

TEST(TranslateCoreHelpers, PromptEscapingTokenClampingAndUserPromptFraming) {
    EXPECT_EQ(tr::escape_prompt_line("一行目\r\n二行目\n三行目%N四"),
              "一行目\\r\\n二行目\\n三行目%N四");

    EXPECT_EQ(tr::compute_max_output_tokens(0), tr::kDefaultMaxOutputTokens);
    EXPECT_EQ(tr::compute_max_output_tokens(kSmallClampBatchSize),
              tr::kDefaultMaxOutputTokens);
    EXPECT_EQ(tr::compute_max_output_tokens(kFirstUnclampedBatchSize),
              static_cast<std::int64_t>(kFirstUnclampedBatchSize) * kTokensPerBatchLine);
    EXPECT_EQ(tr::compute_max_output_tokens(tr::kDefaultBatchSize),
              static_cast<std::int64_t>(tr::kDefaultBatchSize) * kTokensPerBatchLine);
    EXPECT_EQ(tr::compute_max_output_tokens(kMaxUnclampedBatchSize),
              static_cast<std::int64_t>(kMaxUnclampedBatchSize) * kTokensPerBatchLine);
    EXPECT_EQ(tr::compute_max_output_tokens(kClampedHighBatchSize),
              tr::kGeminiMaxOutputTokens);
    EXPECT_EQ(tr::compute_max_output_tokens(kHugeBatchSize),
              tr::kGeminiMaxOutputTokens);

    const std::vector<std::pair<std::string, std::string>> prev_ctx = {
        {"Taichi", "\"Dropped old context.\""},
        {"Miki", "\"Kept recent one.\""},
        {"NARRATION", "The bell rang."},
    };
    const std::vector<tr::PromptLine> batch = {
        {"Taichi", "「行こう%N屋上へ」", 1000},
        {"NARRATION", "階段を上る。\r\n", 1064},
    };
    const std::vector<tr::PromptLine> after_ctx = {
        {"Misato", "「あら、太一くん」", 1128},
        {"Sakuraba", "「カレーパンだ」", 1192},
    };

    const std::string prompt = tr::build_batch_user_prompt(
        batch, prev_ctx, after_ctx, /*context_window=*/2, /*context_after_window=*/1);
    EXPECT_NE(prompt.find("<previous_context>"), std::string::npos);
    EXPECT_EQ(prompt.find("Dropped old context."), std::string::npos);
    EXPECT_NE(prompt.find("Kept recent one."), std::string::npos);
    EXPECT_NE(prompt.find("The bell rang."), std::string::npos);
    EXPECT_NE(prompt.find("<lines_to_translate>"), std::string::npos);
    EXPECT_NE(prompt.find("1."), std::string::npos);
    EXPECT_NE(prompt.find("「行こう%N屋上へ」"), std::string::npos);
    EXPECT_NE(prompt.find("階段を上る。\\r\\n"), std::string::npos);
    EXPECT_NE(prompt.find("<context_after>"), std::string::npos);
    EXPECT_NE(prompt.find("「あら、太一くん」"), std::string::npos);
    EXPECT_EQ(prompt.find("「カレーパンだ」"), std::string::npos);

    const std::string no_ctx_prompt =
        tr::build_batch_user_prompt(batch, {}, {}, /*context_window=*/0,
                                    /*context_after_window=*/0);
    EXPECT_EQ(no_ctx_prompt.find("<previous_context>"), std::string::npos);
    EXPECT_EQ(no_ctx_prompt.find("<context_after>"), std::string::npos);
    EXPECT_NE(no_ctx_prompt.find("<lines_to_translate>"), std::string::npos);
}

TEST(TranslateCoreHelpers, StrictNumberedParserAndUsableExtractionEnforceAlignmentAndMaxTokensSalvage) {
    const auto legacy =
        tr::parse_numbered_response("1. [Taichi] Hello\\r\\nWorld\n\n[NARRATION] 2) Foo\n");
    ASSERT_EQ(legacy.size(), kTwoElements);
    EXPECT_EQ(legacy.at(2), "Foo");

    const tr::ParsedNumberedAnswer strict_ok = tr::parse_numbered_response_strict(
        "**1.** [Taichi] \"First%Nline\"\\r\\ncontinued\n"
        "wrapped tail\n"
        "２． [NARRATION] Second line\\nhere\n",
        /*expected=*/2);
    EXPECT_EQ(strict_ok.rejected, 0);
    ASSERT_EQ(strict_ok.entries.size(), kTwoElements);
    EXPECT_EQ(strict_ok.entries.at(1), "\"First%Nline\"\r\ncontinued wrapped tail");
    EXPECT_EQ(strict_ok.entries.at(2), "Second line\nhere");

    constexpr std::string_view kStrictRejectCases[] = {
        "1. First\n1. Duplicate index\n",
        "2. Skipped one\n3. Next\n",
        "1. First\n3. Out of range for expected=2\n",
        "0. Zero index\n1. First\n",
        "Sure! Here are your translations:\n1. First\n2. Second\n",
    };
    for (std::string_view bad : kStrictRejectCases) {
        SCOPED_TRACE(bad);
        const tr::ParsedNumberedAnswer parsed =
            tr::parse_numbered_response_strict(std::string(bad), /*expected=*/2);
        EXPECT_GT(parsed.rejected, 0);
        EXPECT_TRUE(parsed.entries.empty());
    }

    tr::GeminiResponse stop_exact;
    stop_exact.finish_reason = "STOP";
    stop_exact.text = "1. \"One.\"\n2. \"Two.\"\n";
    const auto exact_map = tr::extract_usable_translations(stop_exact, kTwoElements);
    ASSERT_EQ(exact_map.size(), kTwoElements);
    EXPECT_EQ(exact_map.at(1), "\"One.\"");
    EXPECT_EQ(exact_map.at(2), "\"Two.\"");

    // Missing line 3 on STOP must reject the whole batch so indices never shift.
    std::string warning;
    const auto stop_short =
        tr::extract_usable_translations(stop_exact, kThreeElements, &warning);
    EXPECT_TRUE(stop_short.empty());
    EXPECT_FALSE(warning.empty());

    // Blocked responses yield an empty map.
    tr::GeminiResponse blocked_resp = stop_exact;
    blocked_resp.content_blocked = true;
    EXPECT_TRUE(tr::extract_usable_translations(blocked_resp, kTwoElements).empty());

    // MAX_TOKENS salvages clean prefix 1..2 and drops truncated final entry 3.
    tr::GeminiResponse truncated_resp;
    truncated_resp.finish_reason = "MAX_TOKENS";
    truncated_resp.text =
        "1. \"Clean first.\"\n2. \"Clean second.\"\n3. \"Truncated fragment";
    warning.clear();
    const auto salvaged =
        tr::extract_usable_translations(truncated_resp, kFourElements, &warning);
    ASSERT_EQ(salvaged.size(), kTwoElements);
    EXPECT_EQ(salvaged.at(1), "\"Clean first.\"");
    EXPECT_EQ(salvaged.at(2), "\"Clean second.\"");
    EXPECT_EQ(salvaged.count(3), 0u);

    // MAX_TOKENS with only 1 partial entry drops entry 1 cleanly -> empty map.
    tr::GeminiResponse single_truncated;
    single_truncated.finish_reason = "MAX_TOKENS";
    single_truncated.text = "1. \"Half a line";
    EXPECT_TRUE(tr::extract_usable_translations(single_truncated, kTwoElements).empty());
}

TEST(TranslateCoreHelpers, ValidateLineTranslationRejectsEmptyEchoJapaneseLeakAndPercentNMismatch) {
    constexpr ValidationCase kCases[] = {
        {"valid_plain_dialogue", "「おはよう」", "\"Good morning.\"", true},
        {"valid_percent_n_preserved", "「一%N二%N三」", "\"One%NTwo%NThree\"", true},
        {"reject_empty_output", "「おはよう」", "", false},
        {"reject_unchanged_jp_echo", "「おはよう」", "「おはよう」", false},
        {"reject_hiragana_leak", "「おはよう」", "\"Good おはよう.\"", false},
        {"reject_kanji_leak", "「太一、行こう」", "\"Let's go, 太一.\"", false},
        {"reject_dropped_percent_n", "「一%N二」", "\"One Two\"", false},
        {"reject_extra_percent_n", "「一%N二」", "\"One%NTwo%NThree\"", false},
    };
    for (const ValidationCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        std::string reason;
        const bool ok = tr::validate_line_translation(std::string(tc.jp),
                                                      std::string(tc.en), &reason);
        EXPECT_EQ(ok, tc.expected_valid) << reason;
        if (!tc.expected_valid) {
            EXPECT_FALSE(reason.empty());
        }
    }
}

TEST(TranslateCoreHelpers, CacheOrderingPurgeDiscardGuardOptionsAndCostEstimation) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("朝が来た。", "Morning has come.");
    ASSERT_EQ(cache.size(), kFourElements);
    EXPECT_EQ(cache.items()[0].first, "朝が来た。");
    EXPECT_EQ(cache.items()[0].second, "Morning has come.");

    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));
    EXPECT_FALSE(tr::is_failed_entry("朝が来た。", "Morning has come."));

    EXPECT_EQ(tr::purge_failed_entries(cache), kTwoElements);
    ASSERT_EQ(cache.size(), kTwoElements);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));

    EXPECT_FALSE(tr::refuse_cache_discard(0u, "--retranslate", false).has_value());
    EXPECT_FALSE(
        tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--retranslate", false)
            .has_value());
    const auto why = tr::refuse_cache_discard(
        tr::CACHE_DISCARD_THRESHOLD + 1u, "--retranslate", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("--retranslate"), std::string::npos);
    EXPECT_NE(why->find("--discard-cache"), std::string::npos);
    EXPECT_FALSE(
        tr::refuse_cache_discard(kLargePaidCacheCount, "--retranslate", true).has_value());

    ScopedTempDir scratch;
    tr::GeminiTranslateOptions opt = make_live_options(scratch);
    EXPECT_FALSE(tr::validate_gemini_options(opt).has_value());
    tr::GeminiTranslateOptions bad_batch = opt;
    bad_batch.batch_size = 0;
    EXPECT_TRUE(tr::validate_gemini_options(bad_batch).has_value());
    tr::GeminiTranslateOptions bad_ctx = opt;
    bad_ctx.context_window = -1;
    EXPECT_TRUE(tr::validate_gemini_options(bad_ctx).has_value());

    const tr::TokenUsage zero_usage{};
    EXPECT_DOUBLE_EQ(tr::estimate_cost_usd(zero_usage, "gemini-4-argon"), 0.0);
    EXPECT_DOUBLE_EQ(tr::estimate_cost_eur(zero_usage, "gemini-4-argon"), 0.0);

    tr::TokenUsage uncached_sample;
    uncached_sample.api_calls = 1;
    uncached_sample.prompt_tokens = kOneMillionTokens;
    uncached_sample.cached_tokens = 0;
    uncached_sample.output_tokens = kHalfMillionTokens;
    uncached_sample.thought_tokens = kHalfMillionTokens;
    const double argon_usd = tr::estimate_cost_usd(uncached_sample, "gemini-4-argon");
    const double argon_eur = tr::estimate_cost_eur(uncached_sample, "gemini-4-argon");
    EXPECT_NEAR(argon_usd, kExpectedArgonUncachedPlusOutputUsd, kCostEpsilon);
    EXPECT_NEAR(argon_eur, argon_usd * kEurUsdMultiplier, kCostEpsilon);
    EXPECT_NEAR(tr::estimate_cost_usd(uncached_sample, "gemini-3.1-pro-preview"),
                kExpectedPro31UncachedPlusOutputUsd, kCostEpsilon);
}

TEST(OfflineTranslateAll, SeedsGlossaryAppliesPostprocessWritesTranslatedJsonAndBuildsTsv) {
    ScopedTempDir scratch;
    const fs::path input_path = scratch.path() / "script_output" / "extracted_text.json";
    const fs::path cache_path =
        scratch.path() / "script_output" / "translation_cache_gemini.json";
    const fs::path output_path = scratch.path() / "script_output" / "translated_text.json";
    const fs::path tsv_path = scratch.path() / "build" / "install" / "game" / "translations.tsv";
    fs::create_directories(input_path.parent_path());

    const bj::object extracted = make_corpus_with_all_speakers({
        make_entry(0, "dialogue", "太一", "「屋上へ%N行こう」"),
        make_entry(0, "narration", "", "チャイムが鳴った。\r\n"),
        make_entry(0, "dialogue", "", "太一"),
        make_entry(0, "narration", "", "……"),
    });
    crc::write_file_text(input_path.u8string(),
                         crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

    tr::Cache cache;
    cache.set(kSharedCoverageJp, kSharedCoverageEn);
    cache.set("「屋上へ%N行こう」", "“Let’s head%Nto the roof.”");
    cache.set("チャイムが鳴った。\r\n", "The chime rang.  ");
    tr::save_cache(cache, cache_path.u8string());

    tr::TranslateOptions opt;
    opt.input_file = input_path.u8string();
    opt.cache_file = cache_path.u8string();
    opt.output_file = output_path.u8string();
    opt.tsv_file = tsv_path.u8string();
    opt.game_tsv_file = (scratch.path() / "absent_game" / "translations.tsv").u8string();

    ASSERT_EQ(tr::translate_all(opt), tr::kExitOk);
    ASSERT_TRUE(fs::exists(output_path));
    ASSERT_TRUE(fs::exists(tsv_path));

    const bj::array out_strings = crc::json_parse_file(output_path.u8string())
                                      .as_object()
                                      .at("sn.bin")
                                      .as_object()
                                      .at("strings")
                                      .as_array();
    ASSERT_EQ(out_strings.size(), kExpectedGlossarySize + kFourElements);
    EXPECT_EQ(json_sv(out_strings[kExpectedGlossarySize].as_object().at("translated")),
              "\"Let's head%Nto the roof.\"");
    EXPECT_EQ(json_sv(out_strings[kExpectedGlossarySize + 1u].as_object().at("translated")),
              "The chime rang.\r\n");
    EXPECT_EQ(json_sv(out_strings[kExpectedGlossarySize + 2u].as_object().at("translated")),
              "Taichi");
    EXPECT_EQ(json_sv(out_strings[kExpectedGlossarySize + 3u].as_object().at("translated")),
              "……");

    const std::string tsv = read_file_string(tsv_path);
    EXPECT_EQ(tsv.find('\r'), std::string::npos);
    EXPECT_NE(tsv.find("「屋上へ%N行こう」\t\"Let's head%Nto the roof.\"\n"),
              std::string::npos);
    EXPECT_NE(tsv.find("チャイムが鳴った。\\r\\n\tThe chime rang.\\r\\n\n"),
              std::string::npos);
}

TEST(OfflineTranslateAll, RefusesPaidCacheDiscardOnTestBatchesAndThrowsOnMissingCacheEntry) {
    ScopedTempDir scratch;
    const fs::path input_path = scratch.path() / "script_output" / "extracted_text.json";
    const fs::path cache_path =
        scratch.path() / "script_output" / "translation_cache_gemini.json";
    const fs::path output_path = scratch.path() / "script_output" / "translated_text.json";
    const fs::path tsv_path = scratch.path() / "build" / "install" / "game" / "translations.tsv";
    fs::create_directories(input_path.parent_path());

    const bj::object extracted = make_corpus_with_all_speakers({
        make_entry(0, "dialogue", "太一", "「未翻訳の行」"),
    });
    crc::write_file_text(input_path.u8string(),
                         crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

    // Missing cache entry throws offline without attempting any network access.
    tr::Cache incomplete;
    seed_glossary_and_coverage_line(incomplete);
    tr::save_cache(incomplete, cache_path.u8string());

    tr::TranslateOptions missing_opt;
    missing_opt.input_file = input_path.u8string();
    missing_opt.cache_file = cache_path.u8string();
    missing_opt.output_file = output_path.u8string();
    missing_opt.tsv_file = tsv_path.u8string();
    missing_opt.game_tsv_file = (scratch.path() / "absent_game" / "translations.tsv").u8string();
    EXPECT_THROW((void)tr::translate_all(missing_opt), std::runtime_error);

    // >100-entry cache refuses test_batches discard and stays byte-for-byte intact.
    tr::Cache paid_cache;
    seed_glossary_and_coverage_line(paid_cache);
    paid_cache.set("「未翻訳の行」", "\"Translated line.\"");
    for (std::size_t i = paid_cache.size(); i < kPaidCacheOverThreshold; ++i) {
        paid_cache.set("有料行" + std::to_string(i), "Paid line " + std::to_string(i));
    }
    tr::save_cache(paid_cache, cache_path.u8string());
    const std::string paid_before = read_file_string(cache_path);

    tr::TranslateOptions refuse_opt = missing_opt;
    refuse_opt.test_batches = 1;
    refuse_opt.test_dir = (scratch.path() / "test_out").u8string();
    EXPECT_EQ(tr::translate_all(refuse_opt), tr::kExitGateOrRefused);
    EXPECT_EQ(read_file_string(cache_path), paid_before);
}

TEST(BatchTranslatorLive, GateOnlyAndPreFlightGuardsMakeZeroTransportCalls) {
    ScopedTempDir scratch;
    tr::GeminiTranslateOptions opt = make_live_options(scratch);
    fs::create_directories(fs::u8path(opt.input_file).parent_path());

    const bj::object extracted = make_corpus_with_all_speakers({
        make_entry(0, "dialogue", "太一", "「今、何時だ？」"),
        make_entry(0, "narration", "", "時計を見る。"),
        make_entry(0, "dialogue", "美希", "「急ごう」"),
        make_entry(0, "narration", "", "……"),
    });
    crc::write_file_text(opt.input_file,
                         crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

    tr::Cache seed;
    seed_glossary_and_coverage_line(seed);
    tr::save_cache(seed, opt.cache_file);

    FakeTranslationTransport transport;
    tr::BatchTranslator translator(&transport);

    tr::GeminiTranslateOptions gate_opt = opt;
    gate_opt.gate_only = true;
    const tr::TranslateReport gate_report = translator.run(gate_opt);
    EXPECT_EQ(transport.call_count(), 0u);
    EXPECT_EQ(gate_report.exit_code, tr::kExitOk);
    EXPECT_EQ(gate_report.total_translatable, kExpectedGlossarySize + kThreeElements);
    EXPECT_EQ(gate_report.unique_translatable, kFourElements);
    EXPECT_EQ(gate_report.translated_new, 0u);
    EXPECT_EQ(gate_report.remaining_uncached, kThreeElements);

    // Paid cache (>100 entries) refuses --retranslate when discard_cache == false.
    tr::Cache paid = seed;
    for (std::size_t i = paid.size(); i < kPaidCacheOverThreshold; ++i) {
        paid.set("既存行" + std::to_string(i), "Existing line " + std::to_string(i));
    }
    tr::save_cache(paid, opt.cache_file);
    const std::string paid_before = read_file_string(fs::u8path(opt.cache_file));

    tr::GeminiTranslateOptions retranslate_refused = opt;
    retranslate_refused.retranslate = true;
    retranslate_refused.discard_cache = false;
    const tr::TranslateReport refused_report = translator.run(retranslate_refused);
    EXPECT_EQ(transport.call_count(), 0u);
    EXPECT_EQ(refused_report.exit_code, tr::kExitGateOrRefused);
    EXPECT_EQ(read_file_string(fs::u8path(opt.cache_file)), paid_before);

    // Speaker gate failure stops before spending a single API call.
    const bj::object broken = make_corpus_with_all_speakers({
        make_entry(0, "dialogue", "未登録キャラ", "「誰だ？」"),
    });
    const fs::path broken_path = scratch.path() / "script_output" / "broken_extracted.json";
    crc::write_file_text(broken_path.u8string(),
                         crc::json_pretty(bj::value(broken), kExtractedJsonIndent));
    tr::GeminiTranslateOptions broken_opt = opt;
    broken_opt.input_file = broken_path.u8string();
    const tr::TranslateReport broken_report = translator.run(broken_opt);
    EXPECT_EQ(transport.call_count(), 0u);
    EXPECT_EQ(broken_report.exit_code, tr::kExitGateOrRefused);
}

TEST(BatchTranslatorLive, PropagatesSlidingPreviousContextAndLookaheadAcrossBatchesAndSalvagesMaxTokens) {
    ScopedTempDir scratch;
    tr::GeminiTranslateOptions opt = make_live_options(scratch);
    opt.batch_size = 2;
    opt.context_window = 2;
    opt.context_after_window = 2;
    fs::create_directories(fs::u8path(opt.input_file).parent_path());

    const bj::object extracted = make_corpus_with_all_speakers({
        make_entry(0, "dialogue", "太一", "「一発目の台詞だ」"),
        make_entry(0, "dialogue", "美希", "「二発目の台詞よ」"),
        make_entry(0, "narration", "", "三番目の地の文。"),
        make_entry(0, "dialogue", "見里", "「四番目の先輩です」"),
        make_entry(0, "dialogue", "桜庭・友貴", "「五番目の合唱だ%N行くぞ」"),
    });
    crc::write_file_text(opt.input_file,
                         crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

    tr::Cache seed;
    seed_glossary_and_coverage_line(seed);
    tr::save_cache(seed, opt.cache_file);

    FakeTranslationTransport transport;
    transport.register_translation("「一発目の台詞だ」", "\"First spoken line.\"");
    transport.register_translation("「二発目の台詞よ」", "\"Second spoken line.\"");
    transport.register_translation("三番目の地の文。", "Third narration sentence.");
    transport.register_translation("「四番目の先輩です」", "\"Fourth upperclassman line.\"");
    transport.register_translation("「五番目の合唱だ%N行くぞ」",
                                   "\"Fifth chorus line.%NLet's go.\"");

    // Inject MAX_TOKENS truncation on the first attempt at batch 2 ({line 3, line 4})
    // so line 3 is salvaged cleanly and line 4 is re-translated without index shift.
    bool injected_truncation = false;
    transport.set_custom_hook(
        [&](std::size_t, const tr::GeminiRequest&,
            const std::vector<std::string>& jps) -> std::optional<tr::GeminiResponse> {
            if (!injected_truncation && jps.size() == kTwoElements &&
                jps[0] == "三番目の地の文。") {
                injected_truncation = true;
                tr::GeminiResponse trunc;
                trunc.finish_reason = "MAX_TOKENS";
                trunc.text =
                    "1. Third narration sentence.\n2. \"Truncated fourth upperclass";
                trunc.prompt_tokens = kDefaultFakePromptTokens;
                trunc.output_tokens = kDefaultFakeOutputTokens;
                return trunc;
            }
            return std::nullopt;
        });

    tr::BatchTranslator translator(&transport);
    const tr::TranslateReport report = translator.run(opt);

    EXPECT_EQ(report.exit_code, tr::kExitOk);
    EXPECT_EQ(report.translated_new, kFiveElements);
    EXPECT_EQ(report.remaining_uncached, 0u);
    EXPECT_TRUE(injected_truncation);
    ASSERT_GE(transport.call_count(), kFourElements);

    // Call 0 (batch 1): has <context_after> lookahead with lines 3 & 4.
    const std::string& first_prompt = transport.requests()[0].user_prompt;
    EXPECT_NE(first_prompt.find("<context_after>"), std::string::npos);
    EXPECT_NE(first_prompt.find("三番目の地の文。"), std::string::npos);
    EXPECT_NE(first_prompt.find("「四番目の先輩です」"), std::string::npos);

    // Call 1 (batch 2): carries <previous_context> with batch 1's English translations.
    const std::string& second_prompt = transport.requests()[1].user_prompt;
    EXPECT_NE(second_prompt.find("<previous_context>"), std::string::npos);
    EXPECT_NE(second_prompt.find("\"First spoken line.\""), std::string::npos);
    EXPECT_NE(second_prompt.find("\"Second spoken line.\""), std::string::npos);

    // Final call (line 5): sliding context_window=2 keeps lines 3 & 4, dropping line 1.
    const std::string& final_prompt = transport.requests().back().user_prompt;
    EXPECT_NE(final_prompt.find("<previous_context>"), std::string::npos);
    EXPECT_EQ(final_prompt.find("\"First spoken line.\""), std::string::npos);
    EXPECT_NE(final_prompt.find("\"Fourth upperclassman line.\""), std::string::npos);

    const tr::Cache saved = tr::load_cache(opt.cache_file);
    ASSERT_TRUE(saved.get("「五番目の合唱だ%N行くぞ」") != nullptr);
    EXPECT_EQ(*saved.get("「五番目の合唱だ%N行くぞ」"),
              "\"Fifth chorus line.%NLet's go.\"");
    EXPECT_TRUE(fs::exists(fs::u8path(opt.output_file)));
}

TEST(BatchTranslatorLive, FourStageContentBlockedCascadeIsolatesSingleBadLineWithoutPoisoningCache) {
    ScopedTempDir scratch;
    tr::GeminiTranslateOptions opt = make_live_options(scratch);
    opt.batch_size = 4;
    opt.context_window = 4;
    opt.context_after_window = 2;
    fs::create_directories(fs::u8path(opt.input_file).parent_path());

    constexpr const char* kBlockedJp = "「駄目……激しいっ！」";
    constexpr std::int64_t kBlockedOffset =
        static_cast<std::int64_t>(crc::extract::GAME_START_OFFSET) + 90'000;

    // Batch 1 (4 warmup lines) populates <previous_context>; Batch 2 holds 3
    // innocent lines + 1 persistently blocked line; Batch 3 populates <context_after>.
    const bj::object extracted = make_corpus_with_all_speakers({
        make_entry(0, "dialogue", "太一", "「準備運動その一」"),
        make_entry(0, "dialogue", "美希", "「準備運動その二」"),
        make_entry(0, "dialogue", "見里", "「準備運動その三」"),
        make_entry(0, "dialogue", "曜子", "「準備運動その四」"),
        make_entry(0, "dialogue", "太一", "「無実の隣人ライン一」"),
        make_entry(kBlockedOffset, "dialogue", "冬子", kBlockedJp),
        make_entry(0, "narration", "", "無実の隣人ライン三。"),
        make_entry(0, "dialogue", "霧", "「無実の隣人ライン四」"),
        make_entry(0, "narration", "", "後続コンテキストの行。"),
    });
    crc::write_file_text(opt.input_file,
                         crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

    tr::Cache seed;
    seed_glossary_and_coverage_line(seed);
    tr::save_cache(seed, opt.cache_file);

    FakeTranslationTransport transport;
    transport.register_translation("「準備運動その一」", "\"Warmup one.\"");
    transport.register_translation("「準備運動その二」", "\"Warmup two.\"");
    transport.register_translation("「準備運動その三」", "\"Warmup three.\"");
    transport.register_translation("「準備運動その四」", "\"Warmup four.\"");
    transport.register_translation("「無実の隣人ライン一」", "\"Innocent neighbor one.\"");
    transport.register_translation("無実の隣人ライン三。", "Innocent neighbor three.");
    transport.register_translation("「無実の隣人ライン四」", "\"Innocent neighbor four.\"");
    transport.register_translation("後続コンテキストの行。", "Trailing lookahead line.");
    transport.block_jp(kBlockedJp);

    tr::BatchTranslator translator(&transport);
    const tr::TranslateReport report = translator.run(opt);

    EXPECT_EQ(report.blocked_count, kSingleElementCount);
    EXPECT_EQ(report.translated_new, 8u);
    EXPECT_EQ(report.remaining_uncached, kSingleElementCount);

    // Verify all 4 cascade stages fired in order on Batch 2:
    // (1) full context + system_prompt -> (2) empty context + system_prompt ->
    // (3) empty context + neutral_system_prompt -> (4) bisection down to size-1 leaf.
    const auto& reqs = transport.requests();
    ASSERT_GE(reqs.size(), 7u);
    EXPECT_EQ(reqs[1].system_prompt, tr::system_prompt());
    EXPECT_NE(reqs[1].user_prompt.find("<previous_context>"), std::string::npos);
    EXPECT_NE(reqs[1].user_prompt.find("<context_after>"), std::string::npos);

    EXPECT_EQ(reqs[2].system_prompt, tr::system_prompt());
    EXPECT_EQ(reqs[2].user_prompt.find("<previous_context>"), std::string::npos);
    EXPECT_EQ(reqs[2].user_prompt.find("<context_after>"), std::string::npos);

    EXPECT_EQ(reqs[3].system_prompt, tr::neutral_system_prompt());
    EXPECT_EQ(reqs[3].user_prompt.find("<previous_context>"), std::string::npos);
    EXPECT_EQ(reqs[3].user_prompt.find("<context_after>"), std::string::npos);

    bool saw_size_one_blocked_leaf = false;
    for (std::size_t i = 4u; i < reqs.size(); ++i) {
        if (reqs[i].user_prompt.find(kBlockedJp) != std::string::npos &&
            reqs[i].user_prompt.find("2.") == std::string::npos) {
            saw_size_one_blocked_leaf = true;
            EXPECT_EQ(reqs[i].system_prompt, tr::neutral_system_prompt());
        }
    }
    EXPECT_TRUE(saw_size_one_blocked_leaf);

    // Innocent neighbors are translated and persisted; blocked line NEVER poisons cache.
    const tr::Cache saved_cache = tr::load_cache(opt.cache_file);
    EXPECT_FALSE(saved_cache.contains(kBlockedJp));
    ASSERT_NE(saved_cache.get("「無実の隣人ライン一」"), nullptr);
    EXPECT_EQ(*saved_cache.get("「無実の隣人ライン一」"), "\"Innocent neighbor one.\"");
    ASSERT_NE(saved_cache.get("無実の隣人ライン三。"), nullptr);
    EXPECT_EQ(*saved_cache.get("無実の隣人ライン三。"), "Innocent neighbor three.");
    ASSERT_NE(saved_cache.get("「無実の隣人ライン四」"), nullptr);
    EXPECT_EQ(*saved_cache.get("「無実の隣人ライン四」"), "\"Innocent neighbor four.\"");
    ASSERT_NE(saved_cache.get("後続コンテキストの行。"), nullptr);
    EXPECT_EQ(*saved_cache.get("後続コンテキストの行。"), "Trailing lookahead line.");

    // Blocked line is isolated in blocked_lines.json with offset, speaker, jp, and reason.
    ASSERT_TRUE(fs::exists(fs::u8path(opt.blocked_file)));
    const bj::value blocked_doc = crc::json_parse_file(opt.blocked_file);
    const bj::array& blocked_arr = extract_blocked_array(blocked_doc);
    ASSERT_EQ(blocked_arr.size(), kSingleElementCount);
    const bj::object& blocked_item = blocked_arr[0].as_object();
    EXPECT_EQ(json_i64(blocked_item.at("offset")), kBlockedOffset);
    const std::string_view recorded_jp = blocked_item.contains("jp")
                                             ? json_sv(blocked_item.at("jp"))
                                             : json_sv(blocked_item.at("text"));
    EXPECT_EQ(recorded_jp, kBlockedJp);
    EXPECT_FALSE(json_sv(blocked_item.at("reason")).empty());
}

TEST(BatchTranslatorLive, SecondRunResumesIdempotentlyWithZeroDuplicateTransportCalls) {
    ScopedTempDir scratch;
    tr::GeminiTranslateOptions opt = make_live_options(scratch);
    opt.batch_size = 3;
    fs::create_directories(fs::u8path(opt.input_file).parent_path());

    const bj::object extracted = make_corpus_with_all_speakers({
        make_entry(0, "dialogue", "太一", "「再開テスト一行目」"),
        make_entry(0, "dialogue", "美希", "「再開テスト二行目」"),
    });
    crc::write_file_text(opt.input_file,
                         crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

    FakeTranslationTransport transport;
    transport.register_translation(kSharedCoverageJp, kSharedCoverageEn);
    transport.register_translation("「再開テスト一行目」", "\"Resume test line one.\"");
    transport.register_translation("「再開テスト二行目」", "\"Resume test line two.\"");

    tr::BatchTranslator translator(&transport);
    const tr::TranslateReport first_run = translator.run(opt);
    ASSERT_EQ(first_run.exit_code, tr::kExitOk);
    EXPECT_EQ(first_run.translated_new, kThreeElements);
    EXPECT_EQ(first_run.remaining_uncached, 0u);
    ASSERT_EQ(transport.call_count(), kSingleElementCount);

    const std::vector<std::pair<std::string, std::string>> order_after_first =
        tr::load_cache(opt.cache_file).items();

    const tr::TranslateReport second_run = translator.run(opt);
    EXPECT_EQ(second_run.exit_code, tr::kExitOk);
    EXPECT_EQ(second_run.translated_new, 0u);
    EXPECT_EQ(second_run.remaining_uncached, 0u);
    EXPECT_EQ(transport.call_count(), kSingleElementCount);
    EXPECT_EQ(tr::load_cache(opt.cache_file).items(), order_after_first);
}

TEST(BatchTranslatorLive, MaxBatchesAndMaxBudgetEurCircuitBreakersPersistCheckpointsCleanly) {
    {
        ScopedTempDir scratch;
        tr::GeminiTranslateOptions opt = make_live_options(scratch);
        opt.batch_size = 2;
        opt.max_batches = 1;
        fs::create_directories(fs::u8path(opt.input_file).parent_path());

        const bj::object extracted = make_corpus_with_all_speakers({
            make_entry(0, "dialogue", "太一", "「バッチ上限一」"),
            make_entry(0, "dialogue", "美希", "「バッチ上限二」"),
            make_entry(0, "dialogue", "見里", "「バッチ上限三」"),
            make_entry(0, "dialogue", "曜子", "「バッチ上限四」"),
        });
        crc::write_file_text(opt.input_file,
                             crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

        tr::Cache seed;
        seed_glossary_and_coverage_line(seed);
        tr::save_cache(seed, opt.cache_file);

        FakeTranslationTransport transport;
        transport.register_translation("「バッチ上限一」", "\"Batch cap one.\"");
        transport.register_translation("「バッチ上限二」", "\"Batch cap two.\"");
        transport.register_translation("「バッチ上限三」", "\"Batch cap three.\"");
        transport.register_translation("「バッチ上限四」", "\"Batch cap four.\"");

        tr::BatchTranslator translator(&transport);
        const tr::TranslateReport report = translator.run(opt);
        EXPECT_EQ(report.exit_code, tr::kExitOk);
        EXPECT_TRUE(report.stopped_by_max_batches);
        EXPECT_FALSE(report.stopped_by_budget);
        EXPECT_EQ(transport.call_count(), kSingleElementCount);
        EXPECT_EQ(report.translated_new, kTwoElements);
        EXPECT_EQ(report.remaining_uncached, kTwoElements);

        const tr::Cache saved = tr::load_cache(opt.cache_file);
        EXPECT_EQ(saved.size(), kExpectedGlossarySize + 1u + kTwoElements);
        EXPECT_TRUE(saved.contains("「バッチ上限一」"));
        EXPECT_TRUE(saved.contains("「バッチ上限二」"));
        EXPECT_FALSE(saved.contains("「バッチ上限三」"));

        ASSERT_TRUE(fs::exists(fs::u8path(opt.last_batch_file)));
        const std::string last_batch_raw = read_file_string(fs::u8path(opt.last_batch_file));
        (void)crc::json_parse_file(opt.last_batch_file);
        EXPECT_NE(last_batch_raw.find("Batch cap one."), std::string::npos);
        EXPECT_NE(last_batch_raw.find("Batch cap two."), std::string::npos);
        EXPECT_EQ(last_batch_raw.find("Batch cap three."), std::string::npos);
    }

    {
        ScopedTempDir scratch;
        tr::GeminiTranslateOptions opt = make_live_options(scratch);
        opt.batch_size = 2;
        opt.max_batches = 0;
        opt.max_budget_eur = kTightBudgetEur;
        fs::create_directories(fs::u8path(opt.input_file).parent_path());

        const bj::object extracted = make_corpus_with_all_speakers({
            make_entry(0, "dialogue", "太一", "「予算上限一」"),
            make_entry(0, "dialogue", "美希", "「予算上限二」"),
            make_entry(0, "dialogue", "見里", "「予算上限三」"),
            make_entry(0, "dialogue", "曜子", "「予算上限四」"),
        });
        crc::write_file_text(opt.input_file,
                             crc::json_pretty(bj::value(extracted), kExtractedJsonIndent));

        tr::Cache seed;
        seed_glossary_and_coverage_line(seed);
        tr::save_cache(seed, opt.cache_file);

        FakeTranslationTransport transport;
        transport.set_output_tokens_per_call(kExpensiveOutputTokens);
        transport.register_translation("「予算上限一」", "\"Budget cap one.\"");
        transport.register_translation("「予算上限二」", "\"Budget cap two.\"");
        transport.register_translation("「予算上限三」", "\"Budget cap three.\"");
        transport.register_translation("「予算上限四」", "\"Budget cap four.\"");

        tr::BatchTranslator translator(&transport);
        const tr::TranslateReport report = translator.run(opt);
        EXPECT_TRUE(report.stopped_by_budget);
        EXPECT_FALSE(report.stopped_by_max_batches);
        EXPECT_GT(report.estimated_eur, kTightBudgetEur);
        EXPECT_EQ(transport.call_count(), kSingleElementCount);
        EXPECT_EQ(report.translated_new, kTwoElements);
        EXPECT_EQ(report.remaining_uncached, kTwoElements);

        const tr::Cache saved = tr::load_cache(opt.cache_file);
        EXPECT_TRUE(saved.contains("「予算上限一」"));
        EXPECT_TRUE(saved.contains("「予算上限二」"));
        EXPECT_FALSE(saved.contains("「予算上限三」"));
        ASSERT_TRUE(fs::exists(fs::u8path(opt.last_batch_file)));
        (void)crc::json_parse_file(opt.last_batch_file);
    }
}
