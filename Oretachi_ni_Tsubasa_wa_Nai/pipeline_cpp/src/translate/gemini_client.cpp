// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "translate/gemini_client.h"

#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#include <boost/regex.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace oretsuba::translate {

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr int kLiveZeroThoughtMaxRetries = 2;
constexpr int kZeroThoughtBackoffMultiplier = 2;
constexpr std::size_t kBinarySplitDivisor = 2U;
constexpr std::size_t kMaxSubBatchDepth = 2U;
constexpr int kNoActiveLineNumber = -1;
constexpr int kLineNumberCaptureGroup = 1;
constexpr int kLineBodyCaptureGroup = 2;

std::string ReplaceAllSubstring(std::string input,
                                std::string_view from,
                                std::string_view to) {
    if (from.empty()) {
        return input;
    }
    std::string out;
    out.reserve(input.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = input.find(from, pos);
        if (hit == std::string::npos) {
            out.append(input, pos, std::string::npos);
            return out;
        }
        out.append(input, pos, hit - pos);
        out.append(to.data(), to.size());
        pos = hit + from.size();
    }
}

std::string ReadKeyFromSettingsJson(const fs::path& settings_path) {
    std::error_code ec;
    if (!fs::is_regular_file(settings_path, ec)) {
        return {};
    }
    try {
        const bj::value val = JsonParseFile(settings_path.u8string());
        if (!val.is_object()) {
            return {};
        }
        const auto& obj = val.as_object();
        if (const auto* direct = obj.if_contains("GEMINI_API_KEY");
            direct != nullptr && direct->is_string()) {
            const std::string key = TrimUnicode(std::string(direct->as_string()));
            if (!key.empty()) {
                return key;
            }
        }
        if (const auto* env_val = obj.if_contains("env");
            env_val != nullptr && env_val->is_object()) {
            const auto& env_obj = env_val->as_object();
            if (const auto* nested = env_obj.if_contains("GEMINI_API_KEY");
                nested != nullptr && nested->is_string()) {
                const std::string key = TrimUnicode(std::string(nested->as_string()));
                if (!key.empty()) {
                    return key;
                }
            }
        }
    } catch (...) {
        return {};
    }
    return {};
}

std::map<int, std::string> SubBatchTranslateRecursive(
    ITranslationBackend& backend,
    const std::vector<std::string>& prompt_lines,
    std::size_t depth) {
    std::map<int, std::string> combined;
    if (prompt_lines.empty()) {
        return combined;
    }

    const std::size_t chunk_size =
        (prompt_lines.size() > kFallbackSubBatchSize)
            ? kFallbackSubBatchSize
            : std::max<std::size_t>(1U, prompt_lines.size() / kBinarySplitDivisor);

    for (std::size_t start = 0; start < prompt_lines.size(); start += chunk_size) {
        const std::size_t end = std::min(prompt_lines.size(), start + chunk_size);
        const std::vector<std::string> sub_lines(
            prompt_lines.begin() + static_cast<std::ptrdiff_t>(start),
            prompt_lines.begin() + static_cast<std::ptrdiff_t>(end));

        auto sub_res = backend.TranslateBatch(sub_lines, {}, {}, true);
        if (sub_res.empty() && sub_lines.size() > 1U && depth < kMaxSubBatchDepth) {
            sub_res = SubBatchTranslateRecursive(backend, sub_lines, depth + 1U);
        }
        for (const auto& [sub_idx, text] : sub_res) {
            if (sub_idx >= 1 && static_cast<std::size_t>(sub_idx) <= sub_lines.size()) {
                combined[static_cast<int>(start) + sub_idx] = text;
            }
        }
    }
    return combined;
}

std::string ExtractSystemInstructionText(const bj::object& in_obj) {
    const auto* sys = in_obj.if_contains("system");
    if (sys == nullptr) {
        return {};
    }
    if (sys->is_string()) {
        return std::string(sys->get_string());
    }
    if (!sys->is_array()) {
        return {};
    }
    std::string sys_text;
    for (const auto& item : sys->get_array()) {
        if (!item.is_object()) {
            continue;
        }
        const auto* txt = item.get_object().if_contains("text");
        if (txt == nullptr || !txt->is_string()) {
            continue;
        }
        if (!sys_text.empty()) {
            sys_text += '\n';
        }
        sys_text += std::string(txt->get_string());
    }
    return sys_text;
}

std::string ExtractUserPromptText(const bj::object& in_obj) {
    const auto* msgs = in_obj.if_contains("messages");
    if (msgs == nullptr || !msgs->is_array()) {
        return {};
    }
    std::string user_text;
    for (const auto& mv : msgs->get_array()) {
        if (!mv.is_object()) {
            continue;
        }
        const auto& mobj = mv.get_object();
        const auto* content_v = mobj.if_contains("content");
        if (content_v == nullptr) {
            continue;
        }
        if (content_v->is_string()) {
            if (!user_text.empty()) {
                user_text += '\n';
            }
            user_text += std::string(content_v->get_string());
            continue;
        }
        if (!content_v->is_array()) {
            continue;
        }
        for (const auto& bv : content_v->get_array()) {
            if (!bv.is_object()) {
                continue;
            }
            const auto* txt = bv.get_object().if_contains("text");
            if (txt == nullptr || !txt->is_string()) {
                continue;
            }
            if (!user_text.empty()) {
                user_text += '\n';
            }
            user_text += std::string(txt->get_string());
        }
    }
    return user_text;
}

vn::llm::LlmRequest BuildLlmRequestFromJson(
    const bj::value& body,
    std::string_view fallback_model = vn::llm::kDefaultModel) {
    const bj::object empty_obj;
    const bj::object& in_obj = body.is_object() ? body.get_object() : empty_obj;

    vn::llm::LlmRequest req;
    req.model = std::string(fallback_model);
    if (const auto* mdl = in_obj.if_contains("model");
        mdl != nullptr && mdl->is_string() && !mdl->get_string().empty()) {
        req.model = std::string(mdl->get_string());
    }
    vn::llm::ensure_supported_model(req.model);

    req.system_prompt = ExtractSystemInstructionText(in_obj);
    req.user_prompt = ExtractUserPromptText(in_obj);

    req.max_output_tokens = vn::llm::kMaxOutputTokensCeiling;
    if (const auto* mt = in_obj.if_contains("max_tokens"); mt != nullptr) {
        std::int64_t requested = 0;
        if (mt->is_int64()) {
            requested = mt->get_int64();
        }
        if (mt->is_uint64()) {
            const std::uint64_t raw_u64 = mt->get_uint64();
            requested = raw_u64 > static_cast<std::uint64_t>(vn::llm::kMaxOutputTokensCeiling)
                            ? vn::llm::kMaxOutputTokensCeiling
                            : static_cast<std::int64_t>(raw_u64);
        }
        if (requested > 0) {
            req.max_output_tokens = requested;
        }
    }

    req.thinking_level = std::string(vn::llm::kDefaultThinkingLevel);
    if (const auto* tl = in_obj.if_contains("thinking_level");
        tl != nullptr && tl->is_string() && !tl->get_string().empty()) {
        req.thinking_level = std::string(tl->get_string());
    }
    req.enforce_min_output_floor = true;
    req.safety_block_none = true;
    return req;
}

std::string DetermineStopReason(const vn::llm::LlmResponse& resp) {
    if (resp.content_blocked) {
        return "refusal";
    }
    if (resp.finish_reason == "MAX_TOKENS") {
        return "max_tokens";
    }
    if (resp.finish_reason.empty() || resp.finish_reason == "STOP") {
        return "end_turn";
    }
    return resp.finish_reason;
}

bj::object ConvertLlmResponseToJson(const vn::llm::LlmResponse& llm_resp,
                                    GeminiUsage* usage_out) {
    GeminiUsage usage;
    usage.input_tokens = llm_resp.usage.prompt_tokens;
    usage.candidates_tokens = llm_resp.usage.candidate_tokens;
    usage.thoughts_tokens = llm_resp.usage.thought_tokens;
    usage.output_tokens = llm_resp.usage.total_output_tokens();
    usage.cache_read_input_tokens = llm_resp.usage.cached_tokens;
    usage.total_tokens = usage.input_tokens + usage.output_tokens;

    if (usage_out != nullptr) {
        *usage_out = usage;
    }

    bj::object out;
    out["content"] = bj::array{
        bj::object{{"type", "text"}, {"text", llm_resp.text}},
    };
    out["stop_reason"] = DetermineStopReason(llm_resp);
    out["usage"] = bj::object{
        {"input_tokens", usage.input_tokens},
        {"candidates_tokens", usage.candidates_tokens},
        {"thoughts_tokens", usage.thoughts_tokens},
        {"thoughtsTokenCount", usage.thoughts_tokens},
        {"output_tokens", usage.output_tokens},
        {"total_tokens", usage.total_tokens},
        {"cache_read_input_tokens", usage.cache_read_input_tokens},
        {"cache_creation_input_tokens", 0},
    };
    out["usageMetadata"] = bj::object{
        {"promptTokenCount", usage.input_tokens},
        {"candidatesTokenCount", usage.candidates_tokens},
        {"thoughtsTokenCount", usage.thoughts_tokens},
        {"cachedContentTokenCount", usage.cache_read_input_tokens},
        {"totalTokenCount", usage.total_tokens},
    };
    return out;
}

void AccumulateUsage(GeminiUsage& total, const GeminiUsage& delta) noexcept {
    total.input_tokens += delta.input_tokens;
    total.candidates_tokens += delta.candidates_tokens;
    total.thoughts_tokens += delta.thoughts_tokens;
    total.output_tokens += delta.output_tokens;
    total.cache_read_input_tokens += delta.cache_read_input_tokens;
    total.total_tokens += delta.total_tokens;
}

class CallbackRawHttpTransport final : public vn::llm::IRawHttpTransport {
public:
    explicit CallbackRawHttpTransport(HttpTransportFn fn)
        : fn_(std::move(fn)) {}

    vn::llm::RawHttpResponse post_json(std::string_view target_path,
                                       std::string_view json_body,
                                       int /*connect_timeout_sec*/,
                                       int read_timeout_sec) override {
        const HttpResponse res = fn_(target_path, json_body, read_timeout_sec);
        return vn::llm::RawHttpResponse{
            static_cast<int>(res.status_code),
            res.body,
        };
    }

private:
    HttpTransportFn fn_;
};

}  // namespace

ITranslationBackend::~ITranslationBackend() = default;

std::string LoadGeminiApiKey(const std::string& project_dir) {
    if (std::string resolved = vn::llm::resolve_gemini_api_key(); !resolved.empty()) {
        return resolved;
    }

    std::vector<fs::path> settings_candidates;
    if (!project_dir.empty()) {
        settings_candidates.push_back(fs::u8path(project_dir) / ".claude" / "settings.json");
        settings_candidates.push_back(
            fs::u8path(project_dir).parent_path() / ".claude" / "settings.json");
    }
    settings_candidates.push_back(fs::u8path(".claude/settings.json"));

    if (const wchar_t* wide_home = ::_wgetenv(L"USERPROFILE");
        wide_home != nullptr && *wide_home != L'\0') {
        const fs::path home_dir(wide_home);
        settings_candidates.push_back(home_dir / ".claude_vn" / "settings.json");
        settings_candidates.push_back(home_dir / ".claude" / "settings.json");
    }

    for (const auto& candidate : settings_candidates) {
        if (std::string key = ReadKeyFromSettingsJson(candidate); !key.empty()) {
            return key;
        }
    }
    return {};
}

std::string SanitizeAscii(std::string_view text) {
    std::string s(text);
    s = ReplaceAllSubstring(std::move(s), "—", "--");
    s = ReplaceAllSubstring(std::move(s), "–", "--");
    s = ReplaceAllSubstring(std::move(s), "―", "--");
    s = ReplaceAllSubstring(std::move(s), "─", "--");
    s = ReplaceAllSubstring(std::move(s), "…", "...");
    s = ReplaceAllSubstring(std::move(s), "⋯", "...");
    s = ReplaceAllSubstring(std::move(s), "‘", "'");
    s = ReplaceAllSubstring(std::move(s), "’", "'");
    s = ReplaceAllSubstring(std::move(s), "“", "\"");
    s = ReplaceAllSubstring(std::move(s), "”", "\"");
    s = ReplaceAllSubstring(std::move(s), "「", "\"");
    s = ReplaceAllSubstring(std::move(s), "」", "\"");
    s = ReplaceAllSubstring(std::move(s), "『", "'");
    s = ReplaceAllSubstring(std::move(s), "』", "'");
    s = ReplaceAllSubstring(std::move(s), "≪", "\"");
    s = ReplaceAllSubstring(std::move(s), "≫", "\"");
    s = ReplaceAllSubstring(std::move(s), "　", " ");
    s = ReplaceAllSubstring(std::move(s), "〜", "~");
    s = ReplaceAllSubstring(std::move(s), "～", "~");
    s = ReplaceAllSubstring(std::move(s), "é", "e");
    s = ReplaceAllSubstring(std::move(s), "ê", "e");
    s = ReplaceAllSubstring(std::move(s), "è", "e");

    static const boost::regex kSquareRubyRe(R"re(\[([^|\]]+)\|([^\]]+)\])re");
    s = boost::regex_replace(s, kSquareRubyRe, "$2");

    static const boost::regex kAngleRubyRe(R"re(<"([^"|]+)\|([^"]+)">)re");
    s = boost::regex_replace(s, kAngleRubyRe, "$1");

    return s;
}

std::string StripSpeakerTags(std::string_view text) {
    static const boost::regex kSpeakerTagRe(
        R"re(^(?:\[(?:NARRATION|[A-Za-z0-9\s()'"?&$.,!_/-]+)\]\s*)+)re");
    const std::string raw(text);
    return TrimUnicode(SanitizeAscii(boost::regex_replace(raw, kSpeakerTagRe, "")));
}

std::string BuildUserPrompt(const std::vector<std::string>& batch_lines,
                            const std::vector<std::string>& previous_context,
                            const std::vector<std::string>& context_after) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        const std::size_t start =
            previous_context.size() > kContextWindowLines
                ? previous_context.size() - kContextWindowLines
                : 0U;
        for (std::size_t i = start; i < previous_context.size(); ++i) {
            user_prompt += previous_context[i] + "\n";
        }
        user_prompt += "</previous_context>\n\n";
    }

    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i) {
        user_prompt += std::to_string(i + 1U) + ". " + batch_lines[i] + "\n";
    }
    user_prompt += "</lines_to_translate>\n\n";

    if (!context_after.empty()) {
        user_prompt += "<context_after>\n";
        const std::size_t take = std::min(context_after.size(), kContextAfterLines);
        for (std::size_t i = 0; i < take; ++i) {
            user_prompt += context_after[i] + "\n";
        }
        user_prompt += "</context_after>\n\n";
    }

    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use <previous_context> (if present) for scene continuity, and "
        "<context_after> (if present) for lookahead context, but do NOT translate them. "
        "Never include [Speaker] tags in your output. "
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line.";
    return user_prompt;
}

std::map<int, std::string> ParseNumberedResponse(std::string_view response_text) {
    static const boost::regex kNumberedLineRe(R"(^(\d+)[.\):\s]+\s*(.*))");
    std::map<int, std::string> translations;
    int current_num = kNoActiveLineNumber;
    std::vector<std::string> current_parts;

    auto flush_current = [&]() {
        if (current_num < 0) {
            return;
        }
        std::string joined;
        for (std::size_t i = 0; i < current_parts.size(); ++i) {
            if (i > 0U) {
                joined += ' ';
            }
            joined += current_parts[i];
        }
        translations[current_num] = StripSpeakerTags(TrimUnicode(joined));
    };

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        const std::size_t nl = response_text.find('\n', pos);
        const std::string_view slice =
            (nl == std::string_view::npos)
                ? response_text.substr(pos)
                : response_text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? response_text.size() + 1U : nl + 1U;

        const std::string line = TrimUnicode(slice);
        if (line.empty()) {
            continue;
        }
        boost::smatch match;
        if (boost::regex_match(line, match, kNumberedLineRe)) {
            flush_current();
            current_num = std::stoi(match[kLineNumberCaptureGroup].str());
            const std::string clean_part =
                StripSpeakerTags(TrimUnicode(match[kLineBodyCaptureGroup].str()));
            current_parts.clear();
            if (!clean_part.empty()) {
                current_parts.push_back(clean_part);
            }
            continue;
        }
        if (current_num >= 0) {
            current_parts.push_back(line);
        }
    }
    flush_current();
    return translations;
}

std::map<int, std::string> TranslateBatchWithFallback(
    ITranslationBackend& backend,
    const std::vector<std::string>& prompt_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after) {
    if (prompt_lines.empty()) {
        return {};
    }

    auto translations =
        backend.TranslateBatch(prompt_lines, previous_context, context_after, false);
    if (!translations.empty()) {
        return translations;
    }

    if (!previous_context.empty() || !context_after.empty()) {
        LogInfo("    Empty response with context window -- retrying batch without context");
        translations = backend.TranslateBatch(prompt_lines, {}, {}, false);
        if (!translations.empty()) {
            return translations;
        }
    }

    LogInfo("    Retrying batch with neutral literary prompt...");
    translations = backend.TranslateBatch(prompt_lines, {}, {}, true);
    if (!translations.empty() || prompt_lines.size() <= 1U) {
        return translations;
    }

    LogInfo("    Batch refused or empty -- splitting into sub-batches...");
    return SubBatchTranslateRecursive(backend, prompt_lines, 0U);
}

boost::json::object BuildGenerateContentRequest(const boost::json::value& body) {
    try {
        const vn::llm::LlmRequest req = BuildLlmRequestFromJson(body);
        return bj::parse(vn::llm::build_gemini_request_json(req)).as_object();
    } catch (const vn::llm::LlmError& ex) {
        throw ApiError(ex.what());
    }
}

boost::json::object ParseGenerateContentResponse(
    const boost::json::value& gemini_response,
    GeminiUsage* usage_out) {
    try {
        const std::string raw_json = bj::serialize(gemini_response);
        const vn::llm::LlmResponse parsed =
            vn::llm::parse_gemini_response_json(raw_json, vn::llm::kHttpStatusOk);
        return ConvertLlmResponseToJson(parsed, usage_out);
    } catch (const vn::llm::LlmRateLimitError& ex) {
        throw RateLimitError(ex.what());
    } catch (const vn::llm::LlmError& ex) {
        throw ApiError(ex.what());
    }
}

struct GeminiClient::Impl {
    std::string api_key;
    std::string model;
    Glossary glossary;
    std::unique_ptr<CallbackRawHttpTransport> transport_adapter;
    std::unique_ptr<vn::llm::ILlmClient> llm_client;
    vn::llm::ZeroThoughtCircuitBreaker thought_guard;
    GeminiUsage last_usage;
    GeminiUsage cumulative_usage;

    Impl(std::string key,
         std::string mdl,
         const std::string& project_dir,
         HttpTransportFn custom_transport = {})
        : api_key(std::move(key)),
          model(std::move(mdl)) {
        try {
            vn::llm::ensure_supported_model(model);
        } catch (const vn::llm::LlmError& ex) {
            throw ApiError(ex.what());
        }
        if (!project_dir.empty()) {
            const fs::path tone_path = fs::u8path(project_dir) / "TONE_OF_NOVEL.md";
            glossary.LoadFromToneFile(tone_path.u8string());
        }
        if (api_key.empty()) {
            api_key = LoadGeminiApiKey(project_dir);
        }
        if (api_key.empty()) {
            throw ApiError(
                "GEMINI_API_KEY not set in environment, ~/.gemini/.env, HKCU\\Environment, "
                "or .claude/settings.json");
        }
        if (model.empty()) {
            model = std::string(vn::llm::kDefaultModel);
        }

        vn::llm::LlmClientOptions opts;
        opts.provider = vn::llm::LlmProvider::Gemini;
        opts.api_key = api_key;
        opts.default_model = model;
        opts.read_timeout_seconds = kDefaultTimeoutSec;
        opts.log_fn = [](std::string_view msg) { LogInfo(msg); };
        if (custom_transport) {
            transport_adapter =
                std::make_unique<CallbackRawHttpTransport>(std::move(custom_transport));
            opts.raw_transport = transport_adapter.get();
            opts.max_retries = 0;
        }
        try {
            llm_client = vn::llm::create_llm_client(std::move(opts));
        } catch (const vn::llm::LlmError& ex) {
            throw ApiError(ex.what());
        }
    }

    vn::llm::LlmResponse ExecuteLlmRequest(const vn::llm::LlmRequest& req,
                                           bool is_top_level_batch,
                                           std::size_t slice_size) {
        try {
            const std::string_view active_model =
                req.model.empty() ? std::string_view(model) : std::string_view(req.model);
            const std::string normalized_level =
                vn::llm::normalize_thinking_level(req.thinking_level);
            vn::llm::LlmResponse resp = llm_client->generate(req);
            if (is_top_level_batch && !transport_adapter && !resp.content_blocked &&
                vn::llm::is_guarded_thinking_mode(active_model, req.thinking_level) &&
                resp.usage.thought_tokens <= 0) {
                std::chrono::milliseconds backoff = vn::llm::kDefaultInitialBackoff;
                for (int retry = 0;
                     retry < kLiveZeroThoughtMaxRetries &&
                     !resp.content_blocked &&
                     resp.usage.thought_tokens <= 0;
                     ++retry) {
                    LogInfo("    Live " + normalized_level + " request (" +
                            std::string(active_model) +
                            ") returned zero thought tokens; retrying in " +
                            std::to_string(backoff.count()) + "ms...");
                    std::this_thread::sleep_for(backoff);
                    backoff *= kZeroThoughtBackoffMultiplier;
                    resp = llm_client->generate(req);
                }
            }
            LogInfo("    Tokens: in=" + std::to_string(resp.usage.prompt_tokens) +
                    " out=" + std::to_string(resp.usage.total_output_tokens()) +
                    " thoughts=" + std::to_string(resp.usage.thought_tokens) +
                    " candidates=" + std::to_string(resp.usage.candidate_tokens) +
                    " cache_read=" + std::to_string(resp.usage.cached_tokens) +
                    " finish=" + resp.finish_reason);
            thought_guard.check_response(
                active_model,
                req.thinking_level,
                is_top_level_batch,
                slice_size,
                resp.content_blocked,
                resp.usage.thought_tokens);
            if (is_top_level_batch && !resp.content_blocked) {
                thought_guard.record_top_level_batch_completed();
            }

            GeminiUsage call_usage;
            ConvertLlmResponseToJson(resp, &call_usage);
            last_usage = call_usage;
            AccumulateUsage(cumulative_usage, call_usage);
            return resp;
        } catch (const vn::llm::LlmZeroThoughtError& ex) {
            throw ZeroThoughtError(ex.what());
        } catch (const vn::llm::LlmRateLimitError& ex) {
            throw RateLimitError(ex.what());
        } catch (const vn::llm::LlmError& ex) {
            throw ApiError(ex.what());
        } catch (const std::exception& ex) {
            throw ApiError(std::string("transport error: ") + ex.what());
        }
    }

    bj::value MessagesImpl(const bj::value& body, int /*timeout_sec*/) {
        const vn::llm::LlmRequest req = BuildLlmRequestFromJson(body, model);
        const vn::llm::LlmResponse resp = ExecuteLlmRequest(req, false, 0U);
        return ConvertLlmResponseToJson(resp, nullptr);
    }

    std::map<int, std::string> TranslateBatchImpl(
        const std::vector<std::string>& prompt_lines,
        const std::vector<std::string>& previous_context,
        const std::vector<std::string>& context_after,
        bool use_neutral_prompt) {
        vn::llm::LlmRequest req;
        req.model = model;
        req.system_prompt = use_neutral_prompt
                                ? glossary.NeutralSystemPrompt()
                                : glossary.SystemPrompt();
        req.user_prompt = BuildUserPrompt(prompt_lines, previous_context, context_after);
        req.max_output_tokens = vn::llm::compute_batch_max_output_tokens(prompt_lines.size());
        req.thinking_level = std::string(vn::llm::kDefaultThinkingLevel);
        req.enforce_min_output_floor = true;
        req.safety_block_none = true;

        const bool is_top_level = !use_neutral_prompt;
        const vn::llm::LlmResponse resp =
            ExecuteLlmRequest(req, is_top_level, prompt_lines.size());

        if (resp.content_blocked) {
            return {};
        }
        return ParseNumberedResponse(TrimUnicode(resp.text));
    }
};

GeminiClient::GeminiClient(std::string api_key,
                           std::string model,
                           std::string project_dir)
    : impl_(std::make_unique<Impl>(std::move(api_key),
                                   std::move(model),
                                   project_dir)) {}

GeminiClient::GeminiClient(std::string api_key,
                           std::string model,
                           std::string project_dir,
                           HttpTransportFn transport_override)
    : impl_(std::make_unique<Impl>(std::move(api_key),
                                   std::move(model),
                                   project_dir,
                                   std::move(transport_override))) {}

GeminiClient::GeminiClient(GeminiClient&& other) noexcept = default;
GeminiClient& GeminiClient::operator=(GeminiClient&& other) noexcept = default;
GeminiClient::~GeminiClient() = default;

std::map<int, std::string> GeminiClient::TranslateBatch(
    const std::vector<std::string>& prompt_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after,
    bool use_neutral_prompt) {
    return impl_->TranslateBatchImpl(
        prompt_lines, previous_context, context_after, use_neutral_prompt);
}

boost::json::value GeminiClient::Messages(const boost::json::value& body, int timeout_sec) {
    return impl_->MessagesImpl(body, timeout_sec);
}

const std::string& GeminiClient::Model() const {
    return impl_->model;
}

GeminiUsage GeminiClient::LastUsage() const noexcept {
    return impl_ ? impl_->last_usage : GeminiUsage{};
}

GeminiUsage GeminiClient::CumulativeUsage() const noexcept {
    return impl_ ? impl_->cumulative_usage : GeminiUsage{};
}

}  // namespace oretsuba::translate
