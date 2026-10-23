// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// In-process engine scout — Artemis / Sakura no Toki edition.
//
// sakuranotoki.exe is a CrackProof launcher: a small (704KB) on-disk EXE
// that decrypts the ~5MB Artemis engine into a transient sakuranotoki.log
// file at runtime and memory-maps it INSIDE sakuranotoki.exe's image
// range. So the live runtime image at base 0x00400000 is much larger than
// the on-disk PE and contains the real engine code we need to hook.
//
// Once our version.dll is loaded into sakuranotoki.exe (via the version-
// dll-as-proxy injection vector), we have full read access to that
// decrypted engine from inside the same process.
//
// ScoutEngine() walks every committed region in our own VA space, scans
// each for known Artemis signatures (RTTI type names, font/D3D9 strings,
// x86 cdecl prologue density), logs the top scoring regions, AND dumps
// the full sakuranotoki.exe runtime image to sakuranotoki_runtime.bin
// next to the EXE. That dump is what we load into Ghidra for analysis —
// the on-disk PE doesn't show the engine code.
//
// Read-only and idempotent; safe to call from a worker thread.

void ScoutEngine();
