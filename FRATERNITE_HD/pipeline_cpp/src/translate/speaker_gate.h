// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The speaker gate: five checks that prove a speaker name still reaches the
// API prompt, run before a single token is spent.
//
// Fraternite is "Pattern B-inline": YU-RIS writes the speaker into the line
// itself as `Name「...」`, so extract_speaker parses the raw text with
// inline_speaker_match.  This gate catches the regression where that parse
// stops recognising the prefix and every dialogue line becomes NARRATION --
// the model then has no character context and pronouns, formality and voice
// all break, fluently and plausibly.  The run is one-shot and the cache is
// never edited, so that damage is permanent -- which is why the checks gate
// the translation step itself rather than sitting beside it as something to
// remember.
//
// Spends ZERO API tokens: the user prompt is reconstructed locally.
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

namespace frat::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;  // 1..5; 0 when every check passed
    std::string reason;    // the failure in one line, empty on success

    std::size_t entries_sampled = 0;
    std::size_t entries_total = 0;
    std::size_t speakers_in_data = 0;  // distinct JP speakers the scripts use
    std::size_t glossary_entries = 0;  // rows in the speaker table
};

// Run the checks over an extracted document and print the report as it goes.
// Every line goes through print_line rather than log_info: the report IS the
// gate's output, meant to be read and diffed as-is, so no timestamps.
SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace frat::translate
