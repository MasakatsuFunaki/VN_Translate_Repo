// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box Component Tests (`CT`) driving `vn::llm_client` strictly through
// `create_llm_client(LlmClientOptions)` -> `ILlmClient::generate(LlmRequest)`
// with real `GeminiClientAdapter` + real `gemini_wire_codec` + real
// `ZeroThoughtCircuitBreaker`, injecting ONLY the outer `IRawHttpTransport`
// boundary double.
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vn::llm {
namespace {

constexpr int kHttpStatusBadRequest = 400;
constexpr int kHttpStatusServiceUnavailable = 503;

constexpr int kConfiguredMaxRetries = 3;
constexpr int kConfiguredConnectTimeoutSec = 45;
constexpr int kConfiguredReadTimeoutSec = 720;
constexpr std::chrono::milliseconds kConfiguredInitialBackoff{1500};
constexpr int kBackoffDoublingFactor = 2;
constexpr std::chrono::milliseconds kExpectedSecondBackoff =
    kConfiguredInitialBackoff * kBackoffDoublingFactor;

constexpr std::size_t kProductionBatchSize = 120U;
constexpr std::size_t kTwoTranslatedLines = 2U;
constexpr std::int64_t kExpectedPromptTokensCall1 = 1850;
constexpr std::int64_t kExpectedCachedTokensCall1 = 600;
constexpr std::int64_t kExpectedCandidateTokensCall1 = 48;
constexpr std::int64_t kExpectedThoughtTokensCall1 = 410;
constexpr std::int64_t kExpectedTotalOutputCall1 =
    kExpectedCandidateTokensCall1 + kExpectedThoughtTokensCall1;

constexpr std::int64_t kExpectedPromptTokensCall2 = 920;
constexpr std::int64_t kExpectedZeroTokens = 0;

constexpr std::size_t kExpectedRetrySleepsCall1 = 2U;
constexpr std::size_t kExpectedRetryLogsCall1 = 2U;
constexpr std::size_t kFirstSleepIndex = 0U;
constexpr std::size_t kSecondSleepIndex = 1U;
constexpr std::size_t kExpectedCallsAfterSeq1 = 3U;
constexpr std::size_t kExpectedCallsAfterSeq2 = 4U;
constexpr std::size_t kOverrideModelCallIndex = 3U;

constexpr int kFaultTestMaxRetries = 2;
constexpr std::chrono::milliseconds kFaultTestInitialBackoff{100};
constexpr std::size_t kExpectedFaultSleeps = 2U;
constexpr std::size_t kExpectedFaultLogs = 2U;
constexpr std::size_t kExpectedCallsAfterBadRequest = 4U;
constexpr std::size_t kExpectedCallsAfterRecovery = 5U;

constexpr std::int64_t kExpectedRecoveredPromptTokens = 50;
constexpr std::int64_t kExpectedRecoveredCandidateTokens = 4;
constexpr std::int64_t kExpectedRecoveredThoughtTokens = 300;

constexpr std::size_t kInitialZeroStrikes = 0U;
constexpr std::size_t kCompletedOneTopLevelBatch = 1U;

struct RecordedHttpCall {
    std::string target_path;
    std::string json_body;
    int connect_timeout_sec = 0;
    int read_timeout_sec = 0;
};

class ScriptedRawHttpTransport final : public IRawHttpTransport {
public:
    using Step = std::function<RawHttpResponse(const RecordedHttpCall&)>;

    void push_response(int status_code, std::string body) {
        steps_.push_back([status_code, b = std::move(body)](const RecordedHttpCall&) {
            return RawHttpResponse{status_code, b};
        });
    }

    void push_throw(std::string exception_message) {
        steps_.push_back([msg = std::move(exception_message)](const RecordedHttpCall& call)
                             -> RawHttpResponse {
            throw std::runtime_error(msg + " [target=" + call.target_path + "]");
        });
    }

    [[nodiscard]] RawHttpResponse post_json(
        std::string_view target_path,
        std::string_view json_body,
        int connect_timeout_sec,
        int read_timeout_sec) override {
        RecordedHttpCall recorded{
            std::string(target_path),
            std::string(json_body),
            connect_timeout_sec,
            read_timeout_sec,
        };
        calls_.push_back(recorded);
        if (steps_.empty()) {
            throw std::logic_error("ScriptedRawHttpTransport exhausted");
        }
        Step next = std::move(steps_.front());
        steps_.pop_front();
        return next(recorded);
    }

    [[nodiscard]] const std::vector<RecordedHttpCall>& calls() const noexcept {
        return calls_;
    }

private:
    std::deque<Step> steps_;
    std::vector<RecordedHttpCall> calls_;
};

static_assert(RawHttpTransportLike<ScriptedRawHttpTransport>);

TEST(LlmClientCt, StatefulRetryRecoveryModelOverrideAndWireAdaptation) {
    ScriptedRawHttpTransport transport;
    std::vector<std::chrono::milliseconds> sleeps;
    std::vector<std::string> logs;

    transport.push_response(
        kHttpStatusTooManyRequests,
        R"({"error":{"status":"RESOURCE_EXHAUSTED","message":"RPM quota reached"}})");
    transport.push_response(
        kHttpStatusServiceUnavailable,
        R"({"error":{"status":"UNAVAILABLE","message":"Model server busy"}})");
    transport.push_response(
        kHttpStatusOk,
        R"({"candidates":[{"content":{"parts":[{"thought":true,"text":"planning"},{"text":"1. \"Good morning, Taichi.\"\n2. The cicadas cried."}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":1850,"cachedContentTokenCount":600,"candidatesTokenCount":48,"thoughtsTokenCount":410}})");

    transport.push_response(
        kHttpStatusOk,
        R"({"promptFeedback":{"blockReason":"PROHIBITED_CONTENT"},"usageMetadata":{"promptTokenCount":920,"thoughtsTokenCount":0}})");

    LlmClientOptions opts;
    opts.api_key = "AIzaSyComponentTestKey_01";
    opts.default_model = std::string(kDefaultModel);
    opts.max_retries = kConfiguredMaxRetries;
    opts.connect_timeout_seconds = kConfiguredConnectTimeoutSec;
    opts.read_timeout_seconds = kConfiguredReadTimeoutSec;
    opts.initial_backoff = kConfiguredInitialBackoff;
    opts.raw_transport = &transport;
    opts.sleep_fn = [&](std::chrono::milliseconds d) { sleeps.push_back(d); };
    opts.log_fn = [&](std::string_view msg) { logs.emplace_back(msg); };

    std::unique_ptr<ILlmClient> client = create_llm_client(std::move(opts));
    ASSERT_NE(client, nullptr);

    ZeroThoughtCircuitBreaker breaker;

    LlmRequest req1;
    req1.system_prompt = "Translate visual novel dialogue.";
    req1.user_prompt = "1. 「おはよう、太一」\n2. 蝉が鳴いていた。";
    req1.max_output_tokens = compute_batch_max_output_tokens(kProductionBatchSize);

    const LlmResponse resp1 = client->generate(req1);
    EXPECT_FALSE(resp1.content_blocked);
    EXPECT_EQ(resp1.finish_reason, "STOP");
    EXPECT_EQ(resp1.text,
              "1. \"Good morning, Taichi.\"\n2. The cicadas cried.");
    EXPECT_EQ(resp1.usage.prompt_tokens, kExpectedPromptTokensCall1);
    EXPECT_EQ(resp1.usage.cached_tokens, kExpectedCachedTokensCall1);
    EXPECT_EQ(resp1.usage.candidate_tokens, kExpectedCandidateTokensCall1);
    EXPECT_EQ(resp1.usage.thought_tokens, kExpectedThoughtTokensCall1);
    EXPECT_EQ(resp1.usage.total_output_tokens(), kExpectedTotalOutputCall1);

    breaker.check_response(
        kDefaultModel, req1.thinking_level, /*is_top_level=*/true,
        kTwoTranslatedLines, resp1.content_blocked, resp1.usage.thought_tokens);
    breaker.record_top_level_batch_completed();
    EXPECT_EQ(breaker.consecutive_top_level_zero_thoughts(), kInitialZeroStrikes);
    EXPECT_EQ(breaker.top_level_batches_completed(), kCompletedOneTopLevelBatch);

    ASSERT_EQ(sleeps.size(), kExpectedRetrySleepsCall1);
    EXPECT_EQ(sleeps[kFirstSleepIndex], kConfiguredInitialBackoff);
    EXPECT_EQ(sleeps[kSecondSleepIndex], kExpectedSecondBackoff);
    ASSERT_EQ(logs.size(), kExpectedRetryLogsCall1);

    const std::string expected_default_target =
        "/v1beta/models/" + std::string(kDefaultModel) +
        ":generateContent?key=AIzaSyComponentTestKey_01";
    ASSERT_EQ(transport.calls().size(), kExpectedCallsAfterSeq1);
    for (std::size_t i = 0; i < kExpectedCallsAfterSeq1; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(transport.calls()[i].target_path, expected_default_target);
        EXPECT_EQ(transport.calls()[i].connect_timeout_sec,
                  kConfiguredConnectTimeoutSec);
        EXPECT_EQ(transport.calls()[i].read_timeout_sec,
                  kConfiguredReadTimeoutSec);
        EXPECT_NE(transport.calls()[i].json_body.find("\"maxOutputTokens\":36000"),
                  std::string::npos);
        EXPECT_NE(transport.calls()[i].json_body.find(
                      "\"thinkingConfig\":{\"thinkingLevel\":\"MEDIUM\"}"),
                  std::string::npos);
        EXPECT_NE(transport.calls()[i].json_body.find("「おはよう、太一」"),
                  std::string::npos);
    }

    LlmRequest banned_req;
    banned_req.model = "gemini-4-argon";
    banned_req.user_prompt = "1. 禁止モデル";
    EXPECT_THROW((void)client->generate(banned_req), LlmError);
    EXPECT_EQ(transport.calls().size(), kExpectedCallsAfterSeq1);

    LlmClientOptions banned_opts;
    banned_opts.api_key = "AIzaSyComponentTestKey_01";
    banned_opts.default_model = "GEMINI-4-ARGON";
    banned_opts.raw_transport = &transport;
    EXPECT_THROW((void)create_llm_client(std::move(banned_opts)), LlmError);
    EXPECT_EQ(transport.calls().size(), kExpectedCallsAfterSeq1);

    LlmRequest req2;
    req2.model = "gemini-3.1-pro-preview";
    req2.user_prompt = "Evaluate window";
    req2.json_mode = true;

    const LlmResponse resp2 = client->generate(req2);
    EXPECT_TRUE(resp2.content_blocked);
    EXPECT_EQ(resp2.finish_reason, "PROHIBITED_CONTENT");
    EXPECT_EQ(resp2.text, "");
    EXPECT_EQ(resp2.usage.prompt_tokens, kExpectedPromptTokensCall2);
    EXPECT_EQ(resp2.usage.thought_tokens, kExpectedZeroTokens);
    EXPECT_EQ(sleeps.size(), kExpectedRetrySleepsCall1);
    ASSERT_EQ(transport.calls().size(), kExpectedCallsAfterSeq2);
    EXPECT_EQ(transport.calls()[kOverrideModelCallIndex].target_path,
              "/v1beta/models/gemini-3.1-pro-preview:generateContent?key=AIzaSyComponentTestKey_01");
    EXPECT_NE(transport.calls()[kOverrideModelCallIndex].json_body.find(
                  "\"responseMimeType\":\"application/json\""),
              std::string::npos);
}

TEST(LlmClientCt, RedactsApiKeyAcrossTransportFaultsNonRetryableErrorsAndRecovers) {
    constexpr std::string_view kSecretKey = "AIzaSyTopSecretRedactionCanary_987654";

    ScriptedRawHttpTransport transport;
    std::vector<std::chrono::milliseconds> sleeps;
    std::vector<std::string> logs;

    transport.push_throw("TLS socket reset");
    transport.push_throw("TLS handshake EOF");
    transport.push_throw("TLS write broken pipe");

    transport.push_response(
        kHttpStatusBadRequest,
        R"({"error":{"status":"INVALID_ARGUMENT","message":"Invalid request for key=AIzaSyTopSecretRedactionCanary_987654"}})");

    transport.push_response(
        kHttpStatusOk,
        R"({"candidates":[{"content":{"parts":[{"text":"Recovered cleanly."}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":50,"candidatesTokenCount":4,"thoughtsTokenCount":300}})");

    LlmClientOptions opts;
    opts.api_key = std::string(kSecretKey);
    opts.max_retries = kFaultTestMaxRetries;
    opts.initial_backoff = kFaultTestInitialBackoff;
    opts.raw_transport = &transport;
    opts.sleep_fn = [&](std::chrono::milliseconds d) { sleeps.push_back(d); };
    opts.log_fn = [&](std::string_view msg) { logs.emplace_back(msg); };

    std::unique_ptr<ILlmClient> client = create_llm_client(std::move(opts));
    ASSERT_NE(client, nullptr);

    LlmRequest req;
    req.user_prompt = "1. テスト";

    try {
        (void)client->generate(req);
        FAIL() << "Expected LlmTransientError after exhausting retries";
    } catch (const LlmTransientError& ex) {
        const std::string what_str = ex.what();
        EXPECT_EQ(what_str.find(kSecretKey), std::string::npos) << what_str;
        EXPECT_NE(what_str.find("<redacted>"), std::string::npos) << what_str;
    }

    ASSERT_EQ(sleeps.size(), kExpectedFaultSleeps);
    ASSERT_EQ(logs.size(), kExpectedFaultLogs);
    for (const std::string& log_entry : logs) {
        EXPECT_EQ(log_entry.find(kSecretKey), std::string::npos) << log_entry;
        EXPECT_NE(log_entry.find("<redacted>"), std::string::npos) << log_entry;
    }

    try {
        (void)client->generate(req);
        FAIL() << "Expected LlmError on HTTP 400";
    } catch (const LlmTransientError&) {
        FAIL() << "HTTP 400 must not be retried as LlmTransientError";
    } catch (const LlmError& ex) {
        EXPECT_EQ(ex.http_status(), kHttpStatusBadRequest);
        const std::string what_str = ex.what();
        EXPECT_EQ(what_str.find(kSecretKey), std::string::npos) << what_str;
        EXPECT_NE(what_str.find("<redacted>"), std::string::npos) << what_str;
    }
    EXPECT_EQ(sleeps.size(), kExpectedFaultSleeps);
    ASSERT_EQ(transport.calls().size(), kExpectedCallsAfterBadRequest);

    const LlmResponse recovered = client->generate(req);
    EXPECT_FALSE(recovered.content_blocked);
    EXPECT_EQ(recovered.finish_reason, "STOP");
    EXPECT_EQ(recovered.text, "Recovered cleanly.");
    EXPECT_EQ(recovered.usage.prompt_tokens, kExpectedRecoveredPromptTokens);
    EXPECT_EQ(recovered.usage.candidate_tokens,
              kExpectedRecoveredCandidateTokens);
    EXPECT_EQ(recovered.usage.thought_tokens, kExpectedRecoveredThoughtTokens);
    ASSERT_EQ(transport.calls().size(), kExpectedCallsAfterRecovery);
}

TEST(LlmClientCt, ZeroThoughtRetryKeepsMediumAddsNudgeAndAccumulatesBilledTokens) {
    ScriptedRawHttpTransport transport;
    std::vector<std::chrono::milliseconds> sleeps;
    std::vector<std::string> logs;

    transport.push_response(
        kHttpStatusOk,
        R"({"candidates":[{"content":{"parts":[{"text":"1. Lazy zero-thought attempt 1"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":500,"cachedContentTokenCount":100,"candidatesTokenCount":10,"thoughtsTokenCount":0}})");
    transport.push_response(
        kHttpStatusOk,
        R"({"candidates":[{"content":{"parts":[{"text":"1. Lazy zero-thought attempt 2"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":500,"cachedContentTokenCount":200,"candidatesTokenCount":12,"thoughtsTokenCount":0}})");
    transport.push_response(
        kHttpStatusOk,
        R"({"candidates":[{"content":{"parts":[{"thought":true,"text":"deep reasoning"},{"text":"1. Thoughtful final translation"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":500,"cachedContentTokenCount":200,"candidatesTokenCount":15,"thoughtsTokenCount":420}})");

    LlmClientOptions opts;
    opts.api_key = "AIzaSyZeroThoughtRetryKey";
    opts.zero_thought_max_retries = kDefaultZeroThoughtRetries;
    opts.initial_backoff = kFaultTestInitialBackoff;
    opts.raw_transport = &transport;
    opts.sleep_fn = [&](std::chrono::milliseconds d) { sleeps.push_back(d); };
    opts.log_fn = [&](std::string_view msg) { logs.emplace_back(msg); };

    std::unique_ptr<ILlmClient> client = create_llm_client(std::move(opts));
    ASSERT_NE(client, nullptr);

    LlmRequest req;
    req.user_prompt = "1. テスト行";
    const LlmResponse resp = client->generate(req);
    EXPECT_FALSE(resp.content_blocked);
    EXPECT_EQ(resp.text, "1. Thoughtful final translation");
    EXPECT_EQ(resp.usage.prompt_tokens, 1500);
    EXPECT_EQ(resp.usage.cached_tokens, 500);
    EXPECT_EQ(resp.usage.candidate_tokens, 37);
    EXPECT_EQ(resp.usage.thought_tokens, 420);

    ASSERT_EQ(transport.calls().size(), 3U);
    EXPECT_EQ(transport.calls()[0].json_body.find("Reason carefully in your internal thoughts"),
              std::string::npos);
    for (std::size_t i = 0; i < transport.calls().size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_NE(transport.calls()[i].json_body.find(
                      "\"thinkingConfig\":{\"thinkingLevel\":\"MEDIUM\"}"),
                  std::string::npos);
        EXPECT_EQ(transport.calls()[i].json_body.find("\"HIGH\""),
                  std::string::npos);
    }
    EXPECT_NE(transport.calls()[1].json_body.find("Reason carefully in your internal thoughts"),
              std::string::npos);
    EXPECT_NE(transport.calls()[2].json_body.find("Reason carefully in your internal thoughts"),
              std::string::npos);
    ASSERT_EQ(sleeps.size(), 2U);
    ASSERT_EQ(logs.size(), 2U);
}

}  // namespace
}  // namespace vn::llm
