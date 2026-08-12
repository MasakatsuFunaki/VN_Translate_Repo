# Visual Novel Translation Pipelines

JP→EN translation for eleven visual novels plus one compatibility patch (`kishin_hisho`).
Each game has a C++ pipeline (extract → translate via Claude → TSV) and a proxy DLL that
hooks the engine's text path at runtime. C++17 / Conan 2 / CMake / MSVC / GoogleTest.
Anthropic API via Boost.Beast + OpenSSL (no SDK).

## Games

| Folder | Game | Engine | JP lines | DLL |
|--------|------|--------|----------|-----|
| `EXTRAVAGANZA_CE` | EXTRAVAGANZA ~Mushi Mederu Shoujo~ CE | BLACKCyc (SPT) | 141k | `winmm` + repacked SPT |
| `EXTRAVAGANZA_MATSURO_CE` | EXTRAVAGANZA ~Matsuro~ CE | BLACKCyc (SPT) | ~6k | `winmm` |
| `mushigurui_HD10` | Mushigurui HD10 | BLACKCyc (SPT) | 13k | `winmm` |
| `amakano2pe` | Amakano 2 PE | CatSystem2 (KIF) | 57k | `winmm` |
| `CROSS_CHANNEL` | CROSS†CHANNEL FC | WillPlus/AdvHD | 54k | `xinput1_3` |
| `shingakkou` | Shingakkou ~Noli me tangere~ DL | DDSystem/PIL (DDP3) | 53k | `winmm` |
| `FRATERNITE_HD` | Fraternite HD | YU-RIS (YPF v490) | 34k | `winmm` |
| `Adieu` | Adieu | SiglusEngine | 23k | `winmm` |
| `SakuraNoUta` | Sakura no Uta | BGI/Buriko | ~67k | `winmm` |
| `sakuranotoki` | Sakura no Toki | Artemis (PF8/Lua) | ~93k | `version` |
| `FAVORITE` | 夜、灯す。 | FVP (`Sakura.hcb`) | 61k | `winmm` |
| `kishin_hisho` | Kishin Hishou Demonbane | Nitroplus + AlphaROM | — | helper DLL |

`kishin_hisho` is not a translation — it patches AlphaROM so the 2006 game runs on Windows 11.

## Folder layout

```
<game>/
  build.py            --test --install --deploy --clean
  CMakeLists.txt      drives pipeline (x64) + proxy (Win32) as separate sub-builds
  pipeline_cpp/       extract/translate executables, GoogleTest
  proxy_dll/          runtime hook DLL (32-bit, injected into game)
  analysys/           RE harnesses, Ghidra scripts, verification
  script_output/      extracted JSON, translation cache
  build/              pipeline/ + proxy/ + install/bin + install/game
```

## Pipeline

| Stage | What |
|-------|------|
| `01_extract` | Decrypt/decompress archive → `extracted_text.json` |
| `02_translate` | Speaker gate → Claude translation (cached, resumable) → `translation_table.tsv` |
| `03_*`–`05_*` | Per-game extras (archive repack, CG translation, etc.) |
| `00_run_all` | Extract + translate + build table in one shot |
| `--deploy` | Copy DLL + table into the game directory |

## Build & run

```bat
cd <game>
python build.py --test --install

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\01_extract.exe
build\install\bin\02_translate.exe --batch 150
python build.py --deploy
```

Every executable takes `--help`, `--dir <project>`, `--game-dir <install>`.
`--dir` auto-discovers the project root from the executable's location.
API key: `ANTHROPIC_API_KEY` env var, or `primaryApiKey` in `~/.claude_vn/settings.json`.
Model: set per game in `pipeline_cpp/src/translate/glossary.h`, currently Opus 4.7.

## Releases

The repository root has its own `build.py`. It builds nothing itself — each
game owns two architectures, its own presets and its own install target — it
calls each game's `build.py` and collects what they stage:

```powershell
python build.py --list              :: which games ship, and which are skipped
python build.py --install           :: build them all, stage install/<game>/
python build.py --test --install    :: run each game's suite first
python build.py --only shingakkou --install
python build.py --clean             :: delete every build tree and install/
```

**`.gitignore` is the manifest.** A game listed there is neither tracked nor
built nor shipped; every other top-level folder with a `build.py` and a
`CMakeLists.txt` is. Adding or removing a line changes what a release
contains, so the set that builds is by construction the set that ships.

`install/<game>/` keeps the per-game split: `bin/` is what you run, `game/` is
what you copy into the game folder. They stay apart because each proxy is a
32-bit DLL named after a system DLL — beside the x64 executables the loader
picks it up and then refuses it.

Cutting a release: run `python build.py --test --install` locally, then push a
tag. `v0.1.0` publishes a release; `v0.1.0-rc1` publishes a prerelease (any
tag containing `-`); `workflow_dispatch` builds the artifact without creating
a release entry. The workflow builds on `windows-2022`, zips `install/`, and
attaches it — GitHub adds the source archives itself, so a release is the
source plus every shipped game's binaries.

CI does not run the tests. Several suites read fixtures from `script_output/`,
which is the game's own text and is not in the repository, so they cannot pass
on a clean checkout — the gate is the local `--test` run before you tag. What
CI does enforce is that no game text reached `install/`: the extraction, its
translation and the runtime tables are the publisher's script, and both
`build.py` and the workflow fail rather than ship them.

## Notes

- Translation is runtime — game archives stay untouched (`EXTRAVAGANZA_CE` is the exception: repacked SPT).
- Every stage is resume-safe. `Ctrl+C` and restart.
- `TOOLS/` — shared tooling (GARbro, OCR). `docs/` — engine-agnostic recipes.
- Per-game READMEs carry engine formats, hook addresses, and traps. Start there.
