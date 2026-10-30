# Sakura no Uta — English Translation

An English patch for **桜の詩** (Sakura no Uta) by SCA-DI / Makura. A proxy
`winmm.dll` replaces each Japanese line with its English translation while the
game draws it. The game's own files stay unchanged.

The finished translation, `translations.tsv`, and the translation cache are
already in this folder.

## What you need

- The game, installed.
- Python 3, CMake 3.23 or later, Conan 2, Ninja, and the MSVC C++ toolchain
  (Visual Studio 2022 or its Build Tools).

## Patching the game

Close the game. Then build the patch and copy it into the game folder:

```bat
cd SakuraNoUta
python build.py --deploy --game-dir C:\SakuraNoUta
```

Replace `C:\SakuraNoUta` with your game folder. The command copies
`winmm.dll` and `translations.tsv` into it. Start the game as usual.

### Choice menus

The game draws each choice option as a picture, so the DLL cannot translate
it. This command writes 34 English pictures into the game folder:

```bat
python analysys\make_option_image.py --all --format bmp32 --game-dir C:\SakuraNoUta --deploy
```

## Removing the patch

Delete these files from the game folder:

```bat
del C:\SakuraNoUta\winmm.dll C:\SakuraNoUta\translations.tsv
del C:\SakuraNoUta\select??-?
```

The second line removes the 34 choice pictures.

## Rebuilding the translation table

```bat
cd SakuraNoUta
python build.py --install
build\install\bin\00_run_all.exe --game-dir C:\SakuraNoUta
```

`00_run_all.exe` extracts the Japanese text, applies the cached translations,
writes `translations.tsv` and copies the patch into the game folder.

To run the steps one at a time:

```bat
cd build\install\bin
01_extract.exe   --game-dir C:\SakuraNoUta
02_build_tsv.exe --game-dir C:\SakuraNoUta
cd ..\..\..
python build.py --deploy --game-dir C:\SakuraNoUta
```

### Options

```bat
00_run_all.exe --batch N             :: lines per batch (default 150)
00_run_all.exe --max-batches N       :: stop after N batches, keep the cache
00_run_all.exe --test N              :: N batches, then stop -- DELETES THE CACHE
00_run_all.exe --clean               :: delete all output, then a full run

02_build_tsv.exe --script NAME       :: process one script only
02_build_tsv.exe --rebuild-table     :: rebuild translations.tsv from translated_text.json
02_build_tsv.exe --retranslate       :: ignore the cache, redo all -- DELETES THE CACHE
```

> **WARNING:** Back up `script_output/translation_cache_anthropic.json` and
> `script_output/translated_text.json` before you use `--test`, `--clean` or
> `--retranslate`. These flags delete the paid translations, and nothing can
> rebuild them. To stop a run early and keep them, use `--max-batches N`.

When the cache holds finished work, these three flags refuse to run unless you also give
`--discard-cache`.
