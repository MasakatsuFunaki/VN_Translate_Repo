// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The pipeline, in the order it runs.
//
// One list, read by the orchestrator for its banners and by the tests for the
// order, so the sequence a person is told about and the sequence that executes
// cannot drift apart.  Each step is a command someone can also run by hand;
// the last one copies rather than launching anything, which is why its name is
// not an executable.
#pragma once

#include <array>
#include <cstddef>

namespace fav::apps {

inline constexpr std::size_t kPipelineStepCount = 4;

struct PipelineStep {
    const char* name;  // the executable, or "deploy" for the in-process copy
    const char* what;  // the banner line, and what the README's table says
};

inline const std::array<PipelineStep, kPipelineStepCount>& pipeline_steps() {
    static const std::array<PipelineStep, kPipelineStepCount> steps = {{
        {"01_extract", "Extract strings from Sakura.hcb"},
        {"02_translate", "Cast gate, translate via Gemini, build translation_table.tsv"},
        {"03_patch_ui", "Patch Options & System UI textures in graph.bin"},
        {"deploy", "Deploy winmm.dll to the game folder"},
    }};
    return steps;
}

}  // namespace fav::apps
