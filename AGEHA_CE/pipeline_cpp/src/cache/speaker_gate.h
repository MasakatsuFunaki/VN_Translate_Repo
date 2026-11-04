// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// The speaker gate: five checks that prove a speaker name reaches the
// API prompt, run before a single token is spent.
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

namespace exm::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;
    std::string reason;

    std::size_t dialogue_sampled = 0;
    std::size_t dialogue_total = 0;
    std::size_t speakers_in_data = 0;
    std::size_t glossary_entries = 0;
};

SpeakerCheckReport run_speaker_checks(const boost::json::object& extracted);

}  // namespace exm::translate
