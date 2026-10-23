// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Install IAT hooks on the WinAPI functions Artemis uses for CP932 text
// handling. See iat_translator.cpp for the rationale and current status
// (diagnostic vs. translation).

extern "C" void start_iat_translator();
extern "C" void log_iat_translator_summary();

// Register a heap address that memory_scanner has just patched. The
// IAT hooks consult this registry on every call: when called with a
// registered address, lstrlenA returns en_len (the FULL English length,
// possibly > jp_len), and the str/MB2WC family substitutes src to point
// at full_en. Without this, the engine asks lstrlenA "how long is this
// dialogue?", gets jp_len back (because we wrote NUL at jp_len), and
// renders a truncated line.
//
// `full_en` MUST point into a buffer that lives for the rest of the
// process lifetime. memory_scanner uses pair.en.c_str() from a reserved
// std::vector — no reallocations, pointers stable forever.
extern "C" void iat_register_patched(void* addr, const char* full_en,
                                      int en_len);
