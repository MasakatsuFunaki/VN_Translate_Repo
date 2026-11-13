# AGEHA ~EXTRAVAGANZA~ CE (九条アゲハ / Ageha Kujou)

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) — same as EXTRAVAGANZA CE / Matsuro CE |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | ~5,279 translatable (5,085 unique across 4 SPT files) |
| **SPT Files** | main.spt (main story), agh_backscript.spt, agh_test01.spt, sys.spt |
| **Characters** | Ageha Kujou (九条アゲハ), Shumi (シュミ), Shirou Kitabatake (北畠史郎), Ren (レン), Nishi (西) |
| **Delivery** | `winmm.dll` — runtime text hook via `translation_table.tsv` + first-launch menu/UI auto-patch via `menu_patch.bin` (`"AGHMENU1"` v1). SPTs are NOT modified (repack caused save/load crashes). |

## Pipeline

```
01_extract          → extracted_text.json
02_build_tsv        → speaker gate → translated_text.json + translation_table.tsv (CP932, tab-separated)
00_run_all          → all in one shot
03_build_xtx        → name-plate tables (spt/charaname.xtx)
04_patch_menus      → menu/UI DWQ archive (dwq/sys0.gpk + sys0.gtb) & FXF configs (init2/mainControl.fxf, backlog.fxf) via menu_patch.bin
```

`python build.py --deploy --game-dir <install>` copies `winmm.dll`, `menu_patch.bin`, and `translation_table.tsv` into the game folder. On first game launch, `winmm.dll` (`menu_autopatch::EnsureGameFilesPatched()`) verifies `menu_patch.bin` (`"AGHMENU1"` v1 keyless XOR-delta stream), backs up unpatched originals (`spt/charaname.xtx.bak`, `init2/mainControl.fxf.bak`, `init2/backlog.fxf.bak`, `dwq/sys0.gtb.bak`, `dwq/sys0.gpk.bak`), and reconstructs the English UI/menu files in place.

## Build

```bat
python build.py --test --install   # both halves + tests + stage
python build.py --deploy --game-dir <install>
python build.py --clean

build\install\bin\00_run_all.exe --game-dir <install>
```

Options: `--batch 50`, `--max-batches 2`, `--clean`, `--discard-cache`.

`--clean` and `--retranslate` refuse a cache that already holds paid work.
Add `--discard-cache` to let them run.

`--game-dir <install>` is required: the game install path differs per machine.

Diagnostics: check `proxy_log.txt` in the game dir.
