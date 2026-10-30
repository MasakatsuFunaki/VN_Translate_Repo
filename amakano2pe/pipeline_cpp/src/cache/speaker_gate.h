// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The speaker gate: five checks that prove a speaker name still reaches the
// API prompt, run before a single token is spent.
//
// Amakano 2 has no per-line speaker field.  A state machine
// (`extract_message_runs`) walks NAME / MESSAGE / `fw 0` lines in order and
// yields (line_idx, speaker, content).  The regression this catches is the one
// that costs the most and shows the least: that machine stops tracking names,
// every dialogue line reaches the model as [NARRATION], and the answer comes
// back fluent, plausible and wrong in every voice.  A run is one-shot and the
// cache is never edited afterwards, so the damage is permanent -- which is why
// the checks gate the translation step itself rather than sitting beside it as
// something to remember.
//
// Spends ZERO API tokens: it reads the extraction and the glossary, and the
// only prompt it builds is the one it inspects.
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

namespace ama::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;  // 1..5; 0 when every check passed
    std::string reason;    // the failure in one line, empty on success

    std::size_t runs_sampled = 0;       // message runs checks 1 and 3 read
    std::size_t scripts_scanned = 0;    // scripts in the extracted document
    std::size_t speakers_in_data = 0;   // distinct JP speakers the scripts use
    std::size_t glossary_entries = 0;   // rows in the speaker table
};

// Run the checks over an extracted document, printing the report as it goes.
SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace ama::translate
