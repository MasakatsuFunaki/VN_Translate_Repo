# EXTRA VA MIZUNA ~EXTRAVAGANZA~ Complete Edition (`EXTRA_VA_MIZUNA_CE`)

Complete English translation pipeline and runtime proxy DLL patch for BLACK Cyc's
**『EXTRA VA MIZUNA ～EXTRAVAGANZA～ Complete Edition』** (`mizuna_CE.exe`, System-NNN engine).

## Architecture

- **`pipeline_cpp/` (x64)**:
  - `01_extract.exe`: Decrypts `spt/*.spt` (`~byte` bitwise NOT) and extracts all script strings to `script_output/extracted_text.json`.
  - `02_build_tsv.exe`: Translates strings with `gemini-4-argon` (`127.0.0.1:8000`) and builds `translations.tsv` and `translation_table.tsv` with 52-column monospace word-wrap (`<= 4` lines per page).
  - `03_build_xtx.exe`: Translates `spt/charaname.xtx` via the System-NNN two-table substitution cipher (`水菜` -> `Ｍｉｚｕｎａ`, `史郎` -> `Ｓｈｉｒｏｕ`).
  - `04_patch_menus.exe`: Translates `init2/mainControl.fxf` window titles, renders English UI overlay sprites into `dwq/sys0.gpk` + `dwq/sys0.gtb` (`PACKTYPE=3A` `PACKBMP+MASK` and `PACKTYPE=0` raw BMP), and packs the keyless XOR-delta archive `menu_patch.bin` (`MIZMENU1`).
  - `00_run_all.exe`: Runs the complete translation, verification, and packaging pipeline end-to-end.
- **`proxy_dll/` (Win32 `winmm.dll`)**:
  - Hooks `StringPoolLookup`, `PrintMessage` font pitch (`12` px half-width monospace), and `TextSplitter` (`maxCharsPerLine = 52`) in `mizuna_CE.exe` (via static candidate RVAs and dynamic unpacked `.text` pattern scanning).
  - Applies `menu_patch.bin` idempotently on startup (`Local\MizunaCeMenuPatchMutex`), preserving `.bak` originals before any in-place modification.

## Build & Test

```powershell
python build.py --test --install
python build.py --deploy --game-dir "C:\path\to\EXTRA_VA_MIZUNA_CE"
```
