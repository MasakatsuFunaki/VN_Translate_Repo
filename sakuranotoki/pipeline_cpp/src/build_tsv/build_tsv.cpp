// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv.h"

#include "common/util.h"
#include "translate/glossary.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/json.hpp>

namespace snt::build_tsv {

namespace bj = boost::json;
namespace fs = std::filesystem;

std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        default: out += c;
        }
    }
    return out;
}

namespace {

// U+3000 IDEOGRAPHIC SPACE, the paragraph indent the scripts are written with.
constexpr std::string_view kIdeographicSpace = "　";
constexpr std::size_t kMinSharedPlateKeys = 2;
constexpr std::size_t kShippedReserveFactor = 2;
constexpr std::size_t kFileSizeBufChars = 64;
constexpr double kBytesPerKilobyte = 1024.0;
constexpr double kBytesPerMegabyte = kBytesPerKilobyte * kBytesPerKilobyte;
constexpr int kExitOk = 0;

std::size_t leading_indent_bytes(const std::string& s) {
    std::size_t n = 0;
    while (s.compare(n, kIdeographicSpace.size(), kIdeographicSpace.data()) == 0) {
        n += kIdeographicSpace.size();
    }
    return n;
}

bool is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// Replace `from` with `to` at word boundaries only (ASCII letter or digit).
std::string replace_whole_term(const std::string& text, const std::string& from,
                               const std::string& to) {
    if (from.empty() || from == to) {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        // Advance past text that already reads the new spelling, so a
        // replacement containing the old spelling cannot grow without end.
        if (!to.empty() && text.compare(i, to.size(), to) == 0) {
            out += to;
            i += to.size();
            continue;
        }
        if (text.compare(i, from.size(), from) == 0) {
            const std::size_t end = i + from.size();
            const bool free_left =
                i == 0 || !is_word_byte(static_cast<unsigned char>(text[i - 1]));
            const bool free_right =
                end == text.size() ||
                !is_word_byte(static_cast<unsigned char>(text[end]));
            if (free_left && free_right) {
                out += to;
                i = end;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

const std::unordered_map<std::string, std::string>& pinned_terms() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& [jp, en] : translate::terminology_translations()) {
            m.emplace(jp, en);
        }
        return m;
    }();
    return map;
}

// Rewrite superseded spellings, only where the Japanese carries the term.
void repin(std::string& out, const std::string& jp,
           const std::vector<std::pair<std::string, std::string>>& superseded,
           const std::unordered_map<std::string, std::string>& pins) {
    for (const auto& [term, old_en] : superseded) {
        if (jp.find(term) == std::string::npos) {
            continue;
        }
        const auto pin = pins.find(term);
        if (pin == pins.end() || pin->second == old_en) {
            continue;
        }
        out = replace_whole_term(out, old_en, pin->second);
    }
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

}  // namespace

std::vector<std::string> name_plates_sharing_english(
    const std::vector<std::pair<std::string, std::string>>& name_rows) {
    std::map<std::string, std::vector<std::string>> keys_of;
    for (const auto& [jp, en] : name_rows) {
        keys_of[en].push_back(jp);
    }
    std::vector<std::string> out;
    for (const auto& [en, keys] : keys_of) {
        if (keys.size() >= kMinSharedPlateKeys) {
            out.push_back(join(keys, " / ") + " -> " + en);
        }
    }
    return out;
}

std::string apply_pinned_terms(const std::string& jp, const std::string& en) {
    std::string out = en;
    repin(out, jp, translate::superseded_renderings(), pinned_terms());
    repin(out, jp, translate::superseded_name_renderings(),
          translate::name_translations());
    return out;
}

std::string normalize_leading_space(const std::string& jp, const std::string& en) {
    const std::size_t want = leading_indent_bytes(jp);
    std::size_t have = leading_indent_bytes(en);
    while (have < en.size() && (en[have] == ' ' || en[have] == '\t')) {
        ++have;
    }
    return jp.substr(0, want) + en.substr(have);
}

std::string run_together(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\n' && value[i] != '\r') {
            out += value[i];
            continue;
        }
        if (value.find_first_not_of("\r\n", i) == std::string::npos) {
            out.append(value, i, std::string::npos);
            break;
        }
        while (i + 1 < value.size() &&
               (value[i + 1] == '\n' || value[i + 1] == '\r')) {
            ++i;
        }
        if (!out.empty() && out.back() != ' ') {
            out += ' ';
        }
    }
    return out;
}

int run_build(const std::string& translated_file, const std::string& cache_file,
              const std::string& out_tsv, bool include_untranslated,
              bool flow_breaks) {
    bj::value root = json_parse_file(translated_file);

    // Resume cache is the live source of truth -- translated_text.json may be
    // stale if the user ran 02_translate multiple times without rebuilding.
    std::unordered_map<std::string, std::string> cache;
    if (fs::exists(fs::u8path(cache_file))) {
        bj::value cv = json_parse_file(cache_file);
        for (const auto& kv : cv.get_object()) {
            if (kv.value().is_string()) {
                cache.emplace(
                    std::string(kv.key()),
                    std::string(kv.value().get_string()));
            }
        }
    }

    std::vector<std::pair<std::string, std::string>> order;
    std::unordered_map<std::string, std::size_t> index;
    std::size_t conflicts = 0;
    std::size_t untranslated = 0;
    std::size_t total_seen = 0;
    std::size_t cache_fills = 0;

    std::vector<std::string> name_keys;
    std::unordered_set<std::string> name_seen;

    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto& script = kv.value().get_object();
        const auto it = script.find("strings");
        if (it == script.end() || !it->value().is_array()) {
            continue;
        }
        for (const auto& ev : it->value().get_array()) {
            const auto& entry = ev.get_object();
            std::string jp(entry.at("text").get_string());
            std::string en = jp;
            if (const auto* t = entry.if_contains("translated");
                t != nullptr && t->is_string()) {
                en = std::string(t->get_string());
            }
            if (const auto* t = entry.if_contains("type");
                t != nullptr && t->is_string() && t->get_string() == "name") {
                if (name_seen.insert(jp).second) {
                    name_keys.push_back(jp);
                }
            }
            ++total_seen;
            if (en.empty() || en == jp) {
                const auto cit = cache.find(jp);
                if (cit != cache.end() && cit->second != jp) {
                    en = cit->second;
                    ++cache_fills;
                }
            }
            if (!include_untranslated && en == jp) {
                ++untranslated;
                continue;
            }
            const auto idx_it = index.find(jp);
            if (idx_it != index.end()) {
                if (order[idx_it->second].second != en) {
                    ++conflicts;
                }
                order[idx_it->second].second = en;
                continue;
            }
            index.emplace(jp, order.size());
            order.emplace_back(jp, en);
        }
    }

    const std::size_t rows_from_scripts = order.size();

    std::size_t name_disagreements = 0;
    for (const auto& [jp, en] : translate::name_translations_ordered()) {
        if (name_seen.insert(jp).second) {
            name_keys.push_back(jp);
        }
        const auto it = index.find(jp);
        if (it != index.end()) {
            if (apply_pinned_terms(jp, order[it->second].second) != en) {
                ++name_disagreements;
                log_info("  Name row disagrees with the glossary: " + jp + " -> " +
                         order[it->second].second + "   (glossary: " + en + ")");
            }
            continue;
        }
        index.emplace(jp, order.size());
        order.emplace_back(jp, en);
    }

    std::string out;
    std::size_t repinned = 0;
    std::size_t flowed = 0;
    std::unordered_map<std::string, std::string> shipped;
    shipped.reserve(order.size() * kShippedReserveFactor);
    for (const auto& [jp, en] : order) {
        std::string value = apply_pinned_terms(jp, en);
        if (value != en) {
            ++repinned;
        }
        value = normalize_leading_space(jp, value);
        if (flow_breaks) {
            const std::string ran_together = run_together(value);
            if (ran_together != value) {
                ++flowed;
            }
            value = ran_together;
        }
        out += esc(jp) + "\t" + esc(value) + "\n";
        shipped.emplace(jp, value);
    }
    write_file(out_tsv, out);

    std::vector<std::pair<std::string, std::string>> plates;
    plates.reserve(name_keys.size());
    for (const std::string& jp : name_keys) {
        const auto it = shipped.find(jp);
        if (it != shipped.end()) {
            plates.emplace_back(jp, it->second);
        }
    }
    const std::vector<std::string> shared_plates =
        name_plates_sharing_english(plates);

    log_info("Total entries seen:      " + std::to_string(total_seen));
    log_info("Unique JP strings:       " +
             std::to_string(rows_from_scripts + untranslated));
    log_info("Translated entries kept: " + std::to_string(rows_from_scripts));
    log_info("Untranslated (skipped):  " + std::to_string(untranslated));
    log_info("Cache-fill (json stale): " + std::to_string(cache_fills) +
             "  (cache supplied translation)");
    log_info("Glossary name rows:      " +
             std::to_string(order.size() - rows_from_scripts));
    log_info("Rows written:            " + std::to_string(order.size()));
    if (repinned > 0) {
        log_info("Rows re-pinned:          " + std::to_string(repinned) +
                 "  (superseded terminology and names rewritten)");
    }
    if (flowed > 0) {
        log_info("Rows run together:       " + std::to_string(flowed) +
                 "  (a break inside the English became a space)");
    }
    if (name_disagreements > 0) {
        log_info("Name rows disagreeing:   " + std::to_string(name_disagreements) +
                 "  (document row kept -- see the lines above)");
    }
    log_info("Names sharing one English: " + std::to_string(shared_plates.size()) +
             (shared_plates.empty()
                  ? ""
                  : "  (two speakers the player cannot tell apart)"));
    for (const std::string& shared : shared_plates) {
        log_info("  " + shared);
    }
    if (conflicts > 0) {
        log_info("Conflicting duplicates:  " + std::to_string(conflicts) +
                 "  (last-wins)");
    }
    char sz[kFileSizeBufChars];
    std::snprintf(
        sz, sizeof(sz), "%.1f MB",
        static_cast<double>(fs::file_size(fs::u8path(out_tsv))) / kBytesPerMegabyte);
    log_info("Wrote " + out_tsv + " (" + std::string(sz) + ")");
    return kExitOk;
}

}  // namespace snt::build_tsv
