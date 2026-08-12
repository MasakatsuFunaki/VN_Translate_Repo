// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The speaker gate: five checks that prove a speaker name still reaches the
// API prompt, run before a single token is spent.
//
// CROSS_CHANNEL is "Pattern A": 01_extract writes an explicit `speaker` field
// per entry.  The regression this gate exists for is extract_speaker reading
// the wrong field and returning 'NARRATION' for every dialogue line -- the
// model then has no character context, and pronouns, formality and voice all
// break in an answer that comes back fluent and plausible.  A run is paid for
// once and the cache is never edited by hand, so that damage is permanent,
// which is why the checks gate the translation step itself instead of sitting
// beside it as something to remember.
//
// Spends ZERO API tokens: it reads the extraction and the glossary, and the
// only prompt it builds is the one it inspects.
//
// The report it prints as it goes is a deliverable in its own right, so the
// lines are bare -- no "HH:MM:SS INFO" prefix anywhere -- and the layout (repr
// quoting, codepoint-width padding, the "[n/5]" headings) is meant to be
// diffable between runs.
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

namespace crc::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;  // 1..5; 0 when every check passed
    std::string reason;    // the failure in one line, empty on success

    std::size_t dialogue_sampled = 0;
    std::size_t dialogue_total = 0;
    std::size_t speakers_in_data = 0;  // distinct JP speakers the scripts use
    std::size_t glossary_entries = 0;  // rows in the name table
};

// Run the checks over an extracted document and print the report as it goes.
SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace crc::translate
