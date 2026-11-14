// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_mess_overlays.h"

#include "g00/g00_codec.h"
#include "imaging/bgra_image.h"
#include "menu/core/sprite_canvas.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace adi::menu {
namespace {

using imaging::BgraColor;
using imaging::RectI;

constexpr std::string_view kDomainName = "render_mess_overlays";

constexpr int kExpectedCanvasW = 640;
constexpr int kExpectedCanvasH = 480;

constexpr BgraColor kOpaqueBlack{0x00u, 0x00u, 0x00u, 0xFFu};
constexpr BgraColor kOpaqueWhite{0xFFu, 0xFFu, 0xFFu, 0xFFu};
constexpr BgraColor kStripBrightWhite{0xFCu, 0xFCu, 0xFCu, 0xFFu};
constexpr BgraColor kStripDimGray{0x68u, 0x68u, 0x68u, 0xFFu};
constexpr BgraColor kStripInkBlack{0x08u, 0x08u, 0x08u, 0xFFu};
constexpr BgraColor kMess20StrokeDark{0x70u, 0x70u, 0x70u, 0x90u};
constexpr BgraColor kMess20ShadowDark{0x00u, 0x00u, 0x00u, 0xFFu};
constexpr BgraColor kButtonYellow{0x00u, 0xFFu, 0xFFu, 0xFFu};
constexpr BgraColor kAccentRed{0x00u, 0x00u, 0xFFu, 0xFFu};

// Typography defaults for monologue/flashback strips (`MESS01`..`MESS52`).
constexpr float kMonologueFontSizePx = 16.5f;
constexpr float kMonologueMinSqueeze = 0.80f;
constexpr float kMonologueMinScale = 0.55f;
constexpr float kMonologueMultiLineSpacing = 1.05f;
constexpr int kStripPadX = 8;
constexpr int kStripPadY = 1;
constexpr int kStripMultiLinePadX = 12;
constexpr int kStripMultiLinePadY = 2;

// `MESS20.g00` white-on-white embossed relief shadow parameters.
constexpr float kMess20FontSizePx = 18.0f;
constexpr float kMess20StrokePx = 0.30f;
constexpr float kMess20ShadowDx = 1.6f;
constexpr float kMess20ShadowDy = 1.1f;
constexpr float kMess20ShadowBlurPx = 0.0f;

// `MESS99.g00` school map button layout invariants.
constexpr std::size_t kMess99ExpectedCutCount = 31u;
constexpr std::size_t kMess99LocationCount = 7u;
constexpr std::size_t kMess99HoverCutOffset = 10u;
constexpr int kMess99ExpectedCutW = 101;
constexpr int kMess99ExpectedCutH = 20;
constexpr int kMess99HoverWipeRows = 19;
constexpr int kMess99TextLeftInset = 22;
constexpr int kMess99TextRightMargin = 6;
constexpr int kMess99TextBottomMargin = 2;
constexpr float kMess99FontSizePx = 12.0f;
constexpr float kMess99MinSqueeze = 0.78f;
constexpr float kMess99MinScale = 0.60f;

// `MESS98.g00` school map header atlas invariants.
constexpr int kMess98GridRows = 16;
constexpr int kMess98TileHeight = 30;
constexpr int kMess98DotLeftX = 160;
constexpr int kMess98DotRightX = 319;
constexpr int kMess98LeftWipeX = 80;
constexpr int kMess98RightWipeX = 400;
constexpr int kMess98WipeW = 79;
constexpr int kMess98WipeYOffset = 4;
constexpr int kMess98WipeH = 22;
constexpr int kMess98TextXInset = 1;
constexpr int kMess98TextW = 77;
constexpr float kMess98FontSizePx = 11.5f;
constexpr float kMess98MinSqueeze = 0.76f;
constexpr float kMess98MinScale = 0.55f;

// `MESS81.g00` scene overlay invariants (restored from `MESS80.g00`).
constexpr int kMess81DonorBlitH = 160;
constexpr int kMess81TextX = 44;
constexpr int kMess81TextW = 576;
constexpr int kMess81LineH = 27;
constexpr int kMess81Line0Y = 27;
constexpr int kMess81Line1Y = 57;
constexpr int kMess81Line2Y = 87;
constexpr float kMess81FontSizePx = 17.0f;
constexpr float kMess81StrokePx = 1.6f;
constexpr float kMess81ShadowBlurPx = 1.4f;
constexpr float kMess81MinSqueeze = 0.80f;
constexpr float kMess81MinScale = 0.62f;

// `MESS20.g00` & `MESS25.g00` 6-row vertical grid invariants.
constexpr int kMess20CellX = 1;
constexpr int kMess20CellW = 160;
constexpr int kMess20CellH = 29;
constexpr int kMess20RowStepY = 30;

// `MESS02.g00` & `MESS03.g00` 5-band progressive stack invariants.
constexpr int kMess02BandW = 640;
constexpr int kMess02BandH = 91;
constexpr int kMess02BandStepY = 92;
constexpr int kMess02SubLineH = 25;
constexpr int kMess02TopOffsetY = 3;
constexpr int kMess02MidOffsetY = 33;
constexpr int kMess02BotOffsetY = 62;
constexpr int kMess02OuterX = 54;
constexpr int kMess02OuterWideW = 532;
constexpr int kMess02OuterNarrowW = 502;
constexpr int kMess02MidX = 44;
constexpr int kMess02MidW = 552;

struct FontPalette {
    std::wstring serif_family;
    std::wstring ui_sans_family;
};

struct SingleStripSpec {
    RectI cell;
    BgraColor bg;
    std::string_view text;
    bool dark_ink_on_white = false;
};

struct Mess98HeaderEntry {
    int wipe_x;
    int row;
    std::string_view text;
};

void VerifyType0Canvas(const g00::G00Image& img, std::string_view filename) {
    if (img.type != g00::G00Type::kType0Bgr24) {
        throw std::runtime_error(std::string(filename) + ": expected Type 0 G00 sprite");
    }
    if (img.width != kExpectedCanvasW || img.height != kExpectedCanvasH) {
        throw std::runtime_error(std::string(filename) + ": unexpected canvas dimensions");
    }
}

RectI MakeInsetRect(const RectI& cell, int pad_x, int pad_y) noexcept {
    return RectI{
        cell.x + pad_x,
        cell.y + pad_y,
        cell.w - 2 * pad_x,
        cell.h - 2 * pad_y,
    };
}

TextStyle MakeMonologueBrightStyle(const std::wstring& serif_family,
                                   HorizontalAlign halign = HorizontalAlign::kCenter) {
    TextStyle style;
    style.font_family = serif_family;
    style.weight = kDefaultFontWeightNormal;
    style.size_px = kMonologueFontSizePx;
    style.fill = kStripBrightWhite;
    style.halign = halign;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kMonologueMinSqueeze;
    style.min_scale = kMonologueMinScale;
    return style;
}

TextStyle MakeMonologueDimStyle(const std::wstring& serif_family,
                                HorizontalAlign halign = HorizontalAlign::kCenter) {
    TextStyle style = MakeMonologueBrightStyle(serif_family, halign);
    style.fill = kStripDimGray;
    return style;
}

TextStyle MakeMonologueBlackInkStyle(const std::wstring& serif_family,
                                     HorizontalAlign halign = HorizontalAlign::kCenter) {
    TextStyle style = MakeMonologueBrightStyle(serif_family, halign);
    style.fill = kStripInkBlack;
    return style;
}

TextStyle MakeMess20WhiteOnWhiteStyle(const std::wstring& serif_family) {
    TextStyle style = MakeMonologueBrightStyle(serif_family, HorizontalAlign::kCenter);
    style.weight = kDefaultFontWeightBold;
    style.size_px = kMess20FontSizePx;
    style.fill = kOpaqueWhite;
    style.stroke_px = kMess20StrokePx;
    style.stroke_color = kMess20StrokeDark;
    style.shadow = true;
    style.shadow_dx = kMess20ShadowDx;
    style.shadow_dy = kMess20ShadowDy;
    style.shadow_blur_px = kMess20ShadowBlurPx;
    style.shadow_color = kMess20ShadowDark;
    return style;
}

void WipeAndDrawCell(SpriteCanvas& canvas, const RectI& cell, BgraColor bg,
                     std::string_view text, const TextStyle& style) {
    canvas.WipeRectSolid(cell, bg);
    const RectI safe = MakeInsetRect(cell, kStripPadX, kStripPadY);
    canvas.DrawText(text, safe, style);
}

// Renders `prefix_text` anchored to the left ink edge of `full_text` (when
// `full_text` is centered inside `cell`) so crossfaded prefix-to-full strip
// transitions keep the shared opening words stationary on screen.
void WipeAndDrawPrefixCell(SpriteCanvas& canvas, const RectI& cell, BgraColor bg,
                           std::string_view prefix_text, std::string_view full_text,
                           const TextStyle& centered_style) {
    canvas.WipeRectSolid(cell, bg);
    const RectI safe = MakeInsetRect(cell, kStripPadX, kStripPadY);
    const TextLayoutMetrics full_metrics = canvas.MeasureText(full_text, safe, centered_style);
    if (full_metrics.ink_bounds.w <= 0) {
        canvas.DrawText(prefix_text, safe, centered_style);
        return;
    }

    const int start_x = std::max(safe.x, full_metrics.ink_bounds.x);
    const int avail_w = safe.x + safe.w - start_x;
    if (avail_w <= 0) {
        canvas.DrawText(prefix_text, safe, centered_style);
        return;
    }

    TextStyle prefix_style = centered_style;
    prefix_style.halign = HorizontalAlign::kLeft;
    prefix_style.size_px = full_metrics.applied_font_size_px;
    prefix_style.min_squeeze = std::min(prefix_style.min_squeeze, full_metrics.applied_squeeze);
    canvas.DrawText(prefix_text, RectI{start_x, safe.y, avail_w, safe.h}, prefix_style);
}

void ApplySingleStripSpecs(SpriteCanvas& canvas, const FontPalette& fonts,
                           const SingleStripSpec* specs, std::size_t count) {
    const TextStyle white_ink = MakeMonologueBrightStyle(fonts.serif_family);
    const TextStyle black_ink = MakeMonologueBlackInkStyle(fonts.serif_family);
    for (std::size_t i = 0u; i < count; ++i) {
        const SingleStripSpec& spec = specs[i];
        WipeAndDrawCell(canvas, spec.cell, spec.bg, spec.text,
                        spec.dark_ink_on_white ? black_ink : white_ink);
    }
}

void SubmitType0Canvas(PatchBuildContext& ctx, std::string_view filename, g00::G00Image& img,
                       const SpriteCanvas& canvas) {
    img.MutableCanvas() = canvas.CopyImage();
    ctx.SubmitPatchedG00(filename, img);
}

// ---- Individual asset patchers ----------------------------------------------

void PatchMess99(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS99.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    if (img.type != g00::G00Type::kType2MultiCutBgra32 ||
        img.cuts.size() != kMess99ExpectedCutCount) {
        throw std::runtime_error("MESS99.g00: expected Type 2 sprite with 31 cuts");
    }

    // Cuts 0..6 (yellow normal) and 10..16 (red hover with 1px underline on bottom row).
    // Cut 30 ("Select to Spot.") and cuts 20..25 (map scribbles) remain untouched.
    constexpr std::array<std::string_view, kMess99LocationCount> kLabels = {{
        "Infirmary",      // 0 / 10: 保健室
        "Classroom",      // 1 / 11: 教室
        "Rooftop",        // 2 / 12: 屋上
        "Courtyard",      // 3 / 13: 中庭
        "Library",        // 4 / 14: 図書館
        "Archery Range",  // 5 / 15: 弓道場
        "Go Home",        // 6 / 16: 帰る
    }};

    TextStyle normal_style;
    normal_style.font_family = fonts.ui_sans_family;
    normal_style.weight = kDefaultFontWeightSemiBold;
    normal_style.size_px = kMess99FontSizePx;
    normal_style.fill = kButtonYellow;
    normal_style.halign = HorizontalAlign::kLeft;
    normal_style.valign = VerticalAlign::kCenter;
    normal_style.min_squeeze = kMess99MinSqueeze;
    normal_style.min_scale = kMess99MinScale;

    TextStyle hover_style = normal_style;
    hover_style.fill = kAccentRed;

    std::set<std::size_t> modified_cuts;
    for (std::size_t i = 0u; i < kMess99LocationCount; ++i) {
        const std::size_t normal_idx = i;
        const std::size_t hover_idx = kMess99HoverCutOffset + i;

        const g00::G00CutDescriptor normal_desc = img.cuts.at(normal_idx).descriptor;
        const int normal_w = normal_desc.x2 - normal_desc.x1 + 1;
        const int normal_h = normal_desc.y2 - normal_desc.y1 + 1;
        if (normal_w != kMess99ExpectedCutW || normal_h != kMess99ExpectedCutH) {
            throw std::runtime_error("MESS99.g00: unexpected normal cut geometry");
        }

        SpriteCanvas normal_canvas(std::move(img.MutableCutCanvas(normal_idx)));
        normal_canvas.WipeRectSolid(RectI{normal_desc.x1, normal_desc.y1, normal_w, normal_h},
                                    kOpaqueBlack);
        const RectI normal_text_box{
            normal_desc.x1 + kMess99TextLeftInset,
            normal_desc.y1,
            normal_w - kMess99TextLeftInset - kMess99TextRightMargin,
            normal_h - kMess99TextBottomMargin,
        };
        normal_canvas.DrawText(kLabels[i], normal_text_box, normal_style);
        img.MutableCutCanvas(normal_idx) = normal_canvas.CopyImage();
        modified_cuts.insert(normal_idx);

        const g00::G00CutDescriptor hover_desc = img.cuts.at(hover_idx).descriptor;
        const int hover_w = hover_desc.x2 - hover_desc.x1 + 1;
        const int hover_h = hover_desc.y2 - hover_desc.y1 + 1;
        if (hover_w != kMess99ExpectedCutW || hover_h != kMess99ExpectedCutH) {
            throw std::runtime_error("MESS99.g00: unexpected hover cut geometry");
        }

        SpriteCanvas hover_canvas(std::move(img.MutableCutCanvas(hover_idx)));
        // Wipe only the top 19 rows so scanline `y = hover_desc.y2` preserves
        // the retail 1px red underline spanning `x = hover_desc.x1 + 22 .. hover_desc.x2`.
        hover_canvas.WipeRectSolid(
            RectI{hover_desc.x1, hover_desc.y1, hover_w, kMess99HoverWipeRows}, kOpaqueBlack);
        const RectI hover_text_box{
            hover_desc.x1 + kMess99TextLeftInset,
            hover_desc.y1,
            hover_w - kMess99TextLeftInset - kMess99TextRightMargin,
            hover_h - kMess99TextBottomMargin,
        };
        hover_canvas.DrawText(kLabels[i], hover_text_box, hover_style);

        for (int ux = hover_desc.x1 + kMess99TextLeftInset; ux <= hover_desc.x2; ++ux) {
            if (hover_canvas.PixelAt(ux, hover_desc.y2) != kAccentRed) {
                throw std::logic_error("MESS99.g00: hover underline pixel was disturbed");
            }
        }
        img.MutableCutCanvas(hover_idx) = hover_canvas.CopyImage();
        modified_cuts.insert(hover_idx);
    }

    ctx.SubmitPatchedG00(kFilename, img, modified_cuts);
}

void PatchMess98(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS98.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    // Capture all 32 1px registration markers at x = 160 and x = 319 across rows 0..15.
    std::array<BgraColor, kMess98GridRows> left_dots{};
    std::array<BgraColor, kMess98GridRows> right_dots{};
    for (int row = 0; row < kMess98GridRows; ++row) {
        const int y = row * kMess98TileHeight;
        left_dots[static_cast<std::size_t>(row)] = img.canvas.PixelAt(kMess98DotLeftX, y);
        right_dots[static_cast<std::size_t>(row)] = img.canvas.PixelAt(kMess98DotRightX, y);
    }

    constexpr std::array<Mess98HeaderEntry, 17u> kHeaders = {{
        // Left column (`x = 80..158`, clear of `x = 160`).
        {kMess98LeftWipeX, 1, "Boys' Restroom"},      // 男子トイレ
        {kMess98LeftWipeX, 3, "Rooftop"},             // 屋上
        {kMess98LeftWipeX, 4, "Stairs"},              // 階段
        {kMess98LeftWipeX, 5, "Archery Range"},       // 弓道場
        {kMess98LeftWipeX, 6, "Classroom"},           // 教室
        {kMess98LeftWipeX, 9, "Staff Room"},          // 職員室
        {kMess98LeftWipeX, 10, "Library"},            // 図書館
        {kMess98LeftWipeX, 11, "Courtyard"},          // 中庭
        {kMess98LeftWipeX, 12, "Walkway"},            // 渡り廊下
        {kMess98LeftWipeX, 13, "Infirmary"},          // 保健室
        {kMess98LeftWipeX, 14, "Hallway"},            // 廊下
        {kMess98LeftWipeX, 15, "Outside Infirmary"},  // 保健室前
        // Right column (`x = 400..478`, clear of `x = 319`).
        {kMess98RightWipeX, 1, "Exam Room"},          // 診察室
        {kMess98RightWipeX, 3, "Infirmary"},          // 保健室
        {kMess98RightWipeX, 4, "Outside Archery"},    // 弓道場前
        {kMess98RightWipeX, 5, "Outside Library"},    // 図書館前
        {kMess98RightWipeX, 6, "Grove"},              // 雑木林
    }};

    TextStyle header_style;
    header_style.font_family = fonts.ui_sans_family;
    header_style.weight = kDefaultFontWeightSemiBold;
    header_style.size_px = kMess98FontSizePx;
    header_style.fill = kAccentRed;
    header_style.halign = HorizontalAlign::kLeft;
    header_style.valign = VerticalAlign::kCenter;
    header_style.min_squeeze = kMess98MinSqueeze;
    header_style.min_scale = kMess98MinScale;

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    for (const Mess98HeaderEntry& entry : kHeaders) {
        const int wipe_y = entry.row * kMess98TileHeight + kMess98WipeYOffset;
        canvas.WipeRectSolid(RectI{entry.wipe_x, wipe_y, kMess98WipeW, kMess98WipeH}, kOpaqueBlack);
        canvas.DrawText(
            entry.text,
            RectI{entry.wipe_x + kMess98TextXInset, wipe_y, kMess98TextW, kMess98WipeH},
            header_style);
    }

    for (int row = 0; row < kMess98GridRows; ++row) {
        const int y = row * kMess98TileHeight;
        const std::size_t idx = static_cast<std::size_t>(row);
        if (canvas.PixelAt(kMess98DotLeftX, y) != left_dots[idx] ||
            canvas.PixelAt(kMess98DotRightX, y) != right_dots[idx]) {
            throw std::logic_error("MESS98.g00: 1px registration marker was modified");
        }
    }

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess81(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS81.g00";
    constexpr std::string_view kDonorFilename = "MESS80.g00";

    g00::G00Image img = ctx.LoadG00(kFilename);
    const g00::G00Image donor = ctx.LoadG00(kDonorFilename);
    VerifyType0Canvas(img, kFilename);
    VerifyType0Canvas(donor, kDonorFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    canvas.BlitCleanRegion(donor.canvas, RectI{0, 0, kExpectedCanvasW, kMess81DonorBlitH}, 0, 0);

    TextStyle style;
    style.font_family = fonts.serif_family;
    style.weight = kDefaultFontWeightBold;
    style.size_px = kMess81FontSizePx;
    style.fill = kOpaqueWhite;
    style.stroke_px = kMess81StrokePx;
    style.stroke_color = kOpaqueBlack;
    style.shadow = true;
    style.shadow_dx = 1.0f;
    style.shadow_dy = 1.0f;
    style.shadow_blur_px = kMess81ShadowBlurPx;
    style.shadow_color = kOpaqueBlack;
    style.halign = HorizontalAlign::kLeft;
    style.valign = VerticalAlign::kCenter;
    style.min_squeeze = kMess81MinSqueeze;
    style.min_scale = kMess81MinScale;

    // 『キミは僕の指をそのアナルに呑み込んで気をやっただろう？』
    canvas.DrawText(
        "\"You swallowed my fingers into your ass and came, didn't you?\"",
        RectI{kMess81TextX, kMess81Line0Y, kMess81TextW, kMess81LineH}, style);

    // 『前の穴より後ろの穴の方が感じるなんてキミは変態なんだな』
    canvas.DrawText(
        "\"Feeling it more in your back hole than the front--what a pervert you are.\"",
        RectI{kMess81TextX, kMess81Line1Y, kMess81TextW, kMess81LineH}, style);

    // 『ウンチの出るばっちい穴をイジられて恥ずかしくなかった？』
    canvas.DrawText(
        "\"Weren't you ashamed, getting fingered in the filthy hole where shit comes out?\"",
        RectI{kMess81TextX, kMess81Line2Y, kMess81TextW, kMess81LineH}, style);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess01(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS01.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    const TextStyle centered_bright = MakeMonologueBrightStyle(fonts.serif_family);

    constexpr std::array<SingleStripSpec, 9u> kSingleCells = {{
        {RectI{1, 1, 230, 29}, kOpaqueBlack, "White..."},                      // 白・・・・
        {RectI{251, 1, 230, 29}, kOpaqueBlack, "I can't hear anything."},      // なにも聞こえない
        {RectI{1, 31, 230, 29}, kOpaqueBlack, "Light..."},                     // 光・・・・
        {RectI{1, 61, 230, 29}, kOpaqueBlack, "Is it... light?"},              // 光・・・・なのか？
        {RectI{251, 61, 230, 29}, kOpaqueBlack, "Nothing is there."},          // なにもない
        {RectI{1, 91, 230, 29}, kOpaqueBlack, "I can't see anything."},        // なにも見えない
        {RectI{251, 91, 230, 29}, kOpaqueBlack, "Pure white equals darkness."},  // 純白は、闇に等しい
        // すべてに優しく、すべてを癒し、すべてを隠す・・・・
        {RectI{1, 391, 580, 29}, kOpaqueBlack, "Gentle to all, healing all, concealing all..."},
        // すべてを晒し、すべてを暴き、すべてを見せる・・・・
        {RectI{1, 421, 580, 29}, kOpaqueBlack, "Exposing all, laying bare all, revealing all..."},
    }};
    ApplySingleStripSpecs(canvas, fonts, kSingleCells.data(), kSingleCells.size());

    // なにも -> なにもない progressive prefix cell.
    WipeAndDrawPrefixCell(canvas, RectI{251, 31, 230, 29}, kOpaqueBlack, "Nothing...",
                          "Nothing is there.", centered_bright);

    // Four 3-line progressive blocks: 過ぎたる白は、 / 過ぎたる黒と / 等価だった
    constexpr std::string_view kLineTop = "Excessive white,";
    constexpr std::string_view kLineMid = "and excessive black";
    constexpr std::string_view kLineBot = "were equivalent.";

    const TextStyle left_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kLeft);
    const TextStyle left_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kLeft);
    const TextStyle mid_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kCenter);
    const TextStyle mid_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kCenter);
    const TextStyle right_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kRight);
    const TextStyle right_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kRight);

    // Block (1, 211): top bright.
    canvas.WipeRectSolid(RectI{1, 211, 300, 89}, kOpaqueBlack);
    canvas.DrawText(kLineTop, RectI{12, 214, 276, 25}, left_bright);

    // Block (1, 301): top dim, mid bright.
    canvas.WipeRectSolid(RectI{1, 301, 300, 89}, kOpaqueBlack);
    canvas.DrawText(kLineTop, RectI{12, 304, 276, 25}, left_dim);
    canvas.DrawText(kLineMid, RectI{12, 333, 276, 25}, mid_bright);

    // Block (311, 211): top dim, mid dim, bottom bright.
    canvas.WipeRectSolid(RectI{311, 211, 300, 89}, kOpaqueBlack);
    canvas.DrawText(kLineTop, RectI{322, 214, 276, 25}, left_dim);
    canvas.DrawText(kLineMid, RectI{322, 243, 276, 25}, mid_dim);
    canvas.DrawText(kLineBot, RectI{322, 272, 276, 25}, right_bright);

    // Block (311, 301): all three dim.
    canvas.WipeRectSolid(RectI{311, 301, 300, 89}, kOpaqueBlack);
    canvas.DrawText(kLineTop, RectI{322, 304, 276, 25}, left_dim);
    canvas.DrawText(kLineMid, RectI{322, 333, 276, 25}, mid_dim);
    canvas.DrawText(kLineBot, RectI{322, 362, 276, 25}, right_dim);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess02(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS02.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));

    // 甘く溶ける白の救い / 「は、ぁ・・・・んんっ、っくぅ・・・・はぁあぁぁ・・・・」 / 喘ぎ / 生命の力・・・・
    constexpr std::string_view kTopText = "Salvation of sweetly melting white";
    constexpr std::string_view kMidDots = "\"Ha, ah... nngh, khh... haaah...\"";
    constexpr std::string_view kMidDash = "\"Ha, ah... nngh, khh... haaah--\"";
    constexpr std::string_view kBotText = "Gasping";
    constexpr std::string_view kFinalText = "The force of life...";

    const TextStyle left_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kLeft);
    const TextStyle left_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kLeft);
    const TextStyle mid_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kCenter);
    const TextStyle mid_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kCenter);
    const TextStyle right_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kRight);
    const TextStyle right_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kRight);

    for (int band = 0; band < 5; ++band) {
        const int y0 = band * kMess02BandStepY;
        canvas.WipeRectSolid(RectI{0, y0, kMess02BandW, kMess02BandH}, kOpaqueBlack);

        const RectI top_box{kMess02OuterX, y0 + kMess02TopOffsetY, kMess02OuterWideW,
                            kMess02SubLineH};
        const RectI mid_box{kMess02MidX, y0 + kMess02MidOffsetY, kMess02MidW, kMess02SubLineH};
        const RectI bot_box{kMess02OuterX, y0 + kMess02BotOffsetY, kMess02OuterNarrowW,
                            kMess02SubLineH};

        if (band == 0) {
            canvas.DrawText(kMidDots, mid_box, mid_bright);
            continue;
        }
        if (band == 1) {
            canvas.DrawText(kMidDots, mid_box, mid_dim);
            canvas.DrawText(kBotText, bot_box, right_bright);
            continue;
        }
        if (band == 2) {
            canvas.DrawText(kTopText, top_box, left_bright);
            canvas.DrawText(kMidDots, mid_box, mid_dim);
            canvas.DrawText(kBotText, bot_box, right_dim);
            continue;
        }
        if (band == 3) {
            canvas.DrawText(kTopText, top_box, left_dim);
            canvas.DrawText(kMidDash, mid_box, mid_dim);
            canvas.DrawText(kBotText, bot_box, right_dim);
            continue;
        }
        canvas.DrawText(kFinalText, mid_box, mid_bright);
    }

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess03(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS03.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));

    // 淫らに溶ける黒の誘惑 / 「・・・・んっぅ、ふ、はぁ・・・・あぁっ、うぁっ・・・・」 / 喘ぎ / 死への誘い・・・・
    constexpr std::string_view kTopText = "Temptation of lewdly melting black";
    constexpr std::string_view kMidText = "\"...Nngh, fu, haah... aah, uah...\"";
    constexpr std::string_view kBotText = "Gasping";
    constexpr std::string_view kFinalText = "The lure toward death...";

    const TextStyle left_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kLeft);
    const TextStyle left_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kLeft);
    const TextStyle mid_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kCenter);
    const TextStyle mid_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kCenter);
    const TextStyle right_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kRight);
    const TextStyle right_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kRight);

    for (int band = 0; band < 5; ++band) {
        const int y0 = band * kMess02BandStepY;
        canvas.WipeRectSolid(RectI{0, y0, kMess02BandW, kMess02BandH}, kOpaqueBlack);

        const RectI top_box{kMess02OuterX, y0 + kMess02TopOffsetY, kMess02OuterNarrowW,
                            kMess02SubLineH};
        const RectI mid_box{kMess02MidX, y0 + kMess02MidOffsetY, kMess02MidW, kMess02SubLineH};
        const RectI bot_box{kMess02OuterX, y0 + kMess02BotOffsetY, kMess02OuterNarrowW,
                            kMess02SubLineH};

        if (band == 0) {
            canvas.DrawText(kMidText, mid_box, mid_bright);
            continue;
        }
        if (band == 1) {
            canvas.DrawText(kMidText, mid_box, mid_dim);
            canvas.DrawText(kBotText, bot_box, left_bright);
            continue;
        }
        if (band == 2) {
            canvas.DrawText(kTopText, top_box, right_bright);
            canvas.DrawText(kMidText, mid_box, mid_dim);
            canvas.DrawText(kBotText, bot_box, left_dim);
            continue;
        }
        if (band == 3) {
            canvas.DrawText(kTopText, top_box, right_dim);
            canvas.DrawText(kMidText, mid_box, mid_dim);
            canvas.DrawText(kBotText, bot_box, left_dim);
            continue;
        }
        canvas.DrawText(kFinalText, mid_box, mid_bright);
    }

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess04(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS04.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 12u> kSpecs = {{
        // 白き者は・・・・天使
        {RectI{1, 1, 440, 29}, kOpaqueBlack, "The white one is... an angel.", false},
        // 黒き者は・・・・怪物
        {RectI{1, 31, 440, 29}, kOpaqueBlack, "The black one is... a monster.", false},
        // そして、闇
        {RectI{1, 61, 440, 29}, kOpaqueBlack, "And then, darkness.", false},
        // 形而上的な闇。無に帰すところ・・・・
        {RectI{1, 91, 440, 29}, kOpaqueBlack,
         "Metaphysical darkness. Where all returns to nothing...", false},
        // どこかで見た顔
        {RectI{1, 121, 440, 29}, kOpaqueWhite, "A face I've seen somewhere.", true},
        // よく知っている誰か・・・・
        {RectI{1, 151, 440, 29}, kOpaqueWhite, "Someone I know well...", true},
        // 天使の顔
        {RectI{1, 181, 440, 29}, kOpaqueWhite, "The face of an angel.", true},
        // あれは・・・・見知った顔だったような気が・・・・
        {RectI{1, 211, 540, 29}, kOpaqueWhite, "That felt like... a familiar face...", true},
        // 怪物の顔
        {RectI{1, 241, 440, 29}, kOpaqueWhite, "The face of a monster.", true},
        // 知っている顔だったような気がする・・・・
        {RectI{1, 271, 440, 29}, kOpaqueWhite, "I feel like it was a face I knew...", true},
        // 顔
        {RectI{1, 301, 440, 29}, kOpaqueWhite, "A face.", true},
        // 顔だ
        {RectI{1, 331, 440, 29}, kOpaqueWhite, "It's a face.", true},
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess05(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS05.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 6u> kSpecs = {{
        // 白が黒を支配する
        {RectI{1, 1, 540, 29}, kOpaqueBlack, "White dominates black.", false},
        // 黒が白を浸食する
        {RectI{1, 31, 540, 29}, kOpaqueWhite, "Black erodes white.", true},
        // 白が白を・・・・白に染める
        {RectI{1, 91, 540, 29}, kOpaqueBlack, "White dyes white... pure white.", false},
        // 言葉は・・・・
        {RectI{1, 121, 540, 29}, kOpaqueWhite, "Her words...", true},
        // 彼女の唇は、さよならのカタチをえがいて・・・・
        {RectI{1, 151, 540, 29}, kOpaqueWhite, "Her lips trace the shape of goodbye...", true},
        // ・・・・こわばる
        {RectI{1, 181, 540, 29}, kOpaqueBlack, "...and stiffen.", false},
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    // 白が白を・・・・ -> 白が白を・・・・白に染める progressive prefix cell.
    const TextStyle centered_bright = MakeMonologueBrightStyle(fonts.serif_family);
    WipeAndDrawPrefixCell(canvas, RectI{1, 61, 540, 29}, kOpaqueBlack, "White dyes white...",
                          "White dyes white... pure white.", centered_bright);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess11(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS11.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 9u> kSpecs = {{
        {RectI{1, 1, 350, 29}, kOpaqueBlack, "A dream..."},                                   // 夢・・・・
        {RectI{1, 31, 350, 29}, kOpaqueBlack, "A dream...?"},                                 // 夢か・・・・
        {RectI{1, 61, 350, 29}, kOpaqueBlack, "A woman...?"},                                 // 女・・・・？
        {RectI{1, 91, 350, 29}, kOpaqueBlack, "A girl...?"},                                  // 少女・・・・？
        {RectI{1, 121, 350, 29}, kOpaqueBlack, "An angel...?"},                               // 天使・・・・？
        {RectI{1, 151, 350, 29}, kOpaqueBlack, "It's a dream."},                              // 夢だ
        {RectI{1, 181, 350, 29}, kOpaqueBlack, "A dream of an angel."},                       // 天使の夢だ
        {RectI{1, 211, 350, 29}, kOpaqueBlack, "A dream of an angel violated by a monster."}, // 天使が怪物に犯される夢だ
        {RectI{1, 271, 350, 29}, kOpaqueBlack, "Such a... dream..."},                         // そんな・・・・夢・・・・
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    // そんな・・・・ -> そんな・・・・夢・・・・ progressive prefix cell.
    const TextStyle centered_bright = MakeMonologueBrightStyle(fonts.serif_family);
    WipeAndDrawPrefixCell(canvas, RectI{1, 241, 350, 29}, kOpaqueBlack, "Such a...",
                          "Such a... dream...", centered_bright);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess12(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS12.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 9u> kSpecs = {{
        {RectI{1, 1, 350, 29}, kOpaqueBlack, "\"...!\""},                                // 「・・・・！」
        {RectI{1, 31, 350, 29}, kOpaqueBlack, "Her scream cannot be heard..."},          // 彼女の叫びは聞こえない・・・・
        {RectI{1, 61, 350, 29}, kOpaqueBlack, "One who stands against the angel."},      // 天使に相対する者
        {RectI{1, 91, 350, 29}, kOpaqueBlack, "One who spits upon sanctity."},           // 聖性に唾する者
        {RectI{1, 121, 350, 29}, kOpaqueBlack, "One who wishes darkness upon the light."},  // 光には闇を、と願う者
        {RectI{1, 151, 350, 29}, kOpaqueBlack, "One who stains the bright world black."},   // 光明の世界を黒く染める者
        {RectI{1, 181, 350, 29}, kOpaqueBlack, "Just as all who defy 'God'"},            // 『神』と敵対する者、すべてが
        {RectI{1, 211, 350, 29}, kOpaqueBlack, "are branded 'demons'..."},               // 『魔』とされるが如く・・・・
        {RectI{1, 450, 500, 29}, kOpaqueBlack, "Her scream does not reach my ears..."},  // 彼女の叫びは、僕に聞こえない・・・・
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    const TextStyle center_dim = MakeMonologueDimStyle(fonts.serif_family, HorizontalAlign::kCenter);
    const TextStyle right_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kRight);
    const TextStyle left_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kLeft);

    // Block (1, 326, 397, 61): 「・・・・！」 (dim) / 聞こえない (bright)
    canvas.WipeRectSolid(RectI{1, 326, 397, 61}, kOpaqueBlack);
    canvas.DrawText("\"...!\"", RectI{14, 328, 370, 26}, center_dim);
    canvas.DrawText("I can't hear it.", RectI{14, 358, 340, 26}, right_bright);

    // Block (1, 388, 397, 61): 天使の声は聞こえない (bright) / 「・・・・！」 (dim)
    canvas.WipeRectSolid(RectI{1, 388, 397, 61}, kOpaqueBlack);
    canvas.DrawText("I cannot hear the angel's voice.", RectI{16, 390, 368, 26}, left_bright);
    canvas.DrawText("\"...!\"", RectI{14, 420, 370, 26}, center_dim);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess20(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS20.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    const TextStyle style = MakeMess20WhiteOnWhiteStyle(fonts.serif_family);

    // 白。 / 白だ。 / 僕の、 / 自分が、 / 消えて、 / 消えてしまう。
    constexpr std::array<std::string_view, 6u> kLines = {{
        "White.",
        "It is white.",
        "My...",
        "My very self...",
        "is fading,",
        "fading away.",
    }};

    for (std::size_t r = 0u; r < kLines.size(); ++r) {
        const RectI cell{
            kMess20CellX,
            static_cast<int>(r) * kMess20RowStepY + 1,
            kMess20CellW,
            kMess20CellH,
        };
        WipeAndDrawCell(canvas, cell, kOpaqueWhite, kLines[r], style);
    }

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess21(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS21.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 9u> kSpecs = {{
        {RectI{1, 1, 250, 29}, kOpaqueBlack, "A dream..."},            // 夢・・・・
        {RectI{261, 1, 250, 29}, kOpaqueBlack, "A dream of an angel."},  // 天使の夢だ
        {RectI{1, 31, 250, 29}, kOpaqueBlack, "A familiar dream..."},  // 見覚えのある夢・・・・
        {RectI{261, 31, 250, 29}, kOpaqueBlack, "That... dream..."},   // あの・・・・夢・・・・
        {RectI{1, 61, 250, 29}, kOpaqueBlack, "That woman...?"},       // あの女・・・・？
        {RectI{261, 61, 250, 29}, kOpaqueBlack, "\"...!\""},           // 「・・・・！」
        {RectI{1, 91, 250, 29}, kOpaqueBlack, "That girl...?"},        // あの少女・・・・？
        {RectI{1, 121, 250, 29}, kOpaqueBlack, "An angel...?"},        // 天使・・・・？
        {RectI{1, 151, 250, 29}, kOpaqueBlack, "Again... that dream."},  // また・・・・あの夢だ
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    const TextStyle left_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kLeft);
    const TextStyle right_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kRight);
    const TextStyle center_bright = MakeMonologueBrightStyle(fonts.serif_family);

    // Block (1, 211, 439, 60): 『あの』天使が、
    canvas.WipeRectSolid(RectI{1, 211, 439, 60}, kOpaqueBlack);
    canvas.DrawText("'That' angel...", RectI{38, 214, 388, 26}, left_bright);

    // Block (1, 272, 439, 60): 『あの』天使が、 / 『あの』怪物に犯される夢だ
    canvas.WipeRectSolid(RectI{1, 272, 439, 60}, kOpaqueBlack);
    canvas.DrawText("'That' angel...", RectI{38, 275, 388, 26}, left_bright);
    canvas.DrawText("violated in a dream by 'that' monster.", RectI{24, 304, 408, 26},
                    right_bright);

    // Block (1, 333, 439, 60): あの・・・・
    canvas.WipeRectSolid(RectI{1, 333, 439, 60}, kOpaqueBlack);
    canvas.DrawText("That...", RectI{16, 349, 408, 28}, center_bright);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess22(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS22.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    const TextStyle center_bright = MakeMonologueBrightStyle(fonts.serif_family);
    const TextStyle center_dim = MakeMonologueDimStyle(fonts.serif_family);
    const TextStyle right_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kRight);
    const TextStyle left_bright =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kLeft);

    // 彼女の叫びは、僕に聞こえない・・・・
    WipeAndDrawCell(canvas, RectI{1, 1, 500, 29}, kOpaqueBlack,
                    "Her scream does not reach my ears...", center_bright);

    // Block (1, 61, 350, 61): 「・・・・！」 (dim) / 聞こえない (bright)
    canvas.WipeRectSolid(RectI{1, 61, 350, 61}, kOpaqueBlack);
    canvas.DrawText("\"...!\"", RectI{12, 63, 326, 26}, center_dim);
    canvas.DrawText("I can't hear it.", RectI{12, 92, 310, 26}, right_bright);

    // Block (1, 123, 350, 61): 天使の声は聞こえない (bright) / 「・・・・！」 (dim)
    canvas.WipeRectSolid(RectI{1, 123, 350, 61}, kOpaqueBlack);
    canvas.DrawText("I cannot hear the angel's voice.", RectI{14, 125, 324, 26}, left_bright);
    canvas.DrawText("\"...!\"", RectI{12, 154, 326, 26}, center_dim);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess25(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS25.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    const TextStyle style = MakeMonologueBrightStyle(fonts.serif_family);

    // 白。 / 白だ。 / 僕の、 / 自分が、 / 消えて、 / 消えてしまう。
    constexpr std::array<std::string_view, 6u> kLines = {{
        "White.",
        "It is white.",
        "My...",
        "My very self...",
        "is fading,",
        "fading away.",
    }};

    for (std::size_t r = 0u; r < kLines.size(); ++r) {
        const RectI cell{
            kMess20CellX,
            static_cast<int>(r) * kMess20RowStepY + 1,
            kMess20CellW,
            kMess20CellH,
        };
        WipeAndDrawCell(canvas, cell, kOpaqueBlack, kLines[r], style);
    }

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess31(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS31.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 11u> kSpecs = {{
        {RectI{1, 1, 300, 29}, kOpaqueBlack, "Again...?"},                    // またか・・・・
        {RectI{311, 1, 300, 29}, kOpaqueBlack, "That angel again...?"},       // また、あの天使・・・・？
        {RectI{1, 31, 300, 29}, kOpaqueBlack, "That dream again..."},         // また、あの夢・・・・
        {RectI{311, 31, 300, 29}, kOpaqueBlack, "The usual dream."},          // いつもの夢
        {RectI{1, 61, 300, 29}, kOpaqueBlack, "Give me a break already..."},  // いい加減にしてくれ・・・・
        {RectI{311, 61, 300, 29}, kOpaqueBlack, "Her... dream of the angel."},  // 彼女の・・・・天使の夢だ
        {RectI{1, 91, 300, 29}, kOpaqueBlack, "That woman again...?"},        // また、あの女・・・・？
        {RectI{1, 121, 300, 29}, kOpaqueBlack, "That girl again...?"},        // また、あの少女・・・・？
        {RectI{1, 151, 450, 29}, kOpaqueBlack, "Again, that angel..."},       // また、あの天使が、
        // またしても、あの怪物に犯される夢だ
        {RectI{1, 181, 450, 29}, kOpaqueBlack,
         "Once again, a dream where that monster violates her."},
        // また・・・・あの夢だ・・・・
        {RectI{1, 241, 450, 29}, kOpaqueBlack, "Again... that dream..."},
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    // また・・・・ -> また・・・・あの夢だ・・・・ progressive prefix cell.
    const TextStyle centered_bright = MakeMonologueBrightStyle(fonts.serif_family);
    WipeAndDrawPrefixCell(canvas, RectI{1, 211, 450, 29}, kOpaqueBlack, "Again...",
                          "Again... that dream...", centered_bright);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess41(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS41.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    // Eye animation frames at x = 400..488, y = 1..159 stay completely outside all wipe rects.
    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 10u> kSpecs = {{
        {RectI{1, 1, 350, 29}, kOpaqueBlack, "That dream..."},                      // あの夢・・・・
        {RectI{1, 31, 350, 29}, kOpaqueBlack, "The usual dream..."},                // いつもの夢・・・・
        {RectI{1, 61, 350, 29}, kOpaqueBlack, "Terror, and ecstasy. And then,"},    // 恐怖と、恍惚。そして、
        {RectI{1, 91, 350, 29}, kOpaqueBlack, "That woman again...?"},              // また、あの女・・・・？
        {RectI{1, 121, 350, 29}, kOpaqueBlack, "That girl again...?"},              // また、あの少女・・・・？
        {RectI{1, 151, 350, 29}, kOpaqueBlack, "That angel again..."},              // また、あの天使・・・・
        {RectI{1, 181, 350, 29}, kOpaqueBlack, "Again... it's that dream."},        // また・・・・あの夢だ
        {RectI{1, 211, 350, 29}, kOpaqueBlack, "Today too... that dream of the angel."},  // 今日もまた・・・・あの、天使の夢
        {RectI{1, 331, 350, 29}, kOpaqueBlack, "The usual... dream..."},            // いつもの・・・・あの夢・・・・
        // もはや懐かしささえ覚えるあの夢・・・・
        {RectI{1, 361, 500, 29}, kOpaqueBlack, "That dream, which almost feels nostalgic now..."},
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    const TextStyle centered_bright = MakeMonologueBrightStyle(fonts.serif_family);
    // 『また』あの天使が、 / 『また』あの怪物に犯される shared-prefix pair.
    WipeAndDrawPrefixCell(canvas, RectI{1, 241, 350, 29}, kOpaqueBlack, "'Again,' that angel...",
                          "'Again,' violated by that monster.", centered_bright);
    WipeAndDrawCell(canvas, RectI{1, 271, 350, 29}, kOpaqueBlack,
                    "'Again,' violated by that monster.", centered_bright);

    // いつもの・・・・ -> いつもの・・・・あの夢・・・・ progressive prefix cell.
    WipeAndDrawPrefixCell(canvas, RectI{1, 301, 350, 29}, kOpaqueBlack, "The usual...",
                          "The usual... dream...", centered_bright);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess51(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS51.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    // Wiping strictly inside y >= 1 preserves scanline y = 0 (including its x = 230..318 notch).
    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 12u> kSpecs = {{
        // 『え？』
        {RectI{1, 1, 550, 29}, kOpaqueBlack, "'Huh?'"},
        // 『せ、先生は・・・・先生も、男の人ですもんね・・・・』
        {RectI{1, 31, 550, 29}, kOpaqueBlack, "'S-Sensei... you're a man too, after all...'"},
        // 『ここから見る風景が、一番好きなんです・・・・』
        {RectI{1, 61, 550, 29}, kOpaqueBlack, "'The view from up here is my very favorite...'"},
        // 『ふふっ・・・・くすくすくす・・・・』
        {RectI{1, 91, 550, 29}, kOpaqueBlack, "'Fufu... heeheehee...'"},
        // 『あの、私、ここにいても・・・・いいですか？』
        {RectI{1, 121, 550, 29}, kOpaqueBlack, "'Um... may I... stay here?'"},
        // 『それまでは・・・・います・・・・』
        {RectI{1, 151, 550, 29}, kOpaqueBlack, "'Until then... I'll be here...'"},
        // 『・・・・さよなら』
        {RectI{1, 181, 550, 29}, kOpaqueBlack, "'...Goodbye.'"},
        // 『なんにも、見えない・・・・』
        {RectI{1, 272, 550, 29}, kOpaqueBlack, "'I can't... see anything...'"},
        // 『貴方のでしょ！　手伝って頂戴』
        {RectI{1, 302, 550, 29}, kOpaqueBlack, "'It's yours, isn't it! Come help me.'"},
        // 『でも・・・・おにーちゃんは、おにーちゃんだもん！』
        {RectI{1, 332, 550, 29}, kOpaqueBlack, "'But... Onii-chan is still Onii-chan!'"},
        // 『恐いんです、人が。特に・・・・男の人が・・・・』
        {RectI{1, 362, 550, 29}, kOpaqueBlack, "'People frighten me. Especially... men...'"},
        // 『うーん・・・・先生って、見栄っ張りなんですねぇ』
        {RectI{1, 392, 550, 29}, kOpaqueBlack,
         "'Hmm... Sensei, you really like showing off, don't you?'"},
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    // 2-line cell (1, 211, 550, 60): 『あンた、意外とセコいんだねェ・・・・へへっ、モ / テないだろ～？』
    const RectI two_line_cell{1, 211, 550, 60};
    canvas.WipeRectSolid(two_line_cell, kOpaqueBlack);
    TextStyle two_line_style =
        MakeMonologueBrightStyle(fonts.serif_family, HorizontalAlign::kLeft);
    two_line_style.wrap = true;
    two_line_style.line_spacing = kMonologueMultiLineSpacing;
    canvas.DrawText(
        "'You're surprisingly petty, huh... Heh, bet you're\nnot popular with women, are you?'",
        MakeInsetRect(two_line_cell, kStripMultiLinePadX, kStripMultiLinePadY), two_line_style);

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

void PatchMess52(PatchBuildContext& ctx, const FontPalette& fonts) {
    constexpr std::string_view kFilename = "MESS52.g00";
    g00::G00Image img = ctx.LoadG00(kFilename);
    VerifyType0Canvas(img, kFilename);

    SpriteCanvas canvas(std::move(img.MutableCanvas()));
    constexpr std::array<SingleStripSpec, 7u> kSpecs = {{
        {RectI{1, 1, 500, 29}, kOpaqueBlack, "You're..."},                      // キミは・・・・
        {RectI{1, 31, 500, 29}, kOpaqueBlack, "Why..."},                        // どうして・・・・
        {RectI{1, 61, 500, 29}, kOpaqueBlack, "Wait! What about me..."},        // 待って！　僕を・・・・
        {RectI{1, 91, 500, 29}, kOpaqueBlack, "Wait..."},                       // 待って・・・・
        {RectI{1, 121, 500, 29}, kOpaqueBlack, "That can't be... No..."},       // そんなこと・・・・そんな・・・・
        // いちばんダメな僕・・・・僕にも、なんにも・・・・
        {RectI{1, 151, 500, 29}, kOpaqueBlack, "The most worthless me... Even for me, nothing..."},
        {RectI{1, 181, 500, 29}, kOpaqueBlack, "My... My... what?"},            // 僕の・・・・僕の・・・・なに？
    }};
    ApplySingleStripSpecs(canvas, fonts, kSpecs.data(), kSpecs.size());

    SubmitType0Canvas(ctx, kFilename, img, canvas);
}

}  // namespace

struct RenderMessOverlays::Impl {
    FontPalette fonts;
    bool fonts_resolved = false;

    const FontPalette& EnsureFonts() {
        if (!fonts_resolved) {
            fonts.serif_family = ResolveInstalledFontFamily(
                {L"Georgia", L"Palatino Linotype", L"Times New Roman"});
            fonts.ui_sans_family =
                ResolveInstalledFontFamily({L"Segoe UI", L"Trebuchet MS"});
            fonts_resolved = true;
        }
        return fonts;
    }
};

RenderMessOverlays::RenderMessOverlays() : impl_(std::make_unique<Impl>()) {}

RenderMessOverlays::RenderMessOverlays(RenderMessOverlays&& other) noexcept = default;

RenderMessOverlays& RenderMessOverlays::operator=(RenderMessOverlays&& other) noexcept = default;

RenderMessOverlays::~RenderMessOverlays() = default;

std::string RenderMessOverlays::DomainName() const { return std::string(kDomainName); }

void RenderMessOverlays::Apply(PatchBuildContext& ctx) {
    const FontPalette& fonts = impl_->EnsureFonts();

    PatchMess01(ctx, fonts);
    PatchMess02(ctx, fonts);
    PatchMess03(ctx, fonts);
    PatchMess04(ctx, fonts);
    PatchMess05(ctx, fonts);
    PatchMess11(ctx, fonts);
    PatchMess12(ctx, fonts);
    PatchMess20(ctx, fonts);
    PatchMess21(ctx, fonts);
    PatchMess22(ctx, fonts);
    PatchMess25(ctx, fonts);
    PatchMess31(ctx, fonts);
    PatchMess41(ctx, fonts);
    PatchMess51(ctx, fonts);
    PatchMess52(ctx, fonts);
    PatchMess81(ctx, fonts);
    PatchMess98(ctx, fonts);
    PatchMess99(ctx, fonts);
}

std::unique_ptr<IMenuDomainRenderer> CreateRenderMessOverlays() {
    return std::make_unique<RenderMessOverlays>();
}

}  // namespace adi::menu
