// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pipeline step sequence — single source of truth for banners and the README.
#pragma once

#include <array>
#include <cstddef>

namespace frat::apps {

struct PipelineStep {
    const char* name;  // the executable, or "deploy" for the in-process copy
    const char* what;  // the banner line, and what the README's table says
};

inline const std::array<PipelineStep, 3>& pipeline_steps() {
    static const std::array<PipelineStep, 3> steps = {{
        {"01_extract", "Extract JP strings from the YPF archive"},
        {"02_translate", "Speaker gate, translate via Claude, build translation_table.tsv"},
        {"deploy", "Deploy winmm.dll to the game folder"},
    }};
    return steps;
}

}  // namespace frat::apps
