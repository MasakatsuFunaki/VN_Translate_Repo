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

inline constexpr int kDefaultMaxApiRetries = 4;
inline constexpr std::chrono::milliseconds kDefaultInitialBackoff{2000};

struct ArgonClientOptions {
    std::string api_key;
    std::string model{kDefaultArgonModel};
    int max_retries = kDefaultMaxApiRetries;
    std::chrono::milliseconds initial_backoff = kDefaultInitialBackoff;
    IClock* clock = nullptr;
    ILogSink* log_sink = nullptr;
};

// PIMPL-insulated HTTPS/TLS client for `generativelanguage.googleapis.com:443`
// (`gemini-4-argon` by default). Loads the Windows `ROOT` certificate store once
// into an internal RAII SSL context and reuses it across calls.
class ArgonClient final : public ILlmTransport {
public:
    explicit ArgonClient(ArgonClientOptions options = {});
    explicit ArgonClient(std::string api_key,
                         std::string model = std::string(kDefaultArgonModel));
    ~ArgonClient() override;

    ArgonClient(ArgonClient&&) noexcept;
    ArgonClient& operator=(ArgonClient&&) noexcept;

    ArgonClient(const ArgonClient&) = delete;
    ArgonClient& operator=(const ArgonClient&) = delete;

    Result<LlmResponse> generate(const LlmRequest& request) override;

    // Resolves `GEMINI_API_KEY` from process environment (`_wgetenv`) or, if
    // unset, reads `dotenv_override` (defaulting to `%USERPROFILE%/.gemini/.env`)
    // via `parse_dotenv_value` without mutating global process environment.
    static std::string resolve_gemini_api_key(
        const std::filesystem::path& dotenv_override = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spot_checker
