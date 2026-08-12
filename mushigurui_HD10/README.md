# Mushigurui HD10 (蟲狂い / Bug Madness)

> **AGENT — Ghidra is the ONLY allowed EXE disassembly method.**
> This project has a Ghidra setup at `C:\ghidra` with project `D:\ghidra_projects\mushigurui_HD10`.
> When you need to analyze the game EXE (find functions, cross-references, string refs, struct offsets, opcode patterns):
> - Write a `.java` GhidraScript and run it via `analyzeHeadless.bat` (see `analysys/analyze_dump.bat`)
> - **NEVER** hand-roll a script that scans EXE bytes for x86 opcodes (push/mov/call patterns, ModRM parsing, etc.)
> - Ghidra already does this correctly with full disassembly, control flow, and type recovery
> - Manual hex-based x86 analysis wastes tokens and produces fragile, incomplete results
> - Existing Ghidra scripts: `analysys/ghidra_*.java`, `analysys/decompile_*.java`

English translation pipeline for **Mushigurui HD10** — a dark supernatural VN in the EXTRAVAGANZA universe by BLACKCyc.

## Game Info

| Property | Value |
|----------|-------|
| Engine | nnn/BCmkri (BLACKCyc) |
| Format | SPT (XOR 0xFF encrypted, CP932) |
| JP Strings | ~13,105 |
| SPT Files | s100–s400 (story arcs), etc.spt, BCmkri_backscript.spt, sys.spt |
| Characters | Ren (レン), Miyaka (美弥香), Ayaka (綾佳), Yuria (ユーリア), Shio (シオ), Kyou (キョウ) |

## Pipeline

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake). It produces
`extracted_text.json`, `translated_text.json` + resume cache, and the CP932
`translation_table.tsv` the proxy DLL reads at runtime (13,105 entries,
2,138,934 bytes).

Three steps, and 00_run_all performs all three in one command:

```
00_run_all.exe                   → runs 01, 02, then deploys winmm.dll
01_extract.exe                   → script_output/extracted_text.json
02_translate.exe                 → speaker gate (zero tokens) →
                                   script_output/translated_text.json +
                                   translation_cache_anthropic.json →
                                   translation_table.tsv in the game folder
python build.py --deploy         → winmm.dll into the game folder
proxy_dll/                       → winmm.dll (runtime font patches + text hook)

03_translate_xtx.exe             → translated config files (character name plates)
04_find_narrative_cg.exe         → script_output/narrative_candidates.json + narrative_patched/
                                   (CG asset translation; repacks dwq/*.gpk)
05_replace_choice_text.exe       → choices/*_en.jpg (choice-menu screenshot translator)
```

03 to 05 are one-shot asset tools and intentionally NOT in the orchestrator:
they rewrite shipped config files, archives and screenshots, not script text.
The speaker gate and the runtime-table build are inside `02_translate`, so the
command that pays for the translation is also the command that proves the
speakers still flow and that produces the table the game reads.

The narrative-CG translator, the GPK+GTB archive module and the
choice-screenshot tool live in `src/cg/`, `src/gpk/` and `src/choices/`.

**Runtime translation**: The proxy DLL hooks the game's text splitter function (RVA 0x063850) at runtime and replaces Japanese text on-the-fly using `translation_table.tsv`. Original SPT files are never modified.

## Quick Start

```bash
cd mushigurui_HD10
python build.py --test --install

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

`build.py` is the supported front end and has four flags: `--test`,
`--install`, `--deploy`, `--clean`. The whole game builds into one tree —
`build/pipeline` (x64), `build/proxy` (Win32), `build/install` (`bin/` to run,
`game/` to copy into the game). See `SakuraNoUta/BUILD_STRUCTURE.md` for the
layout and its conventions.

The orchestrator runs: extract → translate → deploy.

## Individual Steps

```bash
cd build\install\bin
01_extract.exe
02_translate.exe --batch 50
02_translate.exe --test        # smoke test: 1 batch only
02_translate.exe --file s100.spt
03_translate_xtx.exe
```

All apps take `--dir <project>` and `--game-dir <install>`. `--dir` defaults to
the project folder found by walking up from the executable's own location, and
is required when the exe sits outside the project tree.

## Narrative CG Translation (asset translator + repacker)

```bash
04_find_narrative_cg.exe              # scan + translate + repack
04_find_narrative_cg.exe --scan-only  # scan only, write candidates.json
04_find_narrative_cg.exe --repack     # repack from existing patched images
04_find_narrative_cg.exe --no-resume  # restart scan from scratch
04_find_narrative_cg.exe --replay <jsonl>   # replay recorded /v1/messages
                                            # responses; zero API calls
```

1. **Extract** all images from `dwq/ev0.gpk`, `dwq/ta0.gpk`, `dwq/sys0.gpk`
   (BCmkri/nnn GPK+GTB format) into `script_output/narrative_extracted/<arc>/`,
   with a per-archive `_index.json` that doubles as the on-disk cache.
2. **Y/N gate** — nothing is sent to Claude until you confirm.
3. **OCR pre-filter** — a cost optimisation, currently a no-op: no OCR backend
   is wired up, so it logs `[ocr] 0 method(s): none`, every image passes, and
   Claude does all the classification. The vote seam is kept so a backend can
   be dropped in without touching the scan loop.
4. **Vision detect** via Claude Sonnet at confidence ≥ 0.7, appending to
   `narrative_candidates.json` and `narrative_scanned.json` after *every* entry
   so a crashed run resumes for free.
5. **Vision extract + translate** — JP text, EN translation, bbox, colour and
   background tone per region.
6. **Render** — inpaint each bbox with the median of its 2px border and draw the
   English with FreeType (Monotype Corsiva, arial fallback, auto-fitted 36→8px).
7. **Repack** — substitute the patched image into the GPK and regenerate the GTB
   offset tables; the originals are kept as `*.gpk.backup` / `*.gtb.backup` and
   every repack sources from the backup, so reruns never compound.

The GPK+GTB reader/repacker is `pipeline_cpp/src/gpk/` — there is no
`extract_gpk.exe` in `TOOLS/garbro` for this engine.

### Verifying a change to this step

The game is not installed on this machine, so `analysys/make_gpk_corpus.py`
rebuilds a synthetic 1005-entry corpus out of the checked-in extracted payloads.
Run the step against it with `--replay` and diff the artifacts: extraction,
repacking (both with the 90 real patched PNGs and with an empty replacement set)
and the two scan JSONs are all deterministic and must not move.

Rendered PNGs are **not** byte-comparable across encoder or rasteriser versions,
so they are checked structurally (file set, dimensions, colour type). The
deterministic sub-pieces that decide the pixels — border-median background
sampling, the inclusive `fill_rect`, greedy word wrap, font fitting, floor
division, hex parsing — are pinned by `ut_cg_render` / `ut_cg_parse` instead.

## Choice-menu screenshot translator

```bash
05_replace_choice_text.exe            # translates choices/*.jpg -> *_en.jpg
```

Detects the blue choice buttons by column-fraction, wipes the yellow glyphs with
each row's non-yellow median, and redraws the English in a bold face with a dark
stroke. Translations are cached in `choices/_cache.json`.

The detector's arithmetic is pinned by `ut_choices` on a synthetic menu, not on
the shipped screenshots: JPEG decoding is not bit-exact across decoders, and on
`01_choice.jpg` a handful of edge pixels is enough to move one button's right
edge from x=795 to x=798 (the other 11 boxes across the four screenshots are
stable). The rendered `*_en.jpg` is likewise not byte-comparable across JPEG
encoders.

## Proxy DLL

The `proxy_dll/` folder contains a winmm.dll proxy that applies:
- **10 patcher patches**: fontSize 24→14, line spacing, character spacing, message line count (JE→JMP approach)
- **Runtime text hook**: Intercepts text splitter function, looks up JP→EN translations from `translation_table.tsv`
- Word wrapping at 100 chars/line, max 7 lines per textbox

Build requires the MSVC C++ toolchain (VS 2022), CMake 3.23+ and ninja.

## Notes

- Same engine as EXTRAVAGANZA CE / Matsuro CE — extraction tooling is identical
- Original SPT files are NOT modified — no `spt_backup` needed
- `translation_table.tsv` is CP932-encoded, tab-separated (JP→EN)
- Check `proxy_log.txt` in game dir for hook/patch diagnostics
- `analysys/` contains reverse-engineering scripts and Ghidra decompilation outputs
