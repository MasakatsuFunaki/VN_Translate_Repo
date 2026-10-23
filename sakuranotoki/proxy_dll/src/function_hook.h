// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Runtime function-execute hardware breakpoints for identifying
// dialogue-render candidate functions in the unpacked Artemis runtime
// image. See function_hook.cpp for the rationale.

extern "C" void start_function_hook();
extern "C" void log_function_hook_summary();
