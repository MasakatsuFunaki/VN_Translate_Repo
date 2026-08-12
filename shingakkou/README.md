# Shingakkou ~Noli me tangere~ DL

| | |
|---|---|
| **Engine** | DDSystem (PIL) — DDP3 archives, ShsCompression + XOR |
| **Scripts** | 91 files, 52,840 strings (24,736 dialogue + 28,030 narration + 74 choice) |
| **Encoding** | UTF-16LE (scripts), UTF-8 (JSON/TSV output) |
| **Model** | `claude-opus-4-7` with adaptive thinking |
| **Delivery** | `winmm.dll` — hooks decrypt at runtime, swaps strings via `translation_table.tsv` |

## Pipeline

```
01_extract.exe      → script_output/extracted_text.json
02_translate.exe    → translation_cache + translation_table.tsv (written to game dir)
build.py --deploy   → winmm.dll into game folder
00_run_all.exe      → all three in one shot
03_find_narrative_cg.exe  → CG image translation (separate, not part of 00_run_all)
```

## Build

```bat
python build.py                    # both halves (pipeline x64 + proxy Win32)
python build.py --test --install   # build, test, stage to build/install/
python build.py --deploy           # ship winmm.dll + table to game
python build.py --clean            # nuke build tree

set ANTHROPIC_API_KEY=sk-ant-...
build\pipeline\Release\00_run_all.exe
```

Every app takes `--dir <project>` and `--game-dir <install>` (both auto-detected by default).

> `--test N` wipes the cache and re-translates from scratch but does **not** stop after N
> batches — the batch counter is never incremented. Known, left alone.

## Narrative CGs (`03_find_narrative_cg`)

Scans `sin_cgev.dat`/`sin_sysd.dat` for images with JP text, translates via Claude vision,
renders EN over the original, repacks archives. Uses `TOOLS\garbro\extract_ddp.exe` for DDSystem
codecs, FreeType for glyph rendering, stb for BMP/PNG.

```bat
03_find_narrative_cg.exe --scan-only   # preview, no repack
03_find_narrative_cg.exe --repack      # repack with patched BMPs
```

Verified offline by `cg_parity` helper + `analysys/verify_cg_parity.py` (dumps DDP entry tables,
repacks the real 811 MB archive, byte-compares). No OCR pre-filter — every image above threshold
gets a vision call by design.

## Ghidra RE

- Project: `D:\ghidra_projects\Shingakkou` (fully analyzed)
- Headless: `analysys/analyze_shingakkou.bat`
- Scripts: `analysys/ghidra_*.java`, `analysys/decompile_*.java`, `analysys/find_*.java`

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Write `.java` GhidraScripts and run via `analyzeHeadless.bat`.
> NEVER write Python scripts that scan EXE bytes for x86 opcodes.

## Reverse-engineering tools

`analysys/` contents:

| Script | Purpose |
|--------|---------|
| `re_helper.py` | Capstone PE map + decryption + `find_calls_to`/`find_imm_refs`/`get_script` |
| `ddp_codec.py` | Standalone DDP3 reader (no pipeline dependency) |
| `dump_choice_menus.py` | Extract `2A 10 FF 00` menus with option strings and branch targets |
| `dump_script_graph.py` | Map script-name loads + flag writes per branch |
| `verify_word_wrap.py` | Offline word-wrap verification on full 52k corpus |
| `verify_crash_fix.py` | Offline jump-relocation verification |
| `verify_cg_parity.py` | CG archive repack byte-comparison |
| `diag_crash.py` | Crash diagnostics harness |

Flag bytecode: assign `00 3f 03 <var> <imm> 40 ff`, add `...41 ff`.
Condition: `3f 03 <var>` read, with `50` ==, `52` <, `55` >=, `6d` and, `6e` or.

## DDP3 archive format

- **Header**: DDP3 magic (32 bytes) + outer file table + inner file table
- **Compression**: ShsCompression — custom LZ77 (derived from GARbro `SHSystem/ArcHXP.cs`)
- **Encryption**: XOR cipher on DDWuHXB data (key from data length field)
- **Strings**: UTF-16LE null-terminated, preceded by `ff 01 80` marker
- **String type**: Byte before marker — `0x36` = dialogue/narration, `0x04` = sprite, etc.
- **Script format**: `SpeakerName\nText` (dialogue), `\nText` (narration), 75 unique speakers

## Character routes

| Route | Scripts | Leads |
|-------|---------|-------|
| Main | main01–11, op05 | Michael (protagonist) |
| Neil | neil01–12 + endings | Michael, Neil |
| Leo | leo01–11 + endings | Michael, Leonid |
| Cecil | cecil01–11 + endings | Michael, Cecil |
| Gabriel | gabriel01–10 + endings | Michael, Gabriel |
| August | august01–13 + endings | Michael, August |

---

## Engine lessons

Four classes of bugs DDSystem creates when English text changes the buffer length.
Every hex address, opcode, and offset below is engine-specific and exists nowhere else in the repo.

### 1. Choice jump tables (body-resident absolute offsets)

The tail offset-table fixup only relocates the seek table at the script END. **Choice tables
in the bytecode body also hold absolute offsets and are NOT covered.**

| | |
|---|---|
| **Format** | `2A 10 FF 00 <count:u8> <count × 3-byte BE absolute offset>` |
| **Scope** | 32 menus, 76 entries across the whole game, count 2–5 per menu |
| **Symptom** | Selecting a choice crashes — engine lands on stale offset mid-string, interprets text as opcodes. Typically `DIVIDE_BY_ZERO` from `rand() % 0` (opcode `FF 02`) in expression VM, or `ILLEGAL_INSTRUCTION`. Standard VEH misses it (only catches `ACCESS_VIOLATION`). |
| **Fix** | Scan original buffer for `2A 10 FF 00` anchor; validate every entry targets a code boundary (`03 0d xx` / `00 3f xx`); relocate each entry + the table position by the same cumulative-delta as the tail table. `translator_logic::RelocateChoiceTables` + `choice_table_ut`. |
| **VEH** | Broaden to all fatal codes: `(code & 0xC0000000)==0xC0000000`, skip MSVC EH `0xE06D7363`. Log EIP + bytes@EIP. Map registers + stack to patched buffer — `[ESP+0x1C]` recovers the VM cursor. Cursor pointing mid-string is the tell. |
| **Verify** | `analysys/verify_crash_fix.py` — 32 tables, 76 entries, 0 mismatches, zero game round-trips. |

### 2. ALL jump opcodes (runtime relocation)

The choice-table fix (above) covers only opcode `0x2A`. All other jumps also carry absolute
offsets and crash once text shifts the buffer.

**Full jump opcode set** (dispatch table at VA `0x4513F0`, interpreter does `mov ecx,[eax*4+0x4513F0]; jmp ecx` at RVA `0x10F66`):

| Opcode | Meaning | Operand |
|--------|---------|---------|
| `0x29` | GOTO | BE24 after opcode |
| `0x02` | GOSUB/CALL | BE24 (pushes return to 8-deep stack at `[ctx+0x40410]`) |
| `0x26`/`0x27` | Conditional GOTO (if-true / if-false) | inline expr + BE24 |
| `0x28` | Two-way branch | inline expr + 2× BE24 |
| `0x2A` | Switch / choice case-table | handled by §1 above, NOT double-hooked |
| `0x3C` | RETURN | pops gosub stack (already-patched value) — NOT relocated |

**Why runtime, not byte-scan:** these are unanchored single bytes (`0x29` appears 24× per script,
~9 real; `0x02` appears 4657×, 2 real). Real instances require walking the statement stream
as the interpreter does — a 2-byte line-number prefix is consumed per statement only when
`[ctx+0x406DF]` is set.

**Fix:** Replace dispatch-table entries for `0x29`/`0x02`/`0x26`/`0x27`/`0x28` with stubs.
Each stub saves entry position P0 + loop return address, swaps the return to a shared
post-handler, then `jmp`s the original handler. Post-handler relocates
`[0x491A0C]->position` by the cumulative-delta map, UNLESS `position == P0+3` (conditional
that fell through past its 3-byte operand). `translator.cpp` `InstallJumpHooks` / `RelocateAfterJump`.

**Save/load trap (double-relocation):** Every script starts at offset `0x10` with a
conditional `0x26` whose BE24 operand IS the header table pointer at `0x16` (`= table_start-5`),
already relocated by `PatchDecryptedData`. On normal start the condition is false (falls through).
On SAVE-LOAD it fires — jumping to the resume switch at `table_start-5` (a `2A 10 FF 00` with
`count > 16`, which `RelocateChoiceTables`' `count<=16` guard correctly skips).

**Guards:** Skip when handler-entry position is `0x11` (opcode at `0x10`). Skip when the
relocated result would overflow the patched buffer (a correctly-relocated raw target always
stays in-bounds; overflow means the value was already patched).

**Symptom if missed:** choices and scenes work, but loading a save jumps out of bounds — crash
in the dispatcher (`mov dl,[ecx+edx]` at RVA `0x10F53`) reading `base + oversized-pos`.

**If `0xDF15`/base==0 recurs after all jumps are relocated:** opcode `0x03` (handler RVA
`0x13640`) indexes a 200-slot×12-byte descriptor table at VA `0x490E30` (built at runtime).
If a slot's type is 1 it repoints `[0x491A0C]` to a per-script sub-context whose base (`[+4]`)
may be 0 (never loaded). That is a load-ordering bug, not a relocation bug.

Confirm the set is complete: scan every handler for the `movzx r,[reg-3/-2/-1]` +
`mov [ctx+0],r` idiom (bound each handler at its first `ret`/`int3` to avoid window-bleed).

### 3. Eraser stamp geometry

VN engines erase old text with one opaque background stamp; every text blit is colour-keyed
and only adds pixels. If a layout patch moves text outside the stamp rect, stale glyphs
accumulate.

| | |
|---|---|
| **Symptom** | Dotted glyph-top garbage at top of first dialogue line only, worsening as you advance. |
| **Cause** | Stamp covered `var[0x2D3]+36..+126`. Line i sits at `+i*spacing+var[0x2D7]`. At spacing 20 (was 30), line 1 at `+29` pokes 7px above stamp top `+36`. Lines 2+ stay inside. Stock constants were exactly tight — assume ANY layout change needs an eraser audit, both edges. |
| **Finding the eraser** | Decode message-path bytecode, list every draw op's blend mode. Eraser = the lone opaque (mode 255) blit of window-background layer. Shingakkou: entry 7 @`0x14F`, `op16 131→1, y=V[0x2D3]+imm8 36, h=imm8 86+4`. |
| **Fix** | Patch the stamp's imm8 constants (top y src+dst, height), NOT the var init (vars restored from save files; bytecode constants re-execute every message). Derive top from spacing: `kMessageRefreshTop = kMessageLineSpacing + 9`. |
| **Verify** | Pattern unique game-wide, 3-byte diff, every line band within `[top, bottom]`. |
| **Dead end** | "Sliced glyph" look = brush font, not the entry-7 strip-jitter effect. Jitter gated on `V[0x7b]==1` (large-font 32/42 branch); deployed game runs `V[0x7b]==0` (small font 21, spacing 20). |

### 4. Word-wrap with the engine's hard-break character

CJK engines wrap per-character (accumulate width, break on overflow). Fine for JP, slices EN
mid-word. Don't patch the wrap loop — find the engine's hard-break char and pre-wrap in the DLL.

| | |
|---|---|
| **Hard-break** | Disasm wrap handler, grep for `cmp` with 0x0A/0x0D: `\r` (0x000D) is the one. `0x6A` mode-1 loop treats it as flush-line-and-reset (RVA `0x1BA8E`), consuming the CR. |
| **Measure** | Mirror engine fn at RVA `0x2860`: halfwidth = code<0xFF minus 0xB1/0xD7/0xF7, plus FF61–FF9F. Patched halfwidth=8px, fullwidth=21px. `'…'` is fullwidth. Budget: `[0x45E734]=536` (67×8 = 536 exactly). |
| **Wrap** | Greedy: swap last space of overflowing line for `\r` — 1:1 wide-char swap, byte length unchanged, offset relocation unaffected. Words wider than a line fall back to engine char-break. |
| **Scope** | Dialogue/narration only (preByte `0x36`). Choices render in different widgets. |
| **Verify** | `analysys/verify_word_wrap.py` — 0 messages > 4 lines across 52,766 strings. |

## Text overflow fix (proxy DLL)

Two memory patches at DLL load shrink character cell width 21→9 units (~58 EN chars/line
instead of ~53). See `analysys/01_open_issue_text_doesnt_fit/`.

## Backup

`sin_text.dat` is never modified. Step 1 prefers `Data\sin_text.dat.bak` when one exists,
so an archive from an older repack-based run is extracted from rather than the patched file.
