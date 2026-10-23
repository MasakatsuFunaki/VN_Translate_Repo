// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Tiny stderr/file logger used by the proxy DLL.
//
// File output is UTF-8 (BOM) so JP strings rendered via %s show up as
// readable text in any UTF-8-aware editor. ASCII-only log lines round-trip
// unchanged; lines that embed CP932 bytes get transcoded CP932 -> UTF-8
// before write.
#pragma once
#include <cstdio>

void log_init(const char* path);   // open log file (truncate).  Pass NULL to skip.
void log_close();
void log_msg(const char* fmt, ...);

#define LOG(...) log_msg(__VA_ARGS__)
