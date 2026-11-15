// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace spot_checker {

class IClock {
public:
    virtual ~IClock();

    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;

    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
    virtual std::string now_utc_iso8601() const = 0;

protected:
    IClock() = default;
    IClock(IClock&&) noexcept = default;
    IClock& operator=(IClock&&) noexcept = default;
};

class SystemClock final : public IClock {
public:
    SystemClock() = default;
    ~SystemClock() override;

    void sleep_for(std::chrono::milliseconds duration) override;
    std::string now_utc_iso8601() const override;
};

class ILogSink {
public:
    virtual ~ILogSink();

    ILogSink(const ILogSink&) = delete;
    ILogSink& operator=(const ILogSink&) = delete;

    virtual void info(std::string_view message) = 0;
    virtual void warn(std::string_view message) = 0;
    virtual void print(std::string_view line) = 0;

protected:
    ILogSink() = default;
    ILogSink(ILogSink&&) noexcept = default;
    ILogSink& operator=(ILogSink&&) noexcept = default;
};

class NullLogSink final : public ILogSink {
public:
    NullLogSink() = default;
    ~NullLogSink() override;

    void info(std::string_view message) override;
    void warn(std::string_view message) override;
    void print(std::string_view line) override;
};

class ConsoleLogSink final : public ILogSink {
public:
    ConsoleLogSink() = default;
    ~ConsoleLogSink() override;

    void info(std::string_view message) override;
    void warn(std::string_view message) override;
    void print(std::string_view line) override;
};

}  // namespace spot_checker
