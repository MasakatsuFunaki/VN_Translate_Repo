# Amakano 2 ~Perfect Edition~ English Translation

Translates the visual novel from Japanese to English using:
- **Offline translation pipeline** (builds `translation_table.tsv` from the cached translation corpus)
- **Runtime DLL injection** (no archive repacking — translation is applied live)

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake). A full run
covers all 56,779 MESSAGE lines in `scene.int`.

## Requirements

- cmake (>=3.23), conan (>=2), ninja, and the MSVC C++ toolchain
  (VS 2022 with the C++ workload, or the Build Tools) -- the build uses the
  Ninja Multi-Config generator and never MSBuild

Pass `--game-dir <install>` to every app.  It is required: the game install
path differs per machine.

## Quick Start

```batch
cd amakano2pe
python build.py --test --install

build\install\bin\00_run_all.exe
```

`build.py` builds both halves into one tree (`build/pipeline` x64,
`build/proxy` Win32) and takes four flags: `--test`, `--install`,
`--deploy`, `--clean`.

`00_run_all.exe` runs the whole pipeline: extract → translate → deploy.

## Pipeline

| Step | Executable | Description |
|------|-----------|-------------|
| 1 | `01_extract.exe` | Walk every CatScene script in `scene.int` → `script_output/extracted_text.json` |
| 2 | `02_build_tsv.exe` | Speaker gate (5 checks, zero tokens), then apply cached JP → EN translations (resume-safe, cached), then `translation_table.tsv` |
| 3 | `python build.py --deploy` | Copy `winmm.dll` next to `cs2.exe` |

### Step-by-step (manual)

```batch
cd build\install\bin
01_extract.exe                :: scene.int -> extracted_text.json
02_build_tsv.exe --batch 150  :: gate, translate (resume-safe), build the TSV
cd ..\..\.. && python build.py --deploy   :: deploy winmm.dll
```

## Options

```batch
00_run_all.exe --test                :: smoke run: 1 batch only
00_run_all.exe --test 4              :: 4 batches then stop
00_run_all.exe --batch N             :: lines per API call, passed to step 2
00_run_all.exe --clean               :: delete cached JSON, then full run
00_run_all.exe --discard-cache       :: let --test or --clean delete a cache holding paid work
00_run_all.exe --dir <p> --game-dir <p>  :: override project / game folders

02_build_tsv.exe --batch N                    :: lines per API call (default 150)
02_build_tsv.exe --script 01_TGHGDD_01_k.Hqo  :: single script
02_build_tsv.exe --retranslate                :: ignore cache, redo all
02_build_tsv.exe --discard-cache              :: let --test or --retranslate delete a cache holding paid work
02_build_tsv.exe --out <path>                 :: table path (default <game-dir>\translation_table.tsv)
```

## Restore Original

```batch
:: Remove the proxy
del "C:\あざらしそふと\アマカノ2～Perfect Edition～\winmm.dll"
del "C:\あざらしそふと\アマカノ2～Perfect Edition～\translation_table.tsv"

:: scene.int is never modified — there's no backup to restore
```
