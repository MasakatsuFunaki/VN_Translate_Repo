// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv.h"

#include <filesystem>
#include <map>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"
#include "translate/glossary.h"

namespace crc::build_tsv {

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

// Insertion-ordered jp -> en.  Insertion order is load-bearing: it IS the
// TSV's row order.  A repeat JP key updates the value in place without moving,
// so last EN wins and first position wins.
class OrderedMap {
public:
    bool contains(const std::string& k) const { return index_.count(k) != 0; }
    void set(const std::string& k, const std::string& v) {
        auto it = index_.find(k);
        if (it != index_.end()) order_[it->second].second = v;
        else {
            index_.emplace(k, order_.size());
            order_.emplace_back(k, v);
        }
    }
    const std::string& at(const std::string& k) const { return order_[index_.at(k)].second; }
    std::size_t size() const { return order_.size(); }
    const std::vector<std::pair<std::string, std::string>>& items() const { return order_; }

private:
    std::vector<std::pair<std::string, std::string>> order_;
    std::map<std::string, std::size_t> index_;
};

}  // namespace

std::string esc(const std::string& s) {
    std::string out = replace_all(s, "\\", "\\\\");
    out = replace_all(out, "\t", "\\t");
    out = replace_all(out, "\r", "\\r");
    out = replace_all(out, "\n", "\\n");
    return out;
}

int run_build(const std::string& translated_file, const std::string& out_tsv,
              const std::string& game_tsv) {
    if (!fs::exists(fs::u8path(translated_file))) {
        log_info("ERROR: " + translated_file + " not found");
        log_info("Run 02_translate first.");
        return 1;
    }

    log_info("Loading " + translated_file + "...");
    bj::value data = json_parse_file(translated_file);

    const bj::object* sn = nullptr;
    if (data.is_object())
        if (auto* v = data.get_object().if_contains("sn.bin"))
            if (v->is_object() && v->get_object().if_contains("strings")) sn = &v->get_object();
    if (!sn) {
        log_info("ERROR: unexpected translated_text.json schema (no sn.bin.strings)");
        return 1;
    }

    OrderedMap seen;
    long long untranslated = 0, skipped_pairs = 0;
    for (const auto& sv : sn->at("strings").get_array()) {
        const auto& s = sv.get_object();
        const std::string jp = json_str(s, "text");
        const std::string en = json_str(s, "translated");
        if (jp.empty()) continue;
        // A passthrough row would make the runtime hook substitute jp -> jp.
        if (en == jp || en.empty()) {
            ++untranslated;
            continue;
        }
        if (seen.contains(jp) && seen.at(jp) != en) ++skipped_pairs;
        seen.set(jp, en);
    }

    // Speaker names live in their own null-terminated slots in sn.bin (opcode
    // 47 0D 00) and never appear in a dialogue `text` field, so they have to be
    // added explicitly.  The DLL anchors on \0...\0, so 太一 -> Taichi only
    // matches the name plate, never the substring inside 「……太一」.
    long long names_added = 0;
    for (const auto& [jp, en] : translate::name_translations_ordered()) {
        if (jp.empty() || en.empty() || jp == en) continue;
        if (seen.contains(jp)) continue;  // dialogue translation already covers it
        seen.set(jp, en);
        ++names_added;
    }

    const fs::path out_path = fs::u8path(out_tsv);
    if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path());

    std::string tsv;
    for (const auto& [jp, en] : seen.items()) tsv += esc(jp) + "\t" + esc(en) + "\n";
    // Bare LF: the one text file here that is not CRLF (see build_tsv.h).
    write_file(out_tsv, tsv);

    log_info("Wrote " + out_tsv + " (" +
             comma(static_cast<long long>(fs::file_size(out_path))) + " bytes)");
    log_info("  Translated entries:  " + comma(static_cast<long long>(seen.size())));
    log_info("  Speaker names added: " + comma(names_added) + "  (from NAME_TRANSLATIONS)");
    log_info("  Untranslated (skip): " + comma(untranslated));
    log_info("  Conflicting pairs:   " + comma(skipped_pairs) + "  (last EN kept)");

    const fs::path game_path = fs::u8path(game_tsv);
    if (!fs::is_directory(game_path.parent_path())) {
        log_info("WARN: game dir not found, skipping deploy: " + game_tsv);
    } else {
        fs::copy_file(out_path, game_path, fs::copy_options::overwrite_existing);
        log_info("Deployed -> " + game_tsv);
    }
    return 0;
}

}  // namespace crc::build_tsv
