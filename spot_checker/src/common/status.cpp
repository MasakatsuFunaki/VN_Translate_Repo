// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/status.h"

namespace spot_checker {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::CorruptJson: return "corrupt_json";
    case ErrorCode::CorruptCheckpoint: return "corrupt_checkpoint";
    case ErrorCode::CryptSyncFailed: return "crypt_sync_failed";
    case ErrorCode::MissingApiKey: return "missing_api_key";
    case ErrorCode::RateLimited: return "rate_limited";
    case ErrorCode::HttpError: return "http_error";
    case ErrorCode::NetworkError: return "network_error";
    case ErrorCode::BudgetExhausted: return "budget_exhausted";
    }
    return "unknown_error";
}

}  // namespace spot_checker
