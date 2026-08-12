// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The pipeline, in the order it runs.
//
// One list, read by the orchestrator for its banners and by the README for its
// table, so the sequence a person is told about and the sequence that executes
// cannot drift apart.  Each step is a command someone can also run by hand;
// the last one copies rather than launching anything, which is why its name is
// not an executable.
//
// The spine is extract -> translate -> deploy.  This game puts its English
// into the game's own files rather than into a table the DLL reads at run
// time, so the three steps that write those files -- the SPT repack, the .xtx
// name plates and the .fxf flowcharts -- sit between translation and deploy.
#pragma once

#include <array>
#include <cstddef>

namespace exc::apps {

struct PipelineStep {
    const char* name;  // the executable, or "deploy" for the in-process copy
    const char* what;  // the banner line, and what the README's table says
};

inline const std::array<PipelineStep, 6>& pipeline_steps() {
    static const std::array<PipelineStep, 6> steps = {{
        {"01_extract", "Extract text from the .spt scripts"},
        {"02_translate", "Speaker gate, then translate via Claude"},
        {"03_repack", "Word-wrap and write the English back into the .spt scripts"},
        {"04_translate_xtx", "Translate the .xtx config name plates"},
        {"05_translate_charts", "Translate the chart/*.fxf flowcharts"},
        {"deploy", "Deploy winmm.dll to the game folder"},
    }};
    return steps;
}

}  // namespace exc::apps
