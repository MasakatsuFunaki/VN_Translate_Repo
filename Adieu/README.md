# Adieu (*Sayonara o Oshiete ~comment te dire adieu~*) — English Translation

English localization pipeline and runtime hook for **Sayonara o Oshiete ~comment te dire adieu~** (`さよならを教えて ～comment te dire adieu～`, CRAFTWORK / VisualArt's SiglusEngine DL Edition).

Dialogue, narration, choices, save-slot scene titles, backlog, and Win32 system chrome are translated **live** by a 32-bit `winmm.dll` proxy (`Scene.pck` is never modified on disk). `Gameexe.dat` and all `111` Japanese UI `.g00` sprites are patched atomically on first launch from a keyless XOR-delta container (`menu_patch.bin`) with automatic `.bak` backups.

## Game & Archive Overview

| Property | Details |
|---|---|
| **Engine** | VisualArt's `SiglusEngine` (`SiglusEngine.exe`, 32-bit x86 PE32, ASLR enabled, native UTF-16LE `wchar_t`, `640x480` full-screen NVL) |
| **Game Data Directory** | `<Adieu>\StartData\GameData\` (`SiglusEngine.exe`, `Scene.pck`, `Gameexe.dat`, `g00\*.g00`, `savedata\`). Note: the outer `<Adieu>\Start.exe` launcher and `<Adieu>\StartData\StartMenu.exe` spawn `StartData\GameData\SiglusEngine.exe` (often with `CWD = StartData\`); an unused duplicate `StartData\SiglusEngine.exe` sits one level up without `Scene.pck` or `Gameexe.dat`. Always target **`StartData\GameData\`** for `--game-dir` and manual deployment. |
| **Script Archive (`Scene.pck`)** | `194` scenes, `24` macros, `23,400` total string entries (`15,656` Japanese strings). Scene chunks decrypt via 16-byte `ADIEU_PRIVATE_KEY` (`A2 7E C9 DA 2E 2D 09 08 E5 EA FA 96 A3 1B ED E0`) + 256-byte `SIGLUS_KEY_256` + Siglus byte-LZSS, and every `SCENEHEADERV2` UTF-16LE string entry `j` is decoded **unconditionally** with `SS_V2` (`(j * 0x7087) & 0xFFFF`). |
| **Engine Config (`Gameexe.dat`)** | Encrypted with `ADIEU_PRIVATE_KEY` (`use_private_key = 1`) + a distinct 256-byte `GAMEEXE_KEY_256` table at `SiglusEngine.exe` file offset **`0x5F7D90`** (`VA 0x009F9590`), followed by Siglus byte-LZSS UTF-16LE (`499` directives). Uses `#DISCMARK = "sayonara.env"` (no `#REG_NAME`), keeping saves local under `StartData\GameData\savedata\`. |
| **Graphics (`g00\*.g00`)** | `617` loose sprites: `468` **Type 0** (24-bpp BGR pixel-LZSS, `width * height * 4` unpacked BGRA stride), `1` **Type 1** (`BLACK.g00`, 8-bpp indexed byte-LZSS), and `148` **Type 2** (32-bpp BGRA multi-cut/multi-part byte-LZSS sprite sheets with 24-byte `CutDescriptor`, 116-byte `CutHeader`, and 92-byte `PartHeader` blocks). |

## Pipeline & Executables

Two architectures live under `Adieu/` and build into `Adieu/build/`:
- `pipeline_cpp/` (**x64**, preset `conan-windows-x64-release`, Conan 2 + Boost + OpenSSL + GTest + Win32 GDI/WIC)
- `proxy_dll/` (**Win32**, preset `conan-windows-x86-release`, Conan 2 + GTest)

| Executable (`build\install\bin\`) | Role |
|---|---|
| `01_extract.exe` | Decrypts all `194` scenes in `Scene.pck` (unconditional `SS_V2`), attributes contextual `idx - 1` speaker plates (`"＊Ａ" -> "人見"`, `"＊Ｂ" -> "広介"`), and writes `script_output/extracted_text.json`. |
| `03_speaker_gate.exe` | Offline zero-token verification gate (`[1/5]`–`[5/5]`) checking narration leak rate (`< 5%`), prompt formatting, and 100% bidirectional speaker coverage against `glossary.cpp` and `TONE_OF_NOVEL.md`. |
| `02_translate.exe` | Translates uncached unique lines chronologically (`_start`, `_menu`, `_manual`, `seen900`, `seen020`, ...) via Google Gemini (`gemini-4-argon`), updates `script_output/translated_text.json` + `script_output/translation_cache_gemini.json`, and builds `translation_table.tsv`. |
| `02_build_tsv.exe` | Standalone runtime TSV builder (`--rebuild-table --dir Adieu [--game-dir <GameDataDir>] [--out <path>] [--sync-install]`) regenerating `translation_table.tsv` from `script_output/translated_text.json` without API calls. |
| `04_patch_menus.exe` | Decodes `Gameexe.dat` and UI `.g00` sprites, applies deterministic 3x supersampled GDI typography and row-median / SOR Laplace background restoration, optionally emits `Original \| Patched \| Diff` PNG contact sheets (`--contact-sheets`), and packs `Adieu/menu_patch.bin` (`"ADIEUMEN"` v1). Also supports `--dump-ui --dump-dir <dir> [--only <File.g00>]`. |
| `00_run_all.exe` | End-to-end orchestrator running `01_extract` → `03_speaker_gate` → `02_translate` → `04_patch_menus` (unless `--skip-menus`) → deployment of `winmm.dll`, `translation_table.tsv`, and `menu_patch.bin` into `StartData\GameData\`. |

## Build, Full Translation, Spot-Check & Release Workflow

### 1. Build, Test, and Stage

```powershell
python Adieu/build.py --test --install
```

`Adieu/build.py` supports `--test`, `--install`, `--deploy --game-dir <GameDataDir>`, and `--clean`. It idempotently syncs `.json <-> .json.enc` when encrypted payloads are present, builds both x64 and Win32 trees, refreshes `translation_table.tsv` via `02_build_tsv.exe`, runs CTest across both architectures, and stages `build/install/bin/` (6 executables) and `build/install/game/` (`winmm.dll`, `translation_table.tsv`, `menu_patch.bin`).

### 2. Full Script Translation (Launch in a Visible Console Window)

Set `GEMINI_API_KEY` (in the environment or `~/.gemini/.env`) and launch `00_run_all.exe` (or `02_translate.exe`) in a visible console window so live batch progress is visible:

```powershell
Start-Process -FilePath "D:\Projects\VN_TRANSLATE\Adieu\build\install\bin\00_run_all.exe" `
  -ArgumentList '--dir Adieu --game-dir "D:\Kits\my_downloadedGames\+new\Adieu\Adieu\StartData\GameData"' `
  -WorkingDirectory "D:\Projects\VN_TRANSLATE" -Wait
```

Direct command equivalents:

```batch
Adieu\build\install\bin\00_run_all.exe --dir Adieu --game-dir "D:\Kits\my_downloadedGames\+new\Adieu\Adieu\StartData\GameData"
:: Or translate only (after 01_extract.exe and 03_speaker_gate.exe):
Adieu\build\install\bin\02_translate.exe --dir Adieu
```

Useful `02_translate.exe` / `00_run_all.exe` flags:
- `--batch <N>`: Uncached unique lines per API request (default: `150`).
- `--max-batches <N>`: Global cap across all scenes (`0` = unlimited), preserving cached progress.
- `--skip-menus` (`00_run_all.exe`): Reuse the committed `Adieu/menu_patch.bin` without re-running `04_patch_menus.exe`.

### 3. Optional Spot-Check & Downstream Rebuild / Deploy Chain

Because `01_spot_check.exe` updates only `Adieu/script_output/*.json`, always rebuild `translation_table.tsv` and redeploy afterward so staged and installed game files never stay stale:

```powershell
Start-Process -FilePath "D:\Projects\VN_TRANSLATE\spot_checker\build\install\bin\01_spot_check.exe" `
  -ArgumentList "--game Adieu" `
  -WorkingDirectory "D:\Projects\VN_TRANSLATE" -Wait

Adieu\build\install\bin\02_build_tsv.exe --rebuild-table --dir Adieu
python Adieu/build.py --deploy --game-dir "D:\Kits\my_downloadedGames\+new\Adieu\Adieu\StartData\GameData"
```

### 4. VNE2 Encryption for Git & CI Release

Plain `script_output/*.json` and `*.tsv` files are git-ignored. Once full translation (and any spot-check pass) is complete, encrypt `translated_text.json` and `translation_cache_gemini.json` into tracked `VNE2` (`DeflateStream` + `AES-256-CBC`) `.json.enc` archives:

```batch
encrypt_translations.bat --encrypt --dir Adieu
```

Creating `Adieu/script_output/translated_text.json.enc` automatically promotes `Adieu` into root `build.py`'s shipped-game discovery for multi-game release packaging.

## Runtime Architecture & Engine Notes (`proxy_dll` / `winmm.dll`)

- **Executable-Relative Path Resolution & Shutdown Safety (`dllmain.cpp`, `translator.cpp`):**
  `winmm.dll` forwards all 181 system `winmm.dll` exports (`winmm.def`), resolves `translation_table.tsv`, `menu_patch.bin`, and optional diagnostic logs relative to `GetModuleFileNameW(nullptr)` (`StartData\GameData\`) rather than `CWD`, and exits early on `DllMain(DLL_PROCESS_DETACH)` when `lpReserved != nullptr` (process termination).
- **UTF-16 Scene String Post-Hooks & Cross-CRT-Safe `std::wstring` Mutation (`translator.cpp`):**
  - All addresses are computed from `GetModuleHandleW(nullptr)` (ASLR-safe) after verifying PE `.text` execute bounds, 5-byte SEH prologues (`55 8B EC 6A FF`), and the `ret 0x0C` (`C2 0C 00` at `+0xCC`) epilogue of `0x10C3A0` (cleanly skipping inline hooks inside non-Siglus test hosts).
  - Post-hooks `Scene::get_str` (**`RVA 0x14C8E0`**, `std::wstring* __thiscall(void* self, std::wstring* out, int scn_no, int str_no)`, `ret 0x0C`) and `Scene::get_cur_str` (**`RVA 0x14C9C0`**, `std::wstring* __thiscall(void* self, std::wstring* out, int str_no)`, `ret 0x08`).
  - Reads `out` via `SiglusEngine.exe`'s verified **24-byte `SiglusWstring`** layout (`+0x00` `union { wchar_t buf[8]; wchar_t* ptr; }`, `+0x10` `uint32_t size`, `+0x14` `uint32_t res`, heap `ptr` active iff `res >= 8`).
  - Writes English replacements by resetting `out->size = 0` (`cur_buf[0] = L'\0'`) and invoking `SiglusEngine.exe`'s own `basic_string<wchar_t>` range helper at **`RVA 0x10C3A0`** (`ret 0x0C` — 3 stack `DWORD`s including the trailing iterator-category tag `(wstr_self, edx_unused, first, last, 0)`), keeping every allocation on the game executable's CRT heap. This single hook pair translates NVL prose, speaker plates, `#SELBTN` choices, save-slot scene titles, and `#MSGBK` backlog entries.
- **`#NAMAE` Protagonist Macro Preservation (`"＊Ａ"` / `"＊Ｂ"`):**
  `_start` and `_menu` bind `#NAMAE` macro keys `"＊Ａ"` (surname default `"人見"`) and `"＊Ｂ"` (given-name default `"広介"`). Both `02_build_tsv` and runtime `ShouldBypassTranslation` never translate empty strings, ASCII-only engine/asset tokens (`ch < 0x0080`), or `"＊Ａ"` / `"＊Ｂ"` themselves, while translating default values `"人見" -> "Hitomi"` and `"広介" -> "Kousuke"`. SiglusEngine therefore expands `"＊Ａ"` to `"Hitomi"` at runtime while preserving custom player name entry in `_menu`.
- **`640x480` Full-Screen NVL (`#MWND.002`) Layout & `WrapForSiglusNvl`:**
  `Gameexe.dat` `#MWND.002` (`NOVEL_MODE = 1`) is tuned from `26x16` @ `24px` to `MOJI_SIZE = 20`, `MOJI_SPACE = 0, 3`, `MOJI_CNT = 30, 19`, `NAME_MOJI_SIZE = 16`, and `TALK_MARGIN = 20, 24, 0, 20`, with choice buttons `#SELBTN.000` / `#SELBTN.001` set to `MOJI_SIZE = 22, 0, 0, 0`. At runtime, `WrapForSiglusNvl` inserts real `L'\n'` (`U+000A`) breaks at ASCII word boundaries (`56` columns for narration, `50` columns for quoted dialogue) while leaving single-line choices unwrapped.
- **Win32 Chrome & Dialog Localization (`win32_chrome.cpp`):**
  Loader-lock-safely patches `USER32.dll` IAT slots (`SetWindowTextW/A`, `CreateWindowExW/A`, `FindWindowW/A`, `MessageBoxW/A`, `InsertMenuItemW`, `AppendMenuW`, `ModifyMenuW`, `SetDlgItemTextW`, `DialogBoxParamW`, `CreateDialogParamW`, `CreateDialogIndirectParamW`). Translates the window caption, confirmation `MessageBox` dialogs, right-click popup menus, and Win32 Config / Save-Load (`#146`) dialog captions, child controls, tab items, and listview column headers on `WM_INITDIALOG` with a lazily created 9pt `Segoe UI` `HFONT`, keeping all dialog template IDs, control IDs, and save paths untouched.
- **Opt-In Developer Diagnostics:**
  - `ADIEU_CAPTURE_JP=1`: Logs unique runtime Japanese strings seen by `Scene::get_str` / `Scene::get_cur_str` to `StartData\GameData\adieu_jp_strings.txt` (off by default).
  - `ADIEU_DUMP_KEY=1`: Arms the `KeyDumper` VEH `INT3` probe at `RVA 0x2D1A80`, writing `StartData\GameData\siglus_key_dump.txt` (off by default).
- **`"ADIEUMEN"` v1 Two-Phase First-Launch Menu Auto-Patcher (`menu_autopatch.h`, `Adieu/menu_patch.bin`):**
  - Committed `Adieu/menu_patch.bin` stores **`112` keyless XOR-delta records** (`MurmurHash2` `m = 0x5BD1E995, r = 24, seed = 0`, verified free of raw game signatures by `VerifyNoRawAssetSignatures()`):
    - **`Gameexe.dat` (`1` record):** Translates all `45` active + `19` template Japanese UI strings across `64` directives (`#GAMENAME`, `#WARNINGINFO.*`, `#SYSCOMMENU.*`, `#DIALOG.*`, `#SAVE*`, `#LOAD*`, `#CHRKOE.NOT_LOOK_NAME_STR`) and applies the `7` `#MWND.002` / `#SELBTN` metric rules while keeping `#DISCMARK = "sayonara.env"` untouched.
    - **Localized `.g00` UI Sprites (`111` records):**
      - `ME_02.g00`–`ME_04.g00` (`3` in-game / recollection menu multi-cut strips, using `ME_01.g00` as donor background)
      - `ATTEN.g00` (`1` boot content-disclaimer card)
      - `18` `MESS*` monologue, flashback, and school-map overlays/strips (`MESS01`–`05`, `MESS11`–`12`, `MESS20`–`22`, `MESS25`, `MESS31`, `MESS41`, `MESS51`–`52`, `MESS81`, `MESS98`, `MESS99`)
      - `CM_B02.g00`–`CM_B03.g00` (`2` supplementary hover/push banners)
      - `MUSIC1.g00`–`MUSIC3.g00` (`3` music-room tracklist screens)
      - `CM101.g00`–`CM806.g00` (`84` creator-commentary / CG ticker multi-cut strips)
    - Retail sprites that are already English, pure icon/frame artwork, bitmap glyph sheets, or authentic Japanese end-credits rolls — including `_name_input_init.g00` / `_name_input_ok.g00`, `MENU01`–`MENU09`, `ME_01`, `LOGO`, `GRDAT`, `BGJ_1`–`BGJ_B`, `END01`–`END03`, `STAFF01`–`STAFF08`, and `EXFONT_L` / `EXFONT_N` — are preserved untouched.
  - Inside `TranslatorInit()`, `EnsureMenuAssetsPatched()` (pure `kernel32.dll`, serialized by `Local\AdieuMenuPatchMutex`) validates original slices from immutable `<target>.bak` (or live `<target>` on first run) and writes `<target>.bak.tmp` + `<target>.stage.tmp` with `FlushFileBuffers` in **Phase 1**, then atomically promotes `.bak`, swaps live targets via `MoveFileExW`, and writes `menu_patch.applied` (`"ADIEUAPL"` v1) in **Phase 2**. Subsequent launches validate `menu_patch.applied` in `< 1 ms` without reading asset payloads, and updated patches re-apply idempotently from `.bak`.

## Restore Original

```powershell
$gd = "D:\Kits\my_downloadedGames\+new\Adieu\Adieu\StartData\GameData"
Get-ChildItem -LiteralPath $gd -Recurse -Filter *.bak | ForEach-Object {
    $orig = $_.FullName.Substring(0, $_.FullName.Length - 4)
    Move-Item -LiteralPath $_.FullName -Destination $orig -Force
}
Remove-Item -LiteralPath `
    "$gd\winmm.dll", `
    "$gd\translation_table.tsv", `
    "$gd\menu_patch.bin", `
    "$gd\menu_patch.applied" `
    -Force -ErrorAction SilentlyContinue
# Scene.pck is never modified on disk.
```
