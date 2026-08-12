// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv.h"

#include <filesystem>
#include <set>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"
#include "translate/glossary.h"

namespace shin::build_tsv {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

std::string json_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key))
        if (v->is_string()) return std::string(v->get_string());
    return {};
}

}  // namespace

std::string escape_for_tsv(const std::string& text) {
    std::string s = replace_all(text, "\t", "{TAB}");
    s = replace_all(s, "\r", "{CR}");
    s = replace_all(s, "\n", "{LF}");
    return s;
}

int run_build(const std::string& translated_file, const std::string& out_tsv) {
    log_info("Loading translated text...");
    bj::value root = json_parse_file(translated_file);

    std::vector<std::pair<std::string, std::string>> entries;
    long long skipped = 0;

    const auto& names = translate::name_translations();

    for (const auto& fkv : root.get_object()) {
        if (!fkv.value().is_object()) continue;
        const auto* strings = fkv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            const std::string translated = json_str(s, "translated");
            if (translated.empty()) continue;

            const std::string stype = json_str(s, "type");
            const std::string speaker = json_str(s, "speaker");
            const std::string jp_dialogue = json_str(s, "text");
            const std::string& en_dialogue = translated;

            std::string jp_raw, en_raw;
            if (stype == "choice") {
                jp_raw = jp_dialogue;
                en_raw = en_dialogue;
            } else if (stype == "dialogue" && !speaker.empty()) {
                auto it = names.find(speaker);
                const std::string en_name = it == names.end() ? speaker : it->second;
                jp_raw = speaker + "\\n" + jp_dialogue;
                en_raw = en_name + "\\n" + en_dialogue;
            } else {
                // narration (and dialogue whose speaker never resolved)
                jp_raw = "\\n" + jp_dialogue;
                en_raw = "\\n" + en_dialogue;
            }

            if (jp_raw == en_raw) {
                ++skipped;
                continue;
            }
            entries.emplace_back(std::move(jp_raw), std::move(en_raw));
        }
    }

    // Same JP text appears in many scripts; keep the FIRST English for each and
    // preserve the order the keys were first seen in.
    std::vector<std::pair<std::string, std::string>> unique;
    unique.reserve(entries.size());
    std::set<std::string> seen;
    for (auto& e : entries)
        if (seen.insert(e.first).second) unique.push_back(std::move(e));

    std::string rows;
    for (const auto& [jp, en] : unique)
        rows += escape_for_tsv(jp) + "\t" + escape_for_tsv(en) + "\n";

    fs::create_directories(fs::u8path(out_tsv).parent_path());
    write_file_text_bom(out_tsv, rows);

    log_info("Generated " + out_tsv);
    log_info("  Entries: " + std::to_string(unique.size()));
    log_info("  Skipped (identical): " + std::to_string(skipped));
    log_info("  File size: " +
             comma(static_cast<long long>(fs::file_size(fs::u8path(out_tsv)))) + " bytes");
    return 0;
}

}  // namespace shin::build_tsv
