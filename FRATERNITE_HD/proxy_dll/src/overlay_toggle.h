// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// overlay_toggle.h
//
// F1 keypress toggles a transparent click-through overlay window
// above the game window. When ON, the overlay paints the original
// Japanese for the message currently in the dialog box; when OFF,
// the overlay clears and only the engine's English is visible.
//
// The translator hook (HookedTextOutA in translator.cpp) feeds us
// per-glyph TextOutA records via RecordDialogCall — that's the only
// coupling between this module and the rest of the proxy. Init
// spawns the F1 polling thread; Shutdown stops it and tears down
// the overlay window + cached GDI resources.
//
// The engine renders text into a small dialog memory DC and presents
// via DirectX, so any GDI we issue back to the engine's HDCs gets
// overdrawn. The overlay side-steps that entirely by being a separate
// top-level window.

#include <cstdint>
#include <string>
#include <windows.h>

namespace overlay_toggle {

// Spawn the F1 polling thread + initialise the recording critical
// section. Idempotent.
void Init();

// Stop the F1 polling thread, destroy the overlay window, and free
// cached GDI resources.
void Shutdown();

// Feed one dialog TextOutA call into the per-message recording. Called
// from translator.cpp on every CP932 dialog glyph render. msg_hash
// identifies the current message — when it changes, the recording is
// cleared.
void RecordDialogCall(HDC hdc, int x, int y,
                      uint8_t b0, uint8_t b1,
                      int slot_idx, uint32_t msg_hash);

// Set the reconstructed English SPEAKER + BODY for the current message.
// Called from the translator hook once per new message (hash change).
// Stores the text and marks the overlay dirty; the actual painting
// happens on the overlay driver thread so all GDI stays single-threaded.
void SetCurrentEnglish(const std::string& speaker, const std::string& body,
                       uint32_t msg_hash);

// Provide the overlay driver a way to read the engine's "message
// length" dword (state[+0x60]); it is nonzero while a dialogue message
// is active and 0 between messages / when the box is dismissed. The
// driver uses it (debounced) to know when to clear the overlay.
void SetMsgLenProvider(int (*fn)());

}  // namespace overlay_toggle
