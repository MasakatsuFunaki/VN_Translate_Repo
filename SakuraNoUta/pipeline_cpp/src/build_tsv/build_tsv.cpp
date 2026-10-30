// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "box_budget.h"
#include "common/coverage.h"
#include "common/util.h"
#include "cache/glossary.h"
#include "translator_logic.h"

namespace snu::build_tsv {

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

// U+3000 IDEOGRAPHIC SPACE, the engine's paragraph indent.
constexpr char IDEOGRAPHIC_SPACE[] = "\xE3\x80\x80";
constexpr std::size_t IDEOGRAPHIC_SPACE_LEN = 3;

std::size_t leading_indent_bytes(const std::string& s) {
    std::size_t n = 0;
    while (s.compare(n, IDEOGRAPHIC_SPACE_LEN, IDEOGRAPHIC_SPACE) == 0)
        n += IDEOGRAPHIC_SPACE_LEN;
    return n;
}

bool is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Replace `from` with `to` only at word boundaries (ASCII letter/digit).
std::string replace_whole_term(const std::string& text, const std::string& from,
                               const std::string& to) {
    if (from.empty() || from == to) return text;
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        // Skip text already reading the new spelling (avoids runaway growth
        // when the new spelling contains the old one).
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
        for (const auto& [jp, en] : translate::terminology_translations())
            m.emplace(jp, en);
        return m;
    }();
    return map;
}

// Current spelling of every character name, across both name tables.
const std::unordered_map<std::string, std::string>& pinned_names() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto* table : {&translate::name_translations_ordered(),
                                  &translate::extra_name_translations()})
            for (const auto& [jp, en] : *table)
                m.emplace(jp, en);
        return m;
    }();
    return map;
}

// Rewrite superseded spellings, only where the JP source carries the term.
void repin(std::string& out, const std::string& jp,
           const std::vector<std::pair<std::string, std::string>>& superseded,
           const std::unordered_map<std::string, std::string>& pins) {
    for (const auto& [term, old_en] : superseded) {
        if (jp.find(term) == std::string::npos) continue;
        auto pin = pins.find(term);
        if (pin == pins.end() || pin->second == old_en) continue;
        out = replace_whole_term(out, old_en, pin->second);
    }
}

// The engine's backlog brackets around a name (name plate is bare).
constexpr char NAME_BRACKET_OPEN[]  = "\xE3\x80\x90";  // U+3010
constexpr char NAME_BRACKET_CLOSE[] = "\xE3\x80\x91";  // U+3011

std::string bracketed(const std::string& s) {
    return NAME_BRACKET_OPEN + s + NAME_BRACKET_CLOSE;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

// Lines a surface draws this English value on, measured by the wrapper the
// game runs over the bytes the game holds.
std::size_t drawn_lines(const std::string& en_utf8, std::size_t max_lines,
                        bool& truncated) {
    const translator_logic::WrappedText w = translator_logic::WrapForMessageBox(
        translator_logic::EnglishToCp932(en_utf8), max_lines);
    truncated = w.truncated;
    return w.lines;
}

std::size_t history_lines(const std::string& en_utf8, bool& truncated) {
    return drawn_lines(en_utf8, translator_logic::HISTORY_MAX_LINES, truncated);
}

// The same sentences with the breaks between them turned back into spaces, so
// the wrap decides where the lines end.  The writer's breaks fit Japanese
// lines; English is wider, so the segment before a break wrapped and left a
// stub line ("weighty in / the hand.").  Trailing breaks chain a box and
// stay.
std::string run_together(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\n' && value[i] != '\r') { out += value[i]; continue; }
        // Trailing breaks chain a box to its next segment and are not layout
        // this may touch.
        if (value.find_first_not_of("\r\n", i) == std::string::npos) {
            out.append(value, i, std::string::npos);
            break;
        }
        while (i + 1 < value.size() && (value[i + 1] == '\n' || value[i + 1] == '\r'))
            ++i;
        if (!out.empty() && out.back() != ' ') out += ' ';
    }
    return out;
}

// Byte ratio EN/JP; the renderer's rect array is sized from the JP bytes.
double byte_ratio(const std::string& jp_utf8, const std::string& en_utf8) {
    const std::size_t jp = translator_logic::Utf8ToCp932(jp_utf8).size();
    const std::size_t en = translator_logic::EnglishToCp932(en_utf8).size();
    return jp ? static_cast<double>(en) / static_cast<double>(jp) : 0.0;
}

}  // namespace

std::vector<std::string> name_plates_sharing_english(
    const std::vector<std::pair<std::string, std::string>>& name_rows) {
    std::map<std::string, std::vector<std::string>> keys_of;
    for (const auto& [jp, en] : name_rows) keys_of[en].push_back(jp);
    std::vector<std::string> out;
    for (const auto& [en, keys] : keys_of)
        if (keys.size() > 1) out.push_back(join(keys, " / ") + " -> " + en);
    return out;
}

std::string apply_pinned_terms(const std::string& jp, const std::string& en) {
    std::string out = en;
    repin(out, jp, translate::superseded_renderings(), pinned_terms());
    repin(out, jp, translate::superseded_name_renderings(), pinned_names());
    return out;
}

std::string normalize_leading_space(const std::string& jp, const std::string& en) {
    std::size_t want = leading_indent_bytes(jp);
    // Drop whatever the English starts with -- its own ideographic spaces and
    // any stray ASCII blank -- then restore exactly the key's indent.
    std::size_t have = leading_indent_bytes(en);
    while (have < en.size() && (en[have] == ' ' || en[have] == '\t')) ++have;
    return jp.substr(0, want) + en.substr(have);
}

int run_build(const std::string& translated_file, const std::string& out_tsv,
              bool include_untranslated) {
    bj::value root = json_parse_file(translated_file);

    // jp -> en, insertion-ordered, last-wins on value.
    std::vector<std::pair<std::string, std::string>> order;
    std::unordered_map<std::string, std::size_t> index;
    std::size_t conflicts = 0, untranslated = 0, total_seen = 0;

    // Backlog-only data: speaker names and multi-string boxes.
    std::vector<std::string> name_keys;
    std::unordered_set<std::string> name_seen;
    std::vector<ChainedBox> chained;

    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) continue;
        const auto& script = kv.value().get_object();
        auto it = script.find("strings");
        if (it == script.end() || !it->value().is_array()) continue;
        std::vector<DrawnSegment> drawn;
        drawn.reserve(it->value().get_array().size());
        for (const auto& ev : it->value().get_array()) {
            const auto& entry = ev.get_object();
            std::string jp(entry.at("text").get_string());
            std::string en = jp;
            if (auto* t = entry.if_contains("translated"))
                if (t->is_string()) en = std::string(t->get_string());
            DrawnSegment seg;
            seg.jp = jp;
            if (auto* t = entry.if_contains("type"))
                if (t->is_string()) seg.is_name = std::string(t->get_string()) == "name";
            if (seg.is_name && name_seen.insert(jp).second) name_keys.push_back(jp);
            drawn.push_back(std::move(seg));
            ++total_seen;
            if (!include_untranslated && en == jp) {
                ++untranslated;
                continue;
            }
            auto idx_it = index.find(jp);
            if (idx_it != index.end()) {
                if (order[idx_it->second].second != en) ++conflicts;
                order[idx_it->second].second = en;
            } else {
                index.emplace(jp, order.size());
                order.emplace_back(jp, en);
            }
        }
        collect_chained_boxes(drawn, chained);
    }

    const std::size_t rows_from_scripts = order.size();

    // Character names from the glossary; existing document rows win.
    std::size_t name_disagreements = 0;
    for (const auto* names : {&translate::name_translations_ordered(),
                              &translate::extra_name_translations()})
        for (const auto& [jp, en] : *names) {
            if (name_seen.insert(jp).second) name_keys.push_back(jp);
            auto it = index.find(jp);
            if (it != index.end()) {
                // Document wins, but a disagreement after repin means the
                // game would spell the same person two ways.
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
    std::size_t repinned = 0, run_together_rows = 0;
    std::vector<std::string> markup_rows;
    // The shipped value per key; derived rows reuse these so panel and backlog agree.
    std::unordered_map<std::string, std::string> shipped;
    shipped.reserve(order.size() * 2);
    for (const auto& [jp, en] : order) {
        std::string value = apply_pinned_terms(jp, en);
        if (value != en) ++repinned;
        // Strip ruby tags from translations (the engine consumes them silently);
        // untranslated rows keep their original furigana.
        if (value != jp && translator_logic::CarriesEngineMarkup(value)) {
            markup_rows.push_back(jp);
            value = translator_logic::StripEngineMarkup(value);
        }
        // The key keeps its break: the engine passes the whole string with
        // the break inside the buffer.  The value flows.
        const std::string aligned = normalize_leading_space(jp, value);
        const std::string shipped_value = run_together(aligned);
        if (shipped_value != aligned) ++run_together_rows;
        out += esc(jp) + "\t" + esc(shipped_value) + "\n";
        shipped.emplace(jp, shipped_value);
    }
    const std::size_t rows_for_the_panel = order.size();

    // Name plates and their shipped English; a collision here hits both surfaces.
    std::vector<std::pair<std::string, std::string>> plates;
    plates.reserve(name_keys.size());
    for (const std::string& jp : name_keys) {
        auto it = shipped.find(jp);
        if (it != shipped.end()) plates.emplace_back(jp, it->second);
    }
    const std::vector<std::string> shared_name_plates =
        name_plates_sharing_english(plates);

    // --- The rows only the backlog reaches --------------------------------
    // The backlog redraws each box in one call using concatenated strings and
    // bracketed names -- keys no per-string row carries.  Emit them here.
    std::size_t name_rows = 0, box_rows = 0;
    std::size_t box_rows_cut = 0, boxes_without_english = 0;
    double worst_ratio = 0.0;
    auto emit = [&](const std::string& key, const std::string& value) {
        if (!index.emplace(key, order.size()).second) return false;
        order.emplace_back(key, value);
        out += esc(key) + "\t" + esc(value) + "\n";
        const double ratio = byte_ratio(key, value);
        if (ratio > worst_ratio) worst_ratio = ratio;
        return true;
    };

    for (const std::string& jp : name_keys) {
        auto it = shipped.find(jp);
        if (it == shipped.end()) continue;
        if (emit(bracketed(jp), bracketed(it->second))) ++name_rows;
    }

    for (const ChainedBox& box : chained) {
        const std::string key = join(box, "");
        // The same box drawn twice, or a string that already spells it out,
        // needs one row and keeps the one it has.
        if (index.count(key)) continue;
        std::vector<std::string> english;
        english.reserve(box.size());
        for (const std::string& jp : box) {
            auto it = shipped.find(jp);
            if (it == shipped.end()) break;
            // A break of the value's own at the very end would double the one
            // the join puts there.
            std::string part = it->second;
            while (!part.empty() && (part.back() == '\n' || part.back() == '\r'))
                part.pop_back();
            english.push_back(std::move(part));
        }
        // Skip boxes where some strings lack English; the backlog draws
        // those as it does today.
        if (english.size() != box.size()) { ++boxes_without_english; continue; }

        // One paragraph, mirroring the panel: the panel flows a chained box
        // at draw time, so the backlog's one-call redraw joins the same way.
        bool cut = false;
        std::string value = join(english, " ");
        history_lines(value, cut);
        if (cut) ++box_rows_cut;
        emit(key, value);
        ++box_rows;
    }
    write_file(out_tsv, out);

    log_info("Total entries seen:      " + std::to_string(total_seen));
    log_info("Unique JP strings:       " + std::to_string(rows_from_scripts + untranslated));
    log_info("Translated entries kept: " + std::to_string(rows_from_scripts));
    log_info("Untranslated (skipped):  " + std::to_string(untranslated));
    log_info("Glossary name rows:      " +
             std::to_string(rows_for_the_panel - rows_from_scripts));
    log_info("Backlog name rows:       " + std::to_string(name_rows) +
             "  (the same name inside the brackets the backlog draws it in)");
    log_info("Backlog whole-box rows:  " + std::to_string(box_rows) + " of " +
             std::to_string(box_rows + boxes_without_english) +
             " boxes more than one string fills  (" +
             std::to_string(boxes_without_english) +
             " still hold a string with no English)");
    if (box_rows)
        log_info("  of those, over " +
                 std::to_string(translator_logic::HISTORY_MAX_LINES) +
                 " rows as one paragraph and so cut and marked in the "
                 "backlog alone: " +
                 std::to_string(box_rows_cut));
    if (worst_ratio > 0.0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", worst_ratio);
        log_info(std::string("Worst backlog byte ratio: x") + buf +
                 "  (English bytes per Japanese byte; the render's rectangle "
                 "array is sized from the Japanese)");
    }
    log_info("Rows written:            " + std::to_string(order.size()));
    if (repinned)
        log_info("Rows re-pinned:          " + std::to_string(repinned) +
                 "  (superseded terminology and names rewritten)");
    if (run_together_rows)
        log_info("Rows run together:       " + std::to_string(run_together_rows) +
                 "  (a break inside the English became a space and the wrap "
                 "places the lines; the key keeps its break)");
    if (name_disagreements)
        log_info("Name rows disagreeing:   " + std::to_string(name_disagreements) +
                 "  (document row kept -- see the lines above)");
    log_info("Names sharing one English: " +
             std::to_string(shared_name_plates.size()) +
             (shared_name_plates.empty()
                  ? ""
                  : "  (two speakers the player cannot tell apart, on the "
                    "name plate and again in the backlog)"));
    for (const std::string& shared : shared_name_plates) log_info("  " + shared);
    if (conflicts)
        log_info("Conflicting duplicates:  " + std::to_string(conflicts) + "  (last-wins)");
    log_info("Rows carrying ruby markup: " + std::to_string(markup_rows.size()) +
             (markup_rows.empty()
                  ? ""
                  : "  (markup removed -- the engine consumes the tag and"
                    " draws none of it)"));
    for (std::size_t i = 0; i < markup_rows.size() && i < 25; ++i)
        log_info("  ruby markup in the English for: " + markup_rows[i]);
    if (markup_rows.size() > 25)
        log_info("  ... and " + std::to_string(markup_rows.size() - 25) + " more");
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(out_tsv))) / 1024.0 / 1024.0);
    log_info("Wrote " + out_tsv + " (" + std::string(sz) + ")");

    // Measure the shipped table against the boxes the engine draws it in.
    print_line("");
    BoxBudgetReport boxes =
        check_box_budgets_of_document(translated_file,
                                      load_runtime_table(out_tsv));
    log_info("Message boxes:           " + std::to_string(boxes.boxes) +
             "  (" + std::to_string(boxes.chained_boxes) +
             " filled by more than one string)");
    if (boxes.over_budget_boxes == 0) {
        log_info("Boxes over budget:       0");
    } else {
        log_info("Boxes over budget:       " +
                 std::to_string(boxes.over_budget_boxes) + "  (" +
                 std::to_string(boxes.over_budget_chained) + " chained; " +
                 std::to_string(boxes.truncated_segments) + " strings cut, " +
                 std::to_string(boxes.dropped_segments) +
                 " with no line left)  -- the English has to be shorter here");
        for (const BoxOverflow& o : boxes.examples)
            log_info("  " + o.script + " segment " +
                     std::to_string(o.first_segment) + " (+" +
                     std::to_string(o.segments - 1) + "): box held " +
                     std::to_string(o.lines_before) + " lines, this one drew " +
                     std::to_string(o.lines_drawn) + " -- " + o.japanese);
        if (boxes.over_budget_boxes > boxes.examples.size())
            log_info("  (" +
                     std::to_string(boxes.over_budget_boxes -
                                    boxes.examples.size()) +
                     " more not named: the listing stops at " +
                     std::to_string(boxes.max_examples) + ")");
    }

    print_line("");
    CoverageReport coverage = count_coverage(root.get_object(), translated_file);
    log_coverage_summary(coverage);
    write_coverage_report(coverage, coverage_report_path(translated_file));
    return 0;
}

}  // namespace snu::build_tsv
