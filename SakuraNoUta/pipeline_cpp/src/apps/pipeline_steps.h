// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The pipeline, in the order it runs.  Single source of truth for the
// orchestrator's banners and the tests' order.
#pragma once

#include <array>
#include <cstddef>

namespace snu::apps {

struct PipelineStep {
    const char* name;  // the executable, or "deploy" for the in-process copy
    const char* what;  // the banner line, and what the README's table says
};

inline const std::array<PipelineStep, 3>& pipeline_steps() {
    static const std::array<PipelineStep, 3> steps = {{
        {"01_extract", "Extract text from BGI/Buriko .arc archives"},
        {"02_build_tsv", "Speaker gate, apply cached translations, build translations.tsv"},
        {"deploy", "Deploy winmm.dll and translations.tsv to the game folder"},
    }};
    return steps;
}

}  // namespace snu::apps
