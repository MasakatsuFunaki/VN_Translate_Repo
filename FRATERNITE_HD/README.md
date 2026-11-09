# Fraternite HD Remaster (フラテルニテ HDリマスター)

English translation pipeline for **Fraternite HD Remaster** by **CLOCKUP**.

| | |
|---|---|
| **Engine** | YU-RIS (Yuris) |
| **Delivery** | `winmm.dll` draws the English in an overlay (F1 toggles JP). The game files are never modified. |

## Pipeline

```
01_extract.exe          → script_output/extracted_text.json
02_build_tsv.exe        → the speaker gate, then
                          script_output/translated_text.json,
                          translation_cache_anthropic.json, and
                          translation_table.tsv in the game folder
python build.py --deploy → winmm.dll into the game folder
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
01_extract.exe
02_build_tsv.exe --batch 150
02_build_tsv.exe --test              # smoke run: 1 batch only, from a wiped cache
02_build_tsv.exe --max-batches 4     # 4 batches, keeping the cache and outputs
02_build_tsv.exe --discard-cache     # let --test or --retranslate delete a cache holding paid work
02_build_tsv.exe --file bn.ypf
02_build_tsv.exe --out D:\scratch\table.tsv   # keeps the game folder untouched
cd ..\..\.. && python build.py --deploy --game-dir <install>
```

All apps take `--dir <project>` and `--game-dir <install>`. `--game-dir` is
required. `--dir` defaults to the project folder found by walking up from the exe.
