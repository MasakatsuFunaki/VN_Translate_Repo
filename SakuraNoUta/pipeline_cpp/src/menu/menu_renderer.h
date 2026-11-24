// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// PIMPL-insulated visual renderer for Sakura no Uta's localized menus, startup
// splash screens, Extra Music track banners, confirmation dialogs, quick-menu
// tooltips, choice option plates, and 1280x720 QA preview mockups.
//
// Thread-compatible: separate MenuRenderer instances may be used concurrently.
#pragma once

#include "menu/bgra_image.h"
#include "menu/menu_catalog.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snu::menu {

struct SplashPairRenderResult {
    BgraImage att01;
    BgraImage att02;
};

struct PreviewMockupItem {
    std::string file_stem;
    BgraImage image;
};

using SysGrpLookupFn = std::function<const BgraImage*(std::string_view entry_name)>;

class MenuRenderer {
public:
    MenuRenderer();
    MenuRenderer(const MenuRenderer&) = delete;
    MenuRenderer& operator=(const MenuRenderer&) = delete;
    MenuRenderer(MenuRenderer&& other) noexcept;
    MenuRenderer& operator=(MenuRenderer&& other) noexcept;
    ~MenuRenderer();

    // Restores the shared 1280x720 cherry-blossom walkway via cross-donor clean
    // pixel transfer + Laplace-SOR inpainting, localizes the bottom-right logo
    // subtitle, and renders both startup splash screens (`att01` and `att02`).
    SplashPairRenderResult RenderSplashScreenPair(const BgraImage& orig_att01,
                                                  const BgraImage& orig_att02) const;

    // Single-image fallback when only one splash asset is present in a fixture.
    BgraImage RenderSingleSplashScreen(const BgraImage& orig,
                                       const SplashScreenSpec& spec) const;

    // Localizes `SGTitle990000` (1280x720): inpaints the 8 small subtitle kanji
    // between the horizontal rules while keeping the feather-moon calligraphy
    // intact, renders the English subtitle, and adds the `Sakura no Uta` caption.
    BgraImage RenderTitleBackground(const BgraImage& orig) const;

    // Localizes `SGCG800001` (291x170): inpaints the tiny subtitle kanji between
    // its horizontal rules and renders `Sakura no Uta`.
    BgraImage RenderLockedCgThumbnail(const BgraImage& orig) const;

    // Localizes `SGCnfgWnd990000` (1280x720): masks only warm-charcoal Japanese
    // glyph pixels inside each label/slider rect (preserving all glowing
    // blue-white feathers and dotted dividers), draws 9 left + 4 right setting
    // rows + slider endpoints, and rotates `Character Voices` 90° CCW.
    BgraImage RenderConfigBackground(const BgraImage& orig) const;

    // Localizes all supplied `SGCnfgWnd010000..060100` 4-cell toggle strips
    // (552x37x32), cross-donoring clean sakura-petal pixels across sibling
    // strips of the same theme before Laplace-SOR inpainting.
    std::map<std::string, BgraImage> RenderConfigToggleStrips(
        const std::map<std::string, BgraImage>& orig_strips) const;

    // Localizes a 2-cell `SGCnfgWnd100000..110100` voice toggle portrait
    // (200x130): removes the vertical Japanese name on the left of each cell
    // and renders a crisp white-stroked horizontal English nameplate at bottom.
    BgraImage RenderConfigVoicePortrait(const BgraImage& orig,
                                        const ConfigVoicePortraitSpec& spec) const;

    // Localizes `SGDialog990000..990007` (722x242x32): keeps the orange `!`
    // warning triangle 100% pixel-exact, wipes right of the triangle via
    // `WipeRectRowMedian` (preserving exact translucent plate alpha), and draws
    // the left-aligned English prompt.
    BgraImage RenderDialogPlate(const BgraImage& orig,
                                const DialogPlateSpec& spec) const;

    // Localizes `SGMsgWnd800000..800009` (440x20x32): wipes the interior bar via
    // `WipeRectRowMedian` (preserving rounded brown endcaps) and draws centered
    // white tooltip text.
    BgraImage RenderMsgTooltip(const BgraImage& orig,
                               const MsgTooltipSpec& spec) const;

    // Crops the 21x20 pink sakura blossom icon from retail `SGMusic000100` at
    // `RectI{581, 11, 21, 20}`.
    BgraImage ExtractMusicSakuraIcon(const BgraImage& orig_music_000100) const;

    // Localizes `SGMusic000100..004200` (864x42x32): preserves 100% transparent
    // (`alpha == 0`) background, draws charcoal/violet titles in Cells 0 and 1,
    // and centers `[21px sakura icon + 5px gap + rose-pink title]` in Cell 2.
    BgraImage RenderMusicTrackBanner(const MusicTrackSpec& spec,
                                     const BgraImage& sakura_icon,
                                     int width = 864,
                                     int height = 42) const;

    // Finds the largest integer font size in `[12, 26]` at which every string in
    // `english_texts` fits inside `tile_width - 93 - 20` pixels.
    int ComputeChoiceFontSizePx(const std::vector<std::string>& english_texts,
                                int tile_width) const;

    // Renders a 3-state choice option plate (`width = 3 * tile_w`, `height = 58`,
    // `TILE_ALPHA = {130, 182, 182}` translucent white tiles + black text).
    BgraImage RenderChoiceOptionPlate(std::string_view english_text,
                                      int width,
                                      int height,
                                      int font_size_px) const;

    // Composites full 1280x720 mockups (Splash 1, Splash 2, Title Screen,
    // Config Screen, Music Mode, Confirmation Dialog, and Choice Overlay) for
    // visual inspection when `--previews` is requested.
    std::vector<PreviewMockupItem> BuildFullScreenPreviews(
        const BgraImage* patched_att01,
        const BgraImage* patched_att02,
        const SysGrpLookupFn& lookup_orig_sysgrp,
        const SysGrpLookupFn& lookup_patched_sysgrp,
        const std::vector<std::pair<std::string, BgraImage>>& patched_choices) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snu::menu
