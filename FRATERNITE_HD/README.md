# Fraternite HD Remaster (フラテルニテ HDリマスター)

English translation pipeline for **Fraternite HD Remaster** by **CLOCKUP**.

## Game Info

| Property | Value |
|----------|-------|
| Engine | YU-RIS (Yuris) |
| Archives | YPF v490 (`pac\*.ypf`) |
| Scripts | `ysbin\yst%05d.ybn`, `yse.ybn`, `ysc.ybn` |
| DRM | SoftDenchi (renames `fraternite_hd.exe` → `fraternite_hd.log` at runtime) |
| Platform | 32-bit MFC140 |
| Text API | `GDI32.TextOutA` (IAT `0x18E028` in main EXE) |

## Pipeline

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + OpenSSL + zlib +
CMake).

Three steps, and one command that performs all three:

```
01_extract.exe          → script_output/extracted_text.json  (pac\bn.ypf scan)
02_translate.exe        → the speaker gate (zero tokens), then
                          script_output/translated_text.json,
                          translation_cache_anthropic.json, and
                          translation_table.tsv in the game folder
python build.py --deploy → winmm.dll into the game folder
00_run_all.exe          → all three in one command
```

The speaker gate runs first inside `02_translate`, before the API key is read. A run that cannot produce a correct answer costs nothing and leaves nothing behind.

`proxy_dll/` builds `winmm.dll`, the IAT hook on `GDI32.TextOutA`.

Step 1 opens `pac\bn.ypf` (YPF v490), decrypts YSTB bodies (XOR `0x6594DAC3`), scans for CP932 Japanese runs, and splits them into per-message entries. The table build produces runtime-byte *variants*: ruby-stripped, leading-`／`-dropped, missing-`」`-closed, missing-`。`-appended, and `？？？` mystery-speaker.

**Runtime translation only** — original game files are never modified. The proxy DLL hooks `TextOutA`, reads the engine's CP932 buffer, looks it up in `translation_table.tsv`, and draws English in an overlay (F1 toggles JP).

## Quick Start

Both halves build from one place. `build.py` has four flags and no more:
`--test`, `--install`, `--deploy`, `--clean`.

```powershell
cd FRATERNITE_HD
python build.py --test --install

build\install\decrypt_translations.bat

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

**WARNING:** Run `decrypt_translations.bat` before the pipeline. Step 2 reads the
encrypted cache and document. Without them it re-translates the whole game (hours,
real money). Nothing in the pipeline can rebuild those files.

`pipeline_cpp` is x64; `proxy_dll` is Win32. The root `CMakeLists.txt` drives
each as a sub-build into one tree.

```
build/pipeline/   pipeline_cpp, x64
build/proxy/      proxy_dll, Win32
build/install/bin/    what the user runs
build/install/game/   what gets copied into the game folder (winmm.dll)
build/install/script_output/   the paid document and cache, encrypted
```

### Why the script is encrypted

The two JSON files are the whole English script of an adult title. The repo is
public, so they are tracked as `.enc` only. The passphrase in `translations_key.txt`
is public — this keeps the script out of search and crawlers, not secret.

```powershell
cd FRATERNITE_HD

:: after a translation run
.\encrypt_translations.bat --encrypt

:: to read them again
.\encrypt_translations.bat --decrypt
```

`.json` is git-ignored (pipeline reads it); `.enc` is committed. Deflated before
encryption because ciphertext does not compress.

## Individual Steps

```powershell
cd build\install\bin
01_extract.exe
02_translate.exe --batch 150
02_translate.exe --test              # smoke run: 1 batch only, from a wiped cache
02_translate.exe --max-batches 4     # 4 batches, keeping the cache and outputs
02_translate.exe --discard-cache     # let --test or --retranslate delete a cache holding paid work
02_translate.exe --file bn.ypf
02_translate.exe --out D:\scratch\table.tsv   # keeps C:\Games\ untouched
cd ..\..\.. && python build.py --deploy
```

All apps take `--dir <project>` and `--game-dir <install>`. `--game-dir` is
required. `--dir` defaults to the project folder found by walking up from the exe.

## Tests

`ctest --preset conan-windows-x64-release` (label `UT`). Step 1 is tested
end-to-end against a synthetic `bn.ypf` built from `analysys/ybn_samples/`.
Key formats are sha256-pinned.

Diagnostics: check `proxy_log.txt` in the game dir for hook status and hit counters.
