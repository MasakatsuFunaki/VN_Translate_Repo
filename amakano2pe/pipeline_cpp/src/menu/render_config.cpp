// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_config.h"

#include "menu/core/sprite_canvas.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ama::menu {

namespace {

constexpr const char kSysConfigHg3[] = "sys_config.hg3";
constexpr const char kSysConfigBasicHg3[] = "sys_config_basic.hg3";
constexpr const char kSysConfigSoundHg3[] = "sys_config_sound.hg3";
constexpr const char kSysConfigEtcHg3[] = "sys_config_etc.hg3";
constexpr const char kSysConfigGageHg3[] = "sys_config_gage.hg3";
constexpr const char kSysConfigNum01Hg3[] = "sys_config_num01.hg3";
constexpr const char kSysConfigScrlHg3[] = "sys_config_scrl.hg3";

constexpr std::size_t kExpectedSysConfigFrames = 15;
constexpr std::size_t kExpectedSysConfigBasicFrames = 167;
constexpr std::size_t kExpectedSysConfigSoundFrames = 114;
constexpr std::size_t kExpectedSysConfigEtcFrames = 98;
constexpr std::size_t kExpectedSysConfigGageFrames = 4;
constexpr std::size_t kExpectedSysConfigNum01Frames = 12;
constexpr std::size_t kExpectedSysConfigScrlFrames = 12;

constexpr const wchar_t kFontSegoeUi[] = L"Segoe UI";
constexpr const wchar_t kFontSegoeUiSemibold[] = L"Segoe UI Semibold";

constexpr int kWeightSemibold = 600;
constexpr int kWeightBold = 700;

constexpr float kDefaultLineSpacing = 1.0F;
constexpr float kDefaultMinSqueeze = 0.80F;
constexpr float kDefaultMinScale = 0.60F;
constexpr float kNoStrokePx = 0.0F;

constexpr BgraColor kDarkBrownText{0x1D, 0x2E, 0x49, 0xFF};
constexpr BgraColor kHeaderSubBrown{0x22, 0x33, 0x4D, 0xFF};
constexpr BgraColor kLightCreamText{0xC8, 0xE5, 0xFC, 0xFF};
constexpr BgraColor kPureWhiteText{0xFF, 0xFF, 0xFF, 0xFF};
constexpr BgraColor kHeroineStrokeBrown{0x1D, 0x26, 0x38, 0xFF};
constexpr BgraColor kItemPaleGlow{0xD8, 0xE6, 0xF2, 0xB8};
constexpr BgraColor kGreenPillDarkText{0x30, 0x48, 0x38, 0xFF};
constexpr BgraColor kGreenPillStroke{0x28, 0x7A, 0x32, 0xD0};
constexpr BgraColor kKeyboardConnectorLine{0x75, 0x89, 0xA4, 0xFF};
constexpr BgraColor kTransparent{0x00, 0x00, 0x00, 0x00};

constexpr float kHeaderSubSizePx = 16.0F;
constexpr float kHeaderSubTrackingPx = 0.3F;

constexpr float kSectionSubSizePx = 15.0F;
constexpr float kSectionSubTrackingPx = 0.0F;

constexpr float kItemLabelSizePx = 15.0F;
constexpr float kItemLabelTrackingPx = 0.0F;

constexpr float kHeroineNameSizePx = 20.0F;
constexpr float kHeroineNameStrokePx = 2.5F;
constexpr float kHeroineNameTrackingPx = 0.2F;

constexpr float kCardItemSizePx = 13.5F;
constexpr float kCardItemStrokePx = 1.2F;
constexpr float kCardItemTrackingPx = -0.2F;

constexpr float kVoiceGroupSizePx = 15.0F;
constexpr float kVoiceGroupStrokePx = 2.2F;
constexpr float kVoiceGroupTrackingPx = 0.0F;

constexpr float kTabTextSizePx = 15.5F;
constexpr float kTabTextTrackingPx = 0.8F;

constexpr float kStandardPillTextSizePx = 15.0F;
constexpr float kStandardPillTrackingPx = 0.2F;

constexpr float kSmallPillTextSizePx = 13.5F;
constexpr float kSmallPillTrackingPx = 0.2F;

constexpr float kGreenPillTextSizePx = 14.0F;
constexpr float kGreenPillStrokePx = 1.2F;
constexpr float kGreenPillTrackingPx = 0.2F;

constexpr int kRowMedianMarginSmallPx = 4;
constexpr int kRowMedianMarginTabPx = 6;
constexpr int kConnectorGapLeftPx = 8;
constexpr int kConnectorGapRightPx = 2;
constexpr int kConnectorLineHeightPx = 1;
constexpr int kHalfDivisor = 2;
constexpr std::size_t kPatchedConfigContainerCount = 4;
constexpr std::size_t kBasicPlateRegionCount = 27;
constexpr std::size_t kSoundPlateRegionCount = 23;
constexpr std::size_t kEtcPlateRegionCount = 34;
constexpr std::uint32_t kSoundHeroineMaxTens = 2;
constexpr std::uint32_t kSoundSubGroupMaxTens = 4;

constexpr int kKeycapCol1LeftX = 318;
constexpr int kKeycapCol2LeftX = 720;
constexpr int kKeycapCol3LeftX = 1122;

constexpr std::uint32_t kFrameIdBasicPlate = 0;
constexpr std::uint32_t kFrameIdSoundPlate = 1;
constexpr std::uint32_t kFrameIdEtcPlate = 2;

constexpr std::uint32_t kFrameIdTabBasicNormal = 100;
constexpr std::uint32_t kFrameIdTabSoundNormal = 101;
constexpr std::uint32_t kFrameIdTabEtcNormal = 102;
constexpr std::uint32_t kFrameIdTabBasicActive = 200;
constexpr std::uint32_t kFrameIdTabSoundActive = 201;
constexpr std::uint32_t kFrameIdTabEtcActive = 202;

constexpr std::uint32_t kFrameIdResetNormal = 900;
constexpr std::uint32_t kFrameIdResetHover = 902;

constexpr std::uint32_t kBasicGroup1Start = 100;
constexpr std::uint32_t kBasicGroup1End = 261;
constexpr std::uint32_t kBasicGroup2Start = 300;
constexpr std::uint32_t kBasicGroup2End = 469;
constexpr std::uint32_t kBasicGroup3Start = 500;
constexpr std::uint32_t kBasicGroup3End = 653;

constexpr std::uint32_t kToggleGroupSpan = 200;
constexpr std::uint32_t kToggleStateStep = 50;
constexpr std::uint32_t kToggleStateUnselectedNormal = 0;
constexpr std::uint32_t kToggleStateUnselectedHover = 1;
constexpr std::uint32_t kToggleStateSelectedNormal = 2;
constexpr std::uint32_t kToggleStateSelectedHover = 3;

constexpr std::uint32_t kBasicBaseIdSkipAll = 100;
constexpr std::uint32_t kBasicBaseIdSkipReadOnly = 101;
constexpr std::uint32_t kBasicBaseIdDisplayFullscreen = 300;
constexpr std::uint32_t kBasicBaseIdDisplayWindowed = 301;

constexpr std::uint32_t kEtcToggleMinId = 110;
constexpr std::uint32_t kEtcToggleMaxId = 283;
constexpr std::uint32_t kEtcBaseOffsetMin = 10;
constexpr std::uint32_t kEtcBaseOffsetMax = 33;

constexpr std::uint32_t kSoundToggleUnselectedMinId = 301;
constexpr std::uint32_t kSoundToggleUnselectedMaxId = 392;
constexpr std::uint32_t kSoundToggleSelectedMinId = 401;
constexpr std::uint32_t kSoundToggleSelectedMaxId = 492;
constexpr std::uint32_t kSoundHoverModulus = 100;
constexpr std::uint32_t kSoundHoverThresholdOffset = 50;
constexpr std::uint32_t kSoundSelectedThresholdId = 400;
constexpr std::uint32_t kSoundDecimalModulus = 10;
constexpr std::uint32_t kSoundVoiceTestDigit = 5;
constexpr std::uint32_t kSoundToggleMaxDigit = 4;

constexpr std::uint32_t kSoundWhitePillOn1 = 661;
constexpr std::uint32_t kSoundWhitePillOff1 = 662;
constexpr std::uint32_t kSoundWhitePillOn2 = 671;
constexpr std::uint32_t kSoundWhitePillOff2 = 672;
constexpr std::uint32_t kSoundGreenPillOn1 = 861;
constexpr std::uint32_t kSoundGreenPillOff1 = 862;
constexpr std::uint32_t kSoundGreenPillOn2 = 871;
constexpr std::uint32_t kSoundGreenPillOff2 = 872;

constexpr std::uint32_t kParityDivisor = 2;

constexpr RectI kTabEraseRect{10, 6, 102, 32};
constexpr RectI kTabSafeRect{8, 6, 106, 32};

constexpr RectI kPillUnselectedEraseRect{12, 4, 105, 24};
constexpr RectI kPillUnselectedSafeRect{8, 3, 113, 26};
constexpr RectI kPillSelectedEraseRect{17, 10, 105, 24};
constexpr RectI kPillSelectedSafeRect{13, 9, 113, 26};

constexpr RectI kSmallPillUnselectedEraseRect{8, 3, 49, 19};
constexpr RectI kSmallPillUnselectedSafeRect{5, 2, 55, 21};
constexpr RectI kSmallPillSelectedEraseRect{10, 5, 49, 19};
constexpr RectI kSmallPillSelectedSafeRect{7, 4, 55, 21};

constexpr RectI kPlayBtnUnselectedEraseRect{14, 3, 103, 19};
constexpr RectI kPlayBtnUnselectedSafeRect{10, 2, 111, 21};
constexpr RectI kPlayBtnSelectedEraseRect{16, 5, 103, 19};
constexpr RectI kPlayBtnSelectedSafeRect{12, 4, 111, 21};

constexpr RectI kAuxGreenPillEraseRect{14, 4, 65, 23};
constexpr RectI kAuxGreenPillSafeRect{10, 3, 73, 25};

enum class StyleKind {
    kHeaderSub,
    kSectionSub,
    kItemLabel,
    kHeroineName,
    kCardItem,
    kVoiceGroup,
    kTabNormal,
    kTabActive,
    kBtnUnselNorm,
    kBtnHover,
    kBtnSelNorm,
    kBtnSelHover,
    kSmUnselNorm,
    kSmHover,
    kSmSelNorm,
    kSmSelHover,
    kAuxWhitePill,
    kAuxGreenPill,
};

enum class EraseKind {
    kRowMedian,
    kBorderInpaint,
};

struct PlateRegionSpec {
    EraseKind erase_kind;
    int sample_margin_px;
    RectI erase_rect;
    RectI safe_rect;
    StyleKind style_kind;
    const char* english_text;
    int connector_target_x;
};

TextStyle BuildTextStyle(StyleKind kind) {
    TextStyle style;
    style.font_family = kFontSegoeUiSemibold;
    style.weight = kWeightSemibold;
    style.italic = false;
    style.line_spacing = kDefaultLineSpacing;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;
    style.stroke_px = kNoStrokePx;
    style.stroke = kTransparent;
    style.shadow = false;
    style.wrap = false;

    switch (kind) {
        case StyleKind::kHeaderSub:
            style.size_px = kHeaderSubSizePx;
            style.fill = kHeaderSubBrown;
            style.tracking_px = kHeaderSubTrackingPx;
            style.halign = HorizontalAlign::kLeft;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kSectionSub:
            style.size_px = kSectionSubSizePx;
            style.fill = kDarkBrownText;
            style.tracking_px = kSectionSubTrackingPx;
            style.halign = HorizontalAlign::kLeft;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kItemLabel:
            style.size_px = kItemLabelSizePx;
            style.fill = kDarkBrownText;
            style.tracking_px = kItemLabelTrackingPx;
            style.halign = HorizontalAlign::kLeft;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kHeroineName:
            style.font_family = kFontSegoeUi;
            style.weight = kWeightBold;
            style.size_px = kHeroineNameSizePx;
            style.fill = kPureWhiteText;
            style.stroke_px = kHeroineNameStrokePx;
            style.stroke = kHeroineStrokeBrown;
            style.tracking_px = kHeroineNameTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kCardItem:
            style.size_px = kCardItemSizePx;
            style.fill = kDarkBrownText;
            style.stroke_px = kCardItemStrokePx;
            style.stroke = kItemPaleGlow;
            style.tracking_px = kCardItemTrackingPx;
            style.halign = HorizontalAlign::kLeft;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kVoiceGroup:
            style.font_family = kFontSegoeUi;
            style.weight = kWeightBold;
            style.size_px = kVoiceGroupSizePx;
            style.fill = kPureWhiteText;
            style.stroke_px = kVoiceGroupStrokePx;
            style.stroke = kHeroineStrokeBrown;
            style.tracking_px = kVoiceGroupTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kTabNormal:
            style.weight = kWeightBold;
            style.size_px = kTabTextSizePx;
            style.fill = kDarkBrownText;
            style.tracking_px = kTabTextTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kTabActive:
            style.weight = kWeightBold;
            style.size_px = kTabTextSizePx;
            style.fill = kLightCreamText;
            style.tracking_px = kTabTextTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kBtnUnselNorm:
            style.size_px = kStandardPillTextSizePx;
            style.fill = kDarkBrownText;
            style.tracking_px = kStandardPillTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kBtnHover:
        case StyleKind::kBtnSelNorm:
        case StyleKind::kBtnSelHover:
            style.size_px = kStandardPillTextSizePx;
            style.fill = kLightCreamText;
            style.tracking_px = kStandardPillTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kSmUnselNorm:
            style.size_px = kSmallPillTextSizePx;
            style.fill = kDarkBrownText;
            style.tracking_px = kSmallPillTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kSmHover:
        case StyleKind::kSmSelNorm:
        case StyleKind::kSmSelHover:
            style.size_px = kSmallPillTextSizePx;
            style.fill = kLightCreamText;
            style.tracking_px = kSmallPillTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kAuxWhitePill:
            style.size_px = kGreenPillTextSizePx;
            style.fill = kGreenPillDarkText;
            style.tracking_px = kGreenPillTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;

        case StyleKind::kAuxGreenPill:
            style.weight = kWeightBold;
            style.size_px = kGreenPillTextSizePx;
            style.fill = kPureWhiteText;
            style.stroke_px = kGreenPillStrokePx;
            style.stroke = kGreenPillStroke;
            style.tracking_px = kGreenPillTrackingPx;
            style.halign = HorizontalAlign::kCenter;
            style.valign = VerticalAlign::kCenter;
            return style;
    }
    return style;
}

TextLayoutResult ApplyEraseAndText(SpriteCanvas& canvas, EraseKind erase_kind, int sample_margin_px,
                                   const RectI& erase_rect, const RectI& safe_rect,
                                   StyleKind style_kind, const std::string& text) {
    if (erase_kind == EraseKind::kBorderInpaint) {
        canvas.EraseWithBorderInpaint(erase_rect);
    } else {
        canvas.EraseWithRowMedian(erase_rect, sample_margin_px);
    }
    return canvas.DrawText(text, safe_rect, BuildTextStyle(style_kind));
}

void ApplyEraseOnly(SpriteCanvas& canvas, EraseKind erase_kind, int sample_margin_px,
                    const RectI& erase_rect) {
    if (erase_kind == EraseKind::kBorderInpaint) {
        canvas.EraseWithBorderInpaint(erase_rect);
        return;
    }
    canvas.EraseWithRowMedian(erase_rect, sample_margin_px);
}

void ApplyPlateRegions(ama::hg3::Hg3Frame& frame, const PlateRegionSpec* specs,
                       std::size_t count) {
    SpriteCanvas canvas(std::move(frame.pixels));
    for (std::size_t i = 0; i < count; ++i) {
        const PlateRegionSpec& s = specs[i];
        ApplyEraseOnly(canvas, s.erase_kind, s.sample_margin_px, s.erase_rect);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const PlateRegionSpec& s = specs[i];
        const TextLayoutResult layout =
            canvas.DrawText(s.english_text, s.safe_rect, BuildTextStyle(s.style_kind));
        if (s.connector_target_x > 0 && layout.ink_bounds.w > 0) {
            const int line_start_x = layout.ink_bounds.x + layout.ink_bounds.w + kConnectorGapLeftPx;
            const int line_end_x = s.connector_target_x - kConnectorGapRightPx;
            if (line_end_x > line_start_x) {
                const int line_y = s.safe_rect.y + s.safe_rect.h / kHalfDivisor;
                canvas.Fill(RectI{line_start_x, line_y, line_end_x - line_start_x,
                                  kConnectorLineHeightPx},
                            kKeyboardConnectorLine);
            }
        }
    }
    frame.pixels = canvas.image();
}

void RenderTabOrResetFrame(ama::hg3::Hg3Frame& frame, const char* label, StyleKind style_kind) {
    SpriteCanvas canvas(std::move(frame.pixels));
    ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx, kTabEraseRect,
                      kTabSafeRect, style_kind, label);
    frame.pixels = canvas.image();
}

void RenderStandardToggleFrame(ama::hg3::Hg3Frame& frame, std::uint32_t state_slot,
                               const char* label) {
    SpriteCanvas canvas(std::move(frame.pixels));
    if (state_slot == kToggleStateUnselectedNormal) {
        ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx,
                          kPillUnselectedEraseRect, kPillUnselectedSafeRect,
                          StyleKind::kBtnUnselNorm, label);
        frame.pixels = canvas.image();
        return;
    }
    if (state_slot == kToggleStateUnselectedHover) {
        ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx,
                          kPillUnselectedEraseRect, kPillUnselectedSafeRect, StyleKind::kBtnHover,
                          label);
        frame.pixels = canvas.image();
        return;
    }
    if (state_slot == kToggleStateSelectedNormal) {
        ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx,
                          kPillSelectedEraseRect, kPillSelectedSafeRect, StyleKind::kBtnSelNorm,
                          label);
        frame.pixels = canvas.image();
        return;
    }
    ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx, kPillSelectedEraseRect,
                      kPillSelectedSafeRect, StyleKind::kBtnSelHover, label);
    frame.pixels = canvas.image();
}

constexpr std::array<PlateRegionSpec, kBasicPlateRegionCount> kBasicPlateRegions = {{
    {EraseKind::kBorderInpaint, 0, {152, 18, 102, 28}, {156, 19, 140, 26}, StyleKind::kHeaderSub,
     "Settings", 0},
    {EraseKind::kRowMedian, kRowMedianMarginTabPx, {1140, 6, 102, 32}, {1138, 6, 106, 32},
     StyleKind::kTabNormal, "DEFAULT", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {202, 70, 246, 28}, {204, 73, 242, 24},
     StyleKind::kSectionSub, "Message & text display", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {198, 360, 232, 24}, {200, 359, 230, 24},
     StyleKind::kSectionSub, "Display & visual effects", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {982, 360, 222, 24}, {984, 359, 218, 24},
     StyleKind::kSectionSub, "Text font settings", 0},
    {EraseKind::kBorderInpaint, 0, {102, 97, 135, 28}, {102, 100, 240, 22}, StyleKind::kItemLabel,
     "Text Speed", 0},
    {EraseKind::kBorderInpaint, 0, {102, 210, 155, 28}, {102, 213, 240, 22}, StyleKind::kItemLabel,
     "Auto Mode Speed", 0},
    {EraseKind::kBorderInpaint, 0, {509, 97, 265, 28}, {509, 100, 295, 22}, StyleKind::kItemLabel,
     "Message Window Opacity", 0},
    {EraseKind::kBorderInpaint, 0, {509, 210, 175, 28}, {509, 213, 295, 22},
     StyleKind::kItemLabel, "Skip Mode", 0},
    {EraseKind::kBorderInpaint, 0, {509, 273, 230, 28}, {509, 276, 295, 22},
     StyleKind::kItemLabel, "Instant Read Text", 0},
    {EraseKind::kBorderInpaint, 0, {925, 86, 295, 28}, {925, 90, 300, 22}, StyleKind::kItemLabel,
     "Stop Voice on Click", 0},
    {EraseKind::kBorderInpaint, 0, {925, 149, 250, 28}, {925, 153, 300, 22},
     StyleKind::kItemLabel, "Continue Auto After Choices", 0},
    {EraseKind::kBorderInpaint, 0, {925, 212, 220, 28}, {925, 216, 300, 22},
     StyleKind::kItemLabel, "Continue Skip After Choices", 0},
    {EraseKind::kBorderInpaint, 0, {925, 275, 260, 28}, {925, 279, 300, 22},
     StyleKind::kItemLabel, "Wait for Voice in Auto Mode", 0},
    {EraseKind::kBorderInpaint, 0, {102, 389, 100, 22}, {102, 388, 260, 22}, StyleKind::kItemLabel,
     "Display Mode", 0},
    {EraseKind::kBorderInpaint, 0, {102, 446, 195, 28}, {102, 451, 260, 22}, StyleKind::kItemLabel,
     "Instant Screen Transitions", 0},
    {EraseKind::kBorderInpaint, 0, {102, 509, 215, 28}, {102, 514, 260, 22}, StyleKind::kItemLabel,
     "Auto-Move Mouse Cursor", 0},
    {EraseKind::kBorderInpaint, 0, {102, 572, 215, 28}, {102, 577, 260, 22}, StyleKind::kItemLabel,
     "Auto-Hide Mouse Cursor", 0},
    {EraseKind::kBorderInpaint, 0, {102, 635, 155, 28}, {102, 640, 260, 22}, StyleKind::kItemLabel,
     "Text-to-Speech", 0},
    {EraseKind::kBorderInpaint, 0, {509, 383, 175, 28}, {509, 388, 285, 22},
     StyleKind::kItemLabel, "Show Pubic Hair", 0},
    {EraseKind::kBorderInpaint, 0, {509, 446, 255, 28}, {509, 451, 285, 22},
     StyleKind::kItemLabel, "Climax Countdown System", 0},
    {EraseKind::kBorderInpaint, 0, {509, 509, 300, 28}, {509, 514, 290, 22},
     StyleKind::kItemLabel, "H-Scene Jump (Tease Mode)", 0},
    {EraseKind::kBorderInpaint, 0, {509, 572, 210, 28}, {509, 577, 285, 22},
     StyleKind::kItemLabel, "Show Heart Eyes", 0},
    {EraseKind::kBorderInpaint, 0, {509, 635, 195, 28}, {509, 640, 285, 22},
     StyleKind::kItemLabel, "Eye Blink & Lip Sync", 0},
    {EraseKind::kBorderInpaint, 0, {925, 383, 165, 28}, {925, 388, 280, 22},
     StyleKind::kItemLabel, "Active Font", 0},
    {EraseKind::kBorderInpaint, 0, {925, 509, 190, 28}, {925, 514, 280, 22},
     StyleKind::kItemLabel, "Bold Font", 0},
    {EraseKind::kBorderInpaint, 0, {925, 572, 175, 28}, {925, 577, 280, 22},
     StyleKind::kItemLabel, "Text Outline", 0},
}};

constexpr std::array<PlateRegionSpec, kSoundPlateRegionCount> kSoundPlateRegions = {{
    {EraseKind::kBorderInpaint, 0, {152, 18, 102, 28}, {156, 19, 140, 26}, StyleKind::kHeaderSub,
     "Settings", 0},
    {EraseKind::kRowMedian, kRowMedianMarginTabPx, {1140, 6, 102, 32}, {1138, 6, 106, 32},
     StyleKind::kTabNormal, "DEFAULT", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {180, 75, 192, 24}, {182, 74, 190, 24},
     StyleKind::kSectionSub, "Audio volume levels", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {638, 75, 304, 24}, {640, 74, 300, 24},
     StyleKind::kSectionSub, "Individual character voice settings", 0},
    {EraseKind::kBorderInpaint, 0, {102, 126, 94, 28}, {102, 128, 240, 24}, StyleKind::kItemLabel,
     "Master Volume", 0},
    {EraseKind::kBorderInpaint, 0, {102, 240, 94, 28}, {102, 242, 240, 24}, StyleKind::kItemLabel,
     "BGM", 0},
    {EraseKind::kBorderInpaint, 0, {102, 354, 80, 28}, {102, 356, 240, 24}, StyleKind::kItemLabel,
     "Sound Effects", 0},
    {EraseKind::kBorderInpaint, 0, {102, 467, 60, 28}, {102, 469, 240, 24}, StyleKind::kItemLabel,
     "Voice", 0},
    {EraseKind::kBorderInpaint, 0, {102, 581, 178, 28}, {102, 583, 240, 24}, StyleKind::kItemLabel,
     "BGM (During Voice)", 0},
    {EraseKind::kBorderInpaint, 0, {604, 382, 148, 44}, {578, 387, 200, 34},
     StyleKind::kHeroineName, "Yuuhi Kurohime", 0},
    {EraseKind::kBorderInpaint, 0, {818, 382, 180, 44}, {807, 387, 200, 34},
     StyleKind::kHeroineName, "Chitose Tsutamachi", 0},
    {EraseKind::kBorderInpaint, 0, {1062, 382, 148, 44}, {1036, 387, 200, 34},
     StyleKind::kHeroineName, "Rei Himiyama", 0},
    {EraseKind::kBorderInpaint, 0, {582, 441, 72, 25}, {584, 443, 68, 22}, StyleKind::kCardItem,
     "Voice", 0},
    {EraseKind::kBorderInpaint, 0, {582, 471, 72, 25}, {584, 473, 68, 22}, StyleKind::kCardItem,
     "Sys Voice", 0},
    {EraseKind::kBorderInpaint, 0, {582, 501, 72, 25}, {584, 503, 68, 22}, StyleKind::kCardItem,
     "Test", 0},
    {EraseKind::kBorderInpaint, 0, {811, 441, 72, 25}, {813, 443, 68, 22}, StyleKind::kCardItem,
     "Voice", 0},
    {EraseKind::kBorderInpaint, 0, {811, 471, 72, 25}, {813, 473, 68, 22}, StyleKind::kCardItem,
     "Sys Voice", 0},
    {EraseKind::kBorderInpaint, 0, {811, 501, 72, 25}, {813, 503, 68, 22}, StyleKind::kCardItem,
     "Test", 0},
    {EraseKind::kBorderInpaint, 0, {1040, 441, 72, 25}, {1042, 443, 68, 22}, StyleKind::kCardItem,
     "Voice", 0},
    {EraseKind::kBorderInpaint, 0, {1040, 471, 72, 25}, {1042, 473, 68, 22}, StyleKind::kCardItem,
     "Sys Voice", 0},
    {EraseKind::kBorderInpaint, 0, {1040, 501, 72, 25}, {1042, 503, 68, 22}, StyleKind::kCardItem,
     "Test", 0},
    {EraseKind::kBorderInpaint, 0, {606, 606, 180, 34}, {580, 610, 202, 26},
     StyleKind::kVoiceGroup, "Male & Other Voices", 0},
    {EraseKind::kBorderInpaint, 0, {806, 606, 208, 34}, {808, 610, 204, 26},
     StyleKind::kVoiceGroup, "Female Sub-Characters", 0},
}};

constexpr std::array<PlateRegionSpec, kEtcPlateRegionCount> kEtcPlateRegions = {{
    {EraseKind::kBorderInpaint, 0, {152, 18, 102, 28}, {156, 19, 140, 26}, StyleKind::kHeaderSub,
     "Settings", 0},
    {EraseKind::kRowMedian, kRowMedianMarginTabPx, {1140, 6, 102, 32}, {1138, 6, 106, 32},
     StyleKind::kTabNormal, "DEFAULT", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {180, 70, 290, 28}, {182, 73, 286, 24},
     StyleKind::kSectionSub, "Confirmation dialog prompts", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {244, 308, 330, 24}, {246, 307, 324, 24},
     StyleKind::kSectionSub, "In-game notification overlays", 0},
    {EraseKind::kRowMedian, kRowMedianMarginSmallPx, {212, 414, 288, 24}, {214, 413, 284, 24},
     StyleKind::kSectionSub, "Keyboard shortcut reference", 0},
    {EraseKind::kBorderInpaint, 0, {102, 94, 220, 28}, {102, 96, 260, 22}, StyleKind::kItemLabel,
     "Confirm Save", 0},
    {EraseKind::kBorderInpaint, 0, {102, 157, 220, 28}, {102, 159, 260, 22}, StyleKind::kItemLabel,
     "Confirm Load", 0},
    {EraseKind::kBorderInpaint, 0, {102, 220, 225, 28}, {102, 222, 260, 22}, StyleKind::kItemLabel,
     "Confirm Quick Save", 0},
    {EraseKind::kBorderInpaint, 0, {514, 94, 225, 28}, {514, 96, 265, 22}, StyleKind::kItemLabel,
     "Confirm Quick Load", 0},
    {EraseKind::kBorderInpaint, 0, {514, 157, 235, 28}, {514, 159, 265, 22},
     StyleKind::kItemLabel, "Confirm Overwrite Save", 0},
    {EraseKind::kBorderInpaint, 0, {514, 220, 225, 28}, {514, 222, 265, 22},
     StyleKind::kItemLabel, "Confirm Move Save", 0},
    {EraseKind::kBorderInpaint, 0, {925, 94, 225, 28}, {925, 96, 265, 22}, StyleKind::kItemLabel,
     "Confirm Delete Save", 0},
    {EraseKind::kBorderInpaint, 0, {925, 157, 225, 28}, {925, 159, 265, 22},
     StyleKind::kItemLabel, "Confirm Return to Title", 0},
    {EraseKind::kBorderInpaint, 0, {925, 220, 225, 28}, {925, 222, 265, 22},
     StyleKind::kItemLabel, "Confirm Exit Game", 0},
    {EraseKind::kBorderInpaint, 0, {102, 329, 180, 28}, {102, 332, 260, 22}, StyleKind::kItemLabel,
     "Show Date", 0},
    {EraseKind::kBorderInpaint, 0, {514, 329, 180, 28}, {514, 332, 265, 22},
     StyleKind::kItemLabel, "Show BGM Title", 0},
    {EraseKind::kBorderInpaint, 0, {925, 329, 180, 28}, {925, 332, 265, 22},
     StyleKind::kItemLabel, "Show Mode Status", 0},
    {EraseKind::kBorderInpaint, 0, {102, 444, 214, 28}, {102, 448, 195, 22}, StyleKind::kItemLabel,
     "Advance Text", kKeycapCol1LeftX},
    {EraseKind::kBorderInpaint, 0, {102, 475, 214, 52}, {102, 491, 195, 22}, StyleKind::kItemLabel,
     "Hold to Skip Text", kKeycapCol1LeftX},
    {EraseKind::kBorderInpaint, 0, {102, 532, 214, 28}, {102, 534, 195, 22}, StyleKind::kItemLabel,
     "System Menu", kKeycapCol1LeftX},
    {EraseKind::kBorderInpaint, 0, {102, 576, 214, 28}, {102, 577, 195, 22}, StyleKind::kItemLabel,
     "Quick Save", kKeycapCol1LeftX},
    {EraseKind::kBorderInpaint, 0, {102, 620, 214, 28}, {102, 619, 195, 22}, StyleKind::kItemLabel,
     "Quick Load", kKeycapCol1LeftX},
    {EraseKind::kBorderInpaint, 0, {102, 657, 214, 28}, {102, 662, 195, 22}, StyleKind::kItemLabel,
     "Toggle Skip Mode", kKeycapCol1LeftX},
    {EraseKind::kBorderInpaint, 0, {504, 444, 214, 28}, {506, 448, 190, 22},
     StyleKind::kItemLabel, "Auto Speed Up", kKeycapCol2LeftX},
    {EraseKind::kBorderInpaint, 0, {504, 488, 214, 28}, {506, 491, 190, 22},
     StyleKind::kItemLabel, "Auto Speed Down", kKeycapCol2LeftX},
    {EraseKind::kBorderInpaint, 0, {504, 532, 214, 28}, {506, 534, 190, 22},
     StyleKind::kItemLabel, "Previous Choice", kKeycapCol2LeftX},
    {EraseKind::kBorderInpaint, 0, {504, 576, 214, 28}, {506, 577, 190, 22},
     StyleKind::kItemLabel, "Next Choice", kKeycapCol2LeftX},
    {EraseKind::kBorderInpaint, 0, {504, 620, 214, 28}, {506, 619, 190, 22},
     StyleKind::kItemLabel, "H-Scene Jump", kKeycapCol2LeftX},
    {EraseKind::kBorderInpaint, 0, {504, 657, 214, 28}, {506, 662, 190, 22},
     StyleKind::kItemLabel, "Show Backlog", kKeycapCol2LeftX},
    {EraseKind::kBorderInpaint, 0, {906, 444, 214, 28}, {908, 448, 190, 22},
     StyleKind::kItemLabel, "Exit Game", kKeycapCol3LeftX},
    {EraseKind::kBorderInpaint, 0, {906, 475, 214, 52}, {908, 491, 190, 22},
     StyleKind::kItemLabel, "Toggle Fullscreen", kKeycapCol3LeftX},
    {EraseKind::kBorderInpaint, 0, {906, 532, 214, 28}, {908, 534, 190, 22},
     StyleKind::kItemLabel, "Minimize Window", kKeycapCol3LeftX},
    {EraseKind::kBorderInpaint, 0, {906, 576, 214, 28}, {908, 577, 190, 22},
     StyleKind::kItemLabel, "Move Mouse Cursor", kKeycapCol3LeftX},
    {EraseKind::kBorderInpaint, 0, {906, 620, 214, 28}, {908, 619, 190, 22},
     StyleKind::kItemLabel, "Hide Message Window", kKeycapCol3LeftX},
}};

MenuPatchEntrySpec PatchSysConfigContainer(const GameAssetSource& source,
                                           const DomainRenderOptions& options) {
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kSysConfigHg3, &src_entry);
    if (container.frames.size() != kExpectedSysConfigFrames) {
        throw std::runtime_error("unexpected frame count in sys_config.hg3");
    }

    for (ama::hg3::Hg3Frame& frame : container.frames) {
        if (frame.frame_id == kFrameIdTabBasicNormal) {
            RenderTabOrResetFrame(frame, "BASIC", StyleKind::kTabNormal);
            continue;
        }
        if (frame.frame_id == kFrameIdTabSoundNormal) {
            RenderTabOrResetFrame(frame, "SOUND", StyleKind::kTabNormal);
            continue;
        }
        if (frame.frame_id == kFrameIdTabEtcNormal) {
            RenderTabOrResetFrame(frame, "ETC.", StyleKind::kTabNormal);
            continue;
        }
        if (frame.frame_id == kFrameIdTabBasicActive) {
            RenderTabOrResetFrame(frame, "BASIC", StyleKind::kTabActive);
            continue;
        }
        if (frame.frame_id == kFrameIdTabSoundActive) {
            RenderTabOrResetFrame(frame, "SOUND", StyleKind::kTabActive);
            continue;
        }
        if (frame.frame_id == kFrameIdTabEtcActive) {
            RenderTabOrResetFrame(frame, "ETC.", StyleKind::kTabActive);
            continue;
        }
        if (frame.frame_id == kFrameIdBasicPlate) {
            ApplyPlateRegions(frame, kBasicPlateRegions.data(), kBasicPlateRegions.size());
            if (options.export_contact_sheets && !options.contact_sheet_dir.empty()) {
                SpriteCanvas(frame.pixels)
                    .SavePng(options.contact_sheet_dir / "sys_config_plate_id0_basic.png");
            }
            continue;
        }
        if (frame.frame_id == kFrameIdSoundPlate) {
            ApplyPlateRegions(frame, kSoundPlateRegions.data(), kSoundPlateRegions.size());
            if (options.export_contact_sheets && !options.contact_sheet_dir.empty()) {
                SpriteCanvas(frame.pixels)
                    .SavePng(options.contact_sheet_dir / "sys_config_plate_id1_sound.png");
            }
            continue;
        }
        if (frame.frame_id == kFrameIdEtcPlate) {
            ApplyPlateRegions(frame, kEtcPlateRegions.data(), kEtcPlateRegions.size());
            if (options.export_contact_sheets && !options.contact_sheet_dir.empty()) {
                SpriteCanvas(frame.pixels)
                    .SavePng(options.contact_sheet_dir / "sys_config_plate_id2_etc.png");
            }
            continue;
        }
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

bool IsBasicToggleFrameId(std::uint32_t frame_id) noexcept {
    if (frame_id >= kBasicGroup1Start && frame_id <= kBasicGroup1End) {
        const std::uint32_t offset = (frame_id - kBasicGroup1Start) % kToggleStateStep;
        return offset <= (kBasicGroup1End - kBasicGroup1Start) % kToggleStateStep;
    }
    if (frame_id >= kBasicGroup2Start && frame_id <= kBasicGroup2End) {
        const std::uint32_t offset = (frame_id - kBasicGroup2Start) % kToggleStateStep;
        return offset <= (kBasicGroup2End - kBasicGroup2Start) % kToggleStateStep;
    }
    if (frame_id >= kBasicGroup3Start && frame_id <= kBasicGroup3End) {
        const std::uint32_t offset = (frame_id - kBasicGroup3Start) % kToggleStateStep;
        return offset <= (kBasicGroup3End - kBasicGroup3Start) % kToggleStateStep;
    }
    return false;
}

const char* ResolveBasicToggleLabel(std::uint32_t base_id) noexcept {
    if (base_id == kBasicBaseIdSkipAll) {
        return "All";
    }
    if (base_id == kBasicBaseIdSkipReadOnly) {
        return "Read Only";
    }
    if (base_id == kBasicBaseIdDisplayFullscreen) {
        return "Fullscreen";
    }
    if (base_id == kBasicBaseIdDisplayWindowed) {
        return "Windowed";
    }
    if ((base_id % kParityDivisor) == 0) {
        return "On";
    }
    return "Off";
}

MenuPatchEntrySpec PatchSysConfigBasicContainer(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kSysConfigBasicHg3, &src_entry);
    if (container.frames.size() != kExpectedSysConfigBasicFrames) {
        throw std::runtime_error("unexpected frame count in sys_config_basic.hg3");
    }

    for (ama::hg3::Hg3Frame& frame : container.frames) {
        const std::uint32_t id = frame.frame_id;
        if (id == kFrameIdResetNormal) {
            RenderTabOrResetFrame(frame, "DEFAULT", StyleKind::kTabNormal);
            continue;
        }
        if (id == kFrameIdResetHover) {
            RenderTabOrResetFrame(frame, "DEFAULT", StyleKind::kTabActive);
            continue;
        }
        if (!IsBasicToggleFrameId(id)) {
            continue;
        }
        const std::uint32_t state_slot =
            ((id - kBasicGroup1Start) % kToggleGroupSpan) / kToggleStateStep;
        const std::uint32_t base_id = id - state_slot * kToggleStateStep;
        RenderStandardToggleFrame(frame, state_slot, ResolveBasicToggleLabel(base_id));
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

bool IsSoundSmallToggleOrTestId(std::uint32_t id) noexcept {
    const bool in_unselected =
        (id >= kSoundToggleUnselectedMinId && id <= kSoundToggleUnselectedMaxId);
    const bool in_selected = (id >= kSoundToggleSelectedMinId && id <= kSoundToggleSelectedMaxId);
    if (!in_unselected && !in_selected) {
        return false;
    }
    const std::uint32_t offset = id % kToggleStateStep;
    const std::uint32_t tens = offset / kSoundDecimalModulus;
    const std::uint32_t digit = offset % kSoundDecimalModulus;
    if (digit == 0 || digit > kSoundVoiceTestDigit) {
        return false;
    }
    if (digit == kSoundVoiceTestDigit) {
        return tens <= kSoundHeroineMaxTens;
    }
    if (tens <= kSoundHeroineMaxTens) {
        return digit <= kSoundToggleMaxDigit;
    }
    return tens <= kSoundSubGroupMaxTens && digit <= kParityDivisor;
}

void RenderSoundSmallButton(ama::hg3::Hg3Frame& frame) {
    const std::uint32_t id = frame.frame_id;
    const bool is_selected = (id >= kSoundSelectedThresholdId);
    const bool is_hover = ((id % kSoundHoverModulus) >= kSoundHoverThresholdOffset);
    const std::uint32_t digit = id % kSoundDecimalModulus;
    const bool is_play_test = (digit == kSoundVoiceTestDigit);
    const char* label = is_play_test ? "Play" : (((digit % kParityDivisor) == 1) ? "On" : "Off");

    StyleKind style_kind = StyleKind::kSmUnselNorm;
    if (!is_selected && is_hover) {
        style_kind = StyleKind::kSmHover;
    } else if (is_selected && !is_hover) {
        style_kind = StyleKind::kSmSelNorm;
    } else if (is_selected && is_hover) {
        style_kind = StyleKind::kSmSelHover;
    }

    SpriteCanvas canvas(std::move(frame.pixels));
    if (is_play_test) {
        const RectI& erase_rect =
            is_selected ? kPlayBtnSelectedEraseRect : kPlayBtnUnselectedEraseRect;
        const RectI& safe_rect =
            is_selected ? kPlayBtnSelectedSafeRect : kPlayBtnUnselectedSafeRect;
        ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx, erase_rect,
                          safe_rect, style_kind, label);
        frame.pixels = canvas.image();
        return;
    }

    const RectI& erase_rect =
        is_selected ? kSmallPillSelectedEraseRect : kSmallPillUnselectedEraseRect;
    const RectI& safe_rect =
        is_selected ? kSmallPillSelectedSafeRect : kSmallPillUnselectedSafeRect;
    ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginSmallPx, erase_rect, safe_rect,
                      style_kind, label);
    frame.pixels = canvas.image();
}

MenuPatchEntrySpec PatchSysConfigSoundContainer(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kSysConfigSoundHg3, &src_entry);
    if (container.frames.size() != kExpectedSysConfigSoundFrames) {
        throw std::runtime_error("unexpected frame count in sys_config_sound.hg3");
    }

    for (ama::hg3::Hg3Frame& frame : container.frames) {
        const std::uint32_t id = frame.frame_id;
        if (id == kFrameIdResetNormal) {
            RenderTabOrResetFrame(frame, "DEFAULT", StyleKind::kTabNormal);
            continue;
        }
        if (id == kFrameIdResetHover) {
            RenderTabOrResetFrame(frame, "DEFAULT", StyleKind::kTabActive);
            continue;
        }
        if (id == kSoundWhitePillOn1 || id == kSoundWhitePillOff1 || id == kSoundWhitePillOn2 ||
            id == kSoundWhitePillOff2) {
            const char* label = ((id % kParityDivisor) == 1) ? "On" : "Off";
            SpriteCanvas canvas(std::move(frame.pixels));
            ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx,
                              kAuxGreenPillEraseRect, kAuxGreenPillSafeRect,
                              StyleKind::kAuxWhitePill, label);
            frame.pixels = canvas.image();
            continue;
        }
        if (id == kSoundGreenPillOn1 || id == kSoundGreenPillOff1 || id == kSoundGreenPillOn2 ||
            id == kSoundGreenPillOff2) {
            const char* label = ((id % kParityDivisor) == 1) ? "On" : "Off";
            SpriteCanvas canvas(std::move(frame.pixels));
            ApplyEraseAndText(canvas, EraseKind::kRowMedian, kRowMedianMarginTabPx,
                              kAuxGreenPillEraseRect, kAuxGreenPillSafeRect,
                              StyleKind::kAuxGreenPill, label);
            frame.pixels = canvas.image();
            continue;
        }
        if (IsSoundSmallToggleOrTestId(id)) {
            RenderSoundSmallButton(frame);
        }
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

bool IsEtcToggleFrameId(std::uint32_t id) noexcept {
    if (id < kEtcToggleMinId || id > kEtcToggleMaxId) {
        return false;
    }
    const std::uint32_t offset = (id - kBasicGroup1Start) % kToggleStateStep;
    return offset >= kEtcBaseOffsetMin && offset <= kEtcBaseOffsetMax;
}

MenuPatchEntrySpec PatchSysConfigEtcContainer(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container container = source.LoadHg3(kSysConfigEtcHg3, &src_entry);
    if (container.frames.size() != kExpectedSysConfigEtcFrames) {
        throw std::runtime_error("unexpected frame count in sys_config_etc.hg3");
    }

    for (ama::hg3::Hg3Frame& frame : container.frames) {
        const std::uint32_t id = frame.frame_id;
        if (id == kFrameIdResetNormal) {
            RenderTabOrResetFrame(frame, "DEFAULT", StyleKind::kTabNormal);
            continue;
        }
        if (id == kFrameIdResetHover) {
            RenderTabOrResetFrame(frame, "DEFAULT", StyleKind::kTabActive);
            continue;
        }
        if (!IsEtcToggleFrameId(id)) {
            continue;
        }
        const std::uint32_t state_slot = (id - kBasicGroup1Start) / kToggleStateStep;
        const char* label = ((id % kParityDivisor) == 0) ? "On" : "Off";
        RenderStandardToggleFrame(frame, state_slot, label);
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

void VerifyPureArtConfigContainers(const GameAssetSource& source) {
    const ama::hg3::Hg3Container gage = source.LoadHg3(kSysConfigGageHg3);
    if (gage.frames.size() != kExpectedSysConfigGageFrames) {
        throw std::runtime_error("unexpected frame count in sys_config_gage.hg3");
    }
    const ama::hg3::Hg3Container num01 = source.LoadHg3(kSysConfigNum01Hg3);
    if (num01.frames.size() != kExpectedSysConfigNum01Frames) {
        throw std::runtime_error("unexpected frame count in sys_config_num01.hg3");
    }
    const ama::hg3::Hg3Container scrl = source.LoadHg3(kSysConfigScrlHg3);
    if (scrl.frames.size() != kExpectedSysConfigScrlFrames) {
        throw std::runtime_error("unexpected frame count in sys_config_scrl.hg3");
    }
}

class ConfigDomainRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "config"; }

    std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) override {
        VerifyPureArtConfigContainers(source);

        std::vector<MenuPatchEntrySpec> entries;
        entries.reserve(kPatchedConfigContainerCount);
        entries.push_back(PatchSysConfigContainer(source, options));
        entries.push_back(PatchSysConfigBasicContainer(source));
        entries.push_back(PatchSysConfigSoundContainer(source));
        entries.push_back(PatchSysConfigEtcContainer(source));
        return entries;
    }
};

}  // namespace

std::unique_ptr<IMenuDomainRenderer> CreateConfigDomainRenderer() {
    return std::make_unique<ConfigDomainRenderer>();
}

}  // namespace ama::menu
