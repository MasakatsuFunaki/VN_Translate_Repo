// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Five checks that prove speaker names reach the API prompt, run before a
// token is spent.  A broken speaker field produces plausible but wrong output.
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

namespace snu::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;  // 1..5; 0 when every check passed
    std::string reason;    // the failure in one line, empty on success

    std::size_t dialogue_sampled = 0;
    std::size_t dialogue_total = 0;
    std::size_t speakers_in_data = 0;   // distinct JP speakers the scripts use
    std::size_t glossary_entries = 0;   // rows in the speaker table
};

// Run the checks over an extracted document and print the report as it goes.
SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace snu::translate
