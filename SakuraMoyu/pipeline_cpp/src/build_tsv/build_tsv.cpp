// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"

namespace fav::build_tsv {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

// str.replace(old, new) over the whole string.
std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

}  // namespace

std::string flatten(const std::string& text) {
    std::string s = replace_all(text, "\r\n", " ");
    s = replace_all(s, "\r", " ");
    s = replace_all(s, "\n", " ");
    return trim(s);
}

std::string escape_for_tsv(const std::string& text) {
    return replace_all(replace_all(text, "\\", "\\\\"), "\t", "\\t");
}

int run_build(const std::string& translated_file, const std::string& cache_file,
              const std::string& out_tsv) {
    log_info("Loading " + translated_file + "...");
    bj::value root = json_parse_file(translated_file);

    // Resume cache is the live source of truth -- translated_text.json may be
    // stale if the user ran step 2 multiple times without rebuilding the final
    // per-line JSON.  Fall back to cache for any line whose JSON entry has no
    // real translation.
    std::unordered_map<std::string, std::string> cache;
    if (fs::exists(fs::u8path(cache_file))) {
        bj::value cv = json_parse_file(cache_file);
        for (const auto& kv : cv.get_object())
            if (kv.value().is_string())
                cache.emplace(std::string(kv.key()), std::string(kv.value().get_string()));
        log_info("  Cache: " + comma(static_cast<long long>(cache.size())) + " entries");
    }

    // jp -> en, insertion-ordered, FIRST-wins (a later differing EN counts as
    // a conflict and is discarded).
    std::vector<std::pair<std::string, std::string>> order;
    std::unordered_map<std::string, std::size_t> index;
    long long conflicts = 0, cache_fills = 0;

    for (const auto& fkv : root.get_object()) {
        if (!fkv.value().is_object()) continue;
        const auto& fdata = fkv.value().get_object();
        auto lines_it = fdata.find("lines");
        if (lines_it == fdata.end() || !lines_it->value().is_array()) continue;
        for (const auto& lv : lines_it->value().get_array()) {
            const auto& ln = lv.get_object();
            std::string jp, en;
            if (auto* c = ln.if_contains("content"))
                if (c->is_string()) jp = std::string(c->get_string());
            if (auto* t = ln.if_contains("translated"))
                if (t->is_string()) en = std::string(t->get_string());
            if (jp.empty()) continue;
            const std::string jp_c = flatten(jp);
            if (jp_c.empty() || jp_c.front() == '_' || jp_c.find('/') != std::string::npos)
                continue;
            if (en.empty() || en == jp) {
                auto cit = cache.find(jp);
                if (cit == cache.end() || cit->second == jp)
                    cit = cache.find(jp_c);
                if (cit != cache.end() && cit->second != jp && cit->second != jp_c) {
                    en = cit->second;
                    ++cache_fills;
                }
            }
            if (en.empty() || jp == en) continue;
            const std::string en_c = flatten(en);
            if (en_c.empty() || en_c == jp_c) continue;
            auto it = index.find(jp_c);
            if (it == index.end()) {
                index.emplace(jp_c, order.size());
                order.emplace_back(jp_c, en_c);
            } else if (order[it->second].second != en_c) {
                ++conflicts;
            }
        }
    }

    fs::create_directories(fs::u8path(out_tsv).parent_path());
    std::string out;
    for (const auto& [jp, en] : order)
        out += escape_for_tsv(jp) + "\t" + escape_for_tsv(en) + "\n";
    write_file(out_tsv, out);

    log_info("Generated " + out_tsv);
    log_info("  Entries:    " + comma(static_cast<long long>(order.size())));
    log_info("  Cache-fill: " + comma(cache_fills) +
             "  (translated_text.json was stale, cache used)");
    log_info("  Conflicts:  " + comma(conflicts) +
             "  (same JP, different EN -- first kept)");
    log_info("  File size:  " +
             comma(static_cast<long long>(fs::file_size(fs::u8path(out_tsv)))) + " bytes");
    return 0;
}

}  // namespace fav::build_tsv
