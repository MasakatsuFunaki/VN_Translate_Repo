// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Detours-style export prologue hook.
//
// Patches the first 5+ bytes of an exported function with a JMP rel32
// to a user-supplied hook. Allocates a trampoline that runs the
// stolen prologue + jumps back to the original function past the
// stolen bytes — the hook can call this trampoline to invoke the
// original.
//
// Why this exists (vs. IAT hooks):
//   IAT hooks only catch callers that go through the EXE's static
//   import table. CrackProof's unpacker resolves imports separately
//   and the unpacked engine uses cached function pointers, bypassing
//   the static IAT entirely. Direct prologue patching of the export
//   itself catches EVERY caller regardless of how they obtained the
//   pointer.
//
// Anti-tamper safety:
//   CrackProof watches sakuranotoki.exe's .text section. kernel32.dll
//   lives in a different mapped image; we patch its prologue via
//   VirtualProtect(PAGE_EXECUTE_READWRITE). Because Windows DLLs are
//   loaded copy-on-write per process, our patches don't affect other
//   processes — they're in our private copy of kernel32's pages.

#include <windows.h>

// Install a prologue hook on `target` (e.g. kernel32!MultiByteToWideChar).
//   target         : address of the function to hook
//   hook           : address of the hook function (same calling convention)
//   trampoline_out : receives an address that, when called, executes the
//                    original function (stolen prologue + jump-back). The
//                    hook can use this to "call original".
// Returns true on success. On failure target is unchanged.
bool ExportHookInstall(void* target, void* hook, void** trampoline_out);
