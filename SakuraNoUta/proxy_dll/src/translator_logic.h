// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// translator_logic.h -- OS-independent translation logic (links into both
// the proxy DLL and gtest).  TSV is UTF-8; both columns are converted to
// CP932 at load time so lookups match the wire bytes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

// --- TSV parsing -----------------------------------------------------

// Undo \\, \r, \n, \t escapes from 03_build_translations, in place.
void UnescapeInPlace(std::string& s);

// An English character with no CP932 form, collected for logging.
struct UnmappableChar {
    std::string jp_key;      // the row it came from, still UTF-8 for the log
    unsigned    codepoint = 0;
};

// Parse "<jp>\t<en>" lines from a UTF-8 TSV buffer into CP932 key/value
// pairs.  Values go through markup strip and English folds; keys do not.
// Returns entries inserted.  `out` is appended to.
int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out,
                   std::vector<UnmappableChar>* unmappable = nullptr,
                   std::vector<std::string>* markup_rows = nullptr);

// Convenience wrapper that opens a file and forwards to ParseTsvBuffer.
// Returns true iff at least one entry was parsed.
bool LoadTsvFile(const std::string& path, TranslationMap& out,
                 std::vector<UnmappableChar>* unmappable = nullptr,
                 std::vector<std::string>* markup_rows = nullptr);

// --- CP932 helpers ---------------------------------------------------

// True if the byte is a valid CP932 lead byte (start of a 2-byte glyph).
bool IsCp932LeadByte(unsigned char b);

// True if the buffer contains at least one CP932 lead byte (has JP content).
bool HasJPLeadByte(const char* data, size_t len);

// Convert UTF-8 to CP932 (unmappable characters become '?').
// Non-Windows builds fall back to identity (gtest exercises the parser only).
std::string Utf8ToCp932(const std::string& utf8);

// CP932 -> UTF-8 (for logging wire keys readably).
std::string Cp932ToUtf8(const std::string& cp932);

// --- Engine markup ----------------------------------------------------
// The engine consumes "<tag>" runs (prefix-matched, case-insensitive).
// A '<' with no '>' after it, and "<>", are drawn as ordinary characters.

// True if `text` carries a bracket pair the engine would consume as a tag.
bool CarriesEngineMarkup(const std::string& text);

// Strip all "<...>" tags from an English value, keeping the base text between
// opener and closer.
std::string StripEngineMarkup(const std::string& text);

// True if `cp932` carries a furigana tag ("<r ...>" / "<ruby ...>").
bool CarriesRuby(const std::string& cp932);

// Strip furigana from a CP932 buffer the engine will draw itself.  At the
// 30 px pitch the reading would overlap the line above; other tags are kept.
std::string StripRubyForDisplay(const std::string& cp932);

// --- English value preparation ---------------------------------------

// Convert a UTF-8 English value to CP932, folding typographic characters
// (curly quotes, dashes, ellipsis, accented letters, Greek, fullwidth ASCII,
// zero-width chars) to ASCII equivalents so they draw halfwidth.  Surviving
// '<...>' bracket pairs become '(' to prevent the engine consuming them.
// Characters with no CP932 form are drawn as '?' and appended to `unmappable`.
std::string EnglishToCp932(const std::string& utf8,
                           std::vector<unsigned>* unmappable = nullptr);

// True if `cp932` holds a two-byte character (fold table gap to report).
bool HasWideCharacter(const std::string& cp932);

// --- Message box geometry --------------------------------------------
// Measured at 1280x720: 14.0 px per half-width glyph, engine wraps at 71
// columns, panel interior y = 557..709.  At the substituted 30 px pitch
// four glyph bands fit (590..704).  The engine also applies kinsoku shori
// (no-line-start / no-line-end lookahead), which we model conservatively.

// Engine word-wrap width in half-width characters.  Kinsoku lookahead can
// break earlier (modelled) or let punctuation hang past it (not modelled).
constexpr size_t BOX_COLUMNS = 71;

// No continuation indent: the engine draws none of its own, so an indented
// second line reads as a new paragraph rather than as a wrap.
constexpr size_t WRAP_INDENT = 0;

// Content columns per continuation line (WRAP_INDENT + WRAP_AT = BOX_COLUMNS).
constexpr size_t WRAP_AT = 71;

// Full width for the first / only line (no indent).
constexpr size_t SINGLE_LINE_MAX = 71;

// The engine indents 28 px (2 columns) after a prefixed segment's 0x0A.
// Budget every line of a prefixed segment 2 columns narrower.
constexpr size_t CHAIN_INDENT = 2;

// Four lines at the substituted 30 px pitch (stock 42 px gives three).
constexpr size_t MAX_LINES = 4;

static_assert(WRAP_AT + WRAP_INDENT <= BOX_COLUMNS,
              "a continuation line must fit the measured box width");
static_assert(SINGLE_LINE_MAX <= BOX_COLUMNS,
              "an unwrapped line must fit the measured box width");
static_assert(WRAP_AT > WRAP_INDENT + 3,
              "a truncated line must have room for its content and the marker");
static_assert(MAX_LINES >= 1, "at least one line has to be rendered");

// --- The history window ----------------------------------------------
// Backlog entries are clipped to 105 px below the first text row.  At the
// substituted 26 px advance, four rows fit; at stock 40 px, three.
constexpr size_t HISTORY_MAX_LINES = 4;

// --- What one call may draw -------------------------------------------
// The renderer allocates 4 rects/byte from the JP buffer it measured; our
// longer English can overrun it.  The hook patches the multiplier (8x);
// the budget below is the backstop when the patch is absent.
constexpr size_t RECTS_PER_WIRE_BYTE = 4;

// Slack for engine-internal segments; capped at 1/8 of the array so short
// lines (speaker names) keep enough budget.
constexpr size_t RECT_BUDGET_MARGIN = 16;

// Max drawable bytes given the wire length and rects-per-byte multiplier.
constexpr size_t DrawBudget(size_t wire_bytes, size_t rects_per_byte) {
    const size_t rects = wire_bytes * rects_per_byte;
    const size_t margin = rects / 8 < RECT_BUDGET_MARGIN ? rects / 8
                                                         : RECT_BUDGET_MARGIN;
    return rects > margin ? rects - margin : 1;
}

// Lines and byte budget for one drawing surface.
struct Surface {
    size_t lines = MAX_LINES;
    // Max bytes the render may draw (0 = no limit).
    size_t max_bytes = 0;
    // Chained segments flow as one paragraph: the wire's trailing 0x0A is
    // replaced by a space and the next segment continues on the same line.
    // Only the panel flows -- the backlog redraws a box in one call, and a
    // derived surface takes one line at most.
    bool flows = false;
};

constexpr Surface DIALOGUE_PANEL { MAX_LINES, 0, true };
constexpr Surface HISTORY_WINDOW { HISTORY_MAX_LINES, 0, false };

// --- The surfaces nobody photographed ---------------------------------
// Script font-size changes produce unmeasured advances.  Width and line count
// are derived from the advance (both scale with the font cell) and only
// one-line values are substituted, so a re-break still lands on the panel.

// Text width in pixels (71 columns * 14.0 px).
constexpr size_t BOX_WIDTH_PX = BOX_COLUMNS * 14;

// Pixels below the first glyph band's top (y 590..709).
constexpr size_t PANEL_BELOW_FIRST_BAND_PX = 119;

// Backlog entry clip height below the first text row (measured on screen).
constexpr size_t HISTORY_BELOW_FIRST_TEXT_ROW_PX = 105;

// Glyph band height (ascender to descender), measured.  Script font sizes
// produce a taller band (~ 0.6 * advance); whichever is larger is used.
constexpr size_t GLYPH_BAND_PX = 25;

// Half-width columns at this advance (advance = 1.5 * point, column = 0.5 * point).
constexpr size_t ColumnsAtPitch(size_t pitch) {
    return pitch >= 3 ? BOX_WIDTH_PX * 3 / pitch : BOX_COLUMNS;
}

// Band height at this advance (max of measured and 3/5 * advance).
constexpr size_t BandAtPitch(size_t pitch) {
    return 3 * pitch / 5 > GLYPH_BAND_PX ? 3 * pitch / 5 : GLYPH_BAND_PX;
}

// Lines that fit in `room_px` at this advance (first line included).
constexpr size_t LinesInRoom(size_t room_px, size_t pitch) {
    return pitch == 0 || room_px <= BandAtPitch(pitch)
         ? 1
         : 1 + (room_px - BandAtPitch(pitch)) / pitch;
}

// Lines the panel holds at this advance.
constexpr size_t LinesAtPitch(size_t pitch) {
    return LinesInRoom(PANEL_BELOW_FIRST_BAND_PX, pitch);
}

static_assert(ColumnsAtPitch(42) == BOX_COLUMNS,
              "the derived width has to reproduce the measured one");
static_assert(LinesAtPitch(30) == MAX_LINES,
              "the derived line count has to reproduce the four-line box at "
              "the 30 px advance the hook substitutes");
static_assert(LinesAtPitch(42) == 3, "and the stock three-line box");
static_assert(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 0x28) == 3,
              "the backlog entry the engine's own 40 px advance draws");
static_assert(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 26) ==
                  HISTORY_MAX_LINES,
              "and the one the substituted advance draws -- the backlog's "
              "budget and its pitch move together");
static_assert(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 27) < HISTORY_MAX_LINES,
              "26 is the widest advance that keeps four rows inside the "
              "entry's clip; a wider one cuts the fourth in half");

// True when `cp932` fits one line of an unmeasured surface at `pitch` and
// that surface has a spare line behind it (prefix and trailing break discounted).
bool FitsUnmeasuredSurface(const std::string& cp932, size_t pitch);

static_assert(RECTS_PER_WIRE_BYTE >= 1, "the array holds at least one");
static_assert(HISTORY_MAX_LINES >= 1, "the history has to hold a line");

// --- Word wrap -------------------------------------------------------
struct WrappedText {
    std::string text;        // ready for the renderer, 0x0A between lines
    size_t      lines = 1;   // physical lines the engine will draw
    bool        truncated = false;  // content past MAX_LINES was dropped
    size_t      end_columns = 0;    // pen column after the last line's content
};

// Break `value` at spaces to fit the box, indenting continuation lines.
// Content past `max_lines` is dropped and the last line marked "...".
// `reserved_columns` accounts for the engine's chain indent.
// With `continue_line`, line one is the line the box left open:
// `start_columns` of it are already drawn, and it counts toward `lines`.
WrappedText WrapForMessageBox(const std::string& value,
                              size_t max_lines = MAX_LINES,
                              size_t reserved_columns = 0,
                              size_t start_columns = 0,
                              bool continue_line = false);

// Mark the last line as truncated ("..."), cutting at a space to fit.
// `start_columns` narrows a single continued line by what it already holds.
void MarkLastLineTruncated(std::string& text, size_t reserved_columns = 0,
                           size_t start_columns = 0);

// --- The box a chain of segments shares ------------------------------
// A box is filled by a chain of calls (first = no prefix, continuations
// carry 0x02..0x08, trailing 0x0A = more follows).  The budget tracks the
// lines each box has used and the pen on its open line, so the next
// segment gets what is left and can keep filling the line.
//
// The engine re-renders one segment many times (typewriter and repaint),
// always with the same wire bytes.  A repeat must reproduce its first
// layout byte for byte, or the box shows the text twice; the budget
// detects a repeat by the wire and replays the state the segment began
// with.

// What a box holds between two calls of its chain.
struct BoxState {
    size_t lines = 0;        // lines the box already holds
    size_t column = 0;       // pen column on the open line
    bool   line_open = false;  // the last counted line can still take text
};

class BoxLineBudget {
public:
    BoxLineBudget();
    ~BoxLineBudget();
    BoxLineBudget(const BoxLineBudget&) = delete;
    BoxLineBudget& operator=(const BoxLineBudget&) = delete;

    // State `box` already holds; resets when `continues` is false.  `wire`
    // is the call's full buffer: a continuation repeating the box's last
    // wire is a re-render, and gets the state that segment began with.
    BoxState Begin(uintptr_t box, bool continues, const char* wire);

    // Record the box's state once the segment is drawn and whether the box
    // stays open for more.
    void Commit(uintptr_t box, const BoxState& state, bool stays_open);

    // Forget every box.
    void Clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- Substitution ----------------------------------------------------

enum class SubstitutionStatus {
    Substituted,  // out.bytes holds the wrapped English
    EmptyKey,     // the buffer carried no text, only control bytes
    NotFound      // no row for this key; the engine draws its own text
};

struct Substitution {
    // Buffer for the renderer (empty on miss when the engine's own is fine).
    std::string bytes;
    // Trailing 0x0A/0x0D bytes the wire carried.  A flowing surface replaces
    // them with a joining space; every other path reattaches them.
    size_t      tail = 0;
    size_t      lines = 1;          // lines this segment adds to the box
    bool        truncated = false;  // content was dropped and marked
    bool        dropped = false;    // the box had no line left for it
    bool        continues = false;  // carried a control prefix
    bool        more_follows = false;  // trailing 0x0A: the box goes on
    size_t      box_lines_before = 0;  // lines the box already held
    size_t      box_lines_after = 0;   // lines it holds once this is drawn
};

// Build a substitution from one CP932 wire buffer: strip the control prefix
// and trailing breaks for lookup, wrap the English, reattach both.
// This form gives the value the whole box (no budget tracking).
SubstitutionStatus BuildSubstitution(const char* wire_text,
                                     const TranslationMap& translations,
                                     Substitution& out);

// Same, but budgeting against the box's remaining lines.
SubstitutionStatus BuildSubstitution(const char* wire_text,
                                     const TranslationMap& translations,
                                     BoxLineBudget& budget, uintptr_t box,
                                     Substitution& out);

// Same, against a specific surface (e.g. history window).
SubstitutionStatus BuildSubstitution(const char* wire_text,
                                     const TranslationMap& translations,
                                     BoxLineBudget& budget, uintptr_t box,
                                     const Surface& surface, Substitution& out);

}  // namespace translator_logic
