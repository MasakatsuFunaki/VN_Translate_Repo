// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The speaker gate: five checks that prove a speaker name still reaches the
// API prompt, run before a single token is spent.
//
// Shingakkou is "Pattern A": 01_extract writes a `speaker` FIELD and
// extract_speaker reads it.  The gate catches the regression where it reads
// the wrong field and returns 'NARRATION' for 100% of dialogue lines -- the
// model then has no character context and pronouns, formality and voice all
// break, fluently and plausibly.  The run is one-shot and the cache is never
// edited, so that damage is permanent; which is why the checks gate the
// translation step itself rather than sitting beside it as a separate command
// someone has to remember.
//
// Spends ZERO API tokens: it reads the extraction and the glossary, and the
// only prompt it builds is the one it inspects.
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

namespace shin::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;  // 1..5; 0 when every check passed
    std::string reason;    // the failure in one line, empty on success

    std::size_t dialogue_sampled = 0;
    std::size_t dialogue_total = 0;
    std::size_t speakers_in_data = 0;  // distinct JP speakers the scripts use
    std::size_t glossary_entries = 0;  // rows in the speaker table
};

// Run the checks over an extracted document and print the report as it goes.
//
// The five non-ASCII characters in that report (the U+2192 arrow in check 2
// and four U+2014 em dashes) are deliberate and must stay: the output is
// diffed run against run, so ASCII-ifying them shows up as a spurious change
// on every line that carries one.
SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace shin::translate
