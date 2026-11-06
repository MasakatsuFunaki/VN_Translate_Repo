// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned XTX name table.  The EN side is FULL-WIDTH romaji on purpose: the
// engine crashes on single-byte ASCII in an XTX display field (see xtx.h).
#include "xtx.h"

namespace mgi::xtx {

const std::vector<std::pair<std::string, std::string>>& charaname_translations() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"名前なし", ""},
        {"美弥香", "Ｍｉｙａｋａ"},
        {"レン", "Ｒｅｎ"},
        {"綾佳", "Ａｙａｋａ"},
        {"ユーリア", "Ｙｕｒｉａ"},
        {"シオ", "Ｓｈｉｏ"},
        {"夢美", "Ｙｕｍｅｍｉ"},
        {"風ノ宮", "Ｋａｚｅｎｏｍｉｙａ"},
        {"キョウ", "Ｋｙｏｕ"},
        {"狂", "Ｋｙｏｕ"},
    };
    return data;
}

}  // namespace mgi::xtx
