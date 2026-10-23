// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
//
// Memory-scan based translator. Runs in a worker thread, periodically
// walks every committed+writable region of the process VA space, and
// for each TSV entry's JP UTF-8 bytes overwrites in place with the EN
// bytes + a NUL terminator. No code patching. No need to know which
// engine function processes the string.
//
// Why this works for Artemis: dialogue strings get loaded from .pfs
// archives into heap-allocated std::string buffers, which then sit in
// memory until rendering. The engine reads those buffers byte-by-byte
// when laying out text. If we overwrite the bytes BEFORE the engine
// does the read, English appears on screen.
//
// Tradeoffs: O(regions × strings) per pass, but the TSV is small
// (~58 entries) and we can prune scan space aggressively. We track
// already-patched (addr, en) pairs to avoid scanning the same buffer
// twice.

extern "C" {

void start_memory_scanner();
void log_scanner_summary();

}
