// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// In-process engine scout.
//
// FAVORITE / FVP doesn't ship the engine in Sakura.exe's mapped image — the
// on-disk EXE is a 24KB launcher stub plus a 663KB encrypted overlay; the
// real FVP engine code is decrypted into the process address space at
// runtime. Once our winmm.dll is loaded into Sakura.exe (engine pulls it
// in via LoadLibrary("winmm.dll")), we have full read access to that
// decrypted code from inside the same process.
//
// ScoutEngine() walks every committed region in our own VA space, scans
// each for known FVP/FAVORITE signatures (RTTI type names, dynamic-API
// strings like "GetGlyphOutlineA", x86 cdecl prologue density), and logs
// the top-scoring regions to proxy_log.txt. The output tells us where to
// install the inline hook for the text-render path.
//
// This is read-only and idempotent. Safe to call from a worker thread.

void ScoutEngine();
