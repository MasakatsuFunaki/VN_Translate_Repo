// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// FRATERNITE_HD runtime translator.
//
// IAT-hooks GDI32!TextOutA. Each call peeks the engine's buf38 (via the
// YU-RIS state struct at RVA 0x4095F0) and, when a new CP932 message
// is detected, builds an EN render plan and substitutes each per-glyph
// TextOutA call with the matching ASCII chunk in a narrower font.

#include <windows.h>

void TranslatorInit();
void TranslatorShutdown();
