// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned reference data for the translation run: the character glossary
// and the story order.
#include "cache/glossary.h"

namespace exm::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"美弥香", "Miyaka"},
        {"綾佳", "Ayaka"},
        {"煉獄", "Rengoku"},
        {"史郎", "Shirou"},
        {"コレクター", "Collector"},
        {"アナウンサー", "Announcer"},
        {"男Ａ", "Man A"},
        {"男Ｂ", "Man B"},
        {"男Ｃ", "Man C"},
        {"男Ｄ", "Man D"},
        {"通行人Ａ", "Passerby A"},
        {"通行人Ｂ", "Passerby B"},
        {"通行人Ｃ", "Passerby C"},
        {"西", "Nishi"},
        {"光○", "Hikari-san"},
        {"史○", "Shi-san"},
        {"安○", "Yasu-san"},
        {"真○", "Ma-san"},
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
        "mushi.spt",
        "mushi0.spt",
        "mushi1.spt",
        "fmb_backscript.spt",
        "sys.spt",
    };
    return data;
}

}  // namespace exm::translate
