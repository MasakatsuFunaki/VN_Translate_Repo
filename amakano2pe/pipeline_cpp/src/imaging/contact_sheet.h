// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Labelled thumbnail grids ("contact sheets") for visual inspection of
// sprite frames.  Each cell shows the image over a checkerboard, so
// transparency is visible, with a caption strip underneath.
#pragma once

#include "imaging/bgra_image.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ama::imaging {

struct ContactSheetCell {
    std::string label;  // UTF-8 caption
    BgraImage image;
};

struct ContactSheetLayout {
    int columns = 6;
    int max_cell_px = 300;    // longest thumbnail side; images are never upscaled
    int label_height_px = 20;
    int padding_px = 6;
    int cells_per_page = 36;  // cells per output PNG
};

// Renders `cells` into a single grid image (every cell, ignoring
// `cells_per_page`).
BgraImage RenderContactSheet(const std::vector<ContactSheetCell>& cells,
                             const ContactSheetLayout& layout);

// Renders `cells` into `<directory>/<stem>_pNN.png` pages of at most
// `layout.cells_per_page` cells each and returns the written paths in order.
std::vector<std::filesystem::path> SaveContactSheetPages(
    const std::vector<ContactSheetCell>& cells, const std::filesystem::path& directory,
    const std::string& stem, const ContactSheetLayout& layout);

}  // namespace ama::imaging
