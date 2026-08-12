# EXTRAVAGANZA ~Mushi Mederu Shoujo~ Matsuro CE (末路 / Downfall)

> **AGENT — Ghidra is the ONLY allowed EXE disassembly method.**
> This project has a Ghidra setup at `C:\ghidra` with project `D:\ghidra_projects\EXTRAVAGANZA_MATSURO_CE`.
> When you need to analyze the game EXE (find functions, cross-references, string refs, struct offsets, opcode patterns):
> - Write a `.java` GhidraScript and run it via `analyzeHeadless.bat` (see `analysys/analyze_dump.bat`)
> - **NEVER** write ad-hoc scripts that manually scan EXE bytes for x86 opcodes (push/mov/call patterns, ModRM parsing, etc.)
> - Ghidra already does this correctly with full disassembly, control flow, and type recovery
> - Manual hex-based x86 analysis wastes tokens and produces fragile, incomplete results
> - Existing Ghidra scripts: `analysys/ghidra_*.java`, `analysys/analyze_text_rendering*.java`

English translation pipeline for **EXTRAVAGANZA Matsuro CE** — a dark side-story in the EXTRAVAGANZA universe by BLACKCyc.

## Game Info

| Property | Value |
|----------|-------|
| Engine | nnn/BCmkri (BLACKCyc) — same as EXTRAVAGANZA CE |
| Format | SPT (XOR 0xFF encrypted, CP932) |
| JP Strings | ~6,174 |
| SPT Files | mushi.spt (main), mushi0.spt, mushi1.spt, fmb_backscript.spt, sys.spt |
| Characters | Miyaka (美弥香), Ayaka (綾佳), Rengoku (煉獄), Shirou (史郎), Collector, Announcer |

## Pipeline

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake). Three steps
run in sequence, each writing the input of the next:

```
01_extract.exe          → script_output/extracted_text.json
02_translate.exe        → script_output/translated_text.json
                        + script_output/translation_cache_anthropic.json
                        + <GAME_DIR>\translation_table.tsv
python build.py --deploy → winmm.dll into the game folder
```

`00_run_all.exe` performs all three in one command.

`02_translate` is the whole translation step: the zero-token speaker gate runs
first and stops the run before a key is read if a speaker name no longer
reaches the prompt, then the batched translation runs, then the runtime table
is built from the result. There is nothing to remember between paying for a
translation and playing.

`03_translate_xtx.exe` is a side tool, not a pipeline step: it rewrites the
`.xtx` name-plate tables **inside the game installation**, so it is run once
per install rather than once per translation run.

**Runtime translation**: Unlike other games in this project, Matsuro CE does NOT
modify SPT files. Instead, the proxy DLL hooks the game's text splitter function
at runtime and replaces Japanese text on-the-fly using `translation_table.tsv`.
This avoids SPT structural corruption that caused crashes on save/load.

## Build

The game builds from two halves that cannot share one CMake configure:
`pipeline_cpp` is x64, `proxy_dll` is Win32. The root `CMakeLists.txt` drives
both into a single `build/` tree; `build.py` is the supported front end and has
exactly four flags. See `SakuraNoUta/BUILD_STRUCTURE.md` for the layout.

```bash
cd EXTRAVAGANZA_MATSURO_CE
python build.py --test --install   # build both halves, run every test, stage
python build.py --clean            # delete build/
```

`--install` stages `build/install/bin` (`00_run_all`, `01_extract`,
`02_translate`, `03_translate_xtx`) and `build/install/game` (`winmm.dll`).
They are separate folders on purpose: `winmm.dll` is 32-bit and carries a
system DLL's name, so beside the x64 executables it is something the loader
would pick up and then refuse.

```bash
set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

## Individual Steps

```bash
cd build\pipeline\Release
01_extract.exe
02_translate.exe --batch 50
02_translate.exe --test              # smoke run: 1 batch, writes a run log
02_translate.exe --max-batches 4     # cap at 4 requests, no run log
02_translate.exe --file mushi.spt
03_translate_xtx.exe                 # once per game install
```

All apps take `--dir <project>` and `--game-dir <install>`. `--dir` defaults to
the project folder found by walking up from the executable's own location, and
is required when the exe sits outside the project tree.

## Notes

- Same engine and SPT format as EXTRAVAGANZA CE — extraction tooling is identical
- Original SPT files are NOT modified — keep `spt_backup/` as-is for reference
- `translation_table.tsv` is CP932-encoded, tab-separated (JP→EN), one line per string
- The proxy DLL applies 10 patches (fontSize 24→14, spacing, line count) + text hook
- Word wrapping at `MAX_LINE_CHARS=60` (set in `pipeline_cpp/src/build_tsv/build_tsv.h`)
- Check `proxy_log.txt` in game dir for hook/patch diagnostics
