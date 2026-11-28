// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "adapters/gemini/gemini_client_adapter.h"

#include "core/ascii_text.h"
#include "core/retry_executor.h"
#include "transport/beast_tls_transport.h"
#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/zero_thought_guard.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace vn::llm::detail {
namespace {

constexpr std::string_view kGeminiApiBasePath = "/v1beta/models/";
constexpr std::string_view kGenerateContentSuffix = ":generateContent?key=";
constexpr std::string_view kInternalThinkingRetryNudge =
    "\n\nReason carefully in your internal thoughts before producing the final output.";
constexpr int kNonPositiveTimeoutThreshold = 0;
constexpr int kNoZeroThoughtRetries = 0;
constexpr std::int64_t kZeroThoughtTokens = 0;

[[nodiscard]] static std::int64_t saturating_add_non_negative(
    std::int64_t lhs,
    std::int64_t rhs) noexcept {
    const std::int64_t a = std::max(kZeroThoughtTokens, lhs);
    const std::int64_t b = std::max(kZeroThoughtTokens, rhs);
    constexpr std::int64_t kMaxI64 = std::numeric_limits<std::int64_t>::max();
    if (a > kMaxI64 - b) {
        return kMaxI64;
    }
    return a + b;
}

[[nodiscard]] static std::string normalize_default_model(
    std::string_view raw_model) {
    ensure_supported_model(raw_model);
    const std::string_view trimmed = trim_ascii_view(raw_model);
    if (trimmed.empty()) {
        return std::string(kDefaultModel);
    }
    return std::string(trimmed);
}

[[nodiscard]] static std::string build_gemini_target_path(
    std::string_view model,
    std::string_view api_key) {
    std::string path;
    path.reserve(kGeminiApiBasePath.size() + model.size() +
                 kGenerateContentSuffix.size() + api_key.size());
    path.append(kGeminiApiBasePath);
    path.append(model);
    path.append(kGenerateContentSuffix);
    path.append(api_key);
    return path;
}

}  // namespace

struct GeminiClientAdapter::Impl {
    LlmClientOptions options;
    BeastTlsHttpTransport owned_transport;

    explicit Impl(LlmClientOptions opts) : options(std::move(opts)) {
        options.default_model = normalize_default_model(options.default_model);
        if (options.api_key.empty()) {
            options.api_key = resolve_gemini_api_key();
        }
        if (options.connect_timeout_seconds <= kNonPositiveTimeoutThreshold) {
            options.connect_timeout_seconds = kDefaultConnectTimeoutSeconds;
        }
        if (options.read_timeout_seconds <= kNonPositiveTimeoutThreshold) {
            options.read_timeout_seconds = kDefaultReadTimeoutSeconds;
        }
    }

    [[nodiscard]] IRawHttpTransport& active_transport() noexcept {
        return options.raw_transport != nullptr ? *options.raw_transport
                                                : owned_transport;
    }

    [[nodiscard]] int effective_zero_thought_retries() const noexcept {
        if (options.zero_thought_max_retries >= kNoZeroThoughtRetries) {
            return options.zero_thought_max_retries;
        }
        return options.raw_transport == nullptr ? kDefaultZeroThoughtRetries
                                                : kNoZeroThoughtRetries;
    }

    void sleep_for(std::chrono::milliseconds dur) const {
        if (options.sleep_fn) {
            options.sleep_fn(dur);
            return;
        }
        std::this_thread::sleep_for(dur);
    }

    void emit_log(std::string_view message) const {
        if (options.log_fn) {
            options.log_fn(message);
        }
    }

    [[nodiscard]] LlmResponse send_once(std::string_view target_path,
                                        std::string_view request_body) {
        RawHttpResponse raw_resp;
        try {
            raw_resp = active_transport().post_json(
                target_path,
                request_body,
                options.connect_timeout_seconds,
                options.read_timeout_seconds);
        } catch (const LlmRateLimitError& ex) {
            throw LlmRateLimitError(
                redact_secret(ex.what(), options.api_key),
                ex.http_status());
        } catch (const LlmTransientError& ex) {
            throw LlmTransientError(
                redact_secret(ex.what(), options.api_key),
                ex.http_status());
        } catch (const LlmError& ex) {
            throw LlmError(
                redact_secret(ex.what(), options.api_key),
                ex.http_status());
        } catch (const std::exception& ex) {
            throw LlmTransientError(
                "HTTPS transport error: " +
                redact_secret(ex.what(), options.api_key));
        }

        try {
            return parse_gemini_response_json(raw_resp.body,
                                              raw_resp.status_code);
        } catch (const LlmRateLimitError& ex) {
            throw LlmRateLimitError(
                redact_secret(ex.what(), options.api_key),
                ex.http_status());
        } catch (const LlmTransientError& ex) {
            throw LlmTransientError(
                redact_secret(ex.what(), options.api_key),
                ex.http_status());
        } catch (const LlmError& ex) {
            throw LlmError(
                redact_secret(ex.what(), options.api_key),
                ex.http_status());
        }
    }

    [[nodiscard]] LlmResponse generate_single_attempt(
        std::string_view target_path,
        const LlmRequest& attempt_request) {
        const std::string request_body = build_gemini_request_json(attempt_request);
        const ExponentialBackoffPolicy retry_policy{
            options.max_retries,
            options.initial_backoff,
            kExponentialBackoffFactor,
        };
        return execute_with_exponential_backoff(
            retry_policy,
            [&]() { return send_once(target_path, request_body); },
            [&](std::chrono::milliseconds dur) { sleep_for(dur); },
            [&](std::string_view msg) { emit_log(msg); });
    }

    [[nodiscard]] LlmResponse generate(const LlmRequest& request) {
        ensure_supported_model(request.model);
        const std::string_view override_model = trim_ascii_view(request.model);
        const std::string_view active_model =
            !override_model.empty()
                ? override_model
                : std::string_view(options.default_model);
        ensure_supported_model(active_model);

        if (options.api_key.empty()) {
            throw LlmError(
                "GEMINI_API_KEY is not set in environment, ~/.gemini/.env, "
                "or HKCU\\Environment");
        }

        (void)build_gemini_request_json(request);
        const std::string target_path =
            build_gemini_target_path(active_model, options.api_key);

        const int zt_max_retries =
            (request.retry_on_zero_thoughts &&
             is_guarded_thinking_mode(active_model, request.thinking_level))
                ? effective_zero_thought_retries()
                : kNoZeroThoughtRetries;

        std::chrono::milliseconds zt_delay =
            options.initial_backoff.count() > kZeroThoughtTokens
                ? options.initial_backoff
                : kDefaultInitialBackoff;

        LlmTokenUsage prior_billed_usage;
        for (int zt_attempt = 0;; ++zt_attempt) {
            LlmRequest attempt_req = request;
            if (zt_attempt > 0) {
                attempt_req.user_prompt.append(kInternalThinkingRetryNudge);
            }

            LlmResponse resp = generate_single_attempt(target_path, attempt_req);
            resp.usage.prompt_tokens = saturating_add_non_negative(
                prior_billed_usage.prompt_tokens, resp.usage.prompt_tokens);
            resp.usage.cached_tokens = saturating_add_non_negative(
                prior_billed_usage.cached_tokens, resp.usage.cached_tokens);
            resp.usage.candidate_tokens = saturating_add_non_negative(
                prior_billed_usage.candidate_tokens, resp.usage.candidate_tokens);
            resp.usage.thought_tokens = saturating_add_non_negative(
                prior_billed_usage.thought_tokens, resp.usage.thought_tokens);

            if (resp.content_blocked ||
                resp.usage.thought_tokens > kZeroThoughtTokens ||
                zt_attempt >= zt_max_retries) {
                return resp;
            }

            prior_billed_usage = resp.usage;
            emit_log(
                "Guarded Gemini 4 request returned 0 thought tokens (attempt " +
                std::to_string(zt_attempt + 1) + "/" +
                std::to_string(zt_max_retries + 1) +
                "); retrying with internal-reasoning nudge in " +
                std::to_string(zt_delay.count()) + "ms");
            sleep_for(zt_delay);
            zt_delay *= kExponentialBackoffFactor;
        }
    }
};

GeminiClientAdapter::GeminiClientAdapter(LlmClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

GeminiClientAdapter::~GeminiClientAdapter() = default;
GeminiClientAdapter::GeminiClientAdapter(GeminiClientAdapter&&) noexcept = default;
GeminiClientAdapter& GeminiClientAdapter::operator=(
    GeminiClientAdapter&&) noexcept = default;

LlmResponse GeminiClientAdapter::generate(const LlmRequest& request) {
    return impl_->generate(request);
}

}  // namespace vn::llm::detail
