// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// FreeType text measurement + rendering for the narrative-CG pipeline.
// y is the top of the ascender, not the baseline.
#pragma once

#include <array>
#include <memory>
#include <string>

#include "image.h"

namespace snu::cg {

struct TextBBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

class Font {
public:
    // Loads path at the given pixel size; returns nullptr on failure.
    static std::unique_ptr<Font> load(const std::string& path, int pixel_size);
    ~Font();

    // Ink bbox of a single line, relative to a (0,0) top-of-ascender origin.
    TextBBox measure(const std::string& utf8) const;

    // Draw a single line at (x, y), y being the top of the ascender.
    void draw(Image& img, int x, int y, const std::string& utf8,
              std::array<std::uint8_t, 3> color) const;

    int pixel_size() const { return pixel_size_; }

private:
    Font() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int pixel_size_ = 0;
};

}  // namespace snu::cg
