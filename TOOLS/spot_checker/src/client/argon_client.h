// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "domain/spot_check_types.h"
#include "ports/clock_and_logger.h"
#include "ports/llm_transport.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace spot_checker {

inline constexpr int kDefaultMaxApiRetries = vn::llm::kDefaultMaxRetries;
inline constexpr std::chrono::milliseconds kDefaultInitialBackoff =
    vn::llm::kDefaultInitialBackoff;

struct ArgonClientOptions {
    std::string api_key;
    std::string model{kDefaultModel};
    int max_retries = kDefaultMaxApiRetries;
    std::chrono::milliseconds initial_backoff = kDefaultInitialBackoff;
    IClock* clock = nullptr;
    ILogSink* log_sink = nullptr;
};

// PIMPL-insulated `ILlmTransport` adapter delegating HTTPS/TLS transport,
// Windows `ROOT` X.509 certificate loading, retry/backoff, and wire encoding
// to shared `vn::llm::create_llm_client` (`tools/llm_client`).
// Construction is non-throwing even if `options.model` violates policy;
// `generate()` returns `ErrorCode::InvalidArgument` before credential lookup or
// network I/O when `vn::llm::is_disallowed_model` rejects the configured model
// (e.g. any `"argon"` model identifier or embedded NUL byte) or when
// `request.thinking_level` is invalid.
class ArgonClient final : public ILlmTransport {
public:
    explicit ArgonClient(ArgonClientOptions options = {});
    explicit ArgonClient(std::string api_key,
                         std::string model = std::string(kDefaultModel));
    ~ArgonClient() override;

    ArgonClient(ArgonClient&&) noexcept;
    ArgonClient& operator=(ArgonClient&&) noexcept;

    ArgonClient(const ArgonClient&) = delete;
    ArgonClient& operator=(const ArgonClient&) = delete;

    Result<LlmResponse> generate(const LlmRequest& request) override;

    // Delegates to `vn::llm::resolve_gemini_api_key(dotenv_override)` without
    // mutating global process environment.
    static std::string resolve_gemini_api_key(
        const std::filesystem::path& dotenv_override = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spot_checker
