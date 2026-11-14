// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_supplementary.h"

#include "g00/g00_codec.h"
#include "imaging/bgra_image.h"
#include "menu/core/sprite_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adi::menu {

namespace {

using imaging::BgraColor;
using imaging::BgraImage;
using imaging::RectI;

constexpr std::string_view kSupplementaryDomainName = "render_supplementary";

// Asset inventory counts (2 + 3 + 84 = 89 patched .g00 assets).
constexpr std::size_t kBannerAssetCount = 2;
constexpr std::size_t kMusicAssetCount = 3;
constexpr std::size_t kBannerCutCount = 8;
constexpr std::size_t kMusicTrackCount = 14;
constexpr std::size_t kCgTickerCount = 84;
constexpr std::size_t kTotalSupplementaryAssetCount = 89;

static_assert(kBannerAssetCount + kMusicAssetCount + kCgTickerCount ==
              kTotalSupplementaryAssetCount);

// Printable ASCII bounds for compile-time English string validation.
constexpr char kAsciiSpaceMin = ' ';
constexpr char kAsciiTildeMax = '~';
constexpr std::string_view kG00Extension = ".g00";

// Color channel & BT.601 integer luminance constants.
constexpr std::uint8_t kChannelMax = 255u;
constexpr int kLumaWeightR = 299;
constexpr int kLumaWeightG = 587;
constexpr int kLumaWeightB = 114;
constexpr int kLumaDivisor = 1000;
constexpr int kPairAverageDivisor = 2;
constexpr int kMaxHistogramValue = 255;
constexpr std::size_t kHistogramBinCount = 256;

// --- CM_B02.g00 & CM_B03.g00 banner layout & stroke-mask constants ---
constexpr std::string_view kBannerHoverFilename = "CM_B02.g00";
constexpr std::string_view kBannerPushFilename = "CM_B03.g00";

constexpr int kBannerOriginX = 0;
constexpr int kBannerSliceWidth = 372;
constexpr int kBannerSliceHeight = 60;
constexpr int kBannerBorderKeepRows = 1;

constexpr int kKanjiBoxY = 1;
constexpr int kKanjiBoxH = 58;
constexpr int kEvenKanjiBoxX = 2;
constexpr int kEvenKanjiBoxW = 146;
constexpr int kOddKanjiBoxX = 172;
constexpr int kOddKanjiBoxW = 152;
constexpr int kEtcKanjiBoxX = 168;
constexpr int kEtcKanjiBoxW = 158;
constexpr int kEvenEyeShadowStartX = 134;

constexpr int kBannerBgWindowRadius = 7;
constexpr int kBannerBgPercentileNumer = 68;
constexpr int kBannerBgPercentileDenom = 100;
constexpr int kBannerR03DropThreshold = 28;
constexpr int kBannerLuma02DropThreshold = 24;
constexpr int kBannerGb03DropThreshold = 24;
constexpr int kBannerMaskDilateRadius = 2;

constexpr int kBannerTextY = 2;
constexpr int kBannerTextH = 56;
constexpr int kBannerLeftTextX = 4;
constexpr int kBannerLeftTextW = 148;
constexpr int kBannerRightTextX = 168;
constexpr int kBannerRightTextW = 148;

constexpr float kBannerNameFontSizePx = 50.0f;
constexpr float kBannerEtcFontSizePx = 48.0f;
constexpr float kBannerMinSqueeze = 0.50f;
constexpr float kBannerMinScale = 0.75f;
constexpr float kBannerHaloBlurPx = 1.0f;

constexpr BgraColor kBannerHoverInk{0x4Au, 0x12u, 0x0Eu, 0xFFu};
constexpr BgraColor kBannerHoverHalo{0xFFu, 0xF4u, 0xF0u, 0x58u};
constexpr BgraColor kBannerPushInk{0x0Eu, 0x08u, 0x5Cu, 0xFFu};
constexpr BgraColor kBannerPushHalo{0xDCu, 0xE0u, 0xFFu, 0x60u};

// --- MUSIC1.g00 .. MUSIC3.g00 tracklist layout & style constants ---
constexpr std::string_view kMusic1Filename = "MUSIC1.g00";
constexpr std::string_view kMusic2Filename = "MUSIC2.g00";
constexpr std::string_view kMusic3Filename = "MUSIC3.g00";

constexpr int kMusicMinSplitX = 160;
constexpr int kMusicDefaultSplitX = 188;
constexpr int kMusicSplitSafetyPadPx = 2;
constexpr int kMusicBgMatchTolerance = 2;
constexpr std::uint8_t kMusicNonBlackFloor = 4u;
constexpr int kMusicColumnMatchMinRows = 160;
constexpr int kMusicRightBorderGuardPx = 2;
constexpr int kMusicTopMarginY = 2;
constexpr int kMusicBottomMarginY = 2;

constexpr int kMusicRowSeedDiffThreshold = 16;
constexpr int kMusicRowSeedMinPixels = 4;
constexpr int kMusicRowMergeGapScanlines = 3;
constexpr int kMusicRowMinHeightScanlines = 6;
constexpr double kMusicFallbackFirstCenterY = 20.0;
constexpr double kMusicFallbackPitchY = 33.77;

constexpr int kMusicEraseDiffThreshold = 6;
constexpr int kMusicMaskDilateRadius = 2;

constexpr int kMusicTextLeftPadPx = 2;
constexpr int kMusicTextRightMarginPx = 14;
constexpr int kMusicRowBoxHeight = 28;
constexpr int kMusicRowBoxMinY = 1;
constexpr float kMusicFontSizePx = 17.0f;
constexpr float kMusicMinSqueeze = 0.78f;
constexpr float kMusicMinScale = 0.76f;

constexpr BgraColor kMusic1Fill{0xFFu, 0xFFu, 0xFFu, 0xFFu};
constexpr BgraColor kMusic1Stroke{0x12u, 0x12u, 0x12u, 0xF2u};
constexpr BgraColor kMusic1Shadow{0x00u, 0x00u, 0x00u, 0xA8u};
constexpr float kMusic1StrokePx = 1.35f;
constexpr float kMusic1ShadowOffsetPx = 1.0f;
constexpr float kMusic1ShadowBlurPx = 1.0f;

constexpr BgraColor kMusic2Fill{0x10u, 0x10u, 0x10u, 0xFFu};
constexpr BgraColor kMusic2Stroke{0xFFu, 0xFFu, 0xFFu, 0xFAu};
constexpr BgraColor kMusic2Shadow{0xFFu, 0xFFu, 0xFFu, 0xD8u};
constexpr float kMusic2StrokePx = 1.65f;
constexpr float kMusic2ShadowBlurPx = 1.6f;

constexpr BgraColor kMusic3Fill{0xFFu, 0xFFu, 0xFFu, 0xFFu};
constexpr BgraColor kMusic3Stroke{0x00u, 0x58u, 0xE8u, 0xFFu};
constexpr BgraColor kMusic3Shadow{0x00u, 0x78u, 0xFFu, 0xE0u};
constexpr float kMusic3StrokePx = 1.75f;
constexpr float kMusic3ShadowBlurPx = 1.8f;

// --- CM101.g00 .. CM806.g00 scrolling CG-caption ticker constants ---
constexpr int kTickerInkMinContrast = 40;
constexpr BgraColor kTickerFallbackInk{0x10u, 0x10u, 0x10u, 0xFFu};
constexpr int kTickerMinWideCanvasW = 512;
constexpr int kTickerPixelsPerCharEstimate = 14;
constexpr int kTickerWideMarginW = 64;
constexpr int kTickerSafePadX = 8;
constexpr float kTickerFontSizePx = 12.0f;
constexpr float kTickerMinSqueeze = 1.0f;
constexpr float kTickerMinScale = 0.85f;
constexpr std::size_t kMinInterpolatedCuts = 3;

struct BannerCutSpec {
    std::string_view jp_source;
    std::string_view en_text;
    bool left_side;
    bool center_in_box;
};

struct MusicTrackSpec {
    std::string_view jp_source;
    std::string_view en_text;
};

struct CgTickerSpec {
    std::string_view filename;
    std::string_view jp_source;
    std::string_view en_text;
};

constexpr std::array<BannerCutSpec, kBannerCutCount> kBannerCutSpecs = {{
    {"巣鴨睦月", "mutsuki sugamo", true, false},
    {"高田望美", "nozomi takada", false, false},
    {"田町まひる", "mahiru tamachi", true, false},
    {"目黒御幸", "miyuki meguro", false, false},
    {"上野こより", "koyori ueno", true, false},
    {"高島瀬美奈", "semina takashima", false, false},
    {"大森となえ", "tonae omori", true, false},
    {"地水火風天", "e   t   c", false, true},
}};

constexpr std::array<MusicTrackSpec, kMusicTrackCount> kMusicTrackSpecs = {{
    {"さよならの黄昏", "soiree des au revoires (Twilight of Farewells)"},
    {"逆光天使", "l'ange a contre jour (Backlit Angel)"},
    {"疑問符と共に", "avec un point d'interrogation (With a Question Mark)"},
    {"猫の時間", "le temps du chat (Time of the Cat)"},
    {"鋭角の微笑", "sourire de aigu (Acute Smile)"},
    {"やわらかい花弁", "petale souple (Soft Petals)"},
    {"上から", "venant des anciens (From Above)"},
    {"怠惰", "feignant (Indolence)"},
    {"檻の中で見た夢", "reve en cage (Dream in a Cage)"},
    {"虚無回廊", "couloir des vanites (Corridor of Vanity)"},
    {"流れとよどみ", "immobilite et tourbillon (Stillness and Eddy)"},
    {"密かなる野蛮", "secret de folie (Secret Madness)"},
    {"さよならの黄昏2", "soiree des au revoires II (Twilight of Farewells II)"},
    {"さよならを教えて", "comment te dire adieu (How to Say Goodbye)"},
}};

constexpr std::array<CgTickerSpec, kCgTickerCount> kCgTickerSpecs = {{
    // Sugamo Mutsuki (CM101..CM114)
    {"CM101.g00", "炎上する立ち木は今にも倒れそうだがそれでもなお美しい",
     "The burning tree looks ready to fall at any moment, yet remains beautiful"},
    {"CM102.g00", "忍び寄る影の肖像", "Portrait of a Creeping Shadow"},
    {"CM103.g00", "肉色のメルヘン", "A Flesh-Colored Fairy Tale"},
    {"CM104.g00", "孔雀狩り～或いは夢見る者と見られる者の尊厳を賭けた果たし合い",
     "Peacock Hunting -- Or a Duel Staking the Dignity of Dreamer and Dreamed"},
    {"CM105.g00", "ゴム", "Rubber"},
    {"CM106.g00", "望郷・母の夢", "Homesickness: Dream of Mother"},
    {"CM107.g00", "バイブレーション！！", "Vibration!!"},
    {"CM108.g00", "貴方は本当は嘘つきだったのでした", "You Truly Were a Liar After All"},
    {"CM109.g00", "晩鐘", "Evening Bell"},
    {"CM110.g00", "恋の生贄", "Sacrifice of Love"},
    {"CM111.g00", "レモン味のキッス", "A Lemon-Flavored Kiss"},
    {"CM112.g00", "祭の夜", "Festival Night"},
    {"CM113.g00", "妖精さん達の帽子", "The Fairies' Hats"},
    {"CM114.g00", "命令違反", "Insubordination"},
    // Takada Nozomi (CM201..CM213)
    {"CM201.g00", "世界の端の滝", "Waterfall at the Edge of the World"},
    {"CM202.g00", "呪詛", "Malediction"},
    {"CM203.g00", "水菓子の収穫における逆さバケツの効能",
     "The Utility of an Overturned Bucket in Harvesting Fruit"},
    {"CM204.g00", "脳漿小町", "Belle of Brain Matter"},
    {"CM205.g00", "嵐の晩", "Stormy Evening"},
    {"CM206.g00", "ノスタルジア", "Nostalgia"},
    {"CM207.g00", "パッチン留めの思い出", "Memories of a Snap Hairclip"},
    {"CM208.g00", "平衡感覚の致命的誤差", "A Fatal Error in Sense of Balance"},
    {"CM209.g00", "焼肉の活造り", "Live-Carved Roast Meat"},
    {"CM210.g00", "ジャンケンの掟", "The Law of Rock-Paper-Scissors"},
    {"CM211.g00", "明日への飛翔", "Flight Toward Tomorrow"},
    {"CM212.g00", "こんにちは", "Hello"},
    {"CM213.g00", "虚勢", "False Bravado"},
    // Tamachi Mahiru (CM301..CM314)
    {"CM301.g00", "遠近法愛好癖", "A Fetish for Perspective"},
    {"CM302.g00", "塩鮭", "Salted Salmon"},
    {"CM303.g00", "独裁者の凱旋", "Triumph of the Dictator"},
    {"CM304.g00", "野生動物の軌道", "Trajectory of a Wild Animal"},
    {"CM305.g00", "化学反応", "Chemical Reaction"},
    {"CM306.g00", "輪になって踊ろう", "Let's Dance in a Circle"},
    {"CM307.g00", "脈打つ縞模様", "Pulsating Stripes"},
    {"CM308.g00", "愛玩動物", "Companion Animal"},
    {"CM309.g00", "大好きだから", "Because I Love You So Much"},
    {"CM310.g00", "底抜けの壷", "Bottomless Jar"},
    {"CM311.g00", "シャンペン・シャワー！", "Champagne Shower!"},
    {"CM312.g00", "餌付け", "Hand-Feeding"},
    {"CM313.g00", "悪戯の代償", "The Price of Mischief"},
    {"CM314.g00", "悪戯の代償", "The Price of Mischief"},
    // Meguro Miyuki (CM401..CM414)
    {"CM401.g00", "哨戒", "Patrol"},
    {"CM402.g00", "内なる窓", "The Window Within"},
    {"CM403.g00", "寝坊した茨姫", "The Briar Rose Who Overslept"},
    {"CM404.g00", "一番大切な標本", "The Most Precious Specimen"},
    {"CM405.g00", "あらかじめ熱しておくと瓶の蓋が開けやすくなります",
     "Warming the jar beforehand makes the lid easier to open"},
    {"CM406.g00", "軟体", "Soft Body"},
    {"CM407.g00", "クロイクツシタ", "Black Socks"},
    {"CM408.g00", "ハンプティダンプティ子", "Humpty Dumpty-ko"},
    {"CM409.g00", "許す人、許さない人", "One Who Forgives, One Who Does Not"},
    {"CM410.g00", "だるまさんがころんだ", "Daruma-san Fell Down"},
    {"CM411.g00", "午睡の夢", "Dream in Afternoon Slumber"},
    {"CM412.g00", "水栽培のある風景", "Landscape with Hydroponics"},
    {"CM413.g00", "遠雷", "Distant Thunder"},
    {"CM414.g00", "だって私は女だから", "Because I am a woman, after all"},
    // Ueno Koyori (CM501..CM513)
    {"CM501.g00", "長くうねる道", "The Long and Winding Road"},
    {"CM502.g00", "ほころび", "A Frayed Seam"},
    {"CM503.g00", "こっち側、あっち側", "This Side, That Side"},
    {"CM504.g00", "夢で逢えたら", "If We Could Meet in a Dream"},
    {"CM505.g00", "トモダチ", "Friends"},
    {"CM506.g00", "作用・反作用・二等兵", "Action, Reaction, Private Second Class"},
    {"CM507.g00", "穴", "Hole"},
    {"CM508.g00", "人形遊び", "Playing with Dolls"},
    {"CM509.g00", "希望的観測", "Wishful Thinking"},
    {"CM510.g00", "境界線上のアリス", "Alice on the Borderline"},
    {"CM511.g00", "性悪説", "Doctrine of Innate Evil"},
    {"CM512.g00", "糖衣", "Sugar Coating"},
    {"CM513.g00", "天然の美", "Beauty of Nature"},
    // Takashima Semina (CM601..CM605)
    {"CM601.g00", "if", "if"},
    {"CM602.g00", "僕にはキミの言っていることがわからないけれどそれで満足かい？",
     "I don't understand what you're saying, but does that satisfy you?"},
    {"CM603.g00", "愉快犯的復讐", "Thrill-Seeker's Vengeance"},
    {"CM604.g00", "プラスチックス", "Plastics"},
    {"CM605.g00", "捜査官・怒る！", "Investigator Enraged!"},
    // Oomori Tonae (CM701..CM705)
    {"CM701.g00", "毒は惜しみなく", "Poison, Unsparingly"},
    {"CM702.g00", "弛緩", "Languor"},
    {"CM703.g00", "陶酔のテーブルクロス、その傾向と対策",
     "Tablecloth of Ecstasy: Trends and Countermeasures"},
    {"CM704.g00", "あの空を飛ぶ飛行船のように", "Like That Airship Flying Across the Sky"},
    {"CM705.g00", "ママレードキッズ", "Marmalade Kids"},
    // Etc / Angel (CM801..CM806)
    {"CM801.g00", "ノアの箱舟", "Noah's Ark"},
    {"CM802.g00", "ゲルニカ", "Guernica"},
    {"CM803.g00", "ぱらいそ", "Paraiso"},
    {"CM804.g00", "みんなは一人のために、一人はみんなのために", "All for One, One for All"},
    {"CM805.g00", "薔薇が咲いた", "A Rose Has Bloomed"},
    {"CM806.g00", "ムゲンノチカラ", "Infinite Power"},
}};

constexpr bool IsNonEmptyPrintableAscii(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < kAsciiSpaceMin || ch > kAsciiTildeMax) {
            return false;
        }
    }
    return true;
}

constexpr bool EndsWithG00Extension(std::string_view filename) {
    if (filename.size() <= kG00Extension.size()) {
        return false;
    }
    return filename.substr(filename.size() - kG00Extension.size()) == kG00Extension;
}

constexpr bool ValidateAllSupplementaryTables() {
    for (const BannerCutSpec& spec : kBannerCutSpecs) {
        if (spec.jp_source.empty() || !IsNonEmptyPrintableAscii(spec.en_text)) {
            return false;
        }
    }
    for (const MusicTrackSpec& spec : kMusicTrackSpecs) {
        if (spec.jp_source.empty() || !IsNonEmptyPrintableAscii(spec.en_text)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < kCgTickerSpecs.size(); ++i) {
        const CgTickerSpec& spec = kCgTickerSpecs[i];
        if (!EndsWithG00Extension(spec.filename) || spec.jp_source.empty() ||
            !IsNonEmptyPrintableAscii(spec.en_text)) {
            return false;
        }
        for (std::size_t j = i + 1; j < kCgTickerSpecs.size(); ++j) {
            if (spec.filename == kCgTickerSpecs[j].filename) {
                return false;
            }
        }
    }
    return true;
}

static_assert(ValidateAllSupplementaryTables(),
              "Supplementary asset tables must have unique .g00 names and ASCII translations");

int ComputeBt601Luminance(const BgraColor& c) noexcept {
    return (kLumaWeightR * static_cast<int>(c.r) +
            kLumaWeightG * static_cast<int>(c.g) +
            kLumaWeightB * static_cast<int>(c.b)) /
           kLumaDivisor;
}

int MaxChannelAbsDiff(const BgraColor& a, const BgraColor& b) noexcept {
    const int db = std::abs(static_cast<int>(a.b) - static_cast<int>(b.b));
    const int dg = std::abs(static_cast<int>(a.g) - static_cast<int>(b.g));
    const int dr = std::abs(static_cast<int>(a.r) - static_cast<int>(b.r));
    return std::max({db, dg, dr});
}

std::vector<std::uint8_t> DilateMaskChebyshev(const std::vector<std::uint8_t>& src,
                                              int width,
                                              int height,
                                              int radius) {
    if (width <= 0 || height <= 0 || radius <= 0) {
        return src;
    }
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height),
                                  0u);
    for (int y = 0; y < height; ++y) {
        const int ny0 = std::max(0, y - radius);
        const int ny1 = std::min(height - 1, y + radius);
        for (int x = 0; x < width; ++x) {
            const int nx0 = std::max(0, x - radius);
            const int nx1 = std::min(width - 1, x + radius);
            std::uint8_t hit = 0u;
            for (int ny = ny0; ny <= ny1 && hit == 0u; ++ny) {
                const std::size_t row_off =
                    static_cast<std::size_t>(ny) * static_cast<std::size_t>(width);
                for (int nx = nx0; nx <= nx1; ++nx) {
                    if (src[row_off + static_cast<std::size_t>(nx)] != 0u) {
                        hit = kChannelMax;
                        break;
                    }
                }
            }
            dst[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x)] = hit;
        }
    }
    return dst;
}

int SampleWindowPercentile(const std::vector<int>& plane,
                           int plane_w,
                           int min_y,
                           int max_y_excl,
                           int cx,
                           int cy,
                           int radius,
                           int pct_numer,
                           int pct_denom) noexcept {
    const int x0 = std::max(0, cx - radius);
    const int x1 = std::min(plane_w - 1, cx + radius);
    const int y0 = std::max(min_y, cy - radius);
    const int y1 = std::min(max_y_excl - 1, cy + radius);

    std::array<int, kHistogramBinCount> hist{};
    int total = 0;
    for (int y = y0; y <= y1; ++y) {
        const std::size_t row_off =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(plane_w);
        for (int x = x0; x <= x1; ++x) {
            const int val =
                std::clamp(plane[row_off + static_cast<std::size_t>(x)], 0, kMaxHistogramValue);
            ++hist[static_cast<std::size_t>(val)];
            ++total;
        }
    }
    if (total <= 0) {
        return 0;
    }

    const int target_rank = (total * pct_numer) / pct_denom;
    int cumulative = 0;
    for (std::size_t bin = 0; bin < kHistogramBinCount; ++bin) {
        cumulative += hist[bin];
        if (cumulative > target_rank) {
            return static_cast<int>(bin);
        }
    }
    return kMaxHistogramValue;
}

RectI ResolveBannerKanjiBox(const BannerCutSpec& spec, int slice_w, int slice_h) noexcept {
    if (spec.left_side) {
        return imaging::ClampRect(
            RectI{kEvenKanjiBoxX, kKanjiBoxY, kEvenKanjiBoxW, kKanjiBoxH}, slice_w, slice_h);
    }
    if (spec.center_in_box) {
        return imaging::ClampRect(
            RectI{kEtcKanjiBoxX, kKanjiBoxY, kEtcKanjiBoxW, kKanjiBoxH}, slice_w, slice_h);
    }
    return imaging::ClampRect(
        RectI{kOddKanjiBoxX, kKanjiBoxY, kOddKanjiBoxW, kKanjiBoxH}, slice_w, slice_h);
}

RectI ResolveBannerTextBounds(const BannerCutSpec& spec, int slice_w, int slice_h) noexcept {
    if (spec.left_side) {
        return imaging::ClampRect(
            RectI{kBannerLeftTextX, kBannerTextY, kBannerLeftTextW, kBannerTextH},
            slice_w, slice_h);
    }
    return imaging::ClampRect(
        RectI{kBannerRightTextX, kBannerTextY, kBannerRightTextW, kBannerTextH},
        slice_w, slice_h);
}

HorizontalAlign ResolveBannerHorizontalAlign(const BannerCutSpec& spec) noexcept {
    if (spec.left_side) {
        return HorizontalAlign::kLeft;
    }
    if (spec.center_in_box) {
        return HorizontalAlign::kCenter;
    }
    return HorizontalAlign::kRight;
}

// CM_B02 (hover) and CM_B03 (push) carry identical kanji glyph coordinates per
// cut: CM_B02 blends dark kanji over the RGB-inverted face banner, whereas
// CM_B03 tints the face banner to R~255 while kanji strokes drop either R
// (black/dark-crimson strokes, even over vivid-red skin shadows) or G+B (red
// strokes over pastel backgrounds). Combining both channels yields a unified
// glyph mask that preserves dark eye/skin illustration regions.
std::vector<std::uint8_t> BuildBannerKanjiMask(const BgraImage& slice02,
                                               const BgraImage& slice03,
                                               const RectI& kanji_box,
                                               const BannerCutSpec& spec) {
    const int slice_w = slice02.width();
    const int slice_h = slice02.height();
    const std::size_t pixel_count =
        static_cast<std::size_t>(slice_w) * static_cast<std::size_t>(slice_h);

    std::vector<int> luma02(pixel_count, 0);
    std::vector<int> r03(pixel_count, 0);
    std::vector<int> gb03(pixel_count, 0);

    for (int y = 0; y < slice_h; ++y) {
        const std::size_t row_off =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(slice_w);
        for (int x = 0; x < slice_w; ++x) {
            const BgraColor p2 = slice02.PixelAt(x, y);
            const BgraColor p3 = slice03.PixelAt(x, y);
            const std::size_t idx = row_off + static_cast<std::size_t>(x);
            luma02[idx] = ComputeBt601Luminance(p2);
            r03[idx] = static_cast<int>(p3.r);
            gb03[idx] =
                (static_cast<int>(p3.g) + static_cast<int>(p3.b)) / kPairAverageDivisor;
        }
    }

    const int sample_y0 = std::min(kBannerBorderKeepRows, slice_h);
    const int sample_y1 = std::max(sample_y0 + 1, slice_h - kBannerBorderKeepRows);

    std::vector<std::uint8_t> seed_mask(
        static_cast<std::size_t>(kanji_box.w) * static_cast<std::size_t>(kanji_box.h), 0u);

    for (int ly = 0; ly < kanji_box.h; ++ly) {
        const int sy = kanji_box.y + ly;
        for (int lx = 0; lx < kanji_box.w; ++lx) {
            const int sx = kanji_box.x + lx;
            const std::size_t s_idx =
                static_cast<std::size_t>(sy) * static_cast<std::size_t>(slice_w) +
                static_cast<std::size_t>(sx);

            const int bg_r = SampleWindowPercentile(
                r03, slice_w, sample_y0, sample_y1, sx, sy, kBannerBgWindowRadius,
                kBannerBgPercentileNumer, kBannerBgPercentileDenom);
            const bool r_drop = (bg_r - r03[s_idx] >= kBannerR03DropThreshold);

            bool tinted_stroke = false;
            if (!r_drop && (!spec.left_side || sx < kEvenEyeShadowStartX)) {
                const int bg_luma = SampleWindowPercentile(
                    luma02, slice_w, sample_y0, sample_y1, sx, sy, kBannerBgWindowRadius,
                    kBannerBgPercentileNumer, kBannerBgPercentileDenom);
                const int bg_gb = SampleWindowPercentile(
                    gb03, slice_w, sample_y0, sample_y1, sx, sy, kBannerBgWindowRadius,
                    kBannerBgPercentileNumer, kBannerBgPercentileDenom);
                tinted_stroke =
                    (bg_luma - luma02[s_idx] >= kBannerLuma02DropThreshold) &&
                    (bg_gb - gb03[s_idx] >= kBannerGb03DropThreshold);
            }

            if (r_drop || tinted_stroke) {
                seed_mask[static_cast<std::size_t>(ly) * static_cast<std::size_t>(kanji_box.w) +
                          static_cast<std::size_t>(lx)] = kChannelMax;
            }
        }
    }

    return DilateMaskChebyshev(seed_mask, kanji_box.w, kanji_box.h, kBannerMaskDilateRadius);
}

void WritePatchedBannerSlice(BgraImage& dst_full,
                             const BgraImage& patched_slice,
                             const RectI& slice_rect) {
    for (int ly = 0; ly < slice_rect.h; ++ly) {
        const int dy = slice_rect.y + ly;
        for (int lx = 0; lx < slice_rect.w; ++lx) {
            const int dx = slice_rect.x + lx;
            const BgraColor orig = dst_full.PixelAt(dx, dy);
            if (orig.a == 0u) {
                continue;
            }
            BgraColor out = patched_slice.PixelAt(lx, ly);
            out.a = orig.a;
            dst_full.SetPixel(dx, dy, out);
        }
    }
}

void ApplyBannerHoverAndPush(PatchBuildContext& ctx, const std::wstring& banner_font) {
    g00::G00Image b02 = ctx.LoadG00(kBannerHoverFilename);
    g00::G00Image b03 = ctx.LoadG00(kBannerPushFilename);

    if (b02.type != g00::G00Type::kType2MultiCutBgra32 ||
        b03.type != g00::G00Type::kType2MultiCutBgra32 ||
        b02.cuts.size() < kBannerCutCount ||
        b03.cuts.size() < kBannerCutCount) {
        throw std::runtime_error(
            "RenderSupplementary: CM_B02.g00 and CM_B03.g00 must be Type 2 sprites with 8 cuts");
    }

    TextStyle hover_style;
    hover_style.font_family = banner_font;
    hover_style.min_squeeze = kBannerMinSqueeze;
    hover_style.min_scale = kBannerMinScale;
    hover_style.valign = VerticalAlign::kCenter;
    hover_style.fill = kBannerHoverInk;
    hover_style.shadow = true;
    hover_style.shadow_blur_px = kBannerHaloBlurPx;
    hover_style.shadow_color = kBannerHoverHalo;

    TextStyle push_style = hover_style;
    push_style.fill = kBannerPushInk;
    push_style.shadow_color = kBannerPushHalo;

    std::set<std::size_t> modified_cuts;
    for (std::size_t cut_idx = 0; cut_idx < kBannerCutCount; ++cut_idx) {
        const BannerCutSpec& spec = kBannerCutSpecs[cut_idx];
        BgraImage& full02 = b02.MutableCutCanvas(cut_idx);
        BgraImage& full03 = b03.MutableCutCanvas(cut_idx);

        const int y_base = static_cast<int>(cut_idx) * kBannerSliceHeight;
        const RectI slice_rect = imaging::ClampRect(
            RectI{kBannerOriginX, y_base, kBannerSliceWidth, kBannerSliceHeight},
            std::min(full02.width(), full03.width()),
            std::min(full02.height(), full03.height()));
        if (slice_rect.empty()) {
            continue;
        }

        BgraImage slice02 = full02.Crop(slice_rect);
        BgraImage slice03 = full03.Crop(slice_rect);
        const RectI kanji_box =
            ResolveBannerKanjiBox(spec, slice_rect.w, slice_rect.h);

        if (!kanji_box.empty()) {
            const std::vector<std::uint8_t> kanji_mask =
                BuildBannerKanjiMask(slice02, slice03, kanji_box, spec);

            SpriteCanvas canvas02(std::move(slice02));
            SpriteCanvas canvas03(std::move(slice03));
            canvas02.InpaintMaskedLaplaceSor(kanji_box, kanji_mask);
            canvas03.InpaintMaskedLaplaceSor(kanji_box, kanji_mask);

            const RectI text_bounds =
                ResolveBannerTextBounds(spec, slice_rect.w, slice_rect.h);
            const HorizontalAlign halign = ResolveBannerHorizontalAlign(spec);
            const float font_px =
                spec.center_in_box ? kBannerEtcFontSizePx : kBannerNameFontSizePx;

            hover_style.halign = halign;
            hover_style.size_px = font_px;
            push_style.halign = halign;
            push_style.size_px = font_px;

            (void)canvas02.DrawText(spec.en_text, text_bounds, hover_style);
            (void)canvas03.DrawText(spec.en_text, text_bounds, push_style);

            WritePatchedBannerSlice(full02, canvas02.image(), slice_rect);
            WritePatchedBannerSlice(full03, canvas03.image(), slice_rect);
        }

        modified_cuts.insert(cut_idx);
    }

    ctx.SubmitPatchedG00(kBannerHoverFilename, b02, modified_cuts);
    ctx.SubmitPatchedG00(kBannerPushFilename, b03, modified_cuts);
}

// Detects the column where MUSIC2 and MUSIC3 transition from their solid-black
// left mask into the shared right-side illustration identical to MUSIC1.
int DetectMusicSharedIllustrationStartX(const BgraImage& m1,
                                        const BgraImage& m2,
                                        const BgraImage& m3) {
    const int w = m1.width();
    const int h = m1.height();
    const int search_x0 = std::min(kMusicMinSplitX, w);
    const int search_x1 = w / kPairAverageDivisor;

    for (int x = search_x0; x < search_x1; ++x) {
        int matching_non_black_rows = 0;
        for (int y = 0; y < h; ++y) {
            const BgraColor p1 = m1.PixelAt(x, y);
            const BgraColor p2 = m2.PixelAt(x, y);
            const BgraColor p3 = m3.PixelAt(x, y);
            const bool non_black = (p2.r > kMusicNonBlackFloor) ||
                                   (p2.g > kMusicNonBlackFloor) ||
                                   (p2.b > kMusicNonBlackFloor);
            if (!non_black) {
                continue;
            }
            if (MaxChannelAbsDiff(p1, p2) <= kMusicBgMatchTolerance &&
                MaxChannelAbsDiff(p1, p3) <= kMusicBgMatchTolerance) {
                ++matching_non_black_rows;
            }
        }
        if (matching_non_black_rows >= kMusicColumnMatchMinRows) {
            return x;
        }
    }
    return std::min(kMusicDefaultSplitX, std::max(0, w - 1));
}

std::array<int, kMusicTrackCount> DetectMusicRowCentersY(
    const std::vector<int>& row_seed_counts,
    int canvas_h) {
    std::vector<std::pair<int, int>> intervals;
    int y = 0;
    while (y < canvas_h) {
        if (row_seed_counts[static_cast<std::size_t>(y)] < kMusicRowSeedMinPixels) {
            ++y;
            continue;
        }
        const int start_y = y;
        while (y < canvas_h &&
               row_seed_counts[static_cast<std::size_t>(y)] >= kMusicRowSeedMinPixels) {
            ++y;
        }
        const int end_y = y - 1;
        if (!intervals.empty() &&
            start_y - intervals.back().second <= kMusicRowMergeGapScanlines) {
            intervals.back().second = end_y;
            continue;
        }
        intervals.emplace_back(start_y, end_y);
    }

    std::vector<std::pair<int, int>> filtered;
    filtered.reserve(intervals.size());
    for (const auto& span : intervals) {
        if (span.second - span.first + 1 >= kMusicRowMinHeightScanlines) {
            filtered.push_back(span);
        }
    }

    std::array<int, kMusicTrackCount> centers{};
    if (filtered.size() == kMusicTrackCount) {
        for (std::size_t i = 0; i < kMusicTrackCount; ++i) {
            centers[i] = (filtered[i].first + filtered[i].second) / kPairAverageDivisor;
        }
        return centers;
    }

    for (std::size_t i = 0; i < kMusicTrackCount; ++i) {
        centers[i] = std::clamp(
            static_cast<int>(std::lround(kMusicFallbackFirstCenterY +
                                         static_cast<double>(i) * kMusicFallbackPitchY)),
            0, std::max(0, canvas_h - 1));
    }
    return centers;
}

bool HasAnyNonZeroByte(const std::vector<std::uint8_t>& mask) noexcept {
    for (const std::uint8_t v : mask) {
        if (v != 0u) {
            return true;
        }
    }
    return false;
}

void ApplyMusicTracklists(PatchBuildContext& ctx, const std::wstring& music_font) {
    g00::G00Image m1 = ctx.LoadG00(kMusic1Filename);
    g00::G00Image m2 = ctx.LoadG00(kMusic2Filename);
    g00::G00Image m3 = ctx.LoadG00(kMusic3Filename);

    if (m1.type != g00::G00Type::kType0Bgr24 ||
        m2.type != g00::G00Type::kType0Bgr24 ||
        m3.type != g00::G00Type::kType0Bgr24 ||
        m1.width <= 0 || m1.height <= 0 ||
        m2.width != m1.width || m2.height != m1.height ||
        m3.width != m1.width || m3.height != m1.height) {
        throw std::runtime_error(
            "RenderSupplementary: MUSIC1..MUSIC3 must be matching Type 0 canvases");
    }

    const int canvas_w = m1.width;
    const int canvas_h = m1.height;
    const BgraImage& src1 = m1.canvas;
    const BgraImage& src2 = m2.canvas;
    const BgraImage& src3 = m3.canvas;

    const int split_x = DetectMusicSharedIllustrationStartX(src1, src2, src3);
    const int track_x0 =
        std::min(canvas_w - 1, split_x + kMusicSplitSafetyPadPx);
    const int track_x1 =
        std::max(track_x0 + 1, canvas_w - kMusicRightBorderGuardPx);
    const int track_w = track_x1 - track_x0;

    // On x >= track_x0, MUSIC1..MUSIC3 share the identical lossless illustration
    // background, whereas every glyph interior, outline, shadow, halo, and orange
    // glow pixel differs across the three states.
    std::vector<int> row_seed_counts(static_cast<std::size_t>(canvas_h), 0);
    std::vector<std::uint8_t> raw_erase_mask(
        static_cast<std::size_t>(track_w) * static_cast<std::size_t>(canvas_h), 0u);

    const int active_y0 = std::min(kMusicTopMarginY, canvas_h);
    const int active_y1 = std::max(active_y0, canvas_h - kMusicBottomMarginY);

    for (int y = active_y0; y < active_y1; ++y) {
        const std::size_t row_off =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(track_w);
        int row_seeds = 0;
        for (int lx = 0; lx < track_w; ++lx) {
            const int x = track_x0 + lx;
            const BgraColor p1 = src1.PixelAt(x, y);
            const BgraColor p2 = src2.PixelAt(x, y);
            const BgraColor p3 = src3.PixelAt(x, y);
            const int max_diff = std::max({MaxChannelAbsDiff(p1, p2),
                                           MaxChannelAbsDiff(p1, p3),
                                           MaxChannelAbsDiff(p2, p3)});
            if (max_diff >= kMusicEraseDiffThreshold) {
                raw_erase_mask[row_off + static_cast<std::size_t>(lx)] = kChannelMax;
            }
            if (max_diff >= kMusicRowSeedDiffThreshold) {
                ++row_seeds;
            }
        }
        row_seed_counts[static_cast<std::size_t>(y)] = row_seeds;
    }

    const std::vector<std::uint8_t> erase_mask =
        DilateMaskChebyshev(raw_erase_mask, track_w, canvas_h, kMusicMaskDilateRadius);
    const std::array<int, kMusicTrackCount> row_centers =
        DetectMusicRowCentersY(row_seed_counts, canvas_h);

    SpriteCanvas canvas1(m1.canvas);
    SpriteCanvas canvas2(m2.canvas);
    SpriteCanvas canvas3(m3.canvas);

    for (std::size_t i = 0; i < kMusicTrackCount; ++i) {
        const int strip_y0 =
            (i == 0)
                ? active_y0
                : (row_centers[i - 1] + row_centers[i]) / kPairAverageDivisor;
        const int strip_y1 =
            (i + 1 == kMusicTrackCount)
                ? active_y1
                : (row_centers[i] + row_centers[i + 1]) / kPairAverageDivisor;
        if (strip_y1 <= strip_y0) {
            continue;
        }

        const int strip_h = strip_y1 - strip_y0;
        std::vector<std::uint8_t> strip_mask(
            static_cast<std::size_t>(track_w) * static_cast<std::size_t>(strip_h), 0u);
        for (int ly = 0; ly < strip_h; ++ly) {
            const std::size_t src_off =
                static_cast<std::size_t>(strip_y0 + ly) * static_cast<std::size_t>(track_w);
            const std::size_t dst_off =
                static_cast<std::size_t>(ly) * static_cast<std::size_t>(track_w);
            std::copy_n(erase_mask.data() + src_off,
                        static_cast<std::size_t>(track_w),
                        strip_mask.data() + dst_off);
        }
        if (!HasAnyNonZeroByte(strip_mask)) {
            continue;
        }

        const RectI strip_rect{track_x0, strip_y0, track_w, strip_h};
        canvas1.InpaintMaskedLaplaceSor(strip_rect, strip_mask);
        canvas2.InpaintMaskedLaplaceSor(strip_rect, strip_mask);
        canvas3.InpaintMaskedLaplaceSor(strip_rect, strip_mask);
    }

    TextStyle style1;
    style1.font_family = music_font;
    style1.size_px = kMusicFontSizePx;
    style1.min_squeeze = kMusicMinSqueeze;
    style1.min_scale = kMusicMinScale;
    style1.halign = HorizontalAlign::kRight;
    style1.valign = VerticalAlign::kCenter;
    style1.fill = kMusic1Fill;
    style1.stroke_px = kMusic1StrokePx;
    style1.stroke_color = kMusic1Stroke;
    style1.shadow = true;
    style1.shadow_dx = kMusic1ShadowOffsetPx;
    style1.shadow_dy = kMusic1ShadowOffsetPx;
    style1.shadow_blur_px = kMusic1ShadowBlurPx;
    style1.shadow_color = kMusic1Shadow;

    TextStyle style2 = style1;
    style2.fill = kMusic2Fill;
    style2.stroke_px = kMusic2StrokePx;
    style2.stroke_color = kMusic2Stroke;
    style2.shadow_dx = 0.0f;
    style2.shadow_dy = 0.0f;
    style2.shadow_blur_px = kMusic2ShadowBlurPx;
    style2.shadow_color = kMusic2Shadow;

    TextStyle style3 = style1;
    style3.fill = kMusic3Fill;
    style3.stroke_px = kMusic3StrokePx;
    style3.stroke_color = kMusic3Stroke;
    style3.shadow_dx = 0.0f;
    style3.shadow_dy = 0.0f;
    style3.shadow_blur_px = kMusic3ShadowBlurPx;
    style3.shadow_color = kMusic3Shadow;

    const int text_x0 = track_x0 + kMusicTextLeftPadPx;
    const int text_x1 = std::max(text_x0 + 1, canvas_w - kMusicTextRightMarginPx);
    const int text_w = text_x1 - text_x0;
    const int max_box_y = std::max(kMusicRowBoxMinY, canvas_h - kMusicRowBoxHeight - kMusicRowBoxMinY);

    for (std::size_t i = 0; i < kMusicTrackCount; ++i) {
        const int box_y = std::clamp(
            row_centers[i] - kMusicRowBoxHeight / kPairAverageDivisor,
            kMusicRowBoxMinY, max_box_y);
        const RectI row_bounds{text_x0, box_y, text_w, kMusicRowBoxHeight};
        const std::string_view label = kMusicTrackSpecs[i].en_text;

        (void)canvas1.DrawText(label, row_bounds, style1);
        (void)canvas2.DrawText(label, row_bounds, style2);
        (void)canvas3.DrawText(label, row_bounds, style3);
    }

    m1.MutableCanvas() = canvas1.CopyImage();
    m2.MutableCanvas() = canvas2.CopyImage();
    m3.MutableCanvas() = canvas3.CopyImage();

    ctx.SubmitPatchedG00(kMusic1Filename, m1);
    ctx.SubmitPatchedG00(kMusic2Filename, m2);
    ctx.SubmitPatchedG00(kMusic3Filename, m3);
}

BgraColor SampleTickerInkColor(const BgraImage& mid_frame,
                               const BgraColor& bg_color) {
    const int bg_luma = ComputeBt601Luminance(bg_color);
    int best_luma = bg_luma;
    BgraColor best_color = kTickerFallbackInk;
    bool found = false;

    for (int y = 0; y < mid_frame.height(); ++y) {
        for (int x = 0; x < mid_frame.width(); ++x) {
            const BgraColor p = mid_frame.PixelAt(x, y);
            if (p.a == 0u) {
                continue;
            }
            const int luma = ComputeBt601Luminance(p);
            if (luma < best_luma) {
                best_luma = luma;
                best_color = p;
                best_color.a = kChannelMax;
                found = true;
            }
        }
    }

    if (found && bg_luma - best_luma >= kTickerInkMinContrast) {
        return best_color;
    }
    return kTickerFallbackInk;
}

void BlitTickerStripFrame(BgraImage& dst,
                          const BgraImage& wide_strip,
                          int strip_x0,
                          int strip_w,
                          int draw_x) {
    const int sx_begin = std::max(0, -draw_x);
    const int sx_end = std::min(strip_w, dst.width() - draw_x);
    if (sx_end <= sx_begin) {
        return;
    }

    const int copy_h = std::min(dst.height(), wide_strip.height());
    for (int y = 0; y < copy_h; ++y) {
        for (int sx = sx_begin; sx < sx_end; ++sx) {
            dst.SetPixel(draw_x + sx, y, wide_strip.PixelAt(strip_x0 + sx, y));
        }
    }
}

void PatchSingleCgTicker(PatchBuildContext& ctx,
                         const CgTickerSpec& spec,
                         const TextStyle& base_style) {
    g00::G00Image img = ctx.LoadG00(spec.filename);
    if (img.type != g00::G00Type::kType2MultiCutBgra32 || img.cuts.empty() ||
        img.width <= 0 || img.height <= 0) {
        throw std::runtime_error("RenderSupplementary: invalid multi-cut CG ticker " +
                                 std::string(spec.filename));
    }

    const int bar_w = img.width;
    const int bar_h = img.height;
    const std::size_t cut_count = img.cuts.size();

    BgraColor bg_color = img.cuts.front().canvas.PixelAt(0, 0);
    bg_color.a = kChannelMax;
    const BgraColor ink_color =
        SampleTickerInkColor(img.cuts[cut_count / static_cast<std::size_t>(kPairAverageDivisor)].canvas,
                             bg_color);

    const int estimated_text_w =
        static_cast<int>(spec.en_text.size()) * kTickerPixelsPerCharEstimate + kTickerWideMarginW;
    const int wide_w = std::max(kTickerMinWideCanvasW, estimated_text_w);

    BgraImage wide_img(wide_w, bar_h, bg_color);

    TextStyle style = base_style;
    style.fill = ink_color;

    SpriteCanvas wide_canvas(std::move(wide_img));
    const RectI safe_bounds{kTickerSafePadX, 0,
                            std::max(1, wide_w - kTickerSafePadX * kPairAverageDivisor), bar_h};
    const TextLayoutMetrics metrics = wide_canvas.DrawText(spec.en_text, safe_bounds, style);
    const BgraImage& rendered = wide_canvas.image();

    int strip_x0 = kTickerSafePadX;
    int strip_w = 1;
    if (!metrics.ink_bounds.empty()) {
        strip_x0 = std::clamp(metrics.ink_bounds.x, 0, rendered.width() - 1);
        strip_w = std::clamp(metrics.ink_bounds.w, 1, rendered.width() - strip_x0);
    }

    std::set<std::size_t> modified_cuts;
    for (std::size_t k = 0; k < cut_count; ++k) {
        BgraImage& cut_canvas = img.MutableCutCanvas(k);
        cut_canvas.Fill(bg_color);
        modified_cuts.insert(k);

        if (cut_count < kMinInterpolatedCuts) {
            if (k + 1 == cut_count) {
                const int centered_x = (bar_w - strip_w) / kPairAverageDivisor;
                BlitTickerStripFrame(cut_canvas, rendered, strip_x0, strip_w, centered_x);
            }
            continue;
        }

        if (k == 0 || k + 1 == cut_count) {
            continue;
        }

        const double progress =
            static_cast<double>(k) / static_cast<double>(cut_count - 1u);
        const int draw_x = static_cast<int>(std::lround(
            static_cast<double>(bar_w) -
            progress * static_cast<double>(bar_w + strip_w)));
        BlitTickerStripFrame(cut_canvas, rendered, strip_x0, strip_w, draw_x);
    }

    ctx.SubmitPatchedG00(spec.filename, img, modified_cuts);
}

void ApplyCgCaptionTickers(PatchBuildContext& ctx, const std::wstring& ticker_font) {
    TextStyle base_style;
    base_style.font_family = ticker_font;
    base_style.size_px = kTickerFontSizePx;
    base_style.min_squeeze = kTickerMinSqueeze;
    base_style.min_scale = kTickerMinScale;
    base_style.halign = HorizontalAlign::kLeft;
    base_style.valign = VerticalAlign::kCenter;

    for (const CgTickerSpec& spec : kCgTickerSpecs) {
        PatchSingleCgTicker(ctx, spec, base_style);
    }
}

}  // namespace

struct RenderSupplementary::Impl {
    bool fonts_resolved = false;
    std::wstring banner_serif_font;
    std::wstring music_serif_font;
    std::wstring ticker_sans_font;

    void EnsureFontsResolved() {
        if (fonts_resolved) {
            return;
        }
        banner_serif_font = ResolveInstalledFontFamily(
            {L"Times New Roman", L"Georgia", L"Palatino Linotype"});
        music_serif_font = ResolveInstalledFontFamily(
            {L"Georgia", L"Palatino Linotype", L"Times New Roman"});
        ticker_sans_font = ResolveInstalledFontFamily(
            {L"Segoe UI", L"Trebuchet MS", L"Tahoma"});
        fonts_resolved = true;
    }
};

RenderSupplementary::RenderSupplementary() : impl_(std::make_unique<Impl>()) {}

RenderSupplementary::RenderSupplementary(RenderSupplementary&& other) noexcept = default;

RenderSupplementary& RenderSupplementary::operator=(RenderSupplementary&& other) noexcept = default;

RenderSupplementary::~RenderSupplementary() = default;

std::string RenderSupplementary::DomainName() const {
    return std::string(kSupplementaryDomainName);
}

void RenderSupplementary::Apply(PatchBuildContext& ctx) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->EnsureFontsResolved();

    const std::size_t before_count = ctx.submitted_record_count();
    ApplyBannerHoverAndPush(ctx, impl_->banner_serif_font);
    ApplyMusicTracklists(ctx, impl_->music_serif_font);
    ApplyCgCaptionTickers(ctx, impl_->ticker_sans_font);

    const std::size_t delta = ctx.submitted_record_count() - before_count;
    if (delta != kTotalSupplementaryAssetCount) {
        throw std::logic_error(
            "RenderSupplementary: expected 89 submitted .g00 records, got " +
            std::to_string(delta));
    }
}

std::unique_ptr<IMenuDomainRenderer> CreateRenderSupplementary() {
    return std::make_unique<RenderSupplementary>();
}

}  // namespace adi::menu
