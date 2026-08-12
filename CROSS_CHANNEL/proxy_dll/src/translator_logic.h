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
