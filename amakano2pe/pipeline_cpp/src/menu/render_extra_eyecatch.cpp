// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_extra_eyecatch.h"

#include "menu/core/sprite_canvas.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ama::menu {

namespace {

constexpr const wchar_t* kPrimaryUiFont = L"Segoe UI";

constexpr int kFontWeightSemibold = 600;
constexpr int kFontWeightBold = 700;

constexpr float kPillButtonFontSizePx = 16.0F;
constexpr float kChitosePillFontSizePx = 15.0F;
constexpr float kMusicTitleFontSizePx = 15.0F;
constexpr float kConsoleLabelFontSizePx = 15.0F;

constexpr float kDefaultMinSqueeze = 0.82F;
constexpr float kDefaultMinScale = 0.68F;
constexpr float kMusicMinSqueeze = 0.78F;
constexpr float kMusicMinScale = 0.65F;

constexpr BgraColor kCreamTextFill{194, 224, 248, 255};
constexpr BgraColor kDarkBrownTextFill{42, 46, 68, 255};
constexpr BgraColor kMusicTitleTextFill{40, 54, 73, 255};

constexpr BgraColor kConsoleTextFill{235, 240, 248, 255};
constexpr BgraColor kConsoleTextStroke{38, 34, 42, 220};
constexpr BgraColor kConsoleTextShadow{20, 18, 24, 180};
constexpr float kConsoleStrokePx = 0.85F;
constexpr float kConsoleShadowOffsetPx = 1.0F;
constexpr float kConsoleShadowBlurPx = 1.0F;

// Shared top-right navigation tabs (122x45, 8 frames per Extra screen).
constexpr std::size_t kTopTabFrameCount = 8;
constexpr std::size_t kCgModeTopTabFirstIndex = 24;
constexpr std::size_t kReplayModeTopTabFirstIndex = 8;
constexpr std::size_t kMusicModeTopTabFirstIndex = 28;
constexpr std::size_t kCharModeTopTabFirstIndex = 0;

constexpr RectI kTopTabInteriorEraseRect{8, 6, 106, 32};
constexpr RectI kTopTabEnglishDonorRect{8, 8, 106, 13};
constexpr int kTopTabEnglishCenteredDstX = 8;
constexpr int kTopTabEnglishCenteredDstY = 15;
constexpr int kTopTabRowMedianMarginPx = 3;

// Full-screen Extra background header bar subtitle erase rectangles (1280x720).
constexpr std::size_t kCgModeBgFrameIndex = 49;
constexpr std::size_t kMusicModeBgFrameIndex = 52;
constexpr std::size_t kReplayModeBgFrameIndex = 33;

constexpr RectI kHeaderExtraJpEraseRect{140, 14, 82, 25};
constexpr RectI kHeaderCgModeJpEraseRect{364, 14, 82, 25};
constexpr RectI kHeaderMusicModeJpEraseRect{404, 14, 116, 25};
constexpr RectI kHeaderReplayModeJpEraseRect{400, 14, 130, 25};

// Pill button geometry for ex_charmode.hg3 and ex_charmode_select.hg3.
constexpr int kPillRowMedianMarginPx = 5;
constexpr RectI kNarrowPillEraseRect{30, 8, 131, 23};
constexpr RectI kNarrowPillTextRect{20, 6, 151, 27};
constexpr RectI kWidePillEraseRect{31, 8, 139, 23};
constexpr RectI kWidePillTextRect{20, 6, 161, 27};

// Left attribute console on ex_charmode.hg3 (#154, 284x464).
constexpr std::size_t kCharConsoleFrameIndex = 154;
constexpr int kCharConsoleRowCount = 8;
constexpr int kCharConsoleRowPitchY = 58;
constexpr int kCharConsoleCleanStripRelY = 5;
constexpr int kCharConsoleCleanStripHeight = 1;
constexpr int kCharConsoleEraseDstX = 26;
constexpr int kCharConsoleEraseRelY = 6;
constexpr int kCharConsoleEraseWidth = 98;
constexpr int kCharConsoleEraseHeight = 37;
constexpr int kCharConsoleTextX = 12;
constexpr int kCharConsoleTextRelY = 8;
constexpr int kCharConsoleTextWidth = 108;
constexpr int kCharConsoleTextHeight = 33;

constexpr std::array<const char*, kCharConsoleRowCount> kCharConsoleRowLabels{{
    "Pose",
    "Outfit",
    "Expression",
    "Blush",
    "Accessory",
    "Pubic Hair",
    "Background",
    "Time of Day",
}};

// Music track plate geometry (ex_music_thumbnail.hg3, 52 frames, 280x54).
constexpr std::size_t kMusicTrackCount = 26;
constexpr std::size_t kStatesPerMusicTrack = 2;
constexpr std::size_t kMusicTotalFrameCount = kMusicTrackCount * kStatesPerMusicTrack;
constexpr RectI kMusicCardEraseRect{10, 8, 261, 38};
constexpr RectI kMusicCardTextRect{14, 8, 254, 38};
constexpr int kMusicCardRowMedianMarginPx = 3;

constexpr std::array<const char*, kMusicTrackCount> kMusicTrackTitles{{
    "01. \"Always By Your Side...\" short ver.",
    "02. \"Always By Your Side...\" inst ver.",
    "03. \"Always By Your Side...\" off vocal",
    "04. Sunset Sunny Spot",
    "05. Crystalline Girl",
    "06. Dappled Sunlight Smile",
    "07. Luna nueva",
    "08. School Life",
    "09. A Spring in My Step",
    "10. Through the Four Seasons",
    "11. Treading Through Snow",
    "12. Let's Take It Easy",
    "13. Two Shadows in the Sunset Glow",
    "14. Warm White Breaths",
    "15. Led by an Ox to Zenkoji",
    "16. Feelings of Love",
    "17. Two Waiting for Dawn",
    "18. Shape of Our Feelings",
    "19. To You Who Waits for Dawn's Glow",
    "20. And Then, Someday",
    "21. \"coincidence\" short ver.",
    "22. \"coincidence\" inst ver.",
    "23. \"5cm ~Duca ver.~\" short ver.",
    "24. \"5cm ~Duca ver.~\" inst ver.",
    "25. \"Treasure\" short ver.",
    "26. \"Treasure\" inst ver.",
}};

struct PillButtonSpec {
    std::size_t frame_index = 0;
    const char* label = "";
    bool wide_pill = false;
    bool cream_background = false;
    float font_size_px = kPillButtonFontSizePx;
};

constexpr std::size_t kCharModePillCount = 12;
constexpr std::size_t kAddNewPillHoverSelectedIndex = 111;
constexpr std::size_t kAddNewPillHoverIndex = 112;
constexpr std::size_t kAddNewPillNormalIndex = 113;
constexpr std::size_t kHideUiPillHoverSelectedIndex = 145;
constexpr std::size_t kHideUiPillHoverIndex = 146;
constexpr std::size_t kHideUiPillNormalIndex = 147;
constexpr std::size_t kCustomBgPillHoverSelectedIndex = 148;
constexpr std::size_t kCustomBgPillHoverIndex = 149;
constexpr std::size_t kCustomBgPillNormalIndex = 150;
constexpr std::size_t kResetPosPillHoverSelectedIndex = 151;
constexpr std::size_t kResetPosPillHoverIndex = 152;
constexpr std::size_t kResetPosPillNormalIndex = 153;

constexpr std::array<PillButtonSpec, kCharModePillCount> kCharModePillSpecs{{
    {kAddNewPillHoverSelectedIndex, "Add New", false, false, kPillButtonFontSizePx},
    {kAddNewPillHoverIndex, "Add New", false, false, kPillButtonFontSizePx},
    {kAddNewPillNormalIndex, "Add New", false, true, kPillButtonFontSizePx},
    {kHideUiPillHoverSelectedIndex, "Hide UI", true, false, kPillButtonFontSizePx},
    {kHideUiPillHoverIndex, "Hide UI", true, false, kPillButtonFontSizePx},
    {kHideUiPillNormalIndex, "Hide UI", true, true, kPillButtonFontSizePx},
    {kCustomBgPillHoverSelectedIndex, "Custom BG", true, false, kPillButtonFontSizePx},
    {kCustomBgPillHoverIndex, "Custom BG", true, false, kPillButtonFontSizePx},
    {kCustomBgPillNormalIndex, "Custom BG", true, true, kPillButtonFontSizePx},
    {kResetPosPillHoverSelectedIndex, "Reset Pos", true, false, kPillButtonFontSizePx},
    {kResetPosPillHoverIndex, "Reset Pos", true, false, kPillButtonFontSizePx},
    {kResetPosPillNormalIndex, "Reset Pos", true, true, kPillButtonFontSizePx},
}};

constexpr std::size_t kCharModeSelectPillCount = 6;
constexpr std::size_t kReiSelectHoverIndex = 3;
constexpr std::size_t kReiSelectNormalIndex = 4;
constexpr std::size_t kChitoseSelectHoverIndex = 5;
constexpr std::size_t kChitoseSelectNormalIndex = 6;
constexpr std::size_t kYuuhiSelectHoverIndex = 7;
constexpr std::size_t kYuuhiSelectNormalIndex = 8;

constexpr std::array<PillButtonSpec, kCharModeSelectPillCount> kCharModeSelectPillSpecs{{
    {kReiSelectHoverIndex, "Himiyama Rei", false, false, kPillButtonFontSizePx},
    {kReiSelectNormalIndex, "Himiyama Rei", false, true, kPillButtonFontSizePx},
    {kChitoseSelectHoverIndex, "Tsutamachi Chitose", false, false, kChitosePillFontSizePx},
    {kChitoseSelectNormalIndex, "Tsutamachi Chitose", false, true, kChitosePillFontSizePx},
    {kYuuhiSelectHoverIndex, "Kurohime Yuuhi", false, false, kPillButtonFontSizePx},
    {kYuuhiSelectNormalIndex, "Kurohime Yuuhi", false, true, kPillButtonFontSizePx},
}};

ama::hg3::Hg3Frame& RequireFrameAt(ama::hg3::Hg3Container& container, std::size_t index,
                                   const char* archive_name) {
    if (index >= container.frames.size()) {
        throw std::runtime_error(std::string("Frame index out of range in ") + archive_name);
    }
    return container.frames[index];
}

void EraseTopNavigationSubtitles(ama::hg3::Hg3Container& container, std::size_t first_index,
                                 const char* archive_name) {
    for (std::size_t offset = 0; offset < kTopTabFrameCount; ++offset) {
        ama::hg3::Hg3Frame& frame = RequireFrameAt(container, first_index + offset, archive_name);
        const BgraImage donor = frame.pixels;
        SpriteCanvas canvas(std::move(frame.pixels));
        canvas.EraseWithRowMedian(kTopTabInteriorEraseRect, kTopTabRowMedianMarginPx);
        canvas.BlitCleanRegion(donor, kTopTabEnglishDonorRect, kTopTabEnglishCenteredDstX,
                               kTopTabEnglishCenteredDstY);
        frame.pixels = canvas.image();
    }
}

void EraseBackgroundHeaderSubtitles(ama::hg3::Hg3Container& container, std::size_t bg_index,
                                    const RectI& mode_jp_rect, const char* archive_name) {
    ama::hg3::Hg3Frame& frame = RequireFrameAt(container, bg_index, archive_name);
    SpriteCanvas canvas(std::move(frame.pixels));
    canvas.EraseWithBorderInpaint(kHeaderExtraJpEraseRect);
    canvas.EraseWithBorderInpaint(mode_jp_rect);
    frame.pixels = canvas.image();
}

void RenderPillButton(ama::hg3::Hg3Container& container, const PillButtonSpec& spec,
                      const char* archive_name) {
    ama::hg3::Hg3Frame& frame = RequireFrameAt(container, spec.frame_index, archive_name);
    SpriteCanvas canvas(std::move(frame.pixels));

    const RectI& erase_rect = spec.wide_pill ? kWidePillEraseRect : kNarrowPillEraseRect;
    const RectI& text_rect = spec.wide_pill ? kWidePillTextRect : kNarrowPillTextRect;
    canvas.EraseWithRowMedian(erase_rect, kPillRowMedianMarginPx);

    TextStyle style;
    style.font_family = kPrimaryUiFont;
    style.weight = kFontWeightSemibold;
    style.size_px = spec.font_size_px;
    style.fill = spec.cream_background ? kDarkBrownTextFill : kCreamTextFill;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;

    canvas.DrawText(spec.label, text_rect, style);
    frame.pixels = canvas.image();
}

void RenderCharModeConsole(ama::hg3::Hg3Container& container, const char* archive_name) {
    ama::hg3::Hg3Frame& frame = RequireFrameAt(container, kCharConsoleFrameIndex, archive_name);
    const BgraImage donor = frame.pixels;
    SpriteCanvas canvas(std::move(frame.pixels));

    TextStyle style;
    style.font_family = kPrimaryUiFont;
    style.weight = kFontWeightBold;
    style.size_px = kConsoleLabelFontSizePx;
    style.fill = kConsoleTextFill;
    style.stroke_px = kConsoleStrokePx;
    style.stroke = kConsoleTextStroke;
    style.shadow = true;
    style.shadow_dx = kConsoleShadowOffsetPx;
    style.shadow_dy = kConsoleShadowOffsetPx;
    style.shadow_blur_px = kConsoleShadowBlurPx;
    style.shadow_color = kConsoleTextShadow;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;

    for (int row = 0; row < kCharConsoleRowCount; ++row) {
        const int row_y = row * kCharConsoleRowPitchY;
        const RectI donor_strip{kCharConsoleEraseDstX, row_y + kCharConsoleCleanStripRelY,
                                kCharConsoleEraseWidth, kCharConsoleCleanStripHeight};
        for (int dy = 0; dy < kCharConsoleEraseHeight; ++dy) {
            canvas.BlitCleanRegion(donor, donor_strip, kCharConsoleEraseDstX,
                                   row_y + kCharConsoleEraseRelY + dy);
        }

        const RectI text_rect{kCharConsoleTextX, row_y + kCharConsoleTextRelY,
                              kCharConsoleTextWidth, kCharConsoleTextHeight};
        canvas.DrawText(kCharConsoleRowLabels[static_cast<std::size_t>(row)], text_rect, style);
    }

    frame.pixels = canvas.image();
}

MenuPatchEntrySpec RenderCgModeArchive(const GameAssetSource& source) {
    constexpr const char* kArchiveName = "ex_cgmode.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container hg3 = source.LoadHg3(kArchiveName, &src_entry);
    EraseTopNavigationSubtitles(hg3, kCgModeTopTabFirstIndex, kArchiveName);
    EraseBackgroundHeaderSubtitles(hg3, kCgModeBgFrameIndex, kHeaderCgModeJpEraseRect,
                                   kArchiveName);
    return GameAssetSource::BuildHg3PatchEntry(src_entry, hg3);
}

MenuPatchEntrySpec RenderReplayModeArchive(const GameAssetSource& source) {
    constexpr const char* kArchiveName = "ex_replaymode.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container hg3 = source.LoadHg3(kArchiveName, &src_entry);
    EraseTopNavigationSubtitles(hg3, kReplayModeTopTabFirstIndex, kArchiveName);
    EraseBackgroundHeaderSubtitles(hg3, kReplayModeBgFrameIndex, kHeaderReplayModeJpEraseRect,
                                   kArchiveName);
    return GameAssetSource::BuildHg3PatchEntry(src_entry, hg3);
}

MenuPatchEntrySpec RenderMusicModeArchive(const GameAssetSource& source) {
    constexpr const char* kArchiveName = "ex_music.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container hg3 = source.LoadHg3(kArchiveName, &src_entry);
    EraseTopNavigationSubtitles(hg3, kMusicModeTopTabFirstIndex, kArchiveName);
    EraseBackgroundHeaderSubtitles(hg3, kMusicModeBgFrameIndex, kHeaderMusicModeJpEraseRect,
                                   kArchiveName);
    return GameAssetSource::BuildHg3PatchEntry(src_entry, hg3);
}

MenuPatchEntrySpec RenderMusicThumbnailsArchive(const GameAssetSource& source) {
    constexpr const char* kArchiveName = "ex_music_thumbnail.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container hg3 = source.LoadHg3(kArchiveName, &src_entry);
    if (hg3.frames.size() < kMusicTotalFrameCount) {
        throw std::runtime_error("Unexpected frame count in ex_music_thumbnail.hg3");
    }

    TextStyle style;
    style.font_family = kPrimaryUiFont;
    style.weight = kFontWeightBold;
    style.size_px = kMusicTitleFontSizePx;
    style.fill = kMusicTitleTextFill;
    style.halign = HorizontalAlign::kLeft;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kMusicMinSqueeze;
    style.min_scale = kMusicMinScale;

    for (std::size_t track_idx = 0; track_idx < kMusicTrackCount; ++track_idx) {
        const std::size_t hover_frame_idx = (kMusicTrackCount - 1) - track_idx;
        const std::size_t normal_frame_idx = (kMusicTotalFrameCount - 1) - track_idx;
        const char* title = kMusicTrackTitles[track_idx];

        for (const std::size_t frame_idx : {hover_frame_idx, normal_frame_idx}) {
            ama::hg3::Hg3Frame& frame = RequireFrameAt(hg3, frame_idx, kArchiveName);
            SpriteCanvas canvas(std::move(frame.pixels));
            canvas.EraseWithRowMedian(kMusicCardEraseRect, kMusicCardRowMedianMarginPx);
            canvas.DrawText(title, kMusicCardTextRect, style);
            frame.pixels = canvas.image();
        }
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, hg3);
}

MenuPatchEntrySpec RenderCharModeArchive(const GameAssetSource& source) {
    constexpr const char* kArchiveName = "ex_charmode.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container hg3 = source.LoadHg3(kArchiveName, &src_entry);
    EraseTopNavigationSubtitles(hg3, kCharModeTopTabFirstIndex, kArchiveName);
    for (const PillButtonSpec& spec : kCharModePillSpecs) {
        RenderPillButton(hg3, spec, kArchiveName);
    }
    RenderCharModeConsole(hg3, kArchiveName);
    return GameAssetSource::BuildHg3PatchEntry(src_entry, hg3);
}

MenuPatchEntrySpec RenderCharModeSelectArchive(const GameAssetSource& source) {
    constexpr const char* kArchiveName = "ex_charmode_select.hg3";
    ExtractedAssetEntry src_entry;
    ama::hg3::Hg3Container hg3 = source.LoadHg3(kArchiveName, &src_entry);
    for (const PillButtonSpec& spec : kCharModeSelectPillSpecs) {
        RenderPillButton(hg3, spec, kArchiveName);
    }
    return GameAssetSource::BuildHg3PatchEntry(src_entry, hg3);
}

constexpr std::size_t kPatchedArchiveCount = 6;

class ExtraEyecatchDomainRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "extra_eyecatch"; }

    std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) override {
        (void)options;
        std::vector<MenuPatchEntrySpec> records;
        records.reserve(kPatchedArchiveCount);
        records.push_back(RenderCgModeArchive(source));
        records.push_back(RenderReplayModeArchive(source));
        records.push_back(RenderMusicModeArchive(source));
        records.push_back(RenderMusicThumbnailsArchive(source));
        records.push_back(RenderCharModeArchive(source));
        records.push_back(RenderCharModeSelectArchive(source));
        return records;
    }
};

}  // namespace

std::unique_ptr<IMenuDomainRenderer> CreateExtraEyecatchDomainRenderer() {
    return std::make_unique<ExtraEyecatchDomainRenderer>();
}

}  // namespace ama::menu
