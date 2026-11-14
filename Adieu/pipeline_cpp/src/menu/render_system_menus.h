// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Screen-domain renderer for system and extra-mode navigation menus:
//   * ME_02.g00, ME_03.g00, ME_04.g00 (Type 2, 20 cuts each): replaces all 20
//     Japanese Recollect Mode ("mémoire") scene title strips across unhovered
//     (ME_02), active/selected olive (ME_03), and hovered crimson (ME_04)
//     button states using clean donor restoration from ME_01.g00 and masked
//     Laplace SOR glyph inpainting.
//   * MENU01-06.g00, MENU09.g00, ME_01.g00, _name_input_init.g00,
//     _name_input_ok.g00, _fmsgbk_*.g00, _cgm_*.g00, _key_*.g00, _mouse_00.g00:
//     verified already English or icon-only in retail.
//
// Thread-compatible.
#pragma once

#include "menu/domain_renderer.h"

#include <memory>
#include <string>

namespace adi::menu {

class RenderSystemMenus final : public IMenuDomainRenderer {
public:
    RenderSystemMenus();
    RenderSystemMenus(const RenderSystemMenus&) = delete;
    RenderSystemMenus& operator=(const RenderSystemMenus&) = delete;
    RenderSystemMenus(RenderSystemMenus&& other) noexcept;
    RenderSystemMenus& operator=(RenderSystemMenus&& other) noexcept;
    ~RenderSystemMenus() override;

    std::string DomainName() const override;
    void Apply(PatchBuildContext& ctx) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<IMenuDomainRenderer> CreateRenderSystemMenus();

}  // namespace adi::menu
