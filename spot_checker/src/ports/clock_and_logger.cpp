// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "ports/clock_and_logger.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

namespace spot_checker {
namespace {

constexpr std::size_t kLocalTimeBufferBytes = 16;
constexpr std::size_t kIso8601BufferBytes = 32;
constexpr int kTmYearBase = 1900;
constexpr int kTmMonthOffset = 1;

std::string local_hms_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &now_t);
    char buf[kLocalTimeBufferBytes];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    return buf;
}

void write_stdout_line(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

IClock::~IClock() = default;
SystemClock::~SystemClock() = default;

void SystemClock::sleep_for(std::chrono::milliseconds duration) {
    if (duration.count() > 0) {
        std::this_thread::sleep_for(duration);
    }
}

std::string SystemClock::now_utc_iso8601() const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
    gmtime_s(&utc_tm, &now_t);
    char buf[kIso8601BufferBytes];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc_tm.tm_year + kTmYearBase,
                  utc_tm.tm_mon + kTmMonthOffset,
                  utc_tm.tm_mday,
                  utc_tm.tm_hour,
                  utc_tm.tm_min,
                  utc_tm.tm_sec);
    return buf;
}

ILogSink::~ILogSink() = default;
NullLogSink::~NullLogSink() = default;
ConsoleLogSink::~ConsoleLogSink() = default;

void NullLogSink::info(std::string_view) {}
void NullLogSink::warn(std::string_view) {}
void NullLogSink::print(std::string_view) {}

void ConsoleLogSink::info(std::string_view message) {
    std::string line = local_hms_timestamp();
    line += " INFO ";
    line.append(message.data(), message.size());
    write_stdout_line(line);
}

void ConsoleLogSink::warn(std::string_view message) {
    std::string line = local_hms_timestamp();
    line += " WARN ";
    line.append(message.data(), message.size());
    write_stdout_line(line);
}

void ConsoleLogSink::print(std::string_view line) {
    write_stdout_line(std::string(line));
}

}  // namespace spot_checker
