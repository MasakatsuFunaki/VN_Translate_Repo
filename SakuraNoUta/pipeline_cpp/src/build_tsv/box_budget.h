// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Does the English fit the box?  Chains strings that share a box and measures
// each through the proxy DLL's own substitution.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "translator_logic.h"

namespace snu::build_tsv {

// One string the engine draws, in the order the script draws them.
struct DrawnSegment {
    std::string jp;             // as extracted: its own trailing 0x0A included
    bool        is_name = false;  // the name plate is a box of its own
};

// A box whose English needs more lines than the box has.
struct BoxOverflow {
    std::string script;
    std::size_t first_segment = 0;  // index into the script's segment list
    std::size_t segments = 0;
    std::size_t lines_before = 0;   // lines the box held when it arrived
    std::size_t lines_drawn = 0;    // lines it got to use
    std::string japanese;           // the segment that did not fit
    std::string english;            // what the box will actually show of it
    bool        dropped = false;    // no line was left for it at all
};

struct BoxBudgetReport {
    std::size_t segments = 0;
    std::size_t boxes = 0;
    std::size_t chained_boxes = 0;
    std::size_t over_budget_boxes = 0;
    std::size_t over_budget_chained = 0;   // of those, boxes with a chain
    // Of those, all-English boxes -- the only overflow count a rate may use.
    std::size_t over_budget_with_english = 0;
    std::size_t truncated_segments = 0;
    std::size_t dropped_segments = 0;
    // Over-budget boxes named individually; the report says when it stops.
    std::vector<BoxOverflow> examples;
    std::size_t max_examples = 500;

    // Boxes by physical line count; last bucket is "at or above".
    static const std::size_t kMaxCountedLines = 9;
    std::size_t lines_histogram[kMaxCountedLines + 1] = {};

    // Boxes every string of which had English.  The measurements below are
    // taken over those alone.
    std::size_t boxes_with_english = 0;

    // JP/EN column pairs per translated string; kept unsummarised for the
    // upper-tail analysis that decides how many boxes overflow.
    std::vector<std::pair<std::size_t, std::size_t>> expansion;
};

// Columns the engine draws `cp932` in: two per full-width character, and none
// at all for a "<...>" run, which the engine consumes.
std::size_t drawn_columns(const std::string& cp932);

// True if this string hands its box on to the string after it: the engine ends
// a box at the first string that does NOT end in 0x0A.
bool hands_the_box_on(const std::string& jp);

// One message box, as the strings that fill it, in draw order.
using ChainedBox = std::vector<std::string>;

// Every box of `script` that more than one string fills, appended to `out`.
// Name plates are their own box and never join a chain.
void collect_chained_boxes(const std::vector<DrawnSegment>& script,
                           std::vector<ChainedBox>& out);

// Walk one script and add findings to `report`.  `table` must be built via
// ParseTsvBuffer over the shipped TSV so the artifact under test is the real one.
void check_box_budgets(const std::string& script_name,
                       const std::vector<DrawnSegment>& script,
                       const translator_logic::TranslationMap& table,
                       BoxBudgetReport& report);

// Read the strings of `translated_file` in draw order, script by script, and
// measure them against `table`.  Returns the whole-corpus report.
BoxBudgetReport check_box_budgets_of_document(
    const std::string& translated_file,
    const translator_logic::TranslationMap& table);

// Load a TSV from disk exactly as the DLL loads it.  Empty on failure.
translator_logic::TranslationMap load_runtime_table(const std::string& tsv_path);

// Every translated string held the way the DLL holds it.  `runtime` wins
// where the two disagree -- it is what the game loads.
translator_logic::TranslationMap measurement_table(
    const std::string& translated_file,
    const translator_logic::TranslationMap& runtime);

// Boxes that would overflow if every Japanese string grew to `ratio` times
// its columns, measured through the game's own wrapper.
std::size_t project_boxes_over_budget(const std::string& translated_file,
                                      double ratio);

// Print worst byte ratio and rows over the renderer's rectangle-array budget
// (sized from JP, filled from EN).  `runtime` is the table the game loads.
void report_rect_array_audit(const translator_logic::TranslationMap& runtime);

// Print the whole-corpus box budget audit.  `table` is the runtime table.
void report_box_budget_audit(const std::string& translated_file,
                             const translator_logic::TranslationMap& table);

}  // namespace snu::build_tsv
