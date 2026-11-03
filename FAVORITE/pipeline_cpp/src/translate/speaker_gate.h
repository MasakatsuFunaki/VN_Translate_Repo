// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.
//
// The speaker gate: four checks that prove the cast still reaches the API
// prompt, run before a single token is spent.
//
// WHY THIS GATE IS DIFFERENT FROM THE OTHER GAMES'
// ================================================
// FAVORITE's FVP engine does NOT tag per-line speakers in the .hcb bytecode.
// Voice files are pre-declared globally at script load and bound to dialogues
// through runtime engine state that a static walker cannot see:
//   * the 0x10 face-graphics opcode encodes character indices but only fires
//     in "scene mode" -- 0/1362 narrative-mode dialogues have a 0x10 within
//     60 ops;
//   * all 1707 voice-file pushes live in the setup region (offset < 0xa0000);
//     none appear within 200 bytes of any dialogue;
//   * the proxy DLL handles name-window translation at render time but does
//     not expose a per-line speaker mapping.
//
// So this game's translation relies on Gemini inferring speakers from prose
// context, backed by the <character_dossier> in the system prompt.  There is
// no per-line [Speaker] tag to gate.  What can be gated is the cast roster,
// and the regression it catches is the one that costs the most and shows the
// least: a name drops out of the glossary or the dossier, the model stops
// recognising who is speaking, and the answers come back fluent, plausible
// and wrong in every voice.  The run is one-shot and the cache is never
// edited by hand, so that damage is permanent -- which is why the checks gate
// the translation step itself rather than sitting beside it as something to
// remember.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include "common/util.h"

namespace fav::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;  // 1..4; 0 when every check passed
    std::string reason;    // the failure in one line, empty on success

    std::size_t registry_names = 0;    // distinct cast names in the bytecode
    std::size_t glossary_entries = 0;  // rows in the name table
};

// Cast names the bytecode registers through the 0x10 -> 0x0E name-window
// pattern, with the number of times each was seen.  Exposed for the tests:
// check 1 is only meaningful if the walker still finds them.
std::map<std::string, int> cast_registry(const Bytes& hcb);

// Map dialogue 0x0E instruction offsets in Sakura.hcb to their canonical
// English speaker names from name_translations() whenever preceded by a call
// (0x02) to an unambiguous speaker subroutine.
std::unordered_map<std::uint32_t, std::string> map_line_speakers(const Bytes& hcb);

// Run the checks over the game's Sakura.hcb and print the report as it goes.
// Spends nothing: it reads the bytecode, the glossary and the system prompt.
SpeakerCheckReport run_speaker_checks(const Bytes& hcb);

}  // namespace fav::translate
