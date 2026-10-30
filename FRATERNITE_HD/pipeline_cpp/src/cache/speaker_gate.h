// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pre-translation gate: five checks that speaker names reach the API prompt.
// Spends zero tokens — the user prompt is reconstructed locally.
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

SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace frat::translate
