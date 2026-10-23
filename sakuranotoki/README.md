# Sakura no Toki — English Translation

Automated translation pipeline for **サクラノ刻** (Sakura no Toki) by
SCA-DI / Makura — DMM edition.

**Engine:** Artemis (PF8 archives, Lua-source `.ast` scripts).
**DRM:** sakuranotoki.exe is a CrackProof-wrapped launcher (5.8 MB,
~5.3 MB encrypted overlay) — the real Artemis engine is unpacked into
memory at runtime.  Our DLL proxies `version.dll`, which the launcher
imports and which is not a KnownDLL, so a copy in the game folder wins
the search order: by the time the engine has finished mapping itself we
are already loaded inside its process.

**Stats (extraction):** 92,774 entries across 234 `.ast` script files in
`sakuranotoki.pfs` (+ `.000`, `.001`, `.002`, `.010`) — 41,257 dialogue,
35,351 name plates, 15,933 narrative, 233 title, across 53,653 distinct
Japanese strings.  Every dialogue and narrative entry carries a `speaker`.

The pipeline is **C++** (`pipeline_cpp/`, Conan 2 + Boost + CMake).

> **Status.** The paid translation run has not happened.  `script_output/`
> holds the extraction and nothing else: there is no translated document and
> no cache, and `translations.tsv` is a 58-row pilot from May 2026 whose
> rows still spell 鳥谷 "Toriya".  The table builder repairs that spelling on
> the next build, so the pilot rows are stale rather than wrong forever.

> **The runtime is unproven.**  Nobody has seen an English line drawn by this
> game.  The last recorded session is from 2026-05-02: the memory scanner
> overwrote 86 heap buffers in place, four hardware breakpoints on candidate
> functions hit 0 times, five IAT hooks hit 0 times, and whether English
> reached the screen was never recorded.  The proxy sources are newer than
> the DLL in the game folder.  Getting English on screen is a separate piece
> of work and none of it is done.

## Prerequisites

- cmake (>=3.23), conan (>=2), ninja, and the MSVC C++ toolchain (VS 2022
  or its Build Tools) -- the build uses Ninja Multi-Config, never MSBuild
- Anthropic API key — set via **one** of:
  - `ANTHROPIC_API_KEY` env var, or
  - `~/.claude_vn/settings.json` (`primaryApiKey`, auto-loaded)

## Pipeline

Three steps, and `00_run_all.exe` is all three in one command.

| Step | Executable | Description |
|------|-----------|-------------|
| 1 | `01_extract.exe` | Walk every `.ast` in `*.pfs`, decrypt (XOR with `SHA-1(index_bytes)`), parse as Lua, capture every dialogue / narrative / speaker / title string with the speaker it belongs to, log every shadowed copy -> `script_output/extracted_text.json` |
| 2 | `02_translate.exe` | The speaker gate (6 checks, zero API tokens), then JP -> EN via Claude with a rolling context window per script (resume-safe, cached), then the coverage report and `translations.tsv` |
| 3 | `python build.py --deploy` | Copy `version.dll` and `translations.tsv` into the game folder |
| ext | `03_find_narrative_cg.exe` | Scan PFS for CG images with narrative text, Claude vision detection, record candidates.  A separate investigation, not part of the run above |

The gate and the table builder are library code inside step 2 rather than
commands of their own: the checks that decide whether a run can be correct
belong to the command that pays for it, and a translation the game never
loads is not a translation.

## Building

Both halves build from the game root, into one `build/` tree — `pipeline_cpp`
is x64, `proxy_dll` is Win32, and `build.py` hides the split:

```bat
cd sakuranotoki
python build.py --test --install   :: build both halves, run every test, stage
python build.py --clean            :: delete build/ and stop
```

`--install` stages `build/install/bin` (what you run) and
`build/install/game` (what goes into the game folder).  The two are kept
apart: the proxy is 32-bit and carries a system DLL's name, so it must not
sit beside the x64 executables.

## Running

`<G>` is the game folder, and every command runs from `sakuranotoki\`.  The API
key auto-loads from `%USERPROFILE%\.claude_vn\settings.json`, or from
`ANTHROPIC_API_KEY`.

Run these four first.  None of them sends a request or reads the key:

```bat
python build.py --test --install                              :: build both halves, 146 + 1 tests
build\install\bin\01_extract.exe --game-dir <G>               :: decrypt + extract from .pfs
build\install\bin\02_translate.exe --gate-only --game-dir <G> :: six checks, exit 0 or 2
python analysys\spend_predict_run.py --batch 150              :: requests, lines, post-run cache
```

The gate has to exit 0 before anything is bought.  The projection states what
the run will send: **723 requests carrying 53,733 lines**, ending on a cache of
53,653 entries.

Then buy one request and read its `in=` and `out=` off the `CLAUDE RAW ANSWER`
line, which is the only honest measurement of what the run costs:

```bat
build\install\bin\02_translate.exe --max-batches 1 --batch 150 --game-dir <G>
python analysys\spend_extrapolate.py --batch 150 --in-tok <in> --out-tok <out>
```

The full run, and the deploy after it:

```bat
build\install\bin\02_translate.exe --batch 150 --game-dir <G>
python build.py --deploy --game-dir <G>
```

It is resume-safe: Ctrl+C and restart, and a line already in the cache is never
bought twice.  `--max-batches N` caps it without losing work.  `--test`,
`--retranslate` and `--clean` delete paid work — see the warning below.

> **WARNING: back up `script_output\translation_cache_anthropic.json` and
> `script_output\translated_text.json`.**  Copy both after the first paid
> request, during the run, and at the end.  Nothing in this pipeline can
> rebuild them, and `02_translate --test N`, `02_translate --retranslate` and
> `00_run_all --clean` all delete them.  A guard refuses each of those three
> once the cache holds real work, unless `--discard-cache` is given as well,
> and it refuses before the key is read.  `python build.py --test` is a
> different program: it runs the unit tests and touches nothing under
> `script_output`.

Options for `02_translate.exe`.  The first two send no request and read no
API key, so they cost nothing:

```bat
02_translate.exe --gate-only     :: run the speaker gate over the extraction and stop
02_translate.exe --rebuild-table :: rebuild translations.tsv and the coverage report
                                 :: from the translated document already on disk, and stop
02_translate.exe --flow-breaks   :: turn a break inside a table value into a space and
                                 :: let the engine wrap it (see the note below)
02_translate.exe --batch 50      :: lines per API call
02_translate.exe --test 4        :: 4 batches then stop, from a clean cache
02_translate.exe --max-batches 4 :: 4 batches then stop, keeping the cache
02_translate.exe --retranslate   :: ignore cache
02_translate.exe --discard-cache :: let --test or --retranslate delete a cache holding paid work
02_translate.exe --include-untranslated  :: keep rows whose English is still Japanese
02_translate.exe --out <path>    :: write the table somewhere other than <dir>\translations.tsv
02_translate.exe --script script\01_01.ast   :: one file only
```

No Japanese string in this corpus contains a line break: 0 of 92,774.
`--flow-breaks` therefore acts only on a break the English answer introduced.
The default keeps those breaks, because whether the engine wraps a line for
itself is a runtime question and nobody has measured it yet.

All apps take `--dir <project>` and `--game-dir <install>`. `--game-dir` is
required: the game install path differs per machine. `--dir` defaults to the
project folder found by walking up from the executable's own location, and is
required when the exe sits outside the project tree.

### After the run

```bat
build\install\bin\02_translate.exe --rebuild-table --game-dir <G>  :: table + coverage, no API call
python analysys\translation_coverage.py                           :: coverage counted three ways
python analysys\pronoun_sweep.py analysys\vndb_cast.json          :: pronouns against each sex
python analysys\patch_translations.py patches.json --apply        :: hand edits, cache and document
python analysys\evict_cache_range.py "script\01_01.ast" 0 20      :: buy a range of lines again
```

Half of those read a document and a cache this game does not have yet.
`python analysys\make_probe_project.py %TEMP%\snt_probe` writes a throwaway
tree so each one can be rehearsed before the run exists.

**The deploy ships a table nobody has seen drawn.**  `--deploy` copies
`version.dll` and `translations.tsv` into the game folder and overwrites what
is there.  The runtime is unproven, so back up both files in the game folder
before the first deploy.

## How it works

### PF8 archive format
- Header: 3-byte `pf8` magic + u32 LE `index_size`.
- Index: u32 entry_count, then per-entry `(u32 name_len, name, u32 res, u32 offset, u32 size)`.
- File payloads: every entry XOR-encrypted with **SHA-1(index_bytes)**,
  applied modulo 20 from the start of each file.  Same key across all
  files inside one archive.  Reference: GARbro `Artemis/ArcPFS.cs`.

- **TRAP — two archives hold the same name, and only one copy is the one
  the game draws.**  `sakuranotoki.pfs.010` supersedes the base archive by
  content: it declares `game_ver="1.0.1"` where the base declares `"1.0.0"`,
  and it carries four proofreading fixes.  No other archive holds a script.
  The general "higher suffix wins" ordering is consistent with this corpus
  but is not proven by it, so `ARCHIVE_ORDER` in
  `pipeline_cpp/src/artemis/pfs_archive.cpp` states the scoped rule and the
  extractor logs every copy that loses:

  ```
  [shadowed] script\03_06_4.ast in sakuranotoki.pfs loses to sakuranotoki.pfs.010
  ```

  Four such lines, for the four duplicated names.  Read them.  Taking the
  wrong copy is the failure that reports 100% coverage while the game draws
  Japanese: the table is checked against the corpus, and both would come
  from the wrong copy.

### `.ast` script format
- After XOR-decryption: plain UTF-8 Lua source file.
- Layout (machine-generated):

  ```lua
  astver = 2.0
  ast = {
      block_NNNNN = {
          {"text"},
          text = { vo = { ... }, ja = {
              { name = {"<speaker>"},
                "「dialogue」",
                {"rt2"} },
          } },
      },
  }
  ```

- `pipeline_cpp/src/extract/` walks the file line-by-line, recognising:
   - `name = {"..."}` -> speaker (type=`name`)
   - `name = {"<speaker>", "<what the box shows>"}` -> the same, taking the
     first element as the speaker and keeping the second as `display`
   - bare `"...",` inside a `ja = {...}` block -> dialogue / narrative
   - `{"savetitle", text="..."}` -> chapter title
- Engine commands (`{"bg", file=...}` etc.) are skipped even when they
  contain JP filenames.
- A plate is kept whether or not it carries a Japanese character, so the
  `？？？` an unintroduced speaker draws still ends the previous speaker's
  run.  `has_jp` records which ones those are.

- **TRAP — Artemis splits a kanji compound and its reading across adjacent
  strings.**  The reading rides on a `{"ruby", text="つぶらぎ"}` command and
  the base compound, `円木`, is a string of its own.  One spoken sentence
  becomes three corpus entries: the head, the base, and the tail.  496
  entries are such a fragment.  The extractor never joins them and nothing
  downstream should: the engine reassembles them at draw time, so a joined
  key would match nothing the engine hands over.  The prompt carries a rule
  for translating a bare two-kanji fragment as the proper noun it usually is.

### The runtime translation table

`translations.tsv` is UTF-8 `<jp>\t<en>` per line, with `\n`, `\t`, `\r` and
`\\` escaped in both columns.  `pipeline_cpp/src/build_tsv/` writes it at the
end of step 2, and `--rebuild-table` writes it again from the document
already on disk without sending a request.  Each row is derived, in order:

1. the last translation for that Japanese string wins, falling back to the
   resume cache when the document is stale;
2. superseded spellings are rewritten, but only where the Japanese of that
   row carries the term — so `鳥谷` reading "Toriya" becomes "Toritani" and a
   stray "Toriya" with no `鳥谷` behind it is left alone;
3. the English is given the key's leading U+3000 indent and no other;
4. with `--flow-breaks`, a break inside the value becomes a space.

Every speaker plate then gets a row from the glossary, unless the document
already answered for it — the document wins, and a disagreement is logged.
Two plates that would draw the same English are reported: the player cannot
tell those two characters apart.

### Runtime translation (proxy DLL)
- See `proxy_dll/README.md` for the deferred-load + IAT-hook strategy that
  was tried against CrackProof.  Nothing there is confirmed working.

## Characters

Returning from Sakura no Uta:
直哉=Naoya, 圭=Kei, 藍=Ai, 健一郎=Ken'ichirou, 真琴=Makoto, 紗希=Saki,
麗華=Reika, 稟=Rin, 優美=Yumi, 里奈=Rina, 雫=Shizuku, 香奈=Kana,
桜子=Sakurako, 鈴菜=Suzuna, ルリヲ=Ruriwo, ノノ未=Nonomi, 奈津子=Natsuko,
霧乃=Kirino, 寧=Nei, 明石=Akashi, トーマス=Thomas, フリッドマン=Friedman,
章一=Shouichi, 片貝=Katagai.

New in Sakura no Toki:
静流=Shizuru, 心鈴=Misuzu, 放哉=Housai, 依瑠=Eru, 礼次郎=Reijirou,
破戒=Hakai, 坂本=Sakamoto, 村田=Murata, 吉田=Yoshida, 吉沢=Yoshizawa.

The sex of every speaker above a hundred lines is in the prompt's character
dossier in `pipeline_cpp/src/translate/glossary.cpp`.  It came from VNDB
(v20431), fetched with `analysys/vndb_cast.py v20431 analysys/vndb_cast.json`.
景 and 冬馬 are not in this game; 寧 reads Nei, not Yasushi, and 依瑠 reads Eru.
氷川ルリヲ reads **Ruriwo**, not Rurio: `script\05_03_2.ast` offset 1216 writes
the name in Latin as `hikawa ruriwo`, offset 1246 turns those letters into the
anagram `A Raw Hour Kiwi`, and `03_mak03_1` offset 1343 counts twelve of them.
The May pilot shipped Rurio, so `superseded_name_renderings()` carries the old
spelling and the next table build repairs it.
The speaker table holds 84 labels and the corpus uses 84, which gate check 5
asserts in both directions.

## Pinned terminology

Beside the names, the prompt carries a `<terminology>` block: the work
titles, family names, places, prizes and motifs that recur often enough to
drift.  A term is pinned when it occurs at least ten times in the corpus;
below that the rolling context window carries it and a pin buys nothing.
Each row in `terminology_translations()` has its entry and script count
beside it, measured with `analysys/corpus_terminology_survey.py`.

Terminology is prompt-side only.  A term names a fragment inside a line, and
the engine hands over whole lines, so a table row keyed on one would answer
for text the game never asks about.  A unit test enforces that.

`superseded_renderings()` and `superseded_name_renderings()` are the repair
path for a spelling a paid run already produced.  Each row names a term and
the English it used to be given, the table builder rewrites the old spelling
wherever that row's Japanese carries the term, and re-running the builder
costs nothing.  That is how the pilot table's "Toriya" becomes "Toritani"
without buying the line again.

## File layout

```
sakuranotoki/
├── CMakeLists.txt                 root: drives both halves, owns the install
├── CMakePresets.json              preset "windows-release"
├── build.py                       the front end: --test --install --deploy --clean
├── pipeline_cpp/                  C++ translation pipeline (Conan 2 + Boost + CMake), x64
│   ├── conanfile.py               boost, openssl, stb, gtest
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── common/                file IO, CP932<->UTF-8, JSON helpers
│   │   ├── artemis/               PF8 reader + SHA-1 XOR decryption
│   │   ├── extract/               .ast Lua line walker (step 1)
│   │   ├── translate/             Anthropic client, glossary, speaker gate, batching/cache (step 2)
│   │   ├── build_tsv/             translations.tsv builder, called at the end of step 2
│   │   ├── cg/                    narrative-CG scanner
│   │   └── apps/                  00_run_all, 01_extract, 02_translate, 03_find_narrative_cg mains
│   └── tests/                     GoogleTest suites (ctest label UT)
├── script_output/
│   ├── extracted_text.json        JP text per script, with the speaker of each line
│   ├── translated_text.json       JP+EN paired            (not written yet)
│   ├── coverage_report.json       per-script English against untouched Japanese
│   └── translation_cache_anthropic.json                   (not written yet)
├── translations.tsv               Runtime lookup table for the DLL
├── proxy_dll/                     version.dll proxy + IAT hook, Win32
│   ├── conanfile.py               gtest (tests only; the DLL has no dependencies)
│   ├── CMakeLists.txt
│   ├── profiles/x86               the Conan profile that pins arch=x86
│   ├── build_and_deploy.bat
│   └── src/
│       ├── version.def
│       ├── dllmain.cpp / proxy.cpp
│       ├── iat_hook.{h,cpp}
│       ├── translator.{h,cpp}
│       ├── translator_logic.{h,cpp}
│       └── log.{h,cpp}
├── build/                         the only build folder (git-ignored)
│   ├── pipeline/                  x64 tree
│   ├── proxy/                     Win32 tree
│   └── install/                   bin/ to run, game/ to copy into the game
└── analysys/                      corpus, spend and reverse-engineering tools
    └── README.md                  one line per tool, with its scope
```

`**/analysys/` is git-ignored for the whole repository, so those tools live
on disk and are indexed in `analysys/README.md` rather than tracked.  The
ignore rule protects the public repository and stays as it is.  Two of them
are the ones to reach for first: `corpus_terminology_survey.py` ranks the
vocabulary a run has to pin, and `make_probe_project.py` builds a throwaway
document and cache so the tools that need those can be run before the paid
run exists.

## Notes

- All steps are resume-safe — Ctrl+C and restart at any time; the
  per-line cache is in `script_output/translation_cache_anthropic.json`.
- The CrackProof launcher means **the static EXE is opaque**.  To
  identify the engine's text-render function, dump the running process
  with Scylla / x64dbg-Scylla / ProcessHacker after the menu is up,
  then re-run the Ghidra script from `analysys/` against the dump.
  The DLL skeleton is ready; only `dllmain.cpp::attempt_install_hook`
  needs to be retargeted once the function RVA is known.
- Image rendering: most `image\bg\*.png` are pure backgrounds with no
  Japanese.  Narrative text typically lives under `image\cg\` and a
  small subset of `image\ev\`.  Use `03_find_narrative_cg.exe --max N` for
  a bounded test before the full sweep.
- The CG scanner has a local-OCR pre-filter stage that would reject
  text-free images before spending vision tokens, but no OCR engine is
  wired in — it logs `[ocr] 0 method(s): none`, passes everything through,
  and Claude does the detection.  Only the dimension and path-prefix checks
  bound the token cost, so use `--max N` first.
