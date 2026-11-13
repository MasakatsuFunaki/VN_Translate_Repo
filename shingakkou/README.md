# Shingakkou ~Noli me tangere~ DL

| | |
|---|---|
| **Engine** | DDSystem (PIL) — DDP3 archives |
| **Scripts** | 91 files, 52,840 strings (24,736 dialogue + 28,030 narration + 74 choice) |
| **Delivery** | `winmm.dll` — runtime hook, swaps strings via `translation_table.tsv` and auto-patches `Data\sin_sysd.dat` + `Bootup.dat` on first launch from keyless XOR-delta `menu_patch.bin` (creating `.bak` backups). `sin_text.dat` is never modified. |

In-game Config → フォント defaults to **UD デジタル 教科書体 N** (patched in `Bootup.dat` `#00001` and `Data\sin_sysd.dat` `#00121`, and enforced at runtime by `winmm.dll`). The glyph advance is a fixed
8px tuned to that font, so any other one renders cramped. Stored per install in `Save/SYSTEM.SAV`.

## Pipeline

```
01_extract.exe            → script_output/extracted_text.json
02_build_tsv.exe          → translation_cache + translation_table.tsv (written to game dir)
04_patch_menus.exe        → applies menu_patch.bin to Data\sin_sysd.dat + Bootup.dat (also run automatically by winmm.dll on first launch)
build.py --deploy         → winmm.dll + translation_table.tsv + menu_patch.bin into game folder
00_run_all.exe            → extract + translate + deploy in one shot
03_find_narrative_cg.exe  → CG image translation (separate, not part of 00_run_all)
```

## Build

```bat
python build.py                    # both halves (pipeline x64 + proxy Win32)
python build.py --test --install   # build, test, stage to build/install/
python build.py --deploy --game-dir <install>   # ship winmm.dll + table + menu_patch.bin to game
python build.py --clean

build\pipeline\Release\00_run_all.exe --game-dir <install>
```

`00_run_all.exe`, `01_extract.exe`, `02_build_tsv.exe`, and `03_find_narrative_cg.exe`
take `--dir <project>` and `--game-dir <install>` (`--dir` is auto-detected;
`--game-dir` is required because the game install path differs per machine).
`02_build_tsv.exe` also accepts `--out <path>` and `--rebuild-table`.
`04_patch_menus.exe` takes `--game-dir <install>` (plus optional `--verify`).

## Narrative CGs (`03_find_narrative_cg`)

Renders cached English overlays over narrative CG images and repacks the DDP archives.

```bat
03_find_narrative_cg.exe --game-dir <install> --repack   # repack with patched BMPs
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
