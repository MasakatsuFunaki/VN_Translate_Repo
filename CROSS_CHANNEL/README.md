# CROSS†CHANNEL -FINAL COMPLETE-

| | |
|---|---|
| **Engine** | WillPlus/AdvHD |
| **Format** | `sn.bin` — LZSS compressed bytecode (CP932) |
| **JP Strings** | 53,635 (28,787 dialogue + 24,848 narration) |
| **Speakers** | 75 (太一, 見里, 美希, 霧, 冬子, 友貴, 桜庭, 曜子, 七香 …) |
| **Delivery** | `xinput1_3.dll` — hooks LZSS decompressor, patches buffer in memory. `sn.bin` is never modified. |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\CROSS_CHANNEL`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write Python scripts that scan EXE bytes for x86 opcodes.

## Pipeline

```
01_extract          → extracted_text.json
02_translate        → speaker gate → translated_text.json + translations.tsv
00_run_all          → all three in one shot
03_find_narrative_cg → CG/UI images with JP text → narrative_patched/ (inspection-only; CPK repack not implemented)
```

`python build.py --deploy` copies `xinput1_3.dll` + `translations.tsv` into the game.

## Build

```bat
python build.py                    # both halves (pipeline x64, proxy Win32)
python build.py --test --install   # build + tests + stage
python build.py --deploy           # ship to game
python build.py --clean

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

Every app takes `--dir <project>` and `--game-dir <path>`.
`--game-dir <install>` is required: the game install path differs per machine.

`--clean` and `--test` refuse a cache that already holds paid work.
Add `--discard-cache` to let them run.

## Narrative CG (03)

Patched BMPs are inspection-only — CPK repacking is not implemented.
No local OCR pre-filter; `--scan-only` and the confirmation prompt keep the bill down.
