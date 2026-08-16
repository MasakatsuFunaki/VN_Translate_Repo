// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// translator_logic.h
//
// Pure (OS-independent) logic that powers the FRATERNITE_HD runtime
// translator. Everything here is free of Windows / GDI / overlay
// dependencies so the same object file can be linked into both the
// proxy DLL and the gtest binary.
//
// The counterpart translator.cpp wires these primitives up to the
// real engine: it reads buf38 via a state-struct peek, calls
// SegmentMessage on the result, and pushes each piece to the overlay.
// All the tricky cases (boundary-gated segmentation, TSV parsing,
// speaker/quote splitting) live here where they can be exercised
// without the game running.
//
// All CP932 Japanese literals referenced in the logic are written as
// raw \x byte escapes so the source file stays pure ASCII; comments
// also avoid Japanese glyphs to keep MSVC (CP932 system codepage)
// from emitting C4819 warnings or -- worse -- eating newlines
// around unsafe byte sequences in comments.

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

// --- Low-level CP932 helpers --------------------------------------------

// True if the byte is a valid CP932 first-byte (i.e. the start of a
// 2-byte JIS glyph). The second byte follows.
bool IsCp932LeadByte(unsigned char b);

// True if the string contains at least one CP932 lead byte. Used to
// distinguish CJK text from pure-ASCII UI strings.
bool HasJPLeadByte(const std::string& s);

// True if the 2-byte CP932 glyph (a, b) is a sentence-terminating
// punctuation mark YSTB uses to end a script entry. The accepted
// set is:
//   0x81 0x42  fullwidth period
//   0x81 0x48  fullwidth question mark
//   0x81 0x49  fullwidth exclamation
//   0x81 0x76  close corner bracket (quote end)
//   0x81 0x63  ellipsis (horizontal)
// Deliberately excluded: 0x81 0x5B (prolonged-sound mark, katakana
// chouon) and 0x81 0x60 (wave dash). Both legitimately appear
// mid-word so accepting them would let a mid-word match slip.
bool IsCp932Terminator(unsigned char a, unsigned char b);

// True if (a, b) marks the START of a new YSTB script unit inside a
// compound buf38 message -- a position at which a match may end even
// without a terminator of its own:
//   0x81 0x75  open corner bracket (speaker -> quote transition)
//   0x81 0x40  fullwidth space (entry separator)
//   '\n'       line break
bool IsCp932EntryStart(unsigned char a, unsigned char b);

// --- String helpers ------------------------------------------------------

// Undo the TSV escape sequences: \r \n \t \\. In-place.
void UnescapeInPlace(std::string& s);

// Replace any \r \n \t in the string with a single space. Used on the
// English side because the overlay uses \n itself as the entry
// separator.
std::string SanitizeEn(const std::string& raw);

// True if `needle` is a byte-wise prefix of `haystack`.
bool IsPrefix(const std::string& needle, const std::string& haystack);

// --- TSV loading ---------------------------------------------------------

// Parse a TSV buffer in memory into a translation map. Each line is
// "jp\ten\n" (or \r\n). Unescapes both sides, drops empty entries and
// the pipeline's "(garbled data)" placeholder. Returns the number of
// entries inserted; `out` is appended to, not cleared.
//
// Takes a view as (ptr, len). Accepts both \n and \r\n line endings.
int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out);

// Load a TSV file by path and parse it into a map. Returns true iff
// the file was opened and at least one entry was parsed.
bool LoadTsvFile(const std::string& path, TranslationMap& out);

// --- Speaker/quote split -------------------------------------------------

// For every map entry whose JP has the shape SPEAKER<open>QUOTE<close>
// AND whose EN has the matching shape, also register the speaker part
// (SPEAKER -> EN-SPEAKER) and the quoted part (<open>QUOTE<close> ->
// EN-<open>QUOTE<close>) as separate keys.
// The CP932 brackets are 0x81 0x75 (open) and 0x81 0x76 (close).
// Returns the number of new keys actually added; does not overwrite
// existing keys.
//
// The engine often renders speaker and quote in separate buf38
// populations, so these split keys let per-slot lookups hit without
// relying on the scenario writer having pre-split them.
int SplitSpeakerQuote(TranslationMap& map);

// --- buf38 reconstruction (pure, testable) -------------------------------
//
// The YU-RIS engine renders messages out of a paired (buf38, buf3c)
// where buf3c[i] is the opcode (0x31 = 1B ASCII glyph, 0x32 = 2B CP932
// glyph, plus formatting opcodes 0x43/0x45/0x50/0x52/0x70/0x72) and
// buf38 holds the actual text bytes.

// Reconstruct the JP message bytes from the opcode/text buffers, walking
// `opcount` opcodes in `buf3c` and concatenating the bytes consumed by
// opcodes 0x31 (1 byte) and 0x32 (2 bytes). All other opcodes (line
// breaks, page breaks, waits) do NOT advance the text cursor in the
// engine's renderer and we mirror that behaviour here so the
// reconstructed string matches what the JP-string extractor recorded
// into the TSV.
//
// `buf38_cap` and `buf3c_cap` cap how far we'll read from each buffer
// so a corrupt opcount can't run off the end of either.
std::string ReconstructJpFromBuffers(const char* buf38, size_t buf38_cap,
                                     const char* buf3c, size_t buf3c_cap,
                                     int opcount);

// --- Segmenter -----------------------------------------------------------

struct Segment {
    std::string jp;
    std::string en;
};

// Greedy longest-prefix segmenter for a compound buf38 message.
//
// Walks the message left to right. At each position it finds the
// longest TSV key that is a prefix of the remaining bytes AND passes
// the BOUNDARY GATE:
//   - consumes the rest of the message, OR
//   - ends at a CP932 sentence terminator, OR
//   - is followed by a CP932 entry-start marker.
// If nothing passes, skip 2 bytes and retry. The gate is what
// prevents mid-word single-kanji matches from polluting the output
// -- real YSTB entry boundaries always align with one of those
// conditions.
std::vector<Segment> SegmentMessage(const std::string& msg,
                                    const TranslationMap& map);

// --- Rendering pipeline (per-glyph-slot chunk plan) ----------------------
//
// The proxy DLL's HookedTextOutA emits one TextOutA call per CP932 glyph
// rendered by the engine's per-glyph render loop. To paint EN over JP we
// build a "plan": one chunk of `chars_per_slot` ASCII characters per
// glyph slot. The engine fires multiple passes per slot (shadow, outline,
// fill) and the call-count → slot-index mapping in HookedTextOutA picks
// the right chunk on each call.
//
// The plan length MUST equal the number of glyphs the engine will render
// (i.e. CountCp932Glyphs of the reconstructed JP message), otherwise
// chunks misalign with glyph slots and the rendered EN drifts off
// position. This invariant was not previously tested and is the source
// of the leading-fullwidth-space / trailing-bracket misalignment bug.

// Count CP932 glyphs in a CP932 byte string. Lead bytes (0x81-0x9F,
// 0xE0-0xFC) advance 2 bytes; everything else advances 1.
int CountCp932Glyphs(const std::string& s);

// Flatten an EN string from the TSV (which may contain a few CP932
// punctuation glyphs the translator left embedded) into pure ASCII.
// Maps the canonical brackets/period/space; drops anything else.
std::string FlattenEnToAscii(const std::string& en);

// Spread an EN string across `slots` chunks of EXACTLY `chars_per_slot`
// chars each, appending to `out`. Shorter EN -> trailing blank-padded
// chunks. Longer EN -> truncated at slots * chars_per_slot.
void SpreadEnAcrossSlots(const std::string& en, int slots, int chars_per_slot,
                         std::vector<std::string>& out);

// Re-split combined "SPEAKER<open>QUOTE<close>" segments into per-piece
// segments. Uses speaker/quote independent lookups in `map` first, then
// falls back to splitting the combined EN at speaker_en's match position
// or at the first " / <open> in the EN.
void ResplitSpeakerQuote(std::vector<Segment>& segs,
                         const TranslationMap& map);

// Build the per-glyph render plan for a JP message: one chunk per
// CP932 glyph slot. Walks the JP message glyph-by-glyph so chunks
// align to glyph positions; unmatched glyphs (gaps, leading/trailing
// non-TSV bytes) emit blank chunks. Hidden-speaker (CP932 / = 0x81 0x5E)
// segments emit blanks for their slots so the original game's hidden-
// speaker behaviour is preserved.
//
// POSTCONDITION: chunks.size() == CountCp932Glyphs(jp). Tests assert
// this invariant explicitly.
void BuildRenderPlan(const std::string& jp, const TranslationMap& map,
                     int chars_per_slot,
                     std::vector<std::string>& chunks);

// One segment whose flattened EN is longer than the slot budget the
// plan can give it (own slots + the gap immediately after, until the
// next segment). Visual symptom: trailing words of the EN don't get
// rendered. Used by tests / corpus replay to catch truncation
// regressions without playing the game.
struct OverflowedSegment {
    std::string seg_jp;
    std::string seg_en;        // FlattenEnToAscii applied
    int         slots_available;
    int         chars_needed;
};

// Run the same region walk BuildRenderPlan does, and return any
// segments whose EN exceeds the available slot budget. An empty
// vector means every segment fits and no rendered char would be
// truncated.
std::vector<OverflowedSegment>
DetectOverflows(const std::string& jp, const TranslationMap& map,
                int chars_per_slot);

// Pick the smallest chars_per_slot in [min_cps, max_cps] for which
// DetectOverflows returns empty for this message — i.e. the largest
// font that still lets every segment's EN fit. The DLL uses this to
// auto-narrow the font on a per-message basis: most messages render
// at min_cps (wide / readable); only messages whose EN doesn't fit
// at min_cps narrow further. Returns max_cps if even that isn't
// enough (caller should report the residual overflow).
int ComputeAutoFitCharsPerSlot(const std::string& jp,
                               const TranslationMap& map,
                               int min_cps, int max_cps);

// One placement decision the planner made for a single visible segment.
// Pack-tight defaults to placing each segment immediately after the
// previous one ended (with a leading separator space so adjacent
// sentences don't visually butt together). The exception is the
// "speaker label → quote body" boundary: the engine renders the
// speaker name at a separate (x,y) from the dialog body, so packing
// the body content into the speaker slots displaces it onto the
// label area. ComputePackPlan detects that boundary (first visible
// segment's JP is in the map AND the next segment's JP starts with
// open-kakko `「`) and lets the quote start at its natural JP slot
// instead of packing.
struct PlacedSeg {
    size_t      seg_idx;     // index into the SegmentMessage+Resplit output
    std::string en;          // exact bytes to paint, post-trim, post-prefix
    size_t      place_at;    // chunk index where the first byte lands
    size_t      en_slots;    // ceil(en.size / chars_per_slot)
};

std::vector<PlacedSeg>
ComputePackPlan(const std::string& jp, const TranslationMap& map,
                int chars_per_slot);

// --- Full-line English reconstruction (overlay renderer) -----------------
//
// The overlay renderer draws the WHOLE English line itself (proportional
// font, real word-wrap) instead of squishing per-glyph chunks into the
// engine's slot grid. It needs the message's English as plain text, with
// the speaker name separated from the body (the engine keeps rendering
// the short speaker label; the overlay only replaces the body).

struct EnglishLine {
    bool        valid         = false;  // false => no TSV segments matched
    std::string speaker;                // ASCII speaker name ("" if none)
    std::string body;                   // ASCII body, ready to word-wrap
    int         speaker_slots = 0;      // CP932 glyph slots the speaker
                                        // occupies at the front of the JP
                                        // message (so the hook can suppress
                                        // only the BODY slots, idx >= this).
};

// Reconstruct the English for a JP message: segment it, re-split any
// speaker/quote, then join the body segment ENs (FlattenEnToAscii'd,
// single-spaced, whitespace-collapsed). Detects a leading speaker label
// the same way ComputePackPlan does (first segment is a known TSV key AND
// the next segment's JP starts with open-kakko 0x81 0x75). Returns
// valid=false when segmentation yields nothing usable, so the caller can
// fall through to the engine's own rendering rather than blanking the line.
EnglishLine ReconstructEnglish(const std::string& jp, const TranslationMap& map);

}  // namespace translator_logic
