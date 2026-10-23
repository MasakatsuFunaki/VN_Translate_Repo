// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// translator.h — Win32-side glue that owns the global lookup table and
// the buffer-substitution entry-point used by the engine hook.
#pragma once
#include <string>

namespace tr_runtime {

// Initialise the table by loading translations.tsv from the same directory
// as our DLL.  Logs success/failure.  Idempotent.
void init();

// Dump per-process counters (total / exact / norm / miss) to the log.
// Call from a periodic timer or on shutdown so the log shows the final
// hit-rate even when the process is force-killed.
void log_summary();

// Translate `jp_utf8` to English UTF-8.  Returns nullptr on miss (caller
// should pass through the original buffer).  The returned pointer is
// owned by the translator and remains valid for the process lifetime.
const char* translate_jp_to_en_utf8(const char* jp_utf8);

// Convenience: same but for null-terminated CP932 input.  We convert to
// UTF-8 internally, look up, and (on hit) return the English UTF-8 buffer.
// This is the codepath some Artemis engines (older builds) need.
const char* translate_jp_to_en_cp932(const char* jp_cp932);

}  // namespace tr_runtime
