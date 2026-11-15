// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// win32_chrome.h
//
// Win32 chrome localization controller for CROSS_CHANNEL (xinput1_3.dll).
// Installs inline detours on user32.dll MessageBox and Dialog creation APIs
// (safe before and after cc.exe's runtime packer unpacks its import table)
// and applies thread-scoped WH_CALLWNDPROCRET WM_INITDIALOG localization for
// Win32 dialog resources (#103 IDD_ABOUTBOX).

#include <memory>

namespace win32_chrome {

class Win32ChromeController {
public:
    Win32ChromeController();
    ~Win32ChromeController();

    Win32ChromeController(const Win32ChromeController&) = delete;
    Win32ChromeController& operator=(const Win32ChromeController&) = delete;

    Win32ChromeController(Win32ChromeController&&) noexcept;
    Win32ChromeController& operator=(Win32ChromeController&&) noexcept;

    bool Install() noexcept;
    void Shutdown() noexcept;
    bool IsInstalled() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Process-wide facade for TranslatorInit() / TranslatorShutdown().
bool InstallWin32ChromeHooks() noexcept;
void ShutdownWin32ChromeHooks() noexcept;

}  // namespace win32_chrome
