# CROSS†CHANNEL -FINAL COMPLETE-

| | |
|---|---|
| **Engine** | WillPlus/AdvHD |
| **Format** | `sn.bin` = 4-byte LE32 header + LZSS compressed bytecode (CP932) |
| **JP Strings** | 53,635 (28,787 dialogue + 24,848 narration) |
| **Speakers** | 75 (太一, 見里, 美希, 霧, 冬子, 友貴, 桜庭, 曜子, 七香 …) |
| **Delivery** | `xinput1_3.dll` — hooks LZSS decompressor, patches buffer in memory. `sn.bin` is never modified. |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\CROSS_CHANNEL`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write Python scripts that scan EXE bytes for x86 opcodes.

## Pipeline

```
01_extract          → extracted_text.json   (LZSS decompress + opcode-based extraction)
02_translate        → speaker gate → translated_text.json + translations.tsv
00_run_all          → all three in one shot
03_find_narrative_cg → CG/UI images with JP text → narrative_patched/ (inspection-only; CPK repack not implemented)
```

`python build.py --deploy` copies `xinput1_3.dll` + `translations.tsv` into the game.

## Build

```bat
python build.py                    # both halves (pipeline x64, proxy Win32 for cc.exe)
python build.py --test --install   # build + 50 pipeline / 59 proxy GoogleTest cases + stage
python build.py --deploy           # ship to game
python build.py --clean

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

Every app takes `--dir <project>`, `--game-dir <path>` (default `C:\Games\CROSS_CHANNEL`).

## Narrative CG (03) — caveats

- Patched BMPs are inspection-only — CPK repacking is not implemented.
- No local OCR pre-filter; every image above the size threshold goes to Claude vision.
  `--scan-only` and the confirmation prompt keep the bill down; `narrative_scanned.json` makes a resumed scan skip everything already seen.

## sn.bin format

- First 4 bytes: LE32 decompressed size (5,702,000)
- LZSS params: `ring=4096, init=0x20, start=0xFEE, min_match=3, LSB flags`
- Decompressed data: bytecode with null-terminated CP932 text strings
- Speaker opcode: `47 0D 00` precedes the name string
- Backup: `data/sn.bin.bak` is a pristine copy (recovery target only)

## Engine identification

`INSTCMPDIR=WillPlus` in SETUP.INI, `sn.bin` script archive, `.cpk` CRI Middleware
asset archives, `font48.xtx` bitmap font atlas (9 MB), published by ensemble/guilty/sweet.

---

## Engine lessons

Four traps, each found the expensive way. Lessons §1 and §2 are runtime techniques; §3 and
§4 are extractor defects whose only symptom is "some lines render in raw Japanese."

### §1. Menu text extension via cursor swap

`sn.bin` stores each choice option inline as a fixed-byte-budget record. English doesn't fit,
and polling the parsed pointer table runs after rasterise (invisible — the engine caches an
atlas). Fix: intercept the parse itself.

**Technique — hook the parse fn's first cursor read:**
1. Save original cursor.
2. Walk original bytes the way the engine does; remember total opcode size.
3. Build a replacement buffer with the same record layout but the long translation in each option's text slot. Metadata copied verbatim.
4. Write replacement-buffer pointer into the cursor global.
5. Replace the function's return address (`[orig_ebp+4]` on x86) with a post-handler.
6. Let the engine parse + rasterise from your buffer.
7. Post-handler restores cursor to `orig + total_orig_size`.

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

For multiple parse-fn variants, add per-fn thunks: each thunk does
`CALL dispatcher; JMP fn + stolen_len`.

**Gotchas:**
- Decompiler `cursor += 1` on a `ushort*` advances 2 bytes — off-by-two means option records overlap and one option silently disappears.
- `imm32` operands rebase at load time — validate hook sites against runtime imm32, not the static dump's.
- Source text may already be translated by a decompress-time patcher — strip trailing padding and keep a reverse map `truncated_translation → full_translation`.
- One parser handles multiple opcodes — hook every variant (or log which fires for your target menu).

### §2. Don't strip non-control prefixes from extracted text

**TSV keys must match the bytes the engine emits at runtime, byte-for-byte.**

Tempting antipattern: "skip leading non-CJK chars to clean up bytecode garbage." It strips
legitimate content too:

| Engine emits | Aggressive strip stores | Result |
|---|---|---|
| `※男心を鷲掴む…`  | `男心を鷲掴む…`  | runtime miss → raw JP |
| `Ａ定食は三時間目…` | `定食は三時間目…` | runtime miss → raw JP |
| `（佐倉）（遊紗）…`  | `佐倉）（遊紗）…`  | runtime miss → raw JP |
| `FLOWER'Sのもう片方` | `のもう片方`     | runtime miss → raw JP |

**Fix:** Only strip when the leading run actually contains a control byte (`\x00..\x1F`).
Pure-printable prefixes pass through:

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

**Cross-game:** Any WillPlus/AdvHD-style CP932 archive needs a similar clean-up. Symptom:
random dialogue lines render as raw JP while neighbours render fine — diff TSV keys against
the engine's runtime bytes and you'll see a single decorator byte difference at the front.

**Don't paper over it at the runtime DLL.** A strip-and-retry fallback in the proxy is OK as
defense-in-depth, but the data fix is upstream — fix the extractor, re-key
`translated_text.json` by `offset` (no LLM calls needed), rebuild the TSV.

### §3. Match thresholds between extract and classify stages

**Two-stage extractors silently drop content when the second stage is stricter than the first.**

If extract uses `count_japanese >= 2` and classify uses `count_japanese >= 3`, every 2-char
Japanese string gets thrown away silently.

**Symptom:** Short choice options (`迫る`, `帰る`, `屋上`, `部室`) render as raw JP while
longer neighbours (`さらに迫る`, `屋上に行く`) translate fine. Unlike §2, the TSV doesn't
have a key at all — `grep` returns nothing. Diagnostic: count occurrences in
`extracted_text.json` vs in the bytecode; a delta means classify dropped them.

**Why the drift:** extract is liberal (feed every candidate). classify tightens to filter
bytecode artifacts that decode as 2 random JP chars (e.g. `\xf8\xf2` → `怙0`). The
2→3 step kills 2-char real choices; lowering to `>= 2` lets ~1500 garbage entries back in.

**Fix — targeted exception keyed on opcode preamble.** WillPlus/AdvHD choice options are
preceded by:
```
ff ff XX YY 00 00 <text> 00
```
where `XX YY` is the LE16 jump label. Gate the threshold relax on this preamble:

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

In CROSS†CHANNEL this recovered exactly 18 dropped choices with zero false positives. The
preamble is engine-level, not game-level — it should match in any AdvHD `sn.bin`.

### §4. Don't skip past the next null on a CP932 decode failure

**A null-walking extractor that jumps to the next `\x00` on strict decode failure will
silently drop every valid string inside a chunk with a non-CP932 prefix byte.**

Common shape:
```python
while pos < fsize:
    if data[pos] == 0x00: pos += 1; continue
    end = data.find(b'\x00', pos)
    raw = data[pos:end]
    try:
        text = raw.decode('cp932', errors='strict')
        pos = end + 1                       # success → skip whole chunk
        continue
    except UnicodeDecodeError:
        pos += 1                            # ← fix: slide one byte, not jump to end
        continue
```

The bug: `pos = end + 1` on decode failure jumps past the `\x00`, losing embedded text.
The chunk has opcode operand bytes before the text — e.g. lead byte `0x86` + trail `0x3a`
(ASCII `:`, not a valid CP932 trail) — so strict decode fails at position 0 and the whole
chunk is skipped.

**Diagnostic:** Lines render as raw JP AND are missing from `extracted_text.json` AND a
substring search of decompressed bytecode shows the bytes ARE present. Reproduce:
1. `data.rfind(b'\x00', 0, target_offset) + 1` → walker's chunk start.
2. `data.find(b'\x00', chunk_start)` → walker's chunk end.
3. `data[chunk_start:chunk_end].decode('cp932', errors='strict')` → raises `UnicodeDecodeError` at position 0.

**Why not `errors='ignore'` or `'replace'`:** Ignore-mode shifts string indices (recorded
`offset` no longer matches runtime bytes — breaks the §2 byte-form-key invariant). Replace
inserts U+FFFD which fails the JP-density filter.

**Distinguishing §2 vs §4:** §2 leaves a key in the TSV that's just shaped wrong (one
decorator byte off); §4 leaves no key at all.

## RE notes

LZSS was identified after brute-force scan (raw `sn.bin` = ~10K strings, mostly garbage at
~30% density → ruled out plaintext and simple XOR), entropy analysis (compressed, not
encrypted), and testing multiple LZSS parameter permutations.

Key insight: decompressing from offset 0 produced 5,702,036 bytes with garbled text
(「=1011 vs 」=120). Skipping the 4-byte LE32 size header gave exactly 5,702,000 bytes with
perfectly balanced brackets (「=28,824 = 」=28,824) and clean Japanese. The 4 extra bytes
at offset 0 were poisoning the LZSS ring buffer.

Control byte stripping: some segments include leading bytecode before text
(e.g. `65 08 59 45 FF FF 18 02「……太一」`). A prefix-stripping pass recovers these.
