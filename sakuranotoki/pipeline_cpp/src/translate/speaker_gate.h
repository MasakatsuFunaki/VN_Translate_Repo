// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The speaker gate: six checks that prove a speaker name still reaches the
// API prompt with a voice behind it, run before a single token is spent.
//
// Sakura no Toki is "Pattern C": step 1 emits type='name' strings interleaved
// with type='dialogue' and type='narrative', and a state machine
// (iter_entries_with_speakers) walks them in script order to attribute each
// dialogue to its preceding name.  The regression these checks exist for is
// the one that costs the most and shows the least: that tracking breaks,
// every dialogue line loses its speaker, the model reads [NARRATION] for all
// of them, and the answer comes back fluent, plausible and wrong in every
// voice.  The run is one-shot and the cache is never edited by hand, so that
// damage is permanent -- which is why the checks gate the translation step
// itself rather than sitting beside it as something to remember.
//
// Spends ZERO API tokens: the checks read the extraction and the glossary,
// and the only prompt they build is the one they inspect.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace snt::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;  // 1..6; 0 when every check passed
    std::string reason;    // the failure in one line, empty on success

    std::size_t dialogue_sampled = 0;   // dialogue entries checks 1 and 3 saw
    std::size_t speakers_in_data = 0;   // distinct JP speakers the scripts use
    std::size_t glossary_entries = 0;   // rows in the speaker table
    std::size_t dossier_labels = 0;     // labels check 6 required a dossier line for
};

// A speaker that carries at least this many spoken lines needs a dossier line.
// Below it the model can read the character off the surrounding text; above it
// a wrong sex is repeated hundreds of times and the cache keeps it.
inline constexpr std::size_t DOSSIER_LINE_THRESHOLD = 200;

// Spoken lines per Japanese speaker label over a whole extraction.  An entry's
// `speaker` field answers for it; an entry without one falls back to the plate
// standing over it.
std::map<std::string, std::size_t> dialogue_lines_by_speaker(
    const boost::json::object& extracted);

// A label the dossier leaves the model guessing about.
struct DossierGap {
    std::string label;   // the Japanese label, as the scripts spell it
    std::size_t lines;   // spoken lines it carries
    std::string reason;  // what the dossier fails to say
};

// Labels at or above DOSSIER_LINE_THRESHOLD that `dossier` does not name, or
// names without stating a sex.  A sex is stated by "**Male**", "**Female**",
// "he/him" or "she/her" on the same line as the label.
std::vector<DossierGap> dossier_gaps(
    const std::map<std::string, std::size_t>& lines_by_label,
    const std::string& dossier);

// A label the dossier prose spells differently from the name table.
struct DossierNameMismatch {
    std::string label;    // the Japanese label, as the scripts spell it
    std::string dossier;  // the English the prose puts in front of "(<label>"
    std::string table;    // the English name_translations_ordered() gives it
};

// Person labels the prose introduces as "<English> (<label>" while the name
// table renders them differently.  A family name may lead the prose spelling,
// so "Nakamura Shouichi (章一)" agrees with the table's "Shouichi"; "Rurio"
// against "Ruriwo" does not.  Labels the prose never introduces are skipped:
// the dossier covers the leads, and check 6 is what demands a line for each.
std::vector<DossierNameMismatch> dossier_name_mismatches(const std::string& dossier);

// The <character_dossier> block of the system prompt, empty when the prompt
// carries no such block.
std::string prompt_dossier();

// Run the checks over an extracted document and print the report as it goes.
SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace snt::translate
