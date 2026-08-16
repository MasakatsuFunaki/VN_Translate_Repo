// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Narrative-CG asset translator: scan dwq/*.gpk for images carrying Japanese
// text, translate them via Claude vision, render the English over the original
// and repack the archives.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "image.h"
#include "text_render.h"

namespace mgi::cg {

struct CgOptions {
    std::string game_dir;
    std::string project_dir;
    bool scan_only = false;   // --scan-only
    bool repack_only = false; // --repack
    bool no_resume = false;   // --no-resume

    // --replay <jsonl>: verification seam.  Each line is one recorded
    // /v1/messages response body, popped in call order instead of hitting the
    // API, so a full scan/translate run can be reproduced offline and its JSON
    // outputs diffed, at zero token cost.
    std::string replay_file;
};

int run_find_narrative_cg(const CgOptions& opt);

// The pure, deterministic pieces of the pipeline.  Every real bug in this step
// lives in one of these, so they are exposed for the unit tests rather than
// buried in an anonymous namespace.
namespace detail {

extern const char* const FONT_PATH;      // Monotype Corsiva
extern const char* const FONT_FALLBACK;  // arial

// ---- resume-set coverage ----

// (archive, entry) -- the pair narrative_scanned.json records, and the key the
// scan skips on.
using ScanKey = std::pair<std::string, std::string>;
using ScanKeySet = std::set<ScanKey>;

// True while the resume set does not cover this entry.  The scan skips on
// exactly this question, so the count below cannot drift away from it.
bool needs_scan(const ScanKeySet& done, const ScanKey& key);

// How many targets the resume set leaves.  Zero means the scan sends nothing
// and therefore needs no API key.  This is an upper bound on the requests, not
// an exact count: the per-image decode and dimension checks run later and can
// still drop a target.
std::size_t count_left_to_scan(const std::vector<ScanKey>& targets,
                               const ScanKeySet& done);

// ---- translate-pass coverage ----

// The painted image a candidate records, as a path that is really there, or an
// empty string when neither location holds it.  `patched_dir` is the folder
// this run paints into.
//
// The recorded path is absolute and wins whenever it still resolves.  A
// candidates file written before the project folder moved records a root that
// no longer exists, so the same file name is looked for in `patched_dir` as
// well -- otherwise a moved checkout cannot see the images it already paid
// for, and paints and bills every one of them a second time.
std::string resolve_patched_path(const std::string& recorded,
                                 const std::string& patched_dir);

// True while the translate pass has not already painted this candidate: it
// needs both a region list and a patched image still on disk to count as done.
// The pass skips on exactly this question, so the count below cannot drift
// away from it.
bool needs_translation(const boost::json::object& candidate,
                       const std::string& patched_dir);

// How many candidates the translate pass would send.  Zero means it makes no
// request and therefore needs no API key.  A hand-painted entry is rendered
// locally and never counted -- the override table is empty in this game, so
// that clause is scaffolding for now.  Like the scan count this is an upper
// bound: a candidate whose image cannot be found or opened is dropped later,
// without a request.
std::size_t count_left_to_translate(const boost::json::array& candidates,
                                    const std::string& patched_dir);

std::string safe_name(const std::string& name);

// '' means "not an image entry" -- the extractor skips those outright.
std::string ext_for_type_tag(const std::string& type_tag);

// Non-greedy object scrape from a model response: first '{' to the FIRST '}'
// after it, newlines included.
std::optional<std::string> first_json_object_text(const std::string& s);

// Balanced-bracket walker over a model response (NOT first-'[' to last-']',
// which would swallow a trailing example array in the prose).
std::optional<std::string> balanced_json_array_text(const std::string& s);

// FLOOR division, not C++'s truncating /.  Both call sites can go negative
// (text taller/wider than its bbox), and truncation would shift the centred
// text by a pixel in exactly those cases.
int floor_div(int a, int b);

// Loose truthiness for `has_narrative_text`: the model returns it as a bool,
// a number or a string depending on the image.
bool is_truthy(const boost::json::value& v);

// `has_narrative_text` set AND `confidence` (default 0) >= 0.7.
// Throws when confidence is present but not a number -- a malformed field is
// caught and logged as a detect error rather than silently scoring 0.
bool detect_hit(const boost::json::object& data);

// Integer coercion for model-supplied coordinates: floats TRUNCATE TOWARD
// ZERO (they are not rounded -- 130.9 -> 130, -130.9 -> -130, so a bbox never
// grows past the edge the model named), an int-looking string is accepted,
// anything else throws.
int to_int(const boost::json::value& v);

// "#rrggbb" (leading '#' optional) -> RGBA, falling back to white on a dark
// background / near-black on a light one.  Alpha is always 255.
std::array<std::uint8_t, 4> parse_hex(const std::string& hex,
                                      const std::string& bg_tone);

// Median of the 2px border of the crop.  An even count averages the two middle
// values then truncates -- NOT nth_element, which returns the upper middle and
// would tint the inpaint towards the brighter side.
std::array<std::uint8_t, 3> sample_bg(const Image& img, int x1, int y1,
                                      int x2, int y2);

std::unique_ptr<Font> fit_font(const std::string& text, const std::string& font_path,
                               int max_w, int max_h);

std::vector<std::string> wrap_lines(const std::string& text, const Font& font,
                                    int max_w);

Image render_translation(const Image& img, const boost::json::array& regions);

}  // namespace detail

}  // namespace mgi::cg
