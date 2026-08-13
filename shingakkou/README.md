# Shingakkou ~Noli me tangere~ DL

| | |
|---|---|
| **Engine** | DDSystem (PIL) — DDP3 archives |
| **Scripts** | 91 files, 52,840 strings (24,736 dialogue + 28,030 narration + 74 choice) |
| **Model** | `claude-opus-4-7` with adaptive thinking |
| **Delivery** | `winmm.dll` — runtime hook, swaps strings via `translation_table.tsv`. `sin_text.dat` is never modified. |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\Shingakkou`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write Python scripts that scan EXE bytes for x86 opcodes.

## Pipeline

```
01_extract.exe      → script_output/extracted_text.json
02_translate.exe    → translation_cache + translation_table.tsv (written to game dir)
build.py --deploy   → winmm.dll into game folder
00_run_all.exe      → all three in one shot
03_find_narrative_cg.exe  → CG image translation (separate, not part of 00_run_all)
```

## Build

```bat
python build.py                    # both halves (pipeline x64 + proxy Win32)
python build.py --test --install   # build, test, stage to build/install/
python build.py --deploy           # ship winmm.dll + table to game
python build.py --clean

set ANTHROPIC_API_KEY=sk-ant-...
build\pipeline\Release\00_run_all.exe
```

Every app takes `--dir <project>` and `--game-dir <install>` (both auto-detected by default).

> `--test N` wipes the cache and re-translates from scratch but does **not** stop after N
> batches — the batch counter is never incremented. Known, left alone.

## Narrative CGs (`03_find_narrative_cg`)

Scans CG archives for images with JP text, translates via Claude vision, renders EN over the
original, repacks. No OCR pre-filter — every image above threshold gets a vision call.

```bat
03_find_narrative_cg.exe --scan-only   # preview, no repack
03_find_narrative_cg.exe --repack      # repack with patched BMPs
```

## Character routes

| Route | Scripts | Leads |
|-------|---------|-------|
| Main | main01–11, op05 | Michael (protagonist) |
| Neil | neil01–12 + endings | Michael, Neil |
| Leo | leo01–11 + endings | Michael, Leonid |
| Cecil | cecil01–11 + endings | Michael, Cecil |
| Gabriel | gabriel01–10 + endings | Michael, Gabriel |
| August | august01–13 + endings | Michael, August |
