// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// XINPUT1_3.dll proxy: forwards the 8 named exports to the real
// system DLL via naked-jmp stubs. Real DLL is loaded from
// SysWOW64\XINPUT1_3.dll on x64 Windows (we always build x86).

#include <windows.h>

void ProxyInit();
void ProxyShutdown();
