// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Supplementary CG-mode banner, music-room tracklist, and scrolling CG-caption
// ticker domain renderer (`CM_B02`..`CM_B03`, `MUSIC1`..`MUSIC3`, and
// `CM101`..`CM806`) for 04_patch_menus.
//
// Thread-compatible.
#pragma once

#include "menu/domain_renderer.h"

#include <memory>
#include <string>

namespace adi::menu {

class RenderSupplementary final : public IMenuDomainRenderer {
public:
    RenderSupplementary();
    RenderSupplementary(const RenderSupplementary&) = delete;
    RenderSupplementary& operator=(const RenderSupplementary&) = delete;
    RenderSupplementary(RenderSupplementary&& other) noexcept;
    RenderSupplementary& operator=(RenderSupplementary&& other) noexcept;
    ~RenderSupplementary() override;

    std::string DomainName() const override;
    void Apply(PatchBuildContext& ctx) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<IMenuDomainRenderer> CreateRenderSupplementary();

}  // namespace adi::menu
