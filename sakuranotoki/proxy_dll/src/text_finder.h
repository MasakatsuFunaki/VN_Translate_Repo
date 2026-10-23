// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Runtime discovery: find the function that reads the patched dialogue
// bytes for rendering. See text_finder.cpp for the rationale.

extern "C" void start_text_finder();
extern "C" void log_text_finder_summary();

// memory_scanner calls this once per successful patch so the finder
// has a list of candidate addresses to install hardware-read
// breakpoints on. We pick a few across the patched set and arm DR0..3
// once enough are accumulated.
extern "C" void text_finder_register_patched(void* addr);
