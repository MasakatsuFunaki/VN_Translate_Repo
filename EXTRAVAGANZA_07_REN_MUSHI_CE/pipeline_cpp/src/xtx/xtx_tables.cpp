// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Character name-plate translations for Ren_mushi_CE spt/charaname.xtx.
//
// Full-width Latin characters are used intentionally: the BLACKCyc engine
// crashes when single-byte ASCII appears in its .xtx configuration tables.
#include "xtx.h"

namespace exm::xtx {

const std::vector<std::pair<std::string, std::string>>& charaname_translations() {
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"名前なし", ""},
        {"夢美", "Ｙｕｍｅｍｉ"},
        {"アゲハ", "Ａｇｅｈａ"},
        {"ユーリア", "Ｙｕｌｉａ"},
        {"サユリ", "Ｓａｙｕｒｉ"},
        {"杏子", "Ａｎｚｕ"},
        {"美弥香", "Ｍｉｙａｋａ"},
        {"唯", "Ｙｕｉ"},
        {"綾佳", "Ａｙａｋａ"},
        {"遥", "Ｈａｒｕｋａ"},
        {"レン", "Ｒｅｎ"},
        {"司会者", "Ｅｍｃｅｅ"},
    };
    return v;
}

}  // namespace exm::xtx
