# Amakano 2 ~Perfect Edition~ English Translation

Translates the visual novel from Japanese to English using:
- **Anthropic Claude** (swap models via `MODEL` in `pipeline_cpp/src/translate/glossary.h`)
- **Runtime DLL injection** (no archive repacking — translation is applied live by hooking the cs2.exe text rendering path)

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake). A full run
covers all 56,779 MESSAGE lines in `scene.int` and produces a 53,494-entry
`translation_table.tsv` (5,295,712 bytes).

## Requirements

- cmake (>=3.23), conan (>=2), ninja, and the MSVC C++ toolchain
  (VS 2022 with the C++ workload, or the Build Tools) -- the build uses the
  Ninja Multi-Config generator and never MSBuild
- Anthropic API key — set via **one** of:
  - `ANTHROPIC_API_KEY` env var, or
  - `~/.claude_vn/settings.json` (`primaryApiKey`, auto-loaded)

Pass `--game-dir <install>` to every app.  It is required: the game install
path differs per machine.

## Quick Start

```batch
cd amakano2pe
python build.py --test --install

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

`build.py` builds both halves into one tree (`build/pipeline` x64,
`build/proxy` Win32) and stages what a user runs into `build/install`:
`bin/` holds the three executables, `game/` holds the `winmm.dll` that is
copied next to `cs2.exe`.  It takes four flags and no more: `--test`,
`--install`, `--deploy`, `--clean`.  See `SakuraNoUta/BUILD_STRUCTURE.md` for
the layout.

`00_run_all.exe` runs the whole pipeline: extract → translate → deploy.

## Pipeline

Three steps.  The speaker gate and the runtime-table build travel with the
translation step, so everything that must happen for the game to show English
happens inside the command that pays for it.

| Step | Executable | Description |
|------|-----------|-------------|
| 1 | `01_extract.exe` | Walk every CatScene script in `scene.int` (KIF archive, Blowfish + zlib) → `script_output/extracted_text.json` |
| 2 | `02_translate.exe` | Speaker gate (5 checks, zero API tokens), then translate JP → EN via Claude in story order with a rolling 50-line context window (resume-safe, cached), then de-duplicate the pairs into `<game-dir>\translation_table.tsv` |
| 3 | `python build.py --deploy` | Copy `winmm.dll` next to `cs2.exe` (the table is already there) |
| — | `proxy_dll/` | C++ winmm.dll proxy that hooks the cs2.exe text path and swaps JP for EN at draw time |

The gate refuses before an API key is read and before any file is touched: it
verifies that the NAME/MESSAGE speaker state machine still feeds real
`[Speaker]` tags into the prompt, and a refused run exits 2 having written
nothing.

### Step-by-step (manual)

```batch
cd build\install\bin
01_extract.exe                :: scene.int -> extracted_text.json
02_translate.exe --batch 150  :: gate, translate (resume-safe), build the TSV
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

02_translate.exe --batch N                    :: lines per API call (default 150)
02_translate.exe --script 01_TGHGDD_01_k.Hqo  :: single script
02_translate.exe --retranslate                :: ignore cache, redo all
02_translate.exe --discard-cache              :: let --test or --retranslate delete a cache holding paid work
02_translate.exe --out <path>                 :: table path (default <game-dir>\translation_table.tsv)
```

**Default model:** `claude-opus-4-7` (set via `MODEL` in
`pipeline_cpp/src/translate/glossary.h`).

## How the runtime translator works

`proxy_dll/` builds a `winmm.dll` placed alongside `cs2.exe`. cs2.exe imports
`WINMM.dll`, so Windows loads our copy first; we forward all 181 winmm exports
to the real `C:\Windows\System32\winmm.dll` and additionally:

1. Read `translation_table.tsv` (UTF-8) from the game directory.
2. Install an inline x86 hook at `cs2.exe + 0x23A9B0` (function `FUN_0063a9b0`,
   the universal `char* -> wchar_t*` converter — internally calls
   `MultiByteToWideChar(CP_UTF8, ...)`). 14 bytes of prologue are stolen.
3. On each call, look up the input string in the translation map and, on a
   hit, swap the pointer to the English text before the engine renders it.
4. Install a second hook at `cs2.exe + 0x23D000` (`FUN_0063d000`, the global
   `LOGFONTW.lfHeight` setter) and scale every requested font height by
   `FONT_NUM/FONT_DEN` (default 4/5 = 80%) so the engine's auto-wrap fits
   more English per textbox row. Tune the ratio in
   `proxy_dll/src/translator.cpp` if needed.

All hook activity is written to `proxy_log.txt` next to cs2.exe — useful for
checking which strings actually reach the hook.

## Restore Original

```batch
:: Remove the proxy
del "C:\あざらしそふと\アマカノ2～Perfect Edition～\winmm.dll"
del "C:\あざらしそふと\アマカノ2～Perfect Edition～\translation_table.tsv"

:: scene.int is never modified — there's no backup to restore
```

## Files

| File | Description |
|------|-------------|
| `pipeline_cpp/src/apps/` | `00_run_all`, `01_extract`, `02_translate` mains |
| `pipeline_cpp/src/cs2/cs2_archive.{h,cpp}` | Self-contained CatSystem2 KIF/CST parser (no external archive library) |
| `pipeline_cpp/src/extract/` | Extracts text from scene.int |
| `pipeline_cpp/src/translate/` | Anthropic client, glossary, speaker gate, batching/cache |
| `pipeline_cpp/src/build_tsv/` | Writes `translation_table.tsv` next to cs2.exe |
| `pipeline_cpp/tests/` | GoogleTest suites (ctest label `UT`) |
| `proxy_dll/` | C++ source for the runtime translator (`winmm.dll` proxy) |
| `script_output/extracted_text.json` | Extracted script data (generated) |
| `script_output/translation_cache_anthropic.json` | Anthropic cache — resume-safe (generated) |
| `script_output/translated_text.json` | Final translated scripts (generated) |
| `script_output/last_anthropic_translate/` | Last translation run artifacts (generated) |
