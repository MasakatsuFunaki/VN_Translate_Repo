# Fraternite HD Remaster (フラテルニテ HDリマスター)

English translation pipeline for **Fraternite HD Remaster** by **CLOCKUP**.

| | |
|---|---|
| **Engine** | YU-RIS (Yuris) |
| **Delivery** | `winmm.dll` draws the English in an overlay (F1 toggles JP). The game files are never modified. |

## Pipeline

```
01_extract.exe          → script_output/extracted_text.json
02_translate.exe        → the speaker gate, then
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

build\install\decrypt_translations.bat

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe --game-dir <install>
```

**WARNING:** Run `decrypt_translations.bat` before the pipeline. Step 2 reads the
encrypted cache and document. Without them it re-translates the whole game (hours,
real money). Nothing in the pipeline can rebuild those files.

### Why the script is encrypted

The two JSON files are the whole English script of an adult title. The repo is
public, so they are tracked as `.enc` only. The passphrase in `translations_key.txt`
is public — this keeps the script out of search and crawlers, not secret.

```powershell
cd FRATERNITE_HD

:: after a translation run
.\encrypt_translations.bat --encrypt

:: to read them again
.\encrypt_translations.bat --decrypt
```

## Individual Steps

```powershell
cd build\install\bin
01_extract.exe
02_translate.exe --batch 150
02_translate.exe --test              # smoke run: 1 batch only, from a wiped cache
02_translate.exe --max-batches 4     # 4 batches, keeping the cache and outputs
02_translate.exe --discard-cache     # let --test or --retranslate delete a cache holding paid work
02_translate.exe --file bn.ypf
02_translate.exe --out D:\scratch\table.tsv   # keeps the game folder untouched
cd ..\..\.. && python build.py --deploy --game-dir <install>
```

All apps take `--dir <project>` and `--game-dir <install>`. `--game-dir` is
required. `--dir` defaults to the project folder found by walking up from the exe.
