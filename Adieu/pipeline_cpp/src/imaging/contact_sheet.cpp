// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "imaging/contact_sheet.h"

#include "imaging/wic_image_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace adi::imaging {
namespace {

constexpr BgraColor kPageBackground{0x18, 0x1A, 0x1F, 0xFF};
constexpr BgraColor kPanelBorder{0x44, 0x4A, 0x56, 0xFF};
constexpr BgraColor kCheckerDark{0x34, 0x37, 0x3E, 0xFF};
constexpr BgraColor kCheckerLight{0x50, 0x55, 0x5F, 0xFF};
constexpr BgraColor kCaptionColor{0xE8, 0xEC, 0xF2, 0xFF};
constexpr BgraColor kSubCaptionColor{0x9E, 0xAA, 0xBC, 0xFF};
constexpr BgraColor kDiffZeroPixel{0x1C, 0x1E, 0x24, 0xFF};

constexpr int kOuterPaddingPx = 12;
constexpr int kPanelGapPx = 10;
constexpr int kRowGapPx = 12;
constexpr int kHeaderHeightPx = 16;
constexpr int kPanelBorderPx = 1;
constexpr int kComparisonColumns = 3;
constexpr int kGlyphWidthPx = 5;
constexpr int kGlyphHeightPx = 7;
constexpr int kGlyphAdvancePx = 6;
constexpr int kChannelRoundingBias = 127;
constexpr int kChannelMaxInt = 255;
constexpr int kPageNumberDigits = 2;

// Compact 5x7 ASCII glyph bitmaps for codepoints 32..126 (row-major 5-bit rows).
constexpr std::array<std::array<std::uint8_t, kGlyphHeightPx>, 95> kFont5x7 = {{
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},  // ' '
    {{0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},  // '!'
    {{0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}},  // '"'
    {{0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},  // '#'
    {{0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}},  // '$'
    {{0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},  // '%'
    {{0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},  // '&'
    {{0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},  // '\''
    {{0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},  // '('
    {{0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},  // ')'
    {{0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},  // '*'
    {{0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},  // '+'
    {{0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},  // ','
    {{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},  // '-'
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},  // '.'
    {{0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},  // '/'
    {{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},  // '0'
    {{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},  // '1'
    {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},  // '2'
    {{0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},  // '3'
    {{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},  // '4'
    {{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},  // '5'
    {{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},  // '6'
    {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},  // '7'
    {{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},  // '8'
    {{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},  // '9'
    {{0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},  // ':'
    {{0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}},  // ';'
    {{0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},  // '<'
    {{0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},  // '='
    {{0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},  // '>'
    {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},  // '?'
    {{0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E}},  // '@'
    {{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},  // 'A'
    {{0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},  // 'B'
    {{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},  // 'C'
    {{0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},  // 'D'
    {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},  // 'E'
    {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},  // 'F'
    {{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},  // 'G'
    {{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},  // 'H'
    {{0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},  // 'I'
    {{0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},  // 'J'
    {{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},  // 'K'
    {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},  // 'L'
    {{0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},  // 'M'
    {{0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},  // 'N'
    {{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},  // 'O'
    {{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},  // 'P'
    {{0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},  // 'Q'
    {{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},  // 'R'
    {{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},  // 'S'
    {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},  // 'T'
    {{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},  // 'U'
    {{0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},  // 'V'
    {{0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},  // 'W'
    {{0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},  // 'X'
    {{0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},  // 'Y'
    {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},  // 'Z'
    {{0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},  // '['
    {{0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}},  // '\\'
    {{0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},  // ']'
    {{0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}},  // '^'
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},  // '_'
    {{0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00}},  // '`'
    {{0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},  // 'a'
    {{0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E}},  // 'b'
    {{0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}},  // 'c'
    {{0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F}},  // 'd'
    {{0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},  // 'e'
    {{0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}},  // 'f'
    {{0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}},  // 'g'
    {{0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}},  // 'h'
    {{0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},  // 'i'
    {{0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}},  // 'j'
    {{0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}},  // 'k'
    {{0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},  // 'l'
    {{0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}},  // 'm'
    {{0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}},  // 'n'
    {{0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},  // 'o'
    {{0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10}},  // 'p'
    {{0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01}},  // 'q'
    {{0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},  // 'r'
    {{0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}},  // 's'
    {{0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06}},  // 't'
    {{0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}},  // 'u'
    {{0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}},  // 'v'
    {{0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}},  // 'w'
    {{0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},  // 'x'
    {{0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E}},  // 'y'
    {{0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}},  // 'z'
    {{0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02}},  // '{'
    {{0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},  // '|'
    {{0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08}},  // '}'
    {{0x00, 0x08, 0x15, 0x02, 0x00, 0x00, 0x00}},  // '~'
}};

void DrawBitmapText5x7(BgraImage& dst, int x, int y, const std::string& text, BgraColor color,
                       int max_width_px) {
    int pen_x = x;
    for (const unsigned char uch : text) {
        if (pen_x + kGlyphWidthPx > x + max_width_px) {
            break;
        }
        const std::size_t glyph_idx =
            (uch >= 32u && uch <= 126u) ? static_cast<std::size_t>(uch - 32u)
                                        : static_cast<std::size_t>('?' - 32);
        const auto& rows = kFont5x7[glyph_idx];
        for (int gy = 0; gy < kGlyphHeightPx; ++gy) {
            const int py = y + gy;
            if (py < 0 || py >= dst.height()) {
                continue;
            }
            const std::uint8_t row_bits = rows[static_cast<std::size_t>(gy)];
            for (int gx = 0; gx < kGlyphWidthPx; ++gx) {
                if ((row_bits & (1u << (kGlyphWidthPx - 1 - gx))) == 0u) {
                    continue;
                }
                const int px = pen_x + gx;
                if (px >= 0 && px < dst.width()) {
                    dst.SetPixel(px, py, color);
                }
            }
        }
        pen_x += kGlyphAdvancePx;
    }
}

BgraImage FitToBox(const BgraImage& img, int max_box_px) {
    if (img.empty() || max_box_px <= 0) {
        return BgraImage(1, 1, BgraColor{0, 0, 0, 0});
    }
    if (img.width() <= max_box_px && img.height() <= max_box_px) {
        return img;
    }
    const double scale = std::min(static_cast<double>(max_box_px) / static_cast<double>(img.width()),
                                  static_cast<double>(max_box_px) / static_cast<double>(img.height()));
    const int nw = std::max(1, static_cast<int>(std::lround(static_cast<double>(img.width()) * scale)));
    const int nh = std::max(1, static_cast<int>(std::lround(static_cast<double>(img.height()) * scale)));
    return img.ResizeAreaPremultiplied(nw, nh);
}

void DrawPanelOverChecker(BgraImage& dst, int panel_x, int panel_y, int panel_w, int panel_h,
                          const BgraImage& sprite, int checker_cell_px) {
    const int cell = std::max(1, checker_cell_px);
    dst.FillRect(RectI{panel_x - kPanelBorderPx, panel_y - kPanelBorderPx,
                       panel_w + 2 * kPanelBorderPx, panel_h + 2 * kPanelBorderPx},
                 kPanelBorder);

    for (int ry = 0; ry < panel_h; ++ry) {
        const int py = panel_y + ry;
        if (py < 0 || py >= dst.height()) {
            continue;
        }
        for (int rx = 0; rx < panel_w; ++rx) {
            const int px = panel_x + rx;
            if (px < 0 || px >= dst.width()) {
                continue;
            }
            const bool even = (((rx / cell) + (ry / cell)) & 1) == 0;
            dst.SetPixel(px, py, even ? kCheckerDark : kCheckerLight);
        }
    }

    const int ox = panel_x + (panel_w - sprite.width()) / 2;
    const int oy = panel_y + (panel_h - sprite.height()) / 2;
    for (int sy = 0; sy < sprite.height(); ++sy) {
        const int py = oy + sy;
        if (py < panel_y || py >= panel_y + panel_h || py < 0 || py >= dst.height()) {
            continue;
        }
        for (int sx = 0; sx < sprite.width(); ++sx) {
            const int px = ox + sx;
            if (px < panel_x || px >= panel_x + panel_w || px < 0 || px >= dst.width()) {
                continue;
            }
            const BgraColor s = sprite.PixelAt(sx, sy);
            if (s.a == 0u) {
                continue;
            }
            if (s.a == kOpaqueAlpha) {
                dst.SetPixel(px, py, s);
                continue;
            }
            const BgraColor bg = dst.PixelAt(px, py);
            const int a = static_cast<int>(s.a);
            const int inv = kChannelMaxInt - a;
            dst.SetPixel(px, py,
                         BgraColor{
                             static_cast<std::uint8_t>(
                                 (static_cast<int>(s.b) * a + static_cast<int>(bg.b) * inv +
                                  kChannelRoundingBias) /
                                 kChannelMaxInt),
                             static_cast<std::uint8_t>(
                                 (static_cast<int>(s.g) * a + static_cast<int>(bg.g) * inv +
                                  kChannelRoundingBias) /
                                 kChannelMaxInt),
                             static_cast<std::uint8_t>(
                                 (static_cast<int>(s.r) * a + static_cast<int>(bg.r) * inv +
                                  kChannelRoundingBias) /
                                 kChannelMaxInt),
                             kOpaqueAlpha,
                         });
        }
    }
}

struct DiffBuildResult {
    BgraImage image;
    std::size_t diff_pixels = 0u;
    int max_channel_diff = 0;
};

DiffBuildResult BuildAmplifiedDiff(const BgraImage& orig, const BgraImage& patched,
                                   int amplification) {
    const int w = std::max({1, orig.width(), patched.width()});
    const int h = std::max({1, orig.height(), patched.height()});
    const int amp = std::max(1, amplification);
    DiffBuildResult res{BgraImage(w, h, kDiffZeroPixel), 0u, 0};

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const BgraColor o =
                (x < orig.width() && y < orig.height()) ? orig.PixelAt(x, y) : BgraColor{};
            const BgraColor p =
                (x < patched.width() && y < patched.height()) ? patched.PixelAt(x, y) : BgraColor{};
            const int db = std::abs(static_cast<int>(o.b) - static_cast<int>(p.b));
            const int dg = std::abs(static_cast<int>(o.g) - static_cast<int>(p.g));
            const int dr = std::abs(static_cast<int>(o.r) - static_cast<int>(p.r));
            const int da = std::abs(static_cast<int>(o.a) - static_cast<int>(p.a));
            const int max_ch = std::max({db, dg, dr, da});
            if (max_ch == 0) {
                continue;
            }
            ++res.diff_pixels;
            res.max_channel_diff = std::max(res.max_channel_diff, max_ch);
            const int scaled = std::min(kChannelMaxInt, max_ch * amp);
            const int alpha_scaled = std::min(kChannelMaxInt, da * amp);
            res.image.SetPixel(x, y,
                               BgraColor{
                                   static_cast<std::uint8_t>(alpha_scaled),
                                   static_cast<std::uint8_t>(scaled / 3),
                                   static_cast<std::uint8_t>(scaled),
                                   kOpaqueAlpha,
                               });
        }
    }
    return res;
}

std::string FormatPageStem(const std::string& file_stem, std::size_t page_index_one_based) {
    std::ostringstream oss;
    oss << file_stem << "_p" << std::setw(kPageNumberDigits) << std::setfill('0')
        << page_index_one_based << ".png";
    return oss.str();
}

struct ComparisonEntry {
    std::string label;
    BgraImage original;
    BgraImage patched;
};

}  // namespace

struct ContactSheetBuilder::Impl {
    ComparisonSheetOptions options;
    std::vector<ComparisonEntry> rows;

    explicit Impl(ComparisonSheetOptions opts) : options(opts) {}
};

ContactSheetBuilder::ContactSheetBuilder(ComparisonSheetOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

ContactSheetBuilder::ContactSheetBuilder(ContactSheetBuilder&& other) noexcept = default;
ContactSheetBuilder& ContactSheetBuilder::operator=(ContactSheetBuilder&& other) noexcept = default;
ContactSheetBuilder::~ContactSheetBuilder() = default;

void ContactSheetBuilder::AddComparison(std::string label, const BgraImage& original,
                                        const BgraImage& patched) {
    impl_->rows.push_back(ComparisonEntry{std::move(label), original, patched});
}

std::size_t ContactSheetBuilder::row_count() const noexcept { return impl_->rows.size(); }

bool ContactSheetBuilder::empty() const noexcept { return impl_->rows.empty(); }

std::vector<BgraImage> ContactSheetBuilder::RenderPages() const {
    std::vector<BgraImage> pages;
    if (impl_->rows.empty()) {
        return pages;
    }

    const int max_panel = std::max(16, impl_->options.max_panel_px);
    const int rows_per_page = std::max(1, impl_->options.rows_per_page);
    const std::size_t total_rows = impl_->rows.size();

    for (std::size_t start = 0; start < total_rows;
         start += static_cast<std::size_t>(rows_per_page)) {
        const std::size_t end =
            std::min(total_rows, start + static_cast<std::size_t>(rows_per_page));

        struct PreparedRow {
            std::string caption;
            BgraImage orig_fit;
            BgraImage patched_fit;
            BgraImage diff_fit;
            int panel_w = 1;
            int panel_h = 1;
        };

        std::vector<PreparedRow> prepared;
        prepared.reserve(end - start);
        int col_panel_w = 64;
        int total_h = kOuterPaddingPx * 2 + kHeaderHeightPx;

        for (std::size_t i = start; i < end; ++i) {
            const ComparisonEntry& entry = impl_->rows[i];
            const DiffBuildResult diff =
                BuildAmplifiedDiff(entry.original, entry.patched, impl_->options.diff_amplification);

            PreparedRow pr;
            pr.orig_fit = FitToBox(entry.original, max_panel);
            pr.patched_fit = FitToBox(entry.patched, max_panel);
            pr.diff_fit = FitToBox(diff.image, max_panel);
            pr.panel_w = std::max({pr.orig_fit.width(), pr.patched_fit.width(), pr.diff_fit.width()});
            pr.panel_h =
                std::max({pr.orig_fit.height(), pr.patched_fit.height(), pr.diff_fit.height()});
            col_panel_w = std::max(col_panel_w, pr.panel_w);

            std::ostringstream cap;
            cap << entry.label << "  [" << entry.original.width() << "x" << entry.original.height()
                << " -> " << entry.patched.width() << "x" << entry.patched.height()
                << ", diff_px=" << diff.diff_pixels << ", max_ch=" << diff.max_channel_diff << "]";
            pr.caption = cap.str();

            total_h += kHeaderHeightPx + pr.panel_h + 2 * kPanelBorderPx + kRowGapPx;
            prepared.push_back(std::move(pr));
        }

        const int page_w = kOuterPaddingPx * 2 +
                           kComparisonColumns * (col_panel_w + 2 * kPanelBorderPx) +
                           (kComparisonColumns - 1) * kPanelGapPx;
        BgraImage page(page_w, total_h, kPageBackground);

        const int col_stride = col_panel_w + 2 * kPanelBorderPx + kPanelGapPx;
        DrawBitmapText5x7(page, kOuterPaddingPx, kOuterPaddingPx, "ORIGINAL", kSubCaptionColor,
                          col_panel_w);
        DrawBitmapText5x7(page, kOuterPaddingPx + col_stride, kOuterPaddingPx, "PATCHED",
                          kSubCaptionColor, col_panel_w);
        DrawBitmapText5x7(page, kOuterPaddingPx + 2 * col_stride, kOuterPaddingPx,
                          "AMPLIFIED DIFF", kSubCaptionColor, col_panel_w);

        int cursor_y = kOuterPaddingPx + kHeaderHeightPx;
        for (const PreparedRow& pr : prepared) {
            DrawBitmapText5x7(page, kOuterPaddingPx, cursor_y + 2, pr.caption, kCaptionColor,
                              page_w - 2 * kOuterPaddingPx);
            cursor_y += kHeaderHeightPx + kPanelBorderPx;
            DrawPanelOverChecker(page, kOuterPaddingPx + kPanelBorderPx, cursor_y, col_panel_w,
                                 pr.panel_h, pr.orig_fit, impl_->options.checker_cell_px);
            DrawPanelOverChecker(page, kOuterPaddingPx + col_stride + kPanelBorderPx, cursor_y,
                                 col_panel_w, pr.panel_h, pr.patched_fit,
                                 impl_->options.checker_cell_px);
            DrawPanelOverChecker(page, kOuterPaddingPx + 2 * col_stride + kPanelBorderPx, cursor_y,
                                 col_panel_w, pr.panel_h, pr.diff_fit,
                                 impl_->options.checker_cell_px);
            cursor_y += pr.panel_h + kPanelBorderPx + kRowGapPx;
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

std::vector<std::filesystem::path> ContactSheetBuilder::SavePages(
    const std::filesystem::path& output_dir, const std::string& file_stem) const {
    const std::vector<BgraImage> pages = RenderPages();
    std::vector<std::filesystem::path> written;
    if (pages.empty()) {
        return written;
    }
    std::filesystem::create_directories(output_dir);
    const WicImageIo io;
    written.reserve(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const std::filesystem::path path = output_dir / FormatPageStem(file_stem, i + 1u);
        io.SavePngFile(path, pages[i]);
        written.push_back(path);
    }
    return written;
}

BgraImage RenderContactSheetGrid(const std::vector<ContactSheetCell>& cells,
                                 const ContactSheetGridLayout& layout) {
    if (cells.empty()) {
        return BgraImage{};
    }
    const int cols = std::max(1, layout.columns);
    const int max_cell = std::max(16, layout.max_cell_px);
    const int rows = static_cast<int>((cells.size() + static_cast<std::size_t>(cols) - 1u) /
                                      static_cast<std::size_t>(cols));

    std::vector<BgraImage> fitted;
    fitted.reserve(cells.size());
    int cell_w = 64;
    int cell_h = 32;
    for (const ContactSheetCell& cell : cells) {
        BgraImage fit = FitToBox(cell.image, max_cell);
        cell_w = std::max(cell_w, fit.width());
        cell_h = std::max(cell_h, fit.height());
        fitted.push_back(std::move(fit));
    }

    const int col_stride = cell_w + 2 * kPanelBorderPx + kPanelGapPx;
    const int row_stride = kHeaderHeightPx + cell_h + 2 * kPanelBorderPx + kRowGapPx;
    const int page_w = kOuterPaddingPx * 2 + cols * col_stride - kPanelGapPx;
    const int page_h = kOuterPaddingPx * 2 + rows * row_stride - kRowGapPx;

    BgraImage sheet(page_w, page_h, kPageBackground);
    for (std::size_t idx = 0; idx < cells.size(); ++idx) {
        const int r = static_cast<int>(idx) / cols;
        const int c = static_cast<int>(idx) % cols;
        const int x0 = kOuterPaddingPx + c * col_stride;
        const int y0 = kOuterPaddingPx + r * row_stride;
        DrawBitmapText5x7(sheet, x0, y0 + 2, cells[idx].label, kCaptionColor, cell_w);
        DrawPanelOverChecker(sheet, x0 + kPanelBorderPx, y0 + kHeaderHeightPx + kPanelBorderPx,
                             cell_w, cell_h, fitted[idx], layout.checker_cell_px);
    }
    return sheet;
}

std::vector<std::filesystem::path> SaveContactSheetGridPages(
    const std::vector<ContactSheetCell>& cells, const std::filesystem::path& output_dir,
    const std::string& file_stem, const ContactSheetGridLayout& layout) {
    std::vector<std::filesystem::path> written;
    if (cells.empty()) {
        return written;
    }
    const std::size_t per_page = static_cast<std::size_t>(std::max(1, layout.cells_per_page));
    std::filesystem::create_directories(output_dir);
    const WicImageIo io;

    std::size_t page_no = 1u;
    for (std::size_t start = 0; start < cells.size(); start += per_page) {
        const std::size_t end = std::min(cells.size(), start + per_page);
        const std::vector<ContactSheetCell> slice(cells.begin() + static_cast<std::ptrdiff_t>(start),
                                                  cells.begin() + static_cast<std::ptrdiff_t>(end));
        const BgraImage page = RenderContactSheetGrid(slice, layout);
        const std::filesystem::path path = output_dir / FormatPageStem(file_stem, page_no++);
        io.SavePngFile(path, page);
        written.push_back(path);
    }
    return written;
}

}  // namespace adi::imaging
