// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Gameexe.dat English UI localization and NVL/choice layout metric rules for
// Sayonara o Oshiete ~comment te dire adieu~.
//
// Translates all 45 retail Japanese UI strings across 47 active directives
// plus the 19 inactive #SYSCOMMENU.LOCAL_EXTRA_* Japanese template strings
// (64 directives total: #GAMENAME, #DUMMY_CHECK_*, #SYSCOMMENU.*,
// #DIALOG.*.STR, #WARNINGINFO.*, #SAVE.NO_DATA_STR, #SAVEINFO.*, #LOADINFO.*,
// #CHRKOE.NOT_LOOK_NAME_STR) and applies the 7 proportional-font NVL
// (#MWND.002.*) and choice (#SELBTN.000/001.MOJI_SIZE) layout adjustments
// while leaving #DISCMARK ("sayonara.env") and all save structure directives
// untouched.
//
// Thread-compatible.
#pragma once

#include "g00/gameexe_codec.h"
#include "menu/domain_renderer.h"

#include <memory>
#include <string>

namespace adi::menu {

class GameexePatchRules final : public IMenuDomainRenderer {
public:
    GameexePatchRules();
    GameexePatchRules(const GameexePatchRules&) = delete;
    GameexePatchRules& operator=(const GameexePatchRules&) = delete;
    GameexePatchRules(GameexePatchRules&& other) noexcept;
    GameexePatchRules& operator=(GameexePatchRules&& other) noexcept;
    ~GameexePatchRules() override;

    std::string DomainName() const override;
    void Apply(PatchBuildContext& ctx) override;

    // Applies all English UI string and NVL/choice layout rules in-place to a
    // decoded GameexeDocument. Throws std::runtime_error if any expected
    // retail directive is missing.
    static void ApplyToDocument(g00::GameexeDocument& doc);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<IMenuDomainRenderer> CreateGameexePatchRules();

}  // namespace adi::menu
