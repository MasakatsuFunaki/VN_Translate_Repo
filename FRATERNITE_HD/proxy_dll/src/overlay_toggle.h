// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// F1 toggles a transparent overlay above the game window: JP when on,
// EN when off. A separate top-level window avoids the engine's DirectX
// overdraw.

#include <cstdint>
#include <string>
#include <windows.h>

namespace overlay_toggle {

// Idempotent.
void Init();

void Shutdown();

// Called from translator.cpp on every CP932 dialog glyph render.
void RecordDialogCall(HDC hdc, int x, int y,
                      uint8_t b0, uint8_t b1,
                      int slot_idx, uint32_t msg_hash);

// Called once per new message. Painting happens on the driver thread.
void SetCurrentEnglish(const std::string& speaker, const std::string& body,
                       uint32_t msg_hash);

// Nonzero while a message is active; 0 clears the overlay (debounced).
void SetMsgLenProvider(int (*fn)());

}  // namespace overlay_toggle
