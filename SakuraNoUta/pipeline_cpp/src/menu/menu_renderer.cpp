// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/menu_renderer.h"

#include "menu/sprite_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace snu::menu {
namespace {

constexpr int kRetailScreenW = 1280;
constexpr int kRetailScreenH = 720;

// ---- Splash screen constants (1280x720) ------------------------------------
constexpr int kSplashLogoCleanDonorMinX = 770;
constexpr int kSplashLogoCleanDonorMinY = 475;
constexpr RectI kSplashWorkspaceRect{64, 30, 1168, 636};
constexpr int kSplashNavyCoreDilateRadiusPx = 6;
constexpr int kSplashGlowBasisCount = 8;
constexpr float kSplashCalibMinDistPx = 9.5f;
constexpr float kSplashCalibMaxDistPx = 46.0f;
constexpr float kSplashCalibOtherMinDistPx = 52.0f;
constexpr float kSplashGlowMaxAlpha = 0.80f;
constexpr float kSplashGlowBlendHalfWidth = 0.05f;
constexpr int kSplashSingleHaloDilatePx = 5;

constexpr RectI kSplashSubtitleMaskRect{888, 645, 246, 16};
constexpr RectI kSplashSubtitleDrawRect{880, 642, 260, 21};
constexpr std::uint8_t kSplashSubtitleInkMaxChannel = 175u;
constexpr int kSplashSubtitleDilatePx = 1;

constexpr BgraColor kSplashNavyFill{0x78, 0x3B, 0x0C, 0xFF};
constexpr BgraColor kSplashGlowStroke{0xFF, 0xFF, 0xFF, 0xF0};
constexpr BgraColor kSplashGlowShadow{0xFF, 0xFF, 0xFF, 0xD8};
constexpr float kSplashBaseFontSizePx = 25.5f;
constexpr float kSplashMinFontSizePx = 16.0f;
constexpr float kSplashStrokePx = 2.6f;
constexpr float kSplashShadowBlurPx = 3.5f;
constexpr float kSplashLineSpacing = 1.24f;

// ---- Title & Locked CG constants -------------------------------------------
constexpr RectI kTitleSubtitleMaskRect{232, 290, 332, 24};
constexpr RectI kTitleSubtitleDrawRect{204, 291, 374, 24};
constexpr RectI kTitleCaptionDrawRect{225, 323, 335, 25};
constexpr std::uint8_t kTitleSubtitleInkMaxRG = 140u;
constexpr int kTitleSubtitleDilatePx = 2;

constexpr int kLockedCgRetailW = 291;
constexpr int kLockedCgRetailH = 170;
constexpr RectI kLockedCgMaskRect{90, 111, 112, 12};
constexpr RectI kLockedCgDrawRect{86, 108, 120, 16};
constexpr std::uint8_t kLockedCgInkMaxR = 155u;
constexpr std::uint8_t kLockedCgInkMaxG = 185u;

// ---- Config background constants (1280x720) --------------------------------
constexpr int kConfigRowPitchY = 64;
constexpr int kConfigLeftMaskX = 44;
constexpr int kConfigLeftMaskW = 238;
constexpr int kConfigLeftDrawX = 46;
constexpr int kConfigLeftDrawW = 232;
constexpr int kConfigRightMaskX = 656;
constexpr int kConfigRightMaskW = 236;
constexpr int kConfigRightDrawX = 658;
constexpr int kConfigRightDrawW = 232;
constexpr int kConfigRowMaskBaseY = 124;
constexpr int kConfigRowMaskH = 26;
constexpr int kConfigRowDrawBaseY = 116;
constexpr int kConfigRowDrawH = 40;

constexpr int kConfigLeftMinMaskX = 312;
constexpr int kConfigLeftMinMaskW = 56;
constexpr int kConfigLeftMinDrawX = 300;
constexpr int kConfigLeftMinDrawW = 76;
constexpr int kConfigLeftMaxMaskX = 568;
constexpr int kConfigLeftMaxMaskW = 52;
constexpr int kConfigLeftMaxDrawX = 552;
constexpr int kConfigLeftMaxDrawW = 74;

constexpr int kConfigRightMinMaskX = 910;
constexpr int kConfigRightMinMaskW = 44;
constexpr int kConfigRightMinDrawX = 898;
constexpr int kConfigRightMinDrawW = 64;
constexpr int kConfigRightMaxMaskX = 1160;
constexpr int kConfigRightMaxMaskW = 50;
constexpr int kConfigRightMaxDrawX = 1150;
constexpr int kConfigRightMaxDrawW = 70;

constexpr int kConfigSliderMaskBaseY = 144;
constexpr int kConfigSliderMaskH = 18;
constexpr int kConfigSliderDrawBaseY = 140;
constexpr int kConfigSliderDrawH = 23;

constexpr RectI kConfigVertVoicesMaskRect{675, 428, 28, 186};
constexpr RectI kConfigVertVoicesDrawRect{668, 412, 42, 218};
constexpr int kConfigInkDilatePx = 2;

// ---- Config toggle strip constants (552x37, 4 cells of 138x37) -------------
constexpr int kToggleCellCount = 4;
constexpr int kToggleRetailCellW = 138;
constexpr int kToggleInnerPadX = 14;
constexpr int kToggleInnerPadY = 5;
constexpr std::uint8_t kToggleDarkInkThreshold = 140u;
constexpr int kToggleMaskDilatePx = 3;

// ---- Config voice portrait constants (200x130, 2 cells of 100x130) ---------
constexpr int kPortraitCellCount = 2;
constexpr int kPortraitRetailCellW = 100;
constexpr int kPortraitRetailH = 130;
constexpr int kPortraitCell0DilatePx = 4;
constexpr int kPortraitSilhouetteDilatePx = 3;
constexpr int kPortraitCell1DilatePx = 3;

// ---- Confirmation dialog constants (722x242) -------------------------------
constexpr std::uint8_t kDialogOrangeMinR = 190u;
constexpr std::uint8_t kDialogOrangeMinG = 90u;
constexpr std::uint8_t kDialogOrangeMaxG = 210u;
constexpr std::uint8_t kDialogOrangeMaxB = 100u;
constexpr int kDialogWipeOffsetFromIconPx = 8;
constexpr int kDialogTextOffsetFromIconPx = 14;
constexpr int kDialogWipeRightLimitX = 616;
constexpr int kDialogWipeTopPadPx = 6;
constexpr int kDialogWipeHeightPx = 36;
constexpr BgraColor kDialogPromptColor{95, 95, 95, 255};

// ---- Tooltip constants (440x20) --------------------------------------------
constexpr int kTooltipEndcapPadX = 16;
constexpr int kTooltipBorderPadY = 1;
constexpr BgraColor kTooltipTextColor{248, 250, 252, 255};

// ---- Music banner constants (864x42, 3 cells of 288x42) --------------------
constexpr int kMusicCellCount = 3;
constexpr RectI kMusicSakuraIconCropRect{576 + 5, 11, 21, 20};
constexpr int kMusicSakuraGapPx = 5;
constexpr int kMusicCellPadX = 8;
constexpr int kMusicCellPadY = 3;
constexpr int kMusicCell2MaxTextW = 244;
constexpr BgraColor kMusicTransparentBg{0, 0, 0, 0};
constexpr BgraColor kMusicCell0Charcoal{18, 15, 15, 255};
constexpr BgraColor kMusicCell1Violet{131, 7, 146, 255};
constexpr BgraColor kMusicCell2RosePink{162, 104, 234, 255};

// ---- Choice option plate constants (2982x58, 3 states of 994x58) -----------
constexpr int kChoiceStateCount = 3;
constexpr int kChoiceTextLeftPx = 93;
constexpr int kChoiceTextRightMarginPx = 20;
constexpr int kChoiceMaxPointSize = 26;
constexpr int kChoiceMinPointSize = 12;
constexpr std::array<std::uint8_t, kChoiceStateCount> kChoiceTileAlpha = {
    130u, 182u, 182u};

RectI ClampRectToImage(const RectI& r, int img_w, int img_h) noexcept {
    if (img_w <= 0 || img_h <= 0 || r.empty()) {
        return {};
    }
    const int x0 = std::clamp(r.x, 0, img_w);
    const int y0 = std::clamp(r.y, 0, img_h);
    const int x1 = std::clamp(r.x + r.w, x0, img_w);
    const int y1 = std::clamp(r.y + r.h, y0, img_h);
    return RectI{x0, y0, x1 - x0, y1 - y0};
}

RectI ScaleRectFromRetail(const RectI& r, int actual_w, int actual_h,
                          int ref_w = kRetailScreenW,
                          int ref_h = kRetailScreenH) noexcept {
    if (actual_w == ref_w && actual_h == ref_h) {
        return ClampRectToImage(r, actual_w, actual_h);
    }
    const double sx = static_cast<double>(actual_w) / static_cast<double>(ref_w);
    const double sy = static_cast<double>(actual_h) / static_cast<double>(ref_h);
    const int x = static_cast<int>(std::lround(r.x * sx));
    const int y = static_cast<int>(std::lround(r.y * sy));
    const int w = std::max(1, static_cast<int>(std::lround(r.w * sx)));
    const int h = std::max(1, static_cast<int>(std::lround(r.h * sy)));
    return ClampRectToImage(RectI{x, y, w, h}, actual_w, actual_h);
}

std::vector<std::uint8_t> DilateMaskBox(const std::vector<std::uint8_t>& src,
                                        int w, int h, int radius) {
    if (radius <= 0 || w <= 0 || h <= 0 || src.empty()) {
        return src;
    }
    std::vector<std::uint8_t> horiz(static_cast<std::size_t>(w * h), 0u);
    for (int y = 0; y < h; ++y) {
        int acc = 0;
        const int row = y * w;
        for (int x = 0; x <= std::min(w - 1, radius); ++x) {
            acc += (src[static_cast<std::size_t>(row + x)] != 0u) ? 1 : 0;
        }
        for (int x = 0; x < w; ++x) {
            if (acc > 0) {
                horiz[static_cast<std::size_t>(row + x)] = 1u;
            }
            const int out_x = x - radius;
            if (out_x >= 0 && src[static_cast<std::size_t>(row + out_x)] != 0u) {
                --acc;
            }
            const int in_x = x + radius + 1;
            if (in_x < w && src[static_cast<std::size_t>(row + in_x)] != 0u) {
                ++acc;
            }
        }
    }

    std::vector<std::uint8_t> out(static_cast<std::size_t>(w * h), 0u);
    for (int x = 0; x < w; ++x) {
        int acc = 0;
        for (int y = 0; y <= std::min(h - 1, radius); ++y) {
            acc += (horiz[static_cast<std::size_t>(y * w + x)] != 0u) ? 1 : 0;
        }
        for (int y = 0; y < h; ++y) {
            if (acc > 0) {
                out[static_cast<std::size_t>(y * w + x)] = 1u;
            }
            const int out_y = y - radius;
            if (out_y >= 0 && horiz[static_cast<std::size_t>(out_y * w + x)] != 0u) {
                --acc;
            }
            const int in_y = y + radius + 1;
            if (in_y < h && horiz[static_cast<std::size_t>(in_y * w + x)] != 0u) {
                ++acc;
            }
        }
    }
    return out;
}

bool HasAnyMaskedPixel(const std::vector<std::uint8_t>& mask) noexcept {
    return std::any_of(mask.begin(), mask.end(),
                       [](std::uint8_t v) { return v != 0u; });
}

std::vector<std::uint8_t> ErodeMaskBox(const std::vector<std::uint8_t>& src,
                                       int w, int h, int radius) {
    if (radius <= 0 || w <= 0 || h <= 0 || src.empty()) {
        return src;
    }
    std::vector<std::uint8_t> inv(src.size(), 0u);
    for (std::size_t i = 0u; i < src.size(); ++i) {
        inv[i] = (src[i] == 0u) ? 1u : 0u;
    }
    const std::vector<std::uint8_t> dil_inv = DilateMaskBox(inv, w, h, radius);
    std::vector<std::uint8_t> out(src.size(), 0u);
    for (std::size_t i = 0u; i < src.size(); ++i) {
        out[i] = (dil_inv[i] == 0u) ? 1u : 0u;
    }
    return out;
}

void BlurFloatPlaneBox1Pass(const std::vector<float>& src,
                            std::vector<float>& dst,
                            int w, int h, int radius) {
    std::vector<float> horiz(static_cast<std::size_t>(w * h), 0.0f);
    const float inv_diam = 1.0f / static_cast<float>(2 * radius + 1);
    for (int y = 0; y < h; ++y) {
        const int row = y * w;
        float acc = 0.0f;
        for (int k = -radius; k <= radius; ++k) {
            const int sx = std::clamp(k, 0, w - 1);
            acc += src[static_cast<std::size_t>(row + sx)];
        }
        for (int x = 0; x < w; ++x) {
            horiz[static_cast<std::size_t>(row + x)] = acc * inv_diam;
            const int out_x = std::clamp(x - radius, 0, w - 1);
            const int in_x = std::clamp(x + radius + 1, 0, w - 1);
            acc += src[static_cast<std::size_t>(row + in_x)] -
                   src[static_cast<std::size_t>(row + out_x)];
        }
    }
    dst.assign(static_cast<std::size_t>(w * h), 0.0f);
    for (int x = 0; x < w; ++x) {
        float acc = 0.0f;
        for (int k = -radius; k <= radius; ++k) {
            const int sy = std::clamp(k, 0, h - 1);
            acc += horiz[static_cast<std::size_t>(sy * w + x)];
        }
        for (int y = 0; y < h; ++y) {
            dst[static_cast<std::size_t>(y * w + x)] = acc * inv_diam;
            const int out_y = std::clamp(y - radius, 0, h - 1);
            const int in_y = std::clamp(y + radius + 1, 0, h - 1);
            acc += horiz[static_cast<std::size_t>(in_y * w + x)] -
                   horiz[static_cast<std::size_t>(out_y * w + x)];
        }
    }
}

std::vector<float> BlurFloatPlaneGaussian3Pass(const std::vector<float>& src,
                                               int w, int h, int radius) {
    if (radius <= 0 || w <= 0 || h <= 0 || src.empty()) {
        return src;
    }
    std::vector<float> pass1;
    std::vector<float> pass2;
    std::vector<float> pass3;
    BlurFloatPlaneBox1Pass(src, pass1, w, h, radius);
    BlurFloatPlaneBox1Pass(pass1, pass2, w, h, radius);
    BlurFloatPlaneBox1Pass(pass2, pass3, w, h, radius);
    return pass3;
}

// Exact 2D Euclidean Distance Transform in IEEE-754 double precision (prevents
// float32 mantissa loss on empty columns!) returning pixel distance to `mask`.
std::vector<float> ComputeExactEdtPx(const std::vector<std::uint8_t>& mask,
                                     int w, int h) {
    constexpr double kInf = 1.0e6;
    const std::size_t n = static_cast<std::size_t>(w * h);
    std::vector<double> col_sq(n, kInf * kInf);

    for (int x = 0; x < w; ++x) {
        double d = kInf;
        for (int y = 0; y < h; ++y) {
            const std::size_t idx = static_cast<std::size_t>(y * w + x);
            d = (mask[idx] != 0u) ? 0.0 : (d + 1.0);
            col_sq[idx] = d;
        }
        d = kInf;
        for (int y = h - 1; y >= 0; --y) {
            const std::size_t idx = static_cast<std::size_t>(y * w + x);
            d = std::min(col_sq[idx], d + 1.0);
            col_sq[idx] = d * d;
        }
    }

    std::vector<float> edt(n, static_cast<float>(kInf));
    std::vector<int> v(static_cast<std::size_t>(w), 0);
    std::vector<double> z(static_cast<std::size_t>(w + 1), 0.0);

    for (int y = 0; y < h; ++y) {
        const int row = y * w;
        int k = 0;
        v[0] = 0;
        z[0] = -1.0e30;
        z[1] = 1.0e30;
        for (int q = 1; q < w; ++q) {
            const double fq = col_sq[static_cast<std::size_t>(row + q)] +
                              static_cast<double>(q * q);
            double s = 0.0;
            while (k >= 0) {
                const int vk = v[static_cast<std::size_t>(k)];
                const double fvk = col_sq[static_cast<std::size_t>(row + vk)] +
                                   static_cast<double>(vk * vk);
                s = (fq - fvk) / static_cast<double>(2 * (q - vk));
                if (s > z[static_cast<std::size_t>(k)]) {
                    break;
                }
                --k;
            }
            ++k;
            v[static_cast<std::size_t>(k)] = q;
            z[static_cast<std::size_t>(k)] = s;
            z[static_cast<std::size_t>(k + 1)] = 1.0e30;
        }
        k = 0;
        for (int q = 0; q < w; ++q) {
            while (z[static_cast<std::size_t>(k + 1)] < static_cast<double>(q)) {
                ++k;
            }
            const int vk = v[static_cast<std::size_t>(k)];
            const double dx = static_cast<double>(q - vk);
            const double d2 =
                dx * dx + col_sq[static_cast<std::size_t>(row + vk)];
            edt[static_cast<std::size_t>(row + q)] =
                static_cast<float>(std::sqrt(std::max(0.0, d2)));
        }
    }
    return edt;
}

// Uses solid Euclidean-distance ribbons (`edt <= 4.5`, `edt <= 8.5`)
// rather than raw thin stroke masks so dense kanji (`護`, `齢`) and sparse kana
// (`の`, `。`) have identical `1.0` interior amplitude without density over-sum.
std::array<std::vector<float>, kSplashGlowBasisCount> BuildSplashGlowBasis2D(
    const std::vector<float>& edt, int w, int h) {
    const std::size_t n = edt.size();
    std::vector<float> f_rib4(n, 0.0f);
    std::vector<float> f_rib8(n, 0.0f);
    for (std::size_t i = 0u; i < n; ++i) {
        f_rib4[i] = (edt[i] <= 4.5f) ? 1.0f : 0.0f;
        f_rib8[i] = (edt[i] <= 8.5f) ? 1.0f : 0.0f;
    }
    return {
        BlurFloatPlaneGaussian3Pass(f_rib4, w, h, 2),
        BlurFloatPlaneGaussian3Pass(f_rib4, w, h, 4),
        BlurFloatPlaneGaussian3Pass(f_rib4, w, h, 7),
        BlurFloatPlaneGaussian3Pass(f_rib4, w, h, 11),
        BlurFloatPlaneGaussian3Pass(f_rib4, w, h, 16),
        BlurFloatPlaneGaussian3Pass(f_rib4, w, h, 22),
        BlurFloatPlaneGaussian3Pass(f_rib4, w, h, 30),
        BlurFloatPlaneGaussian3Pass(f_rib8, w, h, 5),
    };
}

std::array<float, kSplashGlowBasisCount> SolveNonNegativeLeastSquares(
    const std::array<std::array<double, kSplashGlowBasisCount>,
                     kSplashGlowBasisCount>& ata,
    const std::array<double, kSplashGlowBasisCount>& atb) noexcept {
    std::array<double, kSplashGlowBasisCount> w{};
    for (int iter = 0; iter < 500; ++iter) {
        for (std::size_t m = 0u;
             m < static_cast<std::size_t>(kSplashGlowBasisCount); ++m) {
            const double diag = ata[m][m];
            if (diag <= 1e-9) {
                continue;
            }
            double rhs = atb[m];
            for (std::size_t k = 0u;
                 k < static_cast<std::size_t>(kSplashGlowBasisCount); ++k) {
                if (k != m) {
                    rhs -= ata[m][k] * w[k];
                }
            }
            w[m] = std::max(0.0, rhs / diag);
        }
    }
    std::array<float, kSplashGlowBasisCount> out{};
    for (std::size_t m = 0u; m < out.size(); ++m) {
        out[m] = static_cast<float>(w[m]);
    }
    return out;
}

int TriangleWaveReflect(int step, int period) noexcept {
    if (period <= 0) {
        return 0;
    }
    const int two_p = 2 * period;
    int m = step % two_p;
    if (m < 0) {
        m += two_p;
    }
    return (m <= period) ? m : (two_p - m);
}

BgraColor InvertWhiteGlowPixel(BgraColor src, float glow_alpha) noexcept {
    const float a = std::clamp(glow_alpha, 0.0f, kSplashGlowMaxAlpha);
    if (a <= 1e-4f) {
        return src;
    }
    // Equal R/G/B headroom gain (`out_hr = hr * gain`) preserves exact sakura
    // blossom pink and hedgerow green chromaticity without grey desaturation,
    // tapering smoothly when 8-bit headroom is <= 3 LSB to avoid quantization speckle.
    const int min_ch = std::min({static_cast<int>(src.r), static_cast<int>(src.g),
                                 static_cast<int>(src.b)});
    const int max_hr = 255 - min_ch;
    const float raw_gain = 1.0f / (1.0f - a);
    const float taper = std::clamp(static_cast<float>(max_hr) / 4.0f, 0.0f, 1.0f);
    const float gain = 1.0f + (raw_gain - 1.0f) * taper;
    const auto restore = [&](std::uint8_t c) noexcept -> std::uint8_t {
        const float out_hr = (255.0f - static_cast<float>(c)) * gain;
        return static_cast<std::uint8_t>(
            std::clamp(std::lround(255.0f - out_hr), 70L, 255L));
    };
    return BgraColor{
        restore(src.b),
        restore(src.g),
        restore(src.r),
        kOpaqueAlpha,
    };
}

float SmoothStep01(float t) noexcept {
    const float u = std::clamp(t, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

void BlurFloatPlaneBoxAnisotropic1Pass(const std::vector<float>& src,
                                       std::vector<float>& dst,
                                       int w, int h, int rx, int ry) {
    std::vector<float> horiz(static_cast<std::size_t>(w * h), 0.0f);
    if (rx > 0) {
        const float inv_w = 1.0f / static_cast<float>(2 * rx + 1);
        for (int y = 0; y < h; ++y) {
            const int row = y * w;
            float acc = 0.0f;
            for (int k = -rx; k <= rx; ++k) {
                acc += src[static_cast<std::size_t>(row + std::clamp(k, 0, w - 1))];
            }
            for (int x = 0; x < w; ++x) {
                horiz[static_cast<std::size_t>(row + x)] = acc * inv_w;
                const int out_x = std::clamp(x - rx, 0, w - 1);
                const int in_x = std::clamp(x + rx + 1, 0, w - 1);
                acc += src[static_cast<std::size_t>(row + in_x)] -
                       src[static_cast<std::size_t>(row + out_x)];
            }
        }
    } else {
        horiz = src;
    }

    if (ry > 0) {
        dst.assign(static_cast<std::size_t>(w * h), 0.0f);
        const float inv_h = 1.0f / static_cast<float>(2 * ry + 1);
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int k = -ry; k <= ry; ++k) {
                acc += horiz[static_cast<std::size_t>(std::clamp(k, 0, h - 1) * w + x)];
            }
            for (int y = 0; y < h; ++y) {
                dst[static_cast<std::size_t>(y * w + x)] = acc * inv_h;
                const int out_y = std::clamp(y - ry, 0, h - 1);
                const int in_y = std::clamp(y + ry + 1, 0, h - 1);
                acc += horiz[static_cast<std::size_t>(in_y * w + x)] -
                       horiz[static_cast<std::size_t>(out_y * w + x)];
            }
        }
    } else {
        dst = std::move(horiz);
    }
}

std::vector<float> BlurFloatPlaneAnisotropic2Pass(const std::vector<float>& src,
                                                  int w, int h, int rx, int ry) {
    if (w <= 0 || h <= 0 || src.empty()) {
        return src;
    }
    std::vector<float> pass1;
    std::vector<float> pass2;
    BlurFloatPlaneBoxAnisotropic1Pass(src, pass1, w, h, rx, ry);
    BlurFloatPlaneBoxAnisotropic1Pass(pass1, pass2, w, h, rx, ry);
    return pass2;
}

bool IsSplashNavyInk(BgraColor p) noexcept {
    const int r = static_cast<int>(p.r);
    const int g = static_cast<int>(p.g);
    const int b = static_cast<int>(p.b);
    return (b - r >= 26) && (b - g >= 16) && (r < 145) && (g < 165);
}

bool IsConfigCharcoalInk(BgraColor p) noexcept {
    const int min_ch = std::min({static_cast<int>(p.r), static_cast<int>(p.g),
                                 static_cast<int>(p.b)});
    if (min_ch >= 218) {
        return false;
    }
    // Preserve glowing blue-white feather pixels (high B with distinct blue tint).
    if (p.b >= 222u && static_cast<int>(p.b) - static_cast<int>(p.r) >= 14) {
        return false;
    }
    return true;
}

template <typename Pred>
void InpaintSubrectIfPred(SpriteCanvas& canvas, const RectI& raw_rect,
                          int dilate_radius, Pred pred) {
    const RectI rect = ClampRectToImage(raw_rect, canvas.width(), canvas.height());
    if (rect.empty()) {
        return;
    }
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(rect.w * rect.h), 0u);
    for (int ry = 0; ry < rect.h; ++ry) {
        for (int rx = 0; rx < rect.w; ++rx) {
            if (pred(canvas.PixelAt(rect.x + rx, rect.y + ry))) {
                mask[static_cast<std::size_t>(ry * rect.w + rx)] = 1u;
            }
        }
    }
    mask = DilateMaskBox(mask, rect.w, rect.h, dilate_radius);
    if (HasAnyMaskedPixel(mask)) {
        canvas.InpaintMaskedLaplaceSor(rect, mask);
    }
}

BgraImage EnsureOpaqueAlpha(const BgraImage& src) {
    BgraImage out = src;
    const int w = out.width();
    const int h = out.height();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            BgraColor p = out.PixelAt(x, y);
            if (p.a != kOpaqueAlpha) {
                p.a = kOpaqueAlpha;
                out.SetPixel(x, y, p);
            }
        }
    }
    return out;
}

int PortraitVerticalEraseHeight(std::string_view entry_name) noexcept {
    if (entry_name == "SGCnfgWnd100000" || entry_name == "SGCnfgWnd100100" ||
        entry_name == "SGCnfgWnd100300") {
        return 68;
    }
    if (entry_name == "SGCnfgWnd100200" || entry_name == "SGCnfgWnd100400") {
        return 86;
    }
    return 103;
}

BgraImage CropStripCell(const BgraImage& strip, int total_cells, int cell_index) {
    if (strip.empty() || total_cells <= 0) {
        return strip;
    }
    const int cw = std::max(1, strip.width() / total_cells);
    const int idx = std::clamp(cell_index, 0, total_cells - 1);
    return strip.Crop(RectI{idx * cw, 0, cw, strip.height()});
}

}  // namespace

struct MenuRenderer::Impl {
    std::wstring ui_sans_family;
    std::wstring serif_family;

    Impl()
        : ui_sans_family(ResolveInstalledFontFamily(
              {L"Segoe UI", L"Calibri", L"Trebuchet MS", L"Arial"})),
          serif_family(ResolveInstalledFontFamily(
              {L"Georgia", L"Palatino Linotype", L"Times New Roman", L"Segoe UI"})) {}

    void LocalizeSplashLogoSubtitle(SpriteCanvas& canvas) const {
        const RectI mask_rect = ScaleRectFromRetail(
            kSplashSubtitleMaskRect, canvas.width(), canvas.height());
        InpaintSubrectIfPred(canvas, mask_rect, kSplashSubtitleDilatePx,
                             [](BgraColor p) {
                                 return std::max({p.r, p.g, p.b}) <
                                        kSplashSubtitleInkMaxChannel;
                             });

        const RectI draw_rect = ScaleRectFromRetail(
            kSplashSubtitleDrawRect, canvas.width(), canvas.height());
        if (draw_rect.empty()) {
            return;
        }
        TextStyle sub;
        sub.font_family = serif_family;
        sub.weight = kDefaultFontWeightSemiBold;
        sub.size_px = std::max(
            7.0f, 11.5f * static_cast<float>(canvas.height()) /
                      static_cast<float>(kRetailScreenH));
        sub.min_font_size_px = 6.5f;
        sub.fill = BgraColor{0x4E, 0x24, 0x14, 0xFF};
        sub.stroke_px = 1.1f;
        sub.stroke_color = BgraColor{0xFF, 0xFF, 0xFF, 0xDC};
        sub.halign = HorizontalAlign::kCenter;
        sub.valign = VerticalAlign::kCenter;
        sub.size_px = canvas.AutoFitFontSize(kLogoSubtitleEn, draw_rect, sub, 2);
        canvas.DrawText(kLogoSubtitleEn, draw_rect, sub);
    }

    void DrawSplashParagraphs(SpriteCanvas& canvas,
                              const SplashScreenSpec& spec) const {
        const float scale_y =
            static_cast<float>(canvas.height()) / static_cast<float>(kRetailScreenH);
        TextStyle style;
        style.font_family = ui_sans_family;
        style.weight = kDefaultFontWeightSemiBold;
        style.size_px = std::max(10.0f, kSplashBaseFontSizePx * scale_y);
        style.min_font_size_px = std::max(8.0f, kSplashMinFontSizePx * scale_y);
        style.line_spacing = kSplashLineSpacing;
        style.fill = kSplashNavyFill;
        style.stroke_px = std::max(1.0f, kSplashStrokePx * scale_y);
        style.stroke_color = kSplashGlowStroke;
        style.shadow = true;
        style.shadow_dx = 0.0f;
        style.shadow_dy = 0.0f;
        style.shadow_blur_px = std::max(1.2f, kSplashShadowBlurPx * scale_y);
        style.shadow_color = kSplashGlowShadow;
        style.halign = HorizontalAlign::kLeft;
        style.valign = VerticalAlign::kCenter;
        style.wrap = false;

        float unified_size = style.size_px;
        std::array<RectI, kSplashParagraphCount> scaled_rects{};
        for (std::size_t i = 0u; i < spec.paragraphs.size(); ++i) {
            scaled_rects[i] = ScaleRectFromRetail(
                spec.paragraphs[i].bounds, canvas.width(), canvas.height());
            if (!scaled_rects[i].empty()) {
                unified_size = std::min(
                    unified_size,
                    canvas.AutoFitFontSize(spec.paragraphs[i].text_en,
                                           scaled_rects[i], style, 4));
            }
        }
        style.size_px = unified_size;
        for (std::size_t i = 0u; i < spec.paragraphs.size(); ++i) {
            if (!scaled_rects[i].empty()) {
                canvas.DrawText(spec.paragraphs[i].text_en, scaled_rects[i], style);
            }
        }
    }
};

MenuRenderer::MenuRenderer() : impl_(std::make_unique<Impl>()) {}
MenuRenderer::MenuRenderer(MenuRenderer&& other) noexcept = default;
MenuRenderer& MenuRenderer::operator=(MenuRenderer&& other) noexcept = default;
MenuRenderer::~MenuRenderer() = default;

SplashPairRenderResult MenuRenderer::RenderSplashScreenPair(
    const BgraImage& orig_att01, const BgraImage& orig_att02) const {
    const SplashScreenSpec* spec1 = FindSplashScreenSpec(kSplash1EntryName);
    const SplashScreenSpec* spec2 = FindSplashScreenSpec(kSplash2EntryName);
    if (spec1 == nullptr || spec2 == nullptr) {
        throw std::logic_error("SplashScreenCatalog missing att01/att02");
    }
    if (orig_att01.empty() || orig_att02.empty() ||
        orig_att01.width() != orig_att02.width() ||
        orig_att01.height() != orig_att02.height()) {
        return SplashPairRenderResult{
            RenderSingleSplashScreen(orig_att01, *spec1),
            RenderSingleSplashScreen(orig_att02, *spec2),
        };
    }

    const BgraImage opaque_att01 = EnsureOpaqueAlpha(orig_att01);
    const BgraImage opaque_att02 = EnsureOpaqueAlpha(orig_att02);
    const int w = opaque_att01.width();
    const int h = opaque_att01.height();
    if (w != kRetailScreenW || h != kRetailScreenH) {
        return SplashPairRenderResult{
            RenderSingleSplashScreen(opaque_att01, *spec1),
            RenderSingleSplashScreen(opaque_att02, *spec2),
        };
    }

    const RectI ws = ClampRectToImage(kSplashWorkspaceRect, w, h);
    const std::size_t ws_n = static_cast<std::size_t>(ws.w * ws.h);
    std::vector<std::uint8_t> navy1(ws_n, 0u);
    std::vector<std::uint8_t> navy2(ws_n, 0u);

    for (int ry = 0; ry < ws.h; ++ry) {
        const int y = ws.y + ry;
        for (int rx = 0; rx < ws.w; ++rx) {
            const int x = ws.x + rx;
            if (x >= kSplashLogoCleanDonorMinX && y >= kSplashLogoCleanDonorMinY) {
                continue;
            }
            const std::size_t idx = static_cast<std::size_t>(ry * ws.w + rx);
            if (IsSplashNavyInk(opaque_att01.PixelAt(x, y))) {
                navy1[idx] = 1u;
            }
            if (IsSplashNavyInk(opaque_att02.PixelAt(x, y))) {
                navy2[idx] = 1u;
            }
        }
    }

    const std::vector<std::uint8_t> ink1 =
        DilateMaskBox(navy1, ws.w, ws.h, kSplashNavyCoreDilateRadiusPx);
    const std::vector<std::uint8_t> ink2 =
        DilateMaskBox(navy2, ws.w, ws.h, kSplashNavyCoreDilateRadiusPx);

    // Stage 1: Exact double-precision 2D EDT + 8-scale non-negative 2D Gaussian
    // outer-glow basis (`r = 2..30`) calibrated via NNLS on single-screen glow
    // pixels. Non-negative 2D convolutions match Photoshop's exact line-endcap
    // falloff (zero dark rings!) while `r = 30` captures the outer glow skirt.
    const std::vector<float> dist1 = ComputeExactEdtPx(navy1, ws.w, ws.h);
    const std::vector<float> dist2 = ComputeExactEdtPx(navy2, ws.w, ws.h);
    const auto basis1 = BuildSplashGlowBasis2D(dist1, ws.w, ws.h);
    const auto basis2 = BuildSplashGlowBasis2D(dist2, ws.w, ws.h);

    std::array<std::array<double, kSplashGlowBasisCount>, kSplashGlowBasisCount>
        ata{};
    std::array<double, kSplashGlowBasisCount> atb{};
    for (std::size_t k = 0u; k < static_cast<std::size_t>(kSplashGlowBasisCount);
         ++k) {
        ata[k][k] = 1.0;
    }

    auto accumulate_calib = [&](std::size_t idx, BgraColor glow_px,
                                BgraColor clean_px,
                                const auto& basis) noexcept {
        const std::array<int, 3> dg = {
            static_cast<int>(glow_px.b) - static_cast<int>(clean_px.b),
            static_cast<int>(glow_px.g) - static_cast<int>(clean_px.g),
            static_cast<int>(glow_px.r) - static_cast<int>(clean_px.r),
        };
        const std::array<int, 3> hr = {
            255 - static_cast<int>(clean_px.b),
            255 - static_cast<int>(clean_px.g),
            255 - static_cast<int>(clean_px.r),
        };
        bool used = false;
        const std::array<int, 3> glow_ch = {
            static_cast<int>(glow_px.b),
            static_cast<int>(glow_px.g),
            static_cast<int>(glow_px.r),
        };
        for (std::size_t ch = 0u; ch < 3u; ++ch) {
            if (hr[ch] < 35 || glow_ch[ch] >= 252) {
                continue;
            }
            used = true;
            const double w_hr = static_cast<double>(hr[ch]);
            const double obs = static_cast<double>(dg[ch]);
            std::array<double, kSplashGlowBasisCount> row{};
            for (std::size_t m = 0u;
                 m < static_cast<std::size_t>(kSplashGlowBasisCount); ++m) {
                row[m] = w_hr * static_cast<double>(basis[m][idx]);
            }
            for (std::size_t r = 0u;
                 r < static_cast<std::size_t>(kSplashGlowBasisCount); ++r) {
                atb[r] += row[r] * obs;
                for (std::size_t c = r;
                     c < static_cast<std::size_t>(kSplashGlowBasisCount); ++c) {
                    ata[r][c] += row[r] * row[c];
                }
            }
        }
        return used;
    };

    std::size_t calib_pixels = 0u;
    for (int ry = 0; ry < ws.h; ++ry) {
        const int y = ws.y + ry;
        for (int rx = 0; rx < ws.w; ++rx) {
            const int x = ws.x + rx;
            if (x >= kSplashLogoCleanDonorMinX && y >= kSplashLogoCleanDonorMinY) {
                continue;
            }
            const std::size_t idx = static_cast<std::size_t>(ry * ws.w + rx);
            const float d1 = dist1[idx];
            const float d2 = dist2[idx];
            if (d1 >= kSplashCalibMinDistPx && d1 <= kSplashCalibMaxDistPx &&
                d2 >= kSplashCalibOtherMinDistPx) {
                if (accumulate_calib(idx, opaque_att01.PixelAt(x, y),
                                     opaque_att02.PixelAt(x, y), basis1)) {
                    ++calib_pixels;
                }
            } else if (d2 >= kSplashCalibMinDistPx &&
                       d2 <= kSplashCalibMaxDistPx &&
                       d1 >= kSplashCalibOtherMinDistPx) {
                if (accumulate_calib(idx, opaque_att02.PixelAt(x, y),
                                     opaque_att01.PixelAt(x, y), basis2)) {
                    ++calib_pixels;
                }
            }
        }
    }
    for (std::size_t r = 0u; r < static_cast<std::size_t>(kSplashGlowBasisCount);
         ++r) {
        for (std::size_t c = 0u; c < r; ++c) {
            ata[r][c] = ata[c][r];
        }
    }

    const std::array<float, kSplashGlowBasisCount> weights =
        (calib_pixels >= 64u) ? SolveNonNegativeLeastSquares(ata, atb)
                              : std::array<float, kSplashGlowBasisCount>{};

    auto eval_glow_2d = [&](std::size_t idx, float dist_px,
                            const auto& basis) noexcept -> float {
        float sum = 0.0f;
        for (std::size_t m = 0u;
             m < static_cast<std::size_t>(kSplashGlowBasisCount); ++m) {
            sum += weights[m] * basis[m][idx];
        }
        if (calib_pixels >= 64u && sum > 0.05f && dist_px < 10.5f) {
            const float shoulder =
                kSplashGlowMaxAlpha * SmoothStep01((10.5f - dist_px) / 3.5f);
            sum = std::max(sum, shoulder);
        }
        return std::clamp(sum, 0.0f, kSplashGlowMaxAlpha);
    };

    BgraImage clean_bg = opaque_att02;
    std::vector<std::uint8_t> both_ink(ws_n, 0u);

    for (int ry = 0; ry < ws.h; ++ry) {
        const int y = ws.y + ry;
        for (int rx = 0; rx < ws.w; ++rx) {
            const int x = ws.x + rx;
            if (x >= kSplashLogoCleanDonorMinX && y >= kSplashLogoCleanDonorMinY) {
                clean_bg.SetPixel(x, y, opaque_att02.PixelAt(x, y));
                continue;
            }

            const std::size_t idx = static_cast<std::size_t>(ry * ws.w + rx);
            const BgraColor p1 = opaque_att01.PixelAt(x, y);
            const BgraColor p2 = opaque_att02.PixelAt(x, y);
            const float d1 = dist1[idx];
            const float d2 = dist2[idx];
            const bool i1 = (ink1[idx] != 0u);
            const bool i2 = (ink2[idx] != 0u);
            const bool lost1 =
                i1 || (d1 <= 11.5f && std::min({p1.r, p1.g, p1.b}) >= 252u);
            const bool lost2 =
                i2 || (d2 <= 11.5f && std::min({p2.r, p2.g, p2.b}) >= 252u);
            const float w1 = eval_glow_2d(idx, d1, basis1);
            const float w2 = eval_glow_2d(idx, d2, basis2);

            if (lost1 && !lost2) {
                clean_bg.SetPixel(x, y, InvertWhiteGlowPixel(p2, w2));
            } else if (!lost1 && lost2) {
                clean_bg.SetPixel(x, y, InvertWhiteGlowPixel(p1, w1));
            } else if (!lost1 && !lost2) {
                if (w1 == 0.0f && w2 == 0.0f) {
                    const int sum1 = static_cast<int>(p1.r) + p1.g + p1.b;
                    const int sum2 = static_cast<int>(p2.r) + p2.g + p2.b;
                    clean_bg.SetPixel(x, y, (sum1 <= sum2) ? p1 : p2);
                } else {
                    const BgraColor c1 = InvertWhiteGlowPixel(p1, w1);
                    const BgraColor c2 = InvertWhiteGlowPixel(p2, w2);
                    const float t = SmoothStep01(
                        (w1 - w2 + kSplashGlowBlendHalfWidth) /
                        (2.0f * kSplashGlowBlendHalfWidth));
                    clean_bg.SetPixel(
                        x, y,
                        BgraColor{
                            static_cast<std::uint8_t>(std::lround(
                                (1.0f - t) * c1.b + t * c2.b)),
                            static_cast<std::uint8_t>(std::lround(
                                (1.0f - t) * c1.g + t * c2.g)),
                            static_cast<std::uint8_t>(std::lround(
                                (1.0f - t) * c1.r + t * c2.r)),
                            kOpaqueAlpha,
                        });
                }
            } else {
                both_ink[idx] = 1u;
                const BgraColor c1 = InvertWhiteGlowPixel(p1, w1);
                const BgraColor c2 = InvertWhiteGlowPixel(p2, w2);
                const int sum1 = static_cast<int>(c1.r) + c1.g + c1.b;
                const int sum2 = static_cast<int>(c2.r) + c2.g + c2.b;
                clean_bg.SetPixel(x, y, (sum1 >= sum2) ? c1 : c2);
            }
        }
    }

    std::vector<std::uint8_t> both_lost = DilateMaskBox(both_ink, ws.w, ws.h, 2);
    for (int ry = 0; ry < ws.h; ++ry) {
        const int y = ws.y + ry;
        for (int rx = 0; rx < ws.w; ++rx) {
            if (ws.x + rx >= kSplashLogoCleanDonorMinX &&
                y >= kSplashLogoCleanDonorMinY) {
                both_lost[static_cast<std::size_t>(ry * ws.w + rx)] = 0u;
            }
        }
    }

    SpriteCanvas base_canvas(std::move(clean_bg));
    if (HasAnyMaskedPixel(both_lost)) {
        const BgraImage unmasked_ref = base_canvas.CopyImage();
        base_canvas.InpaintMaskedLaplaceSor(ws, both_lost);
        BgraImage harmonic_img = base_canvas.CopyImage();

        std::vector<std::array<float, 3>> detail_ws(ws_n, {0.0f, 0.0f, 0.0f});
        for (int ry = 0; ry < ws.h; ++ry) {
            for (int rx = 0; rx < ws.w; ++rx) {
                const std::size_t idx = static_cast<std::size_t>(ry * ws.w + rx);
                if (both_lost[idx] != 0u) {
                    continue;
                }
                float sb = 0.0f, sg = 0.0f, sr = 0.0f;
                int cnt = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    const int ny = ry + dy;
                    if (ny < 0 || ny >= ws.h) {
                        continue;
                    }
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int nx = rx + dx;
                        if (nx < 0 || nx >= ws.w) {
                            continue;
                        }
                        if (both_lost[static_cast<std::size_t>(ny * ws.w + nx)] != 0u) {
                            continue;
                        }
                        const BgraColor np =
                            unmasked_ref.PixelAt(ws.x + nx, ws.y + ny);
                        sb += np.b;
                        sg += np.g;
                        sr += np.r;
                        ++cnt;
                    }
                }
                if (cnt >= 9) {
                    const BgraColor cp = unmasked_ref.PixelAt(ws.x + rx, ws.y + ry);
                    const float inv = 1.0f / static_cast<float>(cnt);
                    detail_ws[idx] = {
                        std::clamp(static_cast<float>(cp.b) - sb * inv, -12.0f, 12.0f),
                        std::clamp(static_cast<float>(cp.g) - sg * inv, -12.0f, 12.0f),
                        std::clamp(static_cast<float>(cp.r) - sr * inv, -12.0f, 12.0f),
                    };
                }
            }
        }

        std::vector<std::uint8_t> outside_both(ws_n, 0u);
        for (std::size_t idx = 0u; idx < ws_n; ++idx) {
            outside_both[idx] = (both_lost[idx] == 0u) ? 1u : 0u;
        }
        const std::vector<float> dist_to_clean =
            ComputeExactEdtPx(outside_both, ws.w, ws.h);

        auto count_clean_run = [&](int rx, int ry_start, int dir) noexcept -> int {
            int avail = 0;
            for (int step = 0; step < 96; ++step) {
                const int ny = ry_start + dir * step;
                if (ny < 0 || ny >= ws.h ||
                    both_lost[static_cast<std::size_t>(ny * ws.w + rx)] != 0u) {
                    break;
                }
                ++avail;
            }
            return avail;
        };

        for (int rx = 0; rx < ws.w; ++rx) {
            int ry = 0;
            while (ry < ws.h) {
                if (both_lost[static_cast<std::size_t>(ry * ws.w + rx)] == 0u) {
                    ++ry;
                    continue;
                }
                const int ry0 = ry;
                while (ry < ws.h &&
                       both_lost[static_cast<std::size_t>(ry * ws.w + rx)] != 0u) {
                    ++ry;
                }
                const int ry1 = ry - 1;
                const int span_h = ry1 - ry0 + 1;
                const int avail_up = count_clean_run(rx, ry0 - 1, -1);
                const int avail_dn = count_clean_run(rx, ry1 + 1, +1);

                for (int my = ry0; my <= ry1; ++my) {
                    const float t = (static_cast<float>(my - ry0) + 0.5f) /
                                    static_cast<float>(span_h + 1);
                    const BgraColor hp =
                        harmonic_img.PixelAt(ws.x + rx, ws.y + my);
                    const int jx = std::clamp(
                        rx + ((my * 5 + rx * 3) % 7) - 3, 0, ws.w - 1);

                    std::array<float, 3> det = {0.0f, 0.0f, 0.0f};
                    if (avail_up >= 3 && avail_dn >= 3) {
                        const int off_u =
                            1 + TriangleWaveReflect(my - ry0, avail_up - 2);
                        const int off_d =
                            1 + TriangleWaveReflect(ry1 - my, avail_dn - 2);
                        const int uy = std::clamp(ry0 - 1 - off_u, 0, ws.h - 1);
                        const int dy = std::clamp(ry1 + 1 + off_d, 0, ws.h - 1);
                        const int ux =
                            (both_lost[static_cast<std::size_t>(uy * ws.w + jx)] == 0u)
                                ? jx
                                : rx;
                        const int dx =
                            (both_lost[static_cast<std::size_t>(dy * ws.w + jx)] == 0u)
                                ? jx
                                : rx;
                        const auto& du =
                            detail_ws[static_cast<std::size_t>(uy * ws.w + ux)];
                        const auto& dd =
                            detail_ws[static_cast<std::size_t>(dy * ws.w + dx)];
                        for (std::size_t c = 0u; c < 3u; ++c) {
                            det[c] = (1.0f - t) * du[c] + t * dd[c];
                        }
                    } else if (avail_up >= 3) {
                        const int off_u =
                            1 + TriangleWaveReflect(my - ry0, avail_up - 2);
                        const int uy = std::clamp(ry0 - 1 - off_u, 0, ws.h - 1);
                        det = detail_ws[static_cast<std::size_t>(uy * ws.w + rx)];
                    } else if (avail_dn >= 3) {
                        const int off_d =
                            1 + TriangleWaveReflect(ry1 - my, avail_dn - 2);
                        const int dy = std::clamp(ry1 + 1 + off_d, 0, ws.h - 1);
                        det = detail_ws[static_cast<std::size_t>(dy * ws.w + rx)];
                    }

                    float out_b = static_cast<float>(hp.b) + det[0];
                    float out_g = static_cast<float>(hp.g) + det[1];
                    float out_r = static_cast<float>(hp.r) + det[2];

                    // In the upper cherry-blossom canopy (`y < 330`), transfer
                    // crisp full-color blossoms & diagonal `/` branches from the
                    // clean canopy directly above (`src_ry = my - span_h - 2`,
                    // `src_rx = rx + (span_h * 2) / 3`) feathered smoothly into
                    // `harmonic_img + det` at the `5px` boundary so wide double-
                    // occluded bands (`y = 260..298, x = 660..1085`) never look
                    // like a blurred bar!
                    if (ws.y + ry1 < 330 && span_h >= 6) {
                        const int shift_y = span_h + 2;
                        const int src_ry = my - shift_y;
                        const int src_rx = std::clamp(
                            rx + (shift_y * 2) / 3, 0, ws.w - 1);
                        if (src_ry >= 0 &&
                            both_lost[static_cast<std::size_t>(
                                src_ry * ws.w + src_rx)] == 0u) {
                            const BgraColor cp = unmasked_ref.PixelAt(
                                ws.x + src_rx, ws.y + src_ry);
                            const float d_edge = dist_to_clean[
                                static_cast<std::size_t>(my * ws.w + rx)];
                            const float w_patch =
                                SmoothStep01((d_edge - 1.0f) / 5.0f);
                            out_b = (1.0f - w_patch) * out_b + w_patch * cp.b;
                            out_g = (1.0f - w_patch) * out_g + w_patch * cp.g;
                            out_r = (1.0f - w_patch) * out_r + w_patch * cp.r;
                        }
                    }

                    harmonic_img.SetPixel(
                        ws.x + rx, ws.y + my,
                        BgraColor{
                            static_cast<std::uint8_t>(
                                std::clamp(std::lround(out_b), 0L, 255L)),
                            static_cast<std::uint8_t>(
                                std::clamp(std::lround(out_g), 0L, 255L)),
                            static_cast<std::uint8_t>(
                                std::clamp(std::lround(out_r), 0L, 255L)),
                            kOpaqueAlpha,
                        });
                }
            }
        }
        base_canvas = SpriteCanvas(std::move(harmonic_img));
    }
    impl_->LocalizeSplashLogoSubtitle(base_canvas);

    SpriteCanvas canvas1 = base_canvas;
    SpriteCanvas canvas2 = std::move(base_canvas);
    impl_->DrawSplashParagraphs(canvas1, *spec1);
    impl_->DrawSplashParagraphs(canvas2, *spec2);

    return SplashPairRenderResult{canvas1.CopyImage(), canvas2.CopyImage()};
}

BgraImage MenuRenderer::RenderSingleSplashScreen(
    const BgraImage& orig, const SplashScreenSpec& spec) const {
    if (orig.empty()) {
        return orig;
    }
    SpriteCanvas canvas(EnsureOpaqueAlpha(orig));
    const RectI ws = ScaleRectFromRetail(kSplashWorkspaceRect, canvas.width(),
                                         canvas.height());
    if (!ws.empty()) {
        const int logo_x = (canvas.width() * kSplashLogoCleanDonorMinX) / kRetailScreenW;
        const int logo_y = (canvas.height() * kSplashLogoCleanDonorMinY) / kRetailScreenH;
        std::vector<std::uint8_t> mask(static_cast<std::size_t>(ws.w * ws.h), 0u);
        for (int ry = 0; ry < ws.h; ++ry) {
            const int y = ws.y + ry;
            for (int rx = 0; rx < ws.w; ++rx) {
                const int x = ws.x + rx;
                if (x >= logo_x && y >= logo_y) {
                    continue;
                }
                if (IsSplashNavyInk(canvas.PixelAt(x, y))) {
                    mask[static_cast<std::size_t>(ry * ws.w + rx)] = 1u;
                }
            }
        }
        const int radius = std::max(
            2, (kSplashSingleHaloDilatePx * canvas.height()) / kRetailScreenH);
        mask = DilateMaskBox(mask, ws.w, ws.h, radius);
        for (int ry = 0; ry < ws.h; ++ry) {
            const int y = ws.y + ry;
            for (int rx = 0; rx < ws.w; ++rx) {
                if (ws.x + rx >= logo_x && y >= logo_y) {
                    mask[static_cast<std::size_t>(ry * ws.w + rx)] = 0u;
                }
            }
        }
        if (HasAnyMaskedPixel(mask)) {
            canvas.InpaintMaskedLaplaceSor(ws, mask);
        }
    }
    impl_->LocalizeSplashLogoSubtitle(canvas);
    impl_->DrawSplashParagraphs(canvas, spec);
    return canvas.CopyImage();
}

BgraImage MenuRenderer::RenderTitleBackground(const BgraImage& orig) const {
    if (orig.empty()) {
        return orig;
    }
    SpriteCanvas canvas(orig);
    const RectI mask_rect =
        ScaleRectFromRetail(kTitleSubtitleMaskRect, canvas.width(), canvas.height());
    InpaintSubrectIfPred(canvas, mask_rect, kTitleSubtitleDilatePx, [](BgraColor p) {
        return p.r < kTitleSubtitleInkMaxRG && p.g < kTitleSubtitleInkMaxRG;
    });

    const float sy =
        static_cast<float>(canvas.height()) / static_cast<float>(kRetailScreenH);
    const RectI sub_rect =
        ScaleRectFromRetail(kTitleSubtitleDrawRect, canvas.width(), canvas.height());
    if (!sub_rect.empty()) {
        TextStyle sub;
        sub.font_family = impl_->serif_family;
        sub.weight = kDefaultFontWeightSemiBold;
        sub.size_px = std::max(8.0f, 14.5f * sy);
        sub.min_font_size_px = 7.0f;
        sub.fill = BgraColor{0x42, 0x1E, 0x0E, 0xFF};
        sub.stroke_px = std::max(0.8f, 1.5f * sy);
        sub.stroke_color = BgraColor{0xFF, 0xFF, 0xFF, 0xE2};
        sub.shadow = true;
        sub.shadow_dx = 0.0f;
        sub.shadow_dy = 0.0f;
        sub.shadow_blur_px = std::max(1.0f, 2.0f * sy);
        sub.shadow_color = BgraColor{0xFF, 0xFF, 0xFF, 0xBE};
        sub.halign = HorizontalAlign::kCenter;
        sub.valign = VerticalAlign::kCenter;
        sub.size_px = canvas.AutoFitFontSize(kLogoSubtitleEn, sub_rect, sub, 3);
        canvas.DrawText(kLogoSubtitleEn, sub_rect, sub);
    }

    const RectI cap_rect =
        ScaleRectFromRetail(kTitleCaptionDrawRect, canvas.width(), canvas.height());
    if (!cap_rect.empty()) {
        TextStyle cap;
        cap.font_family = impl_->serif_family;
        cap.weight = kDefaultFontWeightNormal;
        cap.italic = true;
        cap.size_px = std::max(8.0f, 15.0f * sy);
        cap.min_font_size_px = 7.0f;
        cap.tracking_px = 0.8f;
        cap.fill = BgraColor{0x52, 0x26, 0x12, 0xFF};
        cap.stroke_px = std::max(0.8f, 1.3f * sy);
        cap.stroke_color = BgraColor{0xFF, 0xFF, 0xFF, 0xDC};
        cap.shadow = true;
        cap.shadow_dx = 0.0f;
        cap.shadow_dy = 0.0f;
        cap.shadow_blur_px = std::max(1.0f, 2.2f * sy);
        cap.shadow_color = BgraColor{0xFF, 0xFF, 0xFF, 0xBE};
        cap.halign = HorizontalAlign::kCenter;
        cap.valign = VerticalAlign::kCenter;
        cap.size_px = canvas.AutoFitFontSize(kTitleRomajiCaptionEn, cap_rect, cap, 2);
        canvas.DrawText(kTitleRomajiCaptionEn, cap_rect, cap);
    }

    return canvas.CopyImage();
}

BgraImage MenuRenderer::RenderLockedCgThumbnail(const BgraImage& orig) const {
    if (orig.empty()) {
        return orig;
    }
    SpriteCanvas canvas(orig);
    const RectI mask_rect = ScaleRectFromRetail(
        kLockedCgMaskRect, canvas.width(), canvas.height(),
        kLockedCgRetailW, kLockedCgRetailH);
    InpaintSubrectIfPred(canvas, mask_rect, 1, [](BgraColor p) {
        return p.r < kLockedCgInkMaxR && p.g < kLockedCgInkMaxG;
    });

    const RectI draw_rect = ScaleRectFromRetail(
        kLockedCgDrawRect, canvas.width(), canvas.height(),
        kLockedCgRetailW, kLockedCgRetailH);
    if (!draw_rect.empty()) {
        TextStyle sub;
        sub.font_family = impl_->serif_family;
        sub.weight = kDefaultFontWeightSemiBold;
        sub.size_px = 10.5f;
        sub.min_font_size_px = 6.5f;
        sub.fill = BgraColor{0x96, 0x6E, 0x48, 0xFF};
        sub.stroke_px = 0.9f;
        sub.stroke_color = BgraColor{0xFA, 0xF5, 0xEB, 0xC8};
        sub.halign = HorizontalAlign::kCenter;
        sub.valign = VerticalAlign::kCenter;
        sub.size_px = canvas.AutoFitFontSize(kTitleRomajiCaptionEn, draw_rect, sub, 2);
        canvas.DrawText(kTitleRomajiCaptionEn, draw_rect, sub);
    }
    return canvas.CopyImage();
}

BgraImage MenuRenderer::RenderConfigBackground(const BgraImage& orig) const {
    if (orig.empty()) {
        return orig;
    }
    SpriteCanvas canvas(orig);
    const int w = canvas.width();
    const int h = canvas.height();
    const float sy = static_cast<float>(h) / static_cast<float>(kRetailScreenH);

    const auto& left_rows = ConfigLeftColumnCatalog();
    const auto& right_rows = ConfigRightVolumeCatalog();

    // 1. Erase all Japanese setting labels, slider endpoints, and vertical strip
    //    using warm-charcoal masking so blue-white feathers stay 100% intact.
    for (std::size_t r = 0u; r < left_rows.size(); ++r) {
        const int row = static_cast<int>(r);
        InpaintSubrectIfPred(
            canvas,
            ScaleRectFromRetail(
                RectI{kConfigLeftMaskX,
                      kConfigRowMaskBaseY + row * kConfigRowPitchY,
                      kConfigLeftMaskW, kConfigRowMaskH},
                w, h),
            kConfigInkDilatePx, IsConfigCharcoalInk);
        if (left_rows[r].has_slider) {
            InpaintSubrectIfPred(
                canvas,
                ScaleRectFromRetail(
                    RectI{kConfigLeftMinMaskX,
                          kConfigSliderMaskBaseY + row * kConfigRowPitchY,
                          kConfigLeftMinMaskW, kConfigSliderMaskH},
                    w, h),
                kConfigInkDilatePx, IsConfigCharcoalInk);
            InpaintSubrectIfPred(
                canvas,
                ScaleRectFromRetail(
                    RectI{kConfigLeftMaxMaskX,
                          kConfigSliderMaskBaseY + row * kConfigRowPitchY,
                          kConfigLeftMaxMaskW, kConfigSliderMaskH},
                    w, h),
                kConfigInkDilatePx, IsConfigCharcoalInk);
        }
    }

    for (std::size_t r = 0u; r < right_rows.size(); ++r) {
        const int row = static_cast<int>(r);
        InpaintSubrectIfPred(
            canvas,
            ScaleRectFromRetail(
                RectI{kConfigRightMaskX,
                      kConfigRowMaskBaseY + row * kConfigRowPitchY,
                      kConfigRightMaskW, kConfigRowMaskH},
                w, h),
            kConfigInkDilatePx, IsConfigCharcoalInk);
        if (right_rows[r].has_slider) {
            InpaintSubrectIfPred(
                canvas,
                ScaleRectFromRetail(
                    RectI{kConfigRightMinMaskX,
                          kConfigSliderMaskBaseY + row * kConfigRowPitchY,
                          kConfigRightMinMaskW, kConfigSliderMaskH},
                    w, h),
                kConfigInkDilatePx, IsConfigCharcoalInk);
            InpaintSubrectIfPred(
                canvas,
                ScaleRectFromRetail(
                    RectI{kConfigRightMaxMaskX,
                          kConfigSliderMaskBaseY + row * kConfigRowPitchY,
                          kConfigRightMaxMaskW, kConfigSliderMaskH},
                    w, h),
                kConfigInkDilatePx, IsConfigCharcoalInk);
        }
    }

    InpaintSubrectIfPred(
        canvas, ScaleRectFromRetail(kConfigVertVoicesMaskRect, w, h),
        kConfigInkDilatePx, IsConfigCharcoalInk);

    // 2. Render unified row labels and slider endpoints.
    TextStyle row_style;
    row_style.font_family = impl_->ui_sans_family;
    row_style.weight = kDefaultFontWeightSemiBold;
    row_style.size_px = std::max(9.0f, 19.0f * sy);
    row_style.min_font_size_px = 8.0f;
    row_style.fill = BgraColor{43, 46, 54, 255};
    row_style.halign = HorizontalAlign::kCenter;
    row_style.valign = VerticalAlign::kCenter;

    float unified_row_px = row_style.size_px;
    for (std::size_t r = 0u; r < left_rows.size(); ++r) {
        const RectI box = ScaleRectFromRetail(
            RectI{kConfigLeftDrawX,
                  kConfigRowDrawBaseY + static_cast<int>(r) * kConfigRowPitchY,
                  kConfigLeftDrawW, kConfigRowDrawH},
            w, h);
        if (!box.empty()) {
            unified_row_px = std::min(
                unified_row_px,
                canvas.AutoFitFontSize(left_rows[r].label_en, box, row_style, 4));
        }
    }
    for (std::size_t r = 0u; r < right_rows.size(); ++r) {
        const RectI box = ScaleRectFromRetail(
            RectI{kConfigRightDrawX,
                  kConfigRowDrawBaseY + static_cast<int>(r) * kConfigRowPitchY,
                  kConfigRightDrawW, kConfigRowDrawH},
            w, h);
        if (!box.empty()) {
            unified_row_px = std::min(
                unified_row_px,
                canvas.AutoFitFontSize(right_rows[r].label_en, box, row_style, 4));
        }
    }
    row_style.size_px = unified_row_px;

    TextStyle end_style;
    end_style.font_family = impl_->ui_sans_family;
    end_style.weight = kDefaultFontWeightNormal;
    end_style.size_px = std::max(7.5f, 13.5f * sy);
    end_style.min_font_size_px = 7.0f;
    end_style.fill = BgraColor{58, 60, 66, 255};
    end_style.halign = HorizontalAlign::kCenter;
    end_style.valign = VerticalAlign::kCenter;

    for (std::size_t r = 0u; r < left_rows.size(); ++r) {
        const int row = static_cast<int>(r);
        const RectI label_box = ScaleRectFromRetail(
            RectI{kConfigLeftDrawX, kConfigRowDrawBaseY + row * kConfigRowPitchY,
                  kConfigLeftDrawW, kConfigRowDrawH},
            w, h);
        if (!label_box.empty()) {
            canvas.DrawText(left_rows[r].label_en, label_box, row_style);
        }
        if (left_rows[r].has_slider) {
            const RectI min_box = ScaleRectFromRetail(
                RectI{kConfigLeftMinDrawX,
                      kConfigSliderDrawBaseY + row * kConfigRowPitchY,
                      kConfigLeftMinDrawW, kConfigSliderDrawH},
                w, h);
            const RectI max_box = ScaleRectFromRetail(
                RectI{kConfigLeftMaxDrawX,
                      kConfigSliderDrawBaseY + row * kConfigRowPitchY,
                      kConfigLeftMaxDrawW, kConfigSliderDrawH},
                w, h);
            if (!min_box.empty()) {
                canvas.DrawText(left_rows[r].slider_min_en, min_box, end_style);
            }
            if (!max_box.empty()) {
                canvas.DrawText(left_rows[r].slider_max_en, max_box, end_style);
            }
        }
    }

    for (std::size_t r = 0u; r < right_rows.size(); ++r) {
        const int row = static_cast<int>(r);
        const RectI label_box = ScaleRectFromRetail(
            RectI{kConfigRightDrawX, kConfigRowDrawBaseY + row * kConfigRowPitchY,
                  kConfigRightDrawW, kConfigRowDrawH},
            w, h);
        if (!label_box.empty()) {
            canvas.DrawText(right_rows[r].label_en, label_box, row_style);
        }
        if (right_rows[r].has_slider) {
            const RectI min_box = ScaleRectFromRetail(
                RectI{kConfigRightMinDrawX,
                      kConfigSliderDrawBaseY + row * kConfigRowPitchY,
                      kConfigRightMinDrawW, kConfigSliderDrawH},
                w, h);
            const RectI max_box = ScaleRectFromRetail(
                RectI{kConfigRightMaxDrawX,
                      kConfigSliderDrawBaseY + row * kConfigRowPitchY,
                      kConfigRightMaxDrawW, kConfigSliderDrawH},
                w, h);
            if (!min_box.empty()) {
                canvas.DrawText(right_rows[r].slider_min_en, min_box, end_style);
            }
            if (!max_box.empty()) {
                canvas.DrawText(right_rows[r].slider_max_en, max_box, end_style);
            }
        }
    }

    const RectI vert_box = ScaleRectFromRetail(kConfigVertVoicesDrawRect, w, h);
    if (!vert_box.empty()) {
        TextStyle vert_style;
        vert_style.font_family = impl_->ui_sans_family;
        vert_style.weight = kDefaultFontWeightSemiBold;
        vert_style.size_px = std::max(8.5f, 18.0f * sy);
        vert_style.min_font_size_px = 7.5f;
        vert_style.fill = BgraColor{36, 36, 40, 255};
        vert_style.halign = HorizontalAlign::kCenter;
        vert_style.valign = VerticalAlign::kCenter;
        const RectI horiz_measure_box{0, 0, vert_box.h, vert_box.w};
        vert_style.size_px = canvas.AutoFitFontSize(
            kConfigVerticalVoicesLabelEn, horiz_measure_box, vert_style, 4);
        canvas.DrawTextRotated90CCW(kConfigVerticalVoicesLabelEn, vert_box,
                                    vert_style);
    }

    return canvas.CopyImage();
}

std::map<std::string, BgraImage> MenuRenderer::RenderConfigToggleStrips(
    const std::map<std::string, BgraImage>& orig_strips) const {
    auto build_strip_ink_mask = [](const BgraImage& img) {
        const int w = img.width();
        const int h = img.height();
        std::vector<std::uint8_t> raw_mask(static_cast<std::size_t>(w * h), 0u);
        const int cell_w = std::max(1, w / kToggleCellCount);
        const int pad_x = std::min(kToggleInnerPadX, cell_w / 4);
        const int pad_y = std::min(kToggleInnerPadY, h / 5);
        for (int c = 0; c < kToggleCellCount; ++c) {
            const int x0 = c * cell_w + pad_x;
            const int x1 = std::min(w, (c + 1) * cell_w - pad_x);
            for (int y = pad_y; y < h - pad_y; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const BgraColor px = img.PixelAt(x, y);
                    if (px.a > 128u &&
                        std::min({px.r, px.g, px.b}) < kToggleDarkInkThreshold) {
                        raw_mask[static_cast<std::size_t>(y * w + x)] = 1u;
                    }
                }
            }
        }
        return DilateMaskBox(raw_mask, w, h, kToggleMaskDilatePx);
    };

    std::map<std::string, std::vector<std::uint8_t>> masks;
    for (const auto& [name, img] : orig_strips) {
        if (!img.empty()) {
            masks.emplace(name, build_strip_ink_mask(img));
        }
    }

    std::map<std::string, BgraImage> rendered;
    for (const auto& [name, orig] : orig_strips) {
        const ConfigToggleStripSpec* spec = FindConfigToggleStripSpec(name);
        if (spec == nullptr || orig.empty()) {
            rendered.emplace(name, orig);
            continue;
        }

        const int w = orig.width();
        const int h = orig.height();
        BgraImage clean = orig;
        std::vector<std::uint8_t> remaining = masks[name];

        // Cross-donor clean sakura-petal pixels from all sibling strips sharing
        // the exact same pill theme (`kLavenderInactiveCell0` or `kSkyBlueActiveCell0`).
        for (const ConfigToggleStripSpec& sib_spec : ConfigToggleStripCatalog()) {
            if (sib_spec.theme != spec->theme || sib_spec.entry_name == spec->entry_name) {
                continue;
            }
            const auto sib_it = orig_strips.find(std::string(sib_spec.entry_name));
            const auto mask_it = masks.find(std::string(sib_spec.entry_name));
            if (sib_it == orig_strips.end() || mask_it == masks.end()) {
                continue;
            }
            const BgraImage& donor = sib_it->second;
            if (donor.width() != w || donor.height() != h) {
                continue;
            }
            const auto& donor_mask = mask_it->second;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t idx = static_cast<std::size_t>(y * w + x);
                    if (remaining[idx] != 0u && donor_mask[idx] == 0u) {
                        BgraColor dp = donor.PixelAt(x, y);
                        dp.a = orig.PixelAt(x, y).a;
                        clean.SetPixel(x, y, dp);
                        remaining[idx] = 0u;
                    }
                }
            }
        }

        SpriteCanvas canvas(std::move(clean));
        const int cell_w = std::max(1, w / kToggleCellCount);
        const int pad_x = std::min(kToggleInnerPadX, cell_w / 4);
        const int pad_y = std::min(kToggleInnerPadY, h / 6);
        for (int c = 0; c < kToggleCellCount; ++c) {
            const RectI cell_box = ClampRectToImage(
                RectI{c * cell_w + pad_x, pad_y,
                      std::max(1, cell_w - 2 * pad_x),
                      std::max(1, h - 2 * pad_y)},
                w, h);
            if (cell_box.empty()) {
                continue;
            }
            std::vector<std::uint8_t> sub_mask(
                static_cast<std::size_t>(cell_box.w * cell_box.h), 0u);
            for (int ry = 0; ry < cell_box.h; ++ry) {
                for (int rx = 0; rx < cell_box.w; ++rx) {
                    sub_mask[static_cast<std::size_t>(ry * cell_box.w + rx)] =
                        remaining[static_cast<std::size_t>(
                            (cell_box.y + ry) * w + (cell_box.x + rx))];
                }
            }
            if (HasAnyMaskedPixel(sub_mask)) {
                canvas.InpaintMaskedLaplaceSor(cell_box, sub_mask);
            }
        }

        TextStyle style;
        style.font_family = impl_->ui_sans_family;
        style.weight = kDefaultFontWeightSemiBold;
        style.size_px = std::max(8.0f, 16.5f * static_cast<float>(h) / 37.0f);
        style.min_font_size_px = 7.5f;
        style.fill = BgraColor{28, 25, 25, 255};
        style.stroke_px = 1.8f;
        style.stroke_color = BgraColor{255, 255, 255, 235};
        style.halign = HorizontalAlign::kCenter;
        style.valign = VerticalAlign::kCenter;

        const RectI ref_cell{10, 3, std::max(8, cell_w - 20), std::max(8, h - 6)};
        style.size_px = canvas.AutoFitFontSize(spec->label_en, ref_cell, style, 2);

        for (int c = 0; c < kToggleCellCount; ++c) {
            TextStyle cell_style = style;
            if (c == kToggleCellCount - 1) {
                cell_style.fill = BgraColor{62, 62, 66, 255};
            }
            const RectI draw_box = ClampRectToImage(
                RectI{c * cell_w + 10, 2, std::max(8, cell_w - 20),
                      std::max(8, h - 4)},
                w, h);
            if (!draw_box.empty()) {
                canvas.DrawText(spec->label_en, draw_box, cell_style);
            }
        }

        BgraImage final_img = canvas.CopyImage();
        // Guarantee outer rounded-pill alpha border is 100% preserved.
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint8_t orig_a = orig.PixelAt(x, y).a;
                if (orig_a == 0u) {
                    final_img.SetPixel(x, y, orig.PixelAt(x, y));
                }
            }
        }
        rendered.emplace(name, std::move(final_img));
    }
    return rendered;
}

BgraImage MenuRenderer::RenderConfigVoicePortrait(
    const BgraImage& orig, const ConfigVoicePortraitSpec& spec) const {
    if (orig.empty()) {
        return orig;
    }
    SpriteCanvas canvas(orig);
    const int w = canvas.width();
    const int h = canvas.height();
    const int cell_w = std::max(1, w / kPortraitCellCount);

    const int erase_h = PortraitVerticalEraseHeight(spec.entry_name);
    const RectI erase_c0 = ScaleRectFromRetail(
        RectI{3, 3, 21, erase_h}, cell_w, h, kPortraitRetailCellW,
        kPortraitRetailH);

    // Cell 0 & Cell 1 Japanese vertical name restoration:
    if (spec.flat_border_wipe) {
        if (spec.entry_name == "SGCnfgWnd110000") {
            // Cyan vertical kanji `その他男性` vs sky-blue silhouette (x >= 24)
            // and subtle top-left feather.
            InpaintSubrectIfPred(canvas, erase_c0, 2, [](BgraColor p) {
                return p.r < 212u &&
                       (static_cast<int>(p.b) - static_cast<int>(p.r)) >= 28;
            });
        } else if (spec.entry_name == "SGCnfgWnd110100") {
            // Vivid rose kanji `その他女性` vs pastel-pink silhouette (x >= 24).
            InpaintSubrectIfPred(canvas, erase_c0, 2, [](BgraColor p) {
                return p.g < 212u &&
                       (static_cast<int>(p.r) - static_cast<int>(p.g)) >= 34;
            });
        }

        RectI erase_c1 = ScaleRectFromRetail(
            RectI{2, 2, 24, erase_h + 4}, cell_w, h, kPortraitRetailCellW,
            kPortraitRetailH);
        erase_c1.x += cell_w;
        erase_c1 = ClampRectToImage(erase_c1, w, h);
        if (!erase_c1.empty()) {
            const int rw = erase_c1.w;
            const int rh = erase_c1.h;
            const std::size_t rn = static_cast<std::size_t>(rw * rh);
            std::vector<std::uint8_t> white_ink(rn, 0u);
            for (int ry = 0; ry < rh; ++ry) {
                for (int rx = 0; rx < rw; ++rx) {
                    const BgraColor p =
                        canvas.PixelAt(erase_c1.x + rx, erase_c1.y + ry);
                    if (p.b >= 188u && p.g >= 196u && p.r >= 202u &&
                        (static_cast<int>(p.r) - static_cast<int>(p.b)) <= 25) {
                        white_ink[static_cast<std::size_t>(ry * rw + rx)] = 1u;
                    }
                }
            }

            auto any_white_in_window = [&](int rx, int ry, int dx0, int dx1,
                                           int dy0, int dy1) noexcept -> bool {
                for (int dy = dy0; dy <= dy1; ++dy) {
                    const int ny = ry + dy;
                    if (ny < 0 || ny >= rh) {
                        continue;
                    }
                    for (int dx = dx0; dx <= dx1; ++dx) {
                        const int nx = rx + dx;
                        if (nx >= 0 && nx < rw &&
                            white_ink[static_cast<std::size_t>(ny * rw + nx)] !=
                                0u) {
                            return true;
                        }
                    }
                }
                return false;
            };

            struct SavedPixel {
                int rx;
                int ry;
                BgraColor color;
            };
            std::vector<SavedPixel> saved_silhouette;
            const int sil_cx = std::clamp(cell_w + cell_w / 2, cell_w, w - 1);
            const int sil_cy = std::clamp(h / 2, 0, h - 1);
            const BgraColor sil_ref = canvas.PixelAt(sil_cx, sil_cy);
            std::vector<std::uint8_t> sil_vis(
                static_cast<std::size_t>(cell_w * h), 0u);
            std::vector<std::pair<int, int>> sil_q;
            sil_q.emplace_back(sil_cx - cell_w, sil_cy);
            sil_vis[static_cast<std::size_t>(
                sil_cy * cell_w + (sil_cx - cell_w))] = 1u;
            std::size_t sq_head = 0u;
            while (sq_head < sil_q.size()) {
                const auto [sx, sy] = sil_q[sq_head++];
                constexpr std::array<std::pair<int, int>, 4> kDirs4 = {{
                    {-1, 0}, {1, 0}, {0, -1}, {0, 1},
                }};
                for (const auto [dx, dy] : kDirs4) {
                    const int nx = sx + dx;
                    const int ny = sy + dy;
                    if (nx < 0 || nx >= cell_w || ny < 0 || ny >= h ||
                        cell_w + nx >= w) {
                        continue;
                    }
                    const std::size_t vidx =
                        static_cast<std::size_t>(ny * cell_w + nx);
                    if (sil_vis[vidx] != 0u) {
                        continue;
                    }
                    const BgraColor np = canvas.PixelAt(cell_w + nx, ny);
                    if (std::abs(static_cast<int>(np.r) - sil_ref.r) <= 22 &&
                        std::abs(static_cast<int>(np.g) - sil_ref.g) <= 22 &&
                        std::abs(static_cast<int>(np.b) - sil_ref.b) <= 22) {
                        sil_vis[vidx] = 1u;
                        sil_q.emplace_back(nx, ny);
                    }
                }
            }
            const std::vector<std::uint8_t> sil_full =
                DilateMaskBox(sil_vis, cell_w, h, 1);

            for (int ry = 0; ry < rh; ++ry) {
                const int gy = erase_c1.y + ry;
                const BgraColor row_card =
                    canvas.PixelAt(std::clamp(w - 6, cell_w, w - 1), gy);
                for (int rx = 0; rx < rw; ++rx) {
                    const int cx_rel = (erase_c1.x + rx) - cell_w;
                    const std::size_t idx =
                        static_cast<std::size_t>(ry * rw + rx);
                    if (cx_rel >= 0 && cx_rel < cell_w &&
                        sil_full[static_cast<std::size_t>(
                            gy * cell_w + cx_rel)] != 0u &&
                        white_ink[idx] == 0u) {
                        saved_silhouette.push_back(
                            {rx, ry, canvas.PixelAt(erase_c1.x + rx, gy)});
                        canvas.WipeRectSolid(
                            RectI{erase_c1.x + rx, gy, 1, 1}, row_card);
                    }
                }
            }

            std::vector<std::uint8_t> mask_c1(rn, 0u);
            for (int ry = 0; ry < rh; ++ry) {
                for (int rx = 0; rx < rw; ++rx) {
                    if (any_white_in_window(rx, ry, -8, 3, -8, 3)) {
                        mask_c1[static_cast<std::size_t>(ry * rw + rx)] = 1u;
                    }
                }
            }
            mask_c1 = DilateMaskBox(mask_c1, rw, rh, 1);
            for (const SavedPixel& sp : saved_silhouette) {
                mask_c1[static_cast<std::size_t>(sp.ry * rw + sp.rx)] = 0u;
            }
            if (HasAnyMaskedPixel(mask_c1)) {
                canvas.InpaintMaskedLaplaceSor(erase_c1, mask_c1);
            }
            for (const SavedPixel& sp : saved_silhouette) {
                canvas.WipeRectSolid(
                    RectI{erase_c1.x + sp.rx, erase_c1.y + sp.ry, 1, 1},
                    sp.color);
            }
        }
    } else {
        // Heroines (`100000..100500`):
        // In retail sprites, Cell 1 (`x + cell_w, y`) is pixel-aligned with
        // Cell 0 (`x, y`) via an exact Rec.601 -> sepia LUT (`RMSE < 1.0 LSB`),
        // and Cell 1's kanji has ONLY thin white strokes (ZERO white halo and
        // ZERO drop shadow!), so Cell 1 preserves the exact razor-sharp hair
        // strand luminance underneath Cell 0's `3-4px` white kanji halo!
        const RectI fit_rect = ScaleRectFromRetail(
            RectI{28, 5, 68, 92}, cell_w, h, kPortraitRetailCellW,
            kPortraitRetailH);
        auto eval_proj_f = [](BgraColor p, int mode) noexcept -> float {
            const float r = static_cast<float>(p.r);
            const float g = static_cast<float>(p.g);
            const float b = static_cast<float>(p.b);
            switch (mode) {
                case 0:
                    return std::clamp(0.299f * r + 0.587f * g + 0.114f * b,
                                      0.0f, 255.0f);
                case 1:
                    return std::clamp(0.2126f * r + 0.7152f * g + 0.0722f * b,
                                      0.0f, 255.0f);
                case 2:
                    return std::clamp((r + g + b) / 3.0f, 0.0f, 255.0f);
                default:
                    return std::clamp(
                        0.5f * (std::max({r, g, b}) + std::min({r, g, b})),
                        0.0f, 255.0f);
            }
        };
        auto eval_proj = [&](BgraColor p, int mode) noexcept -> int {
            return std::clamp(
                static_cast<int>(std::lround(eval_proj_f(p, mode))), 0, 255);
        };

        int best_mode = 0;
        double best_sse = 1.0e30;
        std::array<BgraColor, 256> best_lut{};

        for (int mode = 0; mode < 4; ++mode) {
            std::array<double, 256> sr{};
            std::array<double, 256> sg{};
            std::array<double, 256> sb{};
            std::array<int, 256> cnt{};
            for (int gy = fit_rect.y; gy < fit_rect.y + fit_rect.h; ++gy) {
                for (int gx = fit_rect.x;
                     gx < fit_rect.x + fit_rect.w && cell_w + gx < w; ++gx) {
                    const BgraColor p0 = orig.PixelAt(gx, gy);
                    const BgraColor p1 = orig.PixelAt(cell_w + gx, gy);
                    const int lum = eval_proj(p0, mode);
                    sr[static_cast<std::size_t>(lum)] += p1.r;
                    sg[static_cast<std::size_t>(lum)] += p1.g;
                    sb[static_cast<std::size_t>(lum)] += p1.b;
                    ++cnt[static_cast<std::size_t>(lum)];
                }
            }
            std::array<BgraColor, 256> lut{};
            for (int l = 0; l < 256; ++l) {
                int lo = l;
                while (lo >= 0 && cnt[static_cast<std::size_t>(lo)] == 0) {
                    --lo;
                }
                int hi = l;
                while (hi < 256 && cnt[static_cast<std::size_t>(hi)] == 0) {
                    ++hi;
                }
                if (lo >= 0 && hi < 256) {
                    const double t =
                        (lo == hi) ? 0.0
                                   : static_cast<double>(l - lo) /
                                         static_cast<double>(hi - lo);
                    const double inv_lo =
                        1.0 / cnt[static_cast<std::size_t>(lo)];
                    const double inv_hi =
                        1.0 / cnt[static_cast<std::size_t>(hi)];
                    const double rb =
                        (1.0 - t) * (sb[static_cast<std::size_t>(lo)] * inv_lo) +
                        t * (sb[static_cast<std::size_t>(hi)] * inv_hi);
                    const double rg =
                        (1.0 - t) * (sg[static_cast<std::size_t>(lo)] * inv_lo) +
                        t * (sg[static_cast<std::size_t>(hi)] * inv_hi);
                    const double rr =
                        (1.0 - t) * (sr[static_cast<std::size_t>(lo)] * inv_lo) +
                        t * (sr[static_cast<std::size_t>(hi)] * inv_hi);
                    lut[static_cast<std::size_t>(l)] = BgraColor{
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(rb), 0L, 255L)),
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(rg), 0L, 255L)),
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(rr), 0L, 255L)),
                        kOpaqueAlpha,
                    };
                } else if (lo >= 0) {
                    const double inv = 1.0 / cnt[static_cast<std::size_t>(lo)];
                    lut[static_cast<std::size_t>(l)] = BgraColor{
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(sb[static_cast<std::size_t>(lo)] * inv),
                            0L, 255L)),
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(sg[static_cast<std::size_t>(lo)] * inv),
                            0L, 255L)),
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(sr[static_cast<std::size_t>(lo)] * inv),
                            0L, 255L)),
                        kOpaqueAlpha,
                    };
                } else if (hi < 256) {
                    const double inv = 1.0 / cnt[static_cast<std::size_t>(hi)];
                    lut[static_cast<std::size_t>(l)] = BgraColor{
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(sb[static_cast<std::size_t>(hi)] * inv),
                            0L, 255L)),
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(sg[static_cast<std::size_t>(hi)] * inv),
                            0L, 255L)),
                        static_cast<std::uint8_t>(std::clamp(
                            std::lround(sr[static_cast<std::size_t>(hi)] * inv),
                            0L, 255L)),
                        kOpaqueAlpha,
                    };
                }
            }
            double sse = 0.0;
            for (int gy = fit_rect.y; gy < fit_rect.y + fit_rect.h; ++gy) {
                for (int gx = fit_rect.x;
                     gx < fit_rect.x + fit_rect.w && cell_w + gx < w; ++gx) {
                    const BgraColor p0 = orig.PixelAt(gx, gy);
                    const BgraColor p1 = orig.PixelAt(cell_w + gx, gy);
                    const BgraColor est =
                        lut[static_cast<std::size_t>(eval_proj(p0, mode))];
                    const double db = static_cast<double>(est.b) - p1.b;
                    const double dg = static_cast<double>(est.g) - p1.g;
                    const double dr = static_cast<double>(est.r) - p1.r;
                    sse += db * db + dg * dg + dr * dr;
                }
            }
            if (sse < best_sse) {
                best_sse = sse;
                best_mode = mode;
                best_lut = lut;
            }
        }

        // Fit inverse `Cell 1 green -> Cell 0 float luma` LUT from `best_lut`.
        std::array<double, 256> inv_sum_y{};
        std::array<int, 256> inv_cnt{};
        for (int l = 0; l < 256; ++l) {
            const int g_val = static_cast<int>(best_lut[static_cast<std::size_t>(l)].g);
            inv_sum_y[static_cast<std::size_t>(g_val)] += static_cast<double>(l);
            ++inv_cnt[static_cast<std::size_t>(g_val)];
        }
        std::array<float, 256> inv_luma_by_g{};
        for (int g_val = 0; g_val < 256; ++g_val) {
            int lo = g_val;
            while (lo >= 0 && inv_cnt[static_cast<std::size_t>(lo)] == 0) {
                --lo;
            }
            int hi = g_val;
            while (hi < 256 && inv_cnt[static_cast<std::size_t>(hi)] == 0) {
                ++hi;
            }
            if (lo >= 0 && hi < 256) {
                const double y_lo = inv_sum_y[static_cast<std::size_t>(lo)] /
                                    inv_cnt[static_cast<std::size_t>(lo)];
                const double y_hi = inv_sum_y[static_cast<std::size_t>(hi)] /
                                    inv_cnt[static_cast<std::size_t>(hi)];
                const double t =
                    (lo == hi) ? 0.0
                               : static_cast<double>(g_val - lo) /
                                     static_cast<double>(hi - lo);
                inv_luma_by_g[static_cast<std::size_t>(g_val)] =
                    static_cast<float>((1.0 - t) * y_lo + t * y_hi);
            } else if (lo >= 0) {
                inv_luma_by_g[static_cast<std::size_t>(g_val)] =
                    static_cast<float>(inv_sum_y[static_cast<std::size_t>(lo)] /
                                       inv_cnt[static_cast<std::size_t>(lo)]);
            } else if (hi < 256) {
                inv_luma_by_g[static_cast<std::size_t>(g_val)] =
                    static_cast<float>(inv_sum_y[static_cast<std::size_t>(hi)] /
                                       inv_cnt[static_cast<std::size_t>(hi)]);
            }
        }

        const RectI work = ScaleRectFromRetail(
            RectI{1, 1, 31, erase_h + 5}, cell_w, h, kPortraitRetailCellW,
            kPortraitRetailH);
        if (!work.empty()) {
            const int ww = work.w;
            const int wh = work.h;
            const std::size_t wn = static_cast<std::size_t>(ww * wh);
            const int max_sepia_g = static_cast<int>(best_lut[255].g);
            const int max_sepia_b = static_cast<int>(best_lut[255].b);
            const int min_seed_gx0 = std::max(2, (2 * cell_w) / kPortraitRetailCellW);
            const int min_seed_gy = std::max(2, (2 * h) / kPortraitRetailH);
            const int max_seed_gx0 = std::max(4, (24 * cell_w) / kPortraitRetailCellW);
            const int max_seed_gy =
                std::min(h - 1, ((erase_h + 3) * h) / kPortraitRetailH);

            // 1. Pinpoint exact kanji strokes (`kanji_core`) using Cell 1's
            //    bright white kanji ink (`c1_seed`, `p1.g >= 182`, rejecting
            //    Cell 1's dark neutral-grey frame bevel while keeping leftmost
            //    kanji strokes at `x = 2..3`!) plus connected dark strokes.
            std::vector<std::uint8_t> c1_seed(wn, 0u);
            for (int ry = 0; ry < wh; ++ry) {
                const int gy = work.y + ry;
                if (gy < min_seed_gy || gy > max_seed_gy) {
                    continue;
                }
                for (int rx = 0; rx < ww; ++rx) {
                    const int gx0 = work.x + rx;
                    const int gx1 = cell_w + gx0;
                    if (gx0 < min_seed_gx0 || gx0 > max_seed_gx0 || gx1 >= w) {
                        continue;
                    }
                    const BgraColor p1 = orig.PixelAt(gx1, gy);
                    if (p1.g < 182u) {
                        continue;
                    }
                    const int est_y = std::clamp(
                        static_cast<int>(std::lround(inv_luma_by_g[p1.g])),
                        0, 255);
                    const BgraColor est_s =
                        best_lut[static_cast<std::size_t>(est_y)];
                    const int est_rb =
                        static_cast<int>(est_s.r) - static_cast<int>(est_s.b);
                    const int act_rb =
                        static_cast<int>(p1.r) - static_cast<int>(p1.b);
                    if (static_cast<int>(p1.g) > max_sepia_g + 3 ||
                        static_cast<int>(p1.b) > max_sepia_b + 4 ||
                        (est_rb - act_rb) >= 5) {
                        c1_seed[static_cast<std::size_t>(ry * ww + rx)] = 1u;
                    }
                }
            }

            const std::vector<std::uint8_t> near_c1_2 =
                DilateMaskBox(c1_seed, ww, wh, 2);
            std::vector<std::uint8_t> kanji_core = c1_seed;
            for (int ry = 0; ry < wh; ++ry) {
                const int gy = work.y + ry;
                if (gy > max_seed_gy) {
                    continue;
                }
                for (int rx = 0; rx < ww; ++rx) {
                    const std::size_t idx =
                        static_cast<std::size_t>(ry * ww + rx);
                    if (near_c1_2[idx] == 0u || work.x + rx > max_seed_gx0) {
                        continue;
                    }
                    const BgraColor p0 = orig.PixelAt(work.x + rx, gy);
                    const int mx = std::max({static_cast<int>(p0.r),
                                             static_cast<int>(p0.g),
                                             static_cast<int>(p0.b)});
                    const int mn = std::min({static_cast<int>(p0.r),
                                             static_cast<int>(p0.g),
                                             static_cast<int>(p0.b)});
                    if (mx < 165 && (mx - mn) <= 16) {
                        kanji_core[idx] = 1u;
                    }
                }
            }

            const std::vector<std::uint8_t> core_dil2 =
                DilateMaskBox(kanji_core, ww, wh, 2);
            const std::vector<std::uint8_t> core_dil3 =
                DilateMaskBox(kanji_core, ww, wh, 3);
            const std::vector<std::uint8_t> core_dil4 =
                DilateMaskBox(kanji_core, ww, wh, 4);

            std::vector<std::uint8_t> non_white_hair(wn, 0u);
            for (int ry = 2; ry < wh; ++ry) {
                const int gy = work.y + ry;
                for (int rx = 2; rx < ww; ++rx) {
                    const std::size_t idx =
                        static_cast<std::size_t>(ry * ww + rx);
                    if (core_dil4[idx] != 0u) {
                        continue;
                    }
                    const BgraColor p0 = orig.PixelAt(work.x + rx, gy);
                    const int mx = std::max({static_cast<int>(p0.r),
                                             static_cast<int>(p0.g),
                                             static_cast<int>(p0.b)});
                    const int mn = std::min({static_cast<int>(p0.r),
                                             static_cast<int>(p0.g),
                                             static_cast<int>(p0.b)});
                    if (mn < 240 || (mx - mn) >= 14) {
                        non_white_hair[idx] = 1u;
                    }
                }
            }
            const std::vector<std::uint8_t> near_hair_1 =
                DilateMaskBox(non_white_hair, ww, wh, 1);

            std::vector<std::uint8_t> mask_c0(wn, 0u);
            for (int ry = 1; ry < wh; ++ry) {
                const int gy = work.y + ry;
                for (int rx = 1; rx < ww; ++rx) {
                    const std::size_t idx =
                        static_cast<std::size_t>(ry * ww + rx);
                    const BgraColor p0 = orig.PixelAt(work.x + rx, gy);
                    const int mx = std::max({static_cast<int>(p0.r),
                                             static_cast<int>(p0.g),
                                             static_cast<int>(p0.b)});
                    const int mn = std::min({static_cast<int>(p0.r),
                                             static_cast<int>(p0.g),
                                             static_cast<int>(p0.b)});
                    if (core_dil2[idx] != 0u) {
                        mask_c0[idx] = 1u;
                        continue;
                    }
                    if (core_dil3[idx] != 0u && mn >= 150 && mn < 246 &&
                        (mx - mn) <= 13) {
                        mask_c0[idx] = 1u;
                        continue;
                    }
                    if (core_dil3[idx] != 0u && near_hair_1[idx] != 0u &&
                        mn >= 224 && (mx - mn) <= 20) {
                        mask_c0[idx] = 1u;
                    }
                }
            }
            if (HasAnyMaskedPixel(mask_c0)) {
                std::vector<std::array<float, 3>> bgr(wn, {255.0f, 255.0f, 255.0f});
                std::vector<std::uint8_t> unk = mask_c0;
                std::vector<int> rem;
                for (int ry = 0; ry < wh; ++ry) {
                    const int gy = work.y + ry;
                    for (int rx = 0; rx < ww; ++rx) {
                        const std::size_t idx =
                            static_cast<std::size_t>(ry * ww + rx);
                        if (unk[idx] != 0u) {
                            rem.push_back(static_cast<int>(idx));
                        } else {
                            const BgraColor p0 = orig.PixelAt(work.x + rx, gy);
                            bgr[idx] = {static_cast<float>(p0.b),
                                        static_cast<float>(p0.g),
                                        static_cast<float>(p0.r)};
                        }
                    }
                }
                while (!rem.empty()) {
                    std::vector<std::pair<int, std::array<float, 3>>> upd;
                    std::vector<int> next_rem;
                    for (const int idx : rem) {
                        const int rx = idx % ww;
                        const int ry = idx / ww;
                        float sb = 0.0f, sg = 0.0f, sr = 0.0f, sw = 0.0f;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (dx == 0 && dy == 0) {
                                    continue;
                                }
                                const int nx = rx + dx;
                                const int ny = ry + dy;
                                if (nx < 0 || nx >= ww || ny < 0 || ny >= wh) {
                                    continue;
                                }
                                const std::size_t nidx =
                                    static_cast<std::size_t>(ny * ww + nx);
                                if (unk[nidx] != 0u) {
                                    continue;
                                }
                                const float w_nb =
                                    (dx == 0 || dy == 0) ? 1.0f : 0.7071f;
                                sb += bgr[nidx][0] * w_nb;
                                sg += bgr[nidx][1] * w_nb;
                                sr += bgr[nidx][2] * w_nb;
                                sw += w_nb;
                            }
                        }
                        if (sw > 0.0f) {
                            upd.push_back(
                                {idx, {sb / sw, sg / sw, sr / sw}});
                        } else {
                            next_rem.push_back(idx);
                        }
                    }
                    if (upd.empty()) {
                        break;
                    }
                    for (const auto& [idx, val] : upd) {
                        bgr[static_cast<std::size_t>(idx)] = val;
                        unk[static_cast<std::size_t>(idx)] = 0u;
                    }
                    rem.swap(next_rem);
                }
                constexpr std::array<std::pair<int, int>, 4> kDirs4 = {{
                    {-1, 0}, {1, 0}, {0, -1}, {0, 1},
                }};
                for (int iter = 0; iter < 260; ++iter) {
                    for (int ry = 0; ry < wh; ++ry) {
                        for (int rx = 0; rx < ww; ++rx) {
                            const std::size_t idx =
                                static_cast<std::size_t>(ry * ww + rx);
                            if (mask_c0[idx] == 0u) {
                                continue;
                            }
                            float sb = 0.0f, sg = 0.0f, sr = 0.0f;
                            int cnt = 0;
                            for (const auto [dx, dy] : kDirs4) {
                                const int nx = rx + dx;
                                const int ny = ry + dy;
                                if (nx < 0 || nx >= ww || ny < 0 || ny >= wh) {
                                    continue;
                                }
                                const auto& nb = bgr[static_cast<std::size_t>(
                                    ny * ww + nx)];
                                sb += nb[0];
                                sg += nb[1];
                                sr += nb[2];
                                ++cnt;
                            }
                            if (cnt > 0) {
                                const float inv = 1.0f / static_cast<float>(cnt);
                                bgr[idx][0] += 1.70f * (sb * inv - bgr[idx][0]);
                                bgr[idx][1] += 1.70f * (sg * inv - bgr[idx][1]);
                                bgr[idx][2] += 1.70f * (sr * inv - bgr[idx][2]);
                            }
                        }
                    }
                }
                for (int ry = 0; ry < wh; ++ry) {
                    const int gy = work.y + ry;
                    for (int rx = 0; rx < ww; ++rx) {
                        const std::size_t idx =
                            static_cast<std::size_t>(ry * ww + rx);
                        if (mask_c0[idx] == 0u) {
                            continue;
                        }
                        const int gx0 = work.x + rx;
                        canvas.WipeRectSolid(
                            RectI{gx0, gy, 1, 1},
                            BgraColor{
                                static_cast<std::uint8_t>(std::clamp(
                                    std::lround(bgr[idx][0]), 0L, 255L)),
                                static_cast<std::uint8_t>(std::clamp(
                                    std::lround(bgr[idx][1]), 0L, 255L)),
                                static_cast<std::uint8_t>(std::clamp(
                                    std::lround(bgr[idx][2]), 0L, 255L)),
                                orig.PixelAt(gx0, gy).a,
                            });
                    }
                }
            }

            // 4. Restore Cell 1 across `c1_glow_zone` (`r <= 6px` + 2px smooth
            //    feather) directly from `best_lut(restored Cell 0)` so BOTH
            //    Cell 1's white kanji AND its Multiply dark outer glow are
            //    eliminated while keeping razor-sharp sepia hair strands!
            const std::vector<std::uint8_t> c1_kanji =
                DilateMaskBox(c1_seed, ww, wh, 1);
            const std::vector<std::uint8_t> c1_glow_zone =
                DilateMaskBox(c1_kanji, ww, wh, 5);
            const std::vector<std::uint8_t> c1_f1 =
                DilateMaskBox(c1_glow_zone, ww, wh, 1);
            const std::vector<std::uint8_t> c1_f2 =
                DilateMaskBox(c1_glow_zone, ww, wh, 2);
            for (int ry = 0; ry < wh; ++ry) {
                const int gy = work.y + ry;
                for (int rx = 0; rx < ww; ++rx) {
                    const std::size_t idx =
                        static_cast<std::size_t>(ry * ww + rx);
                    if (c1_f2[idx] == 0u) {
                        continue;
                    }
                    const int gx0 = work.x + rx;
                    const int gx1 = cell_w + gx0;
                    if (gx0 >= cell_w || gx1 >= w) {
                        continue;
                    }
                    const BgraColor c0_clean = canvas.PixelAt(gx0, gy);
                    const BgraColor mapped = best_lut[static_cast<std::size_t>(
                        eval_proj(c0_clean, best_mode))];
                    const float alpha =
                        (c1_glow_zone[idx] != 0u)
                            ? 1.0f
                            : ((c1_f1[idx] != 0u) ? 0.67f : 0.33f);
                    if (alpha >= 0.999f) {
                        canvas.WipeRectSolid(RectI{gx1, gy, 1, 1}, mapped);
                    } else {
                        const BgraColor orig_c1 = orig.PixelAt(gx1, gy);
                        const BgraColor blended{
                            static_cast<std::uint8_t>(std::lround(
                                (1.0f - alpha) * orig_c1.b +
                                alpha * mapped.b)),
                            static_cast<std::uint8_t>(std::lround(
                                (1.0f - alpha) * orig_c1.g +
                                alpha * mapped.g)),
                            static_cast<std::uint8_t>(std::lround(
                                (1.0f - alpha) * orig_c1.r +
                                alpha * mapped.r)),
                            orig_c1.a,
                        };
                        canvas.WipeRectSolid(RectI{gx1, gy, 1, 1}, blended);
                    }
                }
            }
        }
    }

    // Horizontal English nameplate centered at bottom of both cells:
    for (int c = 0; c < kPortraitCellCount; ++c) {
        RectI plate_rect = ScaleRectFromRetail(
            RectI{4, 104, 92, 23}, cell_w, h, kPortraitRetailCellW,
            kPortraitRetailH);
        plate_rect.x += c * cell_w;
        plate_rect = ClampRectToImage(plate_rect, w, h);
        if (plate_rect.empty()) {
            continue;
        }
        TextStyle name_style;
        name_style.font_family = impl_->ui_sans_family;
        name_style.weight = kDefaultFontWeightBold;
        name_style.size_px =
            std::max(8.0f, 15.0f * static_cast<float>(h) / 130.0f);
        name_style.min_font_size_px = 7.5f;
        name_style.fill = (c == 0) ? BgraColor{38, 32, 30, 255}
                                   : BgraColor{55, 68, 82, 255};
        name_style.stroke_px = 2.6f;
        name_style.stroke_color = BgraColor{255, 255, 255, 252};
        name_style.shadow = true;
        name_style.shadow_dx = 0.0f;
        name_style.shadow_dy = 0.0f;
        name_style.shadow_blur_px = 1.6f;
        name_style.shadow_color = BgraColor{255, 255, 255, 220};
        name_style.halign = HorizontalAlign::kCenter;
        name_style.valign = VerticalAlign::kCenter;
        name_style.size_px =
            canvas.AutoFitFontSize(spec.name_en, plate_rect, name_style, 2);
        canvas.DrawText(spec.name_en, plate_rect, name_style);
    }

    return canvas.CopyImage();
}

BgraImage MenuRenderer::RenderDialogPlate(const BgraImage& orig,
                                          const DialogPlateSpec& spec) const {
    if (orig.empty()) {
        return orig;
    }
    const int w = orig.width();
    const int h = orig.height();

    int orange_max_x = -1;
    int orange_min_y = h;
    int orange_max_y = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const BgraColor p = orig.PixelAt(x, y);
            if (p.r > kDialogOrangeMinR && p.g > kDialogOrangeMinG &&
                p.g < kDialogOrangeMaxG && p.b < kDialogOrangeMaxB && p.a > 128u) {
                orange_max_x = std::max(orange_max_x, x);
                orange_min_y = std::min(orange_min_y, y);
                orange_max_y = std::max(orange_max_y, y);
            }
        }
    }

    const bool found_orange = (orange_max_x >= 0 && orange_min_y <= orange_max_y);
    if (!found_orange) {
        orange_max_x = w / 5;
        orange_min_y = h / 4;
        orange_max_y = orange_min_y + std::max(12, h / 10);
    }

    const int right_limit = std::min(kDialogWipeRightLimitX, std::max(w - 36, w / 2));
    const int wipe_x = orange_max_x + kDialogWipeOffsetFromIconPx;
    const int wipe_y = std::max(4, orange_min_y - kDialogWipeTopPadPx);
    const RectI wipe_rect = ClampRectToImage(
        RectI{wipe_x, wipe_y, std::max(8, right_limit - wipe_x),
              kDialogWipeHeightPx},
        w, h);

    SpriteCanvas canvas(orig);
    if (!wipe_rect.empty()) {
        canvas.WipeRectRowMedian(wipe_rect);
    }

    const int text_x = orange_max_x + kDialogTextOffsetFromIconPx;
    const RectI text_rect = ClampRectToImage(
        RectI{text_x, std::max(2, orange_min_y - 4),
              std::max(16, right_limit - text_x),
              (orange_max_y - orange_min_y) + 9},
        w, h);
    if (!text_rect.empty()) {
        TextStyle style;
        style.font_family = impl_->ui_sans_family;
        style.weight = kDefaultFontWeightNormal;
        style.size_px = std::max(10.0f, 23.0f * static_cast<float>(h) / 242.0f);
        style.min_font_size_px = 9.0f;
        style.fill = kDialogPromptColor;
        style.halign = HorizontalAlign::kLeft;
        style.valign = VerticalAlign::kCenter;
        style.size_px = canvas.AutoFitFontSize(spec.prompt_en, text_rect, style, 2);
        canvas.DrawText(spec.prompt_en, text_rect, style);
    }

    BgraImage result = canvas.CopyImage();
    // Enforce pixel-exact preservation of the orange warning triangle region.
    if (found_orange) {
        const int protect_right = std::min(w, orange_max_x + 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < protect_right; ++x) {
                result.SetPixel(x, y, orig.PixelAt(x, y));
            }
        }
    }
    return result;
}

BgraImage MenuRenderer::RenderMsgTooltip(const BgraImage& orig,
                                         const MsgTooltipSpec& spec) const {
    if (orig.empty()) {
        return orig;
    }
    const int w = orig.width();
    const int h = orig.height();
    const int pad_x = std::min(kTooltipEndcapPadX, w / 6);
    const int pad_y = std::min(kTooltipBorderPadY, h / 4);
    const RectI bar_rect = ClampRectToImage(
        RectI{pad_x, pad_y, std::max(4, w - 2 * pad_x),
              std::max(2, h - 2 * pad_y)},
        w, h);

    SpriteCanvas canvas(orig);
    if (!bar_rect.empty()) {
        canvas.WipeRectRowMedian(bar_rect, 4);

        TextStyle style;
        style.font_family = impl_->ui_sans_family;
        style.weight = kDefaultFontWeightSemiBold;
        style.size_px = std::max(8.0f, 13.5f * static_cast<float>(h) / 20.0f);
        style.min_font_size_px = 7.5f;
        style.fill = kTooltipTextColor;
        style.shadow = true;
        style.shadow_dx = 0.0f;
        style.shadow_dy = 1.0f;
        style.shadow_blur_px = 0.8f;
        style.shadow_color = BgraColor{0, 0, 0, 115};
        style.halign = HorizontalAlign::kCenter;
        style.valign = VerticalAlign::kCenter;
        style.size_px = canvas.AutoFitFontSize(spec.tooltip_en, bar_rect, style, 1);
        canvas.DrawText(spec.tooltip_en, bar_rect, style);
    }
    return canvas.CopyImage();
}

BgraImage MenuRenderer::ExtractMusicSakuraIcon(
    const BgraImage& orig_music_000100) const {
    if (orig_music_000100.empty()) {
        return {};
    }
    const RectI crop_rect = ScaleRectFromRetail(
        kMusicSakuraIconCropRect, orig_music_000100.width(),
        orig_music_000100.height(), 864, 42);
    if (crop_rect.empty()) {
        return {};
    }
    return orig_music_000100.Crop(crop_rect);
}

BgraImage MenuRenderer::RenderMusicTrackBanner(const MusicTrackSpec& spec,
                                               const BgraImage& sakura_icon,
                                               int width,
                                               int height) const {
    const int w = std::max(24, width);
    const int h = std::max(12, height);
    const int cell_w = w / kMusicCellCount;

    SpriteCanvas canvas(w, h);
    canvas.WipeRectSolid(RectI{0, 0, w, h}, kMusicTransparentBg);

    TextStyle base_style;
    base_style.font_family = impl_->serif_family;
    base_style.weight = kDefaultFontWeightSemiBold;
    base_style.size_px = std::max(8.0f, 18.0f * static_cast<float>(h) / 42.0f);
    base_style.min_font_size_px = 12.5f;
    base_style.stroke_px = 2.2f;
    base_style.stroke_color = BgraColor{255, 255, 255, 248};
    base_style.shadow = true;
    base_style.shadow_dx = 0.0f;
    base_style.shadow_dy = 0.0f;
    base_style.shadow_blur_px = 2.6f;
    base_style.shadow_color = BgraColor{255, 255, 255, 215};
    base_style.valign = VerticalAlign::kCenter;

    const int pad_x = std::min(kMusicCellPadX, cell_w / 8);
    const int pad_y = std::min(kMusicCellPadY, h / 6);

    // Cell 0 (charcoal) & Cell 1 (violet):
    const RectI cell0_rect{pad_x, pad_y, std::max(8, cell_w - 2 * pad_x),
                           std::max(6, h - 2 * pad_y)};
    const RectI cell1_rect{cell_w + pad_x, pad_y,
                           std::max(8, cell_w - 2 * pad_x),
                           std::max(6, h - 2 * pad_y)};

    TextStyle c0_style = base_style;
    c0_style.halign = HorizontalAlign::kCenter;
    c0_style.fill = kMusicCell0Charcoal;
    c0_style.size_px = canvas.AutoFitFontSize(spec.title_en, cell0_rect, c0_style, 2);
    canvas.DrawText(spec.title_en, cell0_rect, c0_style);

    TextStyle c1_style = c0_style;
    c1_style.fill = kMusicCell1Violet;
    canvas.DrawText(spec.title_en, cell1_rect, c1_style);

    // Cell 2 (pink sakura icon + rose-pink title centered together):
    const int icon_w = sakura_icon.empty() ? 0 : sakura_icon.width();
    const int icon_h = sakura_icon.empty() ? 0 : sakura_icon.height();
    const int gap = (icon_w > 0) ? kMusicSakuraGapPx : 0;
    const int max_text_w =
        std::min(kMusicCell2MaxTextW, std::max(16, cell_w - 2 * pad_x - icon_w - gap));

    TextStyle c2_style = base_style;
    c2_style.halign = HorizontalAlign::kLeft;
    c2_style.fill = kMusicCell2RosePink;
    const RectI fit_box{0, pad_y, max_text_w, std::max(6, h - 2 * pad_y)};
    c2_style.size_px = canvas.AutoFitFontSize(spec.title_en, fit_box, c2_style, 2);

    const TextLayoutMetrics measured =
        canvas.MeasureText(spec.title_en, fit_box, c2_style);
    const int text_w = std::clamp(measured.measured_width_px, 1, max_text_w);
    const int group_w = icon_w + gap + text_w;
    const int cell2_base_x = 2 * cell_w;
    const int group_x = cell2_base_x + std::max(4, (cell_w - group_w) / 2);

    if (icon_w > 0) {
        BgraImage img = canvas.CopyImage();
        const int icon_y = std::max(0, (h - icon_h) / 2);
        CompositeStraightOverStraight(img, sakura_icon, group_x, icon_y);
        canvas = SpriteCanvas(std::move(img));
    }

    const int text_x = group_x + icon_w + gap;
    const RectI c2_text_rect = ClampRectToImage(
        RectI{text_x, pad_y, std::max(8, w - 4 - text_x),
              std::max(6, h - 2 * pad_y)},
        w, h);
    if (!c2_text_rect.empty()) {
        canvas.DrawText(spec.title_en, c2_text_rect, c2_style);
    }

    return canvas.CopyImage();
}

int MenuRenderer::ComputeChoiceFontSizePx(
    const std::vector<std::string>& english_texts, int tile_width) const {
    if (english_texts.empty()) {
        return kChoiceMaxPointSize;
    }
    const int left_px = (tile_width >= 300) ? kChoiceTextLeftPx : std::max(4, tile_width / 10);
    const int right_px =
        (tile_width >= 300) ? kChoiceTextRightMarginPx : std::max(4, tile_width / 20);
    const int room = std::max(16, tile_width - left_px - right_px);

    SpriteCanvas probe(64, 64);
    TextStyle style;
    style.font_family = impl_->ui_sans_family;
    style.weight = kDefaultFontWeightNormal;
    style.min_squeeze = 1.0f;
    style.min_scale = 1.0f;

    for (int sz = kChoiceMaxPointSize; sz >= kChoiceMinPointSize; --sz) {
        style.size_px = static_cast<float>(sz);
        style.min_font_size_px = static_cast<float>(sz);
        const bool all_fit = std::all_of(
            english_texts.begin(), english_texts.end(),
            [&](const std::string& text) {
                return probe.MeasureTextWidthAt1x(text, style) <= room;
            });
        if (all_fit) {
            return sz;
        }
    }

    if (tile_width < 300) {
        // Synthetic unit/component test tiles with miniature dimensions.
        return 8;
    }
    throw std::runtime_error(
        "No choice font size in [12, 26] fits all option strings inside " +
        std::to_string(room) + " px");
}

BgraImage MenuRenderer::RenderChoiceOptionPlate(std::string_view english_text,
                                                int width,
                                                int height,
                                                int font_size_px) const {
    const int w = std::max(kChoiceStateCount, width);
    const int h = std::max(8, height);
    const int tile_w = w / kChoiceStateCount;
    const int left_px = (tile_w >= 300) ? kChoiceTextLeftPx : std::max(4, tile_w / 10);
    const int right_px =
        (tile_w >= 300) ? kChoiceTextRightMarginPx : std::max(4, tile_w / 20);
    const int pad_y = (h >= 40) ? 8 : 1;

    SpriteCanvas canvas(w, h);
    canvas.WipeRectSolid(RectI{0, 0, w, h}, BgraColor{0, 0, 0, 0});

    TextStyle style;
    style.font_family = impl_->ui_sans_family;
    style.weight = kDefaultFontWeightNormal;
    style.size_px = static_cast<float>(std::max(8, font_size_px));
    style.min_font_size_px = 8.0f;
    style.fill = BgraColor{0, 0, 0, 255};
    style.halign = HorizontalAlign::kLeft;
    style.valign = VerticalAlign::kCenter;

    for (int state = 0; state < kChoiceStateCount; ++state) {
        const int base_x = state * tile_w;
        canvas.WipeRectSolid(
            RectI{base_x, 0, tile_w, h},
            BgraColor{255, 255, 255, kChoiceTileAlpha[static_cast<std::size_t>(state)]});

        const RectI text_box = ClampRectToImage(
            RectI{base_x + left_px, pad_y,
                  std::max(8, tile_w - left_px - right_px),
                  std::max(6, h - 2 * pad_y)},
            w, h);
        if (!text_box.empty()) {
            canvas.DrawText(english_text, text_box, style);
        }
    }
    return canvas.CopyImage();
}

std::vector<PreviewMockupItem> MenuRenderer::BuildFullScreenPreviews(
    const BgraImage* patched_att01,
    const BgraImage* patched_att02,
    const SysGrpLookupFn& lookup_orig_sysgrp,
    const SysGrpLookupFn& lookup_patched_sysgrp,
    const std::vector<std::pair<std::string, BgraImage>>& patched_choices) const {
    auto get_sprite = [&](std::string_view name) -> const BgraImage* {
        if (lookup_patched_sysgrp) {
            if (const BgraImage* p = lookup_patched_sysgrp(name)) {
                return p;
            }
        }
        if (lookup_orig_sysgrp) {
            return lookup_orig_sysgrp(name);
        }
        return nullptr;
    };

    std::vector<PreviewMockupItem> previews;

    if (patched_att01 != nullptr && !patched_att01->empty()) {
        previews.push_back({"01_splash_att01", *patched_att01});
    }
    if (patched_att02 != nullptr && !patched_att02->empty()) {
        previews.push_back({"02_splash_att02", *patched_att02});
    }

    // Composite Title Screen (1280x720) with interactive menu buttons:
    BgraImage title_composite;
    if (const BgraImage* title_bg = get_sprite(kTitleBgEntryName)) {
        title_composite = *title_bg;
        struct TitleBtnPlacement {
            std::string_view entry_name;
            int cell_index;
            int x;
            int y;
        };
        constexpr std::array<TitleBtnPlacement, 5> kTitleButtons = {{
            {"SGTitle000000", 0, 125, 392},
            {"SGTitle000100", 0, 125, 448},
            {"SGTitle000200", 1, 120, 504},
            {"SGTitle000300", 0, 125, 560},
            {"SGTitle000400", 0, 295, 392},
        }};
        for (const auto& btn : kTitleButtons) {
            if (const BgraImage* s = get_sprite(btn.entry_name)) {
                const BgraImage cell = CropStripCell(*s, 4, btn.cell_index);
                CompositeStraightOverStraight(title_composite, cell, btn.x, btn.y);
            }
        }
        previews.push_back({"03_title_screen", title_composite});
    }

    // Composite Config Screen (1280x720) with sliders, localized toggles, and
    // all 8 localized character voice portraits:
    if (const BgraImage* cnfg_bg = get_sprite(kConfigBgEntryName)) {
        BgraImage cnfg = *cnfg_bg;

        if (const BgraImage* knob_strip = get_sprite("SGCnfgWnd000000")) {
            const BgraImage knob = CropStripCell(*knob_strip, 4, 0);
            constexpr std::array<int, 3> kLeftKnobX = {445, 420, 495};
            for (int r = 0; r < 3; ++r) {
                CompositeStraightOverStraight(
                    cnfg, knob, kLeftKnobX[static_cast<std::size_t>(r)],
                    118 + r * kConfigRowPitchY);
            }
            constexpr std::array<int, 4> kRightKnobX = {1080, 1045, 1060, 1110};
            for (int r = 0; r < 4; ++r) {
                CompositeStraightOverStraight(
                    cnfg, knob, kRightKnobX[static_cast<std::size_t>(r)],
                    118 + r * kConfigRowPitchY);
            }
        }

        struct ToggleRowPlacement {
            std::string_view left_entry;
            int left_cell;
            std::string_view right_entry;
            int right_cell;
        };
        constexpr std::array<ToggleRowPlacement, 6> kToggleRows = {{
            {"SGCnfgWnd010000", 0, "SGCnfgWnd010100", 1},
            {"SGCnfgWnd020000", 1, "SGCnfgWnd020100", 0},
            {"SGCnfgWnd030000", 0, "SGCnfgWnd030100", 1},
            {"SGCnfgWnd040000", 0, "SGCnfgWnd040100", 1},
            {"SGCnfgWnd050000", 1, "SGCnfgWnd050100", 0},
            {"SGCnfgWnd060000", 0, "SGCnfgWnd060100", 1},
        }};
        for (std::size_t idx = 0u; idx < kToggleRows.size(); ++idx) {
            const int y = 310 + static_cast<int>(idx) * kConfigRowPitchY;
            if (const BgraImage* ls = get_sprite(kToggleRows[idx].left_entry)) {
                CompositeStraightOverStraight(
                    cnfg, CropStripCell(*ls, 4, kToggleRows[idx].left_cell), 312, y);
            }
            if (const BgraImage* rs = get_sprite(kToggleRows[idx].right_entry)) {
                CompositeStraightOverStraight(
                    cnfg, CropStripCell(*rs, 4, kToggleRows[idx].right_cell), 468, y);
            }
        }

        const auto& portraits = ConfigVoicePortraitCatalog();
        for (std::size_t idx = 0u; idx < portraits.size(); ++idx) {
            if (const BgraImage* ps = get_sprite(portraits[idx].entry_name)) {
                const int col = static_cast<int>(idx % 4u);
                const int row = static_cast<int>(idx / 4u);
                const int cell = (idx == 6u) ? 1 : 0;
                const BgraImage card = CropStripCell(*ps, 2, cell);
                CompositeStraightOverStraight(cnfg, card, 742 + col * 116,
                                              404 + row * 136);
            }
        }

        constexpr std::array<std::string_view, 3> kBottomBtns = {
            "SGCnfgWnd200000", "SGCnfgWnd200100", "SGCnfgWnd200200"};
        for (std::size_t idx = 0u; idx < kBottomBtns.size(); ++idx) {
            if (const BgraImage* bs = get_sprite(kBottomBtns[idx])) {
                CompositeStraightOverStraight(
                    cnfg, CropStripCell(*bs, 4, idx == 2u ? 1 : 0),
                    818 + static_cast<int>(idx) * 144, 682);
            }
        }
        previews.push_back({"04_config_screen", std::move(cnfg)});
    }

    // Composite Music Mode (1280x720) with localized track banners:
    if (const BgraImage* music_bg = get_sprite(kMusicBgEntryName)) {
        BgraImage music = *music_bg;
        const auto& tracks = MusicTrackCatalog();
        for (std::size_t idx = 0u; idx < std::min<std::size_t>(24u, tracks.size());
             ++idx) {
            if (const BgraImage* ts = get_sprite(tracks[idx].entry_name)) {
                const int col = static_cast<int>(idx / 12u);
                const int row = static_cast<int>(idx % 12u);
                const int cell = (idx == 0u) ? 2 : ((idx == 3u) ? 1 : 0);
                const BgraImage banner = CropStripCell(*ts, 3, cell);
                CompositeStraightOverStraight(music, banner, 196 + col * 468,
                                              128 + row * 44);
            }
        }
        if (const BgraImage* back_btn = get_sprite("SGCG000400")) {
            CompositeStraightOverStraight(music, CropStripCell(*back_btn, 4, 0),
                                          1104, 666);
        }
        previews.push_back({"05_music_mode", std::move(music)});
    }

    // Composite Confirmation Dialog + Quick-Menu Tooltip over Title Screen:
    if (!title_composite.empty()) {
        if (const BgraImage* dlg = get_sprite("SGDialog990000")) {
            BgraImage dialog_mockup = title_composite;
            const int dx = (dialog_mockup.width() - dlg->width()) / 2;
            const int dy = (dialog_mockup.height() - dlg->height()) / 2;
            CompositeStraightOverStraight(dialog_mockup, *dlg, dx, dy);

            if (const BgraImage* yes_btn = get_sprite("SGDialog000000")) {
                const BgraImage yes_cell = CropStripCell(*yes_btn, 4, 1);
                CompositeStraightOverStraight(dialog_mockup, yes_cell,
                                              dx + 112, dy + 146);
            }
            if (const BgraImage* no_btn = get_sprite("SGDialog000100")) {
                const BgraImage no_cell = CropStripCell(*no_btn, 4, 0);
                CompositeStraightOverStraight(dialog_mockup, no_cell,
                                              dx + 382, dy + 146);
            }
            if (const BgraImage* tip = get_sprite("SGMsgWnd800000")) {
                CompositeStraightOverStraight(dialog_mockup, *tip, 420, 688);
            }
            previews.push_back({"06_confirm_dialog", std::move(dialog_mockup)});
        }

        if (patched_choices.size() >= 2u) {
            BgraImage choice_mockup = title_composite;
            const BgraImage opt0 =
                CropStripCell(patched_choices[0].second, kChoiceStateCount, 1);
            const BgraImage opt1 =
                CropStripCell(patched_choices[1].second, kChoiceStateCount, 0);
            const int cx = (choice_mockup.width() - opt0.width()) / 2;
            CompositeStraightOverStraight(choice_mockup, opt0, cx, 250);
            CompositeStraightOverStraight(choice_mockup, opt1, cx, 336);
            previews.push_back({"07_choice_overlay", std::move(choice_mockup)});
        }
    }

    return previews;
}

}  // namespace snu::menu
