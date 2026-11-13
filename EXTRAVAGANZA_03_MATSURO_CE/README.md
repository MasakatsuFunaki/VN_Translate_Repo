# EXTRAVAGANZA ~Matsuro~ CE (末路 / Downfall)

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) — same as EXTRAVAGANZA CE |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | ~6,174 |
| **SPT Files** | mushi.spt (main), mushi0.spt, mushi1.spt, fmb_backscript.spt, sys.spt |
| **Characters** | Miyaka (美弥香), Ayaka (綾佳), Rengoku (煉獄), Shirou (史郎), Collector, Announcer |
| **Delivery** | `winmm.dll` — runtime text hook via `translation_table.tsv`. SPTs are NOT modified (repack caused save/load crashes). |

## Pipeline

```
01_extract          → extracted_text.json
02_build_tsv        → speaker gate → translated_text.json + translation_table.tsv (CP932, tab-separated)
00_run_all          → all in one shot (01_extract → 02_build_tsv → deploy)
03_build_xtx        → name-plate tables (one-shot, once per install, `--game-dir <install>`)
04_patch_menus      → menu sprites + UI configs via `menu_patch.bin` (one-shot, once per install, `--game-dir <install>`)
```

`python build.py --deploy --game-dir <install>` copies `winmm.dll`, `translation_table.tsv`, and `menu_patch.bin` into the game folder.

## Build

```bat
python build.py --test --install   # both halves + tests + stage
python build.py --deploy --game-dir <install>
python build.py --clean

build\install\bin\00_run_all.exe --game-dir <install>
```

`--game-dir <install>` is required: the game install path differs per machine.

Diagnostics: check `proxy_log.txt` in the game dir.
