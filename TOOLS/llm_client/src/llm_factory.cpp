// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "vn/llm/llm_client.h"

#include "adapters/gemini/gemini_client_adapter.h"

#include <memory>
#include <utility>

namespace vn::llm {

std::unique_ptr<ILlmClient> create_llm_client(LlmClientOptions options) {
    switch (options.provider) {
    case LlmProvider::Gemini:
        return std::make_unique<detail::GeminiClientAdapter>(std::move(options));
    }
    throw LlmError("unsupported LlmProvider");
}

}  // namespace vn::llm
