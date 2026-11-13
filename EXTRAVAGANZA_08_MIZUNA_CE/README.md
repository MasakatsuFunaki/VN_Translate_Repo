# EXTRA VA MIZUNA ~EXTRAVAGANZA~ Complete Edition (`EXTRA_VA_MIZUNA_CE`)

Complete English translation pipeline and runtime proxy DLL patch for BLACK Cyc's
**『EXTRA VA MIZUNA ～EXTRAVAGANZA～ Complete Edition』** (`mizuna_CE.exe`, System-NNN engine).

## Architecture

- **`pipeline_cpp/` (x64)**:
  - `01_extract.exe`: Decrypts `spt/*.spt` (`~byte` bitwise NOT) and extracts all script strings to `script_output/extracted_text.json`.
  - `02_build_tsv.exe`: Runs the speaker gate, translates strings via Gemini (`gemini-4-argon`), writes `script_output/translated_text.json` and `script_output/translation_cache_gemini.json`, and generates `translation_table.tsv` in CP932 keyed by Japanese text (`jp\ten\n`) with 52-column monospace word-wrap (`<= 4` lines per page).
  - `03_build_xtx.exe`: Translates `spt/charaname.xtx` via fullwidth romaji speaker nameplates (`水菜` -> `Ｍｉｚｕｎａ`, `史郎` -> `Ｓｈｉｒｏｕ`) with backup in `xtx_backup/charaname.xtx`.
  - `04_patch_menus.exe`: Applies the keyless XOR-delta archive `menu_patch.bin` (`MIZMENU1`) to `dwq/sys0.gpk` + `dwq/sys0.gtb`, `init2/mainControl.fxf`, `init2/backlog.fxf`, and `spt/charaname.xtx` (`--game-dir <dir>`).
  - `00_run_all.exe`: End-to-end orchestrator (`01_extract` -> `02_build_tsv` -> `winmm.dll` deploy).
- **`proxy_dll/` (Win32 `winmm.dll`)**:
  - Hooks `StringPoolLookup`, `PrintMessage` font pitch (`12` px half-width monospace), and `TextSplitter` (`maxCharsPerLine = 52`) in `mizuna_CE.exe` (via static candidate RVAs and dynamic unpacked `.text` pattern scanning).
  - Applies `menu_patch.bin` idempotently on startup (`Local\MizunaCeMenuPatchMutex`), preserving `.bak` originals before any in-place modification.

## CLI Options

- `00_run_all.exe`: `--help` (`-h`), `--dir <project>` (auto-detected by walking up from the executable), `--game-dir <install>` (required), `--batch <n>` (default `150`), `--max-batches <n>` (default `0`), `--clean`, `--discard-cache` (`--clean` refuses to delete a cache holding paid work unless `--discard-cache` is also passed).
- `01_extract.exe`: `--help` (`-h`), `--dir <project>`, `--game-dir <install>` (required).
- `02_build_tsv.exe`: `--help` (`-h`), `--dir <project>`, `--game-dir <install>` (required), `--batch <n>` (default `150`), `--retranslate`, `--max-batches <n>` (default `0`), `--out <path>`, `--rebuild-table`, `--discard-cache` (`--retranslate` refuses a non-empty cache unless `--discard-cache` is also passed).
- `03_build_xtx.exe`: `--help` (`-h`), `--game-dir <install>` (required).
- `04_patch_menus.exe`: `--help` (`-h`), `--game-dir <install>` (required).

## Build & Test

```powershell
python build.py --test --install
python build.py --deploy --game-dir "C:\path\to\EXTRA_VA_MIZUNA_CE"
```
