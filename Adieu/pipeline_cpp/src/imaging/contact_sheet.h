// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Checkerboard-composited visual QA contact sheets for single-image and
// multi-cut .g00 sprites, supporting both `Original | Patched | Amplified Diff`
// side-by-side comparison strips and multi-column overview grids.
//
// Thread-compatible.
#pragma once

#include "imaging/bgra_image.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace adi::imaging {

inline constexpr int kDefaultContactSheetMaxPanelPx = 320;
inline constexpr int kDefaultContactSheetRowsPerPage = 10;
inline constexpr int kDefaultContactSheetDiffAmp = 4;
inline constexpr int kDefaultCheckerCellPx = 8;
inline constexpr int kDefaultGridColumns = 4;
inline constexpr int kDefaultGridCellsPerPage = 24;

struct ContactSheetCell {
    std::string label;
    BgraImage image;
};

struct ContactSheetGridLayout {
    int columns = kDefaultGridColumns;
    int max_cell_px = kDefaultContactSheetMaxPanelPx;
    int checker_cell_px = kDefaultCheckerCellPx;
    int cells_per_page = kDefaultGridCellsPerPage;
};

struct ComparisonSheetOptions {
    int max_panel_px = kDefaultContactSheetMaxPanelPx;
    int rows_per_page = kDefaultContactSheetRowsPerPage;
    int diff_amplification = kDefaultContactSheetDiffAmp;
    int checker_cell_px = kDefaultCheckerCellPx;
};

// PIMPL-insulated builder accumulating `Original | Patched | Amplified Diff`
// visual inspection rows across modified sprite cuts.
class ContactSheetBuilder {
public:
    explicit ContactSheetBuilder(ComparisonSheetOptions options = {});
    ContactSheetBuilder(const ContactSheetBuilder&) = delete;
    ContactSheetBuilder& operator=(const ContactSheetBuilder&) = delete;
    ContactSheetBuilder(ContactSheetBuilder&& other) noexcept;
    ContactSheetBuilder& operator=(ContactSheetBuilder&& other) noexcept;
    ~ContactSheetBuilder();

    void AddComparison(std::string label, const BgraImage& original,
                       const BgraImage& patched);

    std::size_t row_count() const noexcept;
    bool empty() const noexcept;

    // Renders accumulated comparison rows into in-memory page images.
    std::vector<BgraImage> RenderPages() const;

    // Writes `<output_dir>/<file_stem>_p01.png`, `_p02.png`, ... via WicImageIo
    // and returns the written paths in page order.
    std::vector<std::filesystem::path> SavePages(
        const std::filesystem::path& output_dir, const std::string& file_stem) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Renders a multi-column labeled overview grid over an alpha checkerboard.
BgraImage RenderContactSheetGrid(const std::vector<ContactSheetCell>& cells,
                                 const ContactSheetGridLayout& layout = {});

std::vector<std::filesystem::path> SaveContactSheetGridPages(
    const std::vector<ContactSheetCell>& cells,
    const std::filesystem::path& output_dir, const std::string& file_stem,
    const ContactSheetGridLayout& layout = {});

}  // namespace adi::imaging
