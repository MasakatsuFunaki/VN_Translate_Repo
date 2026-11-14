// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Screen-domain renderer for full-screen warning, brand, and ending cards:
//   * ATTEN.g00 (640x480 Type 0): replaces the 3-line Japanese fiction
//     disclaimer beneath the glowing red "attention" / "!" header with a
//     crisp 3x-supersampled English serif disclaimer while preserving the
//     left-edge vertical title watermark and dark gradient background.
//   * LOGO.g00, GRDAT.g00, END01-03.g00, BGJ_1-9.g00, STAFF01-08.g00:
//     verified English/artwork-only or non-interactive credits requiring no
//     sprite overwrite.
//
// Thread-compatible.
#pragma once

#include "menu/domain_renderer.h"

#include <memory>
#include <string>

namespace adi::menu {

class RenderCardsDialogs final : public IMenuDomainRenderer {
public:
    RenderCardsDialogs();
    RenderCardsDialogs(const RenderCardsDialogs&) = delete;
    RenderCardsDialogs& operator=(const RenderCardsDialogs&) = delete;
    RenderCardsDialogs(RenderCardsDialogs&& other) noexcept;
    RenderCardsDialogs& operator=(RenderCardsDialogs&& other) noexcept;
    ~RenderCardsDialogs() override;

    std::string DomainName() const override;
    void Apply(PatchBuildContext& ctx) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<IMenuDomainRenderer> CreateRenderCardsDialogs();

}  // namespace adi::menu
