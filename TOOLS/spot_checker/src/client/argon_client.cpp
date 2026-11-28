// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_client.h"

#include "common/utf8_text.h"
#include "vn/llm/llm_client.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace spot_checker {
namespace {

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;

}  // namespace

struct ArgonClient::Impl {
    ArgonClientOptions options;
    std::string init_model_error;
    std::unique_ptr<vn::llm::ILlmClient> client;

    explicit Impl(ArgonClientOptions opts) : options(std::move(opts)) {
        try {
            vn::llm::ensure_supported_model(options.model);
        } catch (const vn::llm::LlmError& ex) {
            init_model_error = ex.what();
            return;
        }

        const std::string_view trimmed_model = trim_ascii_view(options.model);
        if (trimmed_model.empty()) {
            options.model = std::string(kDefaultModel);
        } else {
            options.model = std::string(trimmed_model);
        }

        if (options.api_key.empty()) {
            options.api_key = vn::llm::resolve_gemini_api_key();
        }

        vn::llm::LlmClientOptions llm_opts;
        llm_opts.provider = vn::llm::LlmProvider::Gemini;
        llm_opts.api_key = options.api_key;
        llm_opts.default_model = options.model;
        llm_opts.max_retries = options.max_retries;
        llm_opts.initial_backoff = options.initial_backoff;
        if (options.clock != nullptr) {
            IClock* const clk = options.clock;
            llm_opts.sleep_fn = [clk](std::chrono::milliseconds d) {
                clk->sleep_for(d);
            };
        }
        if (options.log_sink != nullptr) {
            ILogSink* const sink = options.log_sink;
            llm_opts.log_fn = [sink](std::string_view msg) {
                sink->warn(msg);
            };
        }
        client = vn::llm::create_llm_client(std::move(llm_opts));
    }

    Result<LlmResponse> generate(const LlmRequest& request) {
        if (!init_model_error.empty()) {
            return Result<LlmResponse>::failure(
                ErrorCode::InvalidArgument, init_model_error);
        }
        if (!is_valid_thinking_level(request.thinking_level)) {
            return Result<LlmResponse>::failure(
                ErrorCode::InvalidArgument,
                "invalid Gemini thinkingLevel: '" + request.thinking_level + "'");
        }
        if (options.api_key.empty()) {
            return Result<LlmResponse>::failure(
                ErrorCode::MissingApiKey,
                "GEMINI_API_KEY is not set in environment, ~/.gemini/.env, or "
                "HKCU\\Environment");
        }

        vn::llm::LlmRequest vn_req;
        vn_req.system_prompt = request.system_prompt;
        vn_req.user_prompt = request.user_prompt;
        vn_req.thinking_level = request.thinking_level;
        vn_req.thinking_budget =
            static_cast<std::int64_t>(request.thinking_budget);
        vn_req.max_output_tokens =
            static_cast<std::int64_t>(request.max_output_tokens);
        vn_req.enforce_min_output_floor = false;
        vn_req.safety_block_none = true;
        vn_req.json_mode = request.json_mode;

        try {
            vn::llm::LlmResponse vn_resp = client->generate(vn_req);
            LlmResponse out;
            if (!vn_resp.content_blocked) {
                out.text = std::move(vn_resp.text);
            }
            out.finish_reason = std::move(vn_resp.finish_reason);
            out.prompt_tokens = vn_resp.usage.prompt_tokens;
            out.output_tokens = vn_resp.usage.candidate_tokens;
            out.thought_tokens = vn_resp.usage.thought_tokens;
            out.content_blocked = vn_resp.content_blocked;
            return Result<LlmResponse>::success(std::move(out));
        } catch (const vn::llm::LlmRateLimitError& ex) {
            return Result<LlmResponse>::failure(
                ErrorCode::RateLimited, ex.what(), ex.http_status());
        } catch (const vn::llm::LlmTransientError& ex) {
            if (ex.http_status() > 0) {
                return Result<LlmResponse>::failure(
                    ErrorCode::HttpError, ex.what(), ex.http_status());
            }
            return Result<LlmResponse>::failure(
                ErrorCode::NetworkError, ex.what());
        } catch (const vn::llm::LlmError& ex) {
            if (ex.http_status() > 0 &&
                (ex.http_status() < kHttpOkMin ||
                 ex.http_status() > kHttpOkMax)) {
                return Result<LlmResponse>::failure(
                    ErrorCode::HttpError, ex.what(), ex.http_status());
            }
            return Result<LlmResponse>::failure(
                ErrorCode::CorruptJson, ex.what(), ex.http_status());
        } catch (const std::exception& ex) {
            return Result<LlmResponse>::failure(
                ErrorCode::NetworkError,
                std::string("HTTPS transport error: ") + ex.what());
        }
    }
};

ArgonClient::ArgonClient(ArgonClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ArgonClient::ArgonClient(std::string api_key, std::string model)
    : ArgonClient(ArgonClientOptions{std::move(api_key), std::move(model)}) {}

ArgonClient::~ArgonClient() = default;
ArgonClient::ArgonClient(ArgonClient&&) noexcept = default;
ArgonClient& ArgonClient::operator=(ArgonClient&&) noexcept = default;

Result<LlmResponse> ArgonClient::generate(const LlmRequest& request) {
    return impl_->generate(request);
}

std::string ArgonClient::resolve_gemini_api_key(
    const std::filesystem::path& dotenv_override) {
    return vn::llm::resolve_gemini_api_key(dotenv_override);
}

}  // namespace spot_checker
