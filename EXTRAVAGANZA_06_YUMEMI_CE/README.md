# YUMEMIRYOJOKU_CE (『夢美陵辱』 / Yumemi Ryoujoku Complete Edition)

English translation pipeline (`pipeline_cpp/`, x64) and runtime hook / keyless menu auto-patcher (`proxy_dll/`, Win32 `winmm.dll`) for BLACKCyc's `System-NNN` visual novel *EXTRAVAGANZA 『夢美陵辱』 Complete Edition*.

## Architecture

- **`pipeline_cpp/` (x64, preset `conan-windows-x64-release`)**:
  - `01_extract.exe`: Decrypts `spt\*.spt` (`m_tokuten.spt`, `backscript.spt`, `sys.spt`) via XOR `0xFF` and extracts CP932 strings to `script_output/extracted_text.json`.
  - `00_run_all.exe`: End-to-end orchestrator (extract -> speaker gate -> `gemini-4-argon` batch translation -> `02_build_tsv` -> `03_build_xtx` -> deploy).
  - `02_build_tsv.exe`: Word-wraps translated strings and generates `translations.tsv` keyed by `offset_dec` (`file:offset`).
  - `03_build_xtx.exe`: Translates speaker nameplates in `spt\charaname.xtx` to fullwidth romaji and BGM titles in `spt\bgmlist.xtx`.
  - `04_patch_menus.exe`: Extracts/repacks `dwq\sys0.{gpk,gtb}` (`PACKTYPE=0`, `2A`, `3A`, `8A`), patches `init2\*.fxf`, and builds the keyless XOR-delta `menu_patch.bin` (`YUMMENU1` magic).
- **`proxy_dll/` (Win32, preset `conan-windows-x86-release`)**:
  - Builds `winmm.dll` forwarding all 181 `WINMM.dll` exports to `<System32>\winmm.dll`.
  - On startup, acquires `Local\YumemiCeMenuPatchMutex`, verifies/applies `menu_patch.bin` idempotently with `.bak` backups, and hooks `StringPoolLookup`, `GetGlyphOutlineA`, `CreateFontIndirectA`, and `SetWindowTextA`.

## Build

```powershell
python build.py --test --install
python build.py --deploy --game-dir "C:\path\to\YUMEMIRYOJOKU_CE"
```
