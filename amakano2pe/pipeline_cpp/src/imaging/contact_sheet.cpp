// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "imaging/contact_sheet.h"

#include "imaging/wic_image_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <stdexcept>

namespace ama::imaging {

namespace {

constexpr int kCheckerCellPx = 8;
constexpr BgraColor kCheckerLight{0xCC, 0xCC, 0xCC, kOpaqueAlpha};
constexpr BgraColor kCheckerDark{0x99, 0x99, 0x99, kOpaqueAlpha};
constexpr BgraColor kSheetBackground{0x30, 0x30, 0x30, kOpaqueAlpha};
constexpr BgraColor kLabelBackground{0x10, 0x10, 0x10, kOpaqueAlpha};
constexpr COLORREF kLabelTextColor = RGB(0xFF, 0xFF, 0xFF);
constexpr int kLabelFontPx = 14;
constexpr int kLabelTextInsetPx = 3;
constexpr int kPageNumberDigits = 2;
constexpr std::size_t kPageSuffixCapacity = 16;
constexpr double kMaxChannelValue = 255.0;
constexpr double kRoundingBias = 0.5;

int CeilDiv(int numerator, int denominator) {
    return (numerator + denominator - 1) / denominator;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int len =
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(len, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

// Owns a screen-compatible memory DC with a top-down 32bpp DIB selected.
class ScopedDibSurface {
public:
    ScopedDibSurface(int width, int height) {
        dc_ = CreateCompatibleDC(nullptr);
        if (dc_ == nullptr) {
            throw std::runtime_error("CreateCompatibleDC failed");
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = kBgraBytesPerPixel * CHAR_BIT;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (bitmap_ == nullptr || bits_ == nullptr) {
            DeleteDC(dc_);
            throw std::runtime_error("CreateDIBSection failed");
        }
        previous_ = SelectObject(dc_, bitmap_);
    }
    ScopedDibSurface(const ScopedDibSurface&) = delete;
    ScopedDibSurface& operator=(const ScopedDibSurface&) = delete;
    ~ScopedDibSurface() {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }

    HDC dc() const { return dc_; }
    std::uint8_t* bits() const { return static_cast<std::uint8_t*>(bits_); }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* bits_ = nullptr;
};

class ScopedFont {
public:
    explicit ScopedFont(int height_px) {
        font_ = CreateFontW(-height_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (font_ == nullptr) {
            throw std::runtime_error("CreateFontW failed");
        }
    }
    ScopedFont(const ScopedFont&) = delete;
    ScopedFont& operator=(const ScopedFont&) = delete;
    ~ScopedFont() { DeleteObject(font_); }
    HFONT get() const { return font_; }

private:
    HFONT font_ = nullptr;
};

void FillRect(BgraImage& image, const RectI& rect, BgraColor color) {
    const RectI r = ClampRect(rect, image.width, image.height);
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            SetPixel(image, x, y, color);
        }
    }
}

void FillChecker(BgraImage& image, const RectI& rect) {
    const RectI r = ClampRect(rect, image.width, image.height);
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            const bool light = (((x - r.x) / kCheckerCellPx) + ((y - r.y) / kCheckerCellPx)) % 2 == 0;
            SetPixel(image, x, y, light ? kCheckerLight : kCheckerDark);
        }
    }
}

std::uint8_t Blend(std::uint8_t src, std::uint8_t dst, double alpha) {
    return static_cast<std::uint8_t>(src * alpha + dst * (1.0 - alpha) + kRoundingBias);
}

void CompositeOver(BgraImage& sheet, const BgraImage& thumb, int dst_x, int dst_y) {
    for (int y = 0; y < thumb.height; ++y) {
        for (int x = 0; x < thumb.width; ++x) {
            const int sx = dst_x + x;
            const int sy = dst_y + y;
            if (sx < 0 || sy < 0 || sx >= sheet.width || sy >= sheet.height) {
                continue;
            }
            const BgraColor s = GetPixel(thumb, x, y);
            const BgraColor d = GetPixel(sheet, sx, sy);
            const double a = s.a / kMaxChannelValue;
            SetPixel(sheet, sx, sy,
                     BgraColor{Blend(s.b, d.b, a), Blend(s.g, d.g, a), Blend(s.r, d.r, a),
                               kOpaqueAlpha});
        }
    }
}

// Draws every label with GDI onto a DIB covering the whole sheet, then copies
// the label strips back, keeping the checkerboard cells untouched.
void DrawLabels(BgraImage& sheet, const std::vector<RectI>& strips,
                const std::vector<std::string>& labels) {
    ScopedDibSurface surface(sheet.width, sheet.height);
    std::copy(sheet.pixels.begin(), sheet.pixels.end(), surface.bits());
    const ScopedFont font(kLabelFontPx);
    const HGDIOBJ old_font = SelectObject(surface.dc(), font.get());
    SetBkMode(surface.dc(), TRANSPARENT);
    SetTextColor(surface.dc(), kLabelTextColor);
    for (std::size_t i = 0; i < strips.size(); ++i) {
        const std::wstring text = Utf8ToWide(labels[i]);
        RECT rc{strips[i].x + kLabelTextInsetPx, strips[i].y,
                strips[i].x + strips[i].w - kLabelTextInsetPx, strips[i].y + strips[i].h};
        DrawTextW(surface.dc(), text.c_str(), static_cast<int>(text.size()), &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    GdiFlush();
    SelectObject(surface.dc(), old_font);
    for (const RectI& strip : strips) {
        const RectI r = ClampRect(strip, sheet.width, sheet.height);
        for (int y = r.y; y < r.y + r.h; ++y) {
            const std::size_t o = PixelOffset(sheet, r.x, y);
            const std::size_t n = static_cast<std::size_t>(r.w) * kBgraBytesPerPixel;
            std::copy(surface.bits() + o, surface.bits() + o + n, sheet.pixels.begin() +
                                                                     static_cast<std::ptrdiff_t>(o));
            for (std::size_t k = o + kChannelAlpha; k < o + n; k += kBgraBytesPerPixel) {
                sheet.pixels[k] = kOpaqueAlpha;
            }
        }
    }
}

BgraImage Thumbnail(const BgraImage& image, int max_side) {
    const int longest = std::max(image.width, image.height);
    if (longest <= max_side || longest == 0) {
        return image;
    }
    const double scale = static_cast<double>(max_side) / longest;
    const int w = std::max(1, static_cast<int>(image.width * scale));
    const int h = std::max(1, static_cast<int>(image.height * scale));
    return ResizeImage(image, w, h);
}

}  // namespace

BgraImage RenderContactSheet(const std::vector<ContactSheetCell>& cells,
                             const ContactSheetLayout& layout) {
    if (layout.columns <= 0 || layout.max_cell_px <= 0) {
        throw std::invalid_argument("contact sheet layout needs positive columns and cell size");
    }
    const int count = static_cast<int>(cells.size());
    const int columns = std::max(1, std::min(layout.columns, count));
    const int rows = std::max(1, CeilDiv(count, columns));
    const int cell_w = layout.max_cell_px + layout.padding_px;
    const int cell_h = layout.max_cell_px + layout.label_height_px + layout.padding_px;
    BgraImage sheet = MakeFilledBgraImage(columns * cell_w + layout.padding_px,
                                          rows * cell_h + layout.padding_px, kSheetBackground);
    std::vector<RectI> strips;
    std::vector<std::string> labels;
    strips.reserve(cells.size());
    labels.reserve(cells.size());
    for (int i = 0; i < count; ++i) {
        const int x0 = layout.padding_px + (i % columns) * cell_w;
        const int y0 = layout.padding_px + (i / columns) * cell_h;
        const BgraImage thumb = Thumbnail(cells[static_cast<std::size_t>(i)].image,
                                          layout.max_cell_px);
        const int tx = x0 + (layout.max_cell_px - thumb.width) / 2;
        const int ty = y0 + (layout.max_cell_px - thumb.height) / 2;
        FillChecker(sheet, RectI{tx, ty, thumb.width, thumb.height});
        CompositeOver(sheet, thumb, tx, ty);
        const RectI strip{x0, y0 + layout.max_cell_px, layout.max_cell_px, layout.label_height_px};
        FillRect(sheet, strip, kLabelBackground);
        strips.push_back(strip);
        labels.push_back(cells[static_cast<std::size_t>(i)].label);
    }
    DrawLabels(sheet, strips, labels);
    return sheet;
}

std::vector<std::filesystem::path> SaveContactSheetPages(
    const std::vector<ContactSheetCell>& cells, const std::filesystem::path& directory,
    const std::string& stem, const ContactSheetLayout& layout) {
    if (layout.cells_per_page <= 0) {
        throw std::invalid_argument("contact sheet layout needs a positive page size");
    }
    std::filesystem::create_directories(directory);
    std::vector<std::filesystem::path> written;
    const std::size_t per_page = static_cast<std::size_t>(layout.cells_per_page);
    for (std::size_t start = 0, page = 1; start < cells.size(); start += per_page, ++page) {
        const std::size_t end = std::min(cells.size(), start + per_page);
        const std::vector<ContactSheetCell> slice(cells.begin() + static_cast<std::ptrdiff_t>(start),
                                                  cells.begin() + static_cast<std::ptrdiff_t>(end));
        char suffix[kPageSuffixCapacity];
        std::snprintf(suffix, sizeof(suffix), "_p%0*zu.png", kPageNumberDigits, page);
        const std::filesystem::path path = directory / std::filesystem::u8path(stem + suffix);
        SavePng(path, RenderContactSheet(slice, layout));
        written.push_back(path);
    }
    return written;
}

}  // namespace ama::imaging
