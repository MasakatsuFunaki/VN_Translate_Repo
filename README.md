# Visual Novel Translation Patches

English translation patches for Japanese visual novels. Each patch translates the game text at
runtime — the original game files stay untouched.

## Games

| Game | Status |
|------|--------|
| EXTRAVAGANZA ~Mushi Mederu Shoujo~ CE | Shipped |
| EXTRAVAGANZA ~Matsuro~ CE | Shipped |
| Mushigurui HD10 | Shipped |
| Amakano 2 PE | Shipped |
| CROSS†CHANNEL FC | Shipped |
| Shingakkou ~Noli me tangere~ DL | Shipped |
| Fraternite HD | Shipped |
| Sakura no Uta | Shipped |
| Sakura, Moyu. -as the Night's, Reincarnation- | Shipped |
| Sakura no Toki | In progress |
| Adieu | In progress |
| 夜、灯す。 | In progress |
| Kishin Hishou Demonbane | Shipped |

## How to use (Release Zip)

1. Download and extract the latest release `.zip` from the
   [Releases](https://github.com/MasakatsuFunaki/VN_Translate_Repo/releases) page.
2. Open a Command Prompt or PowerShell inside the extracted release folder and follow the instruction for your game (all required translation caches are pre-bundled offline — **no API key is required**):

| Game Folder in Release Zip | How to Apply the Patch |
|---|---|
| `CROSS_CHANNEL` | Copy everything inside `CROSS_CHANNEL\game\` (`xinput1_3.dll` + `translations.tsv`) into your game's install directory (or run `CROSS_CHANNEL\bin\00_run_all.exe --game-dir "C:\path\to\game"`). |
| `SakuraNoUta` | Copy everything inside `SakuraNoUta\game\` (`winmm.dll` + `translations.tsv`) into your game's install directory (or run `SakuraNoUta\bin\00_run_all.exe --game-dir "C:\path\to\game"`). |
| `EXTRAVAGANZA_CE` | Run `EXTRAVAGANZA_CE\bin\00_run_all.exe --game-dir "C:\path\to\game"` (repacks `.spt`, `.xtx`, and `chart\*.fxf` from bundled caches with automatic backups in `spt_backup\` and `chart_backup\`, and copies `winmm.dll`). |
| `EXTRAVAGANZA_MATSURO_CE` | Copy everything inside `EXTRAVAGANZA_MATSURO_CE\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory and run `EXTRAVAGANZA_MATSURO_CE\bin\03_build_xtx.exe --game-dir "C:\path\to\game"` (or run `00_run_all.exe` + `03_build_xtx.exe`). |
| `mushigurui_HD10` | Copy everything inside `mushigurui_HD10\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory and run `mushigurui_HD10\bin\03_build_xtx.exe --game-dir "C:\path\to\game"` (or run `00_run_all.exe` + `03_build_xtx.exe`). |
| `amakano2pe` | Copy everything inside `amakano2pe\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory (or run `amakano2pe\bin\00_run_all.exe --game-dir "C:\path\to\game"`). |
| `shingakkou` | Copy everything inside `shingakkou\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory (or run `shingakkou\bin\00_run_all.exe --game-dir "C:\path\to\game"`); in-game font must be set to **UD デジタル 教科書体 N**. |
| `FRATERNITE_HD` | Copy everything inside `FRATERNITE_HD\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory (press `F1` in-game to toggle EN/JP), or run `FRATERNITE_HD\decrypt_translations.bat` then `FRATERNITE_HD\bin\00_run_all.exe --game-dir "C:\path\to\game"`. |
| `FAVORITE` (*Sakura, Moyu.*) | Copy everything inside `FAVORITE\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory and run `FAVORITE\bin\03_patch_ui.exe --game-dir "C:\path\to\game"` (or run `FAVORITE\bin\00_run_all.exe --game-dir "C:\path\to\game"`). |
| `kishin_hisho` | Copy everything inside `kishin_hisho\game\` (`launch.exe`, `alpharom_helper.dll`, `0script.pak`, `script\*.nps`) into your game's install directory, then start the game via `launch.exe`. |

3. Start the game as usual.

## License

GPL-3.0. See [LICENSE](LICENSE).
