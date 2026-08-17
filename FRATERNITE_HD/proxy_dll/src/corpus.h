// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Appends each unique buf38 JP message (hex-encoded CP932) to
// corpus_buf38.txt. The replay test uses this to catch render-plan
// regressions without a playthrough.

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
    // Append-mode; the replay test dedupes on read.
    g_corpusFile = _wfopen(L"corpus_buf38.txt", L"ab");
}

inline void CorpusClose() {
    if (g_corpusFile) {
        fclose(g_corpusFile);
        g_corpusFile = nullptr;
    }
    g_corpusSeen.clear();
}

// Thread-safe. Hex-encodes so the file is encoding-tool-robust.
inline void CorpusCapture(const std::string& jp, uint32_t hash) {
    if (!g_corpusFile || jp.empty()) return;
    std::lock_guard<std::mutex> lock(g_corpusMutex);
    if (!g_corpusSeen.insert(hash).second) return;
    for (unsigned char c : jp) std::fprintf(g_corpusFile, "%02x", c);
    std::fputc('\n', g_corpusFile);
    std::fflush(g_corpusFile);
}
