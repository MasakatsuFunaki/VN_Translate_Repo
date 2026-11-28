// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Live-API System Test (`LABELS "SYSTEM"`) verifying real HTTPS/TLS connectivity
// (`Windows ROOT cert store -> OpenSSL -> Boost.Beast -> generativelanguage.googleapis.com:443`),
// out-of-the-box `create_llm_client()` defaults (`gemini-4-carbon`, `thinkingLevel = "MEDIUM"`),
// active reasoning depth (`thought_tokens >= 150`), and all 6 baseline gates of
// the 8-line Romeo Tanaka `CROSS_CHANNEL` Model-Degradation Baseline Gauntlet.
#include "model_baseline_gauntlet.h"

#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vn::llm {
namespace {

constexpr int kDupenvSuccess = 0;
constexpr std::int64_t kMinimumPositiveTokens = 0;
constexpr std::size_t kOneBasedLineOffset = 1U;
constexpr std::size_t kExpectedZeroStrikes = 0U;
constexpr std::size_t kExpectedCompletedBatches = 1U;

[[nodiscard]] static bool is_github_actions_runner() {
    char* raw_val = nullptr;
    std::size_t len = 0;
    if (::_dupenv_s(&raw_val, &len, "GITHUB_ACTIONS") != kDupenvSuccess ||
        raw_val == nullptr) {
        return false;
    }
    const std::unique_ptr<char, decltype(&std::free)> guard(raw_val, &std::free);
    return std::string_view(guard.get()) == "true";
}

TEST(LiveLlmModelBaselineSt, CarbonMediumThinkingPassesCrossChannelDegradationGauntlet) {
    const std::string resolved_key = resolve_gemini_api_key();
    if (resolved_key.empty()) {
        if (is_github_actions_runner()) {
            GTEST_SKIP() << "Optional external secret GEMINI_API_KEY is unset on "
                            "GitHub Actions runner; skipping live SYSTEM test.";
        }
        FAIL() << "GEMINI_API_KEY is missing locally (checked process environment, "
                  "~/.gemini/.env, and HKCU\\Environment). Live-API SYSTEM baseline "
                  "tests must fail loudly locally rather than silently skipping.";
    }

    std::unique_ptr<ILlmClient> client = create_llm_client();
    ASSERT_NE(client, nullptr);

    LlmRequest req;
    EXPECT_EQ(req.thinking_level, kDefaultThinkingLevel);
    EXPECT_EQ(req.max_output_tokens, kMinOutputTokensFloor);
    req.system_prompt = testing::gauntlet_system_prompt();
    req.user_prompt = testing::build_gauntlet_user_prompt();

    const LlmResponse resp = client->generate(req);
    EXPECT_FALSE(resp.content_blocked)
        << "Unexpected content block on baseline gauntlet: " << resp.finish_reason;
    EXPECT_EQ(resp.finish_reason, "STOP");
    EXPECT_GT(resp.usage.prompt_tokens, kMinimumPositiveTokens);
    EXPECT_GT(resp.usage.candidate_tokens, kMinimumPositiveTokens);
    EXPECT_GE(resp.usage.thought_tokens, testing::kBaselineMinThoughtTokens)
        << "gemini-4-carbon fell below the 150-token MEDIUM-thinking baseline floor "
           "(observed thought_tokens="
        << resp.usage.thought_tokens << ").";

    ZeroThoughtCircuitBreaker breaker;
    breaker.check_response(
        kDefaultModel, req.thinking_level, /*is_top_level=*/true,
        testing::kGauntletLineCount, resp.content_blocked,
        resp.usage.thought_tokens);
    breaker.record_top_level_batch_completed();
    EXPECT_EQ(breaker.consecutive_top_level_zero_thoughts(),
              kExpectedZeroStrikes);
    EXPECT_EQ(breaker.top_level_batches_completed(), kExpectedCompletedBatches);

    const std::vector<std::string> eight_lines =
        testing::parse_gauntlet_numbered_lines(resp.text, testing::kGauntletLineCount);
    ASSERT_EQ(eight_lines.size(), testing::kGauntletLineCount)
        << "Failed to parse 8 sequential numbered lines from live response:\n"
        << resp.text;

    const double usd = estimate_cost_usd(resp.usage, kDefaultModel);
    const double eur = estimate_cost_eur(resp.usage, kDefaultModel);
    std::cout << "[LiveLlmModelBaselineSt] model=" << kDefaultModel
              << " prompt_tokens=" << resp.usage.prompt_tokens
              << " cached_tokens=" << resp.usage.cached_tokens
              << " candidate_tokens=" << resp.usage.candidate_tokens
              << " thought_tokens=" << resp.usage.thought_tokens
              << " cost_usd=" << usd
              << " cost_eur=" << eur << "\n";
    for (std::size_t i = 0; i < eight_lines.size(); ++i) {
        std::cout << "[LiveLlmModelBaselineSt] #" << (i + kOneBasedLineOffset)
                  << ": " << eight_lines[i] << "\n";
    }
    std::cout.flush();

    const testing::GauntletVerdict verdict =
        testing::evaluate_gauntlet_outputs(eight_lines, resp.usage.thought_tokens);
    if (!verdict.passed) {
        std::string joined;
        for (const std::string& failure : verdict.failures) {
            joined += "  - " + failure + "\n";
        }
        FAIL() << "CROSS_CHANNEL model-degradation baseline gauntlet FAILED:\n"
               << joined;
    }
}

}  // namespace
}  // namespace vn::llm
