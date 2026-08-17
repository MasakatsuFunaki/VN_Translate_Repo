// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// OS-independent translation logic, shared by the proxy DLL and tests.
// All JP literals use raw \x escapes to avoid MSVC CP932 codepage issues.

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

// --- CP932 helpers ---------------------------------------------------------

bool IsCp932LeadByte(unsigned char b);
bool HasJPLeadByte(const std::string& s);

// Sentence-ending punctuation: period/question/exclamation/close-bracket/ellipsis.
// Excludes chouon (0x81 0x5B) and wave dash (0x81 0x60) — they appear mid-word.
bool IsCp932Terminator(unsigned char a, unsigned char b);

// Start of a new YSTB unit: open-bracket, fullwidth space, or newline.
bool IsCp932EntryStart(unsigned char a, unsigned char b);

// --- String helpers --------------------------------------------------------

void UnescapeInPlace(std::string& s);
std::string SanitizeEn(const std::string& raw);
bool IsPrefix(const std::string& needle, const std::string& haystack);

// --- TSV loading -----------------------------------------------------------

// Appends to `out`; returns entry count. Drops empty and "(garbled data)".
int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out);
bool LoadTsvFile(const std::string& path, TranslationMap& out);

// --- Speaker/quote split ---------------------------------------------------

// Split SPEAKER+QUOTE entries into independent keys so per-slot lookups
// hit when the engine renders them separately. Returns new keys added.
int SplitSpeakerQuote(TranslationMap& map);

// --- buf38 reconstruction --------------------------------------------------

// Walk buf3c opcodes (0x31=1B, 0x32=2B) and concatenate the text bytes
// from buf38. Non-text opcodes are skipped to match the extractor's keys.
std::string ReconstructJpFromBuffers(const char* buf38, size_t buf38_cap,
                                     const char* buf3c, size_t buf3c_cap,
                                     int opcount);

// --- Segmenter -------------------------------------------------------------

struct Segment {
    std::string jp;
    std::string en;
};

// Greedy longest-prefix segmenter with a boundary gate: a match must
// consume the rest, end at a terminator, or be followed by an entry-start.
std::vector<Segment> SegmentMessage(const std::string& msg,
                                    const TranslationMap& map);

// --- Render plan -----------------------------------------------------------
// One chunk per CP932 glyph slot. Plan length MUST equal CountCp932Glyphs(jp).

int CountCp932Glyphs(const std::string& s);

// Map embedded CP932 punctuation to ASCII; drop the rest.
std::string FlattenEnToAscii(const std::string& en);

// Distribute EN across `slots` chunks of `chars_per_slot` chars each.
void SpreadEnAcrossSlots(const std::string& en, int slots, int chars_per_slot,
                         std::vector<std::string>& out);

// Split combined SPEAKER+QUOTE segments into independent pieces.
void ResplitSpeakerQuote(std::vector<Segment>& segs,
                         const TranslationMap& map);

// POSTCONDITION: chunks.size() == CountCp932Glyphs(jp).
void BuildRenderPlan(const std::string& jp, const TranslationMap& map,
                     int chars_per_slot,
                     std::vector<std::string>& chunks);

// Segments whose EN exceeds the available slot budget (truncation).
struct OverflowedSegment {
    std::string seg_jp;
    std::string seg_en;
    int         slots_available;
    int         chars_needed;
};

// Empty result means every segment fits.
std::vector<OverflowedSegment>
DetectOverflows(const std::string& jp, const TranslationMap& map,
                int chars_per_slot);

// Smallest chars_per_slot in [min, max] with no overflows.
int ComputeAutoFitCharsPerSlot(const std::string& jp,
                               const TranslationMap& map,
                               int min_cps, int max_cps);

// Pack-tight placement for one visible segment. The speaker->quote
// boundary breaks the pack so the body doesn't land on the label area.
struct PlacedSeg {
    size_t      seg_idx;
    std::string en;
    size_t      place_at;
    size_t      en_slots;
};

std::vector<PlacedSeg>
ComputePackPlan(const std::string& jp, const TranslationMap& map,
                int chars_per_slot);

// --- Full-line English (overlay renderer) -----------------------------------

struct EnglishLine {
    bool        valid         = false;
    std::string speaker;
    std::string body;
    int         speaker_slots = 0;  // glyph slots the speaker occupies
};

// Segment, re-split, join body ENs. Returns valid=false when nothing matches.
EnglishLine ReconstructEnglish(const std::string& jp, const TranslationMap& map);

}  // namespace translator_logic
