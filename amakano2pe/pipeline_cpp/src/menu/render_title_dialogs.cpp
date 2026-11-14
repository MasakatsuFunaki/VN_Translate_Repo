// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_title_dialogs.h"

#include "hg3/hg3_codec.h"
#include "imaging/bgra_image.h"
#include "menu/core/sprite_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ama::menu {

namespace {

constexpr const wchar_t* kPrimaryUiFontFamily = L"Segoe UI";

constexpr int kFontWeightSemibold = 600;
constexpr int kFontWeightBold = 700;

constexpr BgraColor kTransparentColor{0x00, 0x00, 0x00, 0x00};

// Title screen button state identification (frame_id modulo arithmetic).
constexpr std::uint32_t kTitleStateIdModulus = 10U;
constexpr std::uint32_t kTitleNormalStateRemainder = 0U;
constexpr std::uint32_t kTitleHoverStateRemainder = 1U;
constexpr std::uint32_t kTitleExtraDisabledFrameId = 132U;
constexpr std::uint32_t kTitleStylizedLogoFrameId = 20U;

// Title screen shared typography metrics.
constexpr float kTitleButtonFontSizeCompact = 25.0F;
constexpr float kTitleButtonFontSizeStandard = 26.0F;
constexpr float kTitleButtonFontSizeLarge = 27.0F;
constexpr float kTitleTriStoryFontSizeCompact = 18.0F;
constexpr float kTitleTriStoryFontSizeStandard = 18.5F;
constexpr float kSingleLineSpacing = 1.0F;
constexpr float kTitleButtonStrokePx = 2.2F;
constexpr float kTitleButtonShadowOffsetPx = 0.0F;
constexpr float kTitleButtonShadowBlurPx = 3.0F;
constexpr float kTitleButtonMinSqueeze = 0.88F;
constexpr float kTitleButtonMinScale = 0.80F;
constexpr float kTitleTriStoryLineSpacing = 1.08F;

// Title screen state palettes (BGRA).
constexpr BgraColor kTitleNormalFill{0x2A, 0x2F, 0x3D, 0xFF};
constexpr BgraColor kTitleNormalStroke{0xC8, 0xE6, 0xFC, 0xE8};
constexpr BgraColor kTitleNormalShadow{0xB8, 0xE0, 0xFC, 0xB0};

constexpr BgraColor kTitleHoverFill{0x0D, 0x49, 0x9A, 0xFF};
constexpr BgraColor kTitleHoverStroke{0xC8, 0xE6, 0xFC, 0xF0};
constexpr BgraColor kTitleHoverShadow{0xA8, 0xD8, 0xFC, 0xC8};

constexpr BgraColor kTitleDisabledFill{0x6A, 0x6B, 0x6E, 0xFF};
constexpr BgraColor kTitleDisabledStroke{0xE0, 0xEC, 0xF5, 0xD8};
constexpr BgraColor kTitleDisabledShadow{0xD8, 0xE6, 0xF2, 0x90};

// Title costume selector (sys_title_naked.hg3) metrics & palettes.
constexpr float kNakedButtonFontSizePx = 18.0F;
constexpr float kNakedButtonStrokePx = 2.0F;
constexpr float kNakedButtonShadowOffsetPx = 0.0F;
constexpr float kNakedButtonShadowBlurPx = 2.0F;
constexpr float kNakedButtonMinSqueeze = 0.85F;
constexpr float kNakedButtonMinScale = 0.80F;

constexpr BgraColor kNakedNormalFill{0x1E, 0x26, 0x36, 0xFF};
constexpr BgraColor kNakedNormalStroke{0xD8, 0xEE, 0xFC, 0xF0};
constexpr BgraColor kNakedNormalShadow{0xC8, 0xE6, 0xFC, 0xA0};

constexpr BgraColor kNakedHoverFill{0x35, 0x70, 0xA8, 0xFF};
constexpr BgraColor kNakedHoverStroke{0xD8, 0xEE, 0xFC, 0xF0};
constexpr BgraColor kNakedHoverShadow{0xB8, 0xE0, 0xFC, 0xB8};

// Standalone launcher skin (boot.hg3) metrics & palette.
constexpr std::uint32_t kBootSingleFrameId = 0U;
constexpr RectI kBootEraseRect{14, 221, 324, 31};
constexpr RectI kBootTextRect{10, 222, 332, 30};
constexpr float kBootFontSizePx = 10.5F;
constexpr float kBootLineSpacing = 1.05F;
constexpr float kBootStrokePx = 1.6F;
constexpr float kBootShadowDx = 0.0F;
constexpr float kBootShadowDy = 1.0F;
constexpr float kBootShadowBlurPx = 1.0F;
constexpr float kBootMinSqueeze = 0.85F;
constexpr float kBootMinScale = 0.75F;
constexpr BgraColor kBootFillColor{0xFF, 0xFF, 0xFF, 0xFF};
constexpr BgraColor kBootStrokeColor{0x9A, 0x55, 0x28, 0xFF};
constexpr BgraColor kBootShadowColor{0x40, 0x20, 0x10, 0xC0};

// Content warning screens (sys_warning_bg.hg3 & sys_warning_bgtr.hg3) metrics & palette.
constexpr std::uint32_t kWarningSingleFrameId = 0U;
constexpr int kWarningInkSumThreshold = 555;
constexpr int kWarningOutlineWhiteSumMin = 645;
constexpr int kWarningOutlineContrastMin = 80;
constexpr int kWarningOutlineCheckRadiusPx = 4;
constexpr int kWarningRedDeltaThreshold = 28;
constexpr int kWarningGlyphDilateRadiusPx = 4;
constexpr float kWarningStrokePx = 2.0F;
constexpr float kWarningDefaultLineSpacing = 1.25F;
constexpr float kWarningTrialHeadlineLineSpacing = 1.30F;
constexpr float kWarningTrialSubtextLineSpacing = 1.22F;
constexpr float kWarningFontSizeSmall = 19.5F;
constexpr float kWarningFontSizeBody = 20.0F;
constexpr float kWarningFontSizeFiction = 20.5F;
constexpr float kWarningFontSizeSupport = 21.0F;
constexpr float kWarningFontSizeAgeNotice = 22.0F;
constexpr float kWarningFontSizeTrialHeadline = 26.0F;
constexpr float kWarningMinSqueeze = 0.85F;
constexpr float kWarningMinScale = 0.75F;
constexpr BgraColor kWarningFillColor{0x3E, 0x41, 0x4E, 0xFF};
constexpr BgraColor kWarningStrokeColor{0xF4, 0xF6, 0xF8, 0xE0};

// Protagonist name-input modal (sys_nameinput.hg3) metrics & palette.
constexpr std::uint32_t kNameInputFirstNameFrameId = 1U;
constexpr std::uint32_t kNameInputLastNameFrameId = 0U;
constexpr RectI kNameInputEraseRect{270, 526, 740, 63};
constexpr RectI kNameInputTextRect{220, 522, 840, 72};
constexpr int kNameInputDarkCoreSumMax = 340;
constexpr int kNameInputBrownRMinusBMin = 22;
constexpr int kNameInputBrownCoreSumMax = 420;
constexpr int kNameInputWhiteStrokeChannelMin = 232;
constexpr int kNameInputCoreStrokeCheckRadiusPx = 4;
constexpr int kNameInputStrokeCoreCheckRadiusPx = 6;
constexpr int kNameInputGlowDilateRadiusPx = 6;
constexpr int kNameInputFrameDiffThreshold = 2;
constexpr int kNameInputOverlapDilatePx = 2;
constexpr int kNameInputBorderSmoothRadiusPx = 2;
constexpr int kNameInputSkirtHemlineTopY = 537;
constexpr int kNameInputSkirtHemlineBotY = 543;
constexpr int kNameInputBagBottomTopY = 575;
constexpr int kNameInputBagBottomBotY = 585;
constexpr int kNameInputSkirtBandStartX = 605;
constexpr int kNameInputSkirtBlendInEndX = 635;
constexpr int kNameInputSkirtFadeOutStartX = 980;
constexpr int kNameInputSkirtBandEndX = 1005;
constexpr float kNameInputSkirtLeftEdgeX = 765.0F;
constexpr float kNameInputSkirtRightEdgeX = 980.0F;
constexpr float kNameInputSkirtRightFlareSlope = 0.16F;
constexpr float kNameInputSkirtHemShadowFactor = 0.92F;
constexpr float kNameInputFontSizePx = 42.0F;
constexpr float kNameInputStrokePx = 3.5F;
constexpr float kNameInputShadowOffsetPx = 0.0F;
constexpr float kNameInputShadowBlurPx = 4.0F;
constexpr float kNameInputMinSqueeze = 0.85F;
constexpr float kNameInputMinScale = 0.75F;
constexpr BgraColor kNameInputFillColor{0x18, 0x36, 0x6B, 0xFF};
constexpr BgraColor kNameInputStrokeColor{0xFF, 0xFF, 0xFF, 0xFF};
constexpr BgraColor kNameInputShadowColor{0xFF, 0xFF, 0xFF, 0xD0};

// Confirmation dialog (conf.hg3) metrics & palettes.
constexpr std::uint32_t kConfSkipHSceneFrameId = 120U;
constexpr RectI kConfSkipHSceneTextRect{2, 0, 316, 24};
constexpr float kConfSkipHSceneFontSizePx = 20.0F;
constexpr BgraColor kConfSkipHSceneFillColor{0x00, 0x00, 0xFF, 0xFF};

constexpr int kConfPromptInsetX = 2;
constexpr int kConfPromptInsetY = 1;
constexpr int kConfPromptWidthMargin = 4;
constexpr int kConfPromptHeightMargin = 2;
constexpr float kConfPromptFontSizePx = 21.0F;
constexpr float kConfPromptStrokePx = 2.8F;
constexpr float kConfPromptShadowDx = 0.0F;
constexpr float kConfPromptShadowDy = 2.0F;
constexpr float kConfPromptShadowBlurPx = 2.0F;
constexpr float kConfPromptMinSqueeze = 0.85F;
constexpr float kConfPromptMinScale = 0.78F;
constexpr BgraColor kConfPromptTopFill{0xFF, 0xFF, 0xFF, 0xFF};
constexpr BgraColor kConfPromptBottomFill{0xCE, 0xD2, 0xD8, 0xFF};
constexpr BgraColor kConfPromptStrokeColor{0x2B, 0x2F, 0x43, 0xFF};
constexpr BgraColor kConfPromptShadowColor{0x2B, 0x2F, 0x43, 0x60};

constexpr RectI kConfPillEraseRect{56, 12, 87, 35};
constexpr RectI kConfPillTextRect{45, 11, 109, 37};
constexpr float kConfPillFontSizePx = 25.0F;
constexpr float kConfPillHoverShadowDx = 1.0F;
constexpr float kConfPillHoverShadowDy = 1.0F;
constexpr float kConfPillHoverShadowBlurPx = 2.0F;
constexpr BgraColor kConfPillHoverFill{0xC4, 0xE4, 0xFC, 0xFF};
constexpr BgraColor kConfPillHoverShadow{0x18, 0x2A, 0x4A, 0xB0};
constexpr BgraColor kConfPillNormalFill{0x2A, 0x30, 0x3E, 0xFF};

struct TitleButtonSpec {
    std::uint32_t frame_id = 0;
    RectI text_box{};
    float font_size_px = 0.0F;
    bool wrap = false;
    float line_spacing = kSingleLineSpacing;
    const char* label = "";
};

constexpr std::size_t kSysTitleButtonCount = 21U;
constexpr std::array<TitleButtonSpec, kSysTitleButtonCount> kSysTitleButtons{{
    {241U, RectI{4, 8, 88, 61}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing, "Back"},
    {240U, RectI{4, 8, 88, 61}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing, "Back"},
    {231U, RectI{4, 6, 173, 53}, kTitleButtonFontSizeCompact, false, kSingleLineSpacing,
     "Heroine Mode"},
    {230U, RectI{4, 6, 173, 53}, kTitleButtonFontSizeCompact, false, kSingleLineSpacing,
     "Heroine Mode"},
    {221U, RectI{4, 6, 131, 52}, kTitleButtonFontSizeCompact, false, kSingleLineSpacing,
     "Music Mode"},
    {220U, RectI{4, 6, 131, 52}, kTitleButtonFontSizeCompact, false, kSingleLineSpacing,
     "Music Mode"},
    {211U, RectI{4, 7, 131, 55}, kTitleButtonFontSizeCompact, false, kSingleLineSpacing,
     "Scene Mode"},
    {210U, RectI{4, 7, 131, 55}, kTitleButtonFontSizeCompact, false, kSingleLineSpacing,
     "Scene Mode"},
    {201U, RectI{4, 8, 123, 59}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "CG Mode"},
    {200U, RectI{4, 8, 123, 59}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "CG Mode"},
    {141U, RectI{4, 8, 134, 63}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "Exit Game"},
    {140U, RectI{4, 8, 134, 63}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "Exit Game"},
    {132U, RectI{4, 7, 127, 56}, kTitleButtonFontSizeLarge, false, kSingleLineSpacing, "Extra"},
    {131U, RectI{4, 7, 127, 56}, kTitleButtonFontSizeLarge, false, kSingleLineSpacing, "Extra"},
    {130U, RectI{4, 7, 127, 56}, kTitleButtonFontSizeLarge, false, kSingleLineSpacing, "Extra"},
    {121U, RectI{4, 8, 113, 61}, kTitleButtonFontSizeLarge, false, kSingleLineSpacing, "Config"},
    {120U, RectI{4, 8, 113, 61}, kTitleButtonFontSizeLarge, false, kSingleLineSpacing, "Config"},
    {111U, RectI{4, 7, 135, 56}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "Continue"},
    {110U, RectI{4, 7, 135, 56}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "Continue"},
    {101U, RectI{4, 8, 131, 59}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "New Game"},
    {100U, RectI{4, 8, 131, 59}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing,
     "New Game"},
}};

constexpr std::size_t kSysTitleNakedButtonCount = 6U;
constexpr std::array<TitleButtonSpec, kSysTitleNakedButtonCount> kSysTitleNakedButtons{{
    {201U, RectI{2, 1, 96, 23}, kNakedButtonFontSizePx, false, kSingleLineSpacing, "Default"},
    {200U, RectI{2, 1, 96, 23}, kNakedButtonFontSizePx, false, kSingleLineSpacing, "Default"},
    {211U, RectI{2, 1, 99, 23}, kNakedButtonFontSizePx, false, kSingleLineSpacing, "Lingerie"},
    {210U, RectI{2, 1, 99, 23}, kNakedButtonFontSizePx, false, kSingleLineSpacing, "Lingerie"},
    {221U, RectI{2, 1, 78, 23}, kNakedButtonFontSizePx, false, kSingleLineSpacing, "Nude"},
    {220U, RectI{2, 1, 78, 23}, kNakedButtonFontSizePx, false, kSingleLineSpacing, "Nude"},
}};

constexpr std::size_t kSysTitleTriButtonCount = 8U;
constexpr std::array<TitleButtonSpec, kSysTitleTriButtonCount> kSysTitleTriButtons{{
    {241U, RectI{4, 8, 88, 61}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing, "Back"},
    {240U, RectI{4, 8, 88, 61}, kTitleButtonFontSizeStandard, false, kSingleLineSpacing, "Back"},
    {221U, RectI{4, 6, 182, 128}, kTitleTriStoryFontSizeStandard, true, kTitleTriStoryLineSpacing,
     "Rei:\n\"Sniff-Sniff\nWake-Up Blowjob\"\nStory 3"},
    {220U, RectI{4, 6, 182, 128}, kTitleTriStoryFontSizeStandard, true, kTitleTriStoryLineSpacing,
     "Rei:\n\"Sniff-Sniff\nWake-Up Blowjob\"\nStory 3"},
    {211U, RectI{4, 6, 186, 128}, kTitleTriStoryFontSizeCompact, true, kTitleTriStoryLineSpacing,
     "Yuuhi:\n\"Can't Hold Back:\nHot Spring Begging\"\nStory 2"},
    {210U, RectI{4, 6, 186, 128}, kTitleTriStoryFontSizeCompact, true, kTitleTriStoryLineSpacing,
     "Yuuhi:\n\"Can't Hold Back:\nHot Spring Begging\"\nStory 2"},
    {201U, RectI{4, 6, 184, 128}, kTitleTriStoryFontSizeStandard, true, kTitleTriStoryLineSpacing,
     "Chitose:\n\"Big Sis Will\nUse Her Hands\"\nStory 1"},
    {200U, RectI{4, 6, 184, 128}, kTitleTriStoryFontSizeStandard, true, kTitleTriStoryLineSpacing,
     "Chitose:\n\"Big Sis Will\nUse Her Hands\"\nStory 1"},
}};

struct WarningBlockSpec {
    RectI erase_rect{};
    RectI text_rect{};
    float font_size_px = 0.0F;
    float line_spacing = kWarningDefaultLineSpacing;
    const char* text = "";
};

constexpr std::size_t kWarningBgBlockCount = 5U;
constexpr std::array<WarningBlockSpec, kWarningBgBlockCount> kWarningBgBlocks{{
    {RectI{370, 130, 540, 46}, RectI{240, 136, 800, 34}, kWarningFontSizeAgeNotice,
     kWarningDefaultLineSpacing, "This work may not be played by anyone under the age of 18."},
    {RectI{370, 194, 540, 112}, RectI{200, 200, 880, 102}, kWarningFontSizeFiction,
     kWarningDefaultLineSpacing,
     "This work is a work of fiction.\n"
     "All characters, organizations, and locations appearing herein are fictitious\n"
     "and have no connection whatsoever to any real persons or entities."},
    {RectI{370, 318, 540, 46}, RectI{240, 324, 800, 34}, kWarningFontSizeSupport,
     kWarningDefaultLineSpacing,
     "Technical support is not provided for environments outside Japan."},
    {RectI{208, 382, 864, 178}, RectI{160, 388, 960, 166}, kWarningFontSizeBody,
     kWarningDefaultLineSpacing,
     "All visuals, images, and audio in this product are protected by copyright law.\n"
     "Unauthorized reproduction, distribution, rental, adaptation, modification, or public "
     "transmission—\n"
     "whether committed directly by you or through involvement in the actions of others—\n"
     "may result in civil liability for damages as well as criminal penalties.\n"
     "Please exercise due caution."},
    {RectI{270, 580, 740, 122}, RectI{200, 586, 880, 106}, kWarningFontSizeBody,
     kWarningDefaultLineSpacing,
     "If you discover an infringement, please report it to the\n"
     "Ethics Organization of Computer Software (EOCS / Sofurin).\n"
     "(http://www.sofurin.org/htm/about/form/illegalcopy.htm)"},
}};

constexpr std::size_t kWarningBgTrBlockCount = 2U;
constexpr std::array<WarningBlockSpec, kWarningBgTrBlockCount> kWarningBgTrBlocks{{
    {RectI{200, 240, 880, 196}, RectI{180, 246, 920, 182}, kWarningFontSizeTrialHeadline,
     kWarningTrialHeadlineLineSpacing,
     "In the \"Amakano 2\" Trial Edition, certain staging and content\n"
     "may differ from the full retail version.\n"
     "Additionally, the adult scenes contain story spoilers.\n"
     "Please keep this in mind and enjoy the preview."},
    {RectI{280, 480, 720, 146}, RectI{220, 486, 840, 132}, kWarningFontSizeSmall,
     kWarningTrialSubtextLineSpacing,
     "This work may not be played by anyone under 18. (R18 Game)\n"
     "This work is fiction; all characters and organizations appearing herein\n"
     "have no connection whatsoever to any real persons or entities.\n"
     "Technical support is not provided outside Japan."},
}};

struct NameInputPromptSpec {
    std::uint32_t frame_id = 0;
    const char* text = "";
};

constexpr std::size_t kNameInputPromptCount = 2U;
constexpr std::array<NameInputPromptSpec, kNameInputPromptCount> kNameInputPrompts{{
    {kNameInputFirstNameFrameId, "Next, please enter your first name."},
    {kNameInputLastNameFrameId, "Please enter your last name."},
}};

struct ConfPromptSpec {
    std::uint32_t frame_id = 0;
    const char* text = "";
};

constexpr std::size_t kConfPromptCount = 11U;
constexpr std::array<ConfPromptSpec, kConfPromptCount> kConfPrompts{{
    {110U, "Jump to this scene?"},
    {109U, "Quick Save now?"},
    {108U, "Quick Load now?"},
    {107U, "Save data?"},
    {106U, "Overwrite this save data?"},
    {105U, "Reset to defaults?"},
    {104U, "Exit the game?"},
    {103U, "Delete this save data?"},
    {102U, "Move this save data?"},
    {101U, "Return to Title?"},
    {100U, "Load data?"},
}};

struct ConfPillButtonSpec {
    std::uint32_t frame_id = 0;
    bool is_hover = false;
    const char* label = "";
};

constexpr std::size_t kConfPillButtonCount = 4U;
constexpr std::array<ConfPillButtonSpec, kConfPillButtonCount> kConfPillButtons{{
    {61U, true, "No"},
    {60U, true, "Yes"},
    {51U, false, "No"},
    {50U, false, "Yes"},
}};

ama::hg3::Hg3Frame& RequireFrame(ama::hg3::Hg3Container& container, std::uint32_t frame_id,
                                 const std::string& container_name) {
    ama::hg3::Hg3Frame* frame = container.FindFrame(frame_id);
    if (frame == nullptr) {
        throw std::runtime_error("missing frame_id " + std::to_string(frame_id) + " in " +
                                 container_name);
    }
    return *frame;
}

TextStyle BuildTitleButtonStyle(const TitleButtonSpec& spec) {
    TextStyle style;
    style.font_family = kPrimaryUiFontFamily;
    style.weight = kFontWeightSemibold;
    style.italic = false;
    style.size_px = spec.font_size_px;
    style.tracking_px = 0.0F;
    style.line_spacing = spec.line_spacing;
    style.stroke_px = kTitleButtonStrokePx;
    style.shadow = true;
    style.shadow_dx = kTitleButtonShadowOffsetPx;
    style.shadow_dy = kTitleButtonShadowOffsetPx;
    style.shadow_blur_px = kTitleButtonShadowBlurPx;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.wrap = spec.wrap;
    style.min_squeeze = kTitleButtonMinSqueeze;
    style.min_scale = kTitleButtonMinScale;

    if (spec.frame_id == kTitleExtraDisabledFrameId) {
        style.fill = kTitleDisabledFill;
        style.stroke = kTitleDisabledStroke;
        style.shadow_color = kTitleDisabledShadow;
        return style;
    }
    if ((spec.frame_id % kTitleStateIdModulus) == kTitleHoverStateRemainder) {
        style.fill = kTitleHoverFill;
        style.stroke = kTitleHoverStroke;
        style.shadow_color = kTitleHoverShadow;
        return style;
    }
    style.fill = kTitleNormalFill;
    style.stroke = kTitleNormalStroke;
    style.shadow_color = kTitleNormalShadow;
    return style;
}

TextStyle BuildNakedButtonStyle(const TitleButtonSpec& spec) {
    TextStyle style;
    style.font_family = kPrimaryUiFontFamily;
    style.weight = kFontWeightBold;
    style.italic = false;
    style.size_px = spec.font_size_px;
    style.tracking_px = 0.0F;
    style.line_spacing = spec.line_spacing;
    style.stroke_px = kNakedButtonStrokePx;
    style.shadow = true;
    style.shadow_dx = kNakedButtonShadowOffsetPx;
    style.shadow_dy = kNakedButtonShadowOffsetPx;
    style.shadow_blur_px = kNakedButtonShadowBlurPx;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.wrap = spec.wrap;
    style.min_squeeze = kNakedButtonMinSqueeze;
    style.min_scale = kNakedButtonMinScale;

    if ((spec.frame_id % kTitleStateIdModulus) == kTitleHoverStateRemainder) {
        style.fill = kNakedHoverFill;
        style.stroke = kNakedHoverStroke;
        style.shadow_color = kNakedHoverShadow;
        return style;
    }
    style.fill = kNakedNormalFill;
    style.stroke = kNakedNormalStroke;
    style.shadow_color = kNakedNormalShadow;
    return style;
}

void RenderTransparentButtonTable(ama::hg3::Hg3Container& container,
                                  const std::string& container_name,
                                  const TitleButtonSpec* specs, std::size_t count,
                                  bool use_naked_palette) {
    for (std::size_t i = 0; i < count; ++i) {
        const TitleButtonSpec& spec = specs[i];
        ama::hg3::Hg3Frame& frame = RequireFrame(container, spec.frame_id, container_name);
        SpriteCanvas canvas(std::move(frame.pixels));
        canvas.Fill(RectI{0, 0, canvas.width(), canvas.height()}, kTransparentColor);
        const TextStyle style =
            use_naked_palette ? BuildNakedButtonStyle(spec) : BuildTitleButtonStyle(spec);
        canvas.DrawText(spec.label, spec.text_box, style);
        frame.pixels = canvas.image();
    }
}

MenuPatchEntrySpec RenderSysTitle(const GameAssetSource& source) {
    constexpr const char* kFilename = "sys_title.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);
    const ama::hg3::Hg3Frame& orig_logo =
        RequireFrame(container, kTitleStylizedLogoFrameId, kFilename);
    const std::vector<std::uint8_t> orig_logo_pixels = orig_logo.pixels.pixels;

    RenderTransparentButtonTable(container, kFilename, kSysTitleButtons.data(),
                                 kSysTitleButtons.size(), false);

    const ama::hg3::Hg3Frame& final_logo =
        RequireFrame(container, kTitleStylizedLogoFrameId, kFilename);
    if (final_logo.pixels.pixels != orig_logo_pixels) {
        throw std::runtime_error("stylized game logo frame was modified in sys_title.hg3");
    }
    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec RenderSysTitleNaked(const GameAssetSource& source) {
    constexpr const char* kFilename = "sys_title_naked.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);
    RenderTransparentButtonTable(container, kFilename, kSysTitleNakedButtons.data(),
                                 kSysTitleNakedButtons.size(), true);
    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec RenderSysTitleTri(const GameAssetSource& source) {
    constexpr const char* kFilename = "sys_title_tri.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);
    RenderTransparentButtonTable(container, kFilename, kSysTitleTriButtons.data(),
                                 kSysTitleTriButtons.size(), false);
    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec RenderBootSplash(const GameAssetSource& source) {
    constexpr const char* kFilename = "boot.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);
    ama::hg3::Hg3Frame& frame = RequireFrame(container, kBootSingleFrameId, kFilename);

    SpriteCanvas canvas(std::move(frame.pixels));
    canvas.EraseWithBorderInpaint(kBootEraseRect);

    TextStyle style;
    style.font_family = kPrimaryUiFontFamily;
    style.weight = kFontWeightSemibold;
    style.size_px = kBootFontSizePx;
    style.line_spacing = kBootLineSpacing;
    style.fill = kBootFillColor;
    style.stroke_px = kBootStrokePx;
    style.stroke = kBootStrokeColor;
    style.shadow = true;
    style.shadow_dx = kBootShadowDx;
    style.shadow_dy = kBootShadowDy;
    style.shadow_blur_px = kBootShadowBlurPx;
    style.shadow_color = kBootShadowColor;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.wrap = true;
    style.min_squeeze = kBootMinSqueeze;
    style.min_scale = kBootMinScale;

    canvas.DrawText(
        "*If the resolution does not fit at startup, open System Settings\n"
        "and select an appropriate window size under Display 1.",
        kBootTextRect, style);
    frame.pixels = canvas.image();
    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

void DilateMaskInRect(const std::vector<std::uint8_t>& seed_mask,
                      std::vector<std::uint8_t>& out_mask, int canvas_w, int canvas_h,
                      const RectI& region, int radius_px) {
    const int x0 = std::max(0, region.x);
    const int y0 = std::max(0, region.y);
    const int x1 = std::min(canvas_w, region.x + region.w);
    const int y1 = std::min(canvas_h, region.y + region.h);
    const int r_sq = radius_px * radius_px;
    constexpr std::uint8_t kMaskedFlag = 1U;

    for (int y = y0; y < y1; ++y) {
        const std::size_t row_off =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas_w);
        for (int x = x0; x < x1; ++x) {
            if (seed_mask[row_off + static_cast<std::size_t>(x)] == 0U) {
                continue;
            }
            const int ny0 = std::max(y0, y - radius_px);
            const int ny1 = std::min(y1 - 1, y + radius_px);
            const int nx0 = std::max(x0, x - radius_px);
            const int nx1 = std::min(x1 - 1, x + radius_px);
            for (int ny = ny0; ny <= ny1; ++ny) {
                const int dy = ny - y;
                const std::size_t nrow_off =
                    static_cast<std::size_t>(ny) * static_cast<std::size_t>(canvas_w);
                for (int nx = nx0; nx <= nx1; ++nx) {
                    const int dx = nx - x;
                    if (dx * dx + dy * dy <= r_sq) {
                        out_mask[nrow_off + static_cast<std::size_t>(nx)] = kMaskedFlag;
                    }
                }
            }
        }
    }
}

bool IsWarningInkPixel(BgraColor c) {
    const int sum = static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b);
    if (sum < kWarningInkSumThreshold) {
        return true;
    }
    return (static_cast<int>(c.r) - static_cast<int>(c.g) > kWarningRedDeltaThreshold) &&
           (static_cast<int>(c.r) - static_cast<int>(c.b) > kWarningRedDeltaThreshold);
}

bool HasNearbyWhiteOutline(const SpriteCanvas& canvas, int cx, int cy, int w, int h) {
    const BgraColor center = canvas.PixelAt(cx, cy);
    const int center_sum =
        static_cast<int>(center.r) + static_cast<int>(center.g) + static_cast<int>(center.b);
    const int x0 = std::max(0, cx - kWarningOutlineCheckRadiusPx);
    const int y0 = std::max(0, cy - kWarningOutlineCheckRadiusPx);
    const int x1 = std::min(w - 1, cx + kWarningOutlineCheckRadiusPx);
    const int y1 = std::min(h - 1, cy + kWarningOutlineCheckRadiusPx);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const BgraColor nb = canvas.PixelAt(x, y);
            const int nb_sum =
                static_cast<int>(nb.r) + static_cast<int>(nb.g) + static_cast<int>(nb.b);
            if (nb_sum >= kWarningOutlineWhiteSumMin &&
                (nb_sum - center_sum) >= kWarningOutlineContrastMin) {
                return true;
            }
        }
    }
    return false;
}

void EraseWarningGlyphsInBlocks(SpriteCanvas& canvas, const WarningBlockSpec* blocks,
                                std::size_t count) {
    const int w = canvas.width();
    const int h = canvas.height();
    const std::size_t total_px = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> seed_mask(total_px, 0U);
    std::vector<std::uint8_t> dilated_mask(total_px, 0U);
    constexpr std::uint8_t kMaskedFlag = 1U;

    for (std::size_t i = 0; i < count; ++i) {
        const RectI& rect = blocks[i].erase_rect;
        const int x0 = std::max(0, rect.x);
        const int y0 = std::max(0, rect.y);
        const int x1 = std::min(w, rect.x + rect.w);
        const int y1 = std::min(h, rect.y + rect.h);
        for (int y = y0; y < y1; ++y) {
            const std::size_t row_off =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
            for (int x = x0; x < x1; ++x) {
                const BgraColor c = canvas.PixelAt(x, y);
                if (IsWarningInkPixel(c) && HasNearbyWhiteOutline(canvas, x, y, w, h)) {
                    seed_mask[row_off + static_cast<std::size_t>(x)] = kMaskedFlag;
                }
            }
        }
        DilateMaskInRect(seed_mask, dilated_mask, w, h, rect, kWarningGlyphDilateRadiusPx);
    }

    canvas.EraseMaskedWithInpaint(dilated_mask);
}

void RenderWarningBlocks(ama::hg3::Hg3Container& container, const std::string& container_name,
                         const WarningBlockSpec* blocks, std::size_t count) {
    ama::hg3::Hg3Frame& frame = RequireFrame(container, kWarningSingleFrameId, container_name);
    SpriteCanvas canvas(std::move(frame.pixels));

    EraseWarningGlyphsInBlocks(canvas, blocks, count);

    for (std::size_t i = 0; i < count; ++i) {
        TextStyle style;
        style.font_family = kPrimaryUiFontFamily;
        style.weight = kFontWeightSemibold;
        style.size_px = blocks[i].font_size_px;
        style.line_spacing = blocks[i].line_spacing;
        style.fill = kWarningFillColor;
        style.stroke_px = kWarningStrokePx;
        style.stroke = kWarningStrokeColor;
        style.shadow = false;
        style.halign = HorizontalAlign::kCenter;
        style.valign = VerticalAlign::kCenter;
        style.wrap = true;
        style.min_squeeze = kWarningMinSqueeze;
        style.min_scale = kWarningMinScale;
        canvas.DrawText(blocks[i].text, blocks[i].text_rect, style);
    }

    frame.pixels = canvas.image();
}

MenuPatchEntrySpec RenderSysWarningBg(const GameAssetSource& source) {
    constexpr const char* kFilename = "sys_warning_bg.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);
    RenderWarningBlocks(container, kFilename, kWarningBgBlocks.data(), kWarningBgBlocks.size());
    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec RenderSysWarningBgTr(const GameAssetSource& source) {
    constexpr const char* kFilename = "sys_warning_bgtr.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);
    RenderWarningBlocks(container, kFilename, kWarningBgTrBlocks.data(),
                        kWarningBgTrBlocks.size());
    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

struct ChannelTriplet {
    float b = 0.0F;
    float g = 0.0F;
    float r = 0.0F;
};

int PixelChannelSum(BgraColor c) {
    return static_cast<int>(c.b) + static_cast<int>(c.g) + static_cast<int>(c.r);
}

std::uint8_t ClampChannelFloat(float value) {
    constexpr float kMaxChannel = 255.0F;
    constexpr float kRoundBias = 0.5F;
    const float clamped = std::clamp(value, 0.0F, kMaxChannel);
    return static_cast<std::uint8_t>(clamped + kRoundBias);
}

bool IsNameInputDarkCoreCandidate(BgraColor c) {
    const int sum = PixelChannelSum(c);
    if (sum <= kNameInputDarkCoreSumMax) {
        return true;
    }
    const int r_minus_b = static_cast<int>(c.r) - static_cast<int>(c.b);
    return r_minus_b >= kNameInputBrownRMinusBMin && sum <= kNameInputBrownCoreSumMax;
}

bool IsNameInputWhiteStrokeCandidate(BgraColor c) {
    return static_cast<int>(c.b) >= kNameInputWhiteStrokeChannelMin &&
           static_cast<int>(c.g) >= kNameInputWhiteStrokeChannelMin &&
           static_cast<int>(c.r) >= kNameInputWhiteStrokeChannelMin;
}

std::vector<std::uint8_t> BuildNameInputGlyphMask(const BgraImage& img, const RectI& rect) {
    constexpr std::uint8_t kMaskedPixel = 1U;
    constexpr std::uint8_t kUnmaskedPixel = 0U;
    if (rect.w <= 0 || rect.h <= 0) {
        return {};
    }
    const std::size_t total =
        static_cast<std::size_t>(rect.w) * static_cast<std::size_t>(rect.h);
    std::vector<std::uint8_t> dark_cand(total, kUnmaskedPixel);
    std::vector<std::uint8_t> white_cand(total, kUnmaskedPixel);

    for (int ly = 0; ly < rect.h; ++ly) {
        const int y = rect.y + ly;
        if (y < 0 || y >= img.height) {
            continue;
        }
        const std::size_t row_off =
            static_cast<std::size_t>(ly) * static_cast<std::size_t>(rect.w);
        for (int lx = 0; lx < rect.w; ++lx) {
            const int x = rect.x + lx;
            if (x < 0 || x >= img.width) {
                continue;
            }
            const BgraColor c = imaging::GetPixel(img, x, y);
            const std::size_t idx = row_off + static_cast<std::size_t>(lx);
            if (IsNameInputDarkCoreCandidate(c)) {
                dark_cand[idx] = kMaskedPixel;
            }
            if (IsNameInputWhiteStrokeCandidate(c)) {
                white_cand[idx] = kMaskedPixel;
            }
        }
    }

    auto has_neighbor = [&](const std::vector<std::uint8_t>& grid, int cx, int cy, int radius) {
        const int x0 = std::max(0, cx - radius);
        const int y0 = std::max(0, cy - radius);
        const int x1 = std::min(rect.w - 1, cx + radius);
        const int y1 = std::min(rect.h - 1, cy + radius);
        for (int ny = y0; ny <= y1; ++ny) {
            const std::size_t r_off =
                static_cast<std::size_t>(ny) * static_cast<std::size_t>(rect.w);
            for (int nx = x0; nx <= x1; ++nx) {
                if (grid[r_off + static_cast<std::size_t>(nx)] != kUnmaskedPixel) {
                    return true;
                }
            }
        }
        return false;
    };

    std::vector<std::uint8_t> verified_seed(total, kUnmaskedPixel);
    for (int ly = 0; ly < rect.h; ++ly) {
        const std::size_t row_off =
            static_cast<std::size_t>(ly) * static_cast<std::size_t>(rect.w);
        for (int lx = 0; lx < rect.w; ++lx) {
            const std::size_t idx = row_off + static_cast<std::size_t>(lx);
            const bool is_core =
                dark_cand[idx] != kUnmaskedPixel &&
                has_neighbor(white_cand, lx, ly, kNameInputCoreStrokeCheckRadiusPx);
            const bool is_stroke =
                white_cand[idx] != kUnmaskedPixel &&
                has_neighbor(dark_cand, lx, ly, kNameInputStrokeCoreCheckRadiusPx);
            if (is_core || is_stroke) {
                verified_seed[idx] = kMaskedPixel;
            }
        }
    }

    std::vector<std::uint8_t> dilated(total, kUnmaskedPixel);
    for (int ly = 0; ly < rect.h; ++ly) {
        const std::size_t row_off =
            static_cast<std::size_t>(ly) * static_cast<std::size_t>(rect.w);
        for (int lx = 0; lx < rect.w; ++lx) {
            if (has_neighbor(verified_seed, lx, ly, kNameInputGlowDilateRadiusPx)) {
                dilated[row_off + static_cast<std::size_t>(lx)] = kMaskedPixel;
            }
        }
    }
    return dilated;
}

ChannelTriplet SampleSmoothedRowColor(const BgraImage& img, int cx, int y) {
    const int x0 = std::max(0, cx - kNameInputBorderSmoothRadiusPx);
    const int x1 = std::min(img.width - 1, cx + kNameInputBorderSmoothRadiusPx);
    float sum_b = 0.0F;
    float sum_g = 0.0F;
    float sum_r = 0.0F;
    int count = 0;
    for (int x = x0; x <= x1; ++x) {
        const BgraColor c = imaging::GetPixel(img, x, y);
        sum_b += static_cast<float>(c.b);
        sum_g += static_cast<float>(c.g);
        sum_r += static_cast<float>(c.r);
        ++count;
    }
    const float inv = 1.0F / static_cast<float>(std::max(1, count));
    return ChannelTriplet{sum_b * inv, sum_g * inv, sum_r * inv};
}

void AttenuateResidualWhiteGlow(BgraImage& img, const RectI& rect) {
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(1, rect.y);
    const int x1 = std::min(img.width, rect.x + rect.w);
    const int y1 = std::min(img.height - 1, rect.y + rect.h);
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw <= 0 || rh <= 0) {
        return;
    }

    const int y_top = y0 - 1;
    const int y_bot = y1;
    const float span_y = static_cast<float>(std::max(1, y_bot - y_top));
    const float hem_span =
        static_cast<float>(std::max(1, kNameInputSkirtHemlineBotY - kNameInputSkirtHemlineTopY));
    const float bag_span =
        static_cast<float>(std::max(1, kNameInputBagBottomBotY - kNameInputBagBottomTopY));

    std::vector<ChannelTriplet> top_row(static_cast<std::size_t>(rw));
    std::vector<ChannelTriplet> bot_row(static_cast<std::size_t>(rw));
    for (int lx = 0; lx < rw; ++lx) {
        const int x = x0 + lx;
        const std::size_t idx = static_cast<std::size_t>(lx);
        top_row[idx] = SampleSmoothedRowColor(img, x, y_top);
        bot_row[idx] = SampleSmoothedRowColor(img, x, y_bot);
    }

    for (int ly = 0; ly < rh; ++ly) {
        const int y = y0 + ly;
        const float t_lin = static_cast<float>(y - y_top) / span_y;
        const float t_hem = std::clamp(
            static_cast<float>(y - kNameInputSkirtHemlineTopY) / hem_span, 0.0F, 1.0F);
        const float t_bag = std::clamp(
            static_cast<float>(y - kNameInputBagBottomTopY) / bag_span, 0.0F, 1.0F);
        for (int lx = 0; lx < rw; ++lx) {
            const int x = x0 + lx;
            const std::size_t idx = static_cast<std::size_t>(lx);
            const BgraColor c = imaging::GetPixel(img, x, y);
            const ChannelTriplet& ct = top_row[idx];
            const ChannelTriplet& cb = bot_row[idx];
            const float tb = (ct.b > cb.b) ? std::max(t_lin, t_hem) : std::min(t_lin, t_bag);
            const float tg = (ct.g > cb.g) ? std::max(t_lin, t_hem) : std::min(t_lin, t_bag);
            const float tr = (ct.r > cb.r) ? std::max(t_lin, t_hem) : std::min(t_lin, t_bag);
            const float lb = (1.0F - tb) * ct.b + tb * cb.b;
            const float lg = (1.0F - tg) * ct.g + tg * cb.g;
            const float lr = (1.0F - tr) * ct.r + tr * cb.r;
            const BgraColor de_glowed{
                ClampChannelFloat(std::min(static_cast<float>(c.b), lb)),
                ClampChannelFloat(std::min(static_cast<float>(c.g), lg)),
                ClampChannelFloat(std::min(static_cast<float>(c.r), lr)),
                imaging::kOpaqueAlpha,
            };
            imaging::SetPixel(img, x, y, de_glowed);
        }
    }
}

ChannelTriplet SampleInterpolatedRowColor(const std::vector<ChannelTriplet>& row, int x0,
                                          float x_coord) {
    const float max_idx = static_cast<float>(std::max(0, static_cast<int>(row.size()) - 1));
    const float local_x = std::clamp(x_coord - static_cast<float>(x0), 0.0F, max_idx);
    const int idx0 = static_cast<int>(local_x);
    const int idx1 = std::min(static_cast<int>(row.size()) - 1, idx0 + 1);
    const float frac = local_x - static_cast<float>(idx0);
    const float one_minus_f = 1.0F - frac;
    const ChannelTriplet& c0 = row[static_cast<std::size_t>(idx0)];
    const ChannelTriplet& c1 = row[static_cast<std::size_t>(idx1)];
    return ChannelTriplet{
        one_minus_f * c0.b + frac * c1.b,
        one_minus_f * c0.g + frac * c1.g,
        one_minus_f * c0.r + frac * c1.r,
    };
}

void ReconstructSkirtBand(BgraImage& img, const RectI& rect) {
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(1, rect.y);
    const int x1 = std::min(img.width, rect.x + rect.w);
    const int y1 = std::min(img.height - 1, rect.y + rect.h);
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw <= 0 || rh <= 0) {
        return;
    }

    const int y_top = y0 - 1;
    const int y_bot = y1;
    const float span_y = static_cast<float>(std::max(1, y_bot - y_top));
    const float hem_span =
        static_cast<float>(std::max(1, kNameInputSkirtHemlineBotY - kNameInputSkirtHemlineTopY));
    const float bag_span =
        static_cast<float>(std::max(1, kNameInputBagBottomBotY - kNameInputBagBottomTopY));
    const float skirt_span_y =
        static_cast<float>(std::max(1, y_bot - kNameInputSkirtHemlineBotY));
    const float blend_in_span_x =
        static_cast<float>(std::max(1, kNameInputSkirtBlendInEndX - kNameInputSkirtBandStartX));
    const float fade_out_span_x =
        static_cast<float>(std::max(1, kNameInputSkirtBandEndX - kNameInputSkirtFadeOutStartX));
    const float flare_width_x =
        std::max(1.0F, kNameInputSkirtRightEdgeX - kNameInputSkirtLeftEdgeX);

    std::vector<ChannelTriplet> top_row(static_cast<std::size_t>(rw));
    std::vector<ChannelTriplet> bot_row(static_cast<std::size_t>(rw));
    for (int lx = 0; lx < rw; ++lx) {
        const int x = x0 + lx;
        const std::size_t idx = static_cast<std::size_t>(lx);
        top_row[idx] = SampleSmoothedRowColor(img, x, y_top);
        bot_row[idx] = SampleSmoothedRowColor(img, x, y_bot);
    }

    const int band_x0 = std::max(x0, kNameInputSkirtBandStartX);
    const int band_x1 = std::min(x1, kNameInputSkirtBandEndX);

    for (int y = y0; y < y1; ++y) {
        const float t_lin = static_cast<float>(y - y_top) / span_y;
        const float t_hem = std::clamp(
            static_cast<float>(y - kNameInputSkirtHemlineTopY) / hem_span, 0.0F, 1.0F);
        const float t_bag = std::clamp(
            static_cast<float>(y - kNameInputBagBottomTopY) / bag_span, 0.0F, 1.0F);
        const float skirt_depth = std::clamp(
            static_cast<float>(y - kNameInputSkirtHemlineBotY) / skirt_span_y, 0.0F, 1.0F);
        const float shadow_mul =
            kNameInputSkirtHemShadowFactor +
            (1.0F - kNameInputSkirtHemShadowFactor) * skirt_depth;
        const float dy_from_bot = static_cast<float>(y_bot - y);

        for (int x = band_x0; x < band_x1; ++x) {
            const float alpha_in = std::clamp(
                static_cast<float>(x - kNameInputSkirtBandStartX) / blend_in_span_x, 0.0F, 1.0F);
            const float alpha_out = std::clamp(
                static_cast<float>(kNameInputSkirtBandEndX - x) / fade_out_span_x, 0.0F, 1.0F);
            const float alpha = std::min(alpha_in, alpha_out);
            const float one_minus_a = 1.0F - alpha;

            const float flare_t = std::clamp(
                (static_cast<float>(x) - kNameInputSkirtLeftEdgeX) / flare_width_x, 0.0F, 1.0F);
            const float x_bot =
                static_cast<float>(x) + flare_t * kNameInputSkirtRightFlareSlope * dy_from_bot;

            const ChannelTriplet& ct = top_row[static_cast<std::size_t>(x - x0)];
            const ChannelTriplet cb_raw = SampleInterpolatedRowColor(bot_row, x0, x_bot);
            const float eff_shadow = (ct.r > cb_raw.r) ? shadow_mul : 1.0F;
            const ChannelTriplet cb{
                cb_raw.b * eff_shadow,
                cb_raw.g * eff_shadow,
                cb_raw.r * eff_shadow,
            };

            const float tb = (ct.b > cb.b) ? std::max(t_lin, t_hem) : std::min(t_lin, t_bag);
            const float tg = (ct.g > cb.g) ? std::max(t_lin, t_hem) : std::min(t_lin, t_bag);
            const float tr = (ct.r > cb.r) ? std::max(t_lin, t_hem) : std::min(t_lin, t_bag);
            const float recon_b = (1.0F - tb) * ct.b + tb * cb.b;
            const float recon_g = (1.0F - tg) * ct.g + tg * cb.g;
            const float recon_r = (1.0F - tr) * ct.r + tr * cb.r;

            const BgraColor cur = imaging::GetPixel(img, x, y);
            const BgraColor blended{
                ClampChannelFloat(one_minus_a * static_cast<float>(cur.b) + alpha * recon_b),
                ClampChannelFloat(one_minus_a * static_cast<float>(cur.g) + alpha * recon_g),
                ClampChannelFloat(one_minus_a * static_cast<float>(cur.r) + alpha * recon_r),
                imaging::kOpaqueAlpha,
            };
            imaging::SetPixel(img, x, y, blended);
        }
    }
}

BgraImage BuildNameInputCleanBackground(const BgraImage& first_frame,
                                        const BgraImage& second_frame) {
    constexpr std::uint8_t kMaskedPixel = 1U;
    constexpr std::uint8_t kUnmaskedPixel = 0U;
    BgraImage clean_bg = first_frame;
    if (first_frame.width != second_frame.width || first_frame.height != second_frame.height) {
        return clean_bg;
    }

    const RectI& rect = kNameInputEraseRect;
    std::vector<std::uint8_t> mask_first = BuildNameInputGlyphMask(first_frame, rect);
    std::vector<std::uint8_t> mask_second = BuildNameInputGlyphMask(second_frame, rect);
    const std::size_t full_px =
        static_cast<std::size_t>(clean_bg.width) * static_cast<std::size_t>(clean_bg.height);
    std::vector<std::uint8_t> overlap_seed(full_px, kUnmaskedPixel);
    std::vector<std::uint8_t> overlap_dilated(full_px, kUnmaskedPixel);

    for (int ly = 0; ly < rect.h; ++ly) {
        const int y = rect.y + ly;
        if (y < 0 || y >= clean_bg.height) {
            continue;
        }
        const std::size_t local_row =
            static_cast<std::size_t>(ly) * static_cast<std::size_t>(rect.w);
        const std::size_t full_row =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(clean_bg.width);
        for (int lx = 0; lx < rect.w; ++lx) {
            const int x = rect.x + lx;
            if (x < 0 || x >= clean_bg.width) {
                continue;
            }
            const std::size_t local_idx = local_row + static_cast<std::size_t>(lx);
            const BgraColor c0 = imaging::GetPixel(first_frame, x, y);
            const BgraColor c1 = imaging::GetPixel(second_frame, x, y);
            const int diff = std::abs(static_cast<int>(c0.b) - static_cast<int>(c1.b)) +
                             std::abs(static_cast<int>(c0.g) - static_cast<int>(c1.g)) +
                             std::abs(static_cast<int>(c0.r) - static_cast<int>(c1.r));

            bool m0 = mask_first[local_idx] != kUnmaskedPixel;
            bool m1 = mask_second[local_idx] != kUnmaskedPixel;
            if (!m0 && !m1 && diff > kNameInputFrameDiffThreshold) {
                if (PixelChannelSum(c0) >= PixelChannelSum(c1)) {
                    m0 = true;
                }
                if (PixelChannelSum(c1) > PixelChannelSum(c0)) {
                    m1 = true;
                }
            }

            if (m0 && !m1) {
                imaging::SetPixel(clean_bg, x, y, c1);
            }
            if (!m0 && m1) {
                imaging::SetPixel(clean_bg, x, y, c0);
            }
            if (m0 && m1) {
                overlap_seed[full_row + static_cast<std::size_t>(x)] = kMaskedPixel;
            }
        }
    }

    DilateMaskInRect(overlap_seed, overlap_dilated, clean_bg.width, clean_bg.height, rect,
                     kNameInputOverlapDilatePx);

    AttenuateResidualWhiteGlow(clean_bg, rect);
    SpriteCanvas clean_canvas(std::move(clean_bg));
    clean_canvas.EraseMaskedWithInpaint(overlap_dilated);
    BgraImage result = clean_canvas.image();
    ReconstructSkirtBand(result, rect);
    return result;
}

MenuPatchEntrySpec RenderSysNameInput(const GameAssetSource& source) {
    constexpr const char* kFilename = "sys_nameinput.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);
    const ama::hg3::Hg3Frame& first_name_frame =
        RequireFrame(container, kNameInputFirstNameFrameId, kFilename);
    const ama::hg3::Hg3Frame& last_name_frame =
        RequireFrame(container, kNameInputLastNameFrameId, kFilename);
    const BgraImage clean_bg =
        BuildNameInputCleanBackground(first_name_frame.pixels, last_name_frame.pixels);

    TextStyle style;
    style.font_family = kPrimaryUiFontFamily;
    style.weight = kFontWeightBold;
    style.size_px = kNameInputFontSizePx;
    style.fill = kNameInputFillColor;
    style.stroke_px = kNameInputStrokePx;
    style.stroke = kNameInputStrokeColor;
    style.shadow = true;
    style.shadow_dx = kNameInputShadowOffsetPx;
    style.shadow_dy = kNameInputShadowOffsetPx;
    style.shadow_blur_px = kNameInputShadowBlurPx;
    style.shadow_color = kNameInputShadowColor;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.wrap = false;
    style.min_squeeze = kNameInputMinSqueeze;
    style.min_scale = kNameInputMinScale;

    for (const NameInputPromptSpec& spec : kNameInputPrompts) {
        ama::hg3::Hg3Frame& frame = RequireFrame(container, spec.frame_id, kFilename);
        SpriteCanvas canvas(clean_bg);
        canvas.DrawText(spec.text, kNameInputTextRect, style);
        frame.pixels = canvas.image();
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec RenderConfDialogs(const GameAssetSource& source) {
    constexpr const char* kFilename = "conf.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kFilename, &src_entry);

    {
        ama::hg3::Hg3Frame& skip_frame =
            RequireFrame(container, kConfSkipHSceneFrameId, kFilename);
        SpriteCanvas canvas(std::move(skip_frame.pixels));
        canvas.Fill(RectI{0, 0, canvas.width(), canvas.height()}, kTransparentColor);

        TextStyle skip_style;
        skip_style.font_family = kPrimaryUiFontFamily;
        skip_style.weight = kFontWeightSemibold;
        skip_style.size_px = kConfSkipHSceneFontSizePx;
        skip_style.fill = kConfSkipHSceneFillColor;
        skip_style.stroke_px = 0.0F;
        skip_style.shadow = false;
        skip_style.halign = HorizontalAlign::kCenter;
        skip_style.valign = VerticalAlign::kCenter;
        skip_style.wrap = false;
        canvas.DrawText("Skip this H-scene?", kConfSkipHSceneTextRect, skip_style);
        skip_frame.pixels = canvas.image();
    }

    TextStyle prompt_style;
    prompt_style.font_family = kPrimaryUiFontFamily;
    prompt_style.weight = kFontWeightBold;
    prompt_style.size_px = kConfPromptFontSizePx;
    prompt_style.gradient = true;
    prompt_style.fill = kConfPromptTopFill;
    prompt_style.fill_bottom = kConfPromptBottomFill;
    prompt_style.stroke_px = kConfPromptStrokePx;
    prompt_style.stroke = kConfPromptStrokeColor;
    prompt_style.shadow = true;
    prompt_style.shadow_dx = kConfPromptShadowDx;
    prompt_style.shadow_dy = kConfPromptShadowDy;
    prompt_style.shadow_blur_px = kConfPromptShadowBlurPx;
    prompt_style.shadow_color = kConfPromptShadowColor;
    prompt_style.halign = HorizontalAlign::kCenter;
    prompt_style.valign = VerticalAlign::kCenter;
    prompt_style.wrap = false;
    prompt_style.min_squeeze = kConfPromptMinSqueeze;
    prompt_style.min_scale = kConfPromptMinScale;

    for (const ConfPromptSpec& spec : kConfPrompts) {
        ama::hg3::Hg3Frame& frame = RequireFrame(container, spec.frame_id, kFilename);
        SpriteCanvas canvas(std::move(frame.pixels));
        canvas.Fill(RectI{0, 0, canvas.width(), canvas.height()}, kTransparentColor);
        const RectI safe_rect{kConfPromptInsetX, kConfPromptInsetY,
                              canvas.width() - kConfPromptWidthMargin,
                              canvas.height() - kConfPromptHeightMargin};
        canvas.DrawText(spec.text, safe_rect, prompt_style);
        frame.pixels = canvas.image();
    }

    for (const ConfPillButtonSpec& spec : kConfPillButtons) {
        ama::hg3::Hg3Frame& frame = RequireFrame(container, spec.frame_id, kFilename);
        SpriteCanvas canvas(std::move(frame.pixels));
        canvas.EraseWithBorderInpaint(kConfPillEraseRect);

        TextStyle pill_style;
        pill_style.font_family = kPrimaryUiFontFamily;
        pill_style.weight = kFontWeightSemibold;
        pill_style.size_px = kConfPillFontSizePx;
        pill_style.stroke_px = 0.0F;
        pill_style.halign = HorizontalAlign::kCenter;
        pill_style.valign = VerticalAlign::kCenter;
        pill_style.wrap = false;
        if (spec.is_hover) {
            pill_style.fill = kConfPillHoverFill;
            pill_style.shadow = true;
            pill_style.shadow_dx = kConfPillHoverShadowDx;
            pill_style.shadow_dy = kConfPillHoverShadowDy;
            pill_style.shadow_blur_px = kConfPillHoverShadowBlurPx;
            pill_style.shadow_color = kConfPillHoverShadow;
        }
        if (!spec.is_hover) {
            pill_style.fill = kConfPillNormalFill;
            pill_style.shadow = false;
        }
        canvas.DrawText(spec.label, kConfPillTextRect, pill_style);
        frame.pixels = canvas.image();
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

class TitleDialogsDomainRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "title_dialogs"; }

    std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) override {
        (void)options;
        constexpr std::size_t kTitleDomainContainerCount = 8U;
        std::vector<MenuPatchEntrySpec> records;
        records.reserve(kTitleDomainContainerCount);
        records.push_back(RenderSysTitle(source));
        records.push_back(RenderSysTitleNaked(source));
        records.push_back(RenderSysTitleTri(source));
        records.push_back(RenderBootSplash(source));
        records.push_back(RenderSysWarningBg(source));
        records.push_back(RenderSysWarningBgTr(source));
        records.push_back(RenderSysNameInput(source));
        records.push_back(RenderConfDialogs(source));
        return records;
    }
};

}  // namespace

std::unique_ptr<IMenuDomainRenderer> CreateTitleDialogsDomainRenderer() {
    return std::make_unique<TitleDialogsDomainRenderer>();
}

}  // namespace ama::menu
