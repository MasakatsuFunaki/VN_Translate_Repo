# Adieu (さよならを教えて ～comment te dire adieu～)

English translation pipeline for **Sayonara o Oshiete ~comment te dire adieu~** by **CRAFTWORK** (VisualArt's SiglusEngine DL Edition).

| | |
|---|---|
| **Engine** | VisualArt's SiglusEngine |
| **Delivery** | Deployed into `<install>\StartData\GameData\`. Copies `winmm.dll`, `translation_table.tsv`, and `menu_patch.bin` to translate story dialogue, menus, and dialogs at runtime (original UI files are backed up automatically on first launch). |

## Pipeline

```
01_extract.exe           → script_output/extracted_text.json
03_speaker_gate.exe      → offline speaker attribution & glossary check
02_translate.exe         → translates extracted lines via Gemini API and builds
                           translation_table.tsv
02_build_tsv.exe         → rebuilds translation_table.tsv offline
04_patch_menus.exe       → builds menu_patch.bin for UI graphics & config
python build.py --deploy → copies winmm.dll, translation_table.tsv, and
                           menu_patch.bin into <install>\StartData\GameData\
00_run_all.exe           → runs all steps in one command
```

## Quick Start

```powershell
cd Adieu
python build.py --test --install

build\install\bin\00_run_all.exe --game-dir "<install>\StartData\GameData"
build\install\bin\02_build_tsv.exe --rebuild-table --game-dir "<install>\StartData\GameData"
python build.py --deploy --game-dir "<install>\StartData\GameData"
```
