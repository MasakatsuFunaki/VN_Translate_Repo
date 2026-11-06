// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned translation data: the name table and story order.
// Both are load-bearing for output consistency -- see glossary.h.
#include "glossary.h"

namespace mgi::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"美弥香", "Miyaka"},
        {"レン", "Ren"},
        {"綾佳", "Ayaka"},
        {"ユーリア", "Yuria"},
        {"シオ", "Shio"},
        {"夢美", "Yumemi"},
        {"風ノ宮", "Kazenomiya"},
        {"キョウ", "Kyou"},
        {"狂", "Kyou"},
        {"？？？", "???"},
        {"店員", "Clerk"},
        {"女性客", "Female Customer"},
        {"牢獄の部下Ａ", "Prison Subordinate A"},
        {"牢獄の部下Ｂ", "Prison Subordinate B"},
        {"牢獄の部下Ｃ", "Prison Subordinate C"},
        {"牢獄の部下Ｄ", "Prison Subordinate D"},
        {"牢獄の部下Ｅ", "Prison Subordinate E"},
        {"見張りの蟲使い", "Lookout Bug User"},
        {"老いた蟲使いＡ", "Old Bug User A"},
        {"老いた蟲使いＢ", "Old Bug User B"},
        {"老いた蟲使いＣ", "Old Bug User C"},
        {"若き蟲使い", "Young Bug User"},
        {"蟲使いの追っ手", "Bug User Pursuer"},
        {"蟲使いＡ", "Bug User A"},
        {"蟲使いＢ", "Bug User B"},
        {"蟲使いＣ", "Bug User C"},
        {"蟲使いＤ", "Bug User D"},
        {"蟲使いＥ", "Bug User E"},
        {"蟲使いＦ", "Bug User F"},
        {"蟲使いＧ", "Bug User G"},
        {"蟲使いＨ", "Bug User H"},
        {"見張りの蟲使いＡ", "Lookout Bug User A"},
        {"見張りの蟲使いＢ", "Lookout Bug User B"},
        {"見張りの蟲使いＣ", "Lookout Bug User C"},
        {"美弥香・ユーリア", "Miyaka & Yuria"},
        {"『蟲狂編』", "Mushigurui Chapter"},
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
        "s100.spt",
        "s200.spt",
        "s300.spt",
        "s400.spt",
        "etc.spt",
        "BCmkri_backscript.spt",
        "sys.spt",
    };
    return data;
}

}  // namespace mgi::translate
