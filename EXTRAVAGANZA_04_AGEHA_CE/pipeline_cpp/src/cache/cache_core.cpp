// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "cache/cache_core.h"

#include "build_tsv/build_tsv.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"

#include <boost/regex.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace exm::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr char32_t kHiraganaMin = 0x3040;
constexpr char32_t kHiraganaMax = 0x309F;
constexpr char32_t kKatakanaMin = 0x30A0;
constexpr char32_t kKatakanaMax = 0x30FF;
constexpr char32_t kCjkUnifiedMin = 0x4E00;
constexpr char32_t kCjkUnifiedMax = 0x9FFF;
constexpr char32_t kCjkExtAMin = 0x3400;
constexpr char32_t kCjkExtAMax = 0x4DBF;

constexpr std::size_t kMinNonJpTranslatableLen = 3;
constexpr std::size_t kMaxSpeakerExtractLen = 20;
constexpr std::size_t kMaxSpeakerPlateReattachLen = 10;
constexpr std::size_t kCrlfLen = 2;
constexpr unsigned char kNonAsciiMin = 0x80;

constexpr std::size_t kContextLogLimit = 120;
constexpr std::size_t kPromptContextLimit = 100;
constexpr std::size_t kBatchLogPreviewLimit = 60;
constexpr int kHttpOkStatus = 200;
constexpr int kCacheJsonIndent = 2;
constexpr int kTranslatedJsonIndent = 1;
constexpr int kRefusedExitCode = 2;
constexpr std::size_t kProgressBufSize = 160;
constexpr std::size_t kFileSizeBufSize = 64;
constexpr double kBytesPerKilobyte = 1024.0;
constexpr std::size_t kPercentScale = 100;
constexpr std::size_t kFallbackSubBatchSize = 25;
constexpr std::size_t kBinarySplitDivisor = 2;
constexpr std::size_t kMaxSubBatchDepth = 2;

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
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

std::string flatten_for_log(const std::string& txt, std::size_t limit) {
    const std::string flat = replace_all(txt, "\r\n", "|");
    std::string out;
    std::size_t i = 0;
    std::size_t n = 0;
    while (i < flat.size() && n < limit) {
        const std::size_t start = i;
        utf8_next(flat, i);
        out.append(flat, start, i - start);
        ++n;
    }
    return out;
}

void print_context_window(
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    const std::size_t n = std::min<std::size_t>(
        previous_context.size(), static_cast<std::size_t>(CONTEXT_WINDOW));
    log_info("\n=== ROLLING CONTEXT WINDOW (last " + std::to_string(n) + " of " +
             std::to_string(previous_context.size()) + ") ===");
    if (n == 0) {
        log_info("  (empty -- start of file)");
    } else {
        for (std::size_t i = previous_context.size() - n; i < previous_context.size(); ++i) {
            log_info("  [" + previous_context[i].first + "] " +
                     flatten_for_log(previous_context[i].second, kContextLogLimit));
        }
    }
    log_info("=== END CONTEXT ===\n");
}

struct Pending {
    std::size_t str_idx;
    std::string type;
    std::string speaker;
    std::string text;
};

struct SliceEntry {
    std::size_t k_idx;
    std::size_t str_idx;
    std::string type;
    std::string speaker;
    std::string text;
    bool was_cached;
};

void accumulate_usage(GeminiUsage& dst, const GeminiUsage& src) noexcept {
    dst.input_tokens += src.input_tokens;
    dst.output_tokens += src.output_tokens;
    dst.thought_tokens += src.thought_tokens;
    dst.cached_tokens += src.cached_tokens;
}

GeminiUsage from_llm_usage(const vn::llm::LlmTokenUsage& u) noexcept {
    GeminiUsage out;
    out.input_tokens = u.prompt_tokens;
    out.output_tokens = u.candidate_tokens;
    out.thought_tokens = u.thought_tokens;
    out.cached_tokens = u.cached_tokens;
    return out;
}

vn::llm::LlmRequest make_batch_llm_request(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after,
    std::string_view model,
    std::string_view thinking_level) {
    vn::llm::LlmRequest req;
    req.model = std::string(model);
    req.system_prompt = SYSTEM_PROMPT;
    req.user_prompt = build_user_prompt(batch_lines, previous_context, context_after);
    req.max_output_tokens = compute_max_output_tokens(batch_lines.size());
    req.thinking_level = std::string(thinking_level);
    req.enforce_min_output_floor = true;
    req.safety_block_none = true;
    return req;
}

GeminiBatchResult invoke_gemini_slice(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after,
    std::string_view model,
    std::string_view thinking_level,
    bool is_top_level,
    vn::llm::ZeroThoughtCircuitBreaker& breaker) {
    vn::llm::ensure_supported_model(model);
    const vn::llm::LlmRequest req = make_batch_llm_request(
        batch_lines, previous_context, context_after, model, thinking_level);
    const vn::llm::LlmResponse resp = client.generate(req);

    const GeminiUsage usage = from_llm_usage(resp.usage);
    log_info("    Tokens: in=" + std::to_string(usage.input_tokens) +
             " out=" + std::to_string(usage.output_tokens) +
             " thoughts=" + std::to_string(usage.thought_tokens) +
             " cached=" + std::to_string(usage.cached_tokens));

    breaker.check_response(model, thinking_level, is_top_level,
                           batch_lines.size(), resp.content_blocked,
                           usage.thought_tokens);

    std::string response_text = trim(resp.text);
    if (!response_text.empty() && response_text.rfind("1.", 0) != 0 &&
        response_text.rfind("1)", 0) != 0 && response_text.rfind("1:", 0) != 0) {
        response_text = "1. " + response_text;
    }

    log_info("\n--- GEMINI'S RAW ANSWER ---");
    log_info(response_text);
    log_info("---------------------------\n");

    GeminiBatchResult result;
    result.translations = parse_numbered_response(response_text);
    result.usage = usage;
    result.blocked = resp.content_blocked;
    return result;
}

std::vector<std::pair<std::string, std::string>> build_sub_batch_lookahead(
    const std::vector<std::pair<std::string, std::string>>& lines,
    std::size_t sub_end,
    const std::vector<std::pair<std::string, std::string>>& outer_after) {
    const std::size_t max_after = static_cast<std::size_t>(CONTEXT_AFTER);
    std::vector<std::pair<std::string, std::string>> after;
    after.reserve(max_after);
    for (std::size_t i = sub_end; i < lines.size() && after.size() < max_after; ++i) {
        after.push_back(lines[i]);
    }
    for (std::size_t i = 0; i < outer_after.size() && after.size() < max_after; ++i) {
        after.push_back(outer_after[i]);
    }
    return after;
}

GeminiBatchResult translate_sub_batches_recursive(
    gemini::Client& api,
    const std::vector<std::pair<std::string, std::string>>& lines,
    const std::vector<std::pair<std::string, std::string>>& outer_context_after,
    std::size_t depth,
    vn::llm::ZeroThoughtCircuitBreaker& breaker) {
    GeminiBatchResult combined;
    if (lines.empty()) return combined;

    const std::size_t chunk_size =
        (lines.size() > kFallbackSubBatchSize)
            ? kFallbackSubBatchSize
            : std::max<std::size_t>(1, lines.size() / kBinarySplitDivisor);

    for (std::size_t start = 0; start < lines.size(); start += chunk_size) {
        const std::size_t end = std::min(lines.size(), start + chunk_size);
        const std::vector<std::pair<std::string, std::string>> sub_lines(
            lines.begin() + static_cast<std::ptrdiff_t>(start),
            lines.begin() + static_cast<std::ptrdiff_t>(end));
        const auto sub_after =
            build_sub_batch_lookahead(lines, end, outer_context_after);

        GeminiBatchResult sub_res = invoke_gemini_slice(
            api, sub_lines, {}, sub_after, MODEL, kDefaultThinkingLevel, false, breaker);
        accumulate_usage(combined.usage, sub_res.usage);
        if (sub_res.translations.empty() && sub_lines.size() > 1 &&
            depth < kMaxSubBatchDepth) {
            GeminiBatchResult split_res = translate_sub_batches_recursive(
                api, sub_lines, sub_after, depth + 1, breaker);
            accumulate_usage(combined.usage, split_res.usage);
            sub_res.translations = std::move(split_res.translations);
        }
        for (const auto& [sub_idx, text] : sub_res.translations) {
            if (sub_idx >= 1 && static_cast<std::size_t>(sub_idx) <= sub_lines.size()) {
                combined.translations[static_cast<int>(start) + sub_idx] = text;
            }
        }
    }
    return combined;
}

GeminiBatchResult call_gemini_with_fallback(
    gemini::Client& api,
    const std::vector<std::pair<std::string, std::string>>& api_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after,
    vn::llm::ZeroThoughtCircuitBreaker& breaker) {
    if (api_lines.empty()) return {};

    GeminiBatchResult first = invoke_gemini_slice(
        api, api_lines, previous_context, context_after, MODEL,
        kDefaultThinkingLevel, true, breaker);
    if (!first.translations.empty()) {
        breaker.record_top_level_batch_completed();
        return first;
    }

    GeminiUsage total_usage = first.usage;
    if (!previous_context.empty() || !context_after.empty()) {
        log_info("    Empty response with context window -- retrying batch without context");
        GeminiBatchResult retry = invoke_gemini_slice(
            api, api_lines, {}, {}, MODEL, kDefaultThinkingLevel, true, breaker);
        accumulate_usage(total_usage, retry.usage);
        if (!retry.translations.empty()) {
            breaker.record_top_level_batch_completed();
            retry.usage = total_usage;
            return retry;
        }
    }

    if (api_lines.size() <= 1) {
        breaker.record_top_level_batch_completed();
        first.usage = total_usage;
        return first;
    }
    log_info("    Batch empty/refused -- splitting into sub-batches...");
    GeminiBatchResult split = translate_sub_batches_recursive(
        api, api_lines, context_after, 0, breaker);
    breaker.record_top_level_batch_completed();
    accumulate_usage(split.usage, total_usage);
    return split;
}

std::pair<long long, bool> translate_file(gemini::LazyClient& client,
                                          const std::string& fname,
                                          const bj::object& fdata,
                                          Cache& cache,
                                          int batch_size,
                                          const std::string& cache_file,
                                          int batch_cap,
                                          int& api_batches_done,
                                          bj::array* log_entries,
                                          TranslateTelemetry* telemetry,
                                          vn::llm::ZeroThoughtCircuitBreaker& breaker) {
    const std::vector<std::size_t> translatable = translatable_indices(fdata);
    if (translatable.empty()) return {0, false};

    const auto& strings = fdata.at("strings").get_array();
    std::vector<std::pair<std::string, std::string>> previous_context;
    long long translated_count = 0;
    bool stopped = false;

    for (std::size_t batch_start = 0; batch_start < translatable.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
        const std::size_t batch_end = std::min(
            translatable.size(), batch_start + static_cast<std::size_t>(batch_size));

        std::vector<SliceEntry> slice_entries;
        slice_entries.reserve(batch_end - batch_start);
        std::optional<std::size_t> first_uncached_offset;
        std::size_t last_uncached_k = batch_start;

        for (std::size_t k = batch_start; k < batch_end; ++k) {
            const std::size_t idx = translatable[k];
            const auto& s = strings[idx].get_object();
            const std::string text(s.at("text").get_string());
            std::string type;
            if (const auto* t = s.if_contains("type")) {
                if (t->is_string()) type = std::string(t->get_string());
            }
            const std::string speaker =
                type == "dialogue" ? extract_speaker(text) : std::string("NARRATION");
            const bool is_cached = cache.contains(text);
            if (!is_cached) {
                if (!first_uncached_offset.has_value()) {
                    first_uncached_offset = slice_entries.size();
                }
                last_uncached_k = k;
            }
            slice_entries.push_back(SliceEntry{k, idx, type, speaker, text, is_cached});
        }

        if (!first_uncached_offset.has_value()) {
            for (const auto& entry : slice_entries) {
                if (const std::string* en = cache.get(entry.text)) {
                    previous_context.emplace_back(entry.speaker, *en);
                    ++translated_count;
                }
            }
            continue;
        }

        for (std::size_t i = 0; i < *first_uncached_offset; ++i) {
            const auto& entry = slice_entries[i];
            if (const std::string* en = cache.get(entry.text)) {
                previous_context.emplace_back(entry.speaker, *en);
                ++translated_count;
            }
        }

        std::vector<Pending> needs_api;
        for (std::size_t i = *first_uncached_offset; i < slice_entries.size(); ++i) {
            const auto& entry = slice_entries[i];
            if (!entry.was_cached) {
                needs_api.push_back(
                    Pending{entry.str_idx, entry.type, entry.speaker, entry.text});
            }
        }

        gemini::Client& api = client.get();

        ++api_batches_done;
        log_info("\n--- API batch #" + std::to_string(api_batches_done) + " (" +
                 std::to_string(needs_api.size()) + " new lines) ---");
        print_context_window(previous_context);

        std::vector<std::pair<std::string, std::string>> api_lines;
        api_lines.reserve(needs_api.size());
        for (const auto& p : needs_api) api_lines.emplace_back(p.speaker, p.text);

        std::vector<std::pair<std::string, std::string>> context_after;
        const std::size_t after_start = last_uncached_k + 1;
        const std::size_t after_end = std::min(
            translatable.size(),
            after_start + static_cast<std::size_t>(CONTEXT_AFTER));
        for (std::size_t k = after_start; k < after_end; ++k) {
            const std::size_t idx = translatable[k];
            const auto& s = strings[idx].get_object();
            const std::string text(s.at("text").get_string());
            std::string type;
            if (const auto* t = s.if_contains("type")) {
                if (t->is_string()) type = std::string(t->get_string());
            }
            const std::string speaker =
                type == "dialogue" ? extract_speaker(text) : std::string("NARRATION");
            if (const std::string* en = cache.get(text)) {
                context_after.emplace_back(speaker, *en);
            } else {
                context_after.emplace_back(speaker, text);
            }
        }

        auto apply = [&](const std::map<int, std::string>& translations, bool verbose) {
            for (std::size_t i = 0; i < needs_api.size(); ++i) {
                const auto& p = needs_api[i];
                const auto it = translations.find(static_cast<int>(i) + 1);
                if (it != translations.end() && !it->second.empty()) {
                    const std::string tr = postprocess(p.text, it->second);
                    cache.set(p.text, tr);
                    if (verbose) {
                        log_info("  JP: " + flatten_for_log(p.text, kBatchLogPreviewLimit));
                        log_info("  EN: " + flatten_for_log(tr, kBatchLogPreviewLimit));
                    }
                    if (log_entries) {
                        bj::object e;
                        e["script"] = fname;
                        e["line_idx"] = static_cast<std::int64_t>(p.str_idx);
                        e["type"] = p.type;
                        e["speaker"] = p.speaker;
                        e["japanese"] = p.text;
                        e["translated"] = tr;
                        log_entries->push_back(std::move(e));
                    }
                    continue;
                }
                if (verbose) {
                    log_info("    Missing translation for line " + std::to_string(i + 1) +
                             ", keeping original");
                }
            }
        };

        GeminiUsage batch_usage;
        try {
            const GeminiBatchResult batch_res = call_gemini_with_fallback(
                api, api_lines, previous_context, context_after, breaker);
            batch_usage = batch_res.usage;
            if (batch_res.translations.size() < needs_api.size()) {
                log_info("    WARNING: got " +
                         std::to_string(batch_res.translations.size()) + "/" +
                         std::to_string(needs_api.size()) + " translations");
            }
            apply(batch_res.translations, true);
        } catch (const ZeroThoughtError&) {
            throw;
        } catch (const std::exception& e) {
            log_info(std::string("    Batch error: ") + e.what());
        }

        for (std::size_t i = *first_uncached_offset; i < slice_entries.size(); ++i) {
            const auto& entry = slice_entries[i];
            if (const std::string* en = cache.get(entry.text)) {
                previous_context.emplace_back(entry.speaker, *en);
                ++translated_count;
            } else {
                previous_context.emplace_back(entry.speaker, entry.text);
            }
        }

        if (telemetry != nullptr) {
            accumulate_usage(telemetry->total_usage, batch_usage);
            telemetry->api_batches = api_batches_done;
        }

        constexpr std::size_t kContextRetentionMultiplier = 2;
        const std::size_t max_context_keep =
            static_cast<std::size_t>(CONTEXT_WINDOW) * kContextRetentionMultiplier;
        if (previous_context.size() > max_context_keep) {
            previous_context.erase(
                previous_context.begin(),
                previous_context.end() - static_cast<std::ptrdiff_t>(max_context_keep));
        }

        save_cache(cache, cache_file);

        try {
            const fs::path last_dir =
                fs::u8path(cache_file).parent_path() / "last_gemini_translate";
            fs::create_directories(last_dir);
            bj::array batch_arr;
            for (std::size_t i = 0; i < needs_api.size(); ++i) {
                const auto& p = needs_api[i];
                bj::object row;
                row["script"] = fname;
                row["line_idx"] = static_cast<std::int64_t>(p.str_idx);
                row["speaker"] = p.speaker;
                row["jp"] = p.text;
                if (const std::string* en = cache.get(p.text)) {
                    row["en"] = *en;
                } else {
                    row["en"] = nullptr;
                }
                batch_arr.push_back(std::move(row));
            }
            bj::object last_doc;
            last_doc["batch_number"] = api_batches_done;
            last_doc["script"] = fname;
            last_doc["line_count"] = static_cast<std::int64_t>(needs_api.size());
            last_doc["input_tokens"] = batch_usage.input_tokens;
            last_doc["output_tokens"] = batch_usage.output_tokens;
            last_doc["thought_tokens"] = batch_usage.thought_tokens;
            last_doc["cached_tokens"] = batch_usage.cached_tokens;
            last_doc["lines"] = std::move(batch_arr);
            write_file_atomic_text(
                (last_dir / "last_translation.json").u8string(),
                json_pretty(bj::value(std::move(last_doc)), kCacheJsonIndent));
        } catch (const std::exception&) {
        }

        if (batch_cap && api_batches_done >= batch_cap) {
            stopped = true;
            break;
        }
    }

    return {translated_count, stopped};
}

}  // namespace

struct Cache::Impl {
    std::vector<std::pair<std::string, std::string>> order;
    std::map<std::string, std::size_t> index;
};

Cache::Cache() : impl_(std::make_unique<Impl>()) {}
Cache::~Cache() = default;
Cache::Cache(const Cache& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Cache& Cache::operator=(const Cache& other) {
    if (this != &other) *impl_ = *other.impl_;
    return *this;
}
Cache::Cache(Cache&& other) noexcept = default;
Cache& Cache::operator=(Cache&& other) noexcept = default;

bool Cache::contains(const std::string& jp) const {
    return impl_->index.count(jp) != 0;
}

const std::string* Cache::get(const std::string& jp) const {
    const auto it = impl_->index.find(jp);
    return it == impl_->index.end() ? nullptr : &impl_->order[it->second].second;
}

void Cache::set(const std::string& jp, const std::string& en) {
    const auto it = impl_->index.find(jp);
    if (it != impl_->index.end()) {
        impl_->order[it->second].second = en;
        return;
    }
    impl_->index.emplace(jp, impl_->order.size());
    impl_->order.emplace_back(jp, en);
}

std::size_t Cache::size() const {
    return impl_->order.size();
}

const std::vector<std::pair<std::string, std::string>>& Cache::items() const {
    return impl_->order;
}

std::string strip(const std::string& s) {
    return trim(s);
}

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches) {
    if (batch_size < 1) {
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    }
    if (test_n < 0) {
        return "--test cannot be negative (got " + std::to_string(test_n) + ")";
    }
    if (max_batches < 0) {
        return "--max-batches cannot be negative (got " + std::to_string(max_batches) +
               "); use 0 for no limit";
    }
    return std::nullopt;
}

bool has_real_japanese(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t cp = utf8_next(text, i);
        if ((cp >= kHiraganaMin && cp <= kHiraganaMax) ||
            (cp >= kKatakanaMin && cp <= kKatakanaMax) ||
            (cp >= kCjkUnifiedMin && cp <= kCjkUnifiedMax) ||
            (cp >= kCjkExtAMin && cp <= kCjkExtAMax)) {
            return true;
        }
    }
    return false;
}

bool needs_translation(const std::string& text) {
    if (text.empty() || trim(text).empty()) return false;
    if (char_len(text) <= kMinNonJpTranslatableLen && !has_real_japanese(text)) return false;
    return has_real_japanese(text);
}

std::vector<std::size_t> translatable_indices(const bj::object& fdata) {
    std::vector<std::size_t> out;
    const auto* strings = fdata.if_contains("strings");
    if (!strings || !strings->is_array()) return out;
    const auto& arr = strings->get_array();
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& s = arr[i].get_object();
        std::string text;
        std::string type;
        if (const auto* t = s.if_contains("text")) {
            if (t->is_string()) text = std::string(t->get_string());
        }
        if (const auto* t = s.if_contains("type")) {
            if (t->is_string()) type = std::string(t->get_string());
        }
        if (needs_translation(text) &&
            (type == "dialogue" || type == "narrative" || type == "menu")) {
            out.push_back(i);
        }
    }
    return out;
}

std::string extract_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf == std::string::npos) return "NARRATION";
    const std::string name = trim(text.substr(0, crlf));
    const std::string rest = trim_left(text.substr(crlf + kCrlfLen));
    if (name.empty() || char_len(name) > kMaxSpeakerExtractLen || rest.empty()) {
        return "NARRATION";
    }
    if (rest.rfind("「", 0) != 0 && rest.rfind("『", 0) != 0) {
        return "NARRATION";
    }
    const auto& map = name_translations();
    const auto it = map.find(name);
    return it == map.end() ? name : it->second;
}

std::string sanitize_ascii(const std::string& text) {
    std::string s = text;
    s = replace_all(s, "—", "--");
    s = replace_all(s, "–", "--");
    s = replace_all(s, "…", "...");
    s = replace_all(s, "‘", "'");
    s = replace_all(s, "’", "'");
    s = replace_all(s, "“", "\"");
    s = replace_all(s, "”", "\"");
    s = replace_all(s, "　", " ");
    return s;
}

std::string postprocess(const std::string& original, const std::string& translated) {
    if (translated.empty() || translated == original) return translated;
    std::string t = sanitize_ascii(translated);

    const std::size_t crlf = original.find("\r\n");
    if (crlf != std::string::npos) {
        const std::string jp_name = trim(original.substr(0, crlf));
        if (!jp_name.empty() && char_len(jp_name) <= kMaxSpeakerPlateReattachLen) {
            const auto& map = name_translations();
            const auto it = map.find(jp_name);
            if (it != map.end()) {
                const std::string& en_name = it->second;
                if (t.rfind(en_name + "\r\n", 0) != 0) {
                    if (t.rfind(jp_name + "\r\n", 0) == 0) {
                        t = t.substr(jp_name.size() + kCrlfLen);
                    }
                    t = en_name + "\r\n" + t;
                }
            }
        }
    }
    const bool orig_crlf_end =
        original.size() >= kCrlfLen &&
        original.compare(original.size() - kCrlfLen, kCrlfLen, "\r\n") == 0;
    const bool tr_crlf_end =
        t.size() >= kCrlfLen && t.compare(t.size() - kCrlfLen, kCrlfLen, "\r\n") == 0;
    if (orig_crlf_end && !tr_crlf_end) t = trim_right(t) + "\r\n";
    return t;
}

Cache load_cache(const std::string& cache_file) {
    Cache cache;
    if (!fs::exists(fs::u8path(cache_file))) return cache;
    const bj::value root = json_parse_file(cache_file);
    if (!root.is_object()) {
        throw std::runtime_error("cannot parse " + cache_file +
                                 ": a translation cache must be a JSON object");
    }
    for (const auto& kv : root.get_object()) {
        if (kv.value().is_string()) {
            cache.set(std::string(kv.key()), std::string(kv.value().get_string()));
        }
    }
    return cache;
}

std::size_t cache_entry_count(const std::string& cache_file) {
    return load_cache(cache_file).size();
}

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache) {
    if (discard_cache || cache_entries <= CACHE_DISCARD_THRESHOLD) return std::nullopt;
    return flag + " discards the translation cache, which holds " +
           std::to_string(cache_entries) +
           " lines that were paid for. Nothing in this pipeline can rebuild "
           "them.\n"
           "        To cap a run and keep them, use --max-batches N instead.\n"
           "        To delete them anyway, add --discard-cache.";
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items()) o[jp] = en;
    write_file_atomic_text(cache_file, json_pretty(bj::value(std::move(o)), kCacheJsonIndent));
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    if (jp != en) return false;
    for (char c : jp) {
        if (static_cast<unsigned char>(c) >= kNonAsciiMin) return true;
    }
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

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        const std::size_t window = static_cast<std::size_t>(CONTEXT_WINDOW);
        const std::size_t start = previous_context.size() > window
                                      ? previous_context.size() - window
                                      : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i) {
            user_prompt += "[" + previous_context[i].first + "] " +
                           flatten_for_log(previous_context[i].second, kPromptContextLimit) + "\n";
        }
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i) {
        std::string escaped = replace_all(batch_lines[i].second, "\r\n", "\\r\\n");
        escaped = replace_all(escaped, "\n", "\\n");
        user_prompt +=
            std::to_string(i + 1) + ". [" + batch_lines[i].first + "] " + escaped + "\n";
    }
    user_prompt += "</lines_to_translate>\n\n";
    if (!context_after.empty()) {
        user_prompt += "<context_after>\n";
        const std::size_t window = static_cast<std::size_t>(CONTEXT_AFTER);
        const std::size_t count = std::min(context_after.size(), window);
        for (std::size_t i = 0; i < count; ++i) {
            user_prompt += "[" + context_after[i].first + "] " +
                           flatten_for_log(context_after[i].second, kPromptContextLimit) + "\n";
        }
        user_prompt += "</context_after>\n\n";
    }
    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use the <previous_context> and <context_after> (if present) for scene continuity "
        "but do NOT translate them. "
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line. "
        "Do NOT include [SPEAKER] tags in your output.";
    return user_prompt;
}

std::map<int, std::string> parse_numbered_response(const std::string& response_text) {
    static const boost::regex tag_re(R"(^(?:\[(?:NARRATION|[A-Za-z0-9\s()'"?&$.,!_/-]+)\]\s*)+)");
    static const boost::regex num_re(R"(^(\d+)[.):\s]+\s*(.*))");
    std::map<int, std::string> translations;

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        const std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty()) continue;
        line = boost::regex_replace(line, tag_re, "");
        boost::smatch m;
        if (boost::regex_match(line, m, num_re)) {
            std::string text = trim(boost::regex_replace(trim(m[2].str()), tag_re, ""));
            text = replace_all(text, "\\r\\n", "\r\n");
            text = replace_all(text, "\\n", "\n");
            translations[std::stoi(m[1].str())] = text;
        }
    }
    return translations;
}

std::int64_t compute_max_output_tokens(std::size_t batch_line_count) noexcept {
    return vn::llm::compute_batch_max_output_tokens(
        batch_line_count, vn::llm::kDefaultTokensPerBatchLine);
}

bj::object build_gemini_request_body(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after,
    std::string_view thinking_level) {
    const vn::llm::LlmRequest req = make_batch_llm_request(
        batch_lines, previous_context, context_after, MODEL, thinking_level);
    return bj::parse(vn::llm::build_gemini_request_json(req)).as_object();
}

ParsedGeminiResponse parse_gemini_response(const bj::value& response) {
    if (!response.is_object()) {
        return {};
    }
    try {
        const vn::llm::LlmResponse llm_resp =
            vn::llm::parse_gemini_response_json(bj::serialize(response), kHttpOkStatus);
        ParsedGeminiResponse parsed;
        parsed.candidate_text = llm_resp.text;
        parsed.usage = from_llm_usage(llm_resp.usage);
        parsed.blocked = llm_resp.content_blocked;
        parsed.finish_reason = llm_resp.finish_reason;
        if (const auto* fb = response.as_object().if_contains("promptFeedback");
            fb != nullptr && fb->is_object()) {
            if (const auto* br = fb->as_object().if_contains("blockReason");
                br != nullptr && br->is_string()) {
                parsed.block_reason = std::string(br->as_string());
            }
        }
        return parsed;
    } catch (const vn::llm::LlmError&) {
        return {};
    }
}

void validate_thought_telemetry(std::string_view model,
                                std::string_view thinking_level,
                                bool blocked,
                                std::size_t batch_line_count,
                                std::int64_t thought_tokens) {
    vn::llm::ZeroThoughtCircuitBreaker breaker;
    breaker.check_response(model, thinking_level, true, batch_line_count,
                           blocked, thought_tokens);
}

GeminiBatchResult call_gemini_detailed(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after,
    std::string_view model,
    std::string_view thinking_level) {
    vn::llm::ZeroThoughtCircuitBreaker breaker;
    return invoke_gemini_slice(client, batch_lines, previous_context,
                               context_after, model, thinking_level, true,
                               breaker);
}

std::map<int, std::string> call_gemini(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after) {
    return call_gemini_detailed(
               client, batch_lines, previous_context, context_after)
        .translations;
}

int translate_all(const TranslateOptions& opt, TranslateTelemetry* telemetry) {
    if (telemetry != nullptr) {
        *telemetry = TranslateTelemetry{};
    }
    log_info("Model: " + MODEL);
    log_info("Batch size: " + std::to_string(opt.batch_size) + " lines per request");
    log_info("Context window: " + std::to_string(CONTEXT_WINDOW) +
             " previous lines + " + std::to_string(CONTEXT_AFTER) + " lookahead lines");

    log_info("\nLoading extracted text...");
    bj::value all_data_v = json_parse_file(opt.input_file);
    bj::object& all_data = all_data_v.get_object();

    log_info("\n--- Speaker gate (no tokens spent) ---");
    const SpeakerCheckReport gate = run_speaker_checks(all_data);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return kRefusedExitCode;
    }

    gemini::LazyClient client;

    log_info("Loading translation cache...");
    Cache cache = load_cache(opt.cache_file);
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
        cache = Cache{};
    } else {
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
        const std::size_t purged = purge_failed_entries(cache);
        log_info("  Failed entries purged: " + std::to_string(purged) +
                 (purged ? "  (those lines re-queue)" : ""));
    }

    for (const auto& [jp, en] : name_translations_ordered()) cache.set(jp, en);

    std::vector<std::string> file_order;
    for (const auto& f : story_order()) {
        if (all_data.if_contains(f)) file_order.push_back(f);
    }
    for (const auto& kv : all_data) {
        const std::string name(kv.key());
        if (std::find(file_order.begin(), file_order.end(), name) == file_order.end()) {
            file_order.push_back(name);
        }
    }

    std::vector<std::tuple<std::string, std::size_t, std::size_t>> file_stats;
    std::size_t total_translatable = 0;
    std::size_t total_uncached = 0;
    for (const auto& fname : file_order) {
        if (opt.only_file && fname != *opt.only_file) continue;
        const auto& fdata = all_data.at(fname).get_object();
        const auto idxs = translatable_indices(fdata);
        const auto& strings = fdata.at("strings").get_array();
        std::size_t uncached = 0;
        for (std::size_t i : idxs) {
            if (!cache.contains(std::string(strings[i].get_object().at("text").get_string()))) {
                ++uncached;
            }
        }
        file_stats.emplace_back(fname, idxs.size(), uncached);
        total_translatable += idxs.size();
        total_uncached += uncached;
    }

    log_info("\nTranslation summary:");
    log_info("  Files to process: " + std::to_string(file_stats.size()));
    log_info("  Total translatable lines: " + std::to_string(total_translatable));
    log_info("  Need translating: " + std::to_string(total_uncached) +
             " (cached: " + std::to_string(total_translatable - total_uncached) + ")");

    if (total_uncached == 0) {
        log_info("  Everything is already cached!");
    } else {
        log_info("\n--- Translating in story order ---");
        std::size_t done_lines = 0;
        const auto start = std::chrono::steady_clock::now();

        const int batch_cap = opt.test_batches > 0 ? opt.test_batches : opt.max_batches;
        const char* cap_flag = opt.test_batches > 0 ? "--test " : "--max-batches ";
        int api_batches_done = 0;
        vn::llm::ZeroThoughtCircuitBreaker breaker;

        for (const auto& [fname, line_count, uncached_count] : file_stats) {
            if (uncached_count == 0) {
                done_lines += line_count;
                continue;
            }

            log_info("\n  [" + fname + "] " + std::to_string(uncached_count) + " uncached / " +
                     std::to_string(line_count) + " translatable");

            bj::array log_entries;
            const auto [count, stopped] = translate_file(
                client, fname, all_data.at(fname).get_object(), cache, opt.batch_size,
                opt.cache_file, batch_cap, api_batches_done,
                opt.test_batches ? &log_entries : nullptr, telemetry, breaker);
            (void)count;
            done_lines += line_count;

            if (opt.test_batches && !log_entries.empty()) {
                fs::create_directories(fs::u8path(opt.test_dir));
                int run_num = 1;
                while (fs::exists(fs::u8path(opt.test_dir + "\\translation_run_" +
                                             std::to_string(run_num) + ".json"))) {
                    ++run_num;
                }
                const std::string log_path =
                    opt.test_dir + "\\translation_run_" + std::to_string(run_num) + ".json";
                write_file_text(log_path, json_pretty(bj::value(log_entries), kCacheJsonIndent));
                log_info("  Test log: " + log_path + " (" +
                         std::to_string(log_entries.size()) + " entries)");
            }

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double rate = elapsed > 0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            char buf[kProgressBufSize];
            std::snprintf(buf, sizeof(buf), "  Progress: %zu/%zu (%zu%%) | %.1f l/s", done_lines,
                          total_translatable,
                          total_translatable ? done_lines * kPercentScale / total_translatable : 0,
                          rate);
            log_info(buf);

            if (stopped) {
                log_info("\n  " + std::string(cap_flag) + std::to_string(batch_cap) +
                         ": stopping after " + std::to_string(batch_cap) + " batch(es)");
                break;
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done! Cache: " + std::to_string(cache.size()) + " entries");
    }

    log_info("\nBuilding translated text...");
    for (auto& kv : all_data) {
        auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (auto& sv : strings->get_array()) {
            auto& s = sv.get_object();
            const std::string text(s.at("text").get_string());
            const std::string* en = cache.get(text);
            s["translated"] = (en && *en != text) ? *en : text;
        }
    }

    write_file_text(opt.output_file, json_pretty(all_data_v, kTranslatedJsonIndent));
    char sz[kFileSizeBufSize];
    const double mb = static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) /
                      kBytesPerKilobyte / kBytesPerKilobyte;
    std::snprintf(sz, sizeof(sz), "%.1f MB", mb);
    log_info("Saved to " + opt.output_file + " (" + sz + ")");

    log_info("\n--- Building the runtime translation table ---");
    return build_tsv::run_build(opt.output_file, opt.tsv_file);
}

}  // namespace exm::translate
