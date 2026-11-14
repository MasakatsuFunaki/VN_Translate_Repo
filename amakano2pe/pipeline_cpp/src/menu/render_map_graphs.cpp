// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_map_graphs.h"

#include "hg3/hg3_codec.h"
#include "imaging/bgra_image.h"
#include "menu/core/sprite_canvas.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ama::menu {

namespace {

constexpr int kFontWeightRegular = 400;
constexpr int kFontWeightSemibold = 600;
constexpr int kFontWeightBold = 700;

constexpr std::uint8_t kOpaqueAlpha = 0xFF;
constexpr std::uint8_t kTransparentAlpha = 0x00;

// Color palette constants (BGRA byte order).
constexpr BgraColor kColorTransparent{0x00, 0x00, 0x00, kTransparentAlpha};
constexpr BgraColor kColorWhite{0xFF, 0xFF, 0xFF, kOpaqueAlpha};
constexpr BgraColor kColorBlack{0x00, 0x00, 0x00, kOpaqueAlpha};

// Map selection and school map dark maroon ink (#4A0E2E) and city banner stroke (#3A0820).
constexpr BgraColor kColorMapMaroonText{0x2E, 0x0E, 0x4A, kOpaqueAlpha};
constexpr BgraColor kColorCityHeaderStroke{0x20, 0x08, 0x3A, kOpaqueAlpha};

// Confession choice prompt colors (#1E2B38 / #D6E9F8 for cancel, #2B1810 / #F7DEC8 for confirm).
constexpr BgraColor kColorConfessCancelFill{0x38, 0x2B, 0x1E, kOpaqueAlpha};
constexpr BgraColor kColorConfessCancelStroke{0xF8, 0xE9, 0xD6, kOpaqueAlpha};
constexpr BgraColor kColorConfessConfirmFill{0x10, 0x18, 0x2B, kOpaqueAlpha};
constexpr BgraColor kColorConfessConfirmStroke{0xC8, 0xDE, 0xF7, kOpaqueAlpha};

// Relationship graph card palette (Lit and Dimmed variants).
constexpr BgraColor kColorGraphAffectionLit{0xC4, 0xDE, 0xF2, kOpaqueAlpha};
constexpr BgraColor kColorGraphAffectionDim{0xE0, 0xEC, 0xF7, kOpaqueAlpha};
constexpr BgraColor kColorGraphBorderLit{0x31, 0x35, 0x4A, kOpaqueAlpha};
constexpr BgraColor kColorGraphBorderDim{0x62, 0x65, 0x76, kOpaqueAlpha};

// Hexagonal side-menu button geometry and erase thresholds in sys_mapselect.hg3.
constexpr std::size_t kHexButtonFirstIndex = 6;
constexpr std::size_t kHexButtonLastIndex = 20;
constexpr std::size_t kStatesPerButton = 3;
constexpr int kHexEraseInsetX = 34;
constexpr int kHexEraseInsetY = 10;
constexpr int kHexTextInsetX = 32;
constexpr int kHexTextInsetY = 8;
constexpr int kHexDoubledInsetFactor = 2;
constexpr std::uint8_t kHexGlyphMaxGreen = 200;
constexpr std::uint8_t kHexGlyphMaxBlue = 210;
constexpr int kHexMaskDilateRadius = 2;
constexpr float kHexConfessFontSizePx = 19.0F;
constexpr float kHexStandardFontSizePx = 20.0F;
constexpr float kHexMinSqueeze = 0.85F;
constexpr float kHexMinScale = 0.70F;

// Map location plate geometry (141x36 rounded plates in sys_mapselect.hg3 and sys_school_map.hg3).
constexpr int kLocationPlateWidth = 141;
constexpr int kLocationPlateHeight = 36;
constexpr int kLocationPlateSampleX = 8;
constexpr int kLocationPlateSampleY = 8;
constexpr RectI kLocationPlateEraseRect{6, 5, 129, 26};
constexpr RectI kLocationPlateSingleLineRect{6, 6, 129, 24};
constexpr RectI kLocationPlateTwoLineRect{6, 5, 129, 26};
constexpr float kLocationSingleLineFontSizePx = 14.0F;
constexpr float kLocationSingleLineMinSqueeze = 0.82F;
constexpr float kLocationSingleLineMinScale = 0.65F;
constexpr float kLocationTwoLineFontSizePx = 10.5F;
constexpr float kLocationTwoLineSpacing = 0.82F;
constexpr float kLocationTwoLineMinSqueeze = 0.80F;
constexpr float kLocationTwoLineMinScale = 0.60F;

// Top-right city header banner (#037, 509x72) in sys_mapselect.hg3.
constexpr std::size_t kCityHeaderFrameIndex = 37;
constexpr RectI kCityHeaderGlyphScanRect{150, 10, 190, 52};
constexpr RectI kCityHeaderMaskClampRect{148, 8, 194, 56};
constexpr RectI kCityHeaderTextRect{130, 10, 248, 52};
constexpr std::uint8_t kCityHeaderGlyphMinGreen = 58;
constexpr std::uint8_t kCityHeaderGlyphMinRed = 115;
constexpr std::uint8_t kCityHeaderGlyphMinBlue = 90;
constexpr int kCityHeaderMaskDilateRadius = 2;
constexpr float kCityHeaderFontSizePx = 30.0F;
constexpr float kCityHeaderStrokePx = 1.0F;
constexpr float kCityHeaderMinSqueeze = 0.88F;
constexpr float kCityHeaderMinScale = 0.75F;

// Seal mascot speech bubble text frames (#093-#095, 139x76) in sys_mapselect.hg3.
constexpr std::size_t kMascotSpeechFirstIndex = 93;
constexpr std::size_t kMascotSpeechSecondIndex = 94;
constexpr std::size_t kMascotSpeechThirdIndex = 95;
constexpr RectI kMascotSpeechTextRect{4, 10, 131, 46};
constexpr float kMascotSpeechFontSizePx = 22.0F;
constexpr float kMascotSpeechMinSqueeze = 0.82F;
constexpr float kMascotSpeechMinScale = 0.65F;

// Confession confirmation banners (#000-#003) in mapkokuhaku.hg3.
constexpr std::size_t kConfessCancelActiveIndex = 0;
constexpr std::size_t kConfessConfirmActiveIndex = 1;
constexpr std::size_t kConfessCancelIdleIndex = 2;
constexpr std::size_t kConfessConfirmIdleIndex = 3;
constexpr int kConfessSampleX = 45;
constexpr int kConfessSampleY = 7;
constexpr RectI kConfessCancelEraseRect{45, 11, 270, 46};
constexpr RectI kConfessCancelTextRect{30, 10, 298, 48};
constexpr RectI kConfessConfirmEraseRect{45, 11, 240, 46};
constexpr RectI kConfessConfirmTextRect{20, 10, 285, 48};
constexpr float kConfessBannerFontSizePx = 24.0F;
constexpr float kConfessBannerStrokePx = 2.0F;
constexpr float kConfessBannerMinSqueeze = 0.85F;
constexpr float kConfessBannerMinScale = 0.70F;

// Relationship graph card constants (sys_graph_friend.hg3 and sys_graph_lover.hg3).
constexpr std::size_t kGraphCardFirstIndex = 2;
constexpr std::size_t kGraphFriendLastCardIndex = 85;
constexpr std::size_t kGraphLoverLastCardIndex = 49;
constexpr int kGraphPillHeight = 21;
constexpr int kGraphPillMinPixelCount = 250;
constexpr int kGraphPillMaxCapWidth = 66;
constexpr std::uint8_t kGraphDimmedRedThreshold = 130;
constexpr int kGraphCapMinRedOverGreen = 15;
constexpr std::uint8_t kGraphCapMaxRed = 180;
constexpr float kGraphNameBadgeRadiusPx = 8.0F;
constexpr float kGraphNameBadgeBorderPx = 2.0F;
constexpr float kGraphAffectionFontSizePx = 11.0F;
constexpr float kGraphAffectionMinSqueeze = 0.82F;
constexpr float kGraphAffectionMinScale = 0.65F;
constexpr float kGraphNameStandardFontSizePx = 16.0F;
constexpr float kGraphNameLongFontSizePx = 15.0F;
constexpr float kGraphNameStrokePx = 1.5F;
constexpr float kGraphNameMinSqueeze = 0.85F;
constexpr float kGraphNameMinScale = 0.72F;

// Card dimensions, pill vertical positions, and name badge boxes in sys_graph_friend.hg3.
constexpr int kFriendReiCardWidth = 337;
constexpr int kFriendReiCardHeight = 639;
constexpr int kFriendReiPillY = 564;
constexpr RectI kFriendReiBadgeRect{78, 448, 156, 39};

constexpr int kFriendYuuhiACardWidth = 391;
constexpr int kFriendYuuhiACardHeight = 634;
constexpr int kFriendYuuhiAPillY = 559;
constexpr RectI kFriendYuuhiABadgeRect{139, 443, 152, 39};

constexpr int kFriendYuuhiBCardWidth = 425;
constexpr int kFriendYuuhiBCardHeight = 613;
constexpr int kFriendYuuhiBPillY = 538;
constexpr RectI kFriendYuuhiBBadgeRect{145, 422, 152, 39};

constexpr int kFriendChitoseCardWidth = 369;
constexpr int kFriendChitoseCardHeight = 593;
constexpr int kFriendChitosePillY = 518;
constexpr RectI kFriendChitoseBadgeRect{92, 402, 176, 39};

// Card dimensions, pill vertical positions, and name badge boxes in sys_graph_lover.hg3.
constexpr int kLoverReiCardWidth = 502;
constexpr int kLoverReiCardHeight = 691;
constexpr int kLoverReiPillY = 616;
constexpr RectI kLoverReiBadgeRect{170, 500, 156, 39};

constexpr int kLoverYuuhiCardWidth = 577;
constexpr int kLoverYuuhiCardHeight = 680;
constexpr int kLoverYuuhiPillY = 605;
constexpr RectI kLoverYuuhiBadgeRect{214, 489, 156, 39};

constexpr int kLoverChitoseCardWidth = 574;
constexpr int kLoverChitoseCardHeight = 655;
constexpr int kLoverChitosePillY = 580;
constexpr RectI kLoverChitoseBadgeRect{196, 464, 176, 39};

constexpr std::size_t kPatchedContainerCount = 5;
constexpr std::size_t kHexButtonGroupCount = 5;
constexpr std::size_t kMapSelectLocationGroupCount = 15;
constexpr std::size_t kSchoolMapLocationPairCount = 5;
constexpr std::size_t kFriendCardLayoutCount = 4;
constexpr std::size_t kLoverCardLayoutCount = 3;

// Location plate first-frame indices in sys_mapselect.hg3.
constexpr std::size_t kMapSelectRooftopFirstIndex = 21;
constexpr std::size_t kMapSelectHallwayFirstIndex = 24;
constexpr std::size_t kMapSelectClassroomFirstIndex = 27;
constexpr std::size_t kMapSelectCourtyardFirstIndex = 30;
constexpr std::size_t kMapSelectHomeEcFirstIndex = 33;
constexpr std::size_t kMapSelectStationFirstIndex = 46;
constexpr std::size_t kMapSelectMarketFirstIndex = 49;
constexpr std::size_t kMapSelectChayagaiFirstIndex = 52;
constexpr std::size_t kMapSelectSamuraiFirstIndex = 55;
constexpr std::size_t kMapSelectAcademyFirstIndex = 61;
constexpr std::size_t kMapSelectSchoolRouteFirstIndex = 64;
constexpr std::size_t kMapSelectDowntownFirstIndex = 67;
constexpr std::size_t kMapSelectTatemachiFirstIndex = 70;
constexpr std::size_t kMapSelectUniversityFirstIndex = 73;
constexpr std::size_t kMapSelectRenchienFirstIndex = 76;

// Location plate active and idle frame indices in sys_school_map.hg3.
constexpr std::size_t kSchoolMapRooftopActiveIndex = 0;
constexpr std::size_t kSchoolMapHallwayActiveIndex = 1;
constexpr std::size_t kSchoolMapClassroomActiveIndex = 2;
constexpr std::size_t kSchoolMapCourtyardActiveIndex = 3;
constexpr std::size_t kSchoolMapHomeEcActiveIndex = 4;
constexpr std::size_t kSchoolMapRooftopIdleIndex = 5;
constexpr std::size_t kSchoolMapHallwayIdleIndex = 6;
constexpr std::size_t kSchoolMapClassroomIdleIndex = 7;
constexpr std::size_t kSchoolMapCourtyardIdleIndex = 8;
constexpr std::size_t kSchoolMapHomeEcIdleIndex = 9;

struct HexButtonSpec {
    std::string_view label;
    float font_size_px;
};

constexpr std::array<HexButtonSpec, kHexButtonGroupCount> kHexButtonSpecs{{
    {"Confess Feelings", kHexConfessFontSizePx},
    {"Amakanograph", kHexStandardFontSizePx},
    {"Save", kHexStandardFontSizePx},
    {"Load", kHexStandardFontSizePx},
    {"System", kHexStandardFontSizePx},
}};

struct LocationPlateTripletSpec {
    std::size_t first_frame_index;
    std::string_view label;
    bool two_line;
};

constexpr std::array<LocationPlateTripletSpec, kMapSelectLocationGroupCount>
    kMapSelectLocationSpecs{{
        {kMapSelectRooftopFirstIndex, "Rooftop", false},
        {kMapSelectHallwayFirstIndex, "Hallway", false},
        {kMapSelectClassroomFirstIndex, "Classroom", false},
        {kMapSelectCourtyardFirstIndex, "Courtyard", false},
        {kMapSelectHomeEcFirstIndex, "Home Ec Room", false},
        {kMapSelectStationFirstIndex, "Shirasagi Station", false},
        {kMapSelectMarketFirstIndex, "Toumi-cho Market", false},
        {kMapSelectChayagaiFirstIndex, "Nagare Chayagai", false},
        {kMapSelectSamuraiFirstIndex, "Nakamachi\nSamurai District", true},
        {kMapSelectAcademyFirstIndex, "Shirasagi Academy\n(Ishiura Castle Ruins)", true},
        {kMapSelectSchoolRouteFirstIndex, "School Route", false},
        {kMapSelectDowntownFirstIndex, "Shirasagi Downtown", false},
        {kMapSelectTatemachiFirstIndex, "Tatemachi Street", false},
        {kMapSelectUniversityFirstIndex, "Shirasagi University", false},
        {kMapSelectRenchienFirstIndex, "Renchien", false},
    }};

struct SchoolMapPairSpec {
    std::size_t active_index;
    std::size_t idle_index;
    std::string_view label;
};

constexpr std::array<SchoolMapPairSpec, kSchoolMapLocationPairCount> kSchoolMapLocationSpecs{{
    {kSchoolMapRooftopActiveIndex, kSchoolMapRooftopIdleIndex, "Rooftop"},
    {kSchoolMapHallwayActiveIndex, kSchoolMapHallwayIdleIndex, "Hallway"},
    {kSchoolMapClassroomActiveIndex, kSchoolMapClassroomIdleIndex, "Classroom"},
    {kSchoolMapCourtyardActiveIndex, kSchoolMapCourtyardIdleIndex, "Courtyard"},
    {kSchoolMapHomeEcActiveIndex, kSchoolMapHomeEcIdleIndex, "Home Ec Room"},
}};

struct GraphCardLayoutSpec {
    int width;
    int height;
    int pill_y;
    RectI badge_rect;
    std::string_view heroine_name;
    float name_font_size_px;
};

constexpr std::array<GraphCardLayoutSpec, kFriendCardLayoutCount> kFriendCardLayouts{{
    {kFriendReiCardWidth, kFriendReiCardHeight, kFriendReiPillY, kFriendReiBadgeRect,
     "Himiyama Rei", kGraphNameStandardFontSizePx},
    {kFriendYuuhiACardWidth, kFriendYuuhiACardHeight, kFriendYuuhiAPillY, kFriendYuuhiABadgeRect,
     "Kurohime Yuuhi", kGraphNameStandardFontSizePx},
    {kFriendYuuhiBCardWidth, kFriendYuuhiBCardHeight, kFriendYuuhiBPillY, kFriendYuuhiBBadgeRect,
     "Kurohime Yuuhi", kGraphNameStandardFontSizePx},
    {kFriendChitoseCardWidth, kFriendChitoseCardHeight, kFriendChitosePillY,
     kFriendChitoseBadgeRect, "Tsutamachi Chitose", kGraphNameLongFontSizePx},
}};

constexpr std::array<GraphCardLayoutSpec, kLoverCardLayoutCount> kLoverCardLayouts{{
    {kLoverReiCardWidth, kLoverReiCardHeight, kLoverReiPillY, kLoverReiBadgeRect,
     "Himiyama Rei", kGraphNameStandardFontSizePx},
    {kLoverYuuhiCardWidth, kLoverYuuhiCardHeight, kLoverYuuhiPillY, kLoverYuuhiBadgeRect,
     "Kurohime Yuuhi", kGraphNameStandardFontSizePx},
    {kLoverChitoseCardWidth, kLoverChitoseCardHeight, kLoverChitosePillY,
     kLoverChitoseBadgeRect, "Tsutamachi Chitose", kGraphNameLongFontSizePx},
}};

std::size_t MaskIndex(int width, int x, int y) {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x);
}

std::vector<std::uint8_t> DilateMaskClamped(
    const std::vector<std::uint8_t>& src,
    int width,
    int height,
    int radius,
    const RectI& clamp_rect) {
    if (radius <= 0) {
        return src;
    }

    std::vector<std::uint8_t> out(src.size(), 0);
    const RectI clamped = imaging::ClampRect(clamp_rect, width, height);

    for (int y = clamped.y; y < clamped.y + clamped.h; ++y) {
        for (int x = clamped.x; x < clamped.x + clamped.w; ++x) {
            std::uint8_t max_val = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int ny = y + dy;
                if (ny < clamped.y || ny >= clamped.y + clamped.h) {
                    continue;
                }
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int nx = x + dx;
                    if (nx < clamped.x || nx >= clamped.x + clamped.w) {
                        continue;
                    }
                    max_val = std::max(max_val, src[MaskIndex(width, nx, ny)]);
                }
            }
            out[MaskIndex(width, x, y)] = max_val;
        }
    }
    return out;
}

void RenderHexSideMenuButton(BgraImage& image, const HexButtonSpec& spec) {
    SpriteCanvas canvas(std::move(image));
    const int w = canvas.width();
    const int h = canvas.height();

    const RectI scan_rect{
        kHexEraseInsetX,
        kHexEraseInsetY,
        w - kHexDoubledInsetFactor * kHexEraseInsetX,
        h - kHexDoubledInsetFactor * kHexEraseInsetY,
    };

    std::vector<std::uint8_t> glyph_mask(
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    for (int y = scan_rect.y; y < scan_rect.y + scan_rect.h; ++y) {
        for (int x = scan_rect.x; x < scan_rect.x + scan_rect.w; ++x) {
            const BgraColor px = canvas.PixelAt(x, y);
            if (px.g < kHexGlyphMaxGreen || px.b < kHexGlyphMaxBlue) {
                glyph_mask[MaskIndex(w, x, y)] = kOpaqueAlpha;
            }
        }
    }

    const std::vector<std::uint8_t> dilated =
        DilateMaskClamped(glyph_mask, w, h, kHexMaskDilateRadius, scan_rect);
    canvas.EraseMaskedWithInpaint(dilated);

    TextStyle style{};
    style.font_family = L"Segoe UI";
    style.size_px = spec.font_size_px;
    style.weight = kFontWeightBold;
    style.fill = kColorMapMaroonText;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kHexMinSqueeze;
    style.min_scale = kHexMinScale;

    const RectI text_rect{
        kHexTextInsetX,
        kHexTextInsetY,
        w - kHexDoubledInsetFactor * kHexTextInsetX,
        h - kHexDoubledInsetFactor * kHexTextInsetY,
    };
    canvas.DrawText(std::string(spec.label), text_rect, style);
    image = canvas.image();
}

void RenderLocationPlate(BgraImage& image, std::string_view label, bool two_line) {
    if (image.width != kLocationPlateWidth || image.height != kLocationPlateHeight) {
        throw std::runtime_error("Unexpected location plate dimensions");
    }

    SpriteCanvas canvas(std::move(image));
    const BgraColor bg = canvas.PixelAt(kLocationPlateSampleX, kLocationPlateSampleY);
    canvas.Fill(kLocationPlateEraseRect, bg);

    TextStyle style{};
    style.font_family = L"Segoe UI";
    style.weight = kFontWeightBold;
    style.fill = kColorMapMaroonText;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;

    if (two_line) {
        style.size_px = kLocationTwoLineFontSizePx;
        style.line_spacing = kLocationTwoLineSpacing;
        style.min_squeeze = kLocationTwoLineMinSqueeze;
        style.min_scale = kLocationTwoLineMinScale;
        canvas.DrawText(std::string(label), kLocationPlateTwoLineRect, style);
        image = canvas.image();
        return;
    }

    style.size_px = kLocationSingleLineFontSizePx;
    style.min_squeeze = kLocationSingleLineMinSqueeze;
    style.min_scale = kLocationSingleLineMinScale;
    canvas.DrawText(std::string(label), kLocationPlateSingleLineRect, style);
    image = canvas.image();
}

void RenderCityHeaderBanner(BgraImage& image) {
    SpriteCanvas canvas(std::move(image));
    const int w = canvas.width();
    const int h = canvas.height();
    std::vector<std::uint8_t> glyph_mask(
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);

    for (int y = kCityHeaderGlyphScanRect.y;
         y < kCityHeaderGlyphScanRect.y + kCityHeaderGlyphScanRect.h; ++y) {
        for (int x = kCityHeaderGlyphScanRect.x;
             x < kCityHeaderGlyphScanRect.x + kCityHeaderGlyphScanRect.w; ++x) {
            const BgraColor px = canvas.PixelAt(x, y);
            if (px.g > kCityHeaderGlyphMinGreen || px.r > kCityHeaderGlyphMinRed ||
                px.b > kCityHeaderGlyphMinBlue) {
                glyph_mask[MaskIndex(w, x, y)] = kOpaqueAlpha;
            }
        }
    }

    const std::vector<std::uint8_t> dilated =
        DilateMaskClamped(glyph_mask, w, h, kCityHeaderMaskDilateRadius, kCityHeaderMaskClampRect);
    canvas.EraseMaskedWithInpaint(dilated);

    TextStyle style{};
    style.font_family = L"Georgia";
    style.size_px = kCityHeaderFontSizePx;
    style.weight = kFontWeightBold;
    style.fill = kColorWhite;
    style.stroke_px = kCityHeaderStrokePx;
    style.stroke = kColorCityHeaderStroke;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kCityHeaderMinSqueeze;
    style.min_scale = kCityHeaderMinScale;

    canvas.DrawText("Shirasagi City", kCityHeaderTextRect, style);
    image = canvas.image();
}

void RenderMascotSpeechFrame(
    BgraImage& image,
    std::string_view text,
    const wchar_t* font_family,
    int font_weight) {
    SpriteCanvas canvas(std::move(image));
    const RectI full_rect{0, 0, canvas.width(), canvas.height()};
    canvas.Fill(full_rect, kColorTransparent);

    TextStyle style{};
    style.font_family = font_family;
    style.size_px = kMascotSpeechFontSizePx;
    style.weight = font_weight;
    style.fill = kColorBlack;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kMascotSpeechMinSqueeze;
    style.min_scale = kMascotSpeechMinScale;

    canvas.DrawText(std::string(text), kMascotSpeechTextRect, style);
    image = canvas.image();
}

void RenderConfessPromptBanner(
    BgraImage& image,
    std::string_view text,
    const RectI& erase_rect,
    const RectI& text_rect,
    BgraColor fill_color,
    BgraColor stroke_color) {
    const BgraColor sampled_bg = imaging::GetPixel(image, kConfessSampleX, kConfessSampleY);

    // Render against an opaque version of the sampled background, then restore the banner's
    // native alpha channel across the modified region so translucent idle banners stay uniform.
    BgraColor opaque_bg = sampled_bg;
    opaque_bg.a = kOpaqueAlpha;

    SpriteCanvas canvas(std::move(image));
    canvas.Fill(erase_rect, opaque_bg);

    TextStyle style{};
    style.font_family = L"Segoe UI";
    style.size_px = kConfessBannerFontSizePx;
    style.weight = kFontWeightSemibold;
    style.fill = fill_color;
    style.stroke_px = kConfessBannerStrokePx;
    style.stroke = stroke_color;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kConfessBannerMinSqueeze;
    style.min_scale = kConfessBannerMinScale;

    canvas.DrawText(std::string(text), text_rect, style);
    image = canvas.image();

    if (sampled_bg.a == kOpaqueAlpha) {
        return;
    }

    const int x0 = std::max(0, std::min(erase_rect.x, text_rect.x));
    const int y0 = std::max(0, std::min(erase_rect.y, text_rect.y));
    const int x1 = std::min(
        image.width, std::max(erase_rect.x + erase_rect.w, text_rect.x + text_rect.w));
    const int y1 = std::min(
        image.height, std::max(erase_rect.y + erase_rect.h, text_rect.y + text_rect.h));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            BgraColor px = imaging::GetPixel(image, x, y);
            px.a = sampled_bg.a;
            imaging::SetPixel(image, x, y, px);
        }
    }
}

template <std::size_t N>
const GraphCardLayoutSpec& FindGraphCardLayout(
    const std::array<GraphCardLayoutSpec, N>& layouts, int width, int height) {
    for (const auto& layout : layouts) {
        if (layout.width == width && layout.height == height) {
            return layout;
        }
    }
    throw std::runtime_error("Unexpected relationship graph card dimensions");
}

struct AffectionPillCap {
    RectI bounds{};
    BgraColor cap_color{};
};

bool IsValidAffectionCapColor(BgraColor px) {
    if (px.a != kOpaqueAlpha || px.r > kGraphCapMaxRed) {
        return false;
    }
    return static_cast<int>(px.r) >= static_cast<int>(px.g) + kGraphCapMinRedOverGreen;
}

AffectionPillCap EraseAndMeasureAffectionPillCap(BgraImage& image, int pill_y) {
    struct ColorCount {
        BgraColor color{};
        int count = 0;
        int min_x = 0;
    };
    std::vector<ColorCount> histogram;

    for (int y = pill_y; y < pill_y + kGraphPillHeight; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const BgraColor px = imaging::GetPixel(image, x, y);
            if (!IsValidAffectionCapColor(px)) {
                continue;
            }
            bool found = false;
            for (auto& entry : histogram) {
                if (entry.color == px) {
                    ++entry.count;
                    entry.min_x = std::min(entry.min_x, x);
                    found = true;
                    break;
                }
            }
            if (!found) {
                histogram.push_back(ColorCount{px, 1, x});
            }
        }
    }

    int best_min_x = image.width;
    int best_count = 0;
    BgraColor cap_color{};
    for (const auto& entry : histogram) {
        if (entry.count >= kGraphPillMinPixelCount && entry.min_x < best_min_x) {
            best_min_x = entry.min_x;
            best_count = entry.count;
            cap_color = entry.color;
        }
    }

    if (best_count < kGraphPillMinPixelCount) {
        throw std::runtime_error("Failed to locate affection pill cap on relationship graph card");
    }

    int overall_max_x = best_min_x;

    for (int y = pill_y; y < pill_y + kGraphPillHeight; ++y) {
        int row_min_x = image.width;
        int row_max_x = -1;
        for (int x = best_min_x; x <= std::min(image.width - 1, best_min_x + kGraphPillMaxCapWidth);
             ++x) {
            if (imaging::GetPixel(image, x, y) == cap_color) {
                row_min_x = std::min(row_min_x, x);
                row_max_x = std::max(row_max_x, x);
            }
        }
        if (row_max_x >= row_min_x) {
            overall_max_x = std::max(overall_max_x, row_max_x);
            for (int x = row_min_x; x <= row_max_x; ++x) {
                imaging::SetPixel(image, x, y, cap_color);
            }
        }
    }

    const RectI bounds{
        best_min_x,
        pill_y,
        overall_max_x - best_min_x + 1,
        kGraphPillHeight,
    };
    return AffectionPillCap{bounds, cap_color};
}

void RenderRelationshipGraphCard(BgraImage& image, const GraphCardLayoutSpec& layout) {
    const AffectionPillCap cap = EraseAndMeasureAffectionPillCap(image, layout.pill_y);

    const BgraColor badge_bg = cap.cap_color;
    const bool is_dimmed = badge_bg.r > kGraphDimmedRedThreshold;
    const BgraColor affection_fill =
        is_dimmed ? kColorGraphAffectionDim : kColorGraphAffectionLit;
    const BgraColor border_color = is_dimmed ? kColorGraphBorderDim : kColorGraphBorderLit;

    SpriteCanvas canvas(std::move(image));

    TextStyle affection_style{};
    affection_style.font_family = L"Segoe UI";
    affection_style.size_px = kGraphAffectionFontSizePx;
    affection_style.weight = kFontWeightSemibold;
    affection_style.fill = affection_fill;
    affection_style.halign = HorizontalAlign::kCenter;
    affection_style.valign = VerticalAlign::kCenter;
    affection_style.min_squeeze = kGraphAffectionMinSqueeze;
    affection_style.min_scale = kGraphAffectionMinScale;
    canvas.DrawText("Affection", cap.bounds, affection_style);

    canvas.DrawRoundedPill(
        layout.badge_rect,
        kGraphNameBadgeRadiusPx,
        badge_bg,
        kGraphNameBadgeBorderPx,
        border_color);

    TextStyle name_style{};
    name_style.font_family = L"Segoe UI";
    name_style.size_px = layout.name_font_size_px;
    name_style.weight = kFontWeightBold;
    name_style.fill = kColorWhite;
    name_style.stroke_px = kGraphNameStrokePx;
    name_style.stroke = border_color;
    name_style.halign = HorizontalAlign::kCenter;
    name_style.valign = VerticalAlign::kCenter;
    name_style.min_squeeze = kGraphNameMinSqueeze;
    name_style.min_scale = kGraphNameMinScale;
    canvas.DrawText(std::string(layout.heroine_name), layout.badge_rect, name_style);

    image = canvas.image();
}

MenuPatchEntrySpec PatchSysMapSelect(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry{};
    hg3::Hg3Container container = source.LoadHg3("sys_mapselect.hg3", &src_entry);

    for (std::size_t idx = kHexButtonFirstIndex; idx <= kHexButtonLastIndex; ++idx) {
        const std::size_t group = (idx - kHexButtonFirstIndex) / kStatesPerButton;
        RenderHexSideMenuButton(container.frames.at(idx).pixels, kHexButtonSpecs.at(group));
    }

    for (const auto& loc : kMapSelectLocationSpecs) {
        for (std::size_t offset = 0; offset < kStatesPerButton; ++offset) {
            RenderLocationPlate(
                container.frames.at(loc.first_frame_index + offset).pixels,
                loc.label,
                loc.two_line);
        }
    }

    RenderCityHeaderBanner(container.frames.at(kCityHeaderFrameIndex).pixels);

    RenderMascotSpeechFrame(
        container.frames.at(kMascotSpeechFirstIndex).pixels,
        "Azarashiii!",
        L"Segoe UI",
        kFontWeightRegular);
    RenderMascotSpeechFrame(
        container.frames.at(kMascotSpeechSecondIndex).pixels,
        "Azarashiii?",
        L"Segoe UI",
        kFontWeightBold);
    RenderMascotSpeechFrame(
        container.frames.at(kMascotSpeechThirdIndex).pixels,
        "Azarashiii!",
        L"Segoe Print",
        kFontWeightBold);

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec PatchSysSchoolMap(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry{};
    hg3::Hg3Container container = source.LoadHg3("sys_school_map.hg3", &src_entry);

    for (const auto& pair_spec : kSchoolMapLocationSpecs) {
        RenderLocationPlate(
            container.frames.at(pair_spec.active_index).pixels, pair_spec.label, false);
        RenderLocationPlate(
            container.frames.at(pair_spec.idle_index).pixels, pair_spec.label, false);
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec PatchMapKokuhaku(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry{};
    hg3::Hg3Container container = source.LoadHg3("mapkokuhaku.hg3", &src_entry);

    RenderConfessPromptBanner(
        container.frames.at(kConfessCancelActiveIndex).pixels,
        "Hold Off for Now",
        kConfessCancelEraseRect,
        kConfessCancelTextRect,
        kColorConfessCancelFill,
        kColorConfessCancelStroke);
    RenderConfessPromptBanner(
        container.frames.at(kConfessConfirmActiveIndex).pixels,
        "Confess Feelings",
        kConfessConfirmEraseRect,
        kConfessConfirmTextRect,
        kColorConfessConfirmFill,
        kColorConfessConfirmStroke);
    RenderConfessPromptBanner(
        container.frames.at(kConfessCancelIdleIndex).pixels,
        "Hold Off for Now",
        kConfessCancelEraseRect,
        kConfessCancelTextRect,
        kColorConfessCancelFill,
        kColorConfessCancelStroke);
    RenderConfessPromptBanner(
        container.frames.at(kConfessConfirmIdleIndex).pixels,
        "Confess Feelings",
        kConfessConfirmEraseRect,
        kConfessConfirmTextRect,
        kColorConfessConfirmFill,
        kColorConfessConfirmStroke);

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec PatchSysGraphFriend(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry{};
    hg3::Hg3Container container = source.LoadHg3("sys_graph_friend.hg3", &src_entry);

    for (std::size_t idx = kGraphCardFirstIndex; idx <= kGraphFriendLastCardIndex; ++idx) {
        auto& pixels = container.frames.at(idx).pixels;
        const auto& layout = FindGraphCardLayout(kFriendCardLayouts, pixels.width, pixels.height);
        RenderRelationshipGraphCard(pixels, layout);
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

MenuPatchEntrySpec PatchSysGraphLover(const GameAssetSource& source) {
    ExtractedAssetEntry src_entry{};
    hg3::Hg3Container container = source.LoadHg3("sys_graph_lover.hg3", &src_entry);

    for (std::size_t idx = kGraphCardFirstIndex; idx <= kGraphLoverLastCardIndex; ++idx) {
        auto& pixels = container.frames.at(idx).pixels;
        const auto& layout = FindGraphCardLayout(kLoverCardLayouts, pixels.width, pixels.height);
        RenderRelationshipGraphCard(pixels, layout);
    }

    return GameAssetSource::BuildHg3PatchEntry(src_entry, container);
}

class MapGraphsDomainRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "map_graphs"; }

    std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) override {
        (void)options;

        std::vector<MenuPatchEntrySpec> entries;
        entries.reserve(kPatchedContainerCount);
        entries.push_back(PatchSysMapSelect(source));
        entries.push_back(PatchSysSchoolMap(source));
        entries.push_back(PatchMapKokuhaku(source));
        entries.push_back(PatchSysGraphFriend(source));
        entries.push_back(PatchSysGraphLover(source));
        return entries;
    }
};

}  // namespace

std::unique_ptr<IMenuDomainRenderer> CreateMapGraphsDomainRenderer() {
    return std::make_unique<MapGraphsDomainRenderer>();
}

}  // namespace ama::menu
