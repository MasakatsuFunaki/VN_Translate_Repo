// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Replace the Japanese choice-box text in mushigurui choice-menu screenshots
// with English.
//
// This is a standalone one-shot tool over the *.jpg files sitting in the
// project's choices/ folder -- it never touches the game archives.
#pragma once

#include <string>
#include <vector>

#include "cg/image.h"

namespace mgi::choices {

struct Options {
    std::string project_dir;
};

int run_replace_choice_text(const Options& opt);

namespace detail {

struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Column-fraction heuristic over the blue/violet button pixels.  Pure integer
// arithmetic -- no float thresholds, so the box edges are reproducible.
std::vector<Box> detect_choice_boxes(const cg::Image& rgb);

// Wipe the yellow glyphs by tiling each row's non-yellow median across the
// button, then draw the English replacement.
cg::Image render_replacement(const cg::Image& img, const std::vector<Box>& boxes,
                             const std::vector<std::string>& texts,
                             const std::string& font_path);

// The first FONT_CANDIDATES entry that exists; "" when none do.
std::string resolve_font();

}  // namespace detail

}  // namespace mgi::choices
