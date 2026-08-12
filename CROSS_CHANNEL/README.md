# CROSS†CHANNEL -FINAL COMPLETE-

English translation pipeline for **CROSS†CHANNEL -FINAL COMPLETE-** by FlyingShine/ensemble (WillPlus).

## Game Info

| Property | Value |
|----------|-------|
| Engine | WillPlus/AdvHD |
| Format | sn.bin = 4-byte LE32 header + LZSS compressed bytecode |
| JP Strings | 53,635 entries (28,787 dialogue + 24,848 narration) |
| Speakers | 75 in the glossary (太一, 見里, 美希, 霧, 冬子, 友貴, 桜庭, 曜子, 七香 ...) |
| Script File | `data/sn.bin` (~2 MB compressed, ~5.7 MB decompressed) |
| Font | `data/font48.xtx` (bitmap font atlas) |
| Assets | `.cpk` archives (bg, bgm, bgs, se, sys, vo) |

## Pipeline

The pipeline is C++ (`pipeline_cpp/`). Three steps take the game from Japanese
to English, and they are numbered in the order they run:

```
01_extract              → script_output/extracted_text.json   (LZSS decompress + opcode-based text extraction)
02_translate            → speaker gate, then script_output/translated_text.json +
                          translation_cache_anthropic.json, then translations.tsv
python build.py --deploy → xinput1_3.dll + translations.tsv into the game folder

00_run_all              → all three in one command
03_find_narrative_cg    → standalone: CG / UI images with Japanese text, translated and
                          re-rendered into script_output/narrative_patched/
```

`02_translate` carries the zero-token speaker gate and the runtime-table build
with it: the gate runs first, before an API key is read and before a file is
touched, and the table the DLL loads is written from the document the same run
produced. There is nothing to remember between translating and playing.

Translation is delivered at runtime by the proxy DLL reading
`translations.tsv` — the original `sn.bin` is never modified.

## Build

The game builds from two halves that cannot share one CMake configure:
`pipeline_cpp` is x64, `proxy_dll` is Win32 because it is injected into the
32-bit `cc.exe`. The root project drives both, and `build.py` is the front end:

```bat
python build.py                    REM build both halves
python build.py --test             REM build, then run every test of both halves
python build.py --install          REM stage build/install/bin + build/install/game
python build.py --deploy           REM ship xinput1_3.dll + translations.tsv to the game
python build.py --clean            REM delete the build tree
```

One build folder for the whole game: `build/pipeline` (x64), `build/proxy`
(Win32), `build/install` (`bin/` to run, `game/` to copy into the game).
`--test` runs 50 GoogleTest cases in the pipeline half and 59 in the proxy
half (labels UT / CT / SYSTEM). See `SakuraNoUta/BUILD_STRUCTURE.md` for the
layout and its conventions.

Dependencies come from Conan: boost (json / regex / program_options / Beast
headers), openssl (TLS for api.anthropic.com), stb (BMP+PNG codecs) and
freetype (the CG text render).

## Quick Start

```bat
set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

## Individual Steps

Every executable accepts `--help`, `--dir <project folder>` (defaults to this
directory, found by walking up from the executable's own location, and required
when the exe sits outside the project tree) and
`--game-dir <path>` (defaults to `C:\Games\CROSS_CHANNEL`).

```bat
01_extract.exe
02_translate.exe                      REM speaker gate, translate, build translations.tsv
02_translate.exe --test               REM smoke test: 1 API batch, then stop
python build.py --deploy              REM ship xinput1_3.dll + translations.tsv

03_find_narrative_cg.exe --scan-only  REM CG scan; --yes skips the confirm prompt

REM Recovery: if data/sn.bin ever gets corrupted, restore from the pristine backup:
copy /Y "%GAME_DIR%\data\sn.bin.bak" "%GAME_DIR%\data\sn.bin"
```

## Narrative CG (03) — Caveats

- **The patched BMPs are inspection-only.** CPK repacking is not implemented,
  so nothing `03_find_narrative_cg` renders reaches the game. The BMP writer
  still emits the canonical container (54-byte header, 24-bit BGR, bottom-up)
  so the files open anywhere.
- **There is no local OCR pre-filter**, so every image that passes the size
  threshold goes to Claude vision. `--scan-only` and the confirmation prompt
  are what keep the bill down; `narrative_scanned.json` makes a resumed scan
  skip everything already seen.
- **The committed `translation_cache_anthropic.json` is `indent=1`**, written
  by an older revision; `save_cache` writes `indent=2`. Do not treat the
  committed file as a formatting reference for the cache writer.

## Engine Notes

This game uses the **WillPlus/AdvHD** engine, identified by:
- `INSTCMPDIR=WillPlus` in SETUP.INI
- `sn.bin` compiled script archive
- `.cpk` CRI Middleware asset archives
- `font48.xtx` bitmap font texture
- Published by ensemble/guilty/sweet (WillPlus subsidiaries)

### sn.bin Format

The script archive uses **LZSS compression**:
- First 4 bytes: LE32 decompressed size (5,702,000)
- Remaining bytes: LZSS stream (ring=4096, init=0x20, start=0xFEE, min_match=3, LSB flags)
- Decompressed data contains bytecode with null-terminated CP932 text strings
- Speaker names are identified by opcode `47 0D 00` preceding the name string
- The proxy DLL hooks the engine's LZSS decompressor and patches the
  decompressed buffer in memory at runtime — the on-disk `sn.bin` is
  never modified.

### Known Challenges

1. **Byte budget**: When patching in the post-decompression buffer, translations must fit within the original Japanese byte length. Since English (ASCII) is typically fewer bytes than Japanese (CP932 multibyte), most translations fit. Strings that exceed the JP slot are handled by the render-time hook (Phase 2 in `architecture.md`) instead.

2. **Font**: `font48.xtx` is a bitmap font atlas (9 MB). English characters need to be present in the atlas, or the engine may fall back to system fonts.

3. **CPK archives**: Asset archives use CRI Middleware format. Tools like `cpk_unpack` or QuickBMS can extract them if needed for UI/menu translation.

4. **Fullwidth rendering**: The game renders half-width parentheses `()` as fullwidth `（）`. English text rendering behavior needs in-game testing.

## Backup Convention

- `data/sn.bin.bak` is a pristine copy of the original `data/sn.bin`.
  The translation pipeline never modifies `data/sn.bin`; the backup is
  kept only as a recovery target.

## Technical Notes: Reverse-Engineering sn.bin

The game's script archive proved challenging to crack. Here's how it was solved:

1. **Initial brute-force scan** of raw sn.bin found ~10K strings — mostly garbage with occasional Japanese fragments at ~30% density. This ruled out plaintext and simple XOR/rotation encryption.

2. **Entropy analysis** showed the data was compressed, not encrypted. Tested zlib, LZMA, and multiple LZSS parameter permutations at various offsets.

3. **LZSS breakthrough**: Standard LZSS (ring=4096, init=0xFEE, min_match=3, LSB flags) at offset 0 produced 5,702,036 bytes — tantalizingly close to the first 4 bytes read as LE32 (5,702,000), but text was garbled (「=1011 vs 」=120, massively unbalanced).

4. **The key insight**: Skipping the first 4 bytes (the LE32 size header) before decompressing gave **exactly** 5,702,000 bytes with perfectly balanced brackets (「=28,824 = 」=28,824) and clean Japanese text. Those 4 extra bytes at offset 0 were poisoning the LZSS ring buffer.

5. **Speaker identification**: Hex-dumping around known character names revealed opcode `47 0D 00` consistently precedes speaker name strings in the bytecode, enabling reliable speaker-dialogue pairing.

6. **Control byte stripping**: Some null-terminated segments include leading bytecode bytes before the actual text (e.g., `65 08 59 45 FF FF 18 02「……太一」`). A prefix-stripping pass recovers these as clean dialogue.

Total analysis: ~35 scripts, ~12 intermediate binary dumps, 3 sessions of iterative reverse engineering.

## Engine lessons

Four traps this engine set, each found the expensive way. The first is a
runtime technique; the other three are extractor defects whose only symptom is
"some lines render in raw Japanese" -- they cost days apiece to localise, so
they are written down with their diagnostics.

### Menu text extension via cursor swap

`sn.bin` stores each choice option inline in the bytecode as a
fixed-byte-budget record (`metadata` + `null-terminated text`), and the engine
parses + rasterises a whole menu in one function call. English does not fit
that budget, and polling the parsed pointer table is invisible — the atlas is
built before your poll fires. The fix is to intercept the parse itself.

(The technique is engine-agnostic and shingakkou's DLL uses it too; anything
that parses and draws a menu in one pass is a candidate.)

**Diagnostic.** If hovering options changes only the highlight, not the
text glyphs, the engine is caching an atlas. Intercept *before* parse.

**Why simpler attempts fail:**
- Overwriting text bytes corrupts the next record's metadata.
- Substituting in the option-pointer array runs after rasterise; invisible.
- Forcing input redraws only the highlight overlay.
- Stealing trailing null padding buys ≤ a few bytes.

**Technique — hook the parse fn's first cursor read:**
1. Save original cursor.
2. Walk original bytes the way the engine does; remember total opcode size.
3. Build a replacement buffer with the same record layout but the long
   translation in place of each option's text. Metadata copied verbatim.
4. Write replacement-buffer pointer into the cursor global.
5. Replace the function's return address (`[orig_ebp+4]` on x86) with a
   post-handler.
6. Let the engine parse + rasterise — it reads from your buffer.
7. Post-handler restores cursor to `orig + total_orig_size`.

Engine doesn't know it parsed something else; metadata, jump labels, and
the "selected option" ABI keep working through indirection.

**x86 outline:**
```
patch parse_fn + offset_of_first_cursor_read with JMP dispatcher

dispatcher (~40 B RWX):
    pushad / pushfd ; push EBP ; call C_pre_handler ; add esp, 4
    popfd / popad
    <stolen cursor-read instruction>      ; now reads OUR buffer ptr
    ret                                   ; → thunk's JMP fn+stolen_len

post_handler (~16 B RWX):
    pushad / pushfd ; call C_post_handler ; popfd / popad
    jmp [g_real_return_address]
```

For multiple parse-fn variants, add per-fn thunks between the patched JMP
and the shared dispatcher: each thunk does `CALL dispatcher;
JMP fn + stolen_len`.

**Gotchas:**
- **Header sizes lie.** Decompiler `cursor += 1` on a `ushort*` advances
  2 bytes. Off-by-two means option records overlap and one option
  silently disappears.
- **imm32 operands rebase at load time.** Validate hook sites against
  runtime imm32, not the static dump's.
- **Source text may already be translated.** If a decompress-time patcher
  pre-truncates and space-pads, your hook sees `"Go up to  "` not
  `"屋上に行く"`. Strip trailing padding and keep a reverse map
  `truncated_translation → full_translation`.
- **One parser handles multiple opcodes.** Hook every variant (or log
  which one fires for your target menu).
- **Reentrancy / threading.** Globals for cursor save / retaddr swap race
  if the function nests or runs off-thread. VN engines rarely do either.

### Don't strip non-control prefixes from extracted text

**TSV keys must match the bytes the engine emits at runtime, byte-for-byte.**

Tempting antipattern in extractors: "skip leading non-CJK chars to clean
up bytecode garbage." It works for the obvious case (raw control bytes
between speaker null and dialogue text) but ALSO eats legitimate content:

| Engine emits | Aggressive strip stores | Result |
|---|---|---|
| `※男心を鷲掴む…`  | `男心を鷲掴む…`  | runtime miss → raw JP |
| `Ａ定食は三時間目…` | `定食は三時間目…` | runtime miss → raw JP |
| `（佐倉）（遊紗）…`  | `佐倉）（遊紗）…`  | runtime miss → raw JP |
| `FLOWER'Sのもう片方` | `のもう片方`     | runtime miss → raw JP |

The fix: only strip when the leading run actually contains a control
byte (`\x00..\x1F`). Pure-printable prefixes pass through.

```python
def strip_control_prefix(text):
    first_kept = next((i for i, ch in enumerate(text)
                        if ch in '「」' or is_real_japanese(ch)
                        or ch in '…―～'), None)
    if first_kept is None or first_kept == 0:
        return text
    prefix = text[:first_kept]
    if any(ord(c) < 0x20 for c in prefix):   # bytecode artifact
        return text[first_kept:]
    return text                              # legitimate content — keep it
```

**Why this matters cross-game:** every WillPlus/AdvHD-style script
archive embeds CP932 strings inline, and most game extractors need a
similar clean-up step. If you copy-paste the strip from one game's
`01_extract.py` to the next, copy this version. Symptom of the old
version: random dialogue lines render in raw Japanese while neighbours
render fine — diff their TSV keys against the engine's runtime bytes
(capture via a dialogue-hook MISS log) and you'll see a single
decorator byte difference at the front.

**Don't paper over it at the runtime DLL.** A "strip-and-retry" lookup
fallback in the proxy DLL is OK as defense-in-depth, but the data fix
is upstream — fix the extractor, re-key `translated_text.json` by
`offset` (no LLM calls needed), rebuild the TSV. The re-key is a
~30-line one-shot: load OLD `translated_text.json` and NEW
`extracted_text.json`, build `{offset → translated}` from OLD, walk
NEW entries copying `translated` over by offset, write back. Same EN,
new (un-stripped) JP keys.

### Match thresholds between extract and classify stages

**Two-stage extractors silently drop content when the second stage is
stricter than the first.**

Typical VN extractor shape:
```
extract_strings(data) → list[(offset, text, is_speaker)]   # filter A
classify_and_pair(strings, data) → list[entry]             # filter B
```
If filter A is `count_japanese >= 2` and filter B is
`count_japanese >= 3`, every 2-char Japanese string that A captures
gets thrown away by B with no warning. The strings exist in memory,
they just never reach `extracted_text.json`.

**Symptom.** Same as §6 in the in-game UI: short choice options like
`迫る`, `帰る`, `屋上`, `部室` render as raw Japanese while their longer
neighbours (`さらに迫る`, `屋上に行く`) translate fine. But unlike §6 the
TSV doesn't even *have* a key for the missing form — `grep` it and
you'll get nothing. Diagnostic: count occurrences of the short JP in
`extracted_text.json` vs in the bytecode (decompress sn.bin and grep
the CP932 bytes); a delta means classify dropped them.

**Why the threshold drift happens.** `extract_strings` is written
liberally to feed every plausible candidate into the pipeline.
`classify_and_pair` then tightens to filter bytecode artifacts that
decode as 2 random Japanese chars (e.g. `怙0` from a `\xf8\xf2`
artifact). The 2→3 step works for noise but also kills 2-char real
choices. Lowering classify to `>= 2` unconditionally lets noise back
in (~1500 garbage entries in CROSS†CHANNEL).

**Fix — targeted exception keyed on opcode preamble.** Choice options
in WillPlus/AdvHD bytecode are preceded by a fixed 6-byte signature:
```
ff ff XX YY 00 00 <text> 00
```
where `XX YY` is the LE16 jump label. All choice options in the
archive share this shape — `さらに迫る`, `迫る`, `ボケる`, `帰る`,
`屋上`, `部室`, `屋上に行く`, … every one. So classify's rule becomes:

```python
elif has_japanese(t) and (
    count_japanese(t) >= 3
    or (count_japanese(t) >= 2 and is_choice_option_at(offset, data))
):
    ...

def is_choice_option_at(offset, data):
    if offset < 6: return False
    return (data[offset-6:offset-4] == b'\xff\xff'
            and data[offset-2:offset] == b'\x00\x00')
```

In CROSS†CHANNEL this recovered exactly 18 dropped choices with zero
false positives — the preamble is specific enough that no narration
or dialogue accidentally matches.

**Cross-game.** If a new WillPlus/AdvHD port shows raw JP for short
choice menus only, suspect this first. The byte signature is engine-
level, not game-level — it should match in any AdvHD `sn.bin`. For
non-WillPlus engines, the *pattern* is what transfers: find a fixed
opcode preamble for the dropped record class and gate the threshold
relax on it.

**Don't just lower the threshold.** Removing the floor entirely is
tempting (one-character fix) but fills the TSV with bytecode noise
that costs LLM tokens and clutters the cache. The preamble check is a
1-function add and keeps the noise floor where it was.

### Don't skip past the next null on a CP932 decode failure

**A null-walking string extractor that jumps to the next `\x00` whenever
strict decode fails will silently drop every valid string sitting inside
a chunk that has a non-CP932 prefix byte.**

Common extractor shape for null-terminated CP932 archives:

```python
while pos < fsize:
    if data[pos] == 0x00: pos += 1; continue
    end = data.find(b'\x00', pos)
    raw = data[pos:end]
    try:
        text = raw.decode('cp932', errors='strict')
        # ... filters / append ...
    except UnicodeDecodeError:
        pass
    pos = end + 1                       # ← the bug
```

The loop assumes "one null-delimited region = one string candidate". But
many script formats interleave opcode operand bytes between the previous
`\x00` and the next text payload, so a region looks like:

```
<opcode operand bytes that aren't valid CP932>  <text bytes>  \x00
^ pos starts here                                ^ valid 「dialogue」 lives here
```

If the operand bytes happen to be invalid CP932 (e.g. lead byte `0x86`
followed by trail `0x3a` — `0x3a` is ASCII `:`, not a valid CP932 trail),
strict decode fails at position 0 of the region. The loop's `pos = end + 1`
then jumps **past the next `\x00`**, losing the embedded text entirely.

**Diagnostic.** If specific dialogue/narration lines render as raw JP in
game and are also missing from `extracted_text.json` *and* a substring
search of decompressed bytecode shows the bytes ARE present, the walker
dropped them. Reproduce by:

1. `data.rfind(b'\x00', 0, target_offset) + 1` → walker's chunk start.
2. `data.find(b'\x00', chunk_start)` → walker's chunk end.
3. `data[chunk_start:chunk_end].decode('cp932', errors='strict')` →
   raises `UnicodeDecodeError` at position 0 (or early).

**Fix.** On decode failure, advance one byte and retry, instead of
jumping past the next `\x00`. The loop will naturally find the first
valid CP932 lead inside the chunk:

```python
try:
    text = raw.decode('cp932', errors='strict')
    # ... filters / append ...
    pos = end + 1                       # success → skip whole chunk
    continue
except UnicodeDecodeError:
    pos += 1                            # failure → slide one byte
    continue
```

After the slide, when the walker eventually lands on the real text
start, decode succeeds and the existing `strip_control_prefix` rule
(§6) cleans any small residual control-byte prefix that survived.

**Cost.** Extracted-string count typically grows ~50-70% (mostly from
text inside opcode-prefixed regions that had been silently dropped).
The classify/threshold stage filters the noise, so `extracted_text.json`
grows by a smaller factor — but the recovered strings include real
dialogue that was missing.

**Why not just use `errors='ignore'` or `'replace'`.** Ignore-mode
shifts string indices (the recorded `offset` no longer matches runtime
bytes — breaks the §6 byte-form-key invariant), and replace-mode
inserts U+FFFD which then fails the JP-density filter. Slide-and-retry
is the only fix that preserves offsets and keeps `strict` decode as
the validity gate.

**Cross-game.** This pattern is engine-agnostic — any null-terminated
CP932 archive walked one chunk at a time is vulnerable. Symptom is
nearly identical to §6 (lines render as raw JP) but the upstream cause
is different: §6 is an over-aggressive prefix strip *after* successful
decode; §9 is a chunk-skip *on* decode failure. The TSV-grep + bytecode-
grep diagnostic distinguishes them — §6 leaves a key in the TSV that's
just shaped wrong; §9 leaves no key at all.
