// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// IAT (Import Address Table) hooking for the game's main module.
// Replaces function pointers in the IAT so calls go through our hooks.

#include <windows.h>

// Replace a function pointer in the IAT of the given module.
// Returns the original function pointer, or nullptr on failure.
void* IATHook(HMODULE module, const char* dllName, const char* funcName, void* hookFunc);
