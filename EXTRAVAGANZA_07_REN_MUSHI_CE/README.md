# Ren and Mushi-kun's Day — *『レンと蟲クンのいちにち』* (`Ren_mushi_CE`)

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc `System-NNN`) — same as EXTRAVAGANZA CE / AGEHA CE / Matsuro CE |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | 188 translatable (175 unique across 4 SPT files) |
| **SPT Files** | `mushi.spt` (main story), `backscript.spt`, `staff.spt`, `sys.spt` |
| **Characters** | Ren (レン), Yumemi Sakurai (桜井夢美 / 夢美), Emcee (司会者) |
| **Delivery** | `winmm.dll` — runtime text hook via `translation_table.tsv` + first-launch menu/UI auto-patch via `menu_patch.bin` (`"RENMENU1"` v1). SPTs are NOT modified (repack causes save/load crashes). |

## Pipeline

```
01_extract          → extracted_text.json
02_build_tsv        → speaker gate → translated_text.json + translation_table.tsv (CP932, tab-separated)
00_run_all          → all in one shot
03_build_xtx        → name-plate tables (spt/charaname.xtx)
04_patch_menus      → menu/UI DWQ archive (dwq/sys0.gpk + sys0.gtb) & FXF configs (init2/mainControl.fxf, backlog.fxf) via menu_patch.bin
```

`python build.py --deploy --game-dir <install>` copies `winmm.dll`, `menu_patch.bin`, and `translation_table.tsv` into the game folder. On first game launch, `winmm.dll` (`menu_autopatch::EnsureGameFilesPatched()`) verifies `menu_patch.bin` (`"RENMENU1"` v1 keyless XOR-delta stream), backs up unpatched originals (`spt/charaname.xtx.bak`, `init2/mainControl.fxf.bak`, `init2/backlog.fxf.bak`, `dwq/sys0.gtb.bak`, `dwq/sys0.gpk.bak`), and reconstructs the English UI/menu files in place.

## Build

```bat
python build.py --test --install   # both halves + tests + stage
python build.py --deploy --game-dir <install>
python build.py --clean

build\install\bin\00_run_all.exe --game-dir <install>
build\install\bin\02_build_tsv.exe --game-dir <install>
build\install\bin\04_patch_menus.exe --game-dir <install>
```

## CLI Options

- `00_run_all.exe`: `--help` (`-h`), `--dir <project>` (auto-detected by walking up from the executable), `--game-dir <install>` (required), `--batch <n>` (default `150`), `--max-batches <n>` (default `0`), `--clean`, `--discard-cache` (`--clean` refuses to delete a cache holding paid work unless `--discard-cache` is also passed).
- `01_extract.exe`: `--help` (`-h`), `--dir <project>`, `--game-dir <install>` (required).
- `02_build_tsv.exe`: `--help` (`-h`), `--dir <project>`, `--game-dir <install>` (required), `--batch <n>` (default `150`), `--retranslate`, `--max-batches <n>` (default `0`), `--out <path>`, `--rebuild-table`, `--discard-cache` (`--retranslate` refuses a non-empty cache unless `--discard-cache` is also passed).
- `03_build_xtx.exe`: `--help` (`-h`), `--game-dir <install>` (required).
- `04_patch_menus.exe`: `--help` (`-h`), `--game-dir <install>` (required).

Diagnostics: check `proxy_log.txt` in the game dir.
