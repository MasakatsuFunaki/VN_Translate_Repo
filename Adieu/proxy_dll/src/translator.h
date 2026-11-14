// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// Runtime UTF-16 scene string hook installer for SiglusEngine.exe.
// Resolves translation_table.tsv relative to the host executable (with DLL and
// working-directory fallbacks), verifies exact PE .text prologues before
// patching Scene::get_str and Scene::get_cur_str, and assigns formatted
// English replacements through SiglusEngine.exe's native std::wstring routine.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

void TranslatorInit(HMODULE self_module = nullptr);
void TranslatorShutdown();
