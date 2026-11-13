# Visual Novel Translation Patches

English translation patches for Japanese visual novels. Each patch translates the game text at
runtime — the original game files stay untouched.

## Support the Project

All reverse engineering, runtime hook development, UI/menu translation, and LLM API translation & spot-check costs across these 15+ visual novels are paid for out of pocket. If you enjoy playing these patches and want to help cover API and tooling costs for upcoming translations, please consider supporting via GitHub Sponsors:

- **[Sponsor @MasakatsuFunaki on GitHub Sponsors](https://github.com/sponsors/MasakatsuFunaki)**

## Games

| Game | Folder | Status |
|------|--------|--------|
| EXTRAVAGANZA \~Mushi Mederu Shoujo\~ CE | `EXTRAVAGANZA_01_CE` | Shipped |
| Mushigurui (HD10 & CE) | `EXTRAVAGANZA_02_MUSHIGURUI_HD10` | Shipped |
| EXTRAVAGANZA \~Matsuro\~ CE | `EXTRAVAGANZA_03_MATSURO_CE` | Shipped |
| AGEHA \~EXTRAVAGANZA\~ CE | `EXTRAVAGANZA_04_AGEHA_CE` | Shipped |
| Welcome to AnzU! CE | `EXTRAVAGANZA_05_ANZU_CE` | Shipped |
| Yumemi Ryoujoku CE | `EXTRAVAGANZA_06_YUMEMI_CE` | Shipped |
| Ren to Mushi-kun no Ichinichi CE | `EXTRAVAGANZA_07_REN_MUSHI_CE` | Shipped |
| EXTRA VA MIZUNA CE | `EXTRAVAGANZA_08_MIZUNA_CE` | Shipped |
| Amakano 2 PE | `amakano2pe` | Shipped |
| CROSS†CHANNEL FC | `CROSS_CHANNEL` | Shipped |
| Shingakkou \~Noli me tangere\~ DL | `shingakkou` | Shipped |
| Fraternite HD | `FRATERNITE_HD` | Shipped |
| Sakura no Uta | `SakuraNoUta` | Shipped |
| Sakura, Moyu. -as the Night's, Reincarnation- | `SakuraMoyu` | Shipped |
| Kishin Hishou Demonbane | `kishin_hisho` | Shipped |
| Oretachi ni Tsubasa wa Nai Anniversary Edition (*We Without Wings*) | `Oretachi_ni_Tsubasa_wa_Nai` | In progress |
| Sakura no Toki | `sakuranotoki` | In progress |
| Adieu | `Adieu` | In progress |

## How to use (Release Zip)

1. Download and extract the latest release `.zip` from the
   [Releases](https://github.com/MasakatsuFunaki/VN_Translate_Repo/releases) page.
2. Follow the instruction for your game below (all runtime translation tables and UI patches are pre-built offline — **no API key or setup is required**):

| Game Folder in Release Zip | How to Apply the Patch |
|---|---|
| `CROSS_CHANNEL` | Copy everything inside `CROSS_CHANNEL\game\` (`xinput1_3.dll` + `translations.tsv`) into your game's install directory. |
| `SakuraNoUta` | Copy everything inside `SakuraNoUta\game\` (`winmm.dll` + `translations.tsv`) into your game's install directory. |
| `EXTRAVAGANZA_01_CE` | Copy everything inside `EXTRAVAGANZA_01_CE\game\` (`winmm.dll` + `extravaganza_patch.bin`) into your game's install directory. |
| `EXTRAVAGANZA_02_MUSHIGURUI_HD10` | Copy everything inside `EXTRAVAGANZA_02_MUSHIGURUI_HD10\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory — works with both Mushigurui HD10 and EXTRAVAGANZA Complete Edition (`winmm.dll` automatically patches `dwq\sys0`, `dwq\ta0`, `dwq\ev0`, `init2\*.fxf`, and `spt\charaname.xtx` on first launch with `.bak` backups). |
| `EXTRAVAGANZA_03_MATSURO_CE` | Copy everything inside `EXTRAVAGANZA_03_MATSURO_CE\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `dwq\sys0.gpk`, `dwq\sys0.gtb`, `init2\*.fxf`, and `spt\charaname.xtx` on first launch with `.bak` backups). |
| `EXTRAVAGANZA_04_AGEHA_CE` | Copy everything inside `EXTRAVAGANZA_04_AGEHA_CE\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `dwq\sys0.gpk`, `dwq\sys0.gtb`, `init2\*.fxf`, and `spt\charaname.xtx` on first launch with `.bak` backups). |
| `EXTRAVAGANZA_05_ANZU_CE` | Copy everything inside `EXTRAVAGANZA_05_ANZU_CE\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `dwq\sys0.gpk`, `dwq\sys0.gtb`, `init2\*.fxf`, and `spt\charaname.xtx` on first launch with `.bak` backups). |
| `EXTRAVAGANZA_06_YUMEMI_CE` | Copy everything inside `EXTRAVAGANZA_06_YUMEMI_CE\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `dwq\sys0.gpk`, `dwq\sys0.gtb`, `init2\*.fxf`, and `spt\charaname.xtx` on first launch with `.bak` backups). |
| `EXTRAVAGANZA_07_REN_MUSHI_CE` | Copy everything inside `EXTRAVAGANZA_07_REN_MUSHI_CE\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `dwq\sys0.gpk`, `dwq\sys0.gtb`, `init2\*.fxf`, and `spt\charaname.xtx` on first launch with `.bak` backups). |
| `EXTRAVAGANZA_08_MIZUNA_CE` | Copy everything inside `EXTRAVAGANZA_08_MIZUNA_CE\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `dwq\sys0.gpk`, `dwq\sys0.gtb`, `init2\*.fxf`, and `spt\charaname.xtx` on first launch with `.bak` backups). |
| `amakano2pe` | Copy everything inside `amakano2pe\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory. |
| `shingakkou` | Copy everything inside `shingakkou\game\` (`winmm.dll` + `translation_table.tsv` + `menu_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `Data\sin_sysd.dat` and `Bootup.dat` on first launch with `.bak` backups). In-game font is automatically defaulted to **UD デジタル 教科書体 N**. |
| `FRATERNITE_HD` | Copy everything inside `FRATERNITE_HD\game\` (`winmm.dll` + `translation_table.tsv` + `cgsys_patch.bin`) into your game's install directory (`winmm.dll` automatically patches `pac\cgsys.ypf` on first launch with a `.bak` backup; press `F1` in-game to toggle EN/JP). |
| `SakuraMoyu` (*Sakura, Moyu.*) | Copy everything inside `SakuraMoyu\game\` (`winmm.dll` + `translation_table.tsv`) into your game's install directory and run `SakuraMoyu\bin\03_patch_ui.exe --game-dir "C:\path\to\game"`. |
| `kishin_hisho` | Copy everything inside `kishin_hisho\game\` (`launch.exe`, `alpharom_helper.dll`, `0script.pak`, `script\*.nps`) into your game's install directory, then start the game via `launch.exe`. |

3. Start the game as usual.

## License

GPL-3.0. See [LICENSE](LICENSE).
