# EXTRAVAGANZA ~Mushi Mederu Shoujo~ CE

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | 141,239 across 16 SPT files |
| **Delivery** | Repacked SPT files + `winmm.dll` (runtime font patch) |

## Pipeline

| Step | Executable | What |
|------|-----------|------|
| 1 | `01_extract` | Extract JP text from `.spt` → `extracted_text.json` |
| 2 | `02_build_tsv` | Speaker gate (zero tokens) → assemble `translated_text.json` from cache |
| 3 | `03_repack` | Word-wrap + repack into `.spt` |
| 4 | `04_build_xtx` | XTX config translation (character name plates only — see note) |
| 5 | `05_build_charts` | Flowchart (`chart/*.fxf`) translation from `chart_translation_cache.json` |
| 6 | `06_build_menus` | Repack `dwq/sys0.gpk` menu graphics and `init2/*.fxf` UI configs |
| — | `build.py --deploy` | Copy `winmm.dll` and `extravaganza_patch.bin` into game folder |
| — | `00_run_all` | All steps in one command |

> **XTX trap:** BGM/SE lists inside the XTX step stay **disabled** — the engine
> crashes when those config fields contain single-byte ASCII. Only the character-name plate
> table is applied, in full-width romaji.

## Build

```bat
python build.py --test --install          # both halves + tests + stage
python build.py --deploy --game-dir <dir> # ship winmm.dll + extravaganza_patch.bin to game
python build.py --clean

build\install\bin\00_run_all.exe --game-dir <dir>
```

Restore original scripts from backup: `03_repack.exe --game-dir <dir> --restore`.

`--game-dir <install>` is required: the game install path differs per machine.

## Characters

夢美=Yumemi, アゲハ=Ageha, ユーリア=Yuria, サユリ=Sayuri, 杏子=Kyouko, 美弥香=Miyaka,
唯=Yui, 綾佳=Ayaka, 遥=Haruka
