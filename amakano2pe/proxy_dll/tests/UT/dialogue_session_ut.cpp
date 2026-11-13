// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — CatSystem2 proportional pixel wrapping, inline keywait carry,
// speaker-name isolation, slot ring-buffer deduplication, UTF-8 truncation,
// control-tag parsing, and GDI font-measurement tests.

#include "gdi_text_measurer.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using gdi_text::GdiTextWidthMeasurer;
using gdi_text::IsMeswndDialogueLayoutSize;
using gdi_text::IsMeswndString8BaseLayout;
using gdi_text::ResolveString8BaseFontObservation;
using gdi_text::ScaleEngineFontHeight;
using gdi_text::String8BaseFontObservation;
using gdi_text::String8LayoutSizes;
using gdi_text::kBacklogDialogueLayoutSizes;
using gdi_text::kBacklogPresetSizeNormal;
using gdi_text::kBoldFontWeight;
using gdi_text::kDefaultFontWeight;
using gdi_text::kDefaultMaxDialogueLineWidthPx;
using gdi_text::kDefaultScaledDialogueLfHeight;
using gdi_text::kEngineTextboxPixelWidth;
using gdi_text::kFontSizePresetCount;
using gdi_text::kLayoutPresetSlotNormal;
using gdi_text::kMeasureGlyphsFullwidthBaseFontRetRva;
using gdi_text::kMeasureGlyphsHalfwidthBaseFontRetRva;
using gdi_text::kMeasureHalfwidthEbpFrameDisp;
using gdi_text::kMeswndDialogueLayoutSizes;
using gdi_text::kMeswndPresetSizeFl;
using gdi_text::kMeswndPresetSizeFs;
using gdi_text::kMeswndPresetSizeNormal;
using gdi_text::kPrimaryFontContextSlot;
using gdi_text::kRenderFullwidthBaseFontRetRva;
using gdi_text::kRenderHalfwidthBaseFontRetRva;
using gdi_text::kRubyLayoutSize;
using gdi_text::kSpeakerNameplateLayoutSize;
using gdi_text::kString8LayoutSizesOffset;
using gdi_text::kString8MeasureWeightOffset;
using gdi_text::kString8MinReadableBytes;
using gdi_text::kString8RenderWeightOffset;
using translator_logic::CodePointWidthMeasurer;
using translator_logic::DefaultSpeakerNameKeys;
using translator_logic::DialogueSessionConfig;
using translator_logic::DialogueTranslationSession;
using translator_logic::EndsWithInlineWaitTag;
using translator_logic::HasDanglingTrailingBackslash;
using translator_logic::MonospaceByteWidthMeasurer;
using translator_logic::SpeakerKeySet;
using translator_logic::StripDanglingTrailingBackslash;
using translator_logic::StripLeadingControlNewline;
using translator_logic::TextWidthMeasurer;
using translator_logic::TranslationMap;
using translator_logic::TranslationResult;
using translator_logic::TranslationSlotRingBuffer;
using translator_logic::Utf8SafeTruncate;
using translator_logic::Utf8SafeTruncateForWideLimit;
using translator_logic::WordWrapByWidth;
using translator_logic::WordWrapForEngine;
using translator_logic::kDefaultMaxDialogueWidthPx;
using translator_logic::kDefaultMaxOutputBytes;
using translator_logic::kDefaultSlotPoolCount;
using translator_logic::kMaxSpeakerWideChars;
using translator_logic::kSingleCharDecoderMaxWchars;

constexpr std::size_t kDialogueMaxWchars = 4096;
constexpr std::size_t kString8SetTextMaxWchars = 2048;
constexpr std::size_t kOverMaxSpeakerWchars = kMaxSpeakerWideChars + 1;

constexpr std::size_t kMockSpaceWidthPx = 7;
constexpr std::size_t kMockNarrowGlyphWidthPx = 5;
constexpr std::size_t kMockWideGlyphWidthPx = 13;
constexpr std::size_t kMockNormalGlyphWidthPx = 11;

constexpr std::size_t kSingleCharStep = 1;
constexpr std::size_t kEscapePairLength = 2;

constexpr std::size_t kFirstLineIndex = 0;
constexpr std::size_t kSecondLineIndex = 1;
constexpr std::size_t kExpectedTwoLines = 2;
constexpr std::size_t kExpectedThreeLines = 3;
constexpr std::size_t kMaxMeswndVisibleLines = 3;
constexpr std::size_t kHalfWidthDivisor = 2;
constexpr std::size_t kTightByteWrapLimit = 20;
constexpr std::size_t kWideByteWrapLimit = 40;
constexpr std::size_t kSingleUnitPerCodePoint = 1;
constexpr std::size_t kDeduplicatedWrapLimit = 40;
constexpr std::size_t kMinLongParagraphBytes = 650;
constexpr std::size_t kCafeOnlyMaxWchars = 7;
constexpr std::size_t kMaxFourWideUnits = 4;
constexpr std::size_t kSafeTruncateTestLimit = 10;
constexpr std::size_t kCutInsideThreeByteUtf8 = 3;
constexpr std::size_t kExpectedAsciiPrefixLen = 3;
constexpr std::size_t kExpectedSingleAsciiByte = 1;
constexpr std::size_t kExpectedThreeWideBytes = 3;
constexpr std::size_t kExtraWraparoundSpeakers = 4;
constexpr std::size_t kConcurrentReaderThreads = 4;
constexpr std::size_t kConcurrentIterations = 200;

constexpr int kExpectedScaledRubyLfHeight = -8;
constexpr int kExpectedScaledBacklogNormalLfHeight = -19;
constexpr int kExpectedScaledPresetFlLfHeight = -24;
constexpr int kSecondaryFontContextSlot = 1;

constexpr std::uintptr_t kSyntheticModuleBase = 0x00400000u;
constexpr std::uintptr_t kUnderflowEbp = 0x10u;
constexpr std::size_t kFakeStackFrameBytes = 0x40u;
constexpr std::array<std::uintptr_t, 5> kInlineFontOverrideRetRvas = {
    0x225A24u,
    0x225CCCu,
    0x225D3Eu,
    0x22BB34u,
    0x22BDBFu,
};

constexpr std::size_t kCutMidEscapedPairBytes = 5;
constexpr std::size_t kNearRightMarginOffsetPx = 2;
constexpr std::size_t kMaxCandidateWordAppends = 128;
constexpr std::size_t kNoWrapCodePointLimit = 256;

constexpr std::size_t kNullTerminatorChars = 1;
constexpr std::wstring_view kMeiryoJapaneseFaceName =
    L"\u30E1\u30A4\u30EA\u30AA";
constexpr std::wstring_view kMeiryoEnglishFaceName = L"Meiryo";
constexpr std::wstring_view kArialFaceName = L"Arial";

constexpr std::string_view kTrainSceneJpSeg1 =
    "\xE6\x96\xB0\xE5\xB9\xB9\xE7\xB7\x9A\xE3\x81\x8C\xE6\xAD\xA2\xE3\x81"
    "\xBE\xE3\x82\x8B\xE3\x81\xA8\xE3\x80\x81\xE6\x96\xB0\xE3\x81\x9F\xE3"
    "\x81\xAB\xE4\xBA\xBA\xE3\x81\x8C\xE4\xB9\x97\xE8\xBB\x8A\xE3\x81\x97"
    "\xE3\x81\xA6\xE3\x81\x8F\xE3\x82\x8B\xE3\x80\x82";
constexpr std::string_view kTrainSceneEnSeg1 =
    "When the train stopped, new passengers began boarding.";
constexpr std::string_view kTrainSceneJpSeg2 =
    "\\n\xE9\x99\x8D\xE3\x82\x8A\xE3\x82\x8B\xE3\x82\x88\xE3\x82\x8A\xE3"
    "\x80\x81\xE4\xB9\x97\xE3\x82\x8B\xE4\xBA\xBA\xE3\x81\xAE\xE6\x96\xB9"
    "\xE3\x81\x8C\xE5\xA4\x9A\xE3\x81\x84\xE3\x80\x82";
constexpr std::string_view kTrainSceneEnSeg2 =
    "\\nMore people were getting on than getting off.";
constexpr std::string_view kTrainSceneFitsPrefix =
    "When the train stopped, new passengers began boarding. More peopl";
constexpr std::string_view kTrainSceneOverflowsPrefix =
    "When the train stopped, new passengers began boarding. More people";
constexpr std::string_view kTrainSceneExpectedMeiryoLine1 =
    "When the train stopped, new passengers began boarding. More";
constexpr std::string_view kTrainSceneExpectedMeiryoLine2 =
    "people were getting on than getting off.";
constexpr std::string_view kTrainSceneExpectedFullText =
    "When the train stopped, new passengers began boarding. More people were "
    "getting on than getting off.";

// Deterministic mock proportional measurer modeling Meiryo's average glyph
// advance (7 px space, 13 px uppercase/wide, 11 px normal, 5 px narrow
// punctuation) for OS-independent unit verification.
class MockProportionalMeasurer final : public TextWidthMeasurer {
public:
    ~MockProportionalMeasurer() override = default;

    std::size_t MeasureSpanWidth(std::string_view utf8_span) const override {
        std::size_t total = 0;
        for (const char ch : utf8_span) {
            if (ch == ' ') {
                total += kMockSpaceWidthPx;
                continue;
            }
            if (ch == '.' || ch == ',' || ch == '\'' || ch == '!' ||
                ch == ';' || ch == ':' || ch == 'i' || ch == 'l') {
                total += kMockNarrowGlyphWidthPx;
                continue;
            }
            if ((ch >= 'A' && ch <= 'Z') || ch == 'm' || ch == 'w') {
                total += kMockWideGlyphWidthPx;
                continue;
            }
            total += kMockNormalGlyphWidthPx;
        }
        return total;
    }
};

// RAII owner for a memory DC and selected CatSystem2-compatible HFONT used to
// query the typeface name resolved by the Win32 GDI font mapper.
class ScopedGdiFontSelection final {
public:
    ScopedGdiFontSelection(int lf_height,
                           std::wstring_view face_name,
                           long font_weight) noexcept
        : dc_(CreateCompatibleDC(nullptr)) {
        if (dc_ == nullptr) {
            return;
        }

        LOGFONTW lf{};
        lf.lfHeight = lf_height;
        lf.lfWeight = font_weight;
        lf.lfCharSet = SHIFTJIS_CHARSET;
        lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = ANTIALIASED_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

        const std::size_t copy_chars = std::min<std::size_t>(
            face_name.size(), LF_FACESIZE - kNullTerminatorChars);
        if (copy_chars > 0) {
            std::wmemcpy(lf.lfFaceName, face_name.data(), copy_chars);
        }
        lf.lfFaceName[copy_chars] = L'\0';

        font_ = CreateFontIndirectW(&lf);
        if (font_ == nullptr) {
            return;
        }
        prev_obj_ = SelectObject(dc_, font_);
    }

    ScopedGdiFontSelection(const ScopedGdiFontSelection&) = delete;
    ScopedGdiFontSelection& operator=(const ScopedGdiFontSelection&) = delete;
    ScopedGdiFontSelection(ScopedGdiFontSelection&&) = delete;
    ScopedGdiFontSelection& operator=(ScopedGdiFontSelection&&) = delete;

    ~ScopedGdiFontSelection() noexcept {
        if (dc_ != nullptr && prev_obj_ != nullptr &&
            prev_obj_ != HGDI_ERROR) {
            SelectObject(dc_, prev_obj_);
        }
        if (font_ != nullptr) {
            DeleteObject(font_);
        }
        if (dc_ != nullptr) {
            DeleteDC(dc_);
        }
    }

    bool IsValid() const noexcept {
        return dc_ != nullptr && font_ != nullptr &&
               prev_obj_ != nullptr && prev_obj_ != HGDI_ERROR;
    }

    std::wstring ResolvedFaceName() const {
        if (!IsValid()) {
            return {};
        }
        std::array<wchar_t, LF_FACESIZE> resolved_face{};
        const int face_chars = GetTextFaceW(
            dc_, static_cast<int>(resolved_face.size()),
            resolved_face.data());
        if (face_chars <= 0) {
            return {};
        }
        resolved_face.back() = L'\0';
        return std::wstring(resolved_face.data());
    }

private:
    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ prev_obj_ = nullptr;
};

// Splits CatSystem2 concatenated textbox text on 2-byte `\n`/`\N` (and literal
// '\n') into individual rendered lines while keeping escaped backslashes (`\\`)
// paired so `\\n` is treated as literal `\` + `n` rather than a line break.
std::vector<std::string> SplitCatSystem2Lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            lines.push_back(current);
            current.clear();
            i += kSingleCharStep;
            continue;
        }
        if (text[i] == '\\' && i + kSingleCharStep < text.size()) {
            const char next = text[i + kSingleCharStep];
            if (next == 'n' || next == 'N') {
                lines.push_back(current);
                current.clear();
                i += kEscapePairLength;
                continue;
            }
            current.push_back(text[i]);
            current.push_back(next);
            i += kEscapePairLength;
            continue;
        }
        current.push_back(text[i]);
        i += kSingleCharStep;
    }
    lines.push_back(current);
    return lines;
}

const char* TranslateThroughSlotPool(std::string_view src,
                                     std::size_t max_wchars,
                                     DialogueTranslationSession& session,
                                     TranslationSlotRingBuffer& slots) {
    const TranslationResult result = session.Translate(src, max_wchars);
    if (!result.translated) {
        return src.data();
    }
    if (result.deduplicated) {
        const char* const cached =
            slots.FindReusableDialogueSlot(result.text.size());
        if (cached != nullptr) {
            return cached;
        }
    }
    return slots.Store(result.text, !result.is_speaker_name);
}

std::size_t MeasureVisibleCodePoints(std::string_view tagged_line) {
    const CodePointWidthMeasurer cp_measurer(kSingleUnitPerCodePoint);
    std::string copy(tagged_line);
    const std::size_t width =
        WordWrapByWidth(copy, kNoWrapCodePointLimit, 0, cp_measurer);
    EXPECT_EQ(copy, tagged_line);
    return width;
}

// Returns true iff Win32 GDI resolves CatSystem2's default `メイリオ` LOGFONTW
// directly to Meiryo rather than substituting a fallback system font on Windows
// installations without optional Japanese supplemental fonts.
bool IsMeiryoGdiFontInstalled() {
    const ScopedGdiFontSelection selection(kDefaultScaledDialogueLfHeight,
                                           kMeiryoJapaneseFaceName,
                                           kDefaultFontWeight);
    const std::wstring resolved = selection.ResolvedFaceName();
    return resolved == kMeiryoJapaneseFaceName ||
           resolved == kMeiryoEnglishFaceName;
}

std::string_view FirstSpaceDelimitedToken(std::string_view text) noexcept {
    const std::size_t space_pos = text.find(' ');
    if (space_pos == std::string_view::npos) {
        return text;
    }
    return text.substr(0, space_pos);
}

void RunTrainSceneTwoLineGdiChecks(const GdiTextWidthMeasurer& measurer,
                                   std::vector<std::string>& out_lines) {
    ASSERT_TRUE(measurer.IsValid());

    TranslationMap map{
        {std::string(kTrainSceneJpSeg1), std::string(kTrainSceneEnSeg1)},
        {std::string(kTrainSceneJpSeg2), std::string(kTrainSceneEnSeg2)},
    };

    DialogueTranslationSession session(&map, &measurer);
    const TranslationResult r1 =
        session.Translate(kTrainSceneJpSeg1, kDialogueMaxWchars);
    const TranslationResult r2 =
        session.Translate(kTrainSceneJpSeg2, kDialogueMaxWchars);

    ASSERT_TRUE(r1.translated);
    ASSERT_TRUE(r2.translated);
    EXPECT_FALSE(r1.is_continuation);
    EXPECT_TRUE(r2.is_continuation);

    out_lines = SplitCatSystem2Lines(r1.text + r2.text);
    ASSERT_EQ(out_lines.size(), kExpectedTwoLines);
    ASSERT_FALSE(out_lines[kFirstLineIndex].empty());
    ASSERT_FALSE(out_lines[kSecondLineIndex].empty());
    EXPECT_NE(out_lines[kFirstLineIndex].front(), ' ');
    EXPECT_NE(out_lines[kFirstLineIndex].back(), ' ');
    EXPECT_NE(out_lines[kSecondLineIndex].front(), ' ');
    EXPECT_NE(out_lines[kSecondLineIndex].back(), ' ');

    for (const std::string& line : out_lines) {
        const std::size_t px = measurer.MeasureSpanWidth(line);
        EXPECT_LE(px, kDefaultMaxDialogueLineWidthPx) << line;
        EXPECT_LE(px, kEngineTextboxPixelWidth) << line;
    }

    const std::string_view next_word =
        FirstSpaceDelimitedToken(out_lines[kSecondLineIndex]);
    ASSERT_FALSE(next_word.empty());
    EXPECT_GT(
        measurer.MeasureSpanWidth(
            out_lines[kFirstLineIndex] + " " + std::string(next_word)),
        kDefaultMaxDialogueLineWidthPx);
    EXPECT_EQ(
        out_lines[kFirstLineIndex] + " " + out_lines[kSecondLineIndex],
        kTrainSceneExpectedFullText);
}

}  // namespace

TEST(DialogueSession, SplitCatSystem2LinesPreservesLiteralBackslashBeforeN) {
    const std::vector<std::string> lines =
        SplitCatSystem2Lines("foo\\\\nbar\\nbaz\\Nqux");
    ASSERT_EQ(lines.size(), kExpectedThreeLines);
    EXPECT_EQ(lines[0], "foo\\\\nbar");
    EXPECT_EQ(lines[kSingleCharStep], "baz");
    EXPECT_EQ(lines[kEscapePairLength], "qux");
}

TEST(DialogueSession, TrainSceneMockProportionalNeverSplitsWordsAcrossLines) {
    TranslationMap map{
        {"jp_train_1",
         "When the train stopped, new passengers began boarding."},
        {"\\njp_train_2",
         "\\nMore people were getting on than getting off."},
    };
    const MockProportionalMeasurer measurer;
    DialogueTranslationSession session(&map, &measurer);

    const TranslationResult r1 =
        session.Translate("jp_train_1", kDialogueMaxWchars);
    const TranslationResult r2 =
        session.Translate("\\njp_train_2", kDialogueMaxWchars);

    ASSERT_TRUE(r1.translated);
    ASSERT_TRUE(r2.translated);
    EXPECT_FALSE(r1.is_continuation);
    EXPECT_TRUE(r2.is_continuation);

    const std::string combined = r1.text + r2.text;
    const std::vector<std::string> lines = SplitCatSystem2Lines(combined);

    EXPECT_EQ(lines.size(), kExpectedTwoLines);
    for (const std::string& line : lines) {
        EXPECT_LE(measurer.MeasureSpanWidth(line), kDefaultMaxDialogueLineWidthPx)
            << "Line overflowed wrap width: " << line;
    }
    EXPECT_EQ(combined.find("peopl\\n"), std::string::npos);
    EXPECT_EQ(combined.find("\\ne "), std::string::npos);
    EXPECT_NE(combined.find("people"), std::string::npos);
}

TEST(DialogueSession, TrainSceneRealGdiMeiryoProducesTwoCleanLines) {
    const GdiTextWidthMeasurer gdi_measurer(kDefaultScaledDialogueLfHeight);
    std::vector<std::string> lines;
    ASSERT_NO_FATAL_FAILURE(RunTrainSceneTwoLineGdiChecks(gdi_measurer, lines));

    if (IsMeiryoGdiFontInstalled()) {
        EXPECT_LE(gdi_measurer.MeasureSpanWidth(kTrainSceneFitsPrefix),
                  kEngineTextboxPixelWidth);
        EXPECT_GT(gdi_measurer.MeasureSpanWidth(kTrainSceneOverflowsPrefix),
                  kEngineTextboxPixelWidth);
        EXPECT_EQ(lines[kFirstLineIndex], kTrainSceneExpectedMeiryoLine1);
        EXPECT_EQ(lines[kSecondLineIndex], kTrainSceneExpectedMeiryoLine2);
    }

    const GdiTextWidthMeasurer arial_measurer(
        kDefaultScaledDialogueLfHeight, kArialFaceName);
    std::vector<std::string> arial_lines;
    ASSERT_NO_FATAL_FAILURE(
        RunTrainSceneTwoLineGdiChecks(arial_measurer, arial_lines));
}

TEST(DialogueSession, TwoSegmentNarrationFitsInThreeLinesWithEndingIntact) {
    TranslationMap map{
        {"jp_41",
         "The girl hesitated for a moment, but perhaps thinking that going "
         "back and forth"},
        {"\\njp_42",
         "\\nwould bother the people around us, she let me handle it without "
         "any more fuss."},
    };

    const GdiTextWidthMeasurer gdi_measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(gdi_measurer.IsValid());

    DialogueTranslationSession session(&map, &gdi_measurer);
    const TranslationResult r1 = session.Translate("jp_41", kDialogueMaxWchars);
    const TranslationResult r2 =
        session.Translate("\\njp_42", kDialogueMaxWchars);

    ASSERT_TRUE(r1.translated);
    ASSERT_TRUE(r2.translated);
    EXPECT_FALSE(r1.truncated);
    EXPECT_FALSE(r2.truncated);

    const std::string combined = r1.text + r2.text;
    const std::vector<std::string> lines = SplitCatSystem2Lines(combined);

    ASSERT_LE(lines.size(), kMaxMeswndVisibleLines)
        << "Must fit within the 3-line meswnd vertical viewport: " << combined;
    ASSERT_FALSE(lines.empty());

    for (const std::string& line : lines) {
        const std::size_t px = gdi_measurer.MeasureSpanWidth(line);
        EXPECT_LE(px, kDefaultMaxDialogueLineWidthPx) << line;
        EXPECT_LE(px, kEngineTextboxPixelWidth) << line;
    }

    constexpr std::string_view kExpectedEnding = "any more fuss.";
    const std::string& final_line = lines.back();
    ASSERT_GE(final_line.size(), kExpectedEnding.size());
    EXPECT_EQ(std::string_view(final_line).substr(
                  final_line.size() - kExpectedEnding.size()),
              kExpectedEnding);
}

TEST(DialogueSession, AllFiveMeswndFontPresetsAndBacklogFitWithinTextboxWidth) {
    TranslationMap map{
        {"jp_1", "When the train stopped, new passengers began boarding."},
        {"\\njp_2", "\\nMore people were getting on than getting off."},
    };

    GdiTextWidthMeasurer meswnd_measurer(kDefaultScaledDialogueLfHeight);
    GdiTextWidthMeasurer backlog_measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(meswnd_measurer.IsValid());
    ASSERT_TRUE(backlog_measurer.IsValid());

    constexpr std::array<long, 2> kConfigWeights = {
        kDefaultFontWeight,
        kBoldFontWeight,
    };

    for (const long weight : kConfigWeights) {
        for (std::size_t step = 0; step < kFontSizePresetCount; ++step) {
            const int scaled_mes =
                ScaleEngineFontHeight(-kMeswndDialogueLayoutSizes[step]);
            const int scaled_log =
                ScaleEngineFontHeight(-kBacklogDialogueLayoutSizes[step]);
            ASSERT_TRUE(
                meswnd_measurer.UpdateDialogueFont(scaled_mes, {}, weight));
            ASSERT_TRUE(
                backlog_measurer.UpdateDialogueFont(scaled_log, {}, weight));
            EXPECT_EQ(meswnd_measurer.ActiveFontWeight(), weight);
            EXPECT_EQ(backlog_measurer.ActiveFontWeight(), weight);

            DialogueTranslationSession session(&map, &meswnd_measurer);
            const TranslationResult r1 =
                session.Translate("jp_1", kDialogueMaxWchars);
            const TranslationResult r2 =
                session.Translate("\\njp_2", kDialogueMaxWchars);

            const std::vector<std::string> lines =
                SplitCatSystem2Lines(r1.text + r2.text);
            ASSERT_GE(lines.size(), kExpectedTwoLines);
            for (const std::string& line : lines) {
                const std::size_t mes_px = meswnd_measurer.MeasureSpanWidth(line);
                const std::size_t log_px = backlog_measurer.MeasureSpanWidth(line);
                EXPECT_LE(mes_px, kDefaultMaxDialogueLineWidthPx)
                    << "weight=" << weight << " step=" << step << " line=" << line;
                EXPECT_LE(mes_px, kEngineTextboxPixelWidth)
                    << "weight=" << weight << " step=" << step << " line=" << line;
                EXPECT_LE(log_px, kEngineTextboxPixelWidth)
                    << "backlog overflow at weight=" << weight << " step=" << step
                    << " line=" << line;
            }
        }
    }
}

TEST(DialogueSession, InlineKeywaitCarriesWidthAndWrapsCleanlyOnlyOnOverflow) {
    EXPECT_TRUE(EndsWithInlineWaitTag("\"Okay--\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("\"Okay--\\\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("plain text"));

    TranslationMap map{
        {"jp_bag_1", "\"Okay. Let me just put my bag up top--\\@"},
        {"jp_bag_2", "Ah!\""},
        {"jp_sent_1", "\"In that case, I'll gladly accept it.\\@"},
        {"jp_sent_2", "When we meet again.\""},
    };

    const GdiTextWidthMeasurer gdi_measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(gdi_measurer.IsValid());

    {
        DialogueTranslationSession session(&map, &gdi_measurer);
        const TranslationResult p1 =
            session.Translate("jp_bag_1", kDialogueMaxWchars);
        ASSERT_TRUE(p1.translated);
        EXPECT_TRUE(session.PrevEndedWithInlineWait());
        EXPECT_EQ(p1.end_width,
                  gdi_measurer.MeasureSpanWidth(
                      "\"Okay. Let me just put my bag up top--"));

        const TranslationResult p2 =
            session.Translate("jp_bag_2", kDialogueMaxWchars);
        ASSERT_TRUE(p2.translated);
        EXPECT_TRUE(p2.is_continuation);
        EXPECT_FALSE(session.PrevEndedWithInlineWait());
        EXPECT_EQ(p1.text + p2.text,
                  "\"Okay. Let me just put my bag up top--\\@Ah!\"");
    }

    {
        const std::size_t part1_px = gdi_measurer.MeasureSpanWidth(
            "\"Okay. Let me just put my bag up top--");
        const std::size_t part2_px = gdi_measurer.MeasureSpanWidth("Ah!\"");
        DialogueSessionConfig tight_cfg{};
        tight_cfg.max_line_width = part1_px + (part2_px / kHalfWidthDivisor);

        DialogueTranslationSession session(&map, &gdi_measurer, tight_cfg);
        const TranslationResult p1 =
            session.Translate("jp_bag_1", kDialogueMaxWchars);
        const TranslationResult p2 =
            session.Translate("jp_bag_2", kDialogueMaxWchars);

        EXPECT_EQ(p1.text, "\"Okay. Let me just put my bag up top--\\@");
        EXPECT_EQ(p2.text, "\\nAh!\"");
    }

    {
        DialogueTranslationSession session(&map, &gdi_measurer);
        const TranslationResult s1 =
            session.Translate("jp_sent_1", kDialogueMaxWchars);
        const TranslationResult s2 =
            session.Translate("jp_sent_2", kDialogueMaxWchars);
        EXPECT_EQ(s1.text + s2.text,
                  "\"In that case, I'll gladly accept it.\\@ When we meet again.\"");

        std::ignore = session.Translate("jp_bag_1", kDialogueMaxWchars);
        ASSERT_TRUE(session.PrevEndedWithInlineWait());
        const TranslationResult standalone_nl =
            session.Translate("\\n", kDialogueMaxWchars);
        EXPECT_FALSE(standalone_nl.translated);
        EXPECT_FALSE(session.PrevEndedWithInlineWait());
        EXPECT_EQ(session.CurrentLineWidth(), 0u);
    }
}

TEST(DialogueSession, ZeroWidthAndCaseInsensitiveCatSystem2ControlTagsMatchEngine) {
    const MonospaceByteWidthMeasurer byte_measurer;
    std::string tagged = "\\pc\\f+20;'Congratulations!'\\f-20;\\@";
    const std::size_t width =
        WordWrapByWidth(tagged, kTightByteWrapLimit, 0, byte_measurer);
    EXPECT_EQ(width, std::string_view("'Congratulations!'").size());
    EXPECT_EQ(tagged, "\\pc\\f+20;'Congratulations!'\\f-20;\\@");

    std::string wait_and_quote = "\\w0;\"\\\"Breathe in\\\"\"\\@";
    const std::size_t w2 =
        WordWrapForEngine(wait_and_quote, kWideByteWrapLimit, 0);
    EXPECT_EQ(w2, std::string_view("\"\"Breathe in\"\"").size());
    EXPECT_EQ(wait_and_quote, "\\w0;\"\\\"Breathe in\\\"\"\\@");

    // Uppercase tags (`\PC`, `\F+20;`, `\W0;`, `\CR0;`, `\ID12:34`, `\X8;`,
    // `\Y4;`, `\R`) are consumed identically to lowercase tags.
    std::string upper_tagged =
        "\\PC\\F+20;\\W0;\\CR0;\\ID12:34\\X8;\\Y4;\\RHello\\F-20;\\@";
    const std::size_t upper_width =
        WordWrapByWidth(upper_tagged, kTightByteWrapLimit, 0, byte_measurer);
    EXPECT_EQ(upper_width, std::string_view("Hello").size());
    EXPECT_EQ(upper_tagged,
              "\\PC\\F+20;\\W0;\\CR0;\\ID12:34\\X8;\\Y4;\\RHello\\F-20;\\@");

    // Uppercase `\N` resets line width and StripLeadingControlNewline handles it.
    std::string upper_newline = "Alpha\\NBeta";
    const std::size_t after_upper_nl =
        WordWrapByWidth(upper_newline, kWideByteWrapLimit, 0, byte_measurer);
    EXPECT_EQ(after_upper_nl, std::string_view("Beta").size());
    EXPECT_EQ(upper_newline, "Alpha\\NBeta");

    std::string leading_upper_nl = "\\NContinuation";
    EXPECT_TRUE(StripLeadingControlNewline(leading_upper_nl));
    EXPECT_EQ(leading_upper_nl, " Continuation");

    // `\s` and `\S` emit fullwidth space U+3000 (`\xE3\x80\x80`) in CatSystem2:
    // measured at U+3000's pixel width while keeping the raw `\s`/`\S` tag intact.
    const GdiTextWidthMeasurer gdi_measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(gdi_measurer.IsValid());
    const std::size_t fw_space_px =
        gdi_measurer.MeasureSpanWidth("\xE3\x80\x80");
    ASSERT_GT(fw_space_px, 0u);

    std::string lower_fw = "\\sWord";
    std::string upper_fw = "\\SWord";
    const std::size_t expected_fw_word_px =
        gdi_measurer.MeasureSpanWidth("\xE3\x80\x80Word");
    EXPECT_EQ(WordWrapByWidth(
                  lower_fw, kDefaultMaxDialogueLineWidthPx, 0, gdi_measurer),
              expected_fw_word_px);
    EXPECT_EQ(WordWrapByWidth(
                  upper_fw, kDefaultMaxDialogueLineWidthPx, 0, gdi_measurer),
              expected_fw_word_px);
    EXPECT_EQ(lower_fw, "\\sWord");
    EXPECT_EQ(upper_fw, "\\SWord");
}

TEST(DialogueSession, ConsecutiveDuplicateDialogueCallsAdvanceWidthOnlyOnce) {
    constexpr std::string_view kFirstHalf = "First half of the sentence";
    TranslationMap map{
        {"jp_a", std::string(kFirstHalf)},
        {"\\njp_b", "\\nsecond half right after."},
    };
    const CodePointWidthMeasurer measurer(kSingleUnitPerCodePoint);
    DialogueSessionConfig cfg{};
    cfg.max_line_width = kDeduplicatedWrapLimit;
    DialogueTranslationSession session(&map, &measurer, cfg);

    const TranslationResult pass1 =
        session.Translate("jp_a", kDialogueMaxWchars);
    const TranslationResult pass2 =
        session.Translate("jp_a", kString8SetTextMaxWchars);

    ASSERT_TRUE(pass1.translated);
    ASSERT_TRUE(pass2.translated);
    EXPECT_FALSE(pass1.deduplicated);
    EXPECT_TRUE(pass2.deduplicated);
    EXPECT_EQ(pass1.text, pass2.text);
    EXPECT_EQ(session.CurrentLineWidth(), kFirstHalf.size());

    const TranslationResult cont1 =
        session.Translate("\\njp_b", kDialogueMaxWchars);
    const TranslationResult cont2 =
        session.Translate("\\njp_b", kDialogueMaxWchars);
    EXPECT_EQ(cont1.text, " second half\\nright after.");
    EXPECT_EQ(cont2.text, cont1.text);
    EXPECT_TRUE(cont2.deduplicated);
}

TEST(DialogueSession, SpeakerNameVmAndApendCallsPreserveDialogueState) {
    ASSERT_GT(DefaultSpeakerNameKeys().count("\xE5\xB0\x91\xE5\xA5\xB3"), 0u);

    TranslationMap map{
        {"\xE5\xB0\x91\xE5\xA5\xB3", "Girl"},
        {"\xE8\xA1\x80", "Blood"},
        {"jp_1", "\"Oh, you don't have to--\\@"},
        {"jp_2", "Yes, that's fine.\""},
    };
    const GdiTextWidthMeasurer gdi_measurer(kDefaultScaledDialogueLfHeight);
    DialogueTranslationSession session(&map, &gdi_measurer);

    const TranslationResult p1 = session.Translate("jp_1", kDialogueMaxWchars);
    ASSERT_TRUE(p1.translated);
    const std::size_t saved_width = session.CurrentLineWidth();
    ASSERT_GT(saved_width, 0u);
    ASSERT_TRUE(session.PrevEndedWithInlineWait());

    // Single-character glyph decoder (max_wchars == 16) passes through untouched.
    const TranslationResult single_char =
        session.Translate("\xE8\xA1\x80", kSingleCharDecoderMaxWchars);
    EXPECT_FALSE(single_char.translated);
    EXPECT_EQ(session.CurrentLineWidth(), saved_width);
    EXPECT_TRUE(session.PrevEndedWithInlineWait());

    // CatScene VM NAME buffer (max_wchars == 4096) followed by STRING8::apend
    // (max_wchars == 2048) returns unwrapped "Girl" without touching dialogue state.
    const TranslationResult name_vm =
        session.Translate("\xE5\xB0\x91\xE5\xA5\xB3", kDialogueMaxWchars);
    const TranslationResult name_apend =
        session.Translate("\xE5\xB0\x91\xE5\xA5\xB3", kString8SetTextMaxWchars);

    EXPECT_TRUE(name_vm.translated);
    EXPECT_TRUE(name_vm.is_speaker_name);
    EXPECT_EQ(name_vm.text, "Girl");

    EXPECT_TRUE(name_apend.translated);
    EXPECT_TRUE(name_apend.is_speaker_name);
    EXPECT_EQ(name_apend.text, "Girl");

    EXPECT_EQ(session.CurrentLineWidth(), saved_width);
    EXPECT_TRUE(session.PrevEndedWithInlineWait());
    EXPECT_TRUE(session.Translate("jp_1", kDialogueMaxWchars).deduplicated);

    const TranslationResult p2 = session.Translate("jp_2", kDialogueMaxWchars);
    EXPECT_TRUE(p2.is_continuation);
    EXPECT_EQ(p1.text + p2.text,
              "\"Oh, you don't have to--\\@Yes, that's fine.\"");
}

TEST(DialogueSession, CustomSpeakerKeySetAndWideBoundControlClassification) {
    TranslationMap map{
        {"custom_speaker",
         "Very Long Custom Speaker Name Exceeding Narrow Wrap Budget"},
    };
    const MonospaceByteWidthMeasurer byte_measurer;
    DialogueSessionConfig cfg{};
    cfg.max_line_width = kTightByteWrapLimit;
    DialogueTranslationSession session(&map, &byte_measurer, cfg);
    session.SetSpeakerNameKeys(SpeakerKeySet{"custom_speaker"});

    const TranslationResult as_speaker =
        session.Translate("custom_speaker", kDialogueMaxWchars);
    EXPECT_TRUE(as_speaker.translated);
    EXPECT_TRUE(as_speaker.is_speaker_name);
    EXPECT_EQ(as_speaker.text.find("\\n"), std::string::npos);
    EXPECT_EQ(session.CurrentLineWidth(), 0u);

    // Calls exceeding kMaxSpeakerWideChars fall back to normal dialogue wrapping.
    const TranslationResult over_bound =
        session.Translate("custom_speaker", kOverMaxSpeakerWchars);
    EXPECT_TRUE(over_bound.translated);
    EXPECT_FALSE(over_bound.is_speaker_name);
    EXPECT_NE(over_bound.text.find("\\n"), std::string::npos);
}

TEST(DialogueSession, BacklogSpeakerNamesDuringInlineWaitPreserveContinuation) {
    TranslationMap map{
        {"jp_wait_1", "Wait--\\@"},
        {"jp_wait_2", "\"Don't go!\""},
        {"\xE5\xB0\x91\xE5\xA5\xB3", "Girl"},
        {"\xE3\x81\xA1\xE3\x81\xA8\xE3\x81\x9B", "Chitose"},
        {"\xE7\xB5\x90\xE7\x81\xAF", "Yuuhi"},
        {"\xE5\xA5\xB3\xE5\xAD\x90\xE4\xB8\x8B\xE7\xB4\x9A\xE7\x94\x9F\xEF\xBC\xA1",
         "Female Underclassman A"},
    };

    const GdiTextWidthMeasurer gdi_measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(gdi_measurer.IsValid());
    DialogueTranslationSession session(&map, &gdi_measurer);

    // Initial dialogue segment paused at `\@` (`meswnd` VM 4096 + `apend` 2048).
    const TranslationResult w1_vm =
        session.Translate("jp_wait_1", kDialogueMaxWchars);
    const TranslationResult w1_apend =
        session.Translate("jp_wait_1", kString8SetTextMaxWchars);
    ASSERT_TRUE(w1_vm.translated);
    ASSERT_TRUE(w1_apend.deduplicated);
    EXPECT_EQ(w1_vm.text, "Wait--\\@");

    const std::size_t width_at_pause = session.CurrentLineWidth();
    ASSERT_GT(width_at_pause, 0u);
    ASSERT_TRUE(session.PrevEndedWithInlineWait());

    // Opening `log.fes` invokes `name[0..3] apend` (`max_wchars = 2048`) for
    // 4 historical speaker names while `meswnd` remains paused at `\@`.
    struct SpeakerExpectation {
        std::string_view jp;
        std::string_view en;
    };
    constexpr std::array<SpeakerExpectation, 4> kBacklogSpeakers = {{
        {"\xE5\xB0\x91\xE5\xA5\xB3", "Girl"},
        {"\xE3\x81\xA1\xE3\x81\xA8\xE3\x81\x9B", "Chitose"},
        {"\xE7\xB5\x90\xE7\x81\xAF", "Yuuhi"},
        {"\xE5\xA5\xB3\xE5\xAD\x90\xE4\xB8\x8B\xE7\xB4\x9A\xE7\x94\x9F\xEF\xBC\xA1",
         "Female Underclassman A"},
    }};

    for (const SpeakerExpectation& sp : kBacklogSpeakers) {
        const TranslationResult res =
            session.Translate(sp.jp, kString8SetTextMaxWchars);
        ASSERT_TRUE(res.translated);
        EXPECT_TRUE(res.is_speaker_name);
        EXPECT_FALSE(res.is_continuation);
        EXPECT_EQ(res.text, sp.en);
        EXPECT_EQ(res.text.find("\\n"), std::string::npos);
        EXPECT_NE(res.text.front(), ' ');
    }

    // Inline-wait state and width carry must survive intact for the next segment.
    EXPECT_TRUE(session.PrevEndedWithInlineWait());
    EXPECT_EQ(session.CurrentLineWidth(), width_at_pause);

    const TranslationResult w2_vm =
        session.Translate("jp_wait_2", kDialogueMaxWchars);
    const TranslationResult w2_apend =
        session.Translate("jp_wait_2", kString8SetTextMaxWchars);
    ASSERT_TRUE(w2_vm.translated);
    EXPECT_TRUE(w2_vm.is_continuation);
    EXPECT_TRUE(w2_apend.deduplicated);
    EXPECT_EQ(w2_vm.start_width, width_at_pause);
    EXPECT_EQ(w1_vm.text + w2_vm.text, "Wait--\\@\"Don't go!\"");
    EXPECT_EQ(w2_apend.text, w2_vm.text);
}

TEST(SlotRingBuffer, WraparoundSpeakerAllocationsNeverClobberDeduplicatedDialogue) {
    // Deliberately choose a 4-byte dialogue translation ("Wait") and a 4-byte
    // speaker translation ("Girl") so equal byte lengths cannot mask a stale
    // `last_dialogue_slot` pointer after 64+ speaker allocations wrap around.
    TranslationMap map{
        {"jp_short", "Wait"},
        {"\xE5\xB0\x91\xE5\xA5\xB3", "Girl"},
    };
    const MonospaceByteWidthMeasurer byte_measurer;
    DialogueTranslationSession session(&map, &byte_measurer);
    TranslationSlotRingBuffer slots;

    const char* const initial_slot =
        TranslateThroughSlotPool("jp_short", kDialogueMaxWchars, session, slots);
    ASSERT_STREQ(initial_slot, "Wait");

    const char* const immediate_dedup = TranslateThroughSlotPool(
        "jp_short", kString8SetTextMaxWchars, session, slots);
    EXPECT_EQ(immediate_dedup, initial_slot);
    EXPECT_STREQ(immediate_dedup, "Wait");

    // Allocate 68 speaker-name slots (> 64 pool size), wrapping completely
    // past `initial_slot` and overwriting its buffer with "Girl".
    const std::size_t total_speaker_calls =
        kDefaultSlotPoolCount + kExtraWraparoundSpeakers;
    for (std::size_t i = 0; i < total_speaker_calls; ++i) {
        const char* const sp_slot = TranslateThroughSlotPool(
            "\xE5\xB0\x91\xE5\xA5\xB3", kString8SetTextMaxWchars, session, slots);
        ASSERT_STREQ(sp_slot, "Girl");
    }

    // Subsequent deduplicated redraw of `jp_short` must NOT return the
    // overwritten "Girl" slot; it must allocate a fresh slot containing "Wait".
    const char* const post_wrap_redraw = TranslateThroughSlotPool(
        "jp_short", kString8SetTextMaxWchars, session, slots);
    EXPECT_STREQ(post_wrap_redraw, "Wait");

    // Reset() must also invalidate any cached dialogue slot.
    ASSERT_NE(slots.FindReusableDialogueSlot(std::string_view("Wait").size()),
              nullptr);
    slots.Reset();
    EXPECT_EQ(slots.FindReusableDialogueSlot(std::string_view("Wait").size()),
              nullptr);
}

TEST(Utf8Truncate, StopsBeforeNullAndRejectsMalformedUtf8Sequences) {
    // Trailing incomplete lead byte `\xE3` immediately before `'\0'`, followed
    // after NUL by valid continuation bytes `\x81\x82` to detect any over-read.
    const std::array<char, 8> truncated_before_nul = {
        'a', 'b', 'c', '\xE3', '\0', '\x81', '\x82', '\0',
    };
    EXPECT_EQ(Utf8SafeTruncate(truncated_before_nul.data(),
                               kSafeTruncateTestLimit),
              kExpectedAsciiPrefixLen);

    const std::array<char, 4> lone_lead_before_nul = {
        '\xE3', '\0', '\x81', '\0',
    };
    EXPECT_EQ(Utf8SafeTruncate(lone_lead_before_nul.data(),
                               kSafeTruncateTestLimit),
              0u);

    // Cutting mid-sequence on valid UTF-8 trims the incomplete scalar cleanly.
    EXPECT_EQ(Utf8SafeTruncate("a\xE3\x81\x82", kCutInsideThreeByteUtf8),
              kExpectedSingleAsciiByte);

    // Reject overlong encodings, UTF-16 surrogates (U+D800), and > U+10FFFF.
    EXPECT_EQ(Utf8SafeTruncate("\xC0\xAF", kSafeTruncateTestLimit), 0u);
    EXPECT_EQ(Utf8SafeTruncate("\xE0\x80\xAF", kSafeTruncateTestLimit), 0u);
    EXPECT_EQ(Utf8SafeTruncate("\xED\xA0\x80", kSafeTruncateTestLimit), 0u);
    EXPECT_EQ(Utf8SafeTruncate("\xF0\x80\x80\x80", kSafeTruncateTestLimit), 0u);
    EXPECT_EQ(Utf8SafeTruncate("\xF4\x90\x80\x80", kSafeTruncateTestLimit), 0u);
}

TEST(Utf8Truncate, WideLimitCountsIllFormedBytesAsReplacementWideUnits) {
    // Under CP_UTF8, `\xF0` followed by ASCII "ABC" produces U+FFFD + 'A' +
    // 'B' + 'C' = 4 wchar_t units. With max_wchars = 4 (at most 3 non-NUL
    // wchar_t units), truncation must keep at most 3 bytes ("\xF0AB").
    const std::string_view malformed_lead("\xF0" "ABC", kMaxFourWideUnits);
    const std::size_t keep = Utf8SafeTruncateForWideLimit(
        malformed_lead, kDefaultMaxOutputBytes, kMaxFourWideUnits);
    EXPECT_EQ(keep, kExpectedThreeWideBytes);
    EXPECT_EQ(malformed_lead.substr(0, keep), "\xF0" "AB");

    // 3-byte UTF-16 surrogate `\xED\xA0\x80` + 'Z' = 3 ill-formed bytes + 1 ASCII
    // = 4 wchar_t units; max_wchars = 4 keeps at most 3 bytes.
    const std::string_view surrogate_and_ascii("\xED\xA0\x80Z", kMaxFourWideUnits);
    const std::size_t keep_surrogate = Utf8SafeTruncateForWideLimit(
        surrogate_and_ascii, kDefaultMaxOutputBytes, kMaxFourWideUnits);
    EXPECT_EQ(keep_surrogate, kExpectedThreeWideBytes);
}

TEST(GdiMeasurer, RubyAndNameplateFontHeightsNeverClobberDialogueFont) {
    GdiTextWidthMeasurer measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(measurer.IsValid());
    EXPECT_EQ(measurer.ActiveFontHeight(), kDefaultScaledDialogueLfHeight);

    const int scaled_ruby = ScaleEngineFontHeight(-kRubyLayoutSize);
    EXPECT_EQ(scaled_ruby, kExpectedScaledRubyLfHeight);
    EXPECT_FALSE(IsMeswndDialogueLayoutSize(-kRubyLayoutSize));
    EXPECT_FALSE(measurer.SetDialogueFontHeight(scaled_ruby));
    EXPECT_FALSE(measurer.SetDialogueFontHeight(-scaled_ruby));
    EXPECT_EQ(measurer.ActiveFontHeight(), kDefaultScaledDialogueLfHeight);

    EXPECT_FALSE(IsMeswndDialogueLayoutSize(kSpeakerNameplateLayoutSize));
    EXPECT_FALSE(IsMeswndDialogueLayoutSize(-kSpeakerNameplateLayoutSize));

    for (const int raw : kMeswndDialogueLayoutSizes) {
        EXPECT_TRUE(IsMeswndDialogueLayoutSize(raw));
        EXPECT_TRUE(IsMeswndDialogueLayoutSize(-raw));
    }

    EXPECT_TRUE(measurer.SetDialogueFontHeight(
        ScaleEngineFontHeight(-kMeswndPresetSizeFl)));
    EXPECT_EQ(measurer.ActiveFontHeight(), kExpectedScaledPresetFlLfHeight);
}

TEST(GdiMeasurer, ConcurrentMeasureAndFontUpdatesAreThreadSafe) {
    GdiTextWidthMeasurer measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(measurer.IsValid());

    std::vector<std::thread> threads;
    threads.reserve(kConcurrentReaderThreads + kSingleCharStep);

    for (std::size_t t = 0; t < kConcurrentReaderThreads; ++t) {
        threads.emplace_back([&measurer]() {
            for (std::size_t i = 0; i < kConcurrentIterations; ++i) {
                const std::size_t px = measurer.MeasureSpanWidth(
                    "When the train stopped, new passengers began boarding.");
                EXPECT_GT(px, 0u);
            }
        });
    }

    threads.emplace_back([&measurer]() {
        for (std::size_t i = 0; i < kConcurrentIterations; ++i) {
            const int raw =
                kMeswndDialogueLayoutSizes[i % kFontSizePresetCount];
            EXPECT_TRUE(measurer.SetDialogueFontHeight(
                ScaleEngineFontHeight(-raw)));
        }
    });

    for (std::thread& th : threads) {
        th.join();
    }
}

TEST(DialogueSession, LongParagraphExceeding512BytesIsNotTruncated) {
    const std::string long_en =
        "The morning express glided smoothly past rows of sun-drenched rice "
        "paddies and distant mountain ridges while commuters settled quietly "
        "into their cushioned seats. Even with luggage racks filling overhead "
        "and footsteps echoing softly down the aisle, the gentle hum of the "
        "rails made every passing kilometer feel calm, steady, and unhurried "
        "as summer clouds drifted lazily across the wide blue horizon outside "
        "our window, promising a bright and unforgettable season ahead for "
        "everyone aboard the train that morning as the coastline finally came "
        "into view beyond the hills, shimmering harbors, golden beaches, "
        "curving coastal viaducts, and the cool sparkling summer sea breeze.";
    ASSERT_GT(long_en.size(), kMinLongParagraphBytes);

    TranslationMap map{{"jp_long", long_en}};
    const GdiTextWidthMeasurer gdi_measurer(kDefaultScaledDialogueLfHeight);
    DialogueTranslationSession session(&map, &gdi_measurer);

    const TranslationResult res =
        session.Translate("jp_long", kString8SetTextMaxWchars);
    ASSERT_TRUE(res.translated);
    EXPECT_FALSE(res.truncated);
    EXPECT_GT(res.text.size(), kMinLongParagraphBytes);
    EXPECT_NE(res.text.find("sparkling summer sea breeze."), std::string::npos);

    const std::string utf8_sample = " Caf\xC3\xA9" "\xF0\x9F\x8E\xB5" "End";
    const std::size_t keep_before_surrogate = Utf8SafeTruncateForWideLimit(
        utf8_sample, kDefaultMaxOutputBytes, kCafeOnlyMaxWchars);
    EXPECT_EQ(utf8_sample.substr(0, keep_before_surrogate), " Caf\xC3\xA9");
}

TEST(DialogueSession,
     DanglingTrailingBackslashNeverCorruptsAppendedLineBreakWhileKeepingEscapedPair) {
    EXPECT_TRUE(HasDanglingTrailingBackslash("Our shoulders\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("C:\\\\Games\\\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Plain text"));

    std::string stripped = "Our shoulders\\";
    StripDanglingTrailingBackslash(stripped);
    EXPECT_EQ(stripped, "Our shoulders");

    std::string preserved = "C:\\\\Games\\\\";
    StripDanglingTrailingBackslash(preserved);
    EXPECT_EQ(preserved, "C:\\\\Games\\\\");

    TranslationMap map{
        {"jp_seg1",
         "As we stood side by side in the quiet school hallway, our shoulders\\"},
        {"\\njp_seg2", "\\nare touching gently."},
        {"jp_pair", "C:\\\\Games\\\\"},
        {"jp_cut_pair", "Path\\\\"},
    };
    const MockProportionalMeasurer mock;
    DialogueTranslationSession session(&map, &mock);

    const TranslationResult r1 =
        session.Translate("jp_seg1", kDialogueMaxWchars);
    const TranslationResult r2 =
        session.Translate("\\njp_seg2", kDialogueMaxWchars);
    ASSERT_TRUE(r1.translated);
    ASSERT_TRUE(r2.translated);
    EXPECT_EQ(
        r1.text,
        "As we stood side by side in the quiet school hallway, our shoulders");
    EXPECT_EQ(r2.text, "\\nare touching gently.");
    EXPECT_FALSE(HasDanglingTrailingBackslash(r1.text));

    const std::string appended = r1.text + r2.text;
    EXPECT_EQ(appended.find("\\\\n"), std::string::npos);
    const std::vector<std::string> lines = SplitCatSystem2Lines(appended);
    ASSERT_EQ(lines.size(), kExpectedTwoLines);
    EXPECT_EQ(
        lines[0],
        "As we stood side by side in the quiet school hallway, our shoulders");
    EXPECT_EQ(lines[kSingleCharStep], "are touching gently.");

    const TranslationResult r_pair =
        session.Translate("jp_pair", kDialogueMaxWchars);
    ASSERT_TRUE(r_pair.translated);
    EXPECT_EQ(r_pair.text, "C:\\\\Games\\\\");

    DialogueSessionConfig trunc_cfg{};
    trunc_cfg.max_line_width = kDefaultMaxDialogueWidthPx;
    trunc_cfg.max_output_bytes = kCutMidEscapedPairBytes;
    DialogueTranslationSession trunc_session(&map, &mock, trunc_cfg);
    const TranslationResult r_trunc =
        trunc_session.Translate("jp_cut_pair", kDialogueMaxWchars);
    ASSERT_TRUE(r_trunc.translated);
    EXPECT_TRUE(r_trunc.truncated);
    EXPECT_EQ(r_trunc.text, "Path");
    EXPECT_FALSE(HasDanglingTrailingBackslash(r_trunc.text));
}

TEST(GdiFontScaling,
     BacklogLayoutAndInlineFontOverridesNeverClobberMeswndWrapWidth) {
    EXPECT_TRUE(IsMeswndString8BaseLayout(
        kMeswndDialogueLayoutSizes, -kMeswndPresetSizeNormal));
    EXPECT_TRUE(IsMeswndString8BaseLayout(
        kMeswndDialogueLayoutSizes, kMeswndPresetSizeNormal));

    String8LayoutSizes mutated_meswnd = kMeswndDialogueLayoutSizes;
    mutated_meswnd[kLayoutPresetSlotNormal] = kMeswndPresetSizeFs;
    EXPECT_FALSE(IsMeswndString8BaseLayout(
        mutated_meswnd, -kMeswndPresetSizeFs));
    EXPECT_FALSE(IsMeswndString8BaseLayout(
        mutated_meswnd, -kMeswndPresetSizeNormal));

    EXPECT_FALSE(IsMeswndString8BaseLayout(
        kBacklogDialogueLayoutSizes, -kBacklogPresetSizeNormal));
    EXPECT_FALSE(IsMeswndString8BaseLayout(
        kMeswndDialogueLayoutSizes, -kMeswndPresetSizeFs));
    EXPECT_FALSE(IsMeswndString8BaseLayout(
        kMeswndDialogueLayoutSizes, -kMeswndPresetSizeFl));

    GdiTextWidthMeasurer measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(measurer.IsValid());

    EXPECT_EQ(measurer.SyncFromString8BaseFont(
                  kPrimaryFontContextSlot,
                  &kMeswndDialogueLayoutSizes,
                  -kMeswndPresetSizeNormal,
                  {},
                  kBoldFontWeight),
              kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontHeight(), kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontWeight(), kBoldFontWeight);

    EXPECT_EQ(measurer.SyncFromString8BaseFont(
                  kPrimaryFontContextSlot,
                  &kMeswndDialogueLayoutSizes,
                  -kMeswndPresetSizeNormal,
                  {},
                  kDefaultFontWeight),
              kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontHeight(), kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontWeight(), kDefaultFontWeight);

    EXPECT_EQ(measurer.SyncFromString8BaseFont(
                  kPrimaryFontContextSlot,
                  &kBacklogDialogueLayoutSizes,
                  -kBacklogPresetSizeNormal,
                  {},
                  kBoldFontWeight),
              kExpectedScaledBacklogNormalLfHeight);
    EXPECT_EQ(measurer.ActiveFontHeight(), kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontWeight(), kDefaultFontWeight);

    EXPECT_EQ(measurer.SyncFromString8BaseFont(
                  kPrimaryFontContextSlot,
                  nullptr,
                  -kBacklogPresetSizeNormal,
                  {},
                  kBoldFontWeight),
              kExpectedScaledBacklogNormalLfHeight);
    EXPECT_EQ(measurer.ActiveFontHeight(), kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontWeight(), kDefaultFontWeight);

    EXPECT_EQ(measurer.SyncFromString8BaseFont(
                  kSecondaryFontContextSlot,
                  &kMeswndDialogueLayoutSizes,
                  -kMeswndPresetSizeNormal,
                  {},
                  kBoldFontWeight),
              kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontHeight(), kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontWeight(), kDefaultFontWeight);

    const GdiTextWidthMeasurer backlog_ref(kExpectedScaledBacklogNormalLfHeight);
    const GdiTextWidthMeasurer meswnd_ref(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(backlog_ref.IsValid());
    ASSERT_TRUE(meswnd_ref.IsValid());

    std::string boundary_line = "Warm sunlight streamed";
    for (std::size_t i = 0; i < kMaxCandidateWordAppends; ++i) {
        const std::size_t w_backlog = backlog_ref.MeasureSpanWidth(boundary_line);
        const std::size_t w_meswnd = meswnd_ref.MeasureSpanWidth(boundary_line);
        if (w_backlog <= kDefaultMaxDialogueLineWidthPx &&
            w_meswnd > kEngineTextboxPixelWidth) {
            break;
        }
        boundary_line += " by";
    }
    ASSERT_LE(backlog_ref.MeasureSpanWidth(boundary_line),
              kDefaultMaxDialogueLineWidthPx);
    ASSERT_GT(meswnd_ref.MeasureSpanWidth(boundary_line),
              kEngineTextboxPixelWidth);

    TranslationMap map{{"jp_after_backlog", boundary_line}};
    DialogueTranslationSession session(&map, &measurer);
    const TranslationResult res =
        session.Translate("jp_after_backlog", kDialogueMaxWchars);
    ASSERT_TRUE(res.translated);
    EXPECT_NE(res.text.find("\\n"), std::string::npos);

    const std::vector<std::string> lines = SplitCatSystem2Lines(res.text);
    EXPECT_GE(lines.size(), kExpectedTwoLines);
    for (const std::string& line : lines) {
        const std::size_t px = meswnd_ref.MeasureSpanWidth(line);
        EXPECT_LE(px, kDefaultMaxDialogueLineWidthPx) << line;
        EXPECT_LE(px, kEngineTextboxPixelWidth) << line;
    }
}

TEST(GdiFontScaling,
     AllFourString8BaseFontCallSitesResolvePointerAndActiveWeight) {
    std::array<unsigned char, kString8MinReadableBytes> fake_string8{};
    std::memcpy(fake_string8.data() + kString8LayoutSizesOffset,
                kMeswndDialogueLayoutSizes.data(),
                sizeof(kMeswndDialogueLayoutSizes));
    const int bold_weight = static_cast<int>(kBoldFontWeight);
    const int normal_weight = static_cast<int>(kDefaultFontWeight);
    std::memcpy(fake_string8.data() + kString8MeasureWeightOffset,
                &bold_weight, sizeof(bold_weight));
    std::memcpy(fake_string8.data() + kString8RenderWeightOffset,
                &normal_weight, sizeof(normal_weight));

    std::array<unsigned char, kFakeStackFrameBytes> fake_frame{};
    const auto string8_addr =
        reinterpret_cast<std::uintptr_t>(fake_string8.data());
    std::memcpy(fake_frame.data(), &string8_addr, sizeof(string8_addr));
    const auto valid_halfwidth_ebp =
        reinterpret_cast<std::uintptr_t>(fake_frame.data()) +
        kMeasureHalfwidthEbpFrameDisp;

    GdiTextWidthMeasurer measurer(kDefaultScaledDialogueLfHeight);
    ASSERT_TRUE(measurer.IsValid());
    EXPECT_EQ(measurer.ActiveFontWeight(), kDefaultFontWeight);

    // 1. STRING8::MeasureGlyphs fullwidth pass (RVA 0x22596B): esi + [+0xCC].
    String8BaseFontObservation obs{};
    ASSERT_TRUE(ResolveString8BaseFontObservation(
        kSyntheticModuleBase,
        kSyntheticModuleBase + kMeasureGlyphsFullwidthBaseFontRetRva,
        fake_string8.data(), nullptr, 0, obs));
    EXPECT_EQ(obs.layout_sizes, kMeswndDialogueLayoutSizes);
    EXPECT_EQ(obs.font_weight, kBoldFontWeight);

    // 2. STRING8::MeasureGlyphs halfwidth ASCII pass (RVA 0x225C03):
    //    *(ebp - 0x24) + [+0xCC], with esi and ebx holding unrelated values.
    obs = {};
    ASSERT_TRUE(ResolveString8BaseFontObservation(
        kSyntheticModuleBase,
        kSyntheticModuleBase + kMeasureGlyphsHalfwidthBaseFontRetRva,
        nullptr, nullptr, valid_halfwidth_ebp, obs));
    EXPECT_EQ(obs.layout_sizes, kMeswndDialogueLayoutSizes);
    EXPECT_EQ(obs.font_weight, kBoldFontWeight);
    EXPECT_EQ(measurer.SyncFromString8BaseFont(
                  kPrimaryFontContextSlot, &obs.layout_sizes,
                  -kMeswndPresetSizeNormal, {}, obs.font_weight),
              kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontWeight(), kBoldFontWeight);

    // 3. STRING8::Render fullwidth pass (RVA 0x22BA7B): esi + [+0x10C].
    obs = {};
    ASSERT_TRUE(ResolveString8BaseFontObservation(
        kSyntheticModuleBase,
        kSyntheticModuleBase + kRenderFullwidthBaseFontRetRva,
        fake_string8.data(), nullptr, 0, obs));
    EXPECT_EQ(obs.layout_sizes, kMeswndDialogueLayoutSizes);
    EXPECT_EQ(obs.font_weight, kDefaultFontWeight);
    EXPECT_EQ(measurer.SyncFromString8BaseFont(
                  kPrimaryFontContextSlot, &obs.layout_sizes,
                  -kMeswndPresetSizeNormal, {}, obs.font_weight),
              kDefaultScaledDialogueLfHeight);
    EXPECT_EQ(measurer.ActiveFontWeight(), kDefaultFontWeight);

    // 4. STRING8::Render halfwidth pass (RVA 0x22BD09): ebx + [+0x10C].
    obs = {};
    ASSERT_TRUE(ResolveString8BaseFontObservation(
        kSyntheticModuleBase,
        kSyntheticModuleBase + kRenderHalfwidthBaseFontRetRva,
        nullptr, fake_string8.data(), 0, obs));
    EXPECT_EQ(obs.layout_sizes, kMeswndDialogueLayoutSizes);
    EXPECT_EQ(obs.font_weight, kDefaultFontWeight);

    // Safety checks: null/underflow ebp, null [ebp - 0x24], and all 5 inline
    // `\f` per-glyph override loop return RVAs must return false.
    EXPECT_FALSE(ResolveString8BaseFontObservation(
        kSyntheticModuleBase,
        kSyntheticModuleBase + kMeasureGlyphsHalfwidthBaseFontRetRva,
        fake_string8.data(), fake_string8.data(), 0, obs));
    EXPECT_FALSE(ResolveString8BaseFontObservation(
        kSyntheticModuleBase,
        kSyntheticModuleBase + kMeasureGlyphsHalfwidthBaseFontRetRva,
        fake_string8.data(), fake_string8.data(), kUnderflowEbp, obs));

    const std::uintptr_t null_ptr_value = 0;
    std::memcpy(fake_frame.data(), &null_ptr_value, sizeof(null_ptr_value));
    EXPECT_FALSE(ResolveString8BaseFontObservation(
        kSyntheticModuleBase,
        kSyntheticModuleBase + kMeasureGlyphsHalfwidthBaseFontRetRva,
        fake_string8.data(), fake_string8.data(), valid_halfwidth_ebp, obs));

    for (const std::uintptr_t inline_rva : kInlineFontOverrideRetRvas) {
        EXPECT_FALSE(ResolveString8BaseFontObservation(
            kSyntheticModuleBase, kSyntheticModuleBase + inline_rva,
            fake_string8.data(), fake_string8.data(), valid_halfwidth_ebp,
            obs));
    }
}

TEST(WordWrap, CatSystem2EscapeParityMatchesEngineTokenParser) {
    // 1. Font preset & size tags (`0x00626496`..`0x00626683`):
    //    `\fss`, `\fs`, `\fl`, `\fll`, and bare `\f;` consume optional `';'`,
    //    whereas `\fn` / `\FN` (`0x006265A6`) consumes ONLY 3 bytes (`"\fn"`),
    //    leaving a trailing `';'` visible.
    EXPECT_EQ(MeasureVisibleCodePoints("\\fss;A"), std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fs;B"), std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fl;C"), std::string_view("C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fll;D"), std::string_view("D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\f;E"), std::string_view("E").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fnE"), std::string_view("E").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fn;E"), std::string_view(";E").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\FN;E"), std::string_view(";E").size());

    //    `\fr` / `\FR` uses unsigned ParseUintArg (`0x006264C0`); when no
    //    unsigned digits follow, `0x00626344` calls `NextChar()` (consuming 1
    //    character without decrementing) and increments `[esi + 0x210]` by one
    //    additional character when that character was `';'` (`0x00626355`).
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr10;F"), std::string_view("F").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr0x10;F"),
              std::string_view("F").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr;GH"), std::string_view("H").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\FR;GH"), std::string_view("H").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr;G"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\FR;G"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr-5;G"),
              std::string_view("5;G").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\FR-5;G"),
              std::string_view("5;G").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\frG"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\FRG"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\FR"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr;\xE3\x81\x82" "H"),
              std::string_view("H").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\fr\xE3\x81\x82" "H"),
              std::string_view("H").size());

    //    `\f+` / `\f-` (`\F+` / `\F-`) parse unsigned digits after `+`/`-`;
    //    when no digits follow `+`/`-`, they consume 3 bytes (`"\f+"` / `"\f-"`),
    //    leaving `';'` visible.
    EXPECT_EQ(MeasureVisibleCodePoints("\\f0x18;H"),
              std::string_view("H").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\f+20;I"), std::string_view("I").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\f-2;J"), std::string_view("J").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\f+;K"), std::string_view(";K").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\f-;L"), std::string_view(";L").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\F+;K"), std::string_view(";K").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\F-;L"), std::string_view(";L").size());

    // 2. Wait tags (`0x006266EE`..`0x0062676B`):
    //    `\wr` / `\WR` consumes 3 bytes without `';'`; `\wf` and `\w` consume
    //    optional `';'` only when digits are present.
    EXPECT_EQ(MeasureVisibleCodePoints("\\wf20;A"), std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\wf0x10;B"),
              std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\wf;C"), std::string_view(";C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\WF;C"), std::string_view(";C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\wrD"), std::string_view("D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\wr;E"), std::string_view(";E").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\WR;E"), std::string_view(";E").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\w25;F"), std::string_view("F").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\w;G"), std::string_view(";G").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\W;H"), std::string_view(";H").size());

    // 3. Coordinate tags `\x` / `\y` (`0x0062680C`..`0x00626860`) use unsigned
    //    ParseUintArg (`allow_sign = false`): `"\x-10;"` consumes `"\x"` (2 bytes)
    //    leaving `"-10;"` visible; `"\x;"` leaves `";"` visible.
    EXPECT_EQ(MeasureVisibleCodePoints("\\x10;A"), std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\y0x08;B"),
              std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\x-10;A"),
              std::string_view("-10;A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\y+5;B"),
              std::string_view("+5;B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\x;A"), std::string_view(";A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\Y;B"), std::string_view(";B").size());

    // 4. Color tags (`0x006262E0`..`0x00626491`):
    //    `\cr` / `\CR` without unsigned digits shares `0x00626344` (`NextChar`
    //    plus extra step on `';'`); bare `\c;` / `\C;` consumes optional `';'`,
    //    whereas `\cs` / `\cd` without digits do NOT consume `';'`.
    EXPECT_EQ(MeasureVisibleCodePoints("\\c0xFF0000;A"),
              std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr0x00FF00;B"),
              std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr;CD"), std::string_view("D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\CR;CD"), std::string_view("D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr;C"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\CR;C"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr-5;C"),
              std::string_view("5;C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\CR-5;C"),
              std::string_view("5;C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\crC"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\CRC"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\CR"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr;\xE3\x81\x82" "D"),
              std::string_view("D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cr\xE3\x81\x82" "D"),
              std::string_view("D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\c;E"), std::string_view("E").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\C;F"), std::string_view("F").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cs12;C"), std::string_view("C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cd34;D"), std::string_view("D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cs;D"), std::string_view(";D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\CS;D"), std::string_view(";D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cd;D"), std::string_view(";D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\CD;D"), std::string_view(";D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\cgX\\cbY"),
              std::string_view("gXbY").size());

    // 5. `\id` (`0x006268D0`..`0x0062691D`) and remaining control tags:
    //    `0x006268ED`..`0x006268FA` checks `':'` directly after the first
    //    unsigned argument (including when `';'` preceded `':'`), `\id;X`
    //    consumes 3 bytes leaving `";X"` visible, and `\id12:;X` consumes 6
    //    bytes (`"\id12:"`) leaving `";X"` visible.
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12;A"), std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12:34;B"),
              std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12;:34;B"),
              std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ID12;:34;B"),
              std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id0x10:0x20;C"),
              std::string_view("C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id0x10;:0x20;C"),
              std::string_view("C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id;X"), std::string_view(";X").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ID;X"), std::string_view(";X").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12:;X"),
              std::string_view(";X").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ID12:;X"),
              std::string_view(";X").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12;:;X"),
              std::string_view(";X").size());
    EXPECT_EQ(MeasureVisibleCodePoints(
                  "\\tbi;D\\tE\\zRuby;F\\z;G\\$0x01;H\\$;I"),
              std::string_view("DEFGHI").size());

    //    `\u` / `\U` and `\ui` / `\UI` (`STRING16::ParseOneEscape`
    //    `0x0062CB36`..`0x0062CBB3`): `\ui<uint>[;]` consumes unsigned digits
    //    plus optional `';'` without exact-EOF decrement (`\ui` without digits
    //    consumes 3 bytes leaving `';'` or `"-5;"` visible); bare `\u` / `\U`
    //    consumes 2 bytes both mid-string and at exact EOF.
    EXPECT_EQ(MeasureVisibleCodePoints("\\uA"), std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\UA"), std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\u;A"), std::string_view(";A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\U;A"), std::string_view(";A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ui12;A"),
              std::string_view("A").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\UI0x10;B"),
              std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ui;C"),
              std::string_view(";C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\UI;C"),
              std::string_view(";C").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ui-5;D"),
              std::string_view("-5;D").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\UI-5;D"),
              std::string_view("-5;D").size());

    // 6. Exact-EOF `dec dword ptr [esi + 0x210]` parity (`0x0062629E`..`0x00626BF7`):
    //    bare `\f`/`\w`/`\p`, unterminated `\t` without `';'`, and `\id<uint>[;]`
    //    at exact EOF hit an unconditional cursor decrement after `NextChar()`
    //    returns 0 at EOF, leaving the trailing byte visible.
    EXPECT_EQ(MeasureVisibleCodePoints("\\f"), std::string_view("f").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\F"), std::string_view("F").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\w"), std::string_view("w").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\W"), std::string_view("W").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\p"), std::string_view("p").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\P"), std::string_view("P").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\t"), std::string_view("t").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\T"), std::string_view("T").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\tb"), std::string_view("b").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\TB"), std::string_view("B").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ti"), std::string_view("i").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\tbi"), std::string_view("i").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12"), std::string_view("2").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ID12"), std::string_view("2").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12;"), std::string_view(";").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\ID12;"), std::string_view(";").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id0x10"), std::string_view("0").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id0x10;"),
              std::string_view(";").size());

    //    Guarded / non-decrementing exact-EOF branches consume the full escape.
    EXPECT_EQ(MeasureVisibleCodePoints("\\t;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\tb;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\ti;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\tbi;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\id"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\ID"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12:"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\ID12:"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12:34"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12:34;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12;:"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12;:34"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12;:34;"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\i"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\c"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\C"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\x"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\y"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fs"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fss"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fl"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fll"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\fn"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\z"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\$"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\u"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\U"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\ui"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\UI"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\ui12"), 0u);
    EXPECT_EQ(MeasureVisibleCodePoints("\\UI0x10"), 0u);

    //    Tags followed by internal whitespace are NOT at `s.size()`, so word
    //    tokenization must not apply exact-EOF cursor backsteps mid-string.
    EXPECT_EQ(MeasureVisibleCodePoints("\\f Word"),
              std::string_view(" Word").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\w Word"),
              std::string_view(" Word").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\p Word"),
              std::string_view(" Word").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\tb Word"),
              std::string_view(" Word").size());
    EXPECT_EQ(MeasureVisibleCodePoints("\\id12; Word"),
              std::string_view(" Word").size());

    // Verify exact wrap-boundary placement when unconsumed characters remain
    // visible after `\x-10;` or at exact EOF (`\f`), and when `\id12;` precedes
    // a wrapped word mid-string.
    const CodePointWidthMeasurer cp_measurer(kSingleUnitPerCodePoint);
    std::string signed_coord_wrap = "\\x-10; Word";
    EXPECT_EQ(WordWrapByWidth(signed_coord_wrap,
                              std::string_view("-10;").size(), 0, cp_measurer),
              std::string_view("Word").size());
    EXPECT_EQ(signed_coord_wrap, "\\x-10;\\nWord");

    std::string eof_bare_f_wrap = "Hello \\f";
    EXPECT_EQ(WordWrapByWidth(eof_bare_f_wrap,
                              std::string_view("Hello").size(), 0, cp_measurer),
              std::string_view("f").size());
    EXPECT_EQ(eof_bare_f_wrap, "Hello\\n\\f");

    std::string mid_id_wrap = "Hello \\id12; World";
    EXPECT_EQ(WordWrapByWidth(mid_id_wrap,
                              std::string_view("Hello ").size(), 0, cp_measurer),
              std::string_view("World").size());
    EXPECT_EQ(mid_id_wrap, "Hello \\id12;\\nWorld");
}

TEST(DialogueSession,
     ControlOnlyContinuationNeverInsertsSpuriousSpaceOrDropsCarryPunctuation) {
    {
        TranslationMap map{
            {"jp_s1", "Sentence.\\@"},
            {"\\n\\@", "\\n\\@"},
            {"jp_wait_only", "\\w20;\\@"},
            {"\\njp_s2", "\\nNext."},
        };
        const MockProportionalMeasurer mock;
        DialogueTranslationSession session(&map, &mock);

        const TranslationResult r1 =
            session.Translate("jp_s1", kDialogueMaxWchars);
        const TranslationResult r2 =
            session.Translate("\\n\\@", kDialogueMaxWchars);
        const TranslationResult r3 =
            session.Translate("jp_wait_only", kDialogueMaxWchars);
        const TranslationResult r4 =
            session.Translate("\\njp_s2", kDialogueMaxWchars);

        ASSERT_TRUE(r1.translated);
        ASSERT_TRUE(r2.translated);
        ASSERT_TRUE(r3.translated);
        ASSERT_TRUE(r4.translated);
        EXPECT_EQ(r1.text, "Sentence.\\@");
        EXPECT_EQ(r2.text, "\\@");
        EXPECT_EQ(r3.text, "\\w20;\\@");
        EXPECT_EQ(r4.text, " Next.");
        EXPECT_EQ(r1.text + r2.text + r3.text + r4.text,
                  "Sentence.\\@\\@\\w20;\\@ Next.");
    }

    {
        const MonospaceByteWidthMeasurer byte_measurer;
        const std::size_t prefix_len =
            kDefaultMaxDialogueWidthPx - kNearRightMarginOffsetPx;
        const std::string near_margin_sentence =
            std::string(prefix_len - kSingleCharStep, 'a') + ".\\@";
        const std::string exact_margin_sentence =
            std::string(kDefaultMaxDialogueWidthPx - kSingleCharStep, 'b') +
            ".\\@";

        TranslationMap map{
            {"jp_near", near_margin_sentence},
            {"jp_exact", exact_margin_sentence},
            {"\\n\\@", "\\n\\@"},
            {"\\njp_next", "\\nNext."},
        };
        DialogueTranslationSession session(&map, &byte_measurer);

        const TranslationResult near_r1 =
            session.Translate("jp_near", kDialogueMaxWchars);
        const TranslationResult near_r2 =
            session.Translate("\\n\\@", kDialogueMaxWchars);
        const TranslationResult near_r3 =
            session.Translate("\\njp_next", kDialogueMaxWchars);
        EXPECT_EQ(near_r1.text, near_margin_sentence);
        EXPECT_EQ(near_r2.text, "\\@");
        EXPECT_EQ(near_r3.text, "\\nNext.");

        const TranslationResult exact_r1 =
            session.Translate("jp_exact", kDialogueMaxWchars);
        const TranslationResult exact_r2 =
            session.Translate("\\n\\@", kDialogueMaxWchars);
        const TranslationResult exact_r3 =
            session.Translate("\\njp_next", kDialogueMaxWchars);
        EXPECT_EQ(exact_r1.text, exact_margin_sentence);
        EXPECT_EQ(exact_r2.text, "\\@");
        EXPECT_EQ(exact_r3.text, "\\nNext.");
    }
}

TEST(DialogueSession,
     ForwardEscapeTokenizationClassifiesTrailingBackslashAndInlineWaitAccurately) {
    // Trailing `'\\'` consumed by `\cr`, `\fr`, `\z`, or `\$` is NOT dangling.
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\cr\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\fr\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\cr;\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\fr;\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\zRuby\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\$0x01\\"));
    EXPECT_FALSE(HasDanglingTrailingBackslash("Hello\\f"));

    // A second `'\\'` after `\cr\` or a `'\\'` after a terminated tag IS dangling.
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\cr\\\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\fr\\\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\cr;\\\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\zRuby;\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\$0x01;\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\t\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\tb\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\f\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\p\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\w\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\id12;\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\u\\"));
    EXPECT_TRUE(HasDanglingTrailingBackslash("Hello\\ui12;\\"));

    std::string cr_consumed = "Hello\\cr\\";
    StripDanglingTrailingBackslash(cr_consumed);
    EXPECT_EQ(cr_consumed, "Hello\\cr\\");

    std::string cr_plus_dangling = "Hello\\cr\\\\";
    StripDanglingTrailingBackslash(cr_plus_dangling);
    EXPECT_EQ(cr_plus_dangling, "Hello\\cr\\");
    StripDanglingTrailingBackslash(cr_plus_dangling);
    EXPECT_EQ(cr_plus_dangling, "Hello\\cr\\");

    std::string unterm_ruby = "Hello\\zRuby\\";
    StripDanglingTrailingBackslash(unterm_ruby);
    EXPECT_EQ(unterm_ruby, "Hello\\zRuby\\");

    // Forward `EndsWithInlineWaitTag` distinguishes `\@` from literal `'@'`
    // after `\cr\`/`\fr\` or inside unterminated `\z`/`\$` arguments.
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\\\\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("Hello\\\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("Hello\\cr\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("Hello\\fr\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("Hello\\cr;\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("Hello\\fr;\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\cr\\\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\fr\\\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\cr;\\\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("Hello\\zRuby\\@"));
    EXPECT_FALSE(EndsWithInlineWaitTag("Hello\\$0x01\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\zRuby;\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\$0x01;\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\t\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\f\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\id12;\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\u\\@"));
    EXPECT_TRUE(EndsWithInlineWaitTag("Hello\\ui12;\\@"));

    // End-to-end session check: `"Hello\\cr\\"` preserves `\cr\` so appending
    // `"\\nNext."` keeps `\n` as a real line break rather than `\cr\n`.
    TranslationMap map{
        {"jp_cr", "Hello\\cr\\"},
        {"\\njp_next", "\\nNext."},
        {"jp_fake_wait", "Hello\\cr\\@"},
        {"jp_fresh", "Fresh."},
    };
    const CodePointWidthMeasurer cp_measurer(kSingleUnitPerCodePoint);
    DialogueTranslationSession session(&map, &cp_measurer);

    const TranslationResult r1 =
        session.Translate("jp_cr", kDialogueMaxWchars);
    const TranslationResult r2 =
        session.Translate("\\njp_next", kDialogueMaxWchars);
    EXPECT_EQ(r1.text, "Hello\\cr\\");
    EXPECT_EQ(r2.text, " Next.");

    const TranslationResult r3 =
        session.Translate("jp_fake_wait", kDialogueMaxWchars);
    EXPECT_FALSE(session.PrevEndedWithInlineWait());
    EXPECT_EQ(r3.end_width, std::string_view("Hello@").size());

    const TranslationResult r4 =
        session.Translate("jp_fresh", kDialogueMaxWchars);
    EXPECT_FALSE(r4.is_continuation);
    EXPECT_EQ(r4.text, "Fresh.");
}

TEST(WordWrap,
     OverflowingSpaceBeforeHardBreakAndZeroWidthTagNeverExceedsMaxWidth) {
    const MonospaceByteWidthMeasurer byte_measurer;
    const CodePointWidthMeasurer cp_measurer(kSingleUnitPerCodePoint);
    const std::size_t max_five = std::string_view("Hello").size();

    // Space overflowing right before an explicit `\n`, `\N`, or literal '\n'
    // is dropped without emitting a duplicate line break.
    std::string space_before_ctrl_nl = "Hello \\nWorld";
    EXPECT_EQ(WordWrapByWidth(space_before_ctrl_nl, max_five, 0, byte_measurer),
              std::string_view("World").size());
    EXPECT_EQ(space_before_ctrl_nl, "Hello\\nWorld");

    std::string space_before_upper_nl = "Hello \\NWorld";
    EXPECT_EQ(WordWrapByWidth(space_before_upper_nl, max_five, 0, byte_measurer),
              std::string_view("World").size());
    EXPECT_EQ(space_before_upper_nl, "Hello\\NWorld");

    std::string space_before_lit_nl = "Hello \nWorld";
    EXPECT_EQ(WordWrapByWidth(space_before_lit_nl, max_five, 0, byte_measurer),
              std::string_view("World").size());
    EXPECT_EQ(space_before_lit_nl, "Hello\nWorld");

    std::string space_tag_before_nl = "Hello \\f-20; \\nWorld";
    EXPECT_EQ(WordWrapByWidth(space_tag_before_nl, max_five, 0, cp_measurer),
              std::string_view("World").size());
    EXPECT_EQ(space_tag_before_nl, "Hello\\f-20;\\nWorld");

    // Space overflowing before a standalone zero-width control tag wraps before
    // the tag and drops the post-tag inter-word space on the auto-wrapped line.
    std::string space_around_id = "Hello \\id12; World";
    EXPECT_EQ(WordWrapByWidth(space_around_id, max_five, 0, cp_measurer),
              std::string_view("World").size());
    EXPECT_EQ(space_around_id, "Hello\\n\\id12;World");

    const std::vector<std::string> id_lines =
        SplitCatSystem2Lines(space_around_id);
    ASSERT_EQ(id_lines.size(), kExpectedTwoLines);
    for (const std::string& line : id_lines) {
        EXPECT_LE(MeasureVisibleCodePoints(line), max_five) << line;
    }

    std::string chained_zero_width = "Hello \\id12; \\ui3; World";
    EXPECT_EQ(WordWrapByWidth(chained_zero_width, max_five, 0, cp_measurer),
              std::string_view("World").size());
    EXPECT_EQ(chained_zero_width, "Hello\\n\\id12;\\ui3;World");
}

TEST(DialogueSession,
     LineBreakAndSpacedTaggedContinuationsNeverInsertSpuriousSpace) {
    const CodePointWidthMeasurer cp_measurer(kSingleUnitPerCodePoint);

    // 1. Within margin: `"\\n\\w20;\\nNext."` has no visible character before
    //    its internal `\n` (`first_vis == '\0'`), and `"\\n\\w20; Next."`
    //    already begins its visible text with `' '` (`first_vis == ' '`).
    //    Neither receives a leading `' '`.
    {
        TranslationMap map{
            {"jp_s1", "Sentence."},
            {"\\njp_brk", "\\n\\w20;\\nNext."},
            {"jp_s2", "Sentence."},
            {"\\njp_sp", "\\n\\w20; Next."},
        };
        DialogueTranslationSession session(&map, &cp_measurer);

        const TranslationResult b1 =
            session.Translate("jp_s1", kDialogueMaxWchars);
        const TranslationResult b2 =
            session.Translate("\\njp_brk", kDialogueMaxWchars);
        EXPECT_EQ(b1.text, "Sentence.");
        EXPECT_EQ(b2.text, "\\w20;\\nNext.");
        EXPECT_EQ(b1.text + b2.text, "Sentence.\\w20;\\nNext.");

        session.ResetDialogueState();
        const TranslationResult s1 =
            session.Translate("jp_s2", kDialogueMaxWchars);
        const TranslationResult s2 =
            session.Translate("\\njp_sp", kDialogueMaxWchars);
        EXPECT_EQ(s1.text, "Sentence.");
        EXPECT_EQ(s2.text, "\\w20; Next.");
        EXPECT_EQ(s1.text + s2.text, "Sentence.\\w20; Next.");
    }

    // 2. At exact line-width budget: `"Sentence."` fills the entire first line,
    //    and neither `\n`-prefixed nor `\@`-continued `"\\w20;\\nNext."` or
    //    `"\\n\\w20; Next."` overflows line 1 with a spurious space.
    {
        constexpr std::string_view kExactSentence = "Sentence.";
        DialogueSessionConfig cfg{};
        cfg.max_line_width = kExactSentence.size();

        TranslationMap map{
            {"jp_exact", std::string(kExactSentence)},
            {"jp_exact_wait", "Sentence.\\@"},
            {"\\njp_brk", "\\n\\w20;\\nNext."},
            {"jp_wait_brk", "\\w20;\\nNext."},
            {"\\njp_sp", "\\n\\w20; Next."},
        };
        DialogueTranslationSession session(&map, &cp_measurer, cfg);

        const TranslationResult r1 =
            session.Translate("jp_exact", kDialogueMaxWchars);
        const TranslationResult r2 =
            session.Translate("\\njp_brk", kDialogueMaxWchars);
        EXPECT_EQ(r1.text, "Sentence.");
        EXPECT_EQ(r2.text, "\\w20;\\nNext.");
        EXPECT_EQ(r2.end_width, std::string_view("Next.").size());

        session.ResetDialogueState();
        const TranslationResult w1 =
            session.Translate("jp_exact_wait", kDialogueMaxWchars);
        const TranslationResult w2 =
            session.Translate("jp_wait_brk", kDialogueMaxWchars);
        EXPECT_EQ(w1.text, "Sentence.\\@");
        EXPECT_EQ(w2.text, "\\w20;\\nNext.");
        EXPECT_EQ(w2.end_width, std::string_view("Next.").size());

        session.ResetDialogueState();
        const TranslationResult sp1 =
            session.Translate("jp_exact", kDialogueMaxWchars);
        const TranslationResult sp2 =
            session.Translate("\\njp_sp", kDialogueMaxWchars);
        EXPECT_EQ(sp1.text, "Sentence.");
        EXPECT_EQ(sp2.text, "\\w20;\\nNext.");
        EXPECT_EQ(sp2.end_width, std::string_view("Next.").size());
    }
}

