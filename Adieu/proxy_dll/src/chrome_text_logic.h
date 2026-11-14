// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// OS-independent UTF-16 lookup and formatting logic for SiglusEngine Win32
// window titles, MessageBox confirmations, right-click popup menus, and native
// Configuration / Save-Load (#146) dialog captions, controls, tabs, and
// listview column headers.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace chrome_text {

inline constexpr std::wstring_view kEnglishWindowTitle =
    L"Sayonara o Oshiete ~comment te dire adieu~";

// Thread-compatible UTF-16 dictionary for Win32 UI chrome insulated via PIMPL.
class ChromeDictionary {
public:
    ChromeDictionary();
    ChromeDictionary(const ChromeDictionary& other);
    ChromeDictionary(ChromeDictionary&& other) noexcept;
    ChromeDictionary& operator=(const ChromeDictionary& other);
    ChromeDictionary& operator=(ChromeDictionary&& other) noexcept;
    ~ChromeDictionary();

    // Translates Japanese window captions (`さよならを教えて` and full-title
    // tilde/wave-dash variants) to `kEnglishWindowTitle`, preserving any
    // trailing status/FPS/chapter suffix (e.g. ` [60.0 fps]`). Returns
    // std::nullopt if `title` is empty, already English, or unrelated.
    std::optional<std::wstring> TranslateWindowTitle(
        std::wstring_view title) const;

    // Translates Japanese confirmation prompts, right-click #SYSCOMMENU items,
    // Config/Save-Load (#146) dialog captions, static labels, buttons, tab
    // headers, and listview column headers. Preserves Win32 menu accelerator
    // tails (`\tCtrl+...` or `(&X)`). Returns std::nullopt on miss.
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
