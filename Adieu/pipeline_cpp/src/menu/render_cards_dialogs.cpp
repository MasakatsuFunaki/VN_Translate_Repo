// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_cards_dialogs.h"

#include "imaging/bgra_image.h"
#include "menu/core/sprite_canvas.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace adi::menu {

namespace {

constexpr std::string_view kAttenFilename = "ATTEN.g00";
constexpr int kAttenExpectedWidth = 640;
constexpr int kAttenExpectedHeight = 480;

// Covers the 3 Japanese disclaimer lines (y = 219..283, x = 88..562) while
// staying well right of the vertical dark title watermark (x = 8..65) and
// well below the white "!" mark (y = 198..212).
constexpr imaging::RectI kAttenWipeRect{82, 215, 490, 76};
constexpr int kAttenRowMedianMarginPx = 8;
constexpr imaging::RectI kAttenTextBounds{76, 216, 488, 76};

constexpr float kAttenFontSizePx = 13.5f;
constexpr float kAttenMinFontSizePx = 10.0f;
constexpr float kAttenLineSpacing = 1.38f;
constexpr imaging::BgraColor kAttenInkFill{0xEA, 0xEC, 0xEE, 0xFF};
constexpr imaging::BgraColor kAttenShadowColor{0x00, 0x00, 0x00, 0xC0};

// Original Japanese:
//   この作品はフィクションです。
//   登場する人名・団体名・地名・事件および職業などはすべて架空のものであり、
//   実在のものとは一切関係ありません。
constexpr std::string_view kAttenEnglishDisclaimer =
    "This work is a work of fiction.\n"
    "All characters, organizations, places, events, and occupations\n"
    "depicted herein are entirely fictitious and bear no relation to reality.";

}  // namespace

struct RenderCardsDialogs::Impl {};

RenderCardsDialogs::RenderCardsDialogs() : impl_(std::make_unique<Impl>()) {}
RenderCardsDialogs::RenderCardsDialogs(RenderCardsDialogs&& other) noexcept = default;
RenderCardsDialogs& RenderCardsDialogs::operator=(RenderCardsDialogs&& other) noexcept = default;
RenderCardsDialogs::~RenderCardsDialogs() = default;

std::string RenderCardsDialogs::DomainName() const {
    return "render_cards_dialogs";
}

void RenderCardsDialogs::Apply(PatchBuildContext& ctx) {
    g00::G00Image atten = ctx.LoadG00(kAttenFilename);
    if (static_cast<int>(atten.width) != kAttenExpectedWidth ||
        static_cast<int>(atten.height) != kAttenExpectedHeight) {
        throw std::runtime_error(
            "RenderCardsDialogs: unexpected ATTEN.g00 dimensions");
    }

    SpriteCanvas canvas(atten.canvas);
    canvas.WipeRectRowMedian(kAttenWipeRect, kAttenRowMedianMarginPx);

    TextStyle style;
    style.font_family = ResolveInstalledFontFamily(
        {L"Georgia", L"Palatino Linotype", L"Times New Roman"});
    style.weight = kDefaultFontWeightNormal;
    style.size_px = kAttenFontSizePx;
    style.min_font_size_px = kAttenMinFontSizePx;
    style.line_spacing = kAttenLineSpacing;
    style.fill = kAttenInkFill;
    style.shadow = true;
    style.shadow_dx = 0.0f;
    style.shadow_dy = 1.0f;
    style.shadow_blur_px = 1.0f;
    style.shadow_color = kAttenShadowColor;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.wrap = false;

    canvas.DrawText(kAttenEnglishDisclaimer, kAttenTextBounds, style);
    atten.MutableCanvas() = canvas.CopyImage();
    ctx.SubmitPatchedG00(kAttenFilename, atten);
}

std::unique_ptr<IMenuDomainRenderer> CreateRenderCardsDialogs() {
    return std::make_unique<RenderCardsDialogs>();
}

}  // namespace adi::menu
