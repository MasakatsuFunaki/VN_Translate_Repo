// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// translator_logic.h
//
// Pure (OS-independent) logic that powers the shingakkou runtime
// translator. Everything here is free of Windows / wide-string / file
// IO so the same object file can be linked into both the proxy DLL
// and the gtest binary.
//
// The counterpart translator.cpp wires these primitives up to the
// real engine: it opens translation_table.tsv with _wfopen, converts
// parsed UTF-8 entries to UTF-16LE with MultiByteToWideChar, installs
// the entry/post-process trampoline around FUN_0040e810, and walks
// the decrypted script buffer scanning for FF 01 80 text markers.
//
// What lives here is the byte-level scaffolding that has no
// Windows dependencies:
//   * TSV escape tokens ({TAB}/{CR}/{LF} -- note this game uses
//     CURLY-BRACE tokens, NOT backslash escapes like the other
//     projects in this tree).
//   * UTF-8 BOM detection.
//   * TSV parsing into a UTF-8 map (caller does wide conversion).
//   * Big-endian 16/24-bit pack/unpack for the script offset-table
//     fixup and the footer pointer fixup.
//   * Cumulative-delta lookup for adjusting the original script's
//     3-byte offset-table entries after variable-length string
//     replacements have shifted everything.
//
// None of this depends on DDSystem/PIL internals, so the gtest
// suite can pin every one of these pieces down without the game
// running.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace translator_logic {

using Utf8TranslationMap = std::unordered_map<std::string, std::string>;

// --- String helpers ------------------------------------------------------

// Undo shingakkou's TSV escape tokens: {TAB} -> \t, {CR} -> \r,
// {LF} -> \n. Applied in-place.
//
// This is the ONLY unescape style shingakkou's TSV understands.
// Backslash escapes (\t, \n, ...) are NOT recognised because the
// game's script text legitimately contains literal backslashes in a
// handful of entries (path-looking strings) and backslash escaping
// would eat them. Unknown curly-brace sequences are preserved
// verbatim.
void UnescapeInPlace(std::string& s);

// --- BOM / TSV parsing ---------------------------------------------------

// If the buffer at `data` starts with the UTF-8 BOM (EF BB BF) and is
// at least 3 bytes long, returns 3; otherwise returns 0. Caller adds
// this to their read cursor to skip the BOM. Bug here would silently
// glue three junk bytes to the first TSV key.
std::size_t Utf8BomLen(const char* data, std::size_t len);

// Parse a TSV buffer in memory into a UTF-8 translation map. Each
// line is "jp\ten\n" (or \r\n). Unescapes both sides via
// UnescapeInPlace; drops empty entries. Returns the number of
// entries inserted; `out` is appended to, not cleared.
//
// Takes a view as (ptr, len). Accepts both \n and \r\n line endings.
// Does NOT strip the UTF-8 BOM -- call Utf8BomLen() first and advance
// the pointer/length to skip it. Last duplicate wins (matches the
// pre-extraction behavior so nothing shifts).
//
// The caller is responsible for UTF-8 -> UTF-16LE conversion before
// comparing against the script's native UTF-16LE text (shingakkou's
// script buffer is wide).
int ParseTsvBuffer(const char* data,
                   std::size_t len,
                   Utf8TranslationMap& out);

// --- Big-endian pack/unpack ---------------------------------------------
// Used for the 3-byte-BE offset-table entries at the script tail and
// the header table pointer (a 3-byte BE value at decrypted_data + 0x16
// equal to table_start - 5). The 16-bit helpers are kept for
// completeness. Endianness here is independent of the host -- the
// script file format fixes it.

std::uint16_t ReadBE16(const unsigned char* p);
void          WriteBE16(unsigned char* p, std::uint16_t v);

// 24-bit big-endian values are packed/unpacked from/to a uint32.
// The top 8 bits of the return value are always zero.
std::uint32_t ReadBE24(const unsigned char* p);
void          WriteBE24(unsigned char* p, std::uint32_t v);

// --- Cumulative-delta offset fixup --------------------------------------
// After each variable-length string replacement the rest of the
// script buffer shifts by `delta` bytes. Every entry in the tail
// offset-table that points PAST the replaced string must be nudged
// by the cumulative sum of all preceding deltas.
//
// `table` holds (threshold, cumulative_delta) in threshold-ascending
// order. For an original offset `val`, the applied delta is the
// cum_delta of the LAST entry whose threshold <= val (i.e. all
// replacements that lie strictly before `val`). Offsets before the
// first replacement get delta = 0.

struct CumulativeDelta {
    std::uint32_t threshold;
    int           cum_delta;
};

int ApplyCumulativeDelta(std::uint32_t offset,
                         const std::vector<CumulativeDelta>& table);

// --- Tail offset-table location -----------------------------------------
// Every decrypted script ends with a table of 3-byte big-endian offsets,
// one per text record, in ascending order. Each entry points at an
// 'FF 01 80' string marker (the marker sits at entry_value + 3), and the
// table is terminated by a single trailing 0xFF byte. (A 3-byte BE
// pointer to table_start - 5 also lives in the header at offset 0x16.)
//
// FindOffsetTable locates the table by structure: from the last entry
// (just before the terminator) it walks backwards in 3-byte steps while
// each entry is a valid, in-range, ascending offset that points at an
// 'FF 01 80' marker.
//
// This replaces an earlier heuristic that walked back while the entry's
// high byte was 0x00 -- i.e. it assumed every offset fit in 16 bits.
// On every script larger than 64KB the deepest offsets need a non-zero
// third byte, so that walk stopped at the very first entry and located
// NO table at all, leaving the whole table (and thus every save taken in
// that script) unfixed. The structural walk handles 24-bit offsets.
//
// Returns {start_byte_offset, entry_count}; {0, 0} when no table is
// found (buffer too small, or not 0xFF-terminated).
struct OffsetTable {
    std::size_t start;
    int         count;
};

OffsetTable FindOffsetTable(const unsigned char* data, std::size_t dataSize);

// --- Choice-destination table relocation ---------------------------------
// Choice/select menus embed a jump table of absolute byte offsets, one per
// branch, laid out as:
//
//     2A 10 FF 00 <count:u8> <count x 3-byte big-endian offset>
//
// Each offset is an absolute position in the *original* (pre-replacement)
// script that the engine seeks to when that option is chosen. The tail
// offset-table fixup does NOT touch these -- they sit in the bytecode body,
// not the tail -- so after variable-length text replacement shifts the
// buffer, selecting a choice seeks to a stale offset, lands mid-string, and
// the engine interprets string bytes as opcodes (observed crash: a bogus
// `rand() % 0` divide-by-zero in the expression VM).
//
// RelocateChoiceTables scans the ORIGINAL buffer for these tables, and for
// every one whose entries all point at a code-like boundary (an '03 0d xx'
// statement preamble or a '00 3f xx' expression token -- the only shapes the
// real tables target), rewrites each entry in the NEW buffer to its
// post-replacement position via the same cumulative-delta table used for the
// tail offset-table. The code-like predicate plus the exact 4-byte anchor
// reject coincidental byte matches (2A 10 / FF 00 cannot occur inside the
// ASCII-range UTF-16LE English replacements).
//
// `cumulative` is the same (threshold, cum_delta) table passed to
// ApplyCumulativeDelta for the tail fixup. Returns the number of entries
// relocated.
int RelocateChoiceTables(const unsigned char* data, std::size_t dataSize,
                         unsigned char* newData, std::size_t newDataSize,
                         const std::vector<CumulativeDelta>& cumulative);

// --- Message-window line-spacing patch ------------------------------------
// Dialogue rendering is two-stage, all in DDW bytecode (Bootup.dat), not
// EXE code. Bootup entry 7 (message window, small-font branch):
//   1. opcode 0x6A mode 1 wraps the message into string vars 251+ (lines
//      are 1-based; no line cap) and mode 2 RENDERS line i into the
//      offscreen text surface (layer 130, var[0x2D4]) at
//          src_y = 2 + i * (var[0x2D0]+5)        = 2 + 26*i
//   2. a per-line op16 blit copies surface row band [26*i, 26*i+cell]
//      to the screen at
//          dst_y = var[0x2D3] + i * var[0x2D1] + ...
// So the VISIBLE spacing is var[0x2D1] (this patch), but the surface row
// step is var[0x2D0]+5 = 26, fixed. Patching var[0x2D1] alone moves
// lines closer on screen; it can never reveal a 4th line, because that
// line was clipped at stage 1 by the surface height (104px -- see
// PatchTextSurfaceHeight below). Both patches together fix the bug:
// surface height makes stage 1 emit the extra lines, spacing 20 keeps
// 4 blitted lines inside the 90px window at stage 2.
//
// The assignment statement encodes as the 6-byte sequence
//     3F 02 D1   0D 1E   40        var[0x2D1] = imm8 30, store
// which occurs once in each of Bootup entries 0 (boot default), 7
// (message window, small-font branch), 11 and 12 -- and nowhere in any
// of the 91 game scripts. Patching the imm8 in place keeps the statement
// length unchanged, so no offsets shift and no relocation is needed.
// The large-font branch (0D 2A = 42) is left alone: 4 lines of size-32
// glyphs cannot fit the box at any spacing.
//
// Returns the number of assignments patched (0 for game scripts).
int PatchMessageLineSpacing(unsigned char* data, std::size_t dataSize,
                            unsigned char newSpacing);

// --- Message text-surface height patch -------------------------------------
// Bootup entry 0 creates the offscreen message text surface (layer 130)
// 550 x 104 px:
//     15 | 0E 00 82 | 00 | 00 | 0E 02 22 04 60 | 0D 1A 04 68 | (args
//     FF-terminated)            w = 546+4        h = 26 * 4
// The height expression is literally "4 line rows of 26px" -- sized for
// JP text, which never wraps past 3 lines (1-based rows 1..3 end at
// y = 80+cell <= 104). English regularly needs a 4th line; its surface
// row starts at y = 2 + 26*4 = 106, entirely below the 104px surface, so
// the mode-2 render clips it and the screen blit copies blank pixels --
// the line vanishes no matter what spacing/window patches do downstream.
// (Diagnosed at runtime: the 0x6A WrapDiag hook logged line count 5 and
// per-line measured widths 536/536/536/72 for a message that displayed
// only 3 lines.)
//
// The fix rewrites the imm8 multiplier (the byte before the 68 mul op)
// from 4 to `newLineCapacity` rows. 7 rows = 182px covers 6 wrapped
// lines (worst translation needs 5). The 19-byte create sequence occurs
// exactly once across Bootup.dat and all 91 game scripts; in-place imm8
// edit, no offsets shift. The per-message clear and the per-line blits
// take full-surface / computed rects, so no other 104 dependency exists.
//
// Returns the number of create calls patched (1 for Bootup entry 0,
// 0 everywhere else).
int PatchTextSurfaceHeight(unsigned char* data, std::size_t dataSize,
                           unsigned char newLineCapacity);

// --- English word wrap for the message window -------------------------------
// The engine wraps messages at CHARACTER granularity: the 0x6A mode-1
// handler accumulates per-char widths (measure fn RVA 0x2860) against the
// wrap budget global [0x45E734] (536px for the message window) and breaks
// the moment a char would overflow -- fine for Japanese, but it slices
// English words mid-letter ("his yo / uth", "Fath / er's").
//
// The same handler treats '\r' (0x000D) as an explicit hard line break
// (flushes the current line, consumes the CR), so the fix is to pre-wrap
// the English replacement in the DLL: WordWrapMessage walks the string
// with the engine's exact metric (EngineCharWidthPx -- 8px halfwidth after
// the proportionalizer patches, 21px full cell otherwise) and swaps the
// last space of an overflowing line for '\r'. The engine then never
// reaches its own break point. Swapping ' ' for '\r' is one-for-one, so
// the replacement's byte length -- and every offset relocation downstream
// -- is unchanged.
//
// Words longer than the whole budget (one 121-char onomatopoeia exists)
// are left to the engine's char break. Pre-existing '\r' are respected.
// Offline corpus check (52k strings): no message grows past 4 wrapped
// lines except the one pre-existing 5-liner, so the surface/window fits
// are unaffected. Apply to dialogue/narration records (preByte 0x36)
// only -- choices and misc strings render in other widgets.
//
// WordWrapMessage returns the number of breaks inserted.
int EngineCharWidthPx(wchar_t c);
int WordWrapMessage(std::wstring& text, int budgetPx);

// --- Message-window background-refresh band patch ---------------------------
// At the start of every message, Bootup entry 7 erases the previous text by
// re-stamping the window background art over it (op16 blit, layer 131 ->
// screen layer 1, opaque mode 255):
//
//     rows var[0x2D3]+36 .. var[0x2D3]+126   (y = +36, h = 86+4 = 90)
//
// This stamp is the ONLY opaque eraser in the whole message path -- the
// name band is covered by the name-ribbon graphic, and every text blit is
// colour-keyed (mode 254), so it adds glyph pixels without ever clearing.
//
// Line i's text band lands at  var[0x2D3] + i*var[0x2D1] + var[0x2D7],
// i.e. line 1 at +spacing+9. The stock geometry is exactly tight: spacing
// 30 puts line 1 at +39, just inside the +36 stamp top. With the spacing
// patched to 20 (PatchMessageLineSpacing), line 1 moves up to +29 and its
// top 7 rows stick out ABOVE the stamp -- old glyph tops accumulate there
// across messages (full window width, first line only: lines 2+ stay
// inside the stamped band).
//
// The fix re-anchors the stamp top at line 1's new position and grows the
// height by the same amount so the bottom edge stays at +126:
//     src y imm8 36 -> newTopOffset      (pattern byte 13)
//     h   imm8 86 -> 122 - newTopOffset  (pattern byte 23; expr adds 4)
//     dst y imm8 36 -> newTopOffset      (pattern byte 37)
// The 40-byte blit statement occurs exactly once game-wide (Bootup entry
// 7 offset 0x14F; nothing in entries 0-6/8+ or the 91 game scripts).
// In-place imm8 edits, no offsets shift. Patching bytecode constants
// (rather than the var[0x2D7] boot initialiser) keeps the fix immune to
// variable state restored from pre-fix save files.
//
// Returns the number of blit statements patched (1 for Bootup entry 7,
// 0 everywhere else).
int PatchMessageWindowRefreshTop(unsigned char* data, std::size_t dataSize,
                                 unsigned char newTopOffset);

}  // namespace translator_logic
