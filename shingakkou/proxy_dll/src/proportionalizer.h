// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Proportional text rendering for the DDSystem/PIL engine.
// Memory-patches the EXE at two locations to use glyph-proportional
// cell advance and adjusted half-width measurement.

#include <windows.h>

void ProportionalizerInit();
void ProportionalizerShutdown();

void ProportionalizerInit();
void ProportionalizerShutdown();
