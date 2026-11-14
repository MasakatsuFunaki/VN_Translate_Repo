# Amakano 2 ~Perfect Edition~ English Translation

Translates the visual novel from Japanese to English using:
- **Offline translation pipeline** (builds `translation_table.tsv` from the cached translation corpus)
- **Runtime DLL injection** (no archive repacking — translation is applied live)

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake). A full run
covers all 56,779 MESSAGE lines in `scene.int`.

## Requirements

- cmake (>=3.23), conan (>=2), ninja, and the MSVC C++ toolchain
  (VS 2022 with the C++ workload, or the Build Tools) -- the build uses the
  Ninja Multi-Config generator and never MSBuild

Pass `--game-dir <install>` to every app.  It is required: the game install
path differs per machine.

## Quick Start

```batch
cd amakano2pe
python build.py --test --install

build\install\bin\00_run_all.exe --game-dir <install>
```

`build.py` builds both halves into one tree (`build/pipeline` x64,
`build/proxy` Win32) and takes four flags: `--test`, `--install`,
`--deploy --game-dir <install>`, `--clean`.

`00_run_all.exe` runs the whole pipeline: extract → build TSV → deploy.

## Pipeline

| Step | Executable | Description |
|------|-----------|-------------|
| 1 | `01_extract.exe` | Walk every CatScene script in `scene.int` → `script_output/extracted_text.json` |
| 2 | `02_build_tsv.exe` | Speaker gate (5 checks, zero tokens), apply cached JP → EN translations, write `translation_table.tsv` |
| 3 | `03_patch_menus.exe` | Render every translated menu `.hg3` sprite and rebuild the `.fes` / `.xml` / `.kcs` / `export\*.zt` UI text, then pack them as keyless XOR-deltas into `menu_patch.bin` |
| 4 | `python build.py --deploy --game-dir <install>` | Copy `winmm.dll`, `translation_table.tsv` and `menu_patch.bin` next to `cs2.exe` |

`menu_patch.bin` is committed, so steps 1–3 are only needed after a
translation or renderer change.  Regenerating it needs the retail archives:

```batch
build\install\bin\03_patch_menus.exe --game-dir <install>                       :: writes amakano2pe\menu_patch.bin
build\install\bin\03_patch_menus.exe --game-dir <install> --contact-sheets <dir> :: also dump before/after PNG contact sheets
```

### Menu patch (`menu_patch.bin`)

`config\startup.xml` ships with `<direct>1</direct>`, so `cs2.exe` reads loose
files from `image\`, `fes\`, `config\`, `kcs\` and `export\` before it looks in
the `.int` archives.  `menu_patch.bin` (`"AMK2MENU"` v1) stores each translated
asset as a keyless XOR-delta against its original encrypted slice in the
user's own archive, so the repository carries no game assets.

On first launch `winmm.dll` takes `Local\Amakano2PeMenuPatchMutex`, checks each
retail slice's MurmurHash2, rebuilds the translated files and writes them into
the override folders through `.tmp` + `MoveFileExW`.  Standalone root files
such as `boot.hg3` get a `.bak` backup first.  A `menu_patch.applied` sentinel
makes later launches skip the work.  `image00.int`, `image01.int` and every
other `.int` stay byte-for-byte untouched, and the window title is translated
live, so the `%APPDATA%\あざらしそふと\アマカノ2～Perfect Edition～` save folder
does not change.

### Step-by-step (manual)

```batch
cd build\install\bin
01_extract.exe --game-dir <install>       :: scene.int -> extracted_text.json
02_build_tsv.exe --game-dir <install>     :: gate, apply cached translations, build the TSV
cd ..\..\.. && python build.py --deploy --game-dir <install>   :: deploy winmm.dll + table
```

## Options

```batch
00_run_all.exe --dir <p> --game-dir <install>  :: override project folder / set game folder

02_build_tsv.exe --game-dir <install> --out <path>       :: table path (default <game-dir>\translation_table.tsv)
02_build_tsv.exe --game-dir <install> --rebuild-table    :: rebuild translation_table.tsv from translated_text.json on disk
```

## Engine & Runtime Hook Notes

- **Engine & Archives:** CatSystem2 (`cs2.exe`, 32-bit x86). Dialogue scripts live in `scene.int` (`CatScene` zlib-compressed bytecode), and UI layout scripts live in `fes.int` (`meswnd.fes` for the main dialogue window, `log.fes` for the backlog window, and `config_basic.fes` for user-configurable font face and normal/bold `fontweight`).
- **Hook Sites (`proxy_dll/src/translator.cpp`):**
  - `FUN_0063a9b0` (`RVA 0x23A9B0`, `void __cdecl(const char* src, wchar_t* dst, std::size_t max_wchars)`): CatSystem2's universal UTF-8 (`CP_UTF8`) to UTF-16 converter. Lookup keys in `translation_table.tsv` remain UTF-8 (no CP932 re-encoding). Calls with `max_wchars <= 16` come from `STRING8::ParseTokens` single-glyph decoding and pass through untouched.
  - `FUN_0063d000` (`RVA 0x23D000`, `Cs2FontContext::SetHeight`): `ScaleHeight` scales `LOGFONTW.lfHeight` by `9/10` (`23 -> 20` at normal size) for all calls, and synchronizes `GdiTextWidthMeasurer` exclusively when `active_slot == 0`, the caller is one of `STRING8`'s 4 initial base-font setup sites (`RVA 0x22596B` fullwidth and `RVA 0x225C03` halfwidth `[ebp - 0x24]` in `STRING8::MeasureGlyphs` reading active weight `[STRING8 + 0xCC]`, or `RVA 0x22BA7B` fullwidth and `RVA 0x22BD09` halfwidth in `STRING8::Render` reading active weight `[STRING8 + 0x10C]` — never an inline `\f` per-glyph override loop), `[STRING8 + 0x24..0x38)` matches `meswnd.fes`'s `str layout size` `{\fss, \fs, \fn, \fl, \fll} = {20, 22, 23, 27, 30}`, and `std::abs(raw_height) == 23` (`\fn`). This syncs any font face and normal/bold `fontweight` (`400`/`700`) selected in `config_basic.fes` while isolating `meswnd.fes` wrapping from `log.fes` (`log layout size 18 20 22 25 28`), inline `\f` tags, ruby (`rsize 9`), and speaker nameplates (`size 26`).
- **Proportional GDI Pixel Wrapping & Vertical Box Budget:** `meswnd.fes` configures `str` width to `690 px` (`str layout size` in `メイリオ`, `SHIFTJIS_CHARSET`, `ANTIALIASED_QUALITY`) and `3` visible rows at `33 px` pitch (`~99 px` vertical clip height). `GdiTextWidthMeasurer` measures spans with Win32 `GetTextExtentPoint32W` and `DialogueTranslationSession` pre-wraps English words at `680 px` (`10 px` safety margin inside `690 px`) using 2-byte `\n` control breaks so `STRING8` never splits English words mid-glyph or overflows into a clipped 4th line.
- **`\@` Inline Keywait & Cross-`MESSAGE` Width Carry:** CatScene splits multi-line and mid-line keywait (`\@`) sentences across consecutive `MESSAGE` opcodes that append into the same `STRING8` buffer (`\n`-prefixed continuations and `\@`-terminated segments). `DialogueTranslationSession` strips any dangling unescaped trailing `\`, carries the running pixel width and punctuation state across continuations (including control-only `"\\n\\@"` / `"\\@"` segments), treats CatSystem2 inline tags (`\@`, `\p`, `\f`, `\w`, `\c`, `\x`, `\y`, `\i`, `\r`, `\t`, `\z`, `\$` case-insensitively, with decimal and `0x` hex parameters) as zero-width and `\s`/`\S` as fullwidth space `U+3000`, and deduplicates consecutive layout/draw passes on the same `MESSAGE`.
- **Speaker-Name Isolation:** In `cs2.exe`, `NAME` strings pass through `FUN_0063a9b0` via the script VM (`max_wchars = 4096`) and `STRING8::apend` (`max_wchars = 2048`), including up to 4 historical speaker names (`name[0..3] apend`) when `log.fes` opens mid-`\@` pause. `DialogueTranslationSession` classifies speaker keys via `DefaultSpeakerNameKeys()` (`0` overlap with `MESSAGE` keys), returning unwrapped speaker names without mutating `current_width`, `prev_ended_with_inline_wait`, or `last_dialogue_src_key`, while `TranslationSlotRingBuffer` invalidates any cached dialogue slot if 64+ allocations wrap around the output ring buffer.

## Restore Original

```batch
:: Remove the proxy, the table and the menu patch
del "<install>\winmm.dll"
del "<install>\translation_table.tsv"
del "<install>\menu_patch.bin"
del "<install>\menu_patch.applied"

:: Remove the loose menu overrides the patch wrote
rmdir /s /q "<install>\image"
rmdir /s /q "<install>\fes"
rmdir /s /q "<install>\kcs"
rmdir /s /q "<install>\export"
:: config\ holds retail files too: delete only the overrides the patch wrote there

:: Put back any standalone root file the patch replaced
move /y "<install>\boot.hg3.bak" "<install>\boot.hg3"
```

The `.int` archives are never modified, so there is nothing to restore in
them.  Check that `image\`, `fes\`, `kcs\` and `export\` did not exist in the
retail install before you delete them.
