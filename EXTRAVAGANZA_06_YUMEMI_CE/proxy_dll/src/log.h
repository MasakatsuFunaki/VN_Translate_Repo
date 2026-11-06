// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// File-based diagnostic logging for the YUMEMIRYOJOKU_CE winmm.dll proxy.
#pragma once

#include <cstdio>

extern FILE* g_logFile;

void LogInit();
void LogClose();
void Log(const char* fmt, ...);
