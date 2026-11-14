// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_hud_saveload.h"

#include "hg3/hg3_codec.h"
#include "menu/core/sprite_canvas.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ama::menu {

namespace {

constexpr BgraColor Rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                         std::uint8_t a = imaging::kOpaqueAlpha) noexcept {
    return BgraColor{b, g, r, a};
}

constexpr wchar_t kDefaultUiFontFamily[] = L"Segoe UI";
constexpr int kFontWeightBold = 700;

constexpr BgraColor kTransparentColor = Rgba(0x00, 0x00, 0x00, 0x00);
constexpr BgraColor kToastFillTopColor = Rgba(0xFF, 0xFF, 0xFF, 0xFF);
constexpr BgraColor kToastFillBottomColor = Rgba(0xDC, 0xD4, 0xCE, 0xFF);
constexpr BgraColor kToastStrokeColor = Rgba(0x3C, 0x27, 0x22, 0xFF);
constexpr BgraColor kToastShadowColor = Rgba(0x00, 0x00, 0x00, 0x60);

constexpr BgraColor kSaveLoadTabNormalTextColor = Rgba(0x41, 0x26, 0x18, 0xFF);
constexpr BgraColor kSaveLoadTabHoverTextColor = Rgba(0xF9, 0xD5, 0xA7, 0xFF);

constexpr float kToastFontSizePx = 18.0F;
constexpr float kDateSlashFontSizePx = 19.0F;
constexpr float kBgmToastFontSizePx = 16.5F;
constexpr float kSaveLoadTabFontSizePx = 16.0F;

constexpr float kToastStrokeWidthPx = 2.5F;
constexpr float kBgmToastStrokeWidthPx = 2.2F;
constexpr float kToastShadowOffsetX = 0.0F;
constexpr float kToastShadowOffsetY = 1.0F;
constexpr float kToastShadowBlurPx = 1.5F;
constexpr float kSaveLoadTabTrackingPx = 0.5F;

constexpr float kDefaultMinSqueeze = 0.80F;
constexpr float kDefaultMinScale = 0.60F;
constexpr float kUnifiedBgmMinScale = 0.98F;
constexpr float kUnifiedBgmMinSqueeze = 0.95F;

// Backlog screen (sys_log.hg3) constants.
constexpr const char kSysLogFilename[] = "sys_log.hg3";
constexpr std::size_t kSysLogExpectedFrameCount = 35;
constexpr std::uint32_t kSysLogBackgroundFrameId = 0;
constexpr RectI kSysLogSubtitleEraseRect{96, 81, 100, 27};
constexpr RectI kSysLogCleanDonorRect{300, 81, 100, 27};

// Top-left toast overlay (sys_info.hg3) constants.
constexpr const char kSysInfoFilename[] = "sys_info.hg3";
constexpr std::size_t kSysInfoExpectedFrameCount = 30;
constexpr std::uint32_t kSysInfoDateFrameId = 200;
constexpr RectI kSysInfoDateSlashTextRect{2, 0, 26, 30};

constexpr int kSysInfoToastPadX = 2;
constexpr int kSysInfoToastPadY = 1;
constexpr int kSysInfoToastWidthTrim = 4;
constexpr int kSysInfoToastHeightTrim = 2;

constexpr int kSysInfoBgmNoteProtectedWidth = 33;
constexpr int kSysInfoBgmTextLeftX = 34;
constexpr int kSysInfoBgmTextTopY = 2;
constexpr int kSysInfoBgmTextWidthTrim = 36;
constexpr int kSysInfoBgmTextHeight = 30;

// Save/Load base screen (sl_base.hg3) constants.
constexpr const char kSaveLoadBaseFilename[] = "sl_base.hg3";
constexpr std::size_t kSaveLoadBaseExpectedFrameCount = 162;
constexpr std::uint32_t kSaveLoadExpectedImgMode = 1;

constexpr std::uint32_t kSaveBgFrameId = 0;
constexpr std::uint32_t kLoadBgFrameId = 1;
constexpr std::uint32_t kLoadTabNormalFrameId = 203;
constexpr std::uint32_t kSaveTabNormalFrameId = 204;
constexpr std::uint32_t kLoadTabHoverFrameId = 223;
constexpr std::uint32_t kSaveTabHoverFrameId = 224;

constexpr RectI kSaveLoadTabEraseRect{20, 6, 82, 31};
constexpr RectI kSaveLoadTabTextRect{20, 8, 82, 28};
constexpr int kSaveLoadTabRowMedianMarginPx = 8;

constexpr RectI kLoadBgSubtitleEraseRect{137, 23, 49, 16};
constexpr RectI kSaveBgSubtitleEraseRect{132, 22, 53, 17};

struct SystemToastSpec {
    std::uint32_t frame_id;
    std::string_view english_text;
};

constexpr std::array<SystemToastSpec, 5> kSystemToastSpecs{{
    {120, "Quick Save Complete"},
    {111, "Auto Mode: ON"},
    {110, "Auto Mode: OFF"},
    {101, "Tease Mode: ON"},
    {100, "Tease Mode: OFF"},
}};

struct BgmToastSpec {
    std::uint32_t frame_id;
    std::string_view unified_title;
    std::string_view concise_title;
};

constexpr std::array<BgmToastSpec, 16> kBgmToastSpecs{{
    {317, "And Then, Someday", "And Someday"},
    {316, "To You Who Waits for Dawn's Glow", "For You, Awaiting Dawn"},
    {315, "Shape of Our Feelings", "Shape of Feelings"},
    {314, "Two Waiting for Dawn", "Two Waiting for Dawn"},
    {313, "Feelings of Love", "Feelings of Love"},
    {312, "Led by an Ox to Zenkoji", "Led to Zenkoji"},
    {311, "Warm White Breaths", "Warm White Breaths"},
    {310, "Two Shadows in the Sunset Glow", "Two Sunset Shadows"},
    {309, "Let's Take It Easy", "Let's Take It Easy"},
    {308, "Treading Through Snow", "Snowsteps"},
    {307, "A Spring in My Step", "Bouncy Steps"},
    {306, "Through the Four Seasons", "Four Seasons"},
    {305, "School Life", "School Life"},
    {303, "Dappled Sunlight Smile", "Sunlit Smile"},
    {302, "Crystalline Girl", "Radiant Girl"},
    {301, "Sunset Sunny Spot", "Sunset Warmth"},
}};

struct SaveLoadTabSpec {
    std::uint32_t frame_id;
    std::string_view label;
    bool is_hover;
};

constexpr std::array<SaveLoadTabSpec, 4> kSaveLoadTabSpecs{{
    {kSaveTabHoverFrameId, "SAVE", true},
    {kLoadTabHoverFrameId, "LOAD", true},
    {kSaveTabNormalFrameId, "SAVE", false},
    {kLoadTabNormalFrameId, "LOAD", false},
}};

TextStyle MakeInfoToastStyle() {
    TextStyle style;
    style.font_family = kDefaultUiFontFamily;
    style.weight = kFontWeightBold;
    style.size_px = kToastFontSizePx;
    style.fill = kToastFillTopColor;
    style.gradient = true;
    style.fill_bottom = kToastFillBottomColor;
    style.stroke_px = kToastStrokeWidthPx;
    style.stroke = kToastStrokeColor;
    style.shadow = true;
    style.shadow_dx = kToastShadowOffsetX;
    style.shadow_dy = kToastShadowOffsetY;
    style.shadow_blur_px = kToastShadowBlurPx;
    style.shadow_color = kToastShadowColor;
    style.halign = HorizontalAlign::kLeft;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;
    return style;
}

TextStyle MakeInfoDateSlashStyle() {
    TextStyle style = MakeInfoToastStyle();
    style.size_px = kDateSlashFontSizePx;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    return style;
}

TextStyle MakeInfoBgmStyle() {
    TextStyle style = MakeInfoToastStyle();
    style.size_px = kBgmToastFontSizePx;
    style.stroke_px = kBgmToastStrokeWidthPx;
    style.halign = HorizontalAlign::kLeft;
    style.valign = VerticalAlign::kCenter;
    return style;
}

TextStyle MakeSaveLoadTabStyle(bool is_hover) {
    TextStyle style;
    style.font_family = kDefaultUiFontFamily;
    style.weight = kFontWeightBold;
    style.size_px = kSaveLoadTabFontSizePx;
    style.tracking_px = kSaveLoadTabTrackingPx;
    style.fill = is_hover ? kSaveLoadTabHoverTextColor : kSaveLoadTabNormalTextColor;
    style.gradient = false;
    style.stroke_px = 0.0F;
    style.shadow = false;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;
    return style;
}

hg3::Hg3Frame& RequireFrame(hg3::Hg3Container& container, std::uint32_t frame_id,
                            const char* container_name) {
    hg3::Hg3Frame* frame = container.FindFrame(frame_id);
    if (frame == nullptr) {
        throw std::runtime_error(std::string("missing frame id ") + std::to_string(frame_id) +
                                 " in " + container_name);
    }
    return *frame;
}

void EraseSubtitleInpaint(hg3::Hg3Frame& frame, const RectI& erase_rect) {
    SpriteCanvas canvas(std::move(frame.pixels));
    canvas.EraseWithBorderInpaint(erase_rect);
    frame.pixels = canvas.image();
}

MenuPatchEntrySpec RenderSysLogContainer(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry;
    hg3::Hg3Container container = source.LoadHg3(kSysLogFilename, &src_entry);
    if (container.frames.size() != kSysLogExpectedFrameCount) {
        throw std::runtime_error("unexpected frame count in sys_log.hg3");
    }

    hg3::Hg3Frame& bg_frame = RequireFrame(container, kSysLogBackgroundFrameId, kSysLogFilename);
    SpriteCanvas canvas(std::move(bg_frame.pixels));
    canvas.BlitCleanRegion(canvas.image(), kSysLogCleanDonorRect, kSysLogSubtitleEraseRect.x,
                           kSysLogSubtitleEraseRect.y);
    bg_frame.pixels = canvas.image();

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

void RenderBgmToastFrame(hg3::Hg3Frame& frame, const BgmToastSpec& spec, const TextStyle& style) {
    SpriteCanvas canvas(std::move(frame.pixels));
    const int width = canvas.width();
    const int height = canvas.height();
    if (width <= kSysInfoBgmTextWidthTrim || height <= kSysInfoBgmTextTopY) {
        throw std::runtime_error("invalid BGM toast frame dimensions in sys_info.hg3");
    }

    const RectI clear_rect{kSysInfoBgmNoteProtectedWidth, 0, width - kSysInfoBgmNoteProtectedWidth,
                           height};
    canvas.Fill(clear_rect, kTransparentColor);

    const RectI text_rect{kSysInfoBgmTextLeftX, kSysInfoBgmTextTopY,
                          width - kSysInfoBgmTextWidthTrim, kSysInfoBgmTextHeight};
    const std::string unified_text(spec.unified_title);
    const TextLayoutResult measured = canvas.MeasureText(unified_text, text_rect, style);
    const bool use_unified = measured.fits && (measured.scale >= kUnifiedBgmMinScale) &&
                             (measured.squeeze >= kUnifiedBgmMinSqueeze);
    const std::string chosen_text =
        use_unified ? unified_text : std::string(spec.concise_title);
    canvas.DrawText(chosen_text, text_rect, style);
    frame.pixels = canvas.image();
}

void RenderSystemToastFrame(hg3::Hg3Frame& frame, std::string_view text, const TextStyle& style) {
    SpriteCanvas canvas(std::move(frame.pixels));
    const int width = canvas.width();
    const int height = canvas.height();
    canvas.Fill(RectI{0, 0, width, height}, kTransparentColor);

    const RectI text_rect{kSysInfoToastPadX, kSysInfoToastPadY, width - kSysInfoToastWidthTrim,
                          height - kSysInfoToastHeightTrim};
    canvas.DrawText(std::string(text), text_rect, style);
    frame.pixels = canvas.image();
}

void RenderDateSlashFrame(hg3::Hg3Frame& frame, const TextStyle& style) {
    SpriteCanvas canvas(std::move(frame.pixels));
    canvas.Fill(RectI{0, 0, canvas.width(), canvas.height()}, kTransparentColor);
    canvas.DrawText("/", kSysInfoDateSlashTextRect, style);
    frame.pixels = canvas.image();
}

MenuPatchEntrySpec RenderSysInfoContainer(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry;
    hg3::Hg3Container container = source.LoadHg3(kSysInfoFilename, &src_entry);
    if (container.frames.size() != kSysInfoExpectedFrameCount) {
        throw std::runtime_error("unexpected frame count in sys_info.hg3");
    }

    const TextStyle bgm_style = MakeInfoBgmStyle();
    for (const BgmToastSpec& spec : kBgmToastSpecs) {
        hg3::Hg3Frame& frame = RequireFrame(container, spec.frame_id, kSysInfoFilename);
        RenderBgmToastFrame(frame, spec, bgm_style);
    }

    const TextStyle date_style = MakeInfoDateSlashStyle();
    RenderDateSlashFrame(RequireFrame(container, kSysInfoDateFrameId, kSysInfoFilename),
                         date_style);

    const TextStyle toast_style = MakeInfoToastStyle();
    for (const SystemToastSpec& spec : kSystemToastSpecs) {
        hg3::Hg3Frame& frame = RequireFrame(container, spec.frame_id, kSysInfoFilename);
        RenderSystemToastFrame(frame, spec.english_text, toast_style);
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

void RenderSaveLoadModeTab(hg3::Hg3Frame& frame, const SaveLoadTabSpec& spec) {
    SpriteCanvas canvas(std::move(frame.pixels));
    canvas.EraseWithRowMedian(kSaveLoadTabEraseRect, kSaveLoadTabRowMedianMarginPx);
    canvas.DrawText(std::string(spec.label), kSaveLoadTabTextRect,
                    MakeSaveLoadTabStyle(spec.is_hover));
    frame.pixels = canvas.image();
}

MenuPatchEntrySpec RenderSaveLoadBaseContainer(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry;
    hg3::Hg3Container container = source.LoadHg3(kSaveLoadBaseFilename, &src_entry);
    if (container.frames.size() != kSaveLoadBaseExpectedFrameCount) {
        throw std::runtime_error("unexpected frame count in sl_base.hg3");
    }

    for (const hg3::Hg3Frame& frame : container.frames) {
        if (frame.imgmode != kSaveLoadExpectedImgMode) {
            throw std::runtime_error("missing expected imgmode tag on sl_base.hg3 frame");
        }
    }

    for (const SaveLoadTabSpec& spec : kSaveLoadTabSpecs) {
        hg3::Hg3Frame& frame = RequireFrame(container, spec.frame_id, kSaveLoadBaseFilename);
        RenderSaveLoadModeTab(frame, spec);
    }

    EraseSubtitleInpaint(RequireFrame(container, kLoadBgFrameId, kSaveLoadBaseFilename),
                         kLoadBgSubtitleEraseRect);
    EraseSubtitleInpaint(RequireFrame(container, kSaveBgFrameId, kSaveLoadBaseFilename),
                         kSaveBgSubtitleEraseRect);

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

class HudSaveLoadDomainRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "hud_saveload"; }

    std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) override {
        (void)options;
        std::vector<MenuPatchEntrySpec> entries;
        entries.reserve(3);
        entries.push_back(RenderSysLogContainer(source));
        entries.push_back(RenderSysInfoContainer(source));
        entries.push_back(RenderSaveLoadBaseContainer(source));
        return entries;
    }
};

}  // namespace

std::unique_ptr<IMenuDomainRenderer> CreateHudSaveLoadDomainRenderer() {
    return std::make_unique<HudSaveLoadDomainRenderer>();
}

}  // namespace ama::menu
