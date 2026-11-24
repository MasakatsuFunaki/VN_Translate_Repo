// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// OS-independent UTF-16 lookup and formatting logic for Sakura no Uta (BGI.exe)
// Win32 window titles, MessageBox confirmations and DirectSound/Direct3D error
// dialogs, popup menus, and native RT_DIALOG controls (Configuration #127,
// Name/Birthday #119, Product Key #121/#125).

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace chrome_text {

inline constexpr std::wstring_view kEnglishWindowTitle =
    L"Sakura no Uta -Fluttering Above the Cherry Blossom Forest-";

// Thread-compatible UTF-16 dictionary for Win32 UI chrome insulated via PIMPL.
class ChromeDictionary {
public:
    ChromeDictionary();
    ChromeDictionary(const ChromeDictionary& other);
    ChromeDictionary(ChromeDictionary&& other) noexcept;
    ChromeDictionary& operator=(const ChromeDictionary& other);
    ChromeDictionary& operator=(ChromeDictionary&& other) noexcept;
    ~ChromeDictionary();

    // Translates Japanese window captions (`サクラノ詩`, `桜の詩`, and full-title
    // dash/bar subtitle variants matched longest-first) to
    // `kEnglishWindowTitle`, preserving any trailing status/FPS/version suffix
    // (e.g. ` [60.0 fps]`). Returns std::nullopt if `title` is empty, already
    // begins with `kEnglishWindowTitle`, or is unrelated.
    std::optional<std::wstring> TranslateWindowTitle(
        std::wstring_view title) const;

    // Translates Japanese confirmation prompts, popup menu items, native BGI
    // RT_DIALOG captions/labels/buttons, DirectSound/Direct3D error messages
    // (including formatted `result[0x...]` suffixes), and `<digits>秒` numeric
    // readouts (`-> "<ascii_digits> s"`). Preserves Win32 menu tab accelerators
    // (`\tCtrl+...`) and/or trailing mnemonics (`(&X)` / ` (&X)` where X is
    // ASCII alphanumeric). Returns std::nullopt if `text` is empty, pure ASCII,
    // or unmapped.
    std::optional<std::wstring> TranslateControlOrMenuText(
        std::wstring_view text) const;

    std::size_t Size() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const ChromeDictionary& DefaultChromeDictionary();

std::optional<std::wstring> TranslateWindowTitle(std::wstring_view title);
std::optional<std::wstring> TranslateChromeWide(std::wstring_view text);

}  // namespace chrome_text
