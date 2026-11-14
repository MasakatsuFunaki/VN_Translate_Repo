// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/adieu_menu_pipeline.h"

#include "menu/gameexe_patch_rules.h"
#include "menu/render_cards_dialogs.h"
#include "menu/render_mess_overlays.h"
#include "menu/render_supplementary.h"
#include "menu/render_system_menus.h"

namespace adi::menu {

MenuPatchPipeline CreateAdieuMenuPatchPipeline() {
    MenuPatchPipeline pipeline;
    pipeline.RegisterRenderer(CreateGameexePatchRules());
    pipeline.RegisterRenderer(CreateRenderSystemMenus());
    pipeline.RegisterRenderer(CreateRenderMessOverlays());
    pipeline.RegisterRenderer(CreateRenderCardsDialogs());
    pipeline.RegisterRenderer(CreateRenderSupplementary());
    return pipeline;
}

}  // namespace adi::menu
