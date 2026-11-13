# WelcomeToAnzU_CE — English Translation Pipeline & Runtime Proxy

English translation pipeline (`pipeline_cpp/`, x64) and runtime hook/auto-patcher (`proxy_dll/`, Win32 `winmm.dll`) for **『うぇるかむ★とぅ AnzU！』** (*Welcome to AnzU!* — Complete Edition) on the BLACKCyc `System-NNN` (`BCmkri`) engine.

## Architecture

- **`pipeline_cpp/` (x64, `conan-windows-x64-release`)**:
  - `00_run_all.exe`: Full pipeline orchestrator.
  - `01_extract.exe`: Decrypts `spt\*.spt` (`tokuten.spt`, `backscript.spt`, `sys.spt`) via XOR `0xFF`, validates the `SPTHEADER0` header, and extracts `818` translatable blocks (`812` unique strings across `3` `.spt` files) to `script_output/extracted_text.json`.
  - `02_build_tsv.exe`: Runs the 5-stage speaker gate, translates via Gemini (`gemini-4-argon`), writes `script_output/translated_text.json` and `script_output/translation_cache_gemini.json`, and compiles `translations.tsv` / `translation_table.tsv` (`819` entries, wrapping at 52 chars/line and merging `"Speaker\r\n\"Line\""` into `"Speaker: \"Line\""`).
  - `03_build_xtx.exe`: Translates `spt\charaname.xtx` speaker name plates (`5` speakers) to fullwidth English romaji (`Ｙｕｍｅｍｉ`, `Ａｇｅｈａ`, `Ｈａｒｕｋａ`, `Ａｎｚｕ`, `Ｍｉｙａｋａ`) while preserving `xtx_backup\charaname.xtx`.
  - `04_patch_menus.exe`: Applies the keyless `"ANZMENU1"` v1 XOR-delta container (`menu_patch.bin`) to `dwq\sys0.gpk` + `dwq\sys0.gtb` (`64` patched sprites out of `184` total entries across `PACKTYPE=0`, `PACKTYPE=2A`, and `PACKTYPE=3A`), `init2\mainControl.fxf`, `init2\backlog.fxf`, and `spt\charaname.xtx` in `--game-dir` while preserving `.bak` backups.
  - `cp932_probe.exe`: Verifies CP932 round-trip fidelity across the game's `.spt` corpus.
- **`proxy_dll/` (Win32, `conan-windows-x86-release`)**:
  - `winmm.dll`: Forwards all 181 `winmm.dll` exports to `<System32>\winmm.dll`, runs first-launch `"ANZMENU1"` (`menu_patch.bin`) auto-patching synchronized by `Local\AnzUCeMenuPatchMutex` (backing up untouched originals to `.bak` before writing patched `spt\charaname.xtx`, `init2\mainControl.fxf`, `init2\backlog.fxf`, `dwq\sys0.gtb`, and `dwq\sys0.gpk`), patches `PrintMessage` layout parameters (`RVA 0x0F89B1`, `10` sites: `fontSize=14`, `nextY=18`, `space=-4`, `windowMessageNumber=7`), and hooks `TextSplitter` (`RVA 0x0F7650`) to substitute CP932 strings from `translation_table.tsv` / `translations.tsv`.

## Character Roster (`spt\charaname.xtx`)

| Japanese Name | Romanized Name | Fullwidth XTX Plate |
| :--- | :--- | :--- |
| `櫻井 夢美` (`夢美`) | Yumemi Sakurai (`Yumemi`) | `Ｙｕｍｅｍｉ` |
| `九条 アゲハ` (`アゲハ`) | Ageha Kujo (`Ageha`) | `Ａｇｅｈａ` |
| `伏島 遥` (`遥`) | Haruka Fushijima (`Haruka`) | `Ｈａｒｕｋａ` |
| `神楽坂 杏子` (`杏子`) | Anzu Kagurazaka (`Anzu`) | `Ａｎｚｕ` |
| `神武 美弥香` (`美弥香`) | Miyaka Kotake (`Miyaka`) | `Ｍｉｙａｋａ` |

## Build & Deploy

```powershell
python build.py --test --install --deploy --game-dir "C:\path\to\WelcomeToAnzU!_CE"
```
