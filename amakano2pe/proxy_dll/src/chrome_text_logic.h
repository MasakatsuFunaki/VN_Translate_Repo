// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Pure mapping of CatSystem2 desktop window-chrome strings (window title)
// from Japanese to English.
//
// config\startup.xml keeps the Japanese <title>, because CatSystem2 derives
// the %APPDATA% save folder from it; only the text shown on the desktop is
// rewritten through these functions.

#include <optional>
#include <string>
#include <string_view>

namespace chrome_text {

// English product name shown in place of the Japanese one.
inline constexpr std::wstring_view kEnglishWindowTitle =
    L"Amakano 2 ~Perfect Edition~";

// Returns `title` with a leading Japanese product name replaced by
// kEnglishWindowTitle, keeping any suffix the engine appended (for example a
// debug or chapter tag). Returns std::nullopt when `title` does not start with
// the Japanese product name. Accepts both the FULLWIDTH TILDE (U+FF5E, what
// CP932 0x8160 decodes to) and the WAVE DASH (U+301C) spellings of "～".
std::optional<std::wstring> TranslateWindowTitle(std::wstring_view title);

// UTF-8 counterpart of TranslateWindowTitle.
std::optional<std::string> TranslateWindowTitleUtf8(std::string_view utf8_title);

}  // namespace chrome_text
