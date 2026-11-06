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
```

Options: `--batch 50`, `--test` (1 batch), `--max-batches 2`, `--file mushi.spt`.

`--clean` and `--retranslate` refuse a cache that already holds paid work.
Add `--discard-cache` to let them run.

`--game-dir <install>` is required: the game install path differs per machine.

Diagnostics: check `proxy_log.txt` in the game dir.
