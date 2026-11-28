// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Private component header for `.env` parsing and Windows credential lookup.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vn::llm {

[[nodiscard]] std::string resolve_gemini_api_key(
    const std::filesystem::path& dotenv_override);

[[nodiscard]] std::string parse_dotenv_value(std::string_view dotenv_contents,
                                             std::string_view key_name);

}  // namespace vn::llm
