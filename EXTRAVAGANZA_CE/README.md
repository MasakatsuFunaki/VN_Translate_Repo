# EXTRAVAGANZA ~Mushi Mederu Shoujo~ CE

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | 141,239 across 16 SPT files |
| **Delivery** | Repacked SPT files + `winmm.dll` (runtime font patch) |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\EXTRAVAGANZA_CE_DUMP`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write Python scripts that scan EXE bytes for x86 opcodes.

## Pipeline

| Step | Executable | What |
|------|-----------|------|
| 1 | `01_extract` | Extract JP text from `.spt` → `extracted_text.json` |
| 2 | `02_translate` | Speaker gate (zero tokens) → Claude translation (cached) |
| 3 | `03_repack` | Word-wrap + repack into `.spt` |
| 4 | `04_translate_xtx` | XTX config translation (character name plates only — see note) |
| 5 | `05_translate_charts` | Flowchart (`chart/*.fxf`) translation |
| — | `build.py --deploy` | Copy `winmm.dll` into game folder |
| — | `00_run_all` | All steps in one command |

> **XTX trap:** BGM/SE lists inside the XTX step stay **disabled** — the engine
> crashes when those config fields contain single-byte ASCII. Only the character-name plate
> table is applied, in full-width romaji.

## Build

```bat
python build.py --test --install   # both halves + tests + stage
python build.py --deploy           # ship winmm.dll to game
python build.py --clean

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

Options: `--test` / `--test 4` (batch cap), `--batch 50`, `--clean` (delete cached JSON),
`--retranslate`, `--file 01.spt`. Restore originals: `03_repack.exe --restore`.

## Characters

夢美=Yumemi, アゲハ=Ageha, ユーリア=Yuria, サユリ=Sayuri, 杏子=Kyouko, 美弥香=Miyaka,
唯=Yui, 綾佳=Ayaka, 遥=Haruka
