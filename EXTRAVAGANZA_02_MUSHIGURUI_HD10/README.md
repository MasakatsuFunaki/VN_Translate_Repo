# Mushigurui (HD10 & Complete Edition)

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) |
| **Supported Editions** | `mushigurui_HD10.exe` (HD10), `mushigurui_HD10_Win7.exe` (HD10 Win7), and `mushigurui.exe` (EXTRAVAGANZA Complete Edition) |
| **Format** | SPT (`XOR 0xFF`, CP932), FXF (`XOR 0xFF`, CP932), GPK/GTB (`"over2G!\0"` dual 32/64-bit offset tables, DWQ/PACKBMP/PNG) |
| **JP Strings** | 13,105 (`12,384` unique lookup keys + 22 runtime UI fallback strings) |
| **SPT Files** | `s100`–`s400` (story arcs), `etc.spt`, `BCmkri_backscript.spt`, `sys.spt` |
| **Characters** | Ren (レン), Miyaka (美弥香), Ayaka (綾佳), Yuria (ユーリア), Shio (シオ), Yumemi (夢美), Kazenomiya (風ノ宮), Kyou (キョウ) |
| **Delivery** | Pure drag-and-drop: `winmm.dll` + `translation_table.tsv` + `menu_patch.bin`. On first launch, `winmm.dll` automatically backs up (`.bak`) and patches `dwq\sys0`, `dwq\ta0`, `dwq\ev0`, `init2\mainControl.fxf`, `init2\backlog.fxf`, and `spt\charaname.xtx` (menus, options, dialogue HUD, save/load, backlog, gallery, and all 7 narrative/omake CGs); SPT archives stay untouched. |

## Pipeline

```text
01_extract          -> extracted_text.json
02_build_tsv        -> speaker gate -> translated_text.json + translation_table.tsv
00_run_all          -> extract + translate + deploy in one shot
```

Side tools (not run by `00_run_all`):

```text
03_build_xtx              -> config name plates (`--game-dir <install>`)
04_find_narrative_cg      -> narrative CG discovery & GPK repack (`--game-dir <install>`)
05_replace_choice_text    -> choice-menu screenshot translator (`choices/*.jpg` -> `*_en.jpg`)
06_patch_menus            -> offline `"MSGMENU1"` v1 `menu_patch.bin` builder (`--game-dir <hd10> --ce-game-dir <ce>`)
```

`python build.py --deploy --game-dir <install>` copies `winmm.dll`, `translation_table.tsv`, and `menu_patch.bin` into either an HD10 or Complete Edition install folder.

## Build

```bat
python build.py --test --install
python build.py --deploy --game-dir <install>
python build.py --clean

build\install\bin\00_run_all.exe --game-dir <install>
```

### CLI Options

- `00_run_all.exe`, `01_extract.exe`: `-h,--help`, `--dir <project>` (auto-detected), `--game-dir <install>` (required).
- `02_build_tsv.exe`: `-h,--help`, `--dir <project>` (auto-detected), `--game-dir <install>` (required), `--out <path>`, `--rebuild-table`.
- `03_build_xtx.exe`: `-h,--help`, `--game-dir <install>` (required).
- `04_find_narrative_cg.exe`: `-h,--help`, `--dir <project>` (auto-detected), `--game-dir <install>` (required), `--repack`.
- `05_replace_choice_text.exe`: `-h,--help`, `--dir <project>` (auto-detected).
- `06_patch_menus.exe`: `-h,--help`, `--game-dir <hd10_dir>` (required), `--ce-game-dir <ce_dir>` (required), `--output <path>` (defaults to `<project>\menu_patch.bin`).

## Menu & Narrative CG Patch Bundle (`06_patch_menus`)

```bat
build\install\bin\06_patch_menus.exe --game-dir "<pristine_hd10_dir>" --ce-game-dir "<pristine_ce_dir>"
```

Generates the keyless XOR-masked `"MSGMENU1"` v1 `menu_patch.bin` covering both editions (`init2\mainControl.fxf` `480` B HD10 + `468` B CE variants, `init2\backlog.fxf`, `spt\charaname.xtx`, and `74` DWQ entries across `dwq\sys0`, `dwq\ta0`, and `dwq\ev0`).
