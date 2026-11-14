// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_system_menus.h"

#include "imaging/bgra_image.h"
#include "menu/core/sprite_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace adi::menu {

namespace {

struct RecollectSceneTitle {
    std::string_view jp_source;
    std::string_view en_title;
};

constexpr std::size_t kRecollectCutCount = 20u;

constexpr std::array<RecollectSceneTitle, kRecollectCutCount> kRecollectTitles = {{
    {"ヘヴンズドア〜", "Heaven's Door~"},
    {"ノウズイノハナ", "Flowers of the Brain"},
    {"天国", "Heaven"},
    {"ヒナ", "Nestling"},
    {"階段13", "Stairway 13"},
    {"エアプレーン", "Airplane"},
    {"俎上", "On the Chopping Block"},
    {"さかづきの…", "Of the Chalice..."},
    {"パ・ドゥ・シャ", "Pas de Chat"},
    {"光の庭", "Garden of Light"},
    {"パンケーキとメープルシロップ", "Pancakes and Maple Syrup"},
    {"阿頼耶識", "Alaya-vijnana"},
    {"福は内 鬼は外", "Fortune In, Demons Out"},
    {"シンクタンクの墓標", "Gravemarker of a Think Tank"},
    {"暖かな春", "Warm Spring"},
    {"ココロノカタチ", "Shape of the Heart"},
    {"マドラーとグラスと", "Stirrer and Glass"},
    {"人間パズル", "Human Puzzle"},
    {"ガイガー・カウンター", "Geiger Counter"},
    {"門", "The Gate"},
}};

constexpr std::string_view kMe01DonorName = "ME_01.g00";
constexpr std::string_view kMe02NormalName = "ME_02.g00";
constexpr std::string_view kMe03ActiveName = "ME_03.g00";
constexpr std::string_view kMe04HoverName = "ME_04.g00";

constexpr int kCanvasWidth = 640;
constexpr int kCanvasHeight = 480;
constexpr int kStripBaseY = 38;
constexpr int kStripStrideY = 20;
constexpr int kStripInteriorRows = 19;

// Left text-free sample band inside each 360x20 strip [280..639] used to verify
// and calibrate ME_02's per-channel offset relative to ME_01.
constexpr int kCalibStartX = 282;
constexpr int kCalibEndX = 312;

// Interior glyph wipe/inpaint zone covering all 20 Japanese titles while
// preserving the 1-pixel bottom separator line at `cut_y + 19` and outer art.
constexpr int kGlyphRegionX = 314;
constexpr int kGlyphRegionW = 224;
constexpr int kGlyphRegionTopInset = 1;
constexpr int kGlyphRegionH = 18;

// Safe English label box centered at x = 412 (matching the retail Japanese
// column visual axis around x = 408..415) with generous width for long titles.
constexpr int kLabelBoxX = 292;
constexpr int kLabelBoxW = 240;

constexpr int kMe02DiffThreshold = 5;
constexpr int kHighlightOutlierThreshold = 14;
constexpr int kDilationRadius = 1;

constexpr float kLabelFontSizePx = 12.0f;
constexpr float kLabelMinFontSizePx = 9.5f;

constexpr imaging::BgraColor kMe02Fill{118, 130, 140, 255};
constexpr imaging::BgraColor kMe02Shadow{6, 6, 8, 195};

constexpr imaging::BgraColor kMe03Fill{196, 226, 232, 255};
constexpr imaging::BgraColor kMe03Shadow{10, 16, 10, 205};

constexpr imaging::BgraColor kMe04Fill{174, 216, 248, 255};
constexpr imaging::BgraColor kMe04Shadow{8, 8, 28, 215};

std::uint8_t ClampToByte(int v) noexcept {
    if (v < 0) {
        return 0u;
    }
    if (v > 255) {
        return 255u;
    }
    return static_cast<std::uint8_t>(v);
}

int MedianInt(std::vector<int>& vals) {
    if (vals.empty()) {
        return 0;
    }
    const std::size_t mid = vals.size() / 2u;
    std::nth_element(vals.begin(), vals.begin() + static_cast<std::ptrdiff_t>(mid),
                     vals.end());
    return vals[mid];
}

struct ChannelOffset {
    int db = 0;
    int dg = 0;
    int dr = 0;
};

ChannelOffset MeasureMe02OffsetFromDonor(const imaging::BgraImage& donor,
                                         const imaging::BgraImage& me02_cut,
                                         int cut_y) {
    std::vector<int> dbs;
    std::vector<int> dgs;
    std::vector<int> drs;
    const std::size_t cap = static_cast<std::size_t>(kCalibEndX - kCalibStartX) *
                            static_cast<std::size_t>(kGlyphRegionH);
    dbs.reserve(cap);
    dgs.reserve(cap);
    drs.reserve(cap);

    for (int ry = 0; ry < kGlyphRegionH; ++ry) {
        const int y = cut_y + kGlyphRegionTopInset + ry;
        for (int x = kCalibStartX; x < kCalibEndX; ++x) {
            const imaging::BgraColor d = donor.PixelAt(x, y);
            const imaging::BgraColor m = me02_cut.PixelAt(x, y);
            dbs.push_back(static_cast<int>(m.b) - static_cast<int>(d.b));
            dgs.push_back(static_cast<int>(m.g) - static_cast<int>(d.g));
            drs.push_back(static_cast<int>(m.r) - static_cast<int>(d.r));
        }
    }
    return ChannelOffset{MedianInt(dbs), MedianInt(dgs), MedianInt(drs)};
}

bool IsBrightLocalGlyphStroke(const imaging::BgraImage& img, int x, int y) {
    constexpr std::array<int, 6> kSampleOffsets = {{-9, -6, -4, 4, 6, 9}};
    std::vector<int> lum_samples;
    lum_samples.reserve(kSampleOffsets.size());
    for (const int dx : kSampleOffsets) {
        const int sx = std::clamp(x + dx, 280, kCanvasWidth - 1);
        const imaging::BgraColor c = img.PixelAt(sx, y);
        lum_samples.push_back(static_cast<int>(c.b) + static_cast<int>(c.g) +
                              static_cast<int>(c.r));
    }
    const int bg_lum = MedianInt(lum_samples);
    const imaging::BgraColor cur = img.PixelAt(x, y);
    const int cur_lum =
        static_cast<int>(cur.b) + static_cast<int>(cur.g) + static_cast<int>(cur.r);
    return (cur_lum - bg_lum) >= (kHighlightOutlierThreshold * 3);
}

std::vector<std::uint8_t> BuildDilatedGlyphMask(
    const imaging::BgraImage& donor,
    const imaging::BgraImage& me02_orig,
    const imaging::BgraImage& me03_orig,
    const imaging::BgraImage& me04_orig,
    int cut_y,
    const ChannelOffset& me02_off) {
    const std::size_t mask_size =
        static_cast<std::size_t>(kGlyphRegionW) * static_cast<std::size_t>(kGlyphRegionH);
    std::vector<std::uint8_t> raw_mask(mask_size, 0u);

    for (int ry = 0; ry < kGlyphRegionH; ++ry) {
        const int y = cut_y + kGlyphRegionTopInset + ry;
        for (int rx = 0; rx < kGlyphRegionW; ++rx) {
            const int x = kGlyphRegionX + rx;
            const imaging::BgraColor d = donor.PixelAt(x, y);
            const imaging::BgraColor m2 = me02_orig.PixelAt(x, y);

            const int pred_b = static_cast<int>(d.b) + me02_off.db;
            const int pred_g = static_cast<int>(d.g) + me02_off.dg;
            const int pred_r = static_cast<int>(d.r) + me02_off.dr;
            const int max_diff = std::max({
                std::abs(static_cast<int>(m2.b) - pred_b),
                std::abs(static_cast<int>(m2.g) - pred_g),
                std::abs(static_cast<int>(m2.r) - pred_r),
            });

            if (max_diff >= kMe02DiffThreshold ||
                IsBrightLocalGlyphStroke(me03_orig, x, y) ||
                IsBrightLocalGlyphStroke(me04_orig, x, y)) {
                raw_mask[static_cast<std::size_t>(ry) *
                             static_cast<std::size_t>(kGlyphRegionW) +
                         static_cast<std::size_t>(rx)] = 1u;
            }
        }
    }

    std::vector<std::uint8_t> dilated(mask_size, 0u);
    for (int ry = 0; ry < kGlyphRegionH; ++ry) {
        for (int rx = 0; rx < kGlyphRegionW; ++rx) {
            bool hit = false;
            for (int dy = -kDilationRadius; dy <= kDilationRadius && !hit; ++dy) {
                const int ny = ry + dy;
                if (ny < 0 || ny >= kGlyphRegionH) {
                    continue;
                }
                for (int dx = -kDilationRadius; dx <= kDilationRadius; ++dx) {
                    const int nx = rx + dx;
                    if (nx < 0 || nx >= kGlyphRegionW) {
                        continue;
                    }
                    if (raw_mask[static_cast<std::size_t>(ny) *
                                     static_cast<std::size_t>(kGlyphRegionW) +
                                 static_cast<std::size_t>(nx)] != 0u) {
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) {
                dilated[static_cast<std::size_t>(ry) *
                            static_cast<std::size_t>(kGlyphRegionW) +
                        static_cast<std::size_t>(rx)] = 1u;
            }
        }
    }
    return dilated;
}

void ValidateMultiCutRecollectSprite(const g00::G00Image& img,
                                     std::string_view filename) {
    if (img.type != g00::G00Type::kType2MultiCutBgra32 ||
        static_cast<int>(img.width) != kCanvasWidth ||
        static_cast<int>(img.height) != kCanvasHeight ||
        img.cuts.size() != kRecollectCutCount) {
        throw std::runtime_error(
            "RenderSystemMenus: unexpected dimensions or cut count in " +
            std::string(filename));
    }
}

TextStyle BuildStripTextStyle(const std::wstring& font_family,
                              imaging::BgraColor fill,
                              imaging::BgraColor shadow) {
    TextStyle style;
    style.font_family = font_family;
    style.weight = kDefaultFontWeightNormal;
    style.size_px = kLabelFontSizePx;
    style.min_font_size_px = kLabelMinFontSizePx;
    style.fill = fill;
    style.shadow = true;
    style.shadow_dx = 1.0f;
    style.shadow_dy = 1.0f;
    style.shadow_blur_px = 1.0f;
    style.shadow_color = shadow;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.wrap = false;
    return style;
}

}  // namespace

struct RenderSystemMenus::Impl {};

RenderSystemMenus::RenderSystemMenus() : impl_(std::make_unique<Impl>()) {}
RenderSystemMenus::RenderSystemMenus(RenderSystemMenus&& other) noexcept = default;
RenderSystemMenus& RenderSystemMenus::operator=(RenderSystemMenus&& other) noexcept = default;
RenderSystemMenus::~RenderSystemMenus() = default;

std::string RenderSystemMenus::DomainName() const {
    return "render_system_menus";
}

void RenderSystemMenus::Apply(PatchBuildContext& ctx) {
    const g00::G00Image me01 = ctx.LoadG00(kMe01DonorName);
    if (static_cast<int>(me01.width) != kCanvasWidth ||
        static_cast<int>(me01.height) != kCanvasHeight) {
        throw std::runtime_error("RenderSystemMenus: unexpected ME_01.g00 dimensions");
    }

    g00::G00Image me02 = ctx.LoadG00(kMe02NormalName);
    g00::G00Image me03 = ctx.LoadG00(kMe03ActiveName);
    g00::G00Image me04 = ctx.LoadG00(kMe04HoverName);
    ValidateMultiCutRecollectSprite(me02, kMe02NormalName);
    ValidateMultiCutRecollectSprite(me03, kMe03ActiveName);
    ValidateMultiCutRecollectSprite(me04, kMe04HoverName);

    const std::wstring font_family =
        ResolveInstalledFontFamily({L"Segoe UI", L"Tahoma", L"Trebuchet MS"});
    const TextStyle me02_style = BuildStripTextStyle(font_family, kMe02Fill, kMe02Shadow);
    const TextStyle me03_style = BuildStripTextStyle(font_family, kMe03Fill, kMe03Shadow);
    const TextStyle me04_style = BuildStripTextStyle(font_family, kMe04Fill, kMe04Shadow);

    std::set<std::size_t> modified_cuts;
    for (std::size_t i = 0u; i < kRecollectCutCount; ++i) {
        modified_cuts.insert(i);
        const int cut_y = kStripBaseY + static_cast<int>(i) * kStripStrideY;
        const imaging::RectI glyph_region{
            kGlyphRegionX, cut_y + kGlyphRegionTopInset, kGlyphRegionW, kGlyphRegionH};
        const imaging::RectI label_bounds{
            kLabelBoxX, cut_y, kLabelBoxW, kStripInteriorRows};

        const imaging::BgraImage me02_orig = me02.cuts[i].canvas;
        const imaging::BgraImage me03_orig = me03.cuts[i].canvas;
        const imaging::BgraImage me04_orig = me04.cuts[i].canvas;

        const ChannelOffset me02_off =
            MeasureMe02OffsetFromDonor(me01.canvas, me02_orig, cut_y);
        const std::vector<std::uint8_t> glyph_mask = BuildDilatedGlyphMask(
            me01.canvas, me02_orig, me03_orig, me04_orig, cut_y, me02_off);

        // 1. ME_02 (unhovered): exact clean donor transfer from ME_01 over masked glyphs.
        imaging::BgraImage& me02_dst = me02.MutableCutCanvas(i);
        for (int ry = 0; ry < kGlyphRegionH; ++ry) {
            const int y = cut_y + kGlyphRegionTopInset + ry;
            for (int rx = 0; rx < kGlyphRegionW; ++rx) {
                if (glyph_mask[static_cast<std::size_t>(ry) *
                                   static_cast<std::size_t>(kGlyphRegionW) +
                               static_cast<std::size_t>(rx)] == 0u) {
                    continue;
                }
                const int x = kGlyphRegionX + rx;
                const imaging::BgraColor d = me01.canvas.PixelAt(x, y);
                me02_dst.SetPixel(x, y, imaging::BgraColor{
                    ClampToByte(static_cast<int>(d.b) + me02_off.db),
                    ClampToByte(static_cast<int>(d.g) + me02_off.dg),
                    ClampToByte(static_cast<int>(d.r) + me02_off.dr),
                    imaging::kOpaqueAlpha,
                });
            }
        }
        SpriteCanvas canvas02(me02_dst);
        canvas02.DrawText(kRecollectTitles[i].en_title, label_bounds, me02_style);
        me02_dst = canvas02.CopyImage();

        // 2. ME_03 (active olive): masked Laplace SOR glyph inpainting + label.
        SpriteCanvas canvas03(me03.MutableCutCanvas(i));
        canvas03.InpaintMaskedLaplaceSor(glyph_region, glyph_mask);
        canvas03.DrawText(kRecollectTitles[i].en_title, label_bounds, me03_style);
        me03.MutableCutCanvas(i) = canvas03.CopyImage();

        // 3. ME_04 (hovered crimson): masked Laplace SOR glyph inpainting + label.
        SpriteCanvas canvas04(me04.MutableCutCanvas(i));
        canvas04.InpaintMaskedLaplaceSor(glyph_region, glyph_mask);
        canvas04.DrawText(kRecollectTitles[i].en_title, label_bounds, me04_style);
        me04.MutableCutCanvas(i) = canvas04.CopyImage();
    }

    ctx.SubmitPatchedG00(kMe02NormalName, me02, modified_cuts);
    ctx.SubmitPatchedG00(kMe03ActiveName, me03, modified_cuts);
    ctx.SubmitPatchedG00(kMe04HoverName, me04, modified_cuts);
}

std::unique_ptr<IMenuDomainRenderer> CreateRenderSystemMenus() {
    return std::make_unique<RenderSystemMenus>();
}

}  // namespace adi::menu
