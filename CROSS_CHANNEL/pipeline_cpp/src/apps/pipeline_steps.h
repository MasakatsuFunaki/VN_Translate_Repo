// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The pipeline, in the order it runs.
//
// One list, read by the orchestrator for its banners, so the sequence a person
// is told about and the sequence that executes cannot drift apart.  Each step
// is a command someone can also run by hand; the last one copies rather than
// launching anything, which is why its name is not an executable.
#pragma once

#include <array>
#include <cstddef>

namespace crc::apps {

struct PipelineStep {
    const char* name;  // the executable, or "deploy" for the in-process copy
    const char* what;  // the banner line, and what the README's table says
};

inline const std::array<PipelineStep, 3>& pipeline_steps() {
    static const std::array<PipelineStep, 3> steps = {{
        {"01_extract", "Extract text from the WillPlus sn.bin script archive"},
        {"02_translate", "Speaker gate, translate via Claude, build translations.tsv"},
        {"deploy", "Deploy xinput1_3.dll and translations.tsv to the game folder"},
    }};
    return steps;
}

}  // namespace crc::apps
