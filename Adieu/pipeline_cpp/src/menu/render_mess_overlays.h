// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Screen-domain renderer (`render_mess_overlays`) patching Adieu's 18 `MESS*` sprite
// assets: school map location buttons (`MESS99.g00`), school map location
// headers (`MESS98.g00`), baked scene overlay (`MESS81.g00` restored from
// `MESS80.g00`), and 15 monologue/flashback text-strip atlases (`MESS01.g00`
// through `MESS52.g00`).
//
// Thread-compatible.
#pragma once

#include "menu/domain_renderer.h"

#include <memory>
#include <string>

namespace adi::menu {

class RenderMessOverlays final : public IMenuDomainRenderer {
public:
    RenderMessOverlays();
    RenderMessOverlays(const RenderMessOverlays&) = delete;
    RenderMessOverlays& operator=(const RenderMessOverlays&) = delete;
    RenderMessOverlays(RenderMessOverlays&& other) noexcept;
    RenderMessOverlays& operator=(RenderMessOverlays&& other) noexcept;
    ~RenderMessOverlays() override;

    std::string DomainName() const override;
    void Apply(PatchBuildContext& ctx) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<IMenuDomainRenderer> CreateRenderMessOverlays();

}  // namespace adi::menu
