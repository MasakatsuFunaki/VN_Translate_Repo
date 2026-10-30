// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// GENERATED -- do not hand-edit. A wrong kanji silently breaks speaker lookups.
#include "cache/glossary.h"

namespace frat::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"大智", "Daichi"},
        {"紗英子", "Saeko"},
        {"美桜", "Mio"},
        {"愛", "Ai"},
        {"円夏", "Madoka"},
        {"芽生", "Mei"},
        {"心音", "Kokone"},
        {"瑛", "Akira"},
        {"園田", "Sonoda"},
        {"友佳", "Yuka"},
        {"舞子", "Maiko"},
        {"奈津美", "Natsumi"},
        {"理恵", "Rie"},
        {"千喜", "Chiki"},
        {"静子", "Shizuko"},
        {"達郎", "Tatsuro"},
        {"三嶋", "Mishima"},
        {"裕也", "Yuya"},
        {"真由美", "Mayumi"},
        {"優衣", "Yui"},
        {"大木", "Oki"},
        {"隼也", "Hayaya"},
        {"武志", "Takeshi"},
        {"高岡", "Takaoka"},
        {"槇原", "Makihara"},
        {"運転手", "Driver"},
        {"医師", "Doctor"},
        {"大智Ｈ", "Daichi (H)"},
        {"園田Ｈ", "Sonoda (H)"},
        {"中年男／園田Ｈ", "Middle-aged Man (Sonoda H)"},
        {"母／静子", "Mother (Shizuko)"},
        {"父／園田", "Father (Sonoda)"},
        {"友佳の母", "Yuka's Mother"},
        {"受付の女性／静子", "Receptionist (Shizuko)"},
        {"男性／園田", "Man (Sonoda)"},
        {"／愛", "Ai"},
        {"／静子", "Shizuko"},
        {"／心音", "Kokone"},
        {"／美桜", "Mio"},
        {"／芽生", "Mei"},
        {"／瑛", "Akira"},
        {"／円夏", "Madoka"},
        {"／紗英子", "Saeko"},
        {"／千喜", "Chiki"},
        {"／大木", "Oki"},
        {"／理恵", "Rie"},
        {"クラブ女性Ａ", "Club Woman A"},
        {"クラブ女性Ｂ", "Club Woman B"},
        {"クラブ女性Ｃ", "Club Woman C"},
        {"クラブ女性Ｄ", "Club Woman D"},
        {"クラブの少女Ｂ", "Club Girl B"},
        {"クラブの少女Ｃ", "Club Girl C"},
        {"クラブ男性", "Club Male"},
        {"クラブ男性Ａ", "Club Male A"},
        {"クラブ男性Ｂ", "Club Male B"},
        {"クラブ男性Ｃ", "Club Male C"},
        {"男Ａ", "Man A"},
        {"男Ｂ", "Man B"},
        {"男Ｃ", "Man C"},
        {"男Ｄ", "Man D"},
        {"男１", "Man 1"},
        {"男２", "Man 2"},
        {"男３", "Man 3"},
        {"男", "Man"},
        {"男性", "Man"},
        {"男優", "Male Actor"},
        {"おじさん", "Mister"},
        {"ホームレス", "Homeless"},
        {"ホームレスＡ", "Homeless A"},
        {"ホームレスＢ", "Homeless B"},
        {"ホームレスＣ", "Homeless C"},
        {"ホームレスＤ", "Homeless D"},
        {"浮浪者", "Vagrant"},
        {"父", "Father"},
        {"父親", "Father"},
        {"母親", "Mother"},
        {"先生", "Teacher"},
        {"監督", "Director"},
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
        "bn.ypf",
    };
    return data;
}

}  // namespace frat::translate
