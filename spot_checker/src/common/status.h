// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace spot_checker {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,
    IoError,
    CorruptJson,
    CorruptCheckpoint,
    CryptSyncFailed,
    MissingApiKey,
    RateLimited,
    HttpError,
    NetworkError,
    BudgetExhausted,
};

std::string_view to_string(ErrorCode code) noexcept;

struct Status {
    ErrorCode code = ErrorCode::Ok;
    int http_status = 0;
    std::string message;

    static Status ok() noexcept { return {}; }

    static Status error(ErrorCode code,
                        std::string message,
                        int http_status = 0) {
        return Status{code, http_status, std::move(message)};
    }

    bool is_ok() const noexcept { return code == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return is_ok(); }
};

template <typename T>
class Result {
public:
    static Result success(T value) {
        return Result(std::move(value));
    }

    static Result failure(Status status) {
        assert(!status.is_ok());
        return Result(std::move(status));
    }

    static Result failure(ErrorCode code,
                          std::string message,
                          int http_status = 0) {
        return failure(Status::error(code, std::move(message), http_status));
    }

    bool is_ok() const noexcept { return status_.is_ok(); }
    explicit operator bool() const noexcept { return is_ok(); }

    const Status& status() const noexcept { return status_; }
    ErrorCode error_code() const noexcept { return status_.code; }
    const std::string& error_message() const noexcept { return status_.message; }

    T& value() & {
        assert(value_.has_value());
        return *value_;
    }

    const T& value() const& {
        assert(value_.has_value());
        return *value_;
    }

    T&& value() && {
        assert(value_.has_value());
        return std::move(*value_);
    }

private:
    explicit Result(T value)
        : status_(Status::ok()), value_(std::move(value)) {}

    explicit Result(Status status)
        : status_(std::move(status)), value_(std::nullopt) {}

    Status status_;
    std::optional<T> value_;
};

}  // namespace spot_checker
