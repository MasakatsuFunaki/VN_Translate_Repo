// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The speaker gate: five checks that prove a speaker name still reaches the
// API prompt, run before a single token is spent.
//
// Mushigurui is the hard case.  BLACKCyc stores no speaker field at all: the
// name plate is the first CRLF segment of the line itself, "Name\r\n
// 「Dialogue」", and extract_speaker parses it back out.  When that parse
// stops recognising the prefix every dialogue line becomes NARRATION, the
// model loses all character context, and the answer comes back fluent,
// plausible and wrong in every voice.  The run is one-shot and the cache is
// never edited, so that damage is permanent -- which is why the checks gate
// the translation step itself rather than sitting beside it as something to
// remember.
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

namespace mgi::translate {

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
// Spends nothing: it reads the extraction and the glossary, and the only
// prompt it builds is the one it inspects.
SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace mgi::translate
