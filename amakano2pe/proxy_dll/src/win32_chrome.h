// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Win32 window-chrome localization hooks for cs2.exe.
//
// Routes the host module's USER32 window-title and dialog imports through
// detours that show the English product name and confirmation text on the
// desktop, while config\startup.xml keeps the Japanese <title> that
// CatSystem2 derives the %APPDATA% save folder from:
//   SetWindowTextW / SetWindowTextA    top-level window titles
//   CreateWindowExW / CreateWindowExA  lpWindowName of top-level windows
//   MessageBoxW / MessageBoxA          dialog caption & stateless UI body text
//   FindWindowW / FindWindowA          Japanese lpWindowName looked up under
//                                      the English title first so single-
//                                      instance checks still find the window.

namespace win32_chrome {

// Patches the import address table of the PE image loaded at
// `host_module_base` (an HMODULE, e.g. GetModuleHandleW(nullptr)).
// Idempotent and thread-safe: slots already routed through a detour are left
// alone, so the replaced imports stay callable and never recurse. Every
// detour passes misses through untouched to the import it replaced and never
// throws. Returns true when at least one window-title import of that module
// is routed through a detour.
bool InstallWindowChromeHooks(void* host_module_base) noexcept;

}  // namespace win32_chrome
