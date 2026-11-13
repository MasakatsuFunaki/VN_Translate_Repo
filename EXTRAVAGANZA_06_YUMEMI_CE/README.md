# YUMEMIRYOJOKU_CE (『夢美陵辱』 / Yumemi Ryoujoku Complete Edition)

English translation pipeline (`pipeline_cpp/`, x64) and runtime hook / keyless menu auto-patcher (`proxy_dll/`, Win32 `winmm.dll`) for BLACKCyc's `System-NNN` visual novel *EXTRAVAGANZA 『夢美陵辱』 Complete Edition*.

## Architecture

- **`pipeline_cpp/` (x64, preset `conan-windows-x64-release`)**:
  - `01_extract.exe`: Decrypts `spt\*.spt` (`m_tokuten.spt`, `backscript.spt`, `sys.spt`) via XOR `0xFF` and extracts CP932 strings to `script_output/extracted_text.json`.
  - `00_run_all.exe`: End-to-end orchestrator (`01_extract` -> `02_build_tsv` speaker gate + `gemini-4-argon` batch translation + `translation_table.tsv` -> `winmm.dll` deploy).
  - `02_build_tsv.exe`: Runs the speaker gate, translates via Gemini (`gemini-4-argon`), writes `script_output/translated_text.json` and `script_output/translation_cache_gemini.json`, word-wraps translated strings, and generates `translation_table.tsv` in CP932 keyed by Japanese text (`jp\ten\n`).
  - `03_build_xtx.exe`: Translates speaker nameplates in `spt\charaname.xtx` to fullwidth romaji (with backup in `xtx_backup\charaname.xtx`).
  - `04_patch_menus.exe`: Applies the keyless `"YUMMENU1"` v1 XOR-delta `menu_patch.bin` to `dwq\sys0.{gpk,gtb}` (`PACKTYPE=0`, `2A`, `3A`, `8A`), `init2\mainControl.fxf`, `init2\backlog.fxf`, and `spt\charaname.xtx` in `--game-dir` while preserving `.bak` backups.
- **`proxy_dll/` (Win32, preset `conan-windows-x86-release`)**:
  - Builds `winmm.dll` forwarding all 181 `WINMM.dll` exports to `<System32>\winmm.dll`.
  - On startup, acquires `Local\YumemiCeMenuPatchMutex`, verifies/applies `menu_patch.bin` idempotently with `.bak` backups, and hooks `StringPoolLookup`, `GetGlyphOutlineA`, `CreateFontIndirectA`, and `SetWindowTextA`.

## CLI Options

- `00_run_all.exe`: `--help` (`-h`), `--dir <project>` (auto-detected by walking up from the executable), `--game-dir <install>` (required), `--batch <n>` (default `150`), `--max-batches <n>` (default `0`), `--clean`, `--discard-cache` (`--clean` refuses to delete a cache holding paid work unless `--discard-cache` is also passed).
- `01_extract.exe`: `--help` (`-h`), `--dir <project>`, `--game-dir <install>` (required).
- `02_build_tsv.exe`: `--help` (`-h`), `--dir <project>`, `--game-dir <install>` (required), `--batch <n>` (default `150`), `--retranslate`, `--max-batches <n>` (default `0`), `--out <path>`, `--rebuild-table`, `--discard-cache` (`--retranslate` refuses a non-empty cache unless `--discard-cache` is also passed).
- `03_build_xtx.exe`: `--help` (`-h`), `--game-dir <install>` (required).
- `04_patch_menus.exe`: `--help` (`-h`), `--game-dir <install>` (required).

## Build

```powershell
python build.py --test --install
python build.py --deploy --game-dir "C:\path\to\YUMEMIRYOJOKU_CE"
```
