// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Private Lakos PIMPL Object Adapter adapting `IRawHttpTransport` +
// `gemini_wire_codec` + `execute_with_exponential_backoff` behind `ILlmClient`.
#pragma once

#include "vn/llm/llm_client.h"

#include <memory>

namespace vn::llm::detail {

// Thread-compatible: concurrent const access is safe; generate() requires
// external synchronization.
class GeminiClientAdapter final : public ILlmClient {
public:
    explicit GeminiClientAdapter(LlmClientOptions options);
    ~GeminiClientAdapter() override;

    GeminiClientAdapter(GeminiClientAdapter&&) noexcept;
    GeminiClientAdapter& operator=(GeminiClientAdapter&&) noexcept;

    GeminiClientAdapter(const GeminiClientAdapter&) = delete;
    GeminiClientAdapter& operator=(const GeminiClientAdapter&) = delete;

    [[nodiscard]] LlmResponse generate(const LlmRequest& request) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vn::llm::detail
