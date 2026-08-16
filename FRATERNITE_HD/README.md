# Fraternite HD Remaster (フラテルニテ HDリマスター)

English translation pipeline for **Fraternite HD Remaster** by **CLOCKUP**.

> **AGENT — USE GHIDRA FOR dissasmbly/reverse engineering !!!**
> Ghidra project: `D:\ghidra_projects\Fraternite_HD` (contains `fraternite_hd_dumped.bin` — the live memory dump of the DRM-unpacked main EXE).
> Use headless scripts (`analysys/*.java`) via `analyzeHeadless.bat`. **Never** hand-decode x86 bytes in a script of your own.

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

The speaker gate runs first inside `02_translate`, before the API key is read
and before anything is deleted: a run that cannot produce a correct answer
costs nothing and leaves nothing behind. The runtime table is built last by the
same command, so there is no step to remember between translating and playing.

`proxy_dll/` builds `winmm.dll`, the IAT hook on `GDI32.TextOutA`.

Step 1 opens `pac\bn.ypf` (YPF v490), zlib-inflates every entry, XORs the
`yst*.ybn` YSTB bodies with the archive ScriptKey `0x6594DAC3`, brute-scans the
plaintext for CP932 runs containing Japanese, and splits each run into one
entry per on-screen message. The table build explodes every (JP, EN) pair into the
runtime-byte *variants* the engine may actually render — ruby markers stripped,
a leading `／` directive dropped, a missing `」` closed, a missing `。`
appended, and the `？？？` mystery-speaker form.

**Runtime translation only** — the original game files are never modified. The proxy DLL is a full `winmm.dll` export-forwarder plus an IAT hook on `GDI32.TextOutA`. On each call, the hook reads the engine's current CP932 message buffer, looks it up in `translation_table.tsv`, and draws the English result in a transparent top-most overlay window anchored to the game (F1 toggles Japanese visibility).

## Quick Start

Both halves build from one place. `build.py` has four flags and no more:
`--test`, `--install`, `--deploy`, `--clean`.

```powershell
cd FRATERNITE_HD
python build.py --test --install

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

`pipeline_cpp` is x64 and `proxy_dll` is Win32, so one CMake configure cannot
produce both: the root `CMakeLists.txt` drives each half as a sub-build with
its own Conan profile and toolchain, into one build tree.

```
build/pipeline/   pipeline_cpp, x64
build/proxy/      proxy_dll, Win32
build/install/bin/    what the user runs
build/install/game/   what gets copied into the game folder (winmm.dll)
```

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
required: the game install path differs per machine. `--dir` defaults to the
project folder found by walking up from the executable's own location, and is
required when the exe sits outside the project tree.

## Tests

`ctest --preset conan-windows-x64-release` (label `UT`) runs the GoogleTest
suites in `pipeline_cpp/tests/`. The game is not installed on this machine, so
step 1 is exercised end-to-end against a synthetic `bn.ypf` assembled in-test
from the four decrypted YBN samples in `analysys/ybn_samples/`. The CP932
codec tables and the JSON / TSV / user-prompt byte formats are pinned by
sha256, so a table edit or a formatting slip fails a test rather than quietly
changing every downstream key.

The tables in `src/translate/glossary.cpp` (78 names + the 5 KB system prompt)
and `src/build_tsv/name_fixups.cpp` (a *different*, 26-entry table) are
machine-generated and must never be hand-edited — one wrong kanji silently
breaks a speaker lookup.

## Proxy DLL (`proxy_dll/`)

- `src/proxy.cpp` + `winmm.def` — forwards all 181 winmm exports
- `src/translator.cpp` — IAT hook on `GDI32.TextOutA`; reads engine buf38, pushes translations to the overlay
- `src/translator_logic.{h,cpp}` — pure (OS-independent) TSV parsing + speaker/quote splitter + CP932 helpers (unit-tested under `tests/`)
- `src/overlay.cpp` — transparent top-most window anchored to the game; F1 toggles JP visibility
- `src/iat_hook.{h,cpp}` — generic IAT walk + patch helper

Build: MSVC C++ toolchain (VS 2022) + CMake 3.23+ + ninja, 32-bit target. See `proxy_dll/build_and_deploy.bat` for one-shot rebuild.

Diagnostics: check `proxy_log.txt` in the game dir for hook install status and hit counters.

## SoftDenchi Notes

The DRM protects the main EXE but **not** the IAT import directory — the GDI32 import table is plaintext from the moment `winmm.dll` is loaded. We don't need to wait for an unpack sentinel; `TranslatorInit()` installs the IAT hook immediately in `DLL_PROCESS_ATTACH`. Code-segment reverse engineering uses the live memory dump at `analysys/fraternite_hd_dumped.bin`.

## analysys/

- `dump_dlls.py` — captures the live process memory (main EXE + YSWBP/YSSNP/YSZLB/YSOHC DLLs) from `fraternite_hd.log` while the game is running
- `fraternite_hd_dumped.bin` — the main EXE memory dump (4.5 MB, plaintext code)
- `find_text_funcs_fraternite.java`, `scout_yswbp.java` — Ghidra scout scripts
- `ybn_samples/` — four already-decrypted YBN blobs (`yscfg`, `yse`, `yst_list`,
  `yst00001`). These are the only game data checked in, and
  `ut_extract.EndToEnd_SyntheticArchive` depends on them.
