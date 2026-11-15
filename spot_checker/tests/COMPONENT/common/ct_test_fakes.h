// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "common/utf8_text.h"
#include "ports/clock_and_logger.h"
#include "ports/crypt_sync.h"
#include "ports/file_store.h"
#include "ports/llm_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace spot_checker::test_support {

inline constexpr std::size_t kMarkerSearchRadiusBytes = 320;
inline constexpr int kDecimalBase = 10;

// Self-cleaning RAII sandbox directory rooted under the OS temporary folder.
class TempSandbox {
public:
    explicit TempSandbox(std::string_view prefix = "spot_checker_ct") {
        static std::atomic<std::uint64_t> counter{1};
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::random_device rd;
        const auto nonce = static_cast<std::uint64_t>(rd());

        std::ostringstream folder_name;
        folder_name << prefix << "_" << tick << "_" << nonce << "_" << seq;
        root_ = std::filesystem::temp_directory_path() / folder_name.str();
        std::filesystem::create_directories(root_);
    }

    ~TempSandbox() noexcept {
        if (root_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    TempSandbox(const TempSandbox&) = delete;
    TempSandbox& operator=(const TempSandbox&) = delete;

    TempSandbox(TempSandbox&& other) noexcept : root_(std::move(other.root_)) {
        other.root_.clear();
    }

    TempSandbox& operator=(TempSandbox&& other) noexcept {
        if (this != &other) {
            std::error_code ec;
            if (!root_.empty()) {
                std::filesystem::remove_all(root_, ec);
            }
            root_ = std::move(other.root_);
            other.root_.clear();
        }
        return *this;
    }

    const std::filesystem::path& path() const noexcept { return root_; }

    std::filesystem::path write_bytes(const std::filesystem::path& relative_path,
                                      std::string_view content) const {
        const auto full_path = root_ / relative_path;
        if (full_path.has_parent_path()) {
            std::filesystem::create_directories(full_path.parent_path());
        }
        std::ofstream out(full_path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        return full_path;
    }

    static std::string read_bytes(const std::filesystem::path& file_path) {
        std::ifstream in(file_path, std::ios::binary);
        if (!in) {
            return {};
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

private:
    std::filesystem::path root_;
};

// Outer filesystem test double wrapping DiskFileStore with deterministic fault triggers.
class FaultInjectingFileStore final : public IFileStore {
public:
    FaultInjectingFileStore() = default;
    ~FaultInjectingFileStore() override = default;

    void fail_on_write_attempt(std::size_t one_based_attempt) noexcept {
        fail_write_attempt_ = one_based_attempt;
    }

    void fail_writes_matching(std::string filename_substring) {
        fail_write_substring_ = std::move(filename_substring);
    }

    void fail_reads_matching(std::string filename_substring) {
        fail_read_substring_ = std::move(filename_substring);
    }

    void clear_faults() noexcept {
        fail_write_attempt_ = 0;
        fail_write_substring_.clear();
        fail_read_substring_.clear();
    }

    std::size_t write_attempts() const noexcept { return write_attempts_; }
    std::size_t successful_writes() const noexcept { return successful_writes_; }
    const std::vector<std::filesystem::path>& written_paths() const noexcept {
        return written_paths_;
    }

    bool exists(const std::filesystem::path& path) const override {
        return inner_.exists(path);
    }

    bool is_directory(const std::filesystem::path& path) const override {
        return inner_.is_directory(path);
    }

    std::vector<std::filesystem::path> list_regular_files(
        const std::filesystem::path& dir) const override {
        return inner_.list_regular_files(dir);
    }

    Result<std::string> read_text(const std::filesystem::path& path) const override {
        if (!fail_read_substring_.empty()) {
            const auto utf8_name = wide_to_utf8(path.wstring());
            if (utf8_name.find(fail_read_substring_) != std::string::npos) {
                return Result<std::string>::failure(
                    ErrorCode::IoError, "injected read fault: " + utf8_name);
            }
        }
        return inner_.read_text(path);
    }

    Status write_text_atomic(const std::filesystem::path& path,
                             std::string_view content) override {
        ++write_attempts_;
        const auto utf8_name = wide_to_utf8(path.wstring());
        if (fail_write_attempt_ != 0 && write_attempts_ == fail_write_attempt_) {
            return Status::error(ErrorCode::IoError,
                                 "injected write fault on attempt #" +
                                     std::to_string(write_attempts_));
        }
        if (!fail_write_substring_.empty() &&
            utf8_name.find(fail_write_substring_) != std::string::npos) {
            return Status::error(ErrorCode::IoError,
                                 "injected write fault on path: " + utf8_name);
        }
        const Status st = inner_.write_text_atomic(path, content);
        if (st.is_ok()) {
            ++successful_writes_;
            written_paths_.push_back(path);
        }
        return st;
    }

private:
    DiskFileStore inner_;
    std::size_t write_attempts_ = 0;
    std::size_t successful_writes_ = 0;
    std::size_t fail_write_attempt_ = 0;
    std::string fail_write_substring_;
    std::string fail_read_substring_;
    std::vector<std::filesystem::path> written_paths_;
};

// Outer `.json.enc` synchronization double recording invocations and injecting errors.
class RecordingCryptSync final : public ICryptSync {
public:
    using HookFn = std::function<Status(const std::filesystem::path&)>;

    RecordingCryptSync() = default;
    ~RecordingCryptSync() override = default;

    void set_fail_decrypt(bool fail) noexcept { fail_decrypt_ = fail; }
    void set_fail_encrypt(bool fail) noexcept { fail_encrypt_ = fail; }
    void set_decrypt_hook(HookFn hook) { decrypt_hook_ = std::move(hook); }
    void set_encrypt_hook(HookFn hook) { encrypt_hook_ = std::move(hook); }

    const std::vector<std::filesystem::path>& decrypt_calls() const noexcept {
        return decrypt_calls_;
    }

    const std::vector<std::filesystem::path>& encrypt_calls() const noexcept {
        return encrypt_calls_;
    }

    Status decrypt_if_needed(const std::filesystem::path& game_root) override {
        decrypt_calls_.push_back(game_root);
        if (fail_decrypt_) {
            return Status::error(ErrorCode::CryptSyncFailed,
                                 "injected decrypt_if_needed failure");
        }
        if (decrypt_hook_) {
            return decrypt_hook_(game_root);
        }
        return Status::ok();
    }

    Status encrypt_if_tracked(const std::filesystem::path& game_root) override {
        encrypt_calls_.push_back(game_root);
        if (fail_encrypt_) {
            return Status::error(ErrorCode::CryptSyncFailed,
                                 "injected encrypt_if_tracked failure");
        }
        if (encrypt_hook_) {
            return encrypt_hook_(game_root);
        }
        return Status::ok();
    }

private:
    bool fail_decrypt_ = false;
    bool fail_encrypt_ = false;
    HookFn decrypt_hook_;
    HookFn encrypt_hook_;
    std::vector<std::filesystem::path> decrypt_calls_;
    std::vector<std::filesystem::path> encrypt_calls_;
};

// Deterministic zero-delay clock recording sleep requests.
class FakeClock final : public IClock {
public:
    FakeClock() = default;
    ~FakeClock() override = default;

    void sleep_for(std::chrono::milliseconds duration) override {
        sleeps_.push_back(duration);
        ++seconds_counter_;
    }

    std::string now_utc_iso8601() const override {
        const auto sec = seconds_counter_ % 60U;
        const auto min = (seconds_counter_ / 60U) % 60U;
        std::ostringstream oss;
        oss << "2026-11-15T12:" << (min < 10U ? "0" : "") << min << ":"
            << (sec < 10U ? "0" : "") << sec << "Z";
        return oss.str();
    }

    const std::vector<std::chrono::milliseconds>& sleeps() const noexcept {
        return sleeps_;
    }

private:
    std::vector<std::chrono::milliseconds> sleeps_;
    std::size_t seconds_counter_ = 0;
};

// Outer structured log sink capturing messages without writing to stdout/stderr.
class CapturingLogSink final : public ILogSink {
public:
    CapturingLogSink() = default;
    ~CapturingLogSink() override = default;

    void info(std::string_view message) override {
        infos_.emplace_back(message);
    }

    void warn(std::string_view message) override {
        warns_.emplace_back(message);
    }

    void print(std::string_view line) override {
        prints_.emplace_back(line);
    }

    const std::vector<std::string>& infos() const noexcept { return infos_; }
    const std::vector<std::string>& warns() const noexcept { return warns_; }
    const std::vector<std::string>& prints() const noexcept { return prints_; }

    void clear() noexcept {
        infos_.clear();
        warns_.clear();
        prints_.clear();
    }

private:
    std::vector<std::string> infos_;
    std::vector<std::string> warns_;
    std::vector<std::string> prints_;
};

// Programmable outer LLM transport recording every LlmRequest.
class ScriptedLlmTransport final : public ILlmTransport {
public:
    using HandlerFn =
        std::function<Result<LlmResponse>(const LlmRequest&, std::size_t)>;

    ScriptedLlmTransport() = default;
    ~ScriptedLlmTransport() override = default;

    void enqueue_handler(HandlerFn handler) {
        queued_handlers_.push_back(std::move(handler));
    }

    void enqueue_error(ErrorCode code,
                       std::string message,
                       int http_status = 0) {
        Status st = Status::error(code, std::move(message), http_status);
        queued_handlers_.push_back(
            [s = std::move(st)](const LlmRequest&, std::size_t) {
                return Result<LlmResponse>::failure(s);
            });
    }

    void enqueue_blocked(std::string finish_reason,
                         std::int64_t prompt_tokens = 0,
                         std::int64_t output_tokens = 0,
                         std::int64_t thought_tokens = 0) {
        LlmResponse blocked;
        blocked.text.clear();
        blocked.finish_reason = std::move(finish_reason);
        blocked.prompt_tokens = prompt_tokens;
        blocked.output_tokens = output_tokens;
        blocked.thought_tokens = thought_tokens;
        blocked.content_blocked = true;
        queued_handlers_.push_back(
            [resp = std::move(blocked)](const LlmRequest&, std::size_t) {
                return Result<LlmResponse>::success(resp);
            });
    }

    void set_default_handler(HandlerFn handler) {
        default_handler_ = std::move(handler);
    }

    Result<LlmResponse> generate(const LlmRequest& request) override {
        const std::size_t index = requests_.size();
        requests_.push_back(request);
        if (!queued_handlers_.empty()) {
            auto handler = std::move(queued_handlers_.front());
            queued_handlers_.pop_front();
            return handler(request, index);
        }
        if (default_handler_) {
            return default_handler_(request, index);
        }
        return Result<LlmResponse>::failure(
            ErrorCode::NetworkError,
            "unexpected unscripted LLM call #" + std::to_string(index));
    }

    const std::vector<LlmRequest>& requests() const noexcept {
        return requests_;
    }

    void clear_requests() noexcept { requests_.clear(); }

private:
    std::vector<LlmRequest> requests_;
    std::deque<HandlerFn> queued_handlers_;
    HandlerFn default_handler_;
};

inline int parse_first_positive_int(std::string_view span) noexcept {
    std::size_t i = 0;
    while (i < span.size() && (span[i] < '0' || span[i] > '9')) {
        ++i;
    }
    if (i >= span.size()) {
        return 0;
    }
    int value = 0;
    while (i < span.size() && span[i] >= '0' && span[i] <= '9') {
        value = value * kDecimalBase + (span[i] - '0');
        ++i;
    }
    return value;
}

struct TargetPromptEntryInfo {
    int target_id = 0;
    bool editable = false;
    bool found = false;
};

// Locates `jp_snippet` strictly inside the `<target_window>...</target_window>`
// section of a Stage 2 user prompt and extracts its 1-based target ID (`1..W`)
// and whether it is tagged `[EDITABLE SUSPECT]`.
inline TargetPromptEntryInfo inspect_target_in_user_prompt(
    std::string_view user_prompt,
    std::string_view jp_snippet) noexcept {
    constexpr std::string_view kEditableTag = "[EDITABLE SUSPECT]";
    constexpr std::string_view kReadOnlyTag = "[READ-ONLY CONTEXT]";
    constexpr std::string_view kTargetOpenTag = "<target_window>";
    constexpr std::string_view kTargetCloseTag = "</target_window>";
    constexpr std::string_view kContextAfterTag = "<context_after>";
    constexpr std::string_view kLfBlankBoundary = "\n\n";
    constexpr std::string_view kCrlfBlankBoundary = "\r\n\r\n";

    std::string_view target_region = user_prompt;
    const std::size_t open_pos = target_region.find(kTargetOpenTag);
    if (open_pos != std::string_view::npos) {
        target_region.remove_prefix(open_pos + kTargetOpenTag.size());
    }
    const std::size_t clip_pos = std::min(
        target_region.find(kTargetCloseTag),
        target_region.find(kContextAfterTag));
    if (clip_pos != std::string_view::npos) {
        target_region = target_region.substr(0, clip_pos);
    }

    std::size_t search_from = 0;
    while (search_from < target_region.size()) {
        const std::size_t jp_pos = target_region.find(jp_snippet, search_from);
        if (jp_pos == std::string_view::npos) {
            return {};
        }

        const std::size_t window_begin =
            (jp_pos > kMarkerSearchRadiusBytes)
                ? (jp_pos - kMarkerSearchRadiusBytes)
                : 0;
        const std::string_view before =
            target_region.substr(window_begin, jp_pos - window_begin);

        const std::size_t edit_rel = before.rfind(kEditableTag);
        const std::size_t ro_rel = before.rfind(kReadOnlyTag);
        if (edit_rel == std::string_view::npos &&
            ro_rel == std::string_view::npos) {
            search_from = jp_pos + jp_snippet.size();
            continue;
        }

        const bool is_editable =
            (edit_rel != std::string_view::npos) &&
            (ro_rel == std::string_view::npos || edit_rel > ro_rel);
        const std::size_t marker_rel = is_editable ? edit_rel : ro_rel;
        const std::size_t marker_pos = window_begin + marker_rel;
        const std::size_t after_marker =
            marker_pos +
            (is_editable ? kEditableTag.size() : kReadOnlyTag.size());
        const std::string_view between_marker_and_jp =
            target_region.substr(after_marker, jp_pos - after_marker);
        if (between_marker_and_jp.find(kLfBlankBoundary) !=
                std::string_view::npos ||
            between_marker_and_jp.find(kCrlfBlankBoundary) !=
                std::string_view::npos) {
            search_from = jp_pos + jp_snippet.size();
            continue;
        }

        std::size_t line_start = target_region.rfind('\n', marker_pos);
        line_start =
            (line_start == std::string_view::npos) ? 0 : (line_start + 1);

        int id = parse_first_positive_int(
            target_region.substr(line_start, marker_pos - line_start));
        if (id <= 0) {
            id = parse_first_positive_int(between_marker_and_jp);
        }
        if (id > 0) {
            return TargetPromptEntryInfo{id, is_editable, true};
        }
        search_from = jp_pos + jp_snippet.size();
    }
    return {};
}

// Extracts the 1-based `passage_id` containing `jp_snippet` inside a Stage 1
// packed Radar prompt.
inline int find_radar_passage_id_for_jp(
    std::string_view radar_prompt,
    std::string_view jp_snippet) {
    const std::size_t jp_pos = radar_prompt.find(jp_snippet);
    if (jp_pos == std::string_view::npos) {
        return 0;
    }

    const std::string lower_prefix = to_lower_ascii(radar_prompt.substr(0, jp_pos));
    std::size_t search_end = lower_prefix.size();
    while (search_end > 0) {
        const std::size_t p_pos = lower_prefix.rfind("passage", search_end - 1);
        if (p_pos == std::string::npos) {
            break;
        }
        const std::size_t line_end = lower_prefix.find('\n', p_pos);
        const std::size_t span_end =
            (line_end == std::string::npos) ? lower_prefix.size() : line_end;
        const int id = parse_first_positive_int(
            std::string_view(lower_prefix).substr(p_pos, span_end - p_pos));
        if (id > 0) {
            return id;
        }
        search_end = p_pos;
    }
    return 0;
}

}  // namespace spot_checker::test_support
