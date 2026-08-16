// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Buf38 corpus capture.
//
// Every unique JP buf38 the engine renders during a play session is
// appended to "corpus_buf38.txt" in the game directory. The format is
// deliberately minimal: one hex-encoded CP932 byte string per line.
// The replay test (IntegrationCorpusReplayTest) hex-decodes each
// line and runs BuildRenderPlan / DetectOverflows on it against the
// deployed TSV — so future code changes that would silently truncate
// or mis-align any previously-captured scene get caught at build
// time, with no replay of the actual game.
//
// Always-on. The dedupe set means the file only grows by one line
// per unique message; in practice a 10-minute ctrl-skip session
// captures a few hundred to a couple thousand unique entries.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

inline FILE*                          g_corpusFile = nullptr;
inline std::unordered_set<uint32_t>   g_corpusSeen;
inline std::mutex                     g_corpusMutex;

inline void CorpusInit() {
    if (g_corpusFile) return;
    // Append-mode: corpus persists across sessions. The replay test
    // dedupes again on read so duplicates from older sessions are
    // harmless.
    g_corpusFile = _wfopen(L"corpus_buf38.txt", L"ab");
}

inline void CorpusClose() {
    if (g_corpusFile) {
        fclose(g_corpusFile);
        g_corpusFile = nullptr;
    }
    g_corpusSeen.clear();
}

// Append `jp` (raw CP932 bytes) to the corpus if not already seen
// in this session. Hex-encoded so the file is robust to encoding
// tools. `hash` should be a stable hash of the raw JP bytes —
// usually FNV1a32. Thread-safe; the engine's render thread can
// fire many TextOutA calls per glyph.
inline void CorpusCapture(const std::string& jp, uint32_t hash) {
    if (!g_corpusFile || jp.empty()) return;
    std::lock_guard<std::mutex> lock(g_corpusMutex);
    if (!g_corpusSeen.insert(hash).second) return;
    for (unsigned char c : jp) std::fprintf(g_corpusFile, "%02x", c);
    std::fputc('\n', g_corpusFile);
    std::fflush(g_corpusFile);
}
