// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// GENERATED -- do not hand-edit. A wrong kanji silently breaks clean_en().
#include "build_tsv/name_fixups.h"

namespace frat::build_tsv {

const std::vector<std::pair<std::string, std::string>>& NameFixups() {
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
        {"大智Ｈ", "Daichi (H)"},
        {"園田Ｈ", "Sonoda (H)"},
        {"母／静子", "Mother (Shizuko)"},
        {"父／園田", "Father (Sonoda)"},
        {"友佳の母", "Yuka's Mother"},
        {"下級生の戸田心音", "Underclassman Kokone Toda"},
    };
    return data;
}

}  // namespace frat::build_tsv
