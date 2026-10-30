// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Character glossary and story order.  Editing a name here changes the
// translation of every line that speaker owns AND the key order of the
// translation cache, so an edit means a cache rebuild -- add new names at the
// END of the table unless you intend that.
#include "cache/glossary.h"

namespace exc::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"少女", "Girl"},
        {"少年", "Boy"},
        {"？？？", "???"},
        {"夢美", "Yumemi"},
        {"アゲハ", "Ageha"},
        {"ユーリア", "Yulia"},
        {"サユリ", "Sayuri"},
        {"杏子", "Anzu"},
        {"美弥香", "Miyaka"},
        {"唯", "Yui"},
        {"綾佳", "Ayaka"},
        {"遥", "Haruka"},
        {"煉悟", "Rengo"},
        {"西", "Nishi"},
        {"優斗", "Yuuto"},
        {"史郎", "Shirou"},
        {"レン", "Ren"},
        {"煉獄", "Rengoku"},
        {"和泉", "Izumi"},
        {"和泉万夜", "Izumi Manya"},
        {"メタ", "Meta"},
        {"ばぐねこ", "Bagneko"},
        {"優斗の友人", "Yuuto's Friend"},
        {"男の声", "Man's Voice"},
        {"客", "Customer"},
        {"三人", "Three of Them"},
        {"列席者達", "Attendees"},
        {"客Ａ", "Customer A"},
        {"客Ｂ", "Customer B"},
        {"客Ｃ", "Customer C"},
        {"客Ｄ", "Customer D"},
        {"客Ｅ", "Customer E"},
        {"客Ｆ", "Customer F"},
        {"客Ｇ", "Customer G"},
        {"客Ｈ", "Customer H"},
        {"男Ａ", "Man A"},
        {"男Ｂ", "Man B"},
        {"男Ｃ", "Man C"},
        {"男Ｄ", "Man D"},
        {"男Ｅ", "Man E"},
        {"男Ｆ", "Man F"},
        {"男Ｇ", "Man G"},
        {"男Ｈ", "Man H"},
        {"男Ｉ", "Man I"},
        {"男Ｊ", "Man J"},
        {"男Ｋ", "Man K"},
        {"男Ｌ", "Man L"},
        {"列席者Ａ", "Attendee A"},
        {"列席者Ｂ", "Attendee B"},
        {"列席者Ｃ", "Attendee C"},
        {"列席者Ｄ", "Attendee D"},
        {"列席者Ｅ", "Attendee E"},
        {"列席者Ｆ", "Attendee F"},
        {"列席者Ｇ", "Attendee G"},
        {"列席者Ｈ", "Attendee H"},
        {"列席者Ｉ", "Attendee I"},
        {"列席者Ｊ", "Attendee J"},
        {"列席者Ｋ", "Attendee K"},
        {"列席者Ｌ", "Attendee L"},
        {"列席者Ｍ", "Attendee M"},
        {"列席者Ｎ", "Attendee N"},
        {"列席者Ｏ", "Attendee O"},
        {"列席者Ｐ", "Attendee P"},
        {"夢美・アゲハ", "Yumemi & Ageha"},
        {"夢美・杏子", "Yumemi & Anzu"},
        {"夢美・唯", "Yumemi & Yui"},
        {"夢美・綾佳", "Yumemi & Ayaka"},
        {"夢美・遥", "Yumemi & Haruka"},
        {"美弥香・唯", "Miyaka & Yui"},
        {"綾佳・遥", "Ayaka & Haruka"},
    };
    return data;
}

const std::unordered_map<std::string, std::string>& name_translations() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& [jp, en] : name_translations_ordered()) m.emplace(jp, en);
        return m;
    }();
    return map;
}

const std::vector<std::string>& story_order() {
    static const std::vector<std::string> data = {
        "01.spt",
        "02.spt",
        "03.spt",
        "04.spt",
        "05.spt",
        "06.spt",
        "062.spt",
        "07.spt",
        "08.spt",
        "backscript.spt",
        "omake.spt",
        "_exgame.spt",
        "_meta.spt",
        "_exban.spt",
        "_staff.spt",
        "sys.spt",
    };
    return data;
}

}  // namespace exc::translate
