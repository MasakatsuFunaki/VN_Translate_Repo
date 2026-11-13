# Fraternite HD Remaster (フラテルニテ HDリマスター)

English translation pipeline for **Fraternite HD Remaster** by **CLOCKUP**.

| | |
|---|---|
| **Engine** | YU-RIS (Yuris) |
| **Delivery** | `winmm.dll` draws the English in an overlay (F1 toggles JP). `pac/cgsys.ypf` is backed up to `pac/cgsys.ypf.bak` and patched with translated menu sprites (`cgsys_patch.bin`), while script archives remain untouched. |

## Pipeline

```
01_extract.exe          → script_output/extracted_text.json
02_build_tsv.exe        → the speaker gate, then
                          script_output/translated_text.json,
                          translation_cache_anthropic.json, and
                          translation_table.tsv in the game folder
python build.py --deploy → winmm.dll, translation_table.tsv, and cgsys_patch.bin
                          into the game folder (plus pac/cgsys.ypf menu patch)
00_run_all.exe          → all three in one command
```

## Quick Start

```powershell
cd FRATERNITE_HD
python build.py --test --install

build\install\bin\00_run_all.exe --game-dir <install>
```

### Encrypted Script Storage (`*.json.enc`)

Across all shipped games, `script_output/translated_text.json` and `script_output/translation_cache_*.json` are tracked in git as `VNE2`-encrypted `.json.enc` files using the public passphrase in the repository root `translations_key.txt`. Both `python build.py` and `spot_checker` automatically and idempotently sync `.json <-> .json.enc` via the repository root `encrypt_translations.bat` and `decrypt_translations.bat` scripts:

```powershell
:: manual sync for this game from the repository root
.\encrypt_translations.bat --encrypt --dir FRATERNITE_HD
.\decrypt_translations.bat --dir FRATERNITE_HD
```

## Individual Steps

```powershell
cd build\install\bin
01_extract.exe --game-dir <install>
02_build_tsv.exe --game-dir <install>
02_build_tsv.exe --game-dir <install> --rebuild-table
02_build_tsv.exe --game-dir <install> --out D:\scratch\table.tsv   # keeps the game folder untouched
03_patch_menus.exe --game-dir <install>                            # patches pac/cgsys.ypf via cgsys_patch.bin
cd ..\..\.. && python build.py --deploy --game-dir <install>
```

`00_run_all`, `01_extract`, and `02_build_tsv` accept `--dir <project>` (defaulting to the project folder found by walking up from the executable) and require `--game-dir <install>`. `03_patch_menus` requires `--game-dir <install>` and resolves `cgsys_patch.bin` automatically next to the project or executable.
