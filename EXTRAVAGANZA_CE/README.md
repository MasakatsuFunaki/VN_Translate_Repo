# EXTRAVAGANZA ~Mushi Mederu Shoujo~ CE

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) |
| **Format** | SPT — XOR 0xFF encrypted, CP932, DWORD-indexed VA offset tables |
| **JP Strings** | 141,239 across 16 SPT files |
| **Delivery** | Repacked SPT files + `winmm.dll` (runtime font patch: fontSize 24→14, nextY 6→4) |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\EXTRAVAGANZA_CE_DUMP`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write Python scripts that scan EXE bytes for x86 opcodes.

## Pipeline

| Step | Executable | What |
|------|-----------|------|
| 1 | `01_extract` | Extract JP text from `.spt` → `extracted_text.json` |
| 2 | `02_translate` | Speaker gate (zero tokens) → Claude translation (cached) |
| 3 | `03_repack` | Word-wrap (`MAX_LINE_CHARS=60`) + repack into `.spt` with VA relocation |
| 4 | `04_translate_xtx` | XTX config translation (character name plates only — see note) |
| 5 | `05_translate_charts` | Flowchart (`chart/*.fxf`) translation |
| — | `build.py --deploy` | Copy `winmm.dll` into game folder |
| — | `00_run_all` | All steps in one command |

> **XTX trap:** BGM/SE lists inside the XTX step stay **disabled** — the BLACKCyc engine
> crashes when those config fields contain single-byte ASCII. Only the character-name plate
> table is applied, in full-width romaji.

Word-wrapping: `MAX_LINE_CHARS=60`, `MAX_LINES=7`, line breaks `\r\n`. Fits the textbox at
fontSize=14. Wrapping counts characters, not bytes.

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

## Proxy DLL (winmm.dll)

Game EXE is packed — static patching not feasible. The proxy forwards `mixerGetLineInfoA`
to the real `winmm.dll` and patches unpacked code at runtime:
- fontSize: 24→14, nextY addend: 6→4 (line spacing 18 instead of 30)
- NOPs out FXF config reads for fontSize, nextY, LNextY

See `proxy_dll/src/patcher.cpp` for patch addresses.

## Characters

夢美=Yumemi, アゲハ=Ageha, ユーリア=Yuria, サユリ=Sayuri, 杏子=Kyouko, 美弥香=Miyaka,
唯=Yui, 綾佳=Ayaka, 遥=Haruka
