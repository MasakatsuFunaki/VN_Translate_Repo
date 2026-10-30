// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Character glossary.  A JP key here is matched against raw
// NAME-line content byte for byte, so an edited or reordered entry changes the
// speaker tags and the seeding order of the cache -- keep it
// in sync with the NAME lines in extracted_text.json (speaker_gate checks both
// directions).
#include "glossary.h"

namespace ama::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"ちとせ", "Chitose"},
        {"ゆうひ", "Yuuhi"},
        {"玲", "Rei"},
        {"結灯", "Yuuhi"},
        {"淵上", "Fuchinoe"},
        {"寶泉路", "Housenji"},
        {"ジャック", "Jack"},
        {"ゾンビ", "Zombie"},
        {"マスター", "Master"},
        {"母さん", "Mom"},
        {"猫", "Cat"},
        {"少女", "Girl"},
        {"少年", "Boy"},
        {"男の子", "Boy"},
        {"男の子の母", "Boy's Mother"},
        {"男の子の母親", "Boy's Mother"},
        {"男の子の父親", "Boy's Father"},
        {"迷子の女の子", "Lost Girl"},
        {"担任", "Homeroom Teacher"},
        {"放送", "Broadcast"},
        {"教員", "Teacher"},
        {"国語教師", "Japanese Teacher"},
        {"数学教師", "Math Teacher"},
        {"英語教師", "English Teacher"},
        {"歴史教師", "History Teacher"},
        {"社会教師", "Social Studies Teacher"},
        {"社会科教師", "Social Studies Teacher"},
        {"女将さん", "Hostess"},
        {"旦那さん", "Husband"},
        {"店員さん", "Clerk"},
        {"プリント機", "Printer"},
        {"副部長", "Vice President"},
        {"吸血鬼", "Vampire"},
        {"おじさん", "Uncle"},
        {"おばさん", "Aunt"},
        {"おばあさん", "Grandmother"},
        {"お客さん", "Customer"},
        {"誰かの声", "Someone's Voice"},
        {"？？？", "???"},
        {"男性", "Man"},
        {"男性客", "Male Customer"},
        {"男性俳優", "Male Actor"},
        {"女性客", "Female Customer"},
        {"女性俳優", "Female Actress"},
        {"男子学生", "Male Student"},
        {"女子学生", "Female Student"},
        {"学生たち", "Students"},
        {"女子学生たち", "Female Students"},
        {"部員たち", "Club Members"},
        {"三人", "Three of Them"},
        {"男子学生Ａ", "Male Student A"},
        {"男子学生Ｂ", "Male Student B"},
        {"男子学生Ｃ", "Male Student C"},
        {"男子下級生Ａ", "Male Underclassman A"},
        {"女子学生Ａ", "Female Student A"},
        {"女子学生Ｂ", "Female Student B"},
        {"女子学生Ｃ", "Female Student C"},
        {"女子学生Ｄ", "Female Student D"},
        {"女子先輩Ａ", "Female Senior A"},
        {"女子先輩Ｂ", "Female Senior B"},
        {"女子下級生Ａ", "Female Underclassman A"},
        {"女子下級生Ｂ", "Female Underclassman B"},
        {"女子下級生Ｃ", "Female Underclassman C"},
        {"女子下級生Ｄ", "Female Underclassman D"},
        {"女子部員Ａ", "Female Club Member A"},
        {"女子部員Ｂ", "Female Club Member B"},
        {"女子部員Ｃ", "Female Club Member C"},
        {"女子部員たち", "Female Club Members"},
        {"通行人Ａ", "Passerby A"},
        {"通行人Ｂ", "Passerby B"},
        {"通行人Ｃ", "Passerby C"},
        {"通行人Ｄ", "Passerby D"},
        {"ちとせ＆$str20", "Chitose & $str20"},
        {"ちとせ＠二人", "Chitose (Both)"},
        {"ちとせ＠？？？", "Chitose (???)"},
        {"玲の父", "Rei's Father"},
        {"玲＠おばけ", "Rei (Ghost)"},
        {"玲＠玲の声", "Rei (Rei's Voice)"},
        {"玲＠静かな声", "Rei (Quiet Voice)"},
        {"玲＠？？？", "Rei (???)"},
        {"結灯の父", "Yuuhi's Father"},
        {"結灯の父＠？？？", "Yuuhi's Father (???)"},
        {"結灯＠少女", "Yuuhi (Girl)"},
        {"結灯＠店員さん", "Yuuhi (Clerk)"},
        {"結灯＠店員さんの声", "Yuuhi (Clerk's Voice)"},
        {"結灯＠？？？", "Yuuhi (???)"},
        {"寶泉路＠？？？", "Housenji (???)"},
        {"淵上＠？？？", "Fuchinoe (???)"},
        {"副部長＠茉莉", "Vice President (Mari)"},
        {"女将さん＠おばあさん", "Hostess (Grandmother)"},
        {"こはる＠店員さん", "Koharu (Clerk)"},
        {"こはる＠観光客さんＢ", "Koharu (Tourist B)"},
        {"奏＠観光客さんＡ", "Kanade (Tourist A)"},
        {"奏＠観光客さんＢ", "Kanade (Tourist B)"},
        {"涙香＠観光客さんＡ", "Ruika (Tourist A)"},
        {"穂波＠お客さんＢ", "Honami (Customer B)"},
        {"穂波＠観光客さんＡ", "Honami (Tourist A)"},
        {"紗雪＠巫女さんＡ", "Sayuki (Shrine Maiden A)"},
        {"紗雪＠観光客さんＢ", "Sayuki (Tourist B)"},
        {"聖＠お客さんＡ", "Mizuki (Customer A)"},
        {"聖＠観光客さんＢ", "Mizuki (Tourist B)"},
        {"雪静＠巫女さんＢ", "Yuzuka (Shrine Maiden B)"},
        {"Chit", "Chit"},
        {"Hona", "Hona"},
        {"Kana", "Kana"},
        {"Koha", "Koha"},
        {"Mizu", "Mizu"},
        {"ReiX", "ReiX"},
        {"Ruik", "Ruik"},
        {"Sayu", "Sayu"},
        {"Yuuh", "Yuuh"},
        {"Yuzu", "Yuzu"},
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

}  // namespace ama::translate
