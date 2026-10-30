// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned reference data: the character glossary and the story order.
// Do not hand-edit -- the tables are asserted character-for-character by the
// unit tests, and every entry is a name the player sees on screen.
#include "cache/glossary.h"

namespace crc::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"太一", "Taichi"},
        {"見里", "Misato"},
        {"美希", "Miki"},
        {"霧", "Kiri"},
        {"冬子", "Touko"},
        {"友貴", "Tomoki"},
        {"桜庭", "Sakuraba"},
        {"曜子", "Youko"},
        {"七香", "Nanaka"},
        {"遊紗", "Yusa"},
        {"新川", "Shinkawa"},
        {"福原", "Fukuhara"},
        {"みゆき", "Miyuki"},
        {"ミミ", "Mimi"},
        {"美々美", "Mimimi"},
        {"ママン", "Maman"},
        {"ポコ", "Poko"},
        {"アキラ", "Akira"},
        {"ミチル", "Michiru"},
        {"重蔵", "Juuzou"},
        {"サメ", "Shark"},
        {"？？？", "???"},
        {"少女", "Girl"},
        {"少年", "Boy"},
        {"女", "Woman"},
        {"声", "Voice"},
        {"老医師", "Old Doctor"},
        {"おばちゃん", "Auntie"},
        {"獣医", "Veterinarian"},
        {"部長", "Club President"},
        {"アナウンス", "Announcement"},
        {"ナレーション", "Narration"},
        {"一同", "Everyone"},
        {"みんな", "Everyone"},
        {"二人", "Both"},
        {"三人", "All Three"},
        {"３人", "All Three"},
        {"四人", "All Four"},
        {"老カラデ家", "Old Karade Family"},
        {"女生徒たち", "Female Students"},
        {"女性陣", "The Girls"},
        {"男子生徒Ａ", "Male Student A"},
        {"男子生徒Ｂ", "Male Student B"},
        {"男子生徒Ｃ", "Male Student C"},
        {"人体模型", "Mannequin"},
        {"ウサミミ", "Bunny-Ears"},
        {"トモキチ", "Tomokichi"},
        {"ミサコ", "Misako"},
        {"速杉", "Hayasugi"},
        {"デス友貴", "Dead Tomoki"},
        {"デス桜庭", "Dead Sakuraba"},
        {"デス見里", "Dead Misato"},
        {"デス霧", "Dead Kiri"},
        {"桜庭・友貴", "Sakuraba & Tomoki"},
        {"太一の声", "Taichi's Voice"},
        {"友貴の死体", "Tomoki's Corpse"},
        {"太一（妄想）", "Taichi (Delusion)"},
        {"（太一）", "(Taichi)"},
        {"（冬子）", "(Touko)"},
        {"美希・見里", "Miki & Misato"},
        {"見里・霧", "Misato & Kiri"},
        {"見里・冬子", "Misato & Touko"},
        {"霧・冬子", "Kiri & Touko"},
        {"友貴・見里", "Tomoki & Misato"},
        {"友貴＆太一", "Tomoki & Taichi"},
        {"太一・冬子", "Taichi & Touko"},
        {"太一・桜庭", "Taichi & Sakuraba"},
        {"太一・美希", "Taichi & Miki"},
        {"霧・美希", "Kiri & Miki"},
        {"美希・霧", "Miki & Kiri"},
        {"霧＆美希", "Kiri & Miki"},
        {"霧・太一", "Kiri & Taichi"},
        {"美希・老医師", "Miki & Old Doctor"},
        {"＊", "*"},
        {"＊＊", "**"},
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
        "sn.bin",
    };
    return data;
}

}  // namespace crc::translate
