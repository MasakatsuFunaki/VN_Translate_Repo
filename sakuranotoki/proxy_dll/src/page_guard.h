// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// PAGE_GUARD-based discovery of the engine's dialogue-read function.
// See page_guard.cpp for rationale.

extern "C" void start_page_guard();
extern "C" void log_page_guard_summary();

// Called by memory_scanner per successful patch. We pick the FIRST
// patched address (whose page becomes the guarded page) and ignore
// subsequent registrations.
extern "C" void page_guard_register_patched(void* addr);
