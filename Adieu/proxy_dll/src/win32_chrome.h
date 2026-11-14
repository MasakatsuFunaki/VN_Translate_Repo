// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// Loader-lock-safe Win32 IAT hooks for SiglusEngine.exe window captions,
// MessageBox confirmations, right-click popup menus, and native Configuration /
// Save-Load (#146) dialogs.
//
// Win32ChromeInit() performs strictly kernel32-only in-memory PE import-table
// pointer patching during DLL_PROCESS_ATTACH (zero LoadLibrary, USER32, or
// GDI32 calls under the loader lock). Dialog fonts and control/tab/column
// translations execute lazily at runtime when dialogs open.

namespace win32_chrome {

bool Win32ChromeInit() noexcept;
bool InstallWindowChromeHooks(void* host_module_base) noexcept;
void Win32ChromeShutdown() noexcept;

}  // namespace win32_chrome
