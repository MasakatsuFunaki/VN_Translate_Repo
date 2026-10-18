// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "box_budget.h"

#include "build_tsv.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <boost/json.hpp>

#include "common/util.h"

namespace snu::build_tsv {

namespace bj = boost::json;
namespace tl = translator_logic;

namespace {

// The two boxes the renderer draws into; identity must differ.
constexpr std::uintptr_t kDialogueBox = 1;
constexpr std::uintptr_t kNameBox = 2;

// The engine's continuation prefix.  Every segment after the first of a box
// arrives with one, and the DLL strips it before the lookup.
constexpr char kContinuationPrefix = '\x05';

}  // namespace

std::size_t drawn_columns(const std::string& cp932) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < cp932.size(); ) {
        const unsigned char b = static_cast<unsigned char>(cp932[i]);
        if (b == '<') {
            // Tags are consumed by the engine and draw nothing.
            const std::size_t close = cp932.find('>', i + 1);
            if (close != std::string::npos && close > i + 1) {
                i = close + 1;
                continue;
            }
        }
        if (b == '\n' || b == '\r') { ++i; continue; }
        const bool wide = tl::IsCp932LeadByte(b) && i + 1 < cp932.size();
        n += wide ? 2 : 1;
        i += wide ? 2 : 1;
    }
    return n;
}

bool hands_the_box_on(const std::string& jp) {
    return !jp.empty() && (jp.back() == '\n' || jp.back() == '\r');
}

void collect_chained_boxes(const std::vector<DrawnSegment>& script,
                           std::vector<ChainedBox>& out) {
    ChainedBox box;
    for (const DrawnSegment& seg : script) {
        if (seg.is_name) continue;
        box.push_back(seg.jp);
        if (hands_the_box_on(seg.jp)) continue;
        if (box.size() > 1) out.push_back(box);
        box.clear();
    }
    // A script whose last string still asks for another one leaves a box open.
    // The engine closes it at the end of the script and so does this.
    if (box.size() > 1) out.push_back(std::move(box));
}

void check_box_budgets(const std::string& script_name,
                       const std::vector<DrawnSegment>& script,
                       const tl::TranslationMap& table,
                       BoxBudgetReport& report) {
    tl::BoxLineBudget budget;
    bool        chain_open = false;
    std::size_t box_start = 0;
    std::size_t box_segments = 0;
    std::size_t box_lines = 0;
    std::size_t box_with_english = 0;   // segments of this box that had English
    BoxOverflow overflow;
    bool        box_overflowed = false;

    auto close_box = [&] {
        if (box_segments == 0) return;
        ++report.boxes;
        if (box_segments > 1) ++report.chained_boxes;
        ++report.lines_histogram[
            box_lines < BoxBudgetReport::kMaxCountedLines
                ? box_lines : BoxBudgetReport::kMaxCountedLines];
        const bool all_english = (box_with_english == box_segments);
        if (all_english) ++report.boxes_with_english;
        if (box_overflowed) {
            ++report.over_budget_boxes;
            // Only all-English overflows share a population with boxes_with_english.
            if (all_english) ++report.over_budget_with_english;
            if (box_segments > 1) ++report.over_budget_chained;
            overflow.script = script_name;
            overflow.first_segment = box_start;
            overflow.segments = box_segments;
            if (report.examples.size() < report.max_examples)
                report.examples.push_back(overflow);
        }
        box_segments = 0;
        box_lines = 0;
        box_with_english = 0;
        box_overflowed = false;
    };

    for (std::size_t i = 0; i < script.size(); ++i) {
        const DrawnSegment& seg = script[i];
        ++report.segments;

        // The name plate is drawn into its own box and never joins a chain,
        // so it neither spends the dialogue box's lines nor closes it.
        const bool continues = !seg.is_name && chain_open;
        if (!seg.is_name && !continues) {
            close_box();
            box_start = i;
        }

        std::string wire;
        if (continues) wire += kContinuationPrefix;
        wire += tl::Utf8ToCp932(seg.jp);

        tl::Substitution sub;
        const tl::SubstitutionStatus status =
            tl::BuildSubstitution(wire.c_str(), table, budget,
                                  seg.is_name ? kNameBox : kDialogueBox, sub);

        if (seg.is_name) continue;

        ++box_segments;
        box_lines += sub.lines;
        if (status == tl::SubstitutionStatus::Substituted) {
            ++box_with_english;
            report.expansion.emplace_back(
                drawn_columns(tl::Utf8ToCp932(seg.jp)),
                drawn_columns(sub.bytes));
        }
        if (sub.truncated || sub.dropped) {
            box_overflowed = true;
            if (sub.dropped) ++report.dropped_segments;
            else             ++report.truncated_segments;
            overflow.japanese = seg.jp;
            overflow.english = tl::Cp932ToUtf8(sub.bytes);
            overflow.lines_before = sub.box_lines_before;
            overflow.lines_drawn = sub.lines;
            overflow.dropped = sub.dropped;
        }
        chain_open = sub.more_follows;
        if (!chain_open) close_box();
    }
    close_box();
}

namespace {

// Walk the translated document in draw order; every measurement uses this.
template <typename Visit>
void for_each_script(const std::string& translated_file, Visit visit) {
    bj::value root = json_parse_file(translated_file);
    if (!root.is_object()) return;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) continue;
        const auto& script = kv.value().get_object();
        auto it = script.find("strings");
        if (it == script.end() || !it->value().is_array()) continue;
        std::vector<DrawnSegment> segments;
        segments.reserve(it->value().get_array().size());
        for (const auto& ev : it->value().get_array()) {
            if (!ev.is_object()) continue;
            const auto& entry = ev.get_object();
            DrawnSegment seg;
            seg.jp = std::string(entry.at("text").get_string());
            if (auto* t = entry.if_contains("type"))
                if (t->is_string())
                    seg.is_name = std::string(t->get_string()) == "name";
            segments.push_back(std::move(seg));
        }
        visit(std::string(kv.key()), segments);
    }
}

}  // namespace

BoxBudgetReport check_box_budgets_of_document(const std::string& translated_file,
                                              const tl::TranslationMap& table) {
    BoxBudgetReport report;
    for_each_script(translated_file,
                    [&](const std::string& name,
                        const std::vector<DrawnSegment>& segments) {
        check_box_budgets(name, segments, table, report);
    });
    return report;
}

std::size_t project_boxes_over_budget(const std::string& translated_file,
                                      double ratio) {
    // Scale each JP string's columns by `ratio` and measure through the game's
    // wrapper.  An unbroken run is best-case, so the result is a floor.
    tl::TranslationMap synthetic;
    for_each_script(translated_file,
                    [&](const std::string&,
                        const std::vector<DrawnSegment>& segments) {
        for (const DrawnSegment& seg : segments) {
            if (seg.is_name) continue;
            std::string key = tl::Utf8ToCp932(seg.jp);
            if (synthetic.count(key)) continue;
            const auto cols = static_cast<std::size_t>(
                drawn_columns(key) * ratio + 0.5);
            if (cols == 0) continue;
            synthetic.emplace(std::move(key), std::string(cols, 'x'));
        }
    });
    return check_box_budgets_of_document(translated_file, synthetic)
               .over_budget_boxes;
}

namespace {

std::string pct(std::size_t part, std::size_t whole) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f%%",
                  100.0 * static_cast<double>(part) /
                      static_cast<double>(whole ? whole : 1));
    return buf;
}

std::string fixed2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

double quantile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    auto i = static_cast<std::size_t>(sorted.size() * q);
    if (i >= sorted.size()) i = sorted.size() - 1;
    return sorted[i];
}

}  // namespace

void report_rect_array_audit(const tl::TranslationMap& runtime) {
    // The rectangle array is sized from the JP and filled from the EN, so
    // the byte ratio decides overruns -- short keys with long values are at risk.
    std::size_t over_stock = 0, over_patched = 0;
    double worst = 0.0;
    std::string worst_key, worst_value;
    for (const auto& [key, value] : runtime) {
        if (key.empty()) continue;
        const double ratio = static_cast<double>(value.size()) /
                             static_cast<double>(key.size());
        if (ratio > worst) { worst = ratio; worst_key = key; worst_value = value; }
        if (value.size() > tl::DrawBudget(key.size(), tl::RECTS_PER_WIRE_BYTE))
            ++over_stock;
        if (value.size() > tl::DrawBudget(key.size(), tl::RECTS_PER_WIRE_BYTE * 8))
            ++over_patched;
    }
    print_line("");
    log_info("Rows measured against the render's rectangle array: " +
             std::to_string(runtime.size()));
    log_info("Worst byte ratio:        x" + fixed2(worst) + "  (" +
             tl::Cp932ToUtf8(worst_key) + " -> " + tl::Cp932ToUtf8(worst_value) +
             ")");
    log_info("Rows over the engine's own 4 rectangles per byte: " +
             std::to_string(over_stock) +
             "  (cut and marked when the byte patch is refused)");
    log_info("Rows over the patched 32 per byte: " + std::to_string(over_patched));
}

void report_box_budget_audit(const std::string& translated_file,
                             const tl::TranslationMap& table) {
    // With no table every string is drawn by the engine, so this measures the
    // Japanese with the engine's own line-breaking rule.
    const BoxBudgetReport jp =
        check_box_budgets_of_document(translated_file, tl::TranslationMap{});
    log_info("Message boxes:           " + std::to_string(jp.boxes) +
             "  (" + std::to_string(jp.chained_boxes) + " filled by more than "
             "one string, " + pct(jp.chained_boxes, jp.boxes) + ")");
    std::string hist;
    for (std::size_t n = 1; n <= BoxBudgetReport::kMaxCountedLines; ++n) {
        if (jp.lines_histogram[n] == 0) continue;
        if (!hist.empty()) hist += "  ";
        hist += std::to_string(n) +
                (n == BoxBudgetReport::kMaxCountedLines ? "+ lines: " : " line"
                 + std::string(n > 1 ? "s" : "") + ": ") +
                std::to_string(jp.lines_histogram[n]);
    }
    log_info("Lines the Japanese needs: " + hist);
    std::size_t full = 0;
    for (std::size_t n = translator_logic::MAX_LINES;
         n <= BoxBudgetReport::kMaxCountedLines; ++n)
        full += jp.lines_histogram[n];
    log_info("Boxes the Japanese already fills: " + std::to_string(full) +
             "  (" + pct(full, jp.boxes) + ")");

    const BoxBudgetReport en =
        check_box_budgets_of_document(translated_file, table);
    log_info("Boxes with English for every string: " +
             std::to_string(en.boxes_with_english));
    log_info("Of those, over the box:   " +
             std::to_string(en.over_budget_with_english) + "  (" +
             pct(en.over_budget_with_english, en.boxes_with_english) +
             " of those measured)");
    if (en.over_budget_boxes != en.over_budget_with_english)
        log_info("Boxes over the box in all: " +
                 std::to_string(en.over_budget_boxes) +
                 "  (the rest are boxes that still hold some Japanese, so "
                 "they are not measured against the line above)");

    std::vector<double> ratios;
    std::size_t jp_cols = 0, en_cols = 0;
    ratios.reserve(en.expansion.size());
    for (const auto& [j, e] : en.expansion) {
        jp_cols += j;
        en_cols += e;
        if (j) ratios.push_back(static_cast<double>(e) / static_cast<double>(j));
    }
    if (ratios.empty()) {
        log_info("No English measured; nothing to project.");
        return;
    }
    std::sort(ratios.begin(), ratios.end());
    const double mean = static_cast<double>(en_cols) /
                        static_cast<double>(jp_cols ? jp_cols : 1);
    log_info("Columns, Japanese -> English: " + std::to_string(jp_cols) +
             " -> " + std::to_string(en_cols) + "  (x" + fixed2(mean) + ")");
    log_info("Per-string ratio: median x" + fixed2(quantile(ratios, 0.5)) +
             "  p75 x" + fixed2(quantile(ratios, 0.75)) +
             "  p90 x" + fixed2(quantile(ratios, 0.90)) +
             "  p99 x" + fixed2(quantile(ratios, 0.99)));

    print_line("");
    log_info("Projected over every box, by scaling each Japanese string's "
             "columns:");
    const std::pair<const char*, double> tiers[] = {
        { "median", quantile(ratios, 0.5) },
        { "mean",   mean },
        { "p75",    quantile(ratios, 0.75) },
        { "p90",    quantile(ratios, 0.90) },
    };
    for (const auto& [label, ratio] : tiers) {
        const std::size_t over =
            project_boxes_over_budget(translated_file, ratio);
        log_info("  at x" + fixed2(ratio) + " (" + label + "): " +
                 std::to_string(over) + " boxes over the box  (" +
                 pct(over, jp.boxes) + ")");
    }
    log_info("Each tier is one scalar applied to every string alike, so the"
             " four of them are not a confidence interval.  What decides"
             " whether a box overflows is the upper tail of the per-string"
             " ratio rather than its average, so the mean tier under-states"
             " and the p90 tier over-states.  The direct measurement above is"
             " the only count; these are its shape.");
}

tl::TranslationMap load_runtime_table(const std::string& tsv_path) {
    tl::TranslationMap table;
    std::ifstream f(tsv_path, std::ios::binary);
    if (!f) return table;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string buf = ss.str();
    tl::ParseTsvBuffer(buf.data(), buf.size(), table);
    return table;
}

tl::TranslationMap measurement_table(const std::string& translated_file,
                                     const tl::TranslationMap& runtime) {
    // Written as a TSV and read back through the DLL's own parser, so the
    // values are folded, stripped and keyed exactly as the game holds them.
    std::string tsv;
    bj::value root = json_parse_file(translated_file);
    if (root.is_object()) {
        for (const auto& kv : root.get_object()) {
            if (!kv.value().is_object()) continue;
            const auto& script = kv.value().get_object();
            auto it = script.find("strings");
            if (it == script.end() || !it->value().is_array()) continue;
            for (const auto& ev : it->value().get_array()) {
                if (!ev.is_object()) continue;
                const auto& entry = ev.get_object();
                const std::string jp(entry.at("text").get_string());
                auto* t = entry.if_contains("translated");
                if (!t || !t->is_string()) continue;
                const std::string en(t->get_string());
                if (en.empty() || en == jp) continue;
                tsv += esc(jp) + "\t" + esc(normalize_leading_space(jp, en)) + "\n";
            }
        }
    }
    tl::TranslationMap table;
    tl::ParseTsvBuffer(tsv.data(), tsv.size(), table);
    for (const auto& kv : runtime) table[kv.first] = kv.second;
    return table;
}

}  // namespace snu::build_tsv
