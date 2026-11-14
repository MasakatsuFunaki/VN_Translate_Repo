// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/sprite_canvas.h"

#include "imaging/wic_image_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#ifdef DrawText
#  undef DrawText
#endif

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ama::menu {

namespace {

constexpr int kSupersampleScale = 3;
constexpr float kSupersampleScaleF = 3.0F;
constexpr float kInvSupersampleArea = 1.0F / static_cast<float>(kSupersampleScale * kSupersampleScale);
constexpr int kSentinelPixels1x = 1;

constexpr int kPillSubdivisions = 4;
constexpr double kPillSubsampleStep = 2.0;
constexpr double kPillSubsampleDenom = 2.0 * static_cast<double>(kPillSubdivisions);
constexpr double kInvPillSubsampleCount =
    1.0 / static_cast<double>(kPillSubdivisions * kPillSubdivisions);

constexpr float kChannelMaxF = 255.0F;
constexpr float kInvChannelMaxF = 1.0F / kChannelMaxF;
constexpr double kChannelMaxD = 255.0;
constexpr double kInvChannelMaxD = 1.0 / kChannelMaxD;
constexpr float kRoundingBiasF = 0.5F;
constexpr double kRoundingBiasD = 0.5;
constexpr double kHalfD = 0.5;
constexpr float kHalfF = 0.5F;
constexpr float kUnitF = 1.0F;
constexpr float kMinAlphaF = 1.0e-6F;
constexpr double kMinAlphaD = 1.0e-7;

constexpr int kSinglePlane = 1;
constexpr COLORREF kGlyphWhiteColor = RGB(0xFF, 0xFF, 0xFF);
constexpr const wchar_t* kDefaultFontFamily = L"Segoe UI";
constexpr int kLuminanceRedWeight = 1;
constexpr int kLuminanceGreenWeight = 2;
constexpr int kLuminanceBlueWeight = 1;
constexpr int kLuminanceWeightSum =
    kLuminanceRedWeight + kLuminanceGreenWeight + kLuminanceBlueWeight;
constexpr int kLuminanceRoundingBias = kLuminanceWeightSum / 2;

constexpr wchar_t kCarriageReturn = L'\r';
constexpr wchar_t kLineFeed = L'\n';
constexpr wchar_t kSpaceChar = L' ';
constexpr std::uint32_t kHighSurrogateMin = 0xD800U;
constexpr std::uint32_t kHighSurrogateMax = 0xDBFFU;
constexpr std::uint32_t kLowSurrogateMin = 0xDC00U;
constexpr std::uint32_t kLowSurrogateMax = 0xDFFFU;
constexpr unsigned int kPairShiftBits = 16U;

constexpr float kMinAllowedFactor = 0.05F;
constexpr float kFitStepFactor = 0.025F;
constexpr float kMaxNaturalLineRatio = 1.25F;
constexpr float kSwashFontHeightRatio = 1.55F;
constexpr float kShadowSafeBlurFactor = 0.5F;
constexpr int kBlurPassCount = 3;
constexpr float kBlurRadiusBias = 0.5F;
constexpr int kBlurKernelSideCount = 2;
constexpr float kStrokeDiskMargin = 1.0F;

constexpr int kEvenDivisor = 2;
constexpr int kEvenMedianBias = 1;
constexpr float kDiagonalInpaintWeight = 0.70710678F;
constexpr float kSorOmega = 1.70F;
constexpr int kSorIterations = 80;
constexpr int kJacobiPolishIterations = 16;
constexpr int kBorderRingPadding = 1;
constexpr int kBorderRingSpanAddend = 2;

struct NeighborOffset {
    int dx = 0;
    int dy = 0;
    float weight = 0.0F;
};

constexpr std::array<NeighborOffset, 8> kEightNeighbors{{
    {-1, 0, kUnitF},
    {1, 0, kUnitF},
    {0, -1, kUnitF},
    {0, 1, kUnitF},
    {-1, -1, kDiagonalInpaintWeight},
    {1, -1, kDiagonalInpaintWeight},
    {-1, 1, kDiagonalInpaintWeight},
    {1, 1, kDiagonalInpaintWeight},
}};

constexpr std::array<std::pair<int, int>, 4> kCardinalNeighbors{{
    {-1, 0},
    {1, 0},
    {0, -1},
    {0, 1},
}};

std::uint8_t ToByteF(float value) {
    const float clamped = std::clamp(value, 0.0F, kChannelMaxF);
    return static_cast<std::uint8_t>(clamped + kRoundingBiasF);
}

std::uint8_t ToByteD(double value) {
    const double clamped = std::clamp(value, 0.0, kChannelMaxD);
    return static_cast<std::uint8_t>(clamped + kRoundingBiasD);
}

// Straight-alpha source-over given a premultiplied source sample (pm_*, src_a in [0, 1]).
BgraColor CompositePremulOverStraight(BgraColor dst, float pm_b, float pm_g, float pm_r,
                                      float src_a) {
    const float clamped_src_a = std::clamp(src_a, 0.0F, kUnitF);
    if (clamped_src_a <= kMinAlphaF) {
        return dst;
    }
    const float dst_a = static_cast<float>(dst.a) * kInvChannelMaxF;
    const float inv_src_a = kUnitF - clamped_src_a;
    const float out_a = clamped_src_a + dst_a * inv_src_a;
    if (out_a <= kMinAlphaF) {
        return BgraColor{};
    }
    const float dst_weight = dst_a * inv_src_a;
    const float out_b = (pm_b * kChannelMaxF + static_cast<float>(dst.b) * dst_weight) / out_a;
    const float out_g = (pm_g * kChannelMaxF + static_cast<float>(dst.g) * dst_weight) / out_a;
    const float out_r = (pm_r * kChannelMaxF + static_cast<float>(dst.r) * dst_weight) / out_a;
    return BgraColor{ToByteF(out_b), ToByteF(out_g), ToByteF(out_r),
                     ToByteF(out_a * kChannelMaxF)};
}

BgraColor CompositeStraightOverStraight(BgraColor dst, BgraColor src) {
    if (src.a == 0) {
        return dst;
    }
    if (src.a == imaging::kOpaqueAlpha) {
        return src;
    }
    const float src_a = static_cast<float>(src.a) * kInvChannelMaxF;
    return CompositePremulOverStraight(
        dst, static_cast<float>(src.b) * kInvChannelMaxF * src_a,
        static_cast<float>(src.g) * kInvChannelMaxF * src_a,
        static_cast<float>(src.r) * kInvChannelMaxF * src_a, src_a);
}

struct ClippedTransfer {
    int src_x = 0;
    int src_y = 0;
    int dst_x = 0;
    int dst_y = 0;
    int w = 0;
    int h = 0;
};

ClippedTransfer ClipTransfer(int src_w, int src_h, const RectI& src_rect, int dst_w, int dst_h,
                             int dst_x, int dst_y) {
    int sx = src_rect.x;
    int sy = src_rect.y;
    int dx = dst_x;
    int dy = dst_y;
    int w = src_rect.w;
    int h = src_rect.h;
    if (w <= 0 || h <= 0) {
        return {};
    }
    if (sx < 0) {
        const int cut = -sx;
        sx = 0;
        dx += cut;
        w -= cut;
    }
    if (sy < 0) {
        const int cut = -sy;
        sy = 0;
        dy += cut;
        h -= cut;
    }
    if (dx < 0) {
        const int cut = -dx;
        dx = 0;
        sx += cut;
        w -= cut;
    }
    if (dy < 0) {
        const int cut = -dy;
        dy = 0;
        sy += cut;
        h -= cut;
    }
    if (w <= 0 || h <= 0 || sx >= src_w || sy >= src_h || dx >= dst_w || dy >= dst_h) {
        return {};
    }
    w = std::min({w, src_w - sx, dst_w - dx});
    h = std::min({h, src_h - sy, dst_h - dy});
    if (w <= 0 || h <= 0) {
        return {};
    }
    return ClippedTransfer{sx, sy, dx, dy, w, h};
}

std::uint8_t ComputeChannelMedian(std::vector<std::uint8_t>& samples) {
    const std::size_t mid = samples.size() / static_cast<std::size_t>(kEvenDivisor);
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(mid),
                     samples.end());
    const int upper = samples[mid];
    if (samples.size() % static_cast<std::size_t>(kEvenDivisor) != 0) {
        return static_cast<std::uint8_t>(upper);
    }
    const auto max_it =
        std::max_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(mid));
    const int lower = *max_it;
    return static_cast<std::uint8_t>((lower + upper + kEvenMedianBias) / kEvenDivisor);
}

struct PremulPixel {
    float b = 0.0F;
    float g = 0.0F;
    float r = 0.0F;
    float a = 0.0F;
};

// Inpaints unknown pixels inside `work_rect` by onion-peeling inward from known
// pixels in premultiplied colour space, followed by SOR + Jacobi harmonic smoothing.
void InpaintWorkRegion(BgraImage& image, const RectI& work_rect,
                       std::vector<std::uint8_t> unknown) {
    const int gw = work_rect.w;
    const int gh = work_rect.h;
    const std::size_t total = static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh);
    if (total == 0) {
        return;
    }

    std::vector<PremulPixel> grid(total);
    std::vector<int> all_unknown;
    all_unknown.reserve(total);
    bool any_known = false;

    for (int ly = 0; ly < gh; ++ly) {
        for (int lx = 0; lx < gw; ++lx) {
            const std::size_t idx =
                static_cast<std::size_t>(ly) * static_cast<std::size_t>(gw) +
                static_cast<std::size_t>(lx);
            if (unknown[idx] != 0) {
                all_unknown.push_back(static_cast<int>(idx));
                continue;
            }
            any_known = true;
            const BgraColor c = imaging::GetPixel(image, work_rect.x + lx, work_rect.y + ly);
            const float a = static_cast<float>(c.a) * kInvChannelMaxF;
            grid[idx] = PremulPixel{static_cast<float>(c.b) * kInvChannelMaxF * a,
                                    static_cast<float>(c.g) * kInvChannelMaxF * a,
                                    static_cast<float>(c.r) * kInvChannelMaxF * a, a};
        }
    }

    if (!any_known || all_unknown.empty()) {
        return;
    }

    std::vector<int> remaining = all_unknown;
    std::vector<std::pair<int, PremulPixel>> wave;
    std::vector<int> next_remaining;
    wave.reserve(remaining.size());
    next_remaining.reserve(remaining.size());

    while (!remaining.empty()) {
        wave.clear();
        next_remaining.clear();
        for (const int idx : remaining) {
            const int lx = idx % gw;
            const int ly = idx / gw;
            PremulPixel acc{};
            float weight_sum = 0.0F;
            for (const NeighborOffset& nb : kEightNeighbors) {
                const int nx = lx + nb.dx;
                const int ny = ly + nb.dy;
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) {
                    continue;
                }
                const std::size_t nidx =
                    static_cast<std::size_t>(ny) * static_cast<std::size_t>(gw) +
                    static_cast<std::size_t>(nx);
                if (unknown[nidx] != 0) {
                    continue;
                }
                acc.b += grid[nidx].b * nb.weight;
                acc.g += grid[nidx].g * nb.weight;
                acc.r += grid[nidx].r * nb.weight;
                acc.a += grid[nidx].a * nb.weight;
                weight_sum += nb.weight;
            }
            if (weight_sum <= 0.0F) {
                next_remaining.push_back(idx);
                continue;
            }
            const float inv_w = kUnitF / weight_sum;
            wave.emplace_back(
                idx, PremulPixel{acc.b * inv_w, acc.g * inv_w, acc.r * inv_w, acc.a * inv_w});
        }
        if (wave.empty()) {
            break;
        }
        for (const auto& [idx, val] : wave) {
            grid[static_cast<std::size_t>(idx)] = val;
            unknown[static_cast<std::size_t>(idx)] = 0;
        }
        remaining.swap(next_remaining);
    }

    for (int iter = 0; iter < kSorIterations; ++iter) {
        for (const int idx : all_unknown) {
            const int lx = idx % gw;
            const int ly = idx / gw;
            PremulPixel acc{};
            int count = 0;
            for (const auto& [dx, dy] : kCardinalNeighbors) {
                const int nx = lx + dx;
                const int ny = ly + dy;
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) {
                    continue;
                }
                const PremulPixel& nb =
                    grid[static_cast<std::size_t>(ny) * static_cast<std::size_t>(gw) +
                         static_cast<std::size_t>(nx)];
                acc.b += nb.b;
                acc.g += nb.g;
                acc.r += nb.r;
                acc.a += nb.a;
                ++count;
            }
            if (count == 0) {
                continue;
            }
            const float inv_c = kUnitF / static_cast<float>(count);
            PremulPixel& cur = grid[static_cast<std::size_t>(idx)];
            cur.b += kSorOmega * (acc.b * inv_c - cur.b);
            cur.g += kSorOmega * (acc.g * inv_c - cur.g);
            cur.r += kSorOmega * (acc.r * inv_c - cur.r);
            cur.a += kSorOmega * (acc.a * inv_c - cur.a);
        }
    }

    std::vector<PremulPixel> scratch = grid;
    for (int iter = 0; iter < kJacobiPolishIterations; ++iter) {
        for (const int idx : all_unknown) {
            const int lx = idx % gw;
            const int ly = idx / gw;
            PremulPixel acc{};
            int count = 0;
            for (const auto& [dx, dy] : kCardinalNeighbors) {
                const int nx = lx + dx;
                const int ny = ly + dy;
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) {
                    continue;
                }
                const PremulPixel& nb =
                    grid[static_cast<std::size_t>(ny) * static_cast<std::size_t>(gw) +
                         static_cast<std::size_t>(nx)];
                acc.b += nb.b;
                acc.g += nb.g;
                acc.r += nb.r;
                acc.a += nb.a;
                ++count;
            }
            if (count == 0) {
                continue;
            }
            const float inv_c = kUnitF / static_cast<float>(count);
            const float a = std::clamp(acc.a * inv_c, 0.0F, kUnitF);
            scratch[static_cast<std::size_t>(idx)] = PremulPixel{
                std::clamp(acc.b * inv_c, 0.0F, a),
                std::clamp(acc.g * inv_c, 0.0F, a),
                std::clamp(acc.r * inv_c, 0.0F, a),
                a,
            };
        }
        for (const int idx : all_unknown) {
            grid[static_cast<std::size_t>(idx)] = scratch[static_cast<std::size_t>(idx)];
        }
    }

    for (const int idx : all_unknown) {
        const int lx = idx % gw;
        const int ly = idx / gw;
        const PremulPixel& p = grid[static_cast<std::size_t>(idx)];
        const std::uint8_t out_a = ToByteF(p.a * kChannelMaxF);
        if (out_a == 0 || p.a <= kMinAlphaF) {
            imaging::SetPixel(image, work_rect.x + lx, work_rect.y + ly, BgraColor{});
            continue;
        }
        const float inv_a = kUnitF / p.a;
        imaging::SetPixel(image, work_rect.x + lx, work_rect.y + ly,
                          BgraColor{ToByteF(p.b * inv_a * kChannelMaxF),
                                    ToByteF(p.g * inv_a * kChannelMaxF),
                                    ToByteF(p.r * inv_a * kChannelMaxF), out_a});
    }
}

double RoundedRectSignedDistance(double px, double py, double cx, double cy, double hw, double hh,
                                 double radius) {
    const double dx = std::abs(px - cx) - (hw - radius);
    const double dy = std::abs(py - cy) - (hh - radius);
    const double outside_x = std::max(dx, 0.0);
    const double outside_y = std::max(dy, 0.0);
    const double inside_dist = std::min(std::max(dx, dy), 0.0);
    return std::hypot(outside_x, outside_y) + inside_dist - radius;
}

class ScopedMemoryDc {
public:
    ScopedMemoryDc() : dc_(CreateCompatibleDC(nullptr)) {
        if (dc_ == nullptr) {
            throw std::runtime_error("CreateCompatibleDC failed");
        }
    }
    ScopedMemoryDc(const ScopedMemoryDc&) = delete;
    ScopedMemoryDc& operator=(const ScopedMemoryDc&) = delete;
    ~ScopedMemoryDc() { DeleteDC(dc_); }

    HDC get() const { return dc_; }

private:
    HDC dc_ = nullptr;
};

class ScopedDib32 {
public:
    ScopedDib32(int width, int height) : width_(width), height_(height) {
        dc_ = CreateCompatibleDC(nullptr);
        if (dc_ == nullptr) {
            throw std::runtime_error("CreateCompatibleDC failed");
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = kSinglePlane;
        info.bmiHeader.biBitCount =
            static_cast<WORD>(imaging::kBgraBytesPerPixel * CHAR_BIT);
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (bitmap_ == nullptr || bits_ == nullptr) {
            DeleteDC(dc_);
            throw std::runtime_error("CreateDIBSection failed");
        }
        previous_ = SelectObject(dc_, bitmap_);
    }
    ScopedDib32(const ScopedDib32&) = delete;
    ScopedDib32& operator=(const ScopedDib32&) = delete;
    ~ScopedDib32() {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }

    HDC dc() const { return dc_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const std::uint8_t* bits() const { return static_cast<const std::uint8_t*>(bits_); }

private:
    int width_ = 0;
    int height_ = 0;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* bits_ = nullptr;
};

class ScopedGdiFont {
public:
    ScopedGdiFont(int em_height_px, int weight, bool italic, const std::wstring& family) {
        const wchar_t* face = family.empty() ? kDefaultFontFamily : family.c_str();
        font_ = CreateFontW(-std::max(1, em_height_px), 0, 0, 0, weight, italic ? TRUE : FALSE,
                            FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        if (font_ == nullptr) {
            throw std::runtime_error("CreateFontW failed");
        }
    }
    ScopedGdiFont(const ScopedGdiFont&) = delete;
    ScopedGdiFont& operator=(const ScopedGdiFont&) = delete;
    ~ScopedGdiFont() { DeleteObject(font_); }

    HFONT get() const { return font_; }

private:
    HFONT font_ = nullptr;
};

class ScopedSelectObject {
public:
    ScopedSelectObject(HDC dc, HGDIOBJ object) : dc_(dc), previous_(SelectObject(dc, object)) {}
    ScopedSelectObject(const ScopedSelectObject&) = delete;
    ScopedSelectObject& operator=(const ScopedSelectObject&) = delete;
    ~ScopedSelectObject() { SelectObject(dc_, previous_); }

private:
    HDC dc_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

std::wstring Utf8ToNormalizedWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring raw(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), raw.data(),
                        needed);

    std::wstring normalized;
    normalized.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == kCarriageReturn) {
            if (i + 1 < raw.size() && raw[i + 1] == kLineFeed) {
                continue;
            }
            normalized.push_back(kLineFeed);
            continue;
        }
        normalized.push_back(raw[i]);
    }
    return normalized;
}

bool IsSurrogatePairAt(const std::wstring& s, std::size_t index) {
    if (index + 1 >= s.size()) {
        return false;
    }
    const auto hi = static_cast<std::uint32_t>(s[index]);
    const auto lo = static_cast<std::uint32_t>(s[index + 1]);
    return hi >= kHighSurrogateMin && hi <= kHighSurrogateMax && lo >= kLowSurrogateMin &&
           lo <= kLowSurrogateMax;
}

struct GlyphMetrics3x {
    int advance = 0;
    int left_overhang = 0;
    int right_overhang = 0;
};

class FontMeasurer3x {
public:
    FontMeasurer3x(int em_height_3x, const TextStyle& style)
        : font_(em_height_3x, style.weight, style.italic, style.font_family),
          selected_(dc_.get(), font_.get()) {
        if (!GetTextMetricsW(dc_.get(), &tm_)) {
            throw std::runtime_error("GetTextMetricsW failed");
        }
        const DWORD pair_count = GetKerningPairsW(dc_.get(), 0, nullptr);
        if (pair_count > 0) {
            std::vector<KERNINGPAIR> pairs(pair_count);
            const DWORD fetched = GetKerningPairsW(dc_.get(), pair_count, pairs.data());
            kerning_.reserve(fetched);
            for (DWORD i = 0; i < fetched; ++i) {
                const std::uint32_t key =
                    (static_cast<std::uint32_t>(pairs[i].wFirst) << kPairShiftBits) |
                    static_cast<std::uint32_t>(pairs[i].wSecond);
                kerning_[key] = pairs[i].iKernAmount;
            }
        }
    }

    const TEXTMETRICW& tm() const { return tm_; }
    HFONT font_handle() const { return font_.get(); }

    GlyphMetrics3x MeasureCluster(const std::wstring& text, std::size_t pos, std::size_t len) {
        if (len == 1) {
            const wchar_t ch = text[pos];
            const auto found = bmp_cache_.find(ch);
            if (found != bmp_cache_.end()) {
                return found->second;
            }
            ABC abc{};
            const UINT code = static_cast<UINT>(ch);
            if (GetCharABCWidthsW(dc_.get(), code, code, &abc)) {
                const GlyphMetrics3x gm{abc.abcA + static_cast<int>(abc.abcB) + abc.abcC,
                                        std::max(0, -abc.abcA), std::max(0, -abc.abcC)};
                bmp_cache_.emplace(ch, gm);
                return gm;
            }
            SIZE sz{};
            GetTextExtentPoint32W(dc_.get(), &ch, 1, &sz);
            const GlyphMetrics3x gm{sz.cx, 0, static_cast<int>(tm_.tmOverhang)};
            bmp_cache_.emplace(ch, gm);
            return gm;
        }
        SIZE sz{};
        GetTextExtentPoint32W(dc_.get(), text.data() + pos, static_cast<int>(len), &sz);
        return GlyphMetrics3x{sz.cx, 0, static_cast<int>(tm_.tmOverhang)};
    }

    int KerningBetween(wchar_t first, wchar_t second) const {
        if (kerning_.empty()) {
            return 0;
        }
        const std::uint32_t key =
            (static_cast<std::uint32_t>(first) << kPairShiftBits) |
            static_cast<std::uint32_t>(second);
        const auto it = kerning_.find(key);
        if (it == kerning_.end()) {
            return 0;
        }
        return it->second;
    }

    int MeasureUntrackedAdvance(const std::wstring& line) const {
        if (line.empty()) {
            return 0;
        }
        SIZE sz{};
        GetTextExtentPoint32W(dc_.get(), line.c_str(), static_cast<int>(line.size()), &sz);
        return sz.cx;
    }

private:
    ScopedMemoryDc dc_;
    ScopedGdiFont font_;
    ScopedSelectObject selected_;
    TEXTMETRICW tm_{};
    std::unordered_map<wchar_t, GlyphMetrics3x> bmp_cache_;
    std::unordered_map<std::uint32_t, int> kerning_;
};

struct MeasuredLine3x {
    std::wstring text;
    std::vector<INT> gdi_dx;
    bool use_explicit_dx = false;
    int unsqueezed_advance = 0;
    int left_overhang = 0;
    int right_overhang = 0;
};

MeasuredLine3x MeasureSingleLine(FontMeasurer3x& measurer, const std::wstring& line,
                                 int tracking_3x, bool has_tracking) {
    MeasuredLine3x out;
    out.text = line;
    if (line.empty()) {
        return out;
    }

    std::vector<std::pair<std::size_t, std::size_t>> clusters;
    clusters.reserve(line.size());
    for (std::size_t i = 0; i < line.size();) {
        const std::size_t clen = IsSurrogatePairAt(line, i) ? 2U : 1U;
        clusters.emplace_back(i, clen);
        i += clen;
    }

    const GlyphMetrics3x first_gm =
        measurer.MeasureCluster(line, clusters.front().first, clusters.front().second);
    const GlyphMetrics3x last_gm =
        measurer.MeasureCluster(line, clusters.back().first, clusters.back().second);
    out.left_overhang = first_gm.left_overhang;
    out.right_overhang = last_gm.right_overhang;

    out.use_explicit_dx = true;
    out.gdi_dx.assign(line.size(), 0);
    int total_adv = 0;
    for (std::size_t k = 0; k < clusters.size(); ++k) {
        const auto [pos, clen] = clusters[k];
        const GlyphMetrics3x gm = measurer.MeasureCluster(line, pos, clen);
        int step = gm.advance;
        if (k + 1 < clusters.size()) {
            if (clen == 1 && clusters[k + 1].second == 1) {
                step += measurer.KerningBetween(line[pos], line[clusters[k + 1].first]);
            }
            if (has_tracking) {
                step += tracking_3x;
            }
        }
        step = std::max(0, step);
        out.gdi_dx[pos] = step;
        total_adv += step;
    }
    out.unsqueezed_advance = total_adv;
    return out;
}

float SqueezedLineSpan3x(const MeasuredLine3x& line, float squeeze) {
    const int total_unsqueezed =
        line.left_overhang + line.unsqueezed_advance + line.right_overhang;
    return static_cast<float>(total_unsqueezed) * squeeze;
}

std::vector<std::wstring> SplitParagraphs(const std::wstring& text) {
    std::vector<std::wstring> paragraphs;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find(kLineFeed, start);
        if (nl == std::wstring::npos) {
            paragraphs.push_back(text.substr(start));
            break;
        }
        paragraphs.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return paragraphs;
}

std::vector<MeasuredLine3x> WrapParagraph(FontMeasurer3x& measurer, const std::wstring& paragraph,
                                          int tracking_3x, bool has_tracking, float squeeze,
                                          float max_line_width_3x, bool wrap_spaces) {
    if (paragraph.empty() || !wrap_spaces) {
        return {MeasureSingleLine(measurer, paragraph, tracking_3x, has_tracking)};
    }

    std::vector<std::wstring> words;
    std::size_t i = 0;
    while (i < paragraph.size()) {
        while (i < paragraph.size() && paragraph[i] == kSpaceChar) {
            ++i;
        }
        if (i >= paragraph.size()) {
            break;
        }
        const std::size_t wstart = i;
        while (i < paragraph.size() && paragraph[i] != kSpaceChar) {
            ++i;
        }
        words.push_back(paragraph.substr(wstart, i - wstart));
    }

    if (words.empty()) {
        return {MeasureSingleLine(measurer, L"", tracking_3x, has_tracking)};
    }

    std::vector<MeasuredLine3x> lines;
    std::wstring current = words.front();
    MeasuredLine3x current_measured =
        MeasureSingleLine(measurer, current, tracking_3x, has_tracking);

    for (std::size_t w = 1; w < words.size(); ++w) {
        std::wstring candidate = current;
        candidate.push_back(kSpaceChar);
        candidate.append(words[w]);
        MeasuredLine3x cand_measured =
            MeasureSingleLine(measurer, candidate, tracking_3x, has_tracking);
        if (SqueezedLineSpan3x(cand_measured, squeeze) <= max_line_width_3x) {
            current = std::move(candidate);
            current_measured = std::move(cand_measured);
            continue;
        }
        lines.push_back(std::move(current_measured));
        current = words[w];
        current_measured = MeasureSingleLine(measurer, current, tracking_3x, has_tracking);
    }
    lines.push_back(std::move(current_measured));
    return lines;
}

struct EffectMargins3x {
    float pad_left = 0.0F;
    float pad_right = 0.0F;
    float pad_top = 0.0F;
    float pad_bottom = 0.0F;
    float stroke_r3 = 0.0F;
    int shadow_dx3 = 0;
    int shadow_dy3 = 0;
    int shadow_box_r3 = 0;
};

EffectMargins3x ComputeEffectMargins3x(const TextStyle& style, float scale) {
    EffectMargins3x m;
    if (style.stroke_px > 0.0F) {
        m.stroke_r3 = style.stroke_px * scale * kSupersampleScaleF;
    }
    float shadow_left = 0.0F;
    float shadow_right = 0.0F;
    float shadow_top = 0.0F;
    float shadow_bottom = 0.0F;
    if (style.shadow && style.shadow_color.a > 0) {
        m.shadow_dx3 =
            static_cast<int>(std::lround(style.shadow_dx * scale * kSupersampleScaleF));
        m.shadow_dy3 =
            static_cast<int>(std::lround(style.shadow_dy * scale * kSupersampleScaleF));
        const float blur_3x = std::max(0.0F, style.shadow_blur_px) * scale * kSupersampleScaleF;
        if (blur_3x > 0.0F) {
            m.shadow_box_r3 =
                std::max(1, static_cast<int>(std::lround(blur_3x - kBlurRadiusBias)));
        }
        const float fit_blur = blur_3x * kShadowSafeBlurFactor;
        shadow_left = std::max(0.0F, fit_blur - static_cast<float>(m.shadow_dx3));
        shadow_right = std::max(0.0F, fit_blur + static_cast<float>(m.shadow_dx3));
        shadow_top = std::max(0.0F, fit_blur - static_cast<float>(m.shadow_dy3));
        shadow_bottom = std::max(0.0F, fit_blur + static_cast<float>(m.shadow_dy3));
    }
    m.pad_left = m.stroke_r3 + shadow_left;
    m.pad_right = m.stroke_r3 + shadow_right;
    m.pad_top = m.stroke_r3 + shadow_top;
    m.pad_bottom = m.stroke_r3 + shadow_bottom;
    return m;
}

struct LayoutCandidate {
    bool fits = false;
    float scale = 1.0F;
    float squeeze = 1.0F;
    int em_height_3x = 1;
    int line_step_3x = 1;
    int tm_height_3x = 1;
    int tm_internal_leading_3x = 0;
    EffectMargins3x margins;
    std::vector<MeasuredLine3x> lines;
    float max_line_span_3x = 0.0F;
    float visual_height_3x = 0.0F;
};

LayoutCandidate EvaluateCandidate(FontMeasurer3x& measurer,
                                  const std::vector<std::wstring>& paragraphs,
                                  const RectI& safe_bounds, const TextStyle& style, float scale,
                                  float squeeze, int em_height_3x) {
    LayoutCandidate cand;
    cand.scale = scale;
    cand.squeeze = squeeze;
    cand.em_height_3x = em_height_3x;
    cand.margins = ComputeEffectMargins3x(style, scale);

    const TEXTMETRICW& tm = measurer.tm();
    cand.tm_height_3x = tm.tmHeight;
    cand.tm_internal_leading_3x = tm.tmInternalLeading;
    const float raw_body_height =
        static_cast<float>(std::max(1L, tm.tmHeight - tm.tmInternalLeading));
    const float body_height =
        std::min(raw_body_height, static_cast<float>(em_height_3x) * kMaxNaturalLineRatio);
    const float raw_step = static_cast<float>(tm.tmHeight + tm.tmExternalLeading);
    const float natural_step = std::min(raw_step, body_height * kMaxNaturalLineRatio);
    cand.line_step_3x =
        std::max(1, static_cast<int>(std::lround(natural_step * std::max(kMinAllowedFactor,
                                                                         style.line_spacing))));

    const float safe_w_3x = static_cast<float>(safe_bounds.w * kSupersampleScale);
    const float safe_h_3x = static_cast<float>(safe_bounds.h * kSupersampleScale);
    const float avail_w_3x =
        std::max(0.0F, safe_w_3x - cand.margins.pad_left - cand.margins.pad_right);
    const float avail_h_3x =
        std::max(0.0F, safe_h_3x - cand.margins.pad_top - cand.margins.pad_bottom);

    const bool has_tracking = std::abs(style.tracking_px) > kMinAlphaF;
    const int tracking_3x =
        static_cast<int>(std::lround(style.tracking_px * scale * kSupersampleScaleF));

    for (const std::wstring& para : paragraphs) {
        std::vector<MeasuredLine3x> wrapped = WrapParagraph(
            measurer, para, tracking_3x, has_tracking, squeeze, avail_w_3x, style.wrap);
        for (MeasuredLine3x& line : wrapped) {
            cand.max_line_span_3x =
                std::max(cand.max_line_span_3x, SqueezedLineSpan3x(line, squeeze));
            cand.lines.push_back(std::move(line));
        }
    }

    const int line_count = static_cast<int>(cand.lines.size());
    if (line_count > 0) {
        const int last_top = (line_count - 1) * cand.line_step_3x;
        const int single_line_visual =
            std::min(cand.tm_height_3x - cand.tm_internal_leading_3x,
                     static_cast<int>( std::lround(body_height)));
        cand.visual_height_3x =
            static_cast<float>(std::max(1, last_top + single_line_visual));
    }
    cand.fits = (safe_bounds.w > 0 && safe_bounds.h > 0 && cand.max_line_span_3x <= avail_w_3x &&
                 cand.visual_height_3x <= avail_h_3x);
    return cand;
}

std::vector<float> BuildDescendingLadder(float top_val, float min_val, float step) {
    std::vector<float> values;
    values.push_back(top_val);
    if (min_val >= top_val - kMinAlphaF) {
        return values;
    }
    for (float v = top_val - step; v > min_val + kMinAlphaF; v -= step) {
        values.push_back(v);
    }
    values.push_back(min_val);
    return values;
}

struct DiskTap {
    int dx = 0;
    int dy = 0;
    float dist = 0.0F;
};

std::vector<float> DilateCoverageDisk(const std::vector<float>& fill_cov, int w, int h,
                                      float radius_3x) {
    std::vector<float> out = fill_cov;
    if (radius_3x <= 0.0F || w <= 0 || h <= 0) {
        return out;
    }

    const int r_ceil = static_cast<int>(std::ceil(radius_3x + kStrokeDiskMargin));
    const float max_dist = radius_3x + kStrokeDiskMargin;
    std::vector<DiskTap> disk;
    disk.reserve(static_cast<std::size_t>((2 * r_ceil + 1) * (2 * r_ceil + 1)));
    for (int dy = -r_ceil; dy <= r_ceil; ++dy) {
        for (int dx = -r_ceil; dx <= r_ceil; ++dx) {
            const float d = std::hypot(static_cast<float>(dx), static_cast<float>(dy));
            if (d < max_dist) {
                disk.push_back(DiskTap{dx, dy, d});
            }
        }
    }

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t idx =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                static_cast<std::size_t>(x);
            const float c = fill_cov[idx];
            if (c <= 0.0F) {
                continue;
            }
            if (c >= kUnitF && x > 0 && y > 0 && x + 1 < w && y + 1 < h) {
                bool interior = true;
                for (const NeighborOffset& nb : kEightNeighbors) {
                    const std::size_t nidx =
                        static_cast<std::size_t>(y + nb.dy) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(x + nb.dx);
                    if (fill_cov[nidx] < kUnitF) {
                        interior = false;
                        break;
                    }
                }
                if (interior) {
                    continue;
                }
            }
            for (const DiskTap& tap : disk) {
                const int nx = x + tap.dx;
                const int ny = y + tap.dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                    continue;
                }
                const float cand = std::min(kUnitF, c + radius_3x - tap.dist);
                if (cand <= 0.0F) {
                    continue;
                }
                const std::size_t nidx =
                    static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) +
                    static_cast<std::size_t>(nx);
                out[nidx] = std::max(out[nidx], cand);
            }
        }
    }
    return out;
}

void BoxBlurHorizontal(const std::vector<float>& src, std::vector<float>& dst, int w, int h,
                       int radius) {
    const float inv_kernel = kUnitF / static_cast<float>(kBlurKernelSideCount * radius + 1);
    for (int y = 0; y < h; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        float sum = 0.0F;
        for (int k = -radius; k <= radius; ++k) {
            const int cx = std::clamp(k, 0, w - 1);
            sum += src[row + static_cast<std::size_t>(cx)];
        }
        for (int x = 0; x < w; ++x) {
            dst[row + static_cast<std::size_t>(x)] = sum * inv_kernel;
            const int remove_x = std::clamp(x - radius, 0, w - 1);
            const int add_x = std::clamp(x + radius + 1, 0, w - 1);
            sum += src[row + static_cast<std::size_t>(add_x)] -
                   src[row + static_cast<std::size_t>(remove_x)];
        }
    }
}

void BoxBlurVertical(const std::vector<float>& src, std::vector<float>& dst, int w, int h,
                     int radius) {
    const float inv_kernel = kUnitF / static_cast<float>(kBlurKernelSideCount * radius + 1);
    for (int x = 0; x < w; ++x) {
        float sum = 0.0F;
        for (int k = -radius; k <= radius; ++k) {
            const int cy = std::clamp(k, 0, h - 1);
            sum += src[static_cast<std::size_t>(cy) * static_cast<std::size_t>(w) +
                       static_cast<std::size_t>(x)];
        }
        for (int y = 0; y < h; ++y) {
            dst[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                static_cast<std::size_t>(x)] = sum * inv_kernel;
            const int remove_y = std::clamp(y - radius, 0, h - 1);
            const int add_y = std::clamp(y + radius + 1, 0, h - 1);
            sum += src[static_cast<std::size_t>(add_y) * static_cast<std::size_t>(w) +
                       static_cast<std::size_t>(x)] -
                   src[static_cast<std::size_t>(remove_y) * static_cast<std::size_t>(w) +
                       static_cast<std::size_t>(x)];
        }
    }
}

std::vector<float> BuildShadowCoverage(const std::vector<float>& shape_cov, int w, int h, int dx3,
                                       int dy3, int box_radius_3x) {
    const std::size_t total = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> current(total, 0.0F);
    for (int y = 0; y < h; ++y) {
        const int sy = y - dy3;
        if (sy < 0 || sy >= h) {
            continue;
        }
        for (int x = 0; x < w; ++x) {
            const int sx = x - dx3;
            if (sx < 0 || sx >= w) {
                continue;
            }
            current[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                    static_cast<std::size_t>(x)] =
                shape_cov[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) +
                          static_cast<std::size_t>(sx)];
        }
    }
    if (box_radius_3x <= 0) {
        return current;
    }
    std::vector<float> scratch(total, 0.0F);
    for (int pass = 0; pass < kBlurPassCount; ++pass) {
        BoxBlurHorizontal(current, scratch, w, h, box_radius_3x);
        BoxBlurVertical(scratch, current, w, h, box_radius_3x);
    }
    return current;
}

BgraColor LerpColor(BgraColor top, BgraColor bottom, float t) {
    const float clamped = std::clamp(t, 0.0F, kUnitF);
    const float inv = kUnitF - clamped;
    return BgraColor{
        ToByteF(static_cast<float>(top.b) * inv + static_cast<float>(bottom.b) * clamped),
        ToByteF(static_cast<float>(top.g) * inv + static_cast<float>(bottom.g) * clamped),
        ToByteF(static_cast<float>(top.r) * inv + static_cast<float>(bottom.r) * clamped),
        ToByteF(static_cast<float>(top.a) * inv + static_cast<float>(bottom.a) * clamped),
    };
}

struct RasterOutcome {
    bool foreground_clipped = false;
    RectI ink_bounds{};
};

RasterOutcome RasterizeCandidate(BgraImage* target_image, const LayoutCandidate& cand,
                                 const RectI& safe_bounds, int canvas_w, int canvas_h,
                                 const TextStyle& style) {
    RasterOutcome outcome;
    if (safe_bounds.w <= 0 || safe_bounds.h <= 0 || cand.lines.empty()) {
        return outcome;
    }

    const float safe_w_3x = static_cast<float>(safe_bounds.w * kSupersampleScale);
    const float safe_h_3x = static_cast<float>(safe_bounds.h * kSupersampleScale);
    const float inner_left = cand.margins.pad_left;
    const float inner_right = std::max(inner_left, safe_w_3x - cand.margins.pad_right);
    const float inner_top = cand.margins.pad_top;
    const float inner_bottom = std::max(inner_top, safe_h_3x - cand.margins.pad_bottom);
    const float inner_h = inner_bottom - inner_top;

    float first_line_y = inner_top - static_cast<float>(cand.tm_internal_leading_3x);
    if (style.valign == VerticalAlign::kCenter) {
        first_line_y = inner_top + (inner_h - cand.visual_height_3x) * kHalfF -
                       static_cast<float>(cand.tm_internal_leading_3x);
    }
    if (style.valign == VerticalAlign::kBottom) {
        first_line_y = inner_bottom - cand.visual_height_3x -
                       static_cast<float>(cand.tm_internal_leading_3x);
    }

    std::vector<float> line_x(cand.lines.size(), 0.0F);
    float est_min_x3 = safe_w_3x;
    float est_max_x3 = 0.0F;
    for (std::size_t i = 0; i < cand.lines.size(); ++i) {
        const MeasuredLine3x& ln = cand.lines[i];
        const float span = SqueezedLineSpan3x(ln, cand.squeeze);
        const float oh_left = static_cast<float>(ln.left_overhang) * cand.squeeze;
        float visual_left = inner_left;
        if (style.halign == HorizontalAlign::kCenter) {
            visual_left = inner_left + (inner_right - inner_left - span) * kHalfF;
        }
        if (style.halign == HorizontalAlign::kRight) {
            visual_left = inner_right - span;
        }
        line_x[i] = visual_left + oh_left;
        est_min_x3 = std::min(est_min_x3, visual_left);
        est_max_x3 = std::max(est_max_x3, visual_left + span);
    }

    const int line_count = static_cast<int>(cand.lines.size());
    const float est_min_y3 = first_line_y;
    const float est_max_y3 =
        first_line_y + static_cast<float>((line_count - 1) * cand.line_step_3x + cand.tm_height_3x);

    const float full_blur_pad =
        static_cast<float>(kBlurPassCount * cand.margins.shadow_box_r3) +
        static_cast<float>(std::max(std::abs(cand.margins.shadow_dx3),
                                    std::abs(cand.margins.shadow_dy3))) +
        cand.margins.stroke_r3 + kSupersampleScaleF;

    const int sub_x0_1x =
        std::min(-kSentinelPixels1x,
                 static_cast<int>(std::floor((est_min_x3 - full_blur_pad) / kSupersampleScaleF)) -
                     kSentinelPixels1x);
    const int sub_y0_1x =
        std::min(-kSentinelPixels1x,
                 static_cast<int>(std::floor((est_min_y3 - full_blur_pad) / kSupersampleScaleF)) -
                     kSentinelPixels1x);
    const int sub_x1_1x =
        std::max(safe_bounds.w + kSentinelPixels1x,
                 static_cast<int>(std::ceil((est_max_x3 + full_blur_pad) / kSupersampleScaleF)) +
                     kSentinelPixels1x);
    const int sub_y1_1x =
        std::max(safe_bounds.h + kSentinelPixels1x,
                 static_cast<int>(std::ceil((est_max_y3 + full_blur_pad) / kSupersampleScaleF)) +
                     kSentinelPixels1x);

    const int sub_w_1x = sub_x1_1x - sub_x0_1x;
    const int sub_h_1x = sub_y1_1x - sub_y0_1x;
    if (sub_w_1x <= 0 || sub_h_1x <= 0) {
        return outcome;
    }

    const int buf_w3 = sub_w_1x * kSupersampleScale;
    const int buf_h3 = sub_h_1x * kSupersampleScale;
    const float origin_x3 = static_cast<float>(sub_x0_1x * kSupersampleScale);
    const float origin_y3 = static_cast<float>(sub_y0_1x * kSupersampleScale);

    ScopedDib32 dib(buf_w3, buf_h3);
    ScopedGdiFont font(cand.em_height_3x, style.weight, style.italic, style.font_family);
    const ScopedSelectObject selected_font(dib.dc(), font.get());
    SetBkMode(dib.dc(), TRANSPARENT);
    SetTextColor(dib.dc(), kGlyphWhiteColor);
    SetGraphicsMode(dib.dc(), GM_ADVANCED);

    for (std::size_t i = 0; i < cand.lines.size(); ++i) {
        const MeasuredLine3x& ln = cand.lines[i];
        if (ln.text.empty()) {
            continue;
        }
        const float draw_x = line_x[i] - origin_x3;
        const float draw_y =
            first_line_y + static_cast<float>(static_cast<int>(i) * cand.line_step_3x) - origin_y3;
        XFORM xf{};
        xf.eM11 = cand.squeeze;
        xf.eM12 = 0.0F;
        xf.eM21 = 0.0F;
        xf.eM22 = kUnitF;
        xf.eDx = draw_x;
        xf.eDy = draw_y;
        SetWorldTransform(dib.dc(), &xf);
        const INT* dx_ptr = ln.use_explicit_dx ? ln.gdi_dx.data() : nullptr;
        ExtTextOutW(dib.dc(), 0, 0, 0, nullptr, ln.text.c_str(),
                    static_cast<UINT>(ln.text.size()), dx_ptr);
    }
    GdiFlush();

    const std::size_t total_3x =
        static_cast<std::size_t>(buf_w3) * static_cast<std::size_t>(buf_h3);
    std::vector<float> fill_cov(total_3x, 0.0F);
    const std::uint8_t* raw = dib.bits();
    int glyph_min_y3 = buf_h3;
    int glyph_max_y3 = -1;
    int glyph_min_x3 = buf_w3;
    int glyph_max_x3 = -1;

    for (int y3 = 0; y3 < buf_h3; ++y3) {
        for (int x3 = 0; x3 < buf_w3; ++x3) {
            const std::size_t idx =
                static_cast<std::size_t>(y3) * static_cast<std::size_t>(buf_w3) +
                static_cast<std::size_t>(x3);
            const std::size_t off = idx * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
            const int lum = (kLuminanceBlueWeight * static_cast<int>(raw[off + imaging::kChannelBlue]) +
                             kLuminanceGreenWeight * static_cast<int>(raw[off + imaging::kChannelGreen]) +
                             kLuminanceRedWeight * static_cast<int>(raw[off + imaging::kChannelRed]) +
                             kLuminanceRoundingBias) /
                            kLuminanceWeightSum;
            if (lum <= 0) {
                continue;
            }
            fill_cov[idx] = static_cast<float>(lum) * kInvChannelMaxF;
            glyph_min_y3 = std::min(glyph_min_y3, y3);
            glyph_max_y3 = std::max(glyph_max_y3, y3);
            glyph_min_x3 = std::min(glyph_min_x3, x3);
            glyph_max_x3 = std::max(glyph_max_x3, x3);
        }
    }

    if (glyph_max_y3 < glyph_min_y3) {
        return outcome;
    }

    // Minimal subpixel boundary nudge when glyph+stroke fits inside safe_bounds
    // in span, yet font overhang/internal-leading nudged a tip across a safe edge.
    const int safe_min_x3 = -sub_x0_1x * kSupersampleScale;
    const int safe_max_x3 = (safe_bounds.w - sub_x0_1x) * kSupersampleScale - 1;
    const int safe_min_y3 = -sub_y0_1x * kSupersampleScale;
    const int safe_max_y3 = (safe_bounds.h - sub_y0_1x) * kSupersampleScale - 1;
    const int stroke_pad = static_cast<int>(std::ceil(cand.margins.stroke_r3));

    int shift_x3 = 0;
    const int fg_w3 = (glyph_max_x3 - glyph_min_x3 + 1) + 2 * stroke_pad;
    if (fg_w3 <= safe_bounds.w * kSupersampleScale) {
        if (glyph_min_x3 - stroke_pad < safe_min_x3) {
            shift_x3 = safe_min_x3 - (glyph_min_x3 - stroke_pad);
        }
        if (shift_x3 == 0 && glyph_max_x3 + stroke_pad > safe_max_x3) {
            shift_x3 = safe_max_x3 - (glyph_max_x3 + stroke_pad);
        }
    }
    int shift_y3 = 0;
    const bool is_swash_metrics =
        static_cast<float>(cand.tm_height_3x) >
        static_cast<float>(cand.em_height_3x) * kSwashFontHeightRatio;
    const int fg_h3 = (glyph_max_y3 - glyph_min_y3 + 1) + 2 * stroke_pad;
    if (is_swash_metrics && fg_h3 <= safe_bounds.h * kSupersampleScale) {
        const float target_top =
            (style.valign == VerticalAlign::kTop)
                ? (static_cast<float>(safe_min_y3) + inner_top)
                : ((style.valign == VerticalAlign::kBottom)
                       ? (static_cast<float>(safe_min_y3) + inner_bottom -
                          static_cast<float>(glyph_max_y3 - glyph_min_y3 + 1))
                       : (static_cast<float>(safe_min_y3) + inner_top +
                          (inner_h - static_cast<float>(glyph_max_y3 - glyph_min_y3 + 1)) * kHalfF));
        shift_y3 = static_cast<int>(std::lround(target_top)) - glyph_min_y3;
    }
    if (fg_h3 <= safe_bounds.h * kSupersampleScale) {
        if (glyph_min_y3 + shift_y3 - stroke_pad < safe_min_y3) {
            shift_y3 = safe_min_y3 - (glyph_min_y3 - stroke_pad);
        }
        if (glyph_max_y3 + shift_y3 + stroke_pad > safe_max_y3) {
            shift_y3 = safe_max_y3 - (glyph_max_y3 + stroke_pad);
        }
    }
    if (shift_x3 != 0 || shift_y3 != 0) {
        std::vector<float> shifted(total_3x, 0.0F);
        glyph_min_y3 = buf_h3;
        glyph_max_y3 = -1;
        for (int y3 = 0; y3 < buf_h3; ++y3) {
            const int sy = y3 - shift_y3;
            if (sy < 0 || sy >= buf_h3) {
                continue;
            }
            for (int x3 = 0; x3 < buf_w3; ++x3) {
                const int sx = x3 - shift_x3;
                if (sx < 0 || sx >= buf_w3) {
                    continue;
                }
                const float v =
                    fill_cov[static_cast<std::size_t>(sy) * static_cast<std::size_t>(buf_w3) +
                             static_cast<std::size_t>(sx)];
                if (v <= 0.0F) {
                    continue;
                }
                shifted[static_cast<std::size_t>(y3) * static_cast<std::size_t>(buf_w3) +
                        static_cast<std::size_t>(x3)] = v;
                glyph_min_y3 = std::min(glyph_min_y3, y3);
                glyph_max_y3 = std::max(glyph_max_y3, y3);
            }
        }
        fill_cov.swap(shifted);
    }

    const bool use_stroke = style.stroke_px > 0.0F && style.stroke.a > 0;
    const std::vector<float> stroke_cov =
        use_stroke ? DilateCoverageDisk(fill_cov, buf_w3, buf_h3, cand.margins.stroke_r3)
                   : std::vector<float>{};
    const std::vector<float>& shape_cov = use_stroke ? stroke_cov : fill_cov;

    for (int y3 = 0; y3 < buf_h3; ++y3) {
        for (int x3 = 0; x3 < buf_w3; ++x3) {
            if (x3 >= safe_min_x3 && x3 <= safe_max_x3 && y3 >= safe_min_y3 && y3 <= safe_max_y3) {
                continue;
            }
            const float fg =
                shape_cov[static_cast<std::size_t>(y3) * static_cast<std::size_t>(buf_w3) +
                          static_cast<std::size_t>(x3)];
            if (fg > 0.0F) {
                outcome.foreground_clipped = true;
                break;
            }
        }
        if (outcome.foreground_clipped) {
            break;
        }
    }

    const bool use_shadow = style.shadow && style.shadow_color.a > 0;
    const std::vector<float> shadow_cov =
        use_shadow ? BuildShadowCoverage(shape_cov, buf_w3, buf_h3, cand.margins.shadow_dx3,
                                         cand.margins.shadow_dy3, cand.margins.shadow_box_r3)
                   : std::vector<float>{};

    const float grad_span = static_cast<float>(std::max(1, glyph_max_y3 - glyph_min_y3));
    const float sh_b = static_cast<float>(style.shadow_color.b) * kInvChannelMaxF;
    const float sh_g = static_cast<float>(style.shadow_color.g) * kInvChannelMaxF;
    const float sh_r = static_cast<float>(style.shadow_color.r) * kInvChannelMaxF;
    const float sh_base_a = static_cast<float>(style.shadow_color.a) * kInvChannelMaxF;

    const float st_b = static_cast<float>(style.stroke.b) * kInvChannelMaxF;
    const float st_g = static_cast<float>(style.stroke.g) * kInvChannelMaxF;
    const float st_r = static_cast<float>(style.stroke.r) * kInvChannelMaxF;
    const float st_base_a = static_cast<float>(style.stroke.a) * kInvChannelMaxF;

    int ink_min_x = canvas_w;
    int ink_min_y = canvas_h;
    int ink_max_x = -1;
    int ink_max_y = -1;

    const int rel_x0 = std::max(0, sub_x0_1x);
    const int rel_y0 = std::max(0, sub_y0_1x);
    const int rel_x1 = std::min(safe_bounds.w, sub_x1_1x);
    const int rel_y1 = std::min(safe_bounds.h, sub_y1_1x);

    for (int ry = rel_y0; ry < rel_y1; ++ry) {
        const int dst_y = safe_bounds.y + ry;
        if (dst_y < 0 || dst_y >= canvas_h) {
            continue;
        }
        const int base_y3 = (ry - sub_y0_1x) * kSupersampleScale;
        for (int rx = rel_x0; rx < rel_x1; ++rx) {
            const int dst_x = safe_bounds.x + rx;
            if (dst_x < 0 || dst_x >= canvas_w) {
                continue;
            }
            const int base_x3 = (rx - sub_x0_1x) * kSupersampleScale;
            float sum_b = 0.0F;
            float sum_g = 0.0F;
            float sum_r = 0.0F;
            float sum_a = 0.0F;

            for (int sy = 0; sy < kSupersampleScale; ++sy) {
                const int y3 = base_y3 + sy;
                const BgraColor fill_col =
                    style.gradient
                        ? LerpColor(style.fill, style.fill_bottom,
                                    static_cast<float>(y3 - glyph_min_y3) / grad_span)
                        : style.fill;
                const float fl_b = static_cast<float>(fill_col.b) * kInvChannelMaxF;
                const float fl_g = static_cast<float>(fill_col.g) * kInvChannelMaxF;
                const float fl_r = static_cast<float>(fill_col.r) * kInvChannelMaxF;
                const float fl_base_a = static_cast<float>(fill_col.a) * kInvChannelMaxF;

                const std::size_t row_off =
                    static_cast<std::size_t>(y3) * static_cast<std::size_t>(buf_w3);
                for (int sx = 0; sx < kSupersampleScale; ++sx) {
                    const std::size_t idx = row_off + static_cast<std::size_t>(base_x3 + sx);
                    float pm_b = 0.0F;
                    float pm_g = 0.0F;
                    float pm_r = 0.0F;
                    float pm_a = 0.0F;

                    if (use_shadow && shadow_cov[idx] > 0.0F) {
                        const float a = shadow_cov[idx] * sh_base_a;
                        pm_b = sh_b * a;
                        pm_g = sh_g * a;
                        pm_r = sh_r * a;
                        pm_a = a;
                    }
                    if (use_stroke && stroke_cov[idx] > 0.0F) {
                        const float a = stroke_cov[idx] * st_base_a;
                        const float inv = kUnitF - a;
                        pm_b = st_b * a + pm_b * inv;
                        pm_g = st_g * a + pm_g * inv;
                        pm_r = st_r * a + pm_r * inv;
                        pm_a = a + pm_a * inv;
                    }
                    if (fill_cov[idx] > 0.0F) {
                        const float a = fill_cov[idx] * fl_base_a;
                        const float inv = kUnitF - a;
                        pm_b = fl_b * a + pm_b * inv;
                        pm_g = fl_g * a + pm_g * inv;
                        pm_r = fl_r * a + pm_r * inv;
                        pm_a = a + pm_a * inv;
                    }
                    sum_b += pm_b;
                    sum_g += pm_g;
                    sum_r += pm_r;
                    sum_a += pm_a;
                }
            }

            const float avg_a = sum_a * kInvSupersampleArea;
            if (ToByteF(avg_a * kChannelMaxF) == 0) {
                continue;
            }
            ink_min_x = std::min(ink_min_x, dst_x);
            ink_min_y = std::min(ink_min_y, dst_y);
            ink_max_x = std::max(ink_max_x, dst_x);
            ink_max_y = std::max(ink_max_y, dst_y);

            if (target_image != nullptr) {
                const BgraColor dst_px = imaging::GetPixel(*target_image, dst_x, dst_y);
                const BgraColor blended = CompositePremulOverStraight(
                    dst_px, sum_b * kInvSupersampleArea, sum_g * kInvSupersampleArea,
                    sum_r * kInvSupersampleArea, avg_a);
                imaging::SetPixel(*target_image, dst_x, dst_y, blended);
            }
        }
    }

    if (ink_max_x >= ink_min_x && ink_max_y >= ink_min_y) {
        outcome.ink_bounds =
            RectI{ink_min_x, ink_min_y, ink_max_x - ink_min_x + 1, ink_max_y - ink_min_y + 1};
    }
    return outcome;
}

TextLayoutResult LayoutAndOptionallyPaint(BgraImage* target_image, int canvas_w, int canvas_h,
                                          const std::string& text, const RectI& safe_bounds,
                                          const TextStyle& style) {
    if (style.size_px <= 0.0F) {
        throw std::invalid_argument("TextStyle::size_px must be positive");
    }
    const std::wstring wide = Utf8ToNormalizedWide(text);
    if (wide.empty()) {
        return TextLayoutResult{safe_bounds.w >= 0 && safe_bounds.h >= 0, kUnitF, kUnitF, 0,
                                RectI{}};
    }

    const float min_squeeze = std::clamp(style.min_squeeze, kMinAllowedFactor, kUnitF);
    const float min_scale = std::clamp(style.min_scale, kMinAllowedFactor, kUnitF);
    const std::vector<std::wstring> paragraphs = SplitParagraphs(wide);

    if (safe_bounds.w <= 0 || safe_bounds.h <= 0) {
        const int em_min =
            std::max(1, static_cast<int>(std::lround(style.size_px * min_scale * kSupersampleScaleF)));
        FontMeasurer3x measurer(em_min, style);
        const LayoutCandidate fallback = EvaluateCandidate(
            measurer, paragraphs, safe_bounds, style, min_scale, min_squeeze, em_min);
        return TextLayoutResult{false, min_scale, min_squeeze,
                                static_cast<int>(fallback.lines.size()), RectI{}};
    }

    const std::vector<float> squeeze_ladder =
        BuildDescendingLadder(kUnitF, min_squeeze, kFitStepFactor);
    const std::vector<float> scale_ladder =
        BuildDescendingLadder(kUnitF, min_scale, kFitStepFactor);

    std::vector<std::pair<float, float>> schedule;
    schedule.reserve(squeeze_ladder.size() + scale_ladder.size());
    for (const float sq : squeeze_ladder) {
        schedule.emplace_back(kUnitF, sq);
    }
    for (std::size_t i = 1; i < scale_ladder.size(); ++i) {
        schedule.emplace_back(scale_ladder[i], min_squeeze);
    }

    std::size_t chosen_idx = schedule.size() - 1;
    LayoutCandidate chosen{};
    int cached_em = -1;
    std::unique_ptr<FontMeasurer3x> measurer;

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        const auto [sc, sq] = schedule[i];
        const int em_3x =
            std::max(1, static_cast<int>(std::lround(style.size_px * sc * kSupersampleScaleF)));
        if (em_3x != cached_em) {
            measurer = std::make_unique<FontMeasurer3x>(em_3x, style);
            cached_em = em_3x;
        }
        LayoutCandidate cand =
            EvaluateCandidate(*measurer, paragraphs, safe_bounds, style, sc, sq, em_3x);
        if (cand.fits || i + 1 == schedule.size()) {
            chosen_idx = i;
            chosen = std::move(cand);
            break;
        }
    }

    // Verify on actual rasterised glyph+stroke coverage before committing to target_image;
    // step down if decorative overhangs exceeded metric ABC widths.
    while (chosen.fits && chosen_idx + 1 < schedule.size()) {
        const RasterOutcome dry_run =
            RasterizeCandidate(nullptr, chosen, safe_bounds, canvas_w, canvas_h, style);
        if (!dry_run.foreground_clipped) {
            if (target_image != nullptr) {
                RasterizeCandidate(target_image, chosen, safe_bounds, canvas_w, canvas_h, style);
            }
            return TextLayoutResult{true, chosen.scale, chosen.squeeze,
                                    static_cast<int>(chosen.lines.size()), dry_run.ink_bounds};
        }
        ++chosen_idx;
        const auto [sc, sq] = schedule[chosen_idx];
        const int em_3x =
            std::max(1, static_cast<int>(std::lround(style.size_px * sc * kSupersampleScaleF)));
        if (em_3x != cached_em) {
            measurer = std::make_unique<FontMeasurer3x>(em_3x, style);
            cached_em = em_3x;
        }
        chosen = EvaluateCandidate(*measurer, paragraphs, safe_bounds, style, sc, sq, em_3x);
    }

    const RasterOutcome final_outcome =
        RasterizeCandidate(target_image, chosen, safe_bounds, canvas_w, canvas_h, style);
    const bool final_fits = chosen.fits && !final_outcome.foreground_clipped;
    return TextLayoutResult{final_fits, chosen.scale, chosen.squeeze,
                            static_cast<int>(chosen.lines.size()), final_outcome.ink_bounds};
}

}  // namespace

struct SpriteCanvas::Impl {
    BgraImage image;
};

SpriteCanvas::SpriteCanvas(int width, int height)
    : impl_(std::make_unique<Impl>(Impl{imaging::MakeBgraImage(width, height)})) {}

SpriteCanvas::SpriteCanvas(BgraImage image) {
    if (image.width < 0 || image.height < 0) {
        throw std::invalid_argument("SpriteCanvas: negative image dimensions");
    }
    const std::size_t expected = static_cast<std::size_t>(image.width) *
                                 static_cast<std::size_t>(image.height) *
                                 static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
    if (image.pixels.size() != expected) {
        throw std::invalid_argument("SpriteCanvas: pixel buffer size mismatch");
    }
    impl_ = std::make_unique<Impl>(Impl{std::move(image)});
}

SpriteCanvas::SpriteCanvas(const SpriteCanvas& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : std::make_unique<Impl>()) {}

SpriteCanvas::SpriteCanvas(SpriteCanvas&& other) noexcept = default;

SpriteCanvas& SpriteCanvas::operator=(const SpriteCanvas& other) {
    if (this == &other) {
        return *this;
    }
    impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : std::make_unique<Impl>();
    return *this;
}

SpriteCanvas& SpriteCanvas::operator=(SpriteCanvas&& other) noexcept = default;

SpriteCanvas::~SpriteCanvas() = default;

SpriteCanvas SpriteCanvas::LoadPng(const std::filesystem::path& path) {
    return SpriteCanvas(imaging::LoadPng(path));
}

void SpriteCanvas::Fill(const RectI& region, BgraColor color) {
    const RectI r = imaging::ClampRect(region, impl_->image.width, impl_->image.height);
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            imaging::SetPixel(impl_->image, x, y, color);
        }
    }
}

void SpriteCanvas::EraseWithRowMedian(const RectI& region, int sample_margin_px) {
    const RectI r = imaging::ClampRect(region, impl_->image.width, impl_->image.height);
    if (r.w <= 0 || r.h <= 0 || sample_margin_px <= 0) {
        return;
    }

    std::vector<std::uint8_t> ch_b;
    std::vector<std::uint8_t> ch_g;
    std::vector<std::uint8_t> ch_r;
    std::vector<std::uint8_t> ch_a;
    const std::size_t max_samples = static_cast<std::size_t>(sample_margin_px * kEvenDivisor);
    ch_b.reserve(max_samples);
    ch_g.reserve(max_samples);
    ch_r.reserve(max_samples);
    ch_a.reserve(max_samples);

    const int left_x0 = std::max(0, r.x - sample_margin_px);
    const int left_x1 = r.x;
    const int right_x0 = r.x + r.w;
    const int right_x1 = std::min(impl_->image.width, r.x + r.w + sample_margin_px);

    for (int y = r.y; y < r.y + r.h; ++y) {
        ch_b.clear();
        ch_g.clear();
        ch_r.clear();
        ch_a.clear();
        for (int x = left_x0; x < left_x1; ++x) {
            const BgraColor p = imaging::GetPixel(impl_->image, x, y);
            ch_b.push_back(p.b);
            ch_g.push_back(p.g);
            ch_r.push_back(p.r);
            ch_a.push_back(p.a);
        }
        for (int x = right_x0; x < right_x1; ++x) {
            const BgraColor p = imaging::GetPixel(impl_->image, x, y);
            ch_b.push_back(p.b);
            ch_g.push_back(p.g);
            ch_r.push_back(p.r);
            ch_a.push_back(p.a);
        }
        if (ch_b.empty()) {
            continue;
        }
        const BgraColor med{ComputeChannelMedian(ch_b), ComputeChannelMedian(ch_g),
                            ComputeChannelMedian(ch_r), ComputeChannelMedian(ch_a)};
        for (int x = r.x; x < r.x + r.w; ++x) {
            imaging::SetPixel(impl_->image, x, y, med);
        }
    }
}

void SpriteCanvas::EraseWithBorderInpaint(const RectI& region) {
    const RectI r = imaging::ClampRect(region, impl_->image.width, impl_->image.height);
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    const RectI work = imaging::ClampRect(
        RectI{r.x - kBorderRingPadding, r.y - kBorderRingPadding,
              r.w + kBorderRingSpanAddend, r.h + kBorderRingSpanAddend},
        impl_->image.width, impl_->image.height);

    std::vector<std::uint8_t> unknown(
        static_cast<std::size_t>(work.w) * static_cast<std::size_t>(work.h), 0);
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            const std::size_t idx =
                static_cast<std::size_t>(y - work.y) * static_cast<std::size_t>(work.w) +
                static_cast<std::size_t>(x - work.x);
            unknown[idx] = 1;
        }
    }
    InpaintWorkRegion(impl_->image, work, std::move(unknown));
}

void SpriteCanvas::EraseMaskedWithInpaint(const std::vector<std::uint8_t>& mask) {
    const std::size_t expected =
        static_cast<std::size_t>(impl_->image.width) *
        static_cast<std::size_t>(impl_->image.height);
    if (mask.size() != expected) {
        throw std::invalid_argument("SpriteCanvas::EraseMaskedWithInpaint: mask size mismatch");
    }
    int min_x = impl_->image.width;
    int min_y = impl_->image.height;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < impl_->image.height; ++y) {
        for (int x = 0; x < impl_->image.width; ++x) {
            if (mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(impl_->image.width) +
                     static_cast<std::size_t>(x)] != 0) {
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return;
    }

    const RectI work = imaging::ClampRect(
        RectI{min_x - kBorderRingPadding, min_y - kBorderRingPadding,
              (max_x - min_x + 1) + kBorderRingSpanAddend,
              (max_y - min_y + 1) + kBorderRingSpanAddend},
        impl_->image.width, impl_->image.height);

    std::vector<std::uint8_t> unknown(
        static_cast<std::size_t>(work.w) * static_cast<std::size_t>(work.h), 0);
    for (int ly = 0; ly < work.h; ++ly) {
        for (int lx = 0; lx < work.w; ++lx) {
            const std::size_t src_idx =
                static_cast<std::size_t>(work.y + ly) *
                    static_cast<std::size_t>(impl_->image.width) +
                static_cast<std::size_t>(work.x + lx);
            if (mask[src_idx] != 0) {
                unknown[static_cast<std::size_t>(ly) * static_cast<std::size_t>(work.w) +
                        static_cast<std::size_t>(lx)] = 1;
            }
        }
    }
    InpaintWorkRegion(impl_->image, work, std::move(unknown));
}

void SpriteCanvas::DrawRoundedPill(const RectI& rect, float radius_px, BgraColor fill,
                                   float border_px, BgraColor border) {
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }
    const RectI clipped = imaging::ClampRect(rect, impl_->image.width, impl_->image.height);
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    const double hw = static_cast<double>(rect.w) * kHalfD;
    const double hh = static_cast<double>(rect.h) * kHalfD;
    const double cx = hw;
    const double cy = hh;
    const double r =
        std::clamp(static_cast<double>(radius_px), 0.0, std::min(hw, hh));
    const double b_thick = std::max(0.0, static_cast<double>(border_px));
    const bool has_border = b_thick > 0.0 && border.a > 0;

    const double fill_a = static_cast<double>(fill.a) * kInvChannelMaxD;
    const double fill_pm_b = static_cast<double>(fill.b) * kInvChannelMaxD * fill_a;
    const double fill_pm_g = static_cast<double>(fill.g) * kInvChannelMaxD * fill_a;
    const double fill_pm_r = static_cast<double>(fill.r) * kInvChannelMaxD * fill_a;

    const double bdr_a = static_cast<double>(border.a) * kInvChannelMaxD;
    const double bdr_pm_b = static_cast<double>(border.b) * kInvChannelMaxD * bdr_a;
    const double bdr_pm_g = static_cast<double>(border.g) * kInvChannelMaxD * bdr_a;
    const double bdr_pm_r = static_cast<double>(border.r) * kInvChannelMaxD * bdr_a;

    for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
        const int iy = y - rect.y;
        for (int x = clipped.x; x < clipped.x + clipped.w; ++x) {
            const int ix = x - rect.x;
            double sum_b = 0.0;
            double sum_g = 0.0;
            double sum_r = 0.0;
            double sum_a = 0.0;

            for (int sy = 0; sy < kPillSubdivisions; ++sy) {
                const double py =
                    static_cast<double>(iy) +
                    (kPillSubsampleStep * static_cast<double>(sy) + 1.0) / kPillSubsampleDenom;
                for (int sx = 0; sx < kPillSubdivisions; ++sx) {
                    const double px =
                        static_cast<double>(ix) +
                        (kPillSubsampleStep * static_cast<double>(sx) + 1.0) / kPillSubsampleDenom;
                    const double sd = RoundedRectSignedDistance(px, py, cx, cy, hw, hh, r);
                    if (sd > 0.0) {
                        continue;
                    }
                    if (has_border && sd > -b_thick) {
                        sum_b += bdr_pm_b;
                        sum_g += bdr_pm_g;
                        sum_r += bdr_pm_r;
                        sum_a += bdr_a;
                        continue;
                    }
                    sum_b += fill_pm_b;
                    sum_g += fill_pm_g;
                    sum_r += fill_pm_r;
                    sum_a += fill_a;
                }
            }

            const double avg_a = sum_a * kInvPillSubsampleCount;
            if (ToByteD(avg_a * kChannelMaxD) == 0 || avg_a <= kMinAlphaD) {
                continue;
            }
            const BgraColor dst_px = imaging::GetPixel(impl_->image, x, y);
            const BgraColor out = CompositePremulOverStraight(
                dst_px, static_cast<float>(sum_b * kInvPillSubsampleCount),
                static_cast<float>(sum_g * kInvPillSubsampleCount),
                static_cast<float>(sum_r * kInvPillSubsampleCount), static_cast<float>(avg_a));
            imaging::SetPixel(impl_->image, x, y, out);
        }
    }
}

void SpriteCanvas::BlitCleanRegion(const BgraImage& source, const RectI& source_rect, int dst_x,
                                   int dst_y) {
    const ClippedTransfer tr = ClipTransfer(source.width, source.height, source_rect,
                                            impl_->image.width, impl_->image.height, dst_x, dst_y);
    if (tr.w <= 0 || tr.h <= 0) {
        return;
    }
    if (&source == &impl_->image) {
        const BgraImage copy =
            imaging::CropImage(source, RectI{tr.src_x, tr.src_y, tr.w, tr.h});
        BlitCleanRegion(copy, RectI{0, 0, tr.w, tr.h}, tr.dst_x, tr.dst_y);
        return;
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(tr.w) * static_cast<std::size_t>(imaging::kBgraBytesPerPixel);
    for (int row = 0; row < tr.h; ++row) {
        const auto src_it = source.pixels.begin() +
                            static_cast<std::ptrdiff_t>(
                                imaging::PixelOffset(source, tr.src_x, tr.src_y + row));
        const auto dst_it = impl_->image.pixels.begin() +
                            static_cast<std::ptrdiff_t>(
                                imaging::PixelOffset(impl_->image, tr.dst_x, tr.dst_y + row));
        std::copy(src_it, src_it + static_cast<std::ptrdiff_t>(row_bytes), dst_it);
    }
}

void SpriteCanvas::CompositeSubRect(const BgraImage& source, const RectI& source_rect, int dst_x,
                                    int dst_y) {
    const ClippedTransfer tr = ClipTransfer(source.width, source.height, source_rect,
                                            impl_->image.width, impl_->image.height, dst_x, dst_y);
    if (tr.w <= 0 || tr.h <= 0) {
        return;
    }
    if (&source == &impl_->image) {
        const BgraImage copy =
            imaging::CropImage(source, RectI{tr.src_x, tr.src_y, tr.w, tr.h});
        CompositeSubRect(copy, RectI{0, 0, tr.w, tr.h}, tr.dst_x, tr.dst_y);
        return;
    }
    for (int row = 0; row < tr.h; ++row) {
        for (int col = 0; col < tr.w; ++col) {
            const BgraColor s = imaging::GetPixel(source, tr.src_x + col, tr.src_y + row);
            if (s.a == 0) {
                continue;
            }
            const BgraColor d = imaging::GetPixel(impl_->image, tr.dst_x + col, tr.dst_y + row);
            imaging::SetPixel(impl_->image, tr.dst_x + col, tr.dst_y + row,
                              CompositeStraightOverStraight(d, s));
        }
    }
}

TextLayoutResult SpriteCanvas::DrawText(const std::string& text, const RectI& safe_bounds,
                                        const TextStyle& style) {
    return LayoutAndOptionallyPaint(&impl_->image, impl_->image.width, impl_->image.height, text,
                                    safe_bounds, style);
}

TextLayoutResult SpriteCanvas::MeasureText(const std::string& text, const RectI& safe_bounds,
                                           const TextStyle& style) const {
    return LayoutAndOptionallyPaint(nullptr, impl_->image.width, impl_->image.height, text,
                                    safe_bounds, style);
}

int SpriteCanvas::width() const {
    return impl_->image.width;
}

int SpriteCanvas::height() const {
    return impl_->image.height;
}

BgraColor SpriteCanvas::PixelAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= impl_->image.width || y >= impl_->image.height) {
        throw std::out_of_range("SpriteCanvas::PixelAt coordinates out of range");
    }
    return imaging::GetPixel(impl_->image, x, y);
}

const BgraImage& SpriteCanvas::image() const {
    return impl_->image;
}

void SpriteCanvas::SavePng(const std::filesystem::path& path) const {
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    imaging::SavePng(path, impl_->image);
}

std::vector<std::filesystem::path> SaveContactSheetGrid(
    const std::vector<imaging::ContactSheetCell>& cells, const std::filesystem::path& directory,
    const std::string& stem, const imaging::ContactSheetLayout& layout) {
    return imaging::SaveContactSheetPages(cells, directory, stem, layout);
}

}  // namespace ama::menu
