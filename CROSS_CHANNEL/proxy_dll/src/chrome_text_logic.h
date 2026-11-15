// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// chrome_text_logic.h
//
// Pure (OS-independent) localization and layout logic for CROSS_CHANNEL's
// Win32 chrome (MessageBox prompts, fatal Direct3D9 startup alerts, About
// dialog #103) and in-engine UI strings rendered from cc.exe's .rdata/.data
// tables (Config help tooltips, modal confirmation/error prompts, Playing Log
// statistics, Extra Story names, Scene & Diary titles, Pause HUD, and Backlog
// labels).
//
// Free of <windows.h> so the exact same compiled object code in
// translator_logic links into both xinput1_3.dll and unit test binaries.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chrome_text_logic {

// Config tooltip panel in FUN_008226b0 -> FUN_0081e7e0:
// maxW = 405.0f, stepX = 27.0f -> 13.5 px per ASCII char (30.0 chars max).
// Wrapping at 28 columns keeps line width <= 378 px, preventing FUN_0081e7e0
// from inserting an automatic wrap right before an explicit %N newline.
inline constexpr std::size_t kDefaultTooltipMaxCols = 28u;

// Modal dialog geometry in FUN_0082f5b0 (1280x720 virtual canvas).
inline constexpr float kScreenCenterX = 640.0f;
inline constexpr float kModalDefaultStepX = 24.0f;
inline constexpr int kModalButtonOffsetY = 0x8E;
inline constexpr int kModalLineStrideY = 0x26;
inline constexpr int kModalTextBaseWithButtonsY = 0x55;
inline constexpr int kModalTextBaseNoButtonsY = 0x68;

// Native mouse hit-rects in FUN_0082fb50 (Ghidra 0x008D9EF4 / 0x008D9F0C):
//   Yes: [offset_x + 535, offset_x + 607] (width 72, y = offset_y + 142)
//   No:  [offset_x + 643, offset_x + 751] (width 108, y = offset_y + 142)
inline constexpr float kModalYesRectLeft = 535.0f;
inline constexpr float kModalYesRectWidth = 72.0f;
inline constexpr float kModalNoRectLeft = 643.0f;
inline constexpr float kModalNoRectWidth = 108.0f;

struct ModalButtonLayout {
    float yes_x = 0.0f;
    float slash_x = 0.0f;
    float no_x = 0.0f;
    float y = 0.0f;
};

// Window title localization. Preserves "CROSS†CHANNEL Ver.1.00" (returns
// std::nullopt) because cc.exe runs a single-instance FindWindowW(L"PH", ...)
// check against that exact title string at startup.
std::optional<std::wstring> TranslateWindowTitle(std::wstring_view text);

// Win32 MessageBoxW / MessageBoxA caption localization ("確認", "エラー").
std::optional<std::wstring> TranslateMessageBoxCaption(std::wstring_view text);

// Win32 MessageBoxW / MessageBoxA body localization (exit prompt and
// Direct3D9 shader / texture / adapter startup errors).
std::optional<std::wstring> TranslateMessageBoxBody(std::wstring_view text);

// Win32 dialog resource (#103 IDD_ABOUTBOX) caption & control localization.
std::optional<std::wstring> TranslateDialogText(std::wstring_view text);

// In-engine CP932 (or UTF-8) UI string localization for cc.exe .rdata/.data
// strings: modal lines, Playing Log stats, Extra Story titles, Scene & Diary
// titles, Pause/Backlog/Achievement format strings, and short HUD tokens.
// Returns std::nullopt when input is empty, already English, or unknown.
std::optional<std::string> TranslateEngineUiCp932(std::string_view text);

// Translates one of the 20 Config help descriptions (RVA 0x0C84A0..0x0C84EC)
// and pre-wraps each paragraph with "%N" to at most `max_cols` columns.
std::optional<std::string> TranslateConfigTooltipCp932(
    std::string_view text,
    std::size_t max_cols = kDefaultTooltipMaxCols);

// Word-wraps English UI text using "%N" as the AdvHD line-break marker while
// preserving explicit "%N" / "%N%N" paragraph breaks.
std::string WrapEngineTooltipCp932(
    std::string_view text,
    std::size_t max_cols = kDefaultTooltipMaxCols);

// Counts horizontal halfwidth display units up to the end of `cp932_text` or
// the first "%N" line break (ASCII = 1 unit, 2-byte CP932 glyph = 2 units).
std::size_t MeasureEngineDisplayUnits(std::string_view cp932_text);

// Computes the centered starting X coordinate for a single modal text line
// around `modal_offset_x + 640.0f`, given `step_x` (where 1 display unit
// advances `step_x * 0.5f` pixels in FUN_0081e6f0).
float ComputeModalLineX(std::string_view cp932_text,
                        int modal_offset_x,
                        float step_x = kModalDefaultStepX);

// Computes the relative Y offset from `modal_offset_y` for the first non-null
// prompt line in FUN_0082f5b0 (matching native vertical centering).
int ComputeModalFirstLineOffsetY(int line_count, bool has_buttons);

// Computes symmetric X/Y coordinates for "Yes", "/", and "No" in FUN_0082f5b0
// such that "Yes" and "No" sit strictly inside their native mouse hit-rects.
ModalButtonLayout ComputeModalButtonLayout(int modal_offset_x,
                                           int modal_offset_y,
                                           float step_x = kModalDefaultStepX);

}  // namespace chrome_text_logic
