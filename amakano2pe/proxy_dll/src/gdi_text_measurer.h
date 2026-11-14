// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// gdi_text_measurer.h  (amakano2pe / CatSystem2)
//
// Pimpl-insulated Win32 GDI implementation of translator_logic::TextWidthMeasurer
// matching CatSystem2 STRING8 glyph measurement (`LOGFONTW` with negative
// character height, SHIFTJIS_CHARSET, ANTIALIASED_QUALITY, default face
// `メイリオ`). Kept free of hook/trampoline state so unit tests exercise
// real GDI pixel measurement directly inside ctest.

#include "translator_logic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace gdi_text {

inline constexpr int kFontScaleNumerator = 9;
inline constexpr int kFontScaleDenominator = 10;

inline constexpr std::size_t kFontSizePresetCount = 5;
inline constexpr std::size_t kLayoutPresetSlotFss = 0;
inline constexpr std::size_t kLayoutPresetSlotFs = 1;
inline constexpr std::size_t kLayoutPresetSlotNormal = 2;
inline constexpr std::size_t kLayoutPresetSlotFl = 3;
inline constexpr std::size_t kLayoutPresetSlotFll = 4;
inline constexpr int kPrimaryFontContextSlot = 0;

using String8LayoutSizes = std::array<int, kFontSizePresetCount>;

inline constexpr int kMeswndPresetSizeFss = 20;
inline constexpr int kMeswndPresetSizeFs = 22;
inline constexpr int kMeswndPresetSizeNormal = 23;
inline constexpr int kMeswndPresetSizeFl = 27;
inline constexpr int kMeswndPresetSizeFll = 30;

inline constexpr String8LayoutSizes kMeswndDialogueLayoutSizes = {
    kMeswndPresetSizeFss,
    kMeswndPresetSizeFs,
    kMeswndPresetSizeNormal,
    kMeswndPresetSizeFl,
    kMeswndPresetSizeFll,
};

inline constexpr int kBacklogPresetSizeFss = 18;
inline constexpr int kBacklogPresetSizeFs = 20;
inline constexpr int kBacklogPresetSizeNormal = 22;
inline constexpr int kBacklogPresetSizeFl = 25;
inline constexpr int kBacklogPresetSizeFll = 28;

inline constexpr String8LayoutSizes kBacklogDialogueLayoutSizes = {
    kBacklogPresetSizeFss,
    kBacklogPresetSizeFs,
    kBacklogPresetSizeNormal,
    kBacklogPresetSizeFl,
    kBacklogPresetSizeFll,
};

inline constexpr int kRubyLayoutSize = 9;
inline constexpr int kSpeakerNameplateLayoutSize = 26;

inline constexpr int kDefaultScaledDialogueLfHeight = -20;
inline constexpr int kMinDialogueFontHeightPx = 14;
inline constexpr int kMaxDialogueFontHeightPx = 36;
inline constexpr long kDefaultFontWeight = 400;
inline constexpr long kBoldFontWeight = 700;

inline constexpr std::size_t kEngineTextboxPixelWidth = 690;
inline constexpr std::size_t kTextboxWrapSafetyMarginPx = 10;
inline constexpr std::size_t kDefaultMaxDialogueLineWidthPx =
    kEngineTextboxPixelWidth - kTextboxWrapSafetyMarginPx;

// Return-address RVAs immediately following `call 0x0063D000`
// (`Cs2FontContext::SetHeight`) across all 4 `STRING8` base-font setup sites in
// `cs2.exe` (`ImageBase == 0x00400000`), distinct from per-glyph inline `\f`
// override loops (`0x225A24`, `0x225CCC`, `0x225D3E`, `0x22BB34`, `0x22BDBF`).
inline constexpr std::uintptr_t kMeasureGlyphsFullwidthBaseFontRetRva = 0x22596Bu;
inline constexpr std::uintptr_t kMeasureGlyphsHalfwidthBaseFontRetRva = 0x225C03u;
inline constexpr std::uintptr_t kRenderFullwidthBaseFontRetRva = 0x22BA7Bu;
inline constexpr std::uintptr_t kRenderHalfwidthBaseFontRetRva = 0x22BD09u;

inline constexpr std::size_t kMeasureHalfwidthEbpFrameDisp = 0x24u;
inline constexpr std::size_t kString8LayoutSizesOffset = 0x24u;
inline constexpr std::size_t kString8MeasureWeightOffset = 0xCCu;
inline constexpr std::size_t kString8RenderWeightOffset = 0x10Cu;
inline constexpr std::size_t kString8MinReadableBytes = 0x110u;

struct String8BaseFontObservation {
    String8LayoutSizes layout_sizes{};
    long font_weight = kDefaultFontWeight;
};

// Returns true iff `[ptr, ptr + min_bytes)` is committed and readable without
// triggering guard or no-access faults.
bool IsReadableMemoryRange(const void* ptr, std::size_t min_bytes) noexcept;

// Resolves the owning `STRING8` instance and its active font weight (`+0xCC`
// in `MeasureGlyphs` fullwidth/halfwidth passes, `+0x10C` in `Render`
// fullwidth/halfwidth passes) when `caller_ret - module_base` matches one of
// the 4 `STRING8` base-font setup call sites. Returns false for all other
// callers or unreadable pointers.
bool ResolveString8BaseFontObservation(
    std::uintptr_t module_base,
    std::uintptr_t caller_ret,
    const void* caller_esi,
    const void* caller_ebx,
    std::uintptr_t caller_ebp,
    String8BaseFontObservation& out) noexcept;

// Scales an unscaled CatSystem2 LOGFONTW height by kFontScaleNumerator /
// kFontScaleDenominator toward zero, preserving sign.
int ScaleEngineFontHeight(int raw_height) noexcept;

// Returns true iff `std::abs(raw_height)` is one of the 5 dialogue font preset
// sizes configured by `meswnd.fes` (`str layout size 20 22 23 27 30`),
// excluding ruby (`rsize 9`), speaker nameplate (`name layout size 26`), and UI.
bool IsMeswndDialogueLayoutSize(int raw_height) noexcept;

// Returns true iff `layout_sizes` equals `meswnd.fes`'s 5-preset STRING8
// layout table (`{\fss, \fs, \fn, \fl, \fll} == {20, 22, 23, 27, 30}`) and
// `std::abs(raw_height) == kMeswndPresetSizeNormal` (`23`), rejecting
// `log.fes` (`{18, 20, 22, 25, 28}`), inline `\f` overrides, speaker
// nameplates, and UI elements.
bool IsMeswndString8BaseLayout(const String8LayoutSizes& layout_sizes,
                               int raw_height) noexcept;

// Thread-safe.
class GdiTextWidthMeasurer final : public translator_logic::TextWidthMeasurer {
public:
    explicit GdiTextWidthMeasurer(
        int initial_lf_height = kDefaultScaledDialogueLfHeight,
        std::wstring_view face_name = L"メイリオ",
        long font_weight = kDefaultFontWeight);
    ~GdiTextWidthMeasurer() override;

    GdiTextWidthMeasurer(const GdiTextWidthMeasurer&) = delete;
    GdiTextWidthMeasurer& operator=(const GdiTextWidthMeasurer&) = delete;
    GdiTextWidthMeasurer(GdiTextWidthMeasurer&&) noexcept;
    GdiTextWidthMeasurer& operator=(GdiTextWidthMeasurer&&) noexcept;

    // Updates the active measurement font when `std::abs(scaled_lf_height)` is
    // within `[kMinDialogueFontHeightPx, kMaxDialogueFontHeightPx]`. Rejects
    // ruby heights (`abs < 14`) without clobbering the active dialogue font.
    bool UpdateDialogueFont(int scaled_lf_height,
                            std::wstring_view face_name = {},
                            long font_weight = 0);

    bool SetDialogueFontHeight(int scaled_lf_height);

    // Scales `raw_height` by 9/10 and updates the dialogue font exclusively
    // when `active_slot == kPrimaryFontContextSlot`, `layout_sizes != nullptr`,
    // and `IsMeswndString8BaseLayout(*layout_sizes, raw_height)` holds.
    int SyncFromString8BaseFont(int active_slot,
                                const String8LayoutSizes* layout_sizes,
                                int raw_height,
                                std::wstring_view face_name = {},
                                long font_weight = 0);

    int ActiveFontHeight() const noexcept;
    long ActiveFontWeight() const noexcept;
    bool IsValid() const noexcept;

    std::size_t MeasureSpanWidth(std::string_view utf8_span) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gdi_text
