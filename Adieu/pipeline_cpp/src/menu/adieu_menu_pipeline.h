// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Factory assembling the complete 5-domain MenuPatchPipeline for Adieu
// (GameexePatchRules, RenderSystemMenus, RenderMessOverlays,
// RenderCardsDialogs, RenderSupplementary).
//
// Thread-compatible.
#pragma once

#include "menu/domain_renderer.h"

namespace adi::menu {

MenuPatchPipeline CreateAdieuMenuPatchPipeline();

}  // namespace adi::menu
