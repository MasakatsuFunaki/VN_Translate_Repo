// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// translator_logic.h
//
// Pure (OS-independent) translation logic for the SakuraNoUta runtime
// translator.  Free of Windows / GDI dependencies so the same object
// file links into both the proxy DLL and the gtest binary.
//
// translations.tsv is UTF-8 (the upstream pipeline writes UTF-8), but
// the BGI engine hands us CP932 byte buffers via the GDI text APIs,
// so the on-disk side is converted to CP932 at load time and lookups
// happen on the wire bytes.  See LoadTsvFile().

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

// --- Choice-text helpers --------------------------------------------

// True when JP is in the byte-range typical of WillPlus/AdvHD choice
// option strings (10..14 CP932 bytes = 5..7 chars). Used both by the
// in-place patcher's slot-fitting decision AND by the entry hook's
// reverse-lookup map builder.
bool IsChoiceLikeJp(const std::string& jp);

// Word-aware truncation: cut at the last space within `slot` bytes if
// possible, otherwise hard-clip at `slot`. Empty result iff slot == 0.
std::string FitToSlot(const std::string& en, std::size_t slot);

// Bytecode layout bounds for op 0x31 / 0x32 choice records in sn.bin.
constexpr std::size_t kChoiceHeaderBytes         = 6;
constexpr std::size_t kChoiceOptionPreambleBytes = 10;
constexpr std::size_t kMaxChoiceOptions          = 16;
constexpr std::size_t kMaxChoiceOptionSrcBytes   = 512;
constexpr std::size_t kMaxChoiceOptionOutBytes   = 256;
constexpr std::size_t kDefaultMaxChoiceInBytes   = 16384;

// Backlog ring-buffer slot text layout for op 0x86 (FUN_0083efb0, RVA 0x2EFB0):
//   slot base 0x06C2B880, text at +0x008, engine state snapshot at +0x200
//   -> 0x1F8 = 504 bytes (max 503 payload bytes + NUL terminator).
// Backlog line renderer FUN_0081df60 (RVA 0x0DF60) parses "%N" line breaks
// and otherwise hard-splits mid-word at column 60 (0x3C).
constexpr std::size_t kBacklogSlotTextCapacity   = 504;
constexpr std::size_t kBacklogWrapCols           = 58;

// Optional reverse-prefix lookup callback used by BuildChoiceBuffer when an
// earlier in-place script patcher overwrote a choice option with truncated EN.
using ChoicePrefixLookupFn = bool (*)(const std::string& maybe_truncated_en,
                                      std::string& full_en_out);

// Parse a Cross-Channel choice opcode record (op 0x31 / 0x32) starting at
// `cursor` (bounded by `max_in_bytes`) and emit a structurally identical
// choice record with translated option strings into `out_buf[0..out_capacity)`.
//
// Bytecode layout at `cursor` (after opcode byte consumption):
//   [0..1]   2-byte preamble (0x80 0x80)
//   [2..3]   uint16 LE option count (1..kMaxChoiceOptions)
//   [4..5]   uint16 LE default option index
//   Per option (`count` records):
//     [0..9] 10-byte option preamble + jump/flag metadata (copied verbatim)
//     [10..] NUL-terminated CP932/ASCII option text, scanned up to
//            kMaxChoiceOptionSrcBytes (512 bytes)
//
// Each option resolves via LookupWithStrippedPrefix(), then trailing-space
// trim + LookupWithStrippedPrefix() + optional `prefix_lookup`, and is
// clamped via FitToSlot() to `kMaxChoiceOptionOutBytes - 1` (255B + NUL).
//
// Returns the total byte length consumed from `cursor` (`> 0`) so the caller's
// post-hook advances the script cursor past the original JP record; returns 0
// on invalid arguments, out-of-range count, unterminated string, or insufficient
// `out_capacity`.
std::size_t BuildChoiceBuffer(const unsigned char* cursor,
                              std::size_t max_in_bytes,
                              unsigned char* out_buf,
                              std::size_t out_capacity,
                              const TranslationMap& translations,
                              ChoicePrefixLookupFn prefix_lookup = nullptr);

std::size_t BuildChoiceBuffer(const unsigned char* cursor,
                              unsigned char* out_buf,
                              std::size_t out_capacity,
                              const TranslationMap& translations,
                              ChoicePrefixLookupFn prefix_lookup = nullptr);

// Seed `out` with built-in CP932 -> EN translations for sn.bin strings that
// are absent from or lose semantic prefixes in translations.tsv:
//   * script[0] op 0x32 scenario-launcher options & op 0x45 controller legend
//   * all 35 CASE chapter cards & Bad-End failure cards ("ＣＡＳＥ　１　失敗"...)
//   * all 31 op 0x86 silent-backlog lines (Miki route)
//   * stripped-prefix lines where stripping drops meaningful leading tokens
//     ("Ａ定食は…", "FLOWER'S…", "２リットルの天然水。", "†が閃く。", ...)
//   * short/unextracted choice & speaker/decorator safety entries
//
// Inserts via `emplace` so entries already present in `out` (loaded from
// translations.tsv) are never overwritten, while literal unstripped CP932 keys
// take priority over LookupWithStrippedPrefix()'s stripped fallback.
// Returns the number of newly inserted entries.
int SeedScriptMenuTranslations(TranslationMap& out);

// Look up `jp_cp932` in `translations` (via LookupWithStrippedPrefix),
// word-wrap lines at `wrap_cols` using Cross-Channel's "%N" line-break token,
// and clamp to `slot_capacity - 1` payload bytes so the NUL-terminated output
// fits cleanly inside the 504-byte op 0x86 backlog text field.
// Returns true on substitution; returns false without mutating `out_formatted`
// on lookup miss or invalid `slot_capacity` (< 2).
bool FormatBacklogSlotText(const std::string& jp_cp932,
                           const TranslationMap& translations,
                           std::string& out_formatted,
                           std::size_t wrap_cols = kBacklogWrapCols,
                           std::size_t slot_capacity = kBacklogSlotTextCapacity);

// --- Lookup with leading-decorator fallback --------------------------

// Look up `jp` in `translations`. If absent, retry after walking past
// any leading bytes the TSV build pipeline would have stripped (the
// extractor's strip_control_prefix keeps only 「」, …, ―, ～, and CJK
// chars — everything else at the front gets stripped).  Mirrors that
// strip exactly so engine-runtime forms like "A定食…" or "※男心を…"
// still resolve against TSV keys "定食…" / "男心を…".  Returns pointer
// to the EN string in the map, or nullptr if neither variant hits. The
// pointer is owned by `translations` and is valid as long as the map
// doesn't mutate.
//
// Why a fallback rather than always-strip: a TSV key MAY legitimately
// start with a decorator byte sequence (rare but possible). We try the
// literal key first to preserve correct lookups, only falling back when
// the literal misses.
const std::string* LookupWithStrippedPrefix(const TranslationMap& translations,
                                             const std::string& jp);

// --- In-place patcher ------------------------------------------------

struct PatchStats {
    int hits     = 0;
    int too_long = 0;
};

// Substring-search every translation in `translations` against
// [begin, end) and overwrite each STANDALONE-NULL-TERMINATED match with
// EN. "Standalone" means the byte BEFORE the match is null (or
// start-of-buffer) AND the byte AFTER the match is null — both required.
// Relaxing either side would corrupt longer JP strings that contain
// shorter JP as substring/suffix (sn.bin has plenty of both).
//
// Slot grows by stealing trailing null padding when EN > JP byte width.
// Choice-likely JP whose EN exceeds the writable slot is auto-truncated
// (FitToSlot). Dialogue-length JP whose EN exceeds the slot is skipped
// — the render-time text hook handles those via DLL buffer redirection,
// no need to clip in place.
PatchStats PatchTranslationsInPlace(unsigned char* begin, unsigned char* end,
                                     const TranslationMap& translations);

// --- TSV parsing -----------------------------------------------------

// Undo the four escape sequences the table builder emits:
//   \\  \r  \n  \t
// In place; cheap enough not to need a return value.
void UnescapeInPlace(std::string& s);

// Parse a UTF-8 TSV buffer into an unordered_map.  Each non-empty line
// must be "<jp>\t<en>\n" (or \r\n).  Both sides are unescaped, then
// the key (JP) is converted UTF-8 -> CP932 because the engine's text
// drawing APIs pass CP932 bytes.  The value (EN) stays UTF-8 since
// we re-encode it CP932 at draw time too (ASCII passes through
// unchanged either way; emitted EN is ASCII-only by policy).
//
// Returns the number of entries inserted.  `out` is appended to.
int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out);

// Convenience wrapper that opens a file and forwards to ParseTsvBuffer.
// Returns true iff at least one entry was parsed.
bool LoadTsvFile(const std::string& path, TranslationMap& out);

// --- CP932 helpers ---------------------------------------------------

// True if the byte is a valid CP932 lead byte (start of a 2-byte glyph).
bool IsCp932LeadByte(unsigned char b);

// True if the buffer contains at least one CP932 lead byte (i.e. it
// has Japanese content as opposed to pure ASCII UI / filename text).
bool HasJPLeadByte(const char* data, size_t len);

// Convert a UTF-8 string to CP932.  Bytes that have no CP932
// representation are dropped (replaced by '?').  Pure ASCII passes
// through unchanged.  Returns the converted string.  Implemented in
// terms of std::wstring on Windows (uses MultiByteToWideChar /
// WideCharToMultiByte); on non-Windows builds (the gtest harness
// running on Linux dev hosts) this falls back to identity for ASCII
// inputs and assumes the input is already CP932 for non-ASCII -- the
// gtest fixtures only exercise the parser side, never the conversion.
std::string Utf8ToCp932(const std::string& utf8);

// Reverse direction.  Used only when we want to log a JP key that came
// off the wire so it shows up readable in proxy_log.txt.  Lossless
// for valid CP932 input.
std::string Cp932ToUtf8(const std::string& cp932);

}  // namespace translator_logic
