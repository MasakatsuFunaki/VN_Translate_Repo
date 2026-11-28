// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate_core.h"

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

#include <boost/regex.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace fav::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int kMinValidBatchSize = 1;
constexpr int kMinNonNegativeOptionValue = 0;
constexpr double kMinNonNegativeBudgetEur = 0.0;
constexpr int kDefaultPrettyIndent = 2;
constexpr unsigned char kNullByte = 0x00u;
constexpr unsigned char kNonAsciiByteMin = 0x80u;
constexpr std::size_t kPromptNumberOffset = 1;
constexpr int kMinBatchNumberForNumberedLog = 1;
constexpr int kBatchNumberDisplayOffset = 1;
constexpr int kGateFailureExitCode = 2;
constexpr int kRuntimeFailureExitCode = 1;
constexpr std::size_t kContextRetentionMultiplier = 2;
constexpr std::size_t kBinarySplitDivisor = 2;
constexpr std::size_t kMaxSamplePreviewLines = 2;
constexpr std::size_t kMaxSamplePreviewChars = 110;
constexpr int kRateLimitSleepSeconds = 60;
constexpr std::size_t kProgressBufferBytes = 256;
constexpr std::size_t kSizeBufferBytes = 64;
constexpr double kBytesPerKilobyte = 1024.0;

constexpr const char* kNeutralSystemPrompt =
    "You are a professional Japanese-to-English literary translator working on "
    "a licensed commercial visual novel (Sakura, Moyu.). "
    "Cast names: Taiga, Kuro, Haru, Chiwa, Himeori, Mashiro, Toya, Asahi, "
    "Azusaki, Nana, Tomohito, Kazuma, Tooya, Sol, Nacht. "
    "Translate each numbered Japanese line faithfully into natural English "
    "dialogue or narration. Preserve exact numbering, never add [Speaker] "
    "tags or commentary, strip ruby brackets, and use ASCII characters only.";

struct BatchCallOptions {
    bool is_top_level = true;
    bool use_neutral_prompt = false;
    bool retry_on_zero_thoughts = true;
};

struct AsciiFoldRule {
    const char* from;
    const char* to;
};

constexpr AsciiFoldRule kAsciiFoldRules[] = {
    {"—", "--"},    {"–", "--"},    {"―", "--"},    {"─", "--"},
    {"…", "..."},   {"⋯", "..."},   {"‘", "'"},     {"’", "'"},
    {"“", "\""},    {"”", "\""},    {"«", "\""},    {"»", "\""},
    {"　", " "},    {"•", "*"},     {"·", "."},     {"\u301C", "~"},
    {"\uFF5E", "~"},{"♪", "~"},     {"☆", "*"},     {"★", "*"},
    {"♡", ""},      {"♥", ""},      {"×", "x"},     {"÷", "/"},
    {"À", "A"},     {"Á", "A"},     {"Â", "A"},     {"Ã", "A"},
    {"Ä", "A"},     {"Å", "A"},     {"à", "a"},     {"á", "a"},
    {"â", "a"},     {"ã", "a"},     {"ä", "a"},     {"å", "a"},
    {"Æ", "AE"},    {"æ", "ae"},    {"Ç", "C"},     {"ç", "c"},
    {"È", "E"},     {"É", "E"},     {"Ê", "E"},     {"Ë", "E"},
    {"è", "e"},     {"é", "e"},     {"ê", "e"},     {"ë", "e"},
    {"Ì", "I"},     {"Í", "I"},     {"Î", "I"},     {"Ï", "I"},
    {"ì", "i"},     {"í", "i"},     {"î", "i"},     {"ï", "i"},
    {"Ñ", "N"},     {"ñ", "n"},     {"Ò", "O"},     {"Ó", "O"},
    {"Ô", "O"},     {"Õ", "O"},     {"Ö", "O"},     {"ò", "o"},
    {"ó", "o"},     {"ô", "o"},     {"õ", "o"},     {"ö", "o"},
    {"Œ", "OE"},    {"œ", "oe"},    {"Ù", "U"},     {"Ú", "U"},
    {"Û", "U"},     {"Ü", "U"},     {"ù", "u"},     {"ú", "u"},
    {"û", "u"},     {"ü", "u"},     {"Ý", "Y"},     {"ý", "y"},
    {"ÿ", "y"},     {"ß", "ss"},
};

void accumulate_usage(vn::llm::LlmTokenUsage& total,
                      const gemini::Usage& delta) noexcept {
    total.prompt_tokens += delta.input_tokens;
    total.cached_tokens += delta.cache_read_input_tokens;
    total.thought_tokens += delta.thought_tokens;
    const std::int64_t candidate_delta =
        delta.output_tokens > delta.thought_tokens
            ? (delta.output_tokens - delta.thought_tokens)
            : 0;
    total.candidate_tokens += candidate_delta;
}

void add_gemini_usage(gemini::Usage& dst, const gemini::Usage& src) noexcept {
    dst.input_tokens += src.input_tokens;
    dst.output_tokens += src.output_tokens;
    dst.thought_tokens += src.thought_tokens;
    dst.cache_read_input_tokens += src.cache_read_input_tokens;
    dst.cache_creation_input_tokens += src.cache_creation_input_tokens;
}

std::string replace_all_str(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

}  // namespace

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches,
                                            double max_budget_eur) {
    if (batch_size < kMinValidBatchSize)
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    if (test_n < kMinNonNegativeOptionValue)
        return "--test cannot be negative (got " + std::to_string(test_n) + ")";
    // A negative cap produces budget < 0, which reads as "no limit" and starts
    // an unbounded run -- the opposite of what the flag is for.
    if (max_batches < kMinNonNegativeOptionValue)
        return "--max-batches cannot be negative (got " + std::to_string(max_batches) +
               "); use 0 for no limit";
    if (!std::isfinite(max_budget_eur))
        return "--max-budget-eur must be finite; use 0 for no limit";
    if (max_budget_eur < kMinNonNegativeBudgetEur)
        return "--max-budget-eur cannot be negative (got " + std::to_string(max_budget_eur) +
               "); use 0 for no limit";
    return std::nullopt;
}

bool needs_translation(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty() || t.front() == '_' || t.find('/') != std::string::npos) return false;
    return is_jp(t);
}

std::string strip(const std::string& s) { return trim(s); }

std::string sanitize_ascii(const std::string& text) {
    std::string s = text;
    for (const auto& rule : kAsciiFoldRules) {
        s = replace_all_str(std::move(s), rule.from, rule.to);
    }
    static const boost::regex ruby_re(R"(\[([^|\]]+)\|([^\]]+)\])");
    s = boost::regex_replace(s, ruby_re, "$2");
    static const boost::regex angle_ruby_re(R"(<([^|>]+)\|([^>]+)>)");
    s = boost::regex_replace(s, angle_ruby_re, "$1");
    return s;
}

bool is_ascii_clean(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](char ch) noexcept {
        const auto b = static_cast<unsigned char>(ch);
        return b != kNullByte && b < kNonAsciiByteMin;
    });
}

std::string strip_speaker_tags(const std::string& text) {
    static const boost::regex re(R"(^(?:\[(?:NARRATION|[A-Za-z0-9\s()'"?&$.,!_-]+)\]\s*)+)");
    return strip(sanitize_ascii(boost::regex_replace(text, re, "")));
}

Cache load_cache(const std::string& cache_file) {
    Cache cache;
    if (!fs::exists(fs::u8path(cache_file))) return cache;
    bj::value root = json_parse_file(cache_file);
    // A cache is a JSON object of jp -> en.  get_object() rejects any other
    // shape without saying which file it read, and the caller is often about
    // to delete that file.
    if (!root.is_object())
        throw std::runtime_error("cannot parse " + cache_file +
                                 ": a translation cache must be a JSON object");
    for (const auto& kv : root.get_object())
        if (kv.value().is_string())
            cache.set(std::string(kv.key()), std::string(kv.value().get_string()));
    return cache;
}

std::size_t cache_entry_count(const std::string& cache_file) {
    return load_cache(cache_file).size();
}

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache) {
    if (discard_cache || cache_entries <= CACHE_DISCARD_THRESHOLD)
        return std::nullopt;
    return flag + " deletes the translation cache, which holds " +
           std::to_string(cache_entries) +
           " lines that were paid for. Nothing in this pipeline can rebuild "
           "them.\n"
           "        To cap a run and keep them, use --max-batches N instead.\n"
           "        To delete them anyway, add --discard-cache.";
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items())
        o[jp] = en;
    write_file_atomic_text(cache_file,
                           json_pretty(bj::value(std::move(o)), kDefaultPrettyIndent));
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    const std::string t = trim(jp);
    if (!t.empty() && (t.front() == '_' || t.find('/') != std::string::npos)) return true;
    if (jp != en) return false;
    for (char c : jp)
        if (static_cast<unsigned char>(c) >= kNonAsciiByteMin) return true;
    return false;
}

std::size_t purge_failed_entries(Cache& cache) {
    std::size_t removed = 0;
    Cache kept;
    for (const auto& [jp, en] : cache.items()) {
        if (is_failed_entry(jp, en)) {
            ++removed;
            continue;
        }
        kept.set(jp, en);
    }
    if (removed) cache = std::move(kept);
    return removed;
}

void apply_line_result(Cache& cache, std::vector<std::string>& previous_context,
                       const std::string& jp, const std::string& en) {
    // An answer that never arrived, leaked Japanese, or retained non-ASCII /
    // NUL bytes is not a usable FVP translation.  Leaving it uncached ensures
    // the next resume run asks for it again while the Japanese carries the
    // context window in sequence.
    if (en.find('\0') != std::string::npos) {
        previous_context.push_back(jp);
        return;
    }
    const std::string cleaned = strip_speaker_tags(en);
    if (cleaned.empty() || jp_char_count(cleaned) > 0 || !is_ascii_clean(cleaned)) {
        previous_context.push_back(jp);
        return;
    }
    cache.set(jp, cleaned);
    previous_context.push_back(cleaned);
}

std::string build_user_prompt(const std::vector<std::string>& batch_lines,
                              const std::vector<std::string>& previous_context,
                              const std::vector<std::string>& context_after) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        std::size_t start = previous_context.size() > CONTEXT_WINDOW
                                ? previous_context.size() - CONTEXT_WINDOW
                                : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i)
            user_prompt += previous_context[i] + "\n";
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i)
        user_prompt += std::to_string(i + kPromptNumberOffset) + ". " + batch_lines[i] + "\n";
    user_prompt += "</lines_to_translate>\n\n";
    if (!context_after.empty()) {
        user_prompt += "<context_after>\n";
        const std::size_t take =
            std::min<std::size_t>(context_after.size(), static_cast<std::size_t>(CONTEXT_AFTER));
        for (std::size_t i = 0; i < take; ++i)
            user_prompt += context_after[i] + "\n";
        user_prompt += "</context_after>\n\n";
    }
    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use <previous_context> (if present) for scene continuity, and <context_after> (if present) for lookahead context (preserving literal imagery when an upcoming line pays off a metaphor, idiom, or wordplay), but do NOT translate them. "
        "Never include [Speaker] tags in your output. "
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line.";
    return user_prompt;
}

std::map<int, std::string> parse_numbered_response(const std::string& response_text) {
    static const boost::regex line_re(R"(^(\d+)[.\):\s]+\s*(.*))");
    std::map<int, std::string> translations;
    int current_num = -1;
    std::vector<std::string> current_parts;

    auto flush = [&] {
        if (current_num < 0) return;
        std::string joined;
        for (std::size_t i = 0; i < current_parts.size(); ++i) {
            if (i) joined += ' ';
            joined += current_parts[i];
        }
        translations[current_num] = strip_speaker_tags(strip(joined));
    };

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = strip(line);
        if (line.empty()) continue;
        boost::smatch m;
        if (boost::regex_match(line, m, line_re)) {
            flush();
            current_num = std::stoi(m[1].str());
            std::string text = strip_speaker_tags(strip(m[2].str()));
            current_parts.clear();
            if (!text.empty()) current_parts.push_back(text);
        } else if (current_num >= 0) {
            current_parts.push_back(line);
        }
    }
    flush();
    return translations;
}

namespace {

BatchResult run_gemini_batch_with_guard(
    gemini::Client& client,
    const std::vector<std::string>& batch_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after,
    const std::string& model,
    const std::string& thinking_level,
    vn::llm::ZeroThoughtCircuitBreaker& guard,
    BatchCallOptions call_opt = {}) {
    const std::string active_model =
        model.empty() ? std::string(vn::llm::kDefaultModel) : model;
    const std::string active_thinking =
        thinking_level.empty() ? std::string(vn::llm::kDefaultThinkingLevel)
                               : thinking_level;

    bj::object body;
    body["model"] = active_model;
    body["max_tokens"] = vn::llm::compute_batch_max_output_tokens(batch_lines.size());
    body["thinking_level"] = active_thinking;
    body["retry_on_zero_thoughts"] = call_opt.retry_on_zero_thoughts;
    bj::object sys_block;
    sys_block["type"] = "text";
    sys_block["text"] =
        call_opt.use_neutral_prompt ? kNeutralSystemPrompt : SYSTEM_PROMPT;
    sys_block["cache_control"] = bj::object{{"type", "ephemeral"}};
    body["system"] = bj::array{std::move(sys_block)};
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = build_user_prompt(batch_lines, previous_context, context_after);
    body["messages"] = bj::array{std::move(msg)};

    const bj::value response = client.messages(bj::value(std::move(body)));
    const bj::object& res_obj = response.as_object();

    std::string response_text;
    if (const auto* content = res_obj.if_contains("content");
        content != nullptr && content->is_array()) {
        for (const auto& block : content->get_array()) {
            if (!block.is_object()) {
                continue;
            }
            const auto* t = block.get_object().if_contains("type");
            if (t != nullptr && t->is_string() && t->get_string() == "text") {
                if (const auto* txt = block.get_object().if_contains("text");
                    txt != nullptr && txt->is_string()) {
                    response_text = strip(std::string(txt->get_string()));
                }
                break;
            }
        }
    }

    std::string stop_reason = "end_turn";
    if (const auto* sr = res_obj.if_contains("stop_reason");
        sr != nullptr && sr->is_string()) {
        stop_reason = std::string(sr->get_string());
    }

    const gemini::Usage usage = gemini::extract_usage(response);
    log_info("    Tokens: in=" + std::to_string(usage.input_tokens) +
             " out=" + std::to_string(usage.output_tokens) +
             " thoughts=" + std::to_string(usage.thought_tokens) +
             " cache_read=" + std::to_string(usage.cache_read_input_tokens) +
             " cache_create=" + std::to_string(usage.cache_creation_input_tokens));

    const bool content_blocked = (stop_reason == "refusal");
    try {
        guard.check_response(active_model, active_thinking, call_opt.is_top_level,
                             batch_lines.size(), content_blocked, usage.thought_tokens);
    } catch (const vn::llm::LlmZeroThoughtError& e) {
        log_info(std::string("[ERROR] ") + e.what());
        throw ZeroThoughtError(e.what());
    }

    BatchResult out;
    out.translations = parse_numbered_response(response_text);
    out.stop_reason = std::move(stop_reason);
    out.usage = usage;
    return out;
}

void bisect_refused_slice(
    gemini::Client& api,
    const std::vector<std::string>& prompt_lines,
    std::size_t base_offset,
    const std::string& active_model,
    const std::string& active_thinking,
    vn::llm::ZeroThoughtCircuitBreaker& circuit_breaker,
    vn::llm::LlmTokenUsage& cumulative_usage,
    double max_budget_eur,
    bool& stopped_by_budget,
    gemini::Usage& batch_usage,
    std::map<int, std::string>& merged_translations) {
    if (prompt_lines.empty() || stopped_by_budget) {
        return;
    }
    if (max_budget_eur > kMinNonNegativeBudgetEur &&
        vn::llm::estimate_cost_eur(cumulative_usage, active_model) >= max_budget_eur) {
        stopped_by_budget = true;
        return;
    }

    const BatchCallOptions sub_opt{
        /*is_top_level=*/false,
        /*use_neutral_prompt=*/true,
        /*retry_on_zero_thoughts=*/prompt_lines.size() >= kZeroThoughtMinBatchLines,
    };
    const BatchResult sub_res = run_gemini_batch_with_guard(
        api, prompt_lines, {}, {}, active_model, active_thinking,
        circuit_breaker, sub_opt);
    add_gemini_usage(batch_usage, sub_res.usage);
    accumulate_usage(cumulative_usage, sub_res.usage);

    if (sub_res.stop_reason != "refusal" && !sub_res.translations.empty()) {
        for (const auto& [one_based, en] : sub_res.translations) {
            if (one_based >= 1 &&
                static_cast<std::size_t>(one_based) <= prompt_lines.size()) {
                merged_translations[static_cast<int>(base_offset) + one_based] = en;
            }
        }
        return;
    }

    if (prompt_lines.size() == 1u) {
        log_info("    [WARN] Blocked single line (PROHIBITED_CONTENT even at size 1) "
                 "-- leaving queued");
        return;
    }

    const std::size_t mid = prompt_lines.size() / kBinarySplitDivisor;
    const std::vector<std::string> left(
        prompt_lines.begin(),
        prompt_lines.begin() + static_cast<std::ptrdiff_t>(mid));
    const std::vector<std::string> right(
        prompt_lines.begin() + static_cast<std::ptrdiff_t>(mid),
        prompt_lines.end());
    bisect_refused_slice(api, left, base_offset, active_model, active_thinking,
                         circuit_breaker, cumulative_usage, max_budget_eur,
                         stopped_by_budget, batch_usage, merged_translations);
    bisect_refused_slice(api, right, base_offset + mid, active_model, active_thinking,
                         circuit_breaker, cumulative_usage, max_budget_eur,
                         stopped_by_budget, batch_usage, merged_translations);
}

}  // namespace

bool is_zero_thought_violation(std::string_view model,
                               std::string_view thinking_level,
                               std::string_view stop_reason,
                               std::size_t batch_line_count,
                               std::int64_t thought_tokens) noexcept {
    try {
        vn::llm::ZeroThoughtCircuitBreaker guard;
        guard.check_response(model, thinking_level, true, batch_line_count,
                             stop_reason == "refusal", thought_tokens);
        return false;
    } catch (const vn::llm::LlmZeroThoughtError&) {
        return true;
    } catch (...) {
        return false;
    }
}

BatchResult call_gemini_batch(
    gemini::Client& client, const std::vector<std::string>& batch_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after,
    const std::string& model,
    const std::string& thinking_level) {
    vn::llm::ZeroThoughtCircuitBreaker guard;
    return run_gemini_batch_with_guard(
        client, batch_lines, previous_context, context_after,
        model, thinking_level, guard);
}

std::map<int, std::string> call_gemini(
    gemini::Client& client, const std::vector<std::string>& batch_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after) {
    return call_gemini_batch(client, batch_lines, previous_context, context_after)
        .translations;
}

namespace {

// Per-batch JSON log: entries accumulate per batch index and the whole batch
// is rewritten after every append, so a Ctrl+C still leaves the partial batch
// on disk.
class BatchLogger {
public:
    BatchLogger(std::string dir, int num_batches)
        : dir_(std::move(dir)), num_batches_(num_batches) {}

    void append(bj::object entry) {
        entries_.push_back(std::move(entry));
        write_file_text(path(), json_pretty(bj::value(entries_), kDefaultPrettyIndent));
    }

    void next_batch() {
        ++batch_num_;
        entries_.clear();
    }

    std::string path() const {
        return num_batches_ > kMinBatchNumberForNumberedLog
                   ? dir_ + "\\last_translation" +
                         std::to_string(batch_num_ + kBatchNumberDisplayOffset) + ".json"
                   : dir_ + "\\last_translation.json";
    }

private:
    std::string dir_;
    int num_batches_;
    int batch_num_ = 0;
    bj::array entries_;
};

// Strips the date suffix off dated model ids for the per-model log folder.
std::string model_short(const std::string& m) {
    std::size_t p = m.find("-20");
    return p == std::string::npos ? m : m.substr(0, p);
}

}  // namespace

int translate_all(const TranslateOptions& opt) {
    const std::string active_model = opt.model.empty() ? MODEL : opt.model;
    const std::string active_thinking =
        opt.thinking_level.empty() ? std::string(gemini::kDefaultThinkingLevel)
                                   : opt.thinking_level;
    log_info("Model: " + active_model);
    log_info("Thinking level: " + active_thinking);
    log_info("Batch size: " + std::to_string(opt.batch_size) + " lines per request");
    log_info("Context window: " + std::to_string(CONTEXT_WINDOW) + " previous lines");

    // The gate comes first, before an API key is read and before a file is
    // removed: a run that cannot produce a correct answer must cost nothing
    // and leave nothing behind.
    log_info("\n--- Cast-roster gate (no tokens spent) ---");
    Bytes hcb;
    try {
        hcb = read_file(opt.hcb_file);
    } catch (const std::exception& e) {
        log_info(std::string("\n[ERROR] the gate cannot read ") + opt.hcb_file + ": " + e.what());
        log_info("        Nothing was translated and nothing was written.");
        return kGateFailureExitCode;
    }
    const SpeakerCheckReport gate = run_speaker_checks(hcb);
    if (!gate.passed) {
        log_info("\n[ERROR] cast check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return kGateFailureExitCode;
    }
    const auto line_speakers = map_line_speakers(hcb);

    // A fresh run drops what earlier runs left, so the table this run produces
    // holds exactly what this run answered.  It happens after the gate for the
    // same reason the gate comes first.
    // A run that discards the cache is certain to call the API, so the key is
    // required before anything is deleted rather than at the first batch.
    if (opt.fresh_run && opt.endpoint == nullptr && gemini::load_api_key().empty()) {
        log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                 "cache for a run that cannot translate.");
        return kRuntimeFailureExitCode;
    }

    if (opt.fresh_run)
        for (const std::string& p : {opt.cache_file, opt.output_file}) {
            // Reported, not thrown, at both calls: the throwing overloads name
            // no file, and this run is about to translate from scratch.  A
            // delete fails when another process holds the file open without
            // FILE_SHARE_DELETE, which scanners, indexers and sync clients
            // routinely do.
            const fs::path path = fs::u8path(p);
            std::error_code ec;
            const bool present = fs::exists(path, ec);
            if (!ec && !present) continue;  // never written: nothing to remove
            const bool removed = !ec && fs::remove(path, ec);
            if (ec) {
                log_info("[ERROR] cannot delete " + p + ": " + ec.message() +
                         " -- close whatever has it open, then retry.");
                return kRuntimeFailureExitCode;
            }
            // A file that vanished between the two calls was not removed here.
            log_info(removed ? "Fresh run: removed " + p
                             : "Fresh run: " + p + " was already gone");
        }

    gemini::LazyClient client(opt.endpoint);

    log_info("\nLoading extracted text...");
    bj::value all_scripts_v = json_parse_file(opt.input_file);
    bj::object& all_scripts = all_scripts_v.get_object();

    log_info("Loading translation cache...");
    Cache cache;
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
    } else {
        cache = load_cache(opt.cache_file);
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
        std::size_t purged = purge_failed_entries(cache);
        log_info("  Failed entries purged: " + std::to_string(purged) +
                 (purged ? "  (those lines re-queue)" : ""));
    }

    for (const auto& [jp, en] : name_translations_ordered())
        cache.set_default(jp, en);

    // Batch log dir: test runs get a per-model subfolder so a smoke run never
    // overwrites the last full run's log.
    const std::string log_dir =
        opt.test_mode ? opt.last_translate_dir + "\\" + model_short(active_model)
                      : opt.last_translate_dir;
    fs::create_directories(fs::u8path(log_dir));
    const fs::path telemetry_path = fs::u8path(log_dir + "\\last_batch_telemetry.json");
    if (fs::exists(telemetry_path)) {
        fs::remove(telemetry_path);
    }
    if (opt.num_batches > 1) {
        for (int bi = 1; bi <= opt.num_batches; ++bi) {
            fs::path p = fs::u8path(log_dir + "\\last_translation" + std::to_string(bi) + ".json");
            if (fs::exists(p)) fs::remove(p);
        }
    } else {
        fs::path p = fs::u8path(log_dir + "\\last_translation.json");
        if (fs::exists(p)) fs::remove(p);
    }
    BatchLogger batch_log(log_dir, opt.num_batches);

    // Work list: every line (in stream order) that needs translation.
    bj::object& script = all_scripts.at("Sakura.hcb").get_object();
    bj::array& all_lines = script.at("lines").get_array();

    std::vector<bj::object*> work;
    for (auto& lv : all_lines) {
        auto& line = lv.get_object();
        auto* type = line.if_contains("type");
        if (!type || !type->is_string() || type->get_string() != "STRING") continue;
        if (!needs_translation(std::string(line.at("content").get_string()))) continue;
        work.push_back(&line);
    }
    log_info("\nTotal translatable lines: " + comma(static_cast<long long>(work.size())));

    auto lookup_cached = [&](const std::string& s) -> const std::string* {
        if (const std::string* en = cache.get(s)) return en;
        const std::string t = trim(s);
        if (t != s) return cache.get(t);
        return nullptr;
    };

    auto count_uncached = [&](const std::vector<bj::object*>& v) {
        std::size_t n = 0;
        for (auto* ln : v)
            if (!lookup_cached(std::string(ln->at("content").get_string()))) ++n;
        return n;
    };
    std::size_t uncached = count_uncached(work);
    log_info("  uncached: " + comma(static_cast<long long>(uncached)));

    auto format_with_speaker = [&](const bj::object& ln) -> std::string {
        std::string c(ln.at("content").get_string());
        if (const auto* off_v = ln.if_contains("offset")) {
            std::uint32_t off = 0;
            if (off_v->is_int64()) off = static_cast<std::uint32_t>(off_v->get_int64());
            else if (off_v->is_uint64()) off = static_cast<std::uint32_t>(off_v->get_uint64());
            auto it = line_speakers.find(off);
            if (it != line_speakers.end() && !it->second.empty() && it->second != "???")
                return "[" + it->second + "] " + c;
        }
        return c;
    };

    if (opt.test_mode) {
        const std::size_t target_lines =
            static_cast<std::size_t>(opt.num_batches) * static_cast<std::size_t>(opt.batch_size);
        log_info("  --test " + std::to_string(opt.num_batches) + ": targeting up to " +
                 std::to_string(std::min(uncached, target_lines)) + " uncached lines across " +
                 std::to_string(opt.num_batches) + " batch(es)");
    }

    if (uncached == 0) {
        save_cache(cache, opt.cache_file);
        log_info("\n  Everything is cached -- skipping API calls.");
    } else {
        std::vector<std::string> previous_context;
        vn::llm::ZeroThoughtCircuitBreaker circuit_breaker;
        vn::llm::LlmTokenUsage cumulative_usage;
        bool stopped_by_budget = false;
        int batches_remaining = opt.num_batches;
        int batch_index = 0;
        const std::size_t batch_sz = static_cast<std::size_t>(opt.batch_size);
        const std::size_t est_total_batches = (uncached + batch_sz - 1u) / batch_sz;
        auto start = std::chrono::steady_clock::now();
        long long done = 0;

        std::size_t i = 0;
        while (i < work.size()) {
            while (i < work.size()) {
                std::string c(work[i]->at("content").get_string());
                const std::string* en = lookup_cached(c);
                if (!en) break;
                previous_context.push_back(*en);
                ++i;
            }
            if (previous_context.size() > CONTEXT_WINDOW * kContextRetentionMultiplier)
                previous_context.erase(
                    previous_context.begin(),
                    previous_context.end() - CONTEXT_WINDOW * kContextRetentionMultiplier);
            if (i >= work.size()) break;

            if (opt.max_budget_eur > kMinNonNegativeBudgetEur &&
                vn::llm::estimate_cost_eur(cumulative_usage, active_model) >=
                    opt.max_budget_eur) {
                stopped_by_budget = true;
                log_info("[BUDGET] Reached --max-budget-eur cap -- stopping cleanly "
                         "with cache saved.");
                break;
            }

            const std::size_t batch_start = i;
            std::vector<std::string> api_lines;
            std::vector<std::string> prompt_lines;
            std::unordered_map<std::string, std::size_t> unique_idx;
            api_lines.reserve(batch_sz);
            prompt_lines.reserve(batch_sz);

            while (i < work.size() && api_lines.size() < batch_sz) {
                std::string c(work[i]->at("content").get_string());
                if (!lookup_cached(c) && unique_idx.find(c) == unique_idx.end()) {
                    unique_idx.emplace(c, api_lines.size());
                    api_lines.push_back(c);
                    prompt_lines.push_back(format_with_speaker(*work[i]));
                }
                ++i;
            }
            const std::size_t batch_end = i;
            if (api_lines.empty()) break;

            std::vector<std::string> context_after;
            const std::size_t after_end =
                std::min(work.size(), batch_end + static_cast<std::size_t>(CONTEXT_AFTER));
            context_after.reserve(after_end - batch_end);
            for (std::size_t k = batch_end; k < after_end; ++k)
                context_after.push_back(format_with_speaker(*work[k]));

            // The first batch the cache cannot cover is where a key becomes
            // required.  Outside the try below on purpose: a missing key has
            // to stop the run, not be caught as a batch error and written to
            // the cache as the Japanese translating to itself.
            gemini::Client& api = client.get();
            ++batch_index;

            // Counted as the answers arrive, not from the batch size: a line
            // the model did not return stays queued, and a queued line is not
            // progress.
            long long translated_now = 0;
            std::int64_t batch_thoughts = 0;
            try {
                const BatchCallOptions top_opt{
                    /*is_top_level=*/true,
                    /*use_neutral_prompt=*/false,
                    /*retry_on_zero_thoughts=*/prompt_lines.size() >= kZeroThoughtMinBatchLines,
                };
                BatchResult batch_res = run_gemini_batch_with_guard(
                    api, prompt_lines, previous_context, context_after,
                    active_model, active_thinking, circuit_breaker, top_opt);
                gemini::Usage batch_usage = batch_res.usage;
                std::int64_t winning_thoughts = batch_res.usage.thought_tokens;
                accumulate_usage(cumulative_usage, batch_res.usage);

                if (batch_res.stop_reason == "refusal") {
                    log_info("    Content blocked (PROHIBITED_CONTENT) -- bisecting "
                             "slice with neutral prompt");
                    std::map<int, std::string> rescued;
                    bisect_refused_slice(api, prompt_lines, 0u, active_model,
                                         active_thinking, circuit_breaker,
                                         cumulative_usage, opt.max_budget_eur,
                                         stopped_by_budget, batch_usage, rescued);
                    batch_res.translations = std::move(rescued);
                    batch_res.usage = batch_usage;
                    winning_thoughts = batch_usage.thought_tokens;
                } else if (batch_res.translations.empty() &&
                           (!previous_context.empty() || !context_after.empty())) {
                    log_info("    Empty response with context window -- retrying batch "
                             "without context");
                    BatchResult retry_res = run_gemini_batch_with_guard(
                        api, prompt_lines, {}, {}, active_model, active_thinking,
                        circuit_breaker, top_opt);
                    winning_thoughts = retry_res.usage.thought_tokens;
                    add_gemini_usage(batch_usage, retry_res.usage);
                    accumulate_usage(cumulative_usage, retry_res.usage);
                    batch_res = std::move(retry_res);
                    batch_res.usage = batch_usage;
                }

                const bool refuse_zero_thought_cache =
                    vn::llm::is_guarded_thinking_mode(active_model, active_thinking) &&
                    batch_res.stop_reason != "refusal" &&
                    prompt_lines.size() >= vn::llm::kMinSubsequentBatchSliceForThoughtCheck &&
                    winning_thoughts <= 0;

                if (!batch_res.translations.empty() || batch_res.stop_reason == "refusal") {
                    circuit_breaker.record_top_level_batch_completed();
                }
                batch_thoughts = batch_res.usage.thought_tokens;

                const double spend_usd =
                    vn::llm::estimate_cost_usd(cumulative_usage, active_model);
                const double spend_eur =
                    vn::llm::estimate_cost_eur(cumulative_usage, active_model);

                bj::object telemetry;
                telemetry["model"] = active_model;
                telemetry["thinking_level"] = active_thinking;
                telemetry["batch_index"] = batch_index;
                telemetry["estimated_total_batches"] =
                    static_cast<std::int64_t>(est_total_batches);
                telemetry["batch_lines"] = static_cast<std::int64_t>(prompt_lines.size());
                telemetry["stop_reason"] = batch_res.stop_reason;
                telemetry["input_tokens"] = batch_res.usage.input_tokens;
                telemetry["output_tokens"] = batch_res.usage.output_tokens;
                telemetry["thought_tokens"] = batch_res.usage.thought_tokens;
                telemetry["cache_read_input_tokens"] =
                    batch_res.usage.cache_read_input_tokens;
                telemetry["cumulative_thought_tokens"] = cumulative_usage.thought_tokens;
                telemetry["cumulative_cost_usd"] = spend_usd;
                telemetry["cumulative_cost_eur"] = spend_eur;
                write_file_text(telemetry_path.u8string(),
                                json_pretty(bj::value(std::move(telemetry)),
                                            kDefaultPrettyIndent));

                if (refuse_zero_thought_cache) {
                    log_info("    [WARN] Top-level batch returned 0 thought tokens "
                             "(strike 1/2) -- refusing to cache; lines stay queued "
                             "for resume");
                    for (std::size_t k = batch_start; k < batch_end; ++k) {
                        const std::string jp(work[k]->at("content").get_string());
                        if (const std::string* cached_en = lookup_cached(jp)) {
                            previous_context.push_back(*cached_en);
                        } else {
                            previous_context.push_back(jp);
                        }
                    }
                } else {
                    auto& translations = batch_res.translations;
                    if (translations.size() < api_lines.size())
                        log_info("    WARNING: got " +
                                 std::to_string(translations.size()) + "/" +
                                 std::to_string(api_lines.size()) + " translations");

                    std::vector<std::string> resolved_en(api_lines.size());
                    for (std::size_t j = 0; j < api_lines.size(); ++j) {
                        auto it = translations.find(static_cast<int>(j) + 1);
                        const bool has_embedded_nul =
                            it != translations.end() &&
                            it->second.find('\0') != std::string::npos;
                        if (has_embedded_nul) {
                            log_info("    [WARN] Embedded NUL byte in line " +
                                     std::to_string(j + 1) + " -- leaving it queued");
                            continue;
                        }
                        if (it != translations.end()) {
                            resolved_en[j] = strip_speaker_tags(it->second);
                        }
                        if (resolved_en[j].empty()) {
                            log_info("    Missing translation for line " +
                                     std::to_string(j + 1) + " -- leaving it queued");
                            continue;
                        }
                        if (jp_char_count(resolved_en[j]) > 0) {
                            log_info("    [WARN] Leaked Japanese characters in line " +
                                     std::to_string(j + 1) + " -- leaving it queued");
                            resolved_en[j].clear();
                            continue;
                        }
                        if (!is_ascii_clean(resolved_en[j])) {
                            log_info("    [WARN] Non-ASCII characters in line " +
                                     std::to_string(j + 1) + " -- leaving it queued");
                            resolved_en[j].clear();
                            continue;
                        }
                        cache.set(api_lines[j], resolved_en[j]);
                    }

                    std::size_t previewed = 0;
                    for (std::size_t k = batch_start; k < batch_end; ++k) {
                        bj::object* ln = work[k];
                        const std::string jp(ln->at("content").get_string());
                        auto uit = unique_idx.find(jp);
                        if (uit != unique_idx.end()) {
                            const std::string& en = resolved_en[uit->second];
                            if (en.empty()) {
                                previous_context.push_back(jp);
                                continue;
                            }
                            previous_context.push_back(en);
                            ++translated_now;
                            if (previewed < kMaxSamplePreviewLines) {
                                std::string snippet = en;
                                if (snippet.size() > kMaxSamplePreviewChars) {
                                    snippet.resize(kMaxSamplePreviewChars);
                                    snippet += "...";
                                }
                                log_info("    [sample " +
                                         std::to_string(previewed + 1u) + "] " +
                                         snippet);
                                ++previewed;
                            }
                            bj::object entry;
                            entry["line_idx"] = ln->at("idx");
                            entry["offset"] = ln->at("offset");
                            entry["japanese"] = jp;
                            entry["translated"] = en;
                            batch_log.append(std::move(entry));
                            continue;
                        }
                        if (const std::string* cached_en = lookup_cached(jp)) {
                            previous_context.push_back(*cached_en);
                        }
                    }
                }
            } catch (const ZeroThoughtError&) {
                return kRuntimeFailureExitCode;
            } catch (const gemini::RateLimitError&) {
                // Back off, then move on to the next batch -- this one is
                // picked up again by the next resume run.
                log_info("    Rate limited, waiting 60s...");
                std::this_thread::sleep_for(std::chrono::seconds(kRateLimitSleepSeconds));
                continue;
            } catch (const std::exception& e) {
                // Nothing is written: the batch stays uncached, so the next
                // run asks for exactly these lines again.
                log_info(std::string("    Batch error: ") + e.what());
            }

            if (previous_context.size() > CONTEXT_WINDOW * kContextRetentionMultiplier)
                previous_context.erase(
                    previous_context.begin(),
                    previous_context.end() - CONTEXT_WINDOW * kContextRetentionMultiplier);
            save_cache(cache, opt.cache_file);
            done += translated_now;
            batch_log.next_batch();

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double rate = elapsed > 0.0 ? static_cast<double>(done) / elapsed : 0.0;
            const double total_usd =
                vn::llm::estimate_cost_usd(cumulative_usage, active_model);
            const double total_eur =
                vn::llm::estimate_cost_eur(cumulative_usage, active_model);
            char buf[kProgressBufferBytes];
            std::snprintf(
                buf, sizeof(buf),
                "  [Batch %d/%zu] +%lld lines (%lld/%zu, cache=%zu) | %.0fs | "
                "thoughts=%lld (cum=%lld) | spend=$%.3f (EUR %.3f) | %.1f l/s",
                batch_index, est_total_batches, translated_now, done, uncached,
                cache.size(), elapsed, static_cast<long long>(batch_thoughts),
                static_cast<long long>(cumulative_usage.thought_tokens),
                total_usd, total_eur, rate);
            log_info(buf);

            if (stopped_by_budget) {
                log_info("[BUDGET] Reached --max-budget-eur cap -- stopping cleanly "
                         "with cache saved.");
                break;
            }

            if (opt.test_mode) {
                --batches_remaining;
                if (batches_remaining <= 0) {
                    log_info("\n  --test mode: stopping after " +
                             std::to_string(opt.num_batches) + " batch(es)");
                    break;
                }
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done. Cache: " + std::to_string(cache.size()) + " entries");
    }

    // Build final translated_text.json
    log_info("\nBuilding translated_text.json...");
    for (auto& lv : all_lines) {
        auto& line = lv.get_object();
        std::string c(line.at("content").get_string());
        const std::string* en = lookup_cached(c);
        line["translated"] = (en && *en != c) ? strip_speaker_tags(*en) : c;
    }

    write_file(opt.output_file, json_dump(all_scripts_v));
    char sz[kSizeBufferBytes];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) /
                      kBytesPerKilobyte / kBytesPerKilobyte);
    log_info("Saved " + opt.output_file + " (" + sz + ")");

    // The table the game loads is derived from the document that was just
    // written, so it is built here rather than by a second command someone has
    // to remember: a translation the game never sees is not a translation.
    print_line("");
    log_info("Building the runtime translation table...");
    return build_tsv::run_build(opt.output_file, opt.cache_file, opt.tsv_file);
}

}  // namespace fav::translate
