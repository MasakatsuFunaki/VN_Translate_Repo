// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// Loader-lock-safe Win32 IAT hooks for BGI.exe window captions, MessageBox
// confirmations and error dialogs, popup menus, and native RT_DIALOG controls
// (Configuration #127, Name/Birthday #119, Product Key #121/#125).
//
// InstallWindowChromeHooks() performs strictly kernel32-only in-memory PE
// import-table pointer patching during DLL_PROCESS_ATTACH (zero LoadLibrary,
// USER32, or GDI32 calls under the loader lock). Dialog fonts and control
// translations execute lazily on WM_INITDIALOG completion via a thread-scoped
// WH_CALLWNDPROCRET observer without ever replacing DWLP_DLGPROC.

namespace win32_chrome {

bool Win32ChromeInit() noexcept;
bool InstallWindowChromeHooks(void* host_module_base) noexcept;
void Win32ChromeShutdown() noexcept;

}  // namespace win32_chrome
