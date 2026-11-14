// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// One-shot dumper for the per-game 16-byte private key (pk[0..15]) used by
// SiglusEngine to encrypt Scene.pck chunks. We hook the engine's LZSS
// decompressor (FUN_006d1a80, RVA 0x002D1A80) with a VEH+INT3 breakpoint:
// when it fires, the function's first __fastcall arg (ECX) points to a
// freshly-decrypted chunk struct {u32 comp_size; u32 decomp_size; u8 body[];}.
// We dump that struct + register state to siglus_key_dump.txt, restore the
// original byte, retry the instruction, and (after MAX_CAPTURES hits) remove
// the breakpoint entirely so the game runs normally.
//
// Offline, a Python script XORs the captured plaintext against the matching
// encrypted chunk on disk to recover all 16 key bytes.

void KeyDumperInit();
void KeyDumperShutdown();
