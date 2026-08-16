# Mushigurui HD10 (蟲狂い / Bug Madness)

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) — same as EXTRAVAGANZA CE |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | ~13,105 |
| **SPT Files** | s100–s400 (story arcs), etc.spt, BCmkri_backscript.spt, sys.spt |
| **Characters** | Ren (レン), Miyaka (美弥香), Ayaka (綾佳), Yuria (ユーリア), Shio (シオ), Kyou (キョウ) |
| **Delivery** | `winmm.dll` — runtime text hook via `translation_table.tsv`. SPTs never modified. |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\mushigurui_HD10`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write scripts that scan EXE bytes for x86 opcodes.

## Pipeline

```
01_extract          → extracted_text.json
02_translate        → speaker gate → translated_text.json + translation_table.tsv
00_run_all          → extract + translate + deploy in one shot
```

Side tools (not in `00_run_all`):
```
03_translate_xtx          → config name plates
04_find_narrative_cg      → CG asset translation + GPK repack
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

`--clean` and `--retranslate` refuse a cache that already holds paid work.
Add `--discard-cache` to let them run.

`--game-dir <install>` is required: the game install path differs per machine.

## Narrative CG (04)

```bat
04_find_narrative_cg.exe              # scan + translate + repack
04_find_narrative_cg.exe --scan-only  # scan only
04_find_narrative_cg.exe --repack     # repack from existing patched images
04_find_narrative_cg.exe --replay <jsonl>   # replay recorded responses, zero API calls
```

## Choice-menu screenshots (05)

```bat
05_replace_choice_text.exe   # choices/*.jpg → *_en.jpg
```
