// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Character-name plate table for the .xtx config files.  Every EN value is
// FULL-WIDTH: the BLACKCyc engine crashes on single-byte ASCII in an XTX
// display field, so a new entry must be full-width too.
#include "xtx.h"

namespace exc::xtx {

const std::vector<std::pair<std::string, std::string>>& charaname_translations() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"少女", "Ｇｉｒｌ"},
        {"少年", "Ｂｏｙ"},
        {"男", "Ｍａｎ"},
        {"女", "Ｗｏｍａｎ"},
        {"男性", "Ｍａｎ"},
        {"女性", "Ｗｏｍａｎ"},
        {"老人", "Ｏｌｄ　ｍａｎ"},
        {"店員", "Ｃｌｅｒｋ"},
        {"蟲", "Ｂｕｇ"},
        {"声", "Ｖｏｉｃｅ"},
        {"？？？", "？？？"},
        {"ウエスト", "Ｗｅｓｔ"},
        {"Ｄｒ．Ｗｅｓｔ", "Ｄｒ．Ｗｅｓｔ"},
        {"名前なし", ""},
        {"夢美", "Ｙｕｍｅｍｉ"},
        {"アゲハ", "Ａｇｅｈａ"},
        {"ユーリア", "Ｙｕｒｉａ"},
        {"サユリ", "Ｓａｙｕｒｉ"},
        {"杏子", "Ｋｙｏｕｋｏ"},
        {"美弥香", "Ｍｉｙａｋａ"},
        {"唯", "Ｙｕｉ"},
        {"綾佳", "Ａｙａｋａ"},
        {"遥", "Ｈａｒｕｋａ"},
    };
    return data;
}

}  // namespace exc::xtx
