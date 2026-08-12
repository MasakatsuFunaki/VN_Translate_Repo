// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned XTX substitution tables.  The EN side is FULL-WIDTH romaji on
// purpose -- see xtx.h: the engine crashes on single-byte ASCII in these
// fields.
#include "xtx.h"

namespace exm::xtx {

const std::vector<std::pair<std::string, std::string>>& charaname_translations() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"名前なし", ""},
        {"美弥香", "Ｍｉｙａｋａ"},
        {"綾佳", "Ａｙａｋａ"},
        {"アナウンサー", "Ａｎｎｏｕｎｃｅｒ"},
    };
    return data;
}

}  // namespace exm::xtx
