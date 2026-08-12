# Mushigurui HD10 (蟲狂い / Bug Madness)

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) — same as EXTRAVAGANZA CE |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | ~13,105 |
| **SPT Files** | s100–s400 (story arcs), etc.spt, BCmkri_backscript.spt, sys.spt |
| **Characters** | Ren (レン), Miyaka (美弥香), Ayaka (綾佳), Yuria (ユーリア), Shio (シオ), Kyou (キョウ) |
| **Delivery** | `winmm.dll` — hooks text splitter (RVA `0x063850`), replaces JP via `translation_table.tsv`. SPTs never modified. |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\mushigurui_HD10`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write scripts that scan EXE bytes for x86 opcodes.

## Pipeline

```
01_extract          → extracted_text.json
02_translate        → speaker gate → translated_text.json + translation_table.tsv
                      (CP932, 13,105 entries, 2,138,934 bytes)
00_run_all          → extract + translate + deploy in one shot
```

Side tools (not in `00_run_all`):
```
03_translate_xtx          → config name plates
04_find_narrative_cg      → CG asset translation + GPK repack (dwq/*.gpk)
05_replace_choice_text    → choice-menu screenshot translator (choices/*.jpg → *_en.jpg)
```

`python build.py --deploy` copies `winmm.dll` into the game folder.

## Build

```bat
python build.py --test --install
python build.py --deploy
python build.py --clean

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

Options: `--batch 50`, `--test` (1 batch), `--file s100.spt`.

## Narrative CG (04)

Extracts images from `dwq/ev0.gpk`, `dwq/ta0.gpk`, `dwq/sys0.gpk` (BCmkri/nnn GPK+GTB
format), detects JP text via Claude vision (Sonnet, confidence ≥ 0.7), translates, renders
EN with FreeType (Monotype Corsiva, arial fallback, auto-fitted 36→8px), repacks into GPK
with regenerated GTB offset tables.

```bat
04_find_narrative_cg.exe              # scan + translate + repack
04_find_narrative_cg.exe --scan-only  # scan only
04_find_narrative_cg.exe --repack     # repack from existing patched images
04_find_narrative_cg.exe --replay <jsonl>   # replay recorded responses, zero API calls
```

- OCR pre-filter is a no-op (`[ocr] 0 method(s): none`) — every image passes, Claude classifies.
- Originals kept as `*.gpk.backup` / `*.gtb.backup`; every repack sources from the backup (reruns never compound).
- GPK+GTB reader/repacker: `pipeline_cpp/src/gpk/` (no GARbro equivalent for this engine).

**Offline verification:** `analysys/make_gpk_corpus.py` rebuilds a synthetic 1005-entry corpus
from checked-in payloads. Run with `--replay` and diff: extraction, repacking (with 90 real
patched PNGs and with empty set), and both scan JSONs are deterministic.

Rendered PNGs are NOT byte-comparable across encoders — checked structurally (dimensions,
colour type). Pixel-deciding sub-pieces (border-median, `fill_rect`, greedy wrap, font
fitting) pinned by `ut_cg_render` / `ut_cg_parse`.

## Choice-menu screenshots (05)

```bat
05_replace_choice_text.exe   # choices/*.jpg → *_en.jpg
```

Detects blue choice buttons by column-fraction, wipes yellow glyphs with each row's
non-yellow median, redraws EN in bold with dark stroke. Translations cached in
`choices/_cache.json`.

Detector pinned by `ut_choices` on a synthetic menu (JPEG decoding is not bit-exact:
on `01_choice.jpg`, edge pixels move one button's right edge from x=795 to x=798; the
other 11 boxes are stable).

## Proxy DLL

10 patches (fontSize 24→14, spacing, line count via JE→JMP) + runtime text hook. Word
wrapping at 100 chars/line, max 7 lines per textbox. Check `proxy_log.txt` for diagnostics.
