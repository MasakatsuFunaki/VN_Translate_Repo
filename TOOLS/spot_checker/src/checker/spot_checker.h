// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "domain/spot_check_types.h"

#include <memory>
#include <string>
#include <vector>

namespace spot_checker {

class ILlmTransport;
class IFileStore;
class ICryptSync;
class IClock;
class ILogSink;

// Optional outer boundary ports for deterministic component testing. Any null
// pointer defaults to its production implementation (`ArgonClient`,
// `DiskFileStore`, `BatchProcessCryptSync`, `SystemClock`, `ConsoleLogSink`).
struct SpotCheckerDependencies {
    ILlmTransport* transport = nullptr;
    IFileStore* file_store = nullptr;
    ICryptSync* crypt_sync = nullptr;
    IClock* clock = nullptr;
    ILogSink* log_sink = nullptr;
};

// PIMPL-insulated orchestrator facade for full-novel statistical hotspot
// triage, GEMBA-MQM packed Radar, surgical Stage 2 window repair, deterministic
// post-edit validation, resumable checkpointing, and artifact/cache/`.enc` sync.
class SpotChecker {
public:
    // Validates `config` (rejecting disallowed `"argon"` model identifiers via
    // `vn::llm::is_disallowed_model`), decrypts `.json.enc` if needed, loads
    // `<game_dir>/script_output/translated_text.json` and optional tone guide,
    // and prepares the system prompt without making any network calls.
    // Returns `ErrorCode::InvalidArgument`, `ErrorCode::CryptSyncFailed`,
    // `ErrorCode::IoError`, or `ErrorCode::CorruptJson` on failure.
    static Result<SpotChecker> create(SpotCheckConfig config,
                                      SpotCheckerDependencies deps = {});

    ~SpotChecker();

    SpotChecker(SpotChecker&& other) noexcept;
    SpotChecker& operator=(SpotChecker&& other) noexcept;

    SpotChecker(const SpotChecker&) = delete;
    SpotChecker& operator=(const SpotChecker&) = delete;

    // Executes the configured pass (`Hotspot` or `Linear`):
    // - When `config().scan_only == true`: runs full `HotspotDetector` OLS +
    //   6-signal + CUSUM/KDE/NMS scan with 0 LLM calls (`GEMINI_API_KEY` not
    //   required). If `!config().out_path.empty()`, writes the structured JSON
    //   scan report ONLY to `config().out_path`; if `config().out_path.empty()`,
    //   performs zero disk writes anywhere. Never writes or modifies any file
    //   inside `config().game_dir`.
    // - Otherwise: resumes from any existing `argon_spot_check.json` (returning
    //   `ErrorCode::CorruptCheckpoint` if corrupt), enforces `max_windows` and
    //   `max_api_calls`, persists checkpoint progress after each window, and
    //   syncs improvements to `translated_text.json`, `translation_cache_*.json`,
    //   and `.json.enc` when `config().auto_apply == true`.
    // - Sustained H-scene avoidance (`config().skip_h_scenes == true` by default;
    //   `--include-h-scenes` sets `false`): detects sustained multi-line H-scene
    //   spans via `detect_h_scene_spans`, skips windows fully inside sustained
    //   H-scene spans (`++h_windows_skipped`), splits/trims partially overlapping
    //   target windows around sustained H-scene spans, and clips `context_before`
    //   / `context_after` at sustained H-scene boundaries so sustained H-scenes
    //   never enter LLM prompts while short non-H dirty/crude banter is checked
    //   intact without redaction.
    // - Progressive content-refusal recovery (`LlmResponse::content_blocked`,
    //   activated strictly on refusal and never on normal windows):
    //   1. Stage 1 Radar refusal: records call/token usage, logs a warning, and
    //      falls back to deterministic statistical hotspot order so no candidate
    //      passage is dropped before Stage 2.
    //   2. Stage 2 context-stripped retry: if a window with non-empty
    //      `context_before` or `context_after` is refused (and `max_api_calls`
    //      allows), retries that target window once with zero surrounding
    //      context (`context_before = 0`, `context_after = 0`).
    //   3. Stage 2 narrow bisection on persistent refusal: if a context-free
    //      window is still refused and `length > kDefaultMinBlockedSplitLines`,
    //      splits `[offset, offset + length)` at `length / 2` into two
    //      context-free child sub-windows pushed to the front of the work queue
    //      without marking visited and without consuming `max_windows` on the
    //      split parent.
    //   4. Stage 2 leaf blocked-span tracking & model-aware resume: if a
    //      context-free leaf (`length <= kDefaultMinBlockedSplitLines`) is
    //      persistently refused, records it in `blocked_offsets` /
    //      `blocked_spans` and `windows_blocked` / `lines_blocked` (never in
    //      `checked_offsets` / `checked_spans` or `windows_checked` /
    //      `lines_checked`), persists checkpoint state, skips `blocked_spans`
    //      when resuming with the same `config().model`, and automatically
    //      re-queues `blocked_spans` when resuming with a different
    //      `config().model` (clearing any newly checked span from
    //      `blocked_spans` on success).
    Result<SpotCheckReport> run();

    // Offline apply mode (`--apply`): loads `argon_spot_check.json`, writes all
    // recorded improvements idempotently into `translated_text.json`,
    // `translation_cache_*.json`, and `.json.enc` without making any LLM calls,
    // and returns exact delta counts (`0` on an already-synced second run).
    Result<ApplyReport> apply_improvements();

    const SpotCheckConfig& config() const noexcept;
    const std::vector<ScriptEntry>& entries() const noexcept;
    const std::string& system_prompt() const noexcept;
    const std::string& output_json_path() const noexcept;

private:
    struct Impl;
    explicit SpotChecker(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spot_checker
