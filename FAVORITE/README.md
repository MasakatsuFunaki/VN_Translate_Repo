# さくら、もゆ。 -as the Night's, Reincarnation- (FAVORITE) — English Translation

Pipeline for translating the FAVORITE / FVP visual novel installed at
`c:\Favorite\` (Sakura.exe + Sakura.hcb).

- **Engine:** FAVORITE / FVP (FavoriteViewPoint)
- **Translator:** Google Gemini (`gemini_client` in `pipeline_cpp/src/translate/`, toggle `MODEL` in
  `pipeline_cpp/src/translate/glossary.h`, caches in `script_output/translation_cache_gemini.json`)
- **Runtime:** `winmm.dll` proxy DLL alongside Sakura.exe

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake).

## Status

| Step | State |
|------|-------|
| `01_extract.exe` (Sakura.hcb → 61,164 strings) | ✅ working |
| `02_translate.exe` (cast gate, Gemini, resume-safe, writes `translation_table.tsv`) | ✅ working |
| `proxy_dll/` (winmm forwarding + TSV loader + inline hook) | ✅ live, English visible in-game |
| Translation corpus | ⏳ run `00_run_all.exe` to translate all ~55K JP lines |

## Prerequisites

- cmake (>=3.23), conan (>=2), ninja, and the MSVC C++ toolchain (VS 2022
  or its Build Tools) -- the build uses Ninja Multi-Config, never MSBuild
- Google Gemini API key — set via **one** of:
  - `GEMINI_API_KEY` env var, or
  - `~/.gemini/.env` (`GEMINI_API_KEY=...`, auto-loaded)

## Quick Start

```batch
cd FAVORITE
python build.py --test --install

set GEMINI_API_KEY=AIza...
build\install\bin\00_run_all.exe --test 1  :: 1 batch smoke test
:: or
build\install\bin\00_run_all.exe           :: full run, ~55K JP lines
```

`build.py` builds both halves — `pipeline_cpp` is x64, `proxy_dll` is Win32 —
into one build tree, and takes exactly four flags: `--test`, `--install`,
`--deploy`, `--clean`.

The pipeline is three steps, and `00_run_all.exe` performs all three:

| Step | Command | Description |
|------|---------|-------------|
| 1 | `01_extract.exe` | Walk `Sakura.hcb` → `script_output/extracted_text.json` |
| 2 | `02_translate.exe` | Cast-roster gate, then translate JP → EN via Gemini (resume-safe, cached in `translation_cache_gemini.json`), then write `translation_table.tsv` |
| 3 | `python build.py --deploy` | Copy `winmm.dll` into the game folder |

## Options

```batch
00_run_all.exe --test                :: smoke run: 1 batch only
00_run_all.exe --test 4              :: 4 batches then stop
00_run_all.exe --max-batches 4       :: 4 batches, keeping cache and outputs
00_run_all.exe --batch N             :: lines per API call (default 150)
00_run_all.exe --clean               :: delete cached JSON, then full run
00_run_all.exe --discard-cache       :: let --test or --clean delete a cache holding paid work
00_run_all.exe --dir <p> --game-dir <p>  :: project folder / game folder (--game-dir required)

02_translate.exe --batch N           :: lines per API call (default 150)
02_translate.exe --retranslate       :: ignore cache, redo all
02_translate.exe --discard-cache     :: let --test or --retranslate delete a cache holding paid work
02_translate.exe --out <path>        :: table path (default <game-dir>\translation_table.tsv)
```

`--test N` is a fresh run: it drops the cache first.
`--max-batches N` caps the work without costing what has already been paid for.

Model: `gemini-4-argon` (`MODEL` in
`pipeline_cpp/src/translate/glossary.h`.
