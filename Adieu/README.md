# Adieu (さよならを教えて ～comment te dire adieu～)

English translation pipeline for **Sayonara o Oshiete ~comment te dire adieu~** by **CRAFTWORK** (VisualArt's SiglusEngine DL Edition).

| | |
|---|---|
| **Engine** | VisualArt's SiglusEngine (`SiglusEngine.exe`, 32-bit x86, UTF-16LE, `640x480` full-screen NVL) |
| **Delivery** | Deployed into `StartData\GameData\` (next to `SiglusEngine.exe`, `Scene.pck`, and `Gameexe.dat` — not the outer `Start.exe` folder). `winmm.dll` translates script strings and Win32 chrome live from `translation_table.tsv` (`Scene.pck` stays untouched), and atomically patches `Gameexe.dat` + `111` UI `.g00` sprites on first launch from `menu_patch.bin` with `.bak` backups. |

## Pipeline

```
01_extract.exe           → script_output/extracted_text.json
03_speaker_gate.exe      → offline speaker attribution & glossary gate
02_translate.exe         → script_output/translated_text.json,
                           script_output/translation_cache_gemini.json, and
                           translation_table.tsv
02_build_tsv.exe         → rebuilds translation_table.tsv without API calls
04_patch_menus.exe       → builds keyless XOR-delta menu_patch.bin
python build.py --deploy → copies winmm.dll, translation_table.tsv, and
                           menu_patch.bin into StartData\GameData\
00_run_all.exe           → extract + gate + translate + menus + deploy
```

## Quick Start

```powershell
cd Adieu
python build.py --test --install

build\install\bin\00_run_all.exe --game-dir "<install>\StartData\GameData"
build\install\bin\02_build_tsv.exe --rebuild-table
python build.py --deploy --game-dir "<install>\StartData\GameData"
```

Plain `script_output/*.json` and `*.tsv` files are git-ignored; `build.py` and `spot_checker` sync `.json <-> .json.enc` (`VNE2`) automatically when `TRANSLATIONS_KEY` or root `translations_key.txt` is present:

```powershell
:: manual VNE2 sync from the repository root
encrypt_translations.bat --encrypt --dir Adieu
decrypt_translations.bat --dir Adieu
```

## Engine Notes

- **`Scene.pck` (`194` scenes):** Chunks decrypt with 16-byte `ADIEU_PRIVATE_KEY` (`A2 7E C9 DA 2E 2D 09 08 E5 EA FA 96 A3 1B ED E0`) + `SIGLUS_KEY_256` + Siglus byte-LZSS; every `SCENEHEADERV2` UTF-16LE string `j` uses `SS_V2` (`(j * 0x7087) & 0xFFFF`) unconditionally.
- **`Gameexe.dat`:** Uses `ADIEU_PRIVATE_KEY` (`use_private_key = 1`) + a distinct 256-byte table at `SiglusEngine.exe` file offset `0x5F7D90` (`VA 0x009F9590`), `#DISCMARK = "sayonara.env"` (local `savedata\`), and `#MWND.002` `640x480` NVL metrics (`MOJI_SIZE = 20`, `MOJI_CNT = 30, 19`).
- **Scene string hooks:** Post-hooks `Scene::get_str` (`RVA 0x14C8E0`, `ret 0x0C`) and `Scene::get_cur_str` (`RVA 0x14C9C0`, `ret 0x08`), mutating 24-byte `SiglusWstring` (`res >= 8` heap) via `SiglusEngine.exe`'s helper at `RVA 0x10C3A0` (`ret 0x0C`) on the game's CRT heap, and bypassing `#NAMAE` macro keys `"＊Ａ"` / `"＊Ｂ"` (`"人見" -> "Hitomi"`, `"広介" -> "Kousuke"`).
- **Win32 dialog localization:** Observes `WM_INITDIALOG` completion via thread-scoped `WH_CALLWNDPROCRET` without replacing `lpDialogFunc` or `DWLP_DLGPROC`, avoiding mutual recursion with `C_window_base::attach_window` (`RVA 0x2B4526`), which subclasses `DWL_DLGPROC` via `CallWindowProcW` whenever `DWL_DLGPROC != &static_wnd_proc`.
- **`.g00` & `menu_patch.bin`:** Decodes `.g00` Types 0 (24-bpp pixel-LZSS), 1 (8-bpp indexed byte-LZSS), and 2 (32-bpp multi-cut/multi-part byte-LZSS), and packs `112` keyless XOR-delta records (`Gameexe.dat` + `111` UI `.g00` sprites) into `"ADIEUMEN"` v1 `menu_patch.bin` (tracked via `menu_patch.applied` + `.bak` backups).
