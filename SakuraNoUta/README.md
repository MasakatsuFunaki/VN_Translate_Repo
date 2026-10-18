# Sakura no Uta — English Translation


> **AGENT — Ghidra is the ONLY allowed EXE disassembly method.**
> When you need to inspect BGI.exe (find the engine state pointer, the
> internal text-render function, opcode tables, IAT entries):
> - Write a `.java` GhidraScript and run it via `analyzeHeadless.bat`.
> - **NEVER** hand-roll a script that scans the EXE for x86 opcode bytes —
>   Ghidra already does this with full disassembly, control flow and type
>   recovery.
> - Existing helper scripts go under `analysys/`.

Automated translation pipeline for **桜の詩** (Sakura no Uta) by SCA-DI / Makura.
BGI / Buriko / Ethornell engine.  Runtime translation via a winmm.dll
proxy that **inline-hooks `BGI.exe!FUN_00433900` (RVA `0x033900`)** —
the engine's core dialogue-text renderer — and substitutes the Japanese
CP932 buffer with an English replacement before the engine lays out
the glyphs.  No `.arc` archives are ever modified.

**Stats:** ~67k unique JP strings across 130+ BurikoCompiledScript files
in `data01099.arc` … `data01999.arc`.

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake).

## Prerequisites

- cmake (>=3.23), conan (>=2), ninja, and the MSVC C++ toolchain (VS 2022
  or its Build Tools) -- the build uses Ninja Multi-Config, never MSBuild
- Anthropic API key — set via **one** of:
  - `ANTHROPIC_API_KEY` env var, or
  - `~/.claude_vn/settings.json` (`primaryApiKey`, auto-loaded)

## Quick Start

```bat
cd SakuraNoUta
python build.py --test --install

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

Runs the whole pipeline: extract → translate → deploy.

`build.py` builds both halves — `pipeline_cpp` is x64, `proxy_dll` is Win32 —
into one build tree and takes four flags:

| Flag | Effect |
|------|--------|
| `--test` | run every test of both halves; gates `--install` and `--deploy` |
| `--install` | stage what the user runs into `build\install` |
| `--deploy` | copy `winmm.dll` + `translations.tsv` into the game folder |
| `--clean` | delete `build\` and stop |

```
build/pipeline/   x64 tree
build/proxy/      Win32 tree
build/install/    bin/ = what you run, game/ = what you copy into the game
```

`BUILD_STRUCTURE.md` describes the root CMake project that drives the two
architectures, and the preset / Conan conventions the two halves share.

## Pipeline

| Step | Executable | Description |
|------|-----------|-------------|
| 1 | `01_extract.exe` | Walk every BGI script in `data*.arc` (BURIKO ARC20 + DSC FORMAT 1.00), classify each bytecode string ref (name / dialogue / narrative) → `script_output/extracted_text.json` |
| 2 | `02_translate.exe` | The speaker gate (5 checks, zero tokens, aborts the step if any fails), then JP → EN via Claude (Opus 4.7, adaptive thinking) with a rolling 50-line context window (resume-safe, cached) → `script_output/translated_text.json`, and finally the de-duplicated `translations.tsv` the game loads (UTF-8, tab-separated, escaped `\r\n\t\\`) |
| 3 | `python build.py --deploy --game-dir DIR` | Copy `winmm.dll` and `translations.tsv` into the game folder |
| — | `03_find_narrative_cg.exe` | Side tool, outside the main order: scan CG arcs for narrative Japanese text (poems / title cards), translate via Claude vision, FreeType-render EN over JP, repack |
| — | `proxy_dll/` | C++ winmm.dll proxy that hooks the BGI message walker and swaps JP buffers for EN at draw time |

The gate and the table build are inside step 2 on purpose: a check someone
has to remember to run does not run, and a translation the game never loads
is not a translation.

### Step-by-step (manual)

```bat
cd build\install\bin
01_extract.exe                    :: Extract JP from .arc archives
02_translate.exe --batch 150      :: Gate, translate (resume-safe), build translations.tsv
cd ..\..\.. && python build.py --deploy --game-dir C:\SakuraNoUta   :: winmm.dll + table
python analysys\make_option_image.py --all --format bmp32 --game-dir C:\SakuraNoUta --deploy
```

Both take `--game-dir`, and neither has a default, because the install path
differs per machine.  `make_option_image.py` also reads it from `VN_GAME_DIR`.

The last line is a **manual step, and `build.py` does not own it.** A choice
option is a picture and not text, so it is replaced by a file rather than by
the hook — *Choice menus (pictures, not text)* below says why, and gives the
one command that puts the Japanese back.

## Options

```bat
00_run_all.exe --test                :: smoke run: 1 batch only -- DELETES THE CACHE
00_run_all.exe --test 4              :: 4 batches then stop -- DELETES THE CACHE
00_run_all.exe --batch N             :: lines per API call (default 150)
00_run_all.exe --max-batches N       :: stop after N API requests, keeping the cache
00_run_all.exe --clean               :: delete cached JSON, then full run
00_run_all.exe --discard-cache       :: let --test or --clean delete a cache holding paid work
00_run_all.exe --dir <p> --game-dir <p>  :: project folder / game folder (--game-dir required)

02_translate.exe --batch N       :: lines per API call (default 150)
02_translate.exe --max-batches N :: stop after N API requests, keeping the cache
02_translate.exe --test N        :: N batches then stop -- DELETES THE CACHE
02_translate.exe --retranslate   :: ignore cache, redo all -- DELETES THE CACHE
02_translate.exe --discard-cache :: let --test or --retranslate delete a cache holding paid work
02_translate.exe --script NAME   :: translate one script only
02_translate.exe --out <path>    :: write the table somewhere other than translations.tsv
02_translate.exe --rebuild-table :: rebuild translations.tsv from the document on disk, no API call
02_translate.exe --box-audit     :: measure the corpus against its message boxes, no API call

03_find_narrative_cg.exe --scan-only | --repack | --no-resume
```

## WARNING: back up the cache, because three flags delete it

**Back up `script_output/translation_cache_anthropic.json` and
`script_output/translated_text.json` after a full run, and periodically during
one. Nothing in this pipeline can rebuild them.**

Everything else is derived and costs nothing to regenerate — `02_translate
--rebuild-table` writes `translations.tsv` back from the document without an API
call, and the coverage report and the DLL come from the build. Those two files
are the originals: they hold what the API was paid for, which is a run of
several hours and tens of dollars.

Three flags delete them:

| flag | what goes | why it is dangerous |
|---|---|---|
| `--test N` (both programs) | the cache **and** the document | **The sharp edge.** It reads as a harmless smoke run, and it deletes both files before it spends anything. One `--test` on a finished run destroys the run. |
| `02_translate --retranslate` | the cache, in effect | It starts from an empty cache and the first save writes that over the file on disk. |
| `00_run_all --clean` | the cache, the document and the extraction | It says what it does. |

**To cap a run without losing anything, use `--max-batches N`.** It stops after N
requests and keeps both files, which is what `--test` reads as if it did.

A guard stands in front of all three. Each refuses once the cache holds more than
`CACHE_DISCARD_THRESHOLD` lines, unless `--discard-cache` is also given, and it
refuses before the API key is read, so a refusal costs nothing.

`python build.py --test` is a different program and a different flag: it runs the
unit tests and touches nothing under `script_output`. It is safe.

## How It Works

### BGI archive format
- `data*.arc` are **BURIKO ARC20** containers: 12-byte magic + u32 file
  count + 128-byte index entries (96-byte name, u32 offset, u32 size,
  24 bytes pad).
- Per-file payload is **DSC FORMAT 1.00** — Buriko's Huffman + 12/4-bit
  LZ codec.  Header has a hash key descrambling the 512-entry codebook.
- `pipeline_cpp/src/bgi/` implements ARC20, DSC and CompressedBG v1
  (v2, the DCT format most CGs use, is decoded by shelling out to
  `TOOLS/garbro/extract_cbg.exe`).
- **TRAP — the engine takes the FIRST arc that holds a name.**  It
  searches `data01099.arc` … `data01999.arc` in name order.
  `data01099.arc` is the update patch: it holds 23 scripts that shadow
  the copies in the original arcs, and 164 lines of `03_picapica_06`
  (the sketching scene) exist only there.  The extractor once let the
  later arc win, so those lines drew in Japanese while the corpus
  reported full coverage.  No test can catch that: the table is checked
  against the corpus, and both came from the wrong copy.  The only
  ground truth is a `miss` in `proxy_log.txt`.  The extractor now keeps
  the first copy and logs every other one as `[shadowed]`.

### Script format (BurikoCompiledScriptVer1.00)
- 28-byte magic + u32 extended-header-size + bytecode + sequential
  CP932 string section at the end of the file.
- The bytecode walks the strings via an internal "next message"
  pointer; it does **not** address them by absolute file offset.
- Therefore **no static repack is needed** — the proxy DLL substitutes
  text at draw time by string content.

### Runtime hook (proxy DLL)
- BGI.exe imports `winmm.dll`.  Our proxy DLL is named `winmm.dll` and
  forwards every winmm export to the real `System32\winmm.dll` via
  naked-jmp stubs (`proxy.cpp`).
- On `DLL_PROCESS_ATTACH` the DLL **inline-trampoline-hooks
  `BGI.exe!FUN_00433900`** (RVA `0x033900`) — the BGI VM's core
  dialogue-text renderer, 7135 bytes.
- **TRAP — the renderer is not `__cdecl`.** It takes **two register
  parameters, ECX and EDX, plus eleven stack dwords, and the caller
  cleans**: both call sites (RVA `0x33369` and RVA `0x338E5`) load ECX
  and EDX immediately before the `CALL` and `ADD ESP,0x2c` after it,
  and the callee spills both registers into its own frame before
  writing either. It reads `[ebp+0x08]..[ebp+0x30]` and nothing above,
  which fixes the stack count at eleven. The CP932 `char *` buffer is
  stack dword 2 (`[ebp+0x0c]` inside the callee). The bare `ret` at the
  single exit proves caller-cleans and nothing more — it does NOT prove
  `__cdecl`. The thirteen-stack-parameter shape is real but belongs to
  the WRAPPER at RVA `0x338B0` (`ret 0x34`), which repacks its arguments
  into the above. `proxy_dll/src/text_display_abi.{h,cpp}` holds the two
  naked thunks that reproduce the convention; re-derive it with
  `analysys/verify_call_contract.java`.
- **TRAP — capture incoming registers in a naked function or not at
  all.** An `__asm` block at the top of an ordinary C++ function is too
  late: the compiler's prologue had already loaded the hook's own text
  argument into EDX, so the DLL logged its own pointer as the engine's
  for a whole session and passed the text buffer where the engine wanted
  a ruby context. The engine reads `[EDX+0x24]` on that path, which
  turned every ruby-bearing untranslated line into an access violation.
- The hook strategy is the same as `amakano2pe`: 5-byte JMP rel32 at
  the function entry, 6-byte stolen prologue (`55 8B EC 83 E4 F8`)
  relocated into a heap trampoline, JMP back to site+6.  When the
  engine calls the function we look the CP932 buffer up in our
  TSV and -- on a hit -- forward the original with our English
  buffer instead.  The engine then lays out and renders **English
  with the original typewriter / line-break / styling code paths
  intact**.  No per-glyph reconstruction or overlay window is needed.
- **A chained box flows as one paragraph.**  A box filled by several
  script strings (each but the last ending in 0x0A) once drew one
  string per line, which left short ragged lines and put the engine's
  28 px chain indent on wrapped continuation lines.  On the panel the
  DLL now replaces a substituted segment's trailing 0x0A with a
  joining space, keeps the pen column per box, and wraps the next
  segment from that column.  The backlog's whole-box rows join with a
  space so both surfaces read the same.  An untranslated segment still
  passes through whole, so a mixed chain can continue mid-line in
  Japanese.
- **A break inside one string flows too.**  The writer's breaks fit
  Japanese lines.  English is wider, so the segment before a break
  wrapped and left a stub line ("weighty in / the hand.").  `build_tsv`
  turns every break inside the English into a space, and the DLL wraps
  the value at 71 columns.  The key keeps its break, because the engine
  passes the whole string with the break inside the buffer.
- Pure-ASCII / NUL / control-only buffers and JP buffers without a
  translation entry pass through to the original walker unchanged.

### Choice menus (pictures, not text)

The hook draws every line of dialogue.  It does not draw a choice menu,
and it never can: **a choice option is a picture of the sentence, not
text the engine lays out.**

- The compiled script builds the menu itself, four dwords per option at
  stride 16.  The first field looks like a label and is the **asset
  name**, `selectNN-M`.  The fourth field holds the option's CP932
  sentence, and nothing draws it.
- `data02502.arc` holds those 34 pictures, `select01-1` to `select17-2`,
  each a `CompressedBG___` image of 2982 x 58 x 32 — three 994 x 58
  tiles side by side.  Tile 0 is the option at rest, tile 1 the option
  under the cursor.
- **The engine looks a name up as a file before it looks in an archive.**
  A file named after an option, in the game folder, replaces that option.
  The archive is read and never written.
- The engine reads a plain bottom-up Windows BMP at 24 and 32 bits.  It
  refuses a PNG, naming the bit count.  32-bit is what ships, because
  the plate is translucent and only the alpha channel keeps it so.

`analysys/make_option_image.py` draws the 34 replacements from the same
`translations.tsv` the DLL loads.  **It is a manual step: `build.py`
ships `winmm.dll` and `translations.tsv` and knows nothing about the
pictures**, because a change to how a game deploys has to be ported to
every other game in this repository.

```bat
cd SakuraNoUta
python analysys\make_option_image.py --all --format bmp32 --game-dir C:\SakuraNoUta --deploy
```

Give `--game-dir` the game folder, or set `VN_GAME_DIR`.  The tool reads the
archive and `translations.tsv` from it, and `--deploy` writes the pictures
back into it.  It stops with a message when neither is given.

A picture cannot overflow — what leaves the tile is clipped, and
clipping loses words — so the point size falls until the longest of the
34 sentences fits, and every option is drawn at that one size.  It is
23 pt in Segoe UI today, and the tool refuses and names the widest
sentence rather than deploying a clipped picture.  `python
analysys\ut_option_image.py` runs that fit over all 34 sentences of
`translations.tsv` and checks that each one stays inside its tile.

To put the Japanese menus back, delete the 34 files:

```bat
del C:\SakuraNoUta\select??-?
```

### How the target was found
The engine is analysed as a **runtime memory dump**, not as the on-disk
EXE: `analysys/dump_process.py` reads a running `BGI.exe` and rewrites
the section headers so Ghidra parses the dump as a memory-layout PE,
capturing code that only exists after relocation.  `analyze_dump.bat`
imports the result (`analysys/bgi_dumped.exe`) into a Ghidra project.
Because the dump is based at `0x790000` rather than the on-disk
`0x400000`, Ghidra names the same function differently in each — the
RVA is the stable identifier, and every RVA below is an RVA.

Six Ghidra headless scripts under `analysys/` then narrow the search,
each writing its own log beside itself:

1. `find_text_paths_sakura.java` → `ghidra_text_paths.log`.  The main
   survey, in five passes: a full decompile of the hook target at RVA
   `0x033900`; its callers to depth 3; its callees to depth 2; every
   caller of GDI's text APIs (`TextOutA`, `TextOutW`, `ExtTextOut`,
   `DrawText`, `PolyTextOut`, `GetGlyphOutline`, `GetTextExtentPoint32`),
   marking which reach the hook target and which are **sibling** paths
   that do not; and the CP932 render leaves — the functions that call a
   rasterizer directly.  Two leaves come out: RVA `0x02CA20` (3 params,
   1334 bytes) and RVA `0x02DE40` (2 params, 446 bytes).
2. `decomp_text_siblings.java` → `ghidra_text_siblings.log`.  Decompiles
   the six sibling renderers the survey found (RVAs `0x02DE40`,
   `0x02CA20`, `0x02CF60`, `0x02E060`, `0x02E100`, `0x02D020`) to see
   which of them draws text that never passes through the hook target.
3. `decomp_string_renderers.java` → `ghidra_string_renderers.log`.
   Decompiles the four callers of the tiny per-glyph wrappers
   (`0x02D020`, `0x02E100`), hunting for a function that loops over a
   CP932 string and rasterizes inside the loop.
4. `verify_calling_convention.java` → `ghidra_calling_conv.log`.  Dumps
   the hook target's first 60 instructions and its exits with raw
   bytes.  This is what yields the 6-byte prologue
   (`55 8B EC 83 E4 F8`) the trampoline steals.
5. `verify_call_contract.java`.  Settles the calling convention of a
   function given by RVA: which registers are read before written
   (incoming register parameters), which `[EBP+n]` slots the body
   reads, the `RET` form, and per call site the argument set-up and the
   cleanup.  This is the evidence behind the ECX/EDX + 11-stack
   convention above.  Takes the RVA and the output path as arguments.
6. `decomp_crash_site.java` → `ghidra_crash_site.log`.  Decompiles the
   tag-lookup function at RVA `0x032F30` and the call site inside the
   hook target that reaches it, showing what it dereferences on the
   first render.  A defensive SEH hook once wrapped that function; it
   has been removed, because what it was catching was the proxy DLL's
   own clobbered register parameter and wrapping it also hid genuine
   engine faults.

Re-run any of them with:

```bat
analyzeHeadless.bat D:\ghidra_projects SakuraNoUtaDumped ^
    -process bgi_dumped.exe -noanalysis ^
    -scriptPath D:\Projects\VN_TRANSLATE\SakuraNoUta\analysys ^
    -postScript find_text_paths_sakura.java
```

`analysys/README.md` indexes these and the corpus, spend and runtime
tools that live beside them.

## File Layout

```
SakuraNoUta/
├── CMakeLists.txt             # root build: drives both halves, owns the install
├── CMakePresets.json          #   preset "windows-release"
├── build.py                   # the build front end (--test --install --deploy --clean)
├── BUILD_STRUCTURE.md         # the build layout and its conventions
├── build/                     # the one build folder (gitignored)
│   ├── pipeline/              #   x64 tree
│   ├── proxy/                 #   Win32 tree
│   └── install/               #   bin/ = what you run, game/ = what you copy
├── pipeline_cpp/              # C++ translation pipeline (Conan 2 + Boost + CMake)
│   ├── conanfile.py           #   dependencies: boost, openssl, freetype, stb, gtest
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── common/            #   file IO, CP932<->UTF-8, JSON helpers
│   │   ├── bgi/               #   BURIKO ARC20 + DSC + CompressedBG v1 codecs
│   │   ├── extract/           #   BGI bytecode walker (step 1)
│   │   ├── translate/         #   Anthropic client (Boost.Beast+OpenSSL),
│   │   │                      #   glossary, speaker gate, batching/cache (step 2)
│   │   ├── build_tsv/         #   translations.tsv builder (end of step 2)
│   │   ├── cg/                #   narrative-CG scan/render/repack
│   │   └── apps/              #   00_run_all, 01..03 mains
│   └── tests/                 #   GoogleTest suites (ctest label UT)
├── script_output/
│   ├── extracted_text.json    # JP text per script
│   ├── translated_text.json   # JP+EN paired
│   └── translation_cache_anthropic.json
├── translations.tsv           # Runtime lookup table for the DLL
├── proxy_dll/                 # winmm.dll proxy (message-walker hook)
│   ├── CMakeLists.txt
│   ├── conanfile.py
│   ├── profiles/x86           #   Conan profile pinning arch=x86
│   ├── build_and_deploy.bat
│   ├── src/ …
│   └── tests/                 # gtest suite for translator_logic
├── tools/                     # extract_cbg.exe C# sources (GARbro-based)
└── analysys/                  # Ghidra scripts + RE notes
```

## Characters

直哉=Naoya, 稟=Rin, 健一郎=Ken'ichirou, 圭=Kei, 藍=Ai, 雫=Shizuku, 真琴=Makoto
(鳥谷 Toritani), 校長/紗希=the headmaster, Toritani Saki, 優美=Yumi, 里奈=Rina,
香奈=Kana, 吹=Sui, 葛=Kuzu, 明石=Akashi (亘 Wataru; 小牧 Komaki and 小沙智 Kosachi
are his sisters), 伯奇=Hakuki.  The sex of every speaker is in the prompt's
character dossier in `glossary.cpp`; it came from VNDB (v562), fetched with
`analysys/vndb_cast.py`, and `analysys/pronoun_sweep.py` checks the English
against it.  美和, 冬馬, 篠田 and 弓子 are not in this game and survive only
as dead name rows in `extra_name_translations()`.

Kei is 夏目圭 — 圭, the kanji the scripts use 1,262 times as a speaker, 29
times as a name record and 52 times inside 夏目圭. This line used to read 景,
which the corpus carries 169 times over 162 entries and every one of them
inside a common noun: 風景 94, 光景 33, 景気 21, 雪景 9, 景色 4, 背景 4,
景観 1, 全景 1, 夜景 1, and one inside the print title 『猿わか町よるの景』.
Never standalone, never as a speaker, never as a name record — so the glossary
grew a second "Kei" row from a kanji nobody in this game is called.

## Notes

- All steps are resume-safe — Ctrl+C and restart at any time; the
  per-line cache is in `script_output/translation_cache_anthropic.json`.
- BGI engine renders text via GDI's `TextOutA` with CP932 byte buffers,
  so the DLL hook is straightforward — no D3D9 surface scraping or
  font-atlas munging required.
- The DLL word-wraps every English value at 71 columns into the box's
  four lines.  A value that needs more is cut and marked, and the box
  audit at the end of `02_translate` names it.
