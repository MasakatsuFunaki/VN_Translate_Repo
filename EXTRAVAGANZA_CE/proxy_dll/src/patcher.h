// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Runtime patcher for BLACKCyc engine text rendering.
// Waits for the packed EXE to unpack, then patches font size and
// line spacing for English text display.

#include <windows.h>

void PatcherInit();
void PatcherShutdown();
