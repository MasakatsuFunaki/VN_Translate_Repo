// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// The pipeline steps in execution order.
#pragma once

#include <array>
#include <cstddef>

namespace exm::apps {

constexpr std::size_t kPipelineStepCount = 3;

struct PipelineStep {
    const char* name;
    const char* what;
};

inline const std::array<PipelineStep, kPipelineStepCount>& pipeline_steps() {
    static const std::array<PipelineStep, kPipelineStepCount> steps = {{
        {"01_extract", "Extract text from BLACKCyc .spt script files"},
        {"02_build_tsv",
         "Speaker gate, translate script, build translation_table.tsv"},
        {"deploy", "Deploy winmm.dll to the game folder"},
    }};
    return steps;
}

}  // namespace exm::apps
