# EXTRAVAGANZA ~Mushi Mederu Shoujo~ — English Translation

> **AGENT — Ghidra is the ONLY allowed EXE disassembly method.**
> This project has a Ghidra setup at `C:\ghidra` with project `D:\ghidra_projects\EXTRAVAGANZA_CE_DUMP`.
> When you need to analyze the game EXE (find functions, cross-references, string refs, struct offsets, opcode patterns):
> - Write a `.java` GhidraScript and run it via `analyzeHeadless.bat` (see `analysys/analyze_dump.bat` for examples)
> - **NEVER** write Python scripts that manually scan EXE bytes for x86 opcodes (push/mov/call patterns, ModRM parsing, etc.)
> - Ghidra already does this correctly with full disassembly, control flow, and type recovery
> - Manual hex-based x86 analysis wastes tokens and produces fragile, incomplete results
> - Existing Ghidra scripts: `analysys/ghidra_*.java`, decompiled output: `analysys/*_decompiled.c`


Automated translation pipeline for **EXTRAVAGANZA～蟲愛でる少女～ CE** by BLACKCyc.
Dark supernatural horror VN — main game with 8 character routes.

**Stats:** 141,239 JP strings across 16 SPT files

## Prerequisites

- CMake 3.23+, ninja, and the MSVC C++ toolchain (Visual Studio 2022 or its
  Build Tools); the generator is Ninja Multi-Config
- Conan 2
- Anthropic API key — set via **one** of:
  - `ANTHROPIC_API_KEY` environment variable, **or**
  - WSL Claude settings (`~/.claude/settings.json` — auto-loaded by pipeline)

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake). It talks to
the API directly over Boost.Beast + OpenSSL, so there is no SDK to install.

## Quick Start

```bash
cd EXTRAVAGANZA_CE
python build.py --test --install

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

`build.py` builds both halves — `pipeline_cpp` (x64) and `proxy_dll` (Win32) —
into one `build/` tree and has four flags: `--test`, `--install`, `--deploy`,
`--clean`. `--install` stages `bin/` (what you run) and `game/` (what you copy
into the game folder).

The API key is auto-loaded from `~/.claude_vn/settings.json` if
`ANTHROPIC_API_KEY` is not set.

`00_run_all` runs the whole pipeline: extract → translate → repack → config
translate → chart translate → deploy.

> **Font fix:** the deploy step copies `winmm.dll` next to `mushiEx_CE.exe`.
> The proxy DLL patches font size 24→14 and line spacing at runtime (no FXF edits needed).

## Pipeline

Three steps carry the work — extract, translate, deploy. The speaker gate is
part of the translation step, so it cannot be forgotten before a paid run, and
the three steps that write English into the game's own files sit between
translation and deploy.

| Step | Executable | Description |
|------|-----------|-------------|
| 1 | `01_extract.exe` | Extract JP text from `.spt` files → `script_output/extracted_text.json` |
| 2 | `02_translate.exe` | Speaker gate (5 checks, zero API tokens), then translate via Claude (resume-safe, cached) |
| 3 | `03_repack.exe` | Word-wrap + repack translations into `.spt` files, with full structural fixup |
| 4 | `04_translate_xtx.exe` | XTX config translation (see note) |
| 5 | `05_translate_charts.exe` | Flowchart (`chart/*.fxf`) translation |
| 6 | `python build.py --deploy` | Copy `winmm.dll` into the game folder (runtime font patch, fontSize 24→14) |

> **Note:** the BGM/SE lists inside the XTX step stay **disabled** — the BLACKCyc
> engine crashes when those config fields contain single-byte ASCII. Only the
> character-name plate table is applied, in full-width romaji.

### Step-by-step (manual)

```bash
cd build\install\bin

01_extract.exe               # extract text from all 16 SPT files
02_translate.exe --batch 50  # speaker gate, then translate (resume-safe)
03_repack.exe                # repack with word-wrapping (60 chars/line)
04_translate_xtx.exe         # config name plates
05_translate_charts.exe      # flowchart nodes

cd ..\..\.. && python build.py --deploy   # copy winmm.dll into the game folder
```

## Options

```bash
00_run_all.exe --test                     # 1 batch only (smoke test)
00_run_all.exe --test 4                   # 4 batches then stop
00_run_all.exe --batch 50                 # lines per API call
00_run_all.exe --clean                    # delete cached JSON, then full run
00_run_all.exe --dir <p> --game-dir <p>   # override project / game folders

02_translate.exe --batch 50               # lines per API call
02_translate.exe --retranslate            # ignore cache, redo all
02_translate.exe --file 01.spt            # a single script

05_translate_charts.exe --test            # dry run: list texts, no API calls
```

## Restore Originals

```bash
03_repack.exe --restore
```

## How It Works

### SPT Format
SPT files are XOR 0xFF encrypted, CP932 (Shift-JIS) encoded binary scripts.
Each file has sections with DWORD-indexed VA offset tables pointing to text strings.
The repack step rebuilds offset tables, updates section sizes, and relocates
post-text section VAs when the text block grows or shrinks.

### Word-Wrapping
English text is automatically word-wrapped at `MAX_LINE_CHARS=60` per line during repack
(configured in `pipeline_cpp/src/repack/repack.h`). Line breaks use `\r\n`. This fits the
game's textbox at fontSize=14. Wrapping counts **characters, not bytes**, and the result is
truncated to `MAX_LINES=7` so the engine's own cap never silently drops text.

### Font Fix (Proxy DLL)
The game EXE is packed, so static patching isn't feasible. Instead, a proxy DLL
(`winmm.dll`) is placed next to `mushiEx_CE.exe`. It forwards `mixerGetLineInfoA`
to the real `winmm.dll` and patches the unpacked code at runtime:
- fontSize: 24→14
- nextY addend: 6→4 (line spacing 18 instead of 30)
- NOPs out FXF config reads for fontSize, nextY, and LNextY

Built by `python build.py` (requires the MSVC C++ toolchain; the half is a Win32
target) and shipped by `python build.py --deploy`.
See `proxy_dll/src/patcher.cpp` for exact patch addresses.

## File Layout

```
EXTRAVAGANZA_CE/
├── CMakeLists.txt             # Root: drives both halves, owns the install
├── CMakePresets.json          #   preset "windows-release"
├── build.py                   #   --test --install --deploy --clean
├── pipeline_cpp/              # C++ pipeline, x64 (Conan 2 + Boost + CMake)
│   ├── conanfile.py           #   boost, openssl, gtest
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── common/            #   file IO, CP932<->UTF-8, JSON, text helpers
│   │   ├── spt/               #   SPT reader + string classifier
│   │   ├── extract/           #   step 1
│   │   ├── translate/         #   Anthropic client, glossary, speaker gate,
│   │   │                      #   batching/cache (step 2)
│   │   ├── repack/            #   step 3: word-wrap + SPT rebuild + VA relocation
│   │   ├── xtx/               #   step 4
│   │   ├── charts/            #   step 5: FXF flowchart translation
│   │   └── apps/              #   00_run_all and the 01..05 mains
│   └── tests/                 #   GoogleTest suites (ctest label UT)
├── script_output/             # Generated artifacts
│   ├── extracted_text.json            # Extracted JP text
│   ├── translated_text.json           # Translated EN text
│   ├── translation_cache_anthropic.json
│   └── chart_translation_cache.json
├── proxy_dll/                 # Runtime font patch, Win32 (winmm.dll proxy)
│   ├── CMakeLists.txt         # Build config (Win32)
│   ├── conanfile.py           # gtest for the SYSTEM tier
│   └── src/                   # Source files
│       ├── dllmain.cpp        # Entry point
│       ├── patcher.cpp        # Runtime code patches (font size, spacing)
│       ├── proxy.cpp          # Forwards mixerGetLineInfoA
│       ├── log.h              # File logging
│       └── winmm.def          # Export definition
├── build/                     # One build tree for the whole game
│   ├── pipeline/              #   x64
│   ├── proxy/                 #   Win32
│   └── install/               #   bin/ to run, game/ to copy into the game
└── analysys/                  # Ghidra scripts + resolved issue notes
```

## Characters

夢美=Yumemi, アゲハ=Ageha, ユーリア=Yuria, サユリ=Sayuri, 杏子=Kyouko, 美弥香=Miyaka, 唯=Yui, 綾佳=Ayaka, 遥=Haruka
