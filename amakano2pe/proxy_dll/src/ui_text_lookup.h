// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// ui_text_lookup.h
//
// Stateless lookup table and dynamic pattern matchers for non-dialogue UI
// strings routed through CatSystem2's FUN_0063a9b0 UTF-8 -> UTF-16 converter
// (protagonist name-input defaults and confirmation prompts, choice markers,
// key-customize action labels, sound-group/voice labels, and config preview
// samples).
//
// Pure OS-independent C++17 logic: callers invoke LookupStatelessUiText before
// dialogue word-wrap and inline-wait continuation state so mid-scene UI modal
// lookups never mutate dialogue layout state or clobber deduplication slots.

#include <optional>
#include <string>
#include <string_view>

namespace translator_logic {

// Translates a single protagonist name token ("和倉" -> "Wakura",
// "賢一" -> "Kenichi"), returning the input unchanged when it is a custom
// player-entered token.
std::string TranslateProtagonistNameToken(std::string_view token);

// Normalizes a full or partial protagonist name string (e.g. "和倉 賢一",
// "和倉　賢一", "和倉賢一", "和倉", "賢一", or custom player-entered names)
// into English display form.
std::string NormalizeProtagonistDisplayName(std::string_view raw_name);

// Matches dynamic protagonist name-confirmation prompts (such as
// "<surname> <given> さんでお間違いないですか？" and bracketed/newline
// variants) and formats the corresponding English confirmation prompt.
std::optional<std::string> TryFormatProtagonistConfirmationPrompt(
    std::string_view utf8_src);

// Looks up a stateless non-dialogue UI string by exact literal match or
// dynamic UI pattern match. Returns std::nullopt when `utf8_src` is not a
// recognized UI literal or pattern.
std::optional<std::string> LookupStatelessUiText(std::string_view utf8_src);

// Alias matching the S7_win32_non_graphic_ui.md Section 6.1 entry point name.
inline std::optional<std::string> TryTranslateStatelessUiText(
    std::string_view utf8_src) {
    return LookupStatelessUiText(utf8_src);
}

}  // namespace translator_logic
