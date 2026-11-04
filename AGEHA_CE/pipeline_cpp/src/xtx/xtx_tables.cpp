// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pinned XTX substitution tables for AGEHA_CE.
// The EN side is full-width romaji on purpose: the BLACKCyc engine crashes
// on single-byte ASCII in XTX display fields.
#include "xtx.h"

namespace exm::xtx {

const std::vector<std::pair<std::string, std::string>>& charaname_translations() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"名前なし", ""},
        {"アゲハ", "Ａｇｅｈａ"},
        {"シュミ", "Ｓｈｕｍｉ"},
    };
    return data;
}

}  // namespace exm::xtx
