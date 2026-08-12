// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Self-dumper for the host (packed) module. Spawns a background thread on
// init that waits long enough for the unpacker stub to run, then writes
// the in-memory image of cc.exe to `cc_unpacked.bin` next to the EXE.
// Re-import that file in Ghidra (raw binary, image base 0x400000, x86)
// to get the unpacked code with all sections fully decrypted.

void UnpackDumpInit();
