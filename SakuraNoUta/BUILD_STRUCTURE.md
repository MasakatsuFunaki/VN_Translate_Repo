# Build structure

How this game builds, and the pattern the other games follow.

## The problem

A game has two halves that need different architectures:

- `pipeline_cpp` — **x64**. The tools that read the archives and call the API.
- `proxy_dll` — **Win32**. It is injected into a 32-bit game process.

One CMake project has one toolchain, so one configure cannot produce both. The
root project therefore builds nothing itself: it drives each half as a separate
sub-build with its own Conan profile, its own toolchain file and its own binary
directory, and it owns the install that collects both.

## Layout

```
<Game>/
├── CMakeLists.txt          # root: drives both halves, owns the install
├── CMakePresets.json       # preset "windows-release"
├── build.py                # the front end: --test --install --deploy --clean
├── BUILD_STRUCTURE.md
├── pipeline_cpp/           # x64 half: CMakeLists.txt, CMakePresets.json, conanfile.py
├── proxy_dll/              # Win32 half: same three files, plus profiles/x86
└── build/                  # ONE build folder for the whole game
    ├── pipeline/           #   x64 tree   (+ generators/, Conan's output)
    ├── proxy/              #   Win32 tree (+ generators/)
    └── install/
        ├── bin/            #   what the user runs
        └── game/           #   what the user copies into the game folder
```

No build folder lives in a subfolder of a half. `build/` is the only one, and
`build.py --clean` deletes it whole.

## The three commands

```bat
cmake --preset windows-release
cmake --build   --preset windows-release
cmake --install build --config Release
```

`build.py` wraps exactly these. It has four flags and no more:
`--test`, `--install`, `--deploy`, `--clean`.

## How the root drives the two architectures

`ExternalProject_Add` per half, in the root `CMakeLists.txt`:

- `BINARY_DIR` is `build/<half>` — one subtree per architecture.
- `CONFIGURE_COMMAND` runs **Conan first** (it writes the toolchain the preset
  points at), then `cmake --preset <half preset>`. Going through the half's own
  preset means the preset a human types and the configure the root performs are
  the same one and cannot drift.
- `BUILD_COMMAND` is `cmake --build <BINARY_DIR> --config Release`, with
  `BUILD_ALWAYS ON`: sources change far more often than the configure does, so
  the inner build decides what is out of date. The configure step re-runs only
  on a cold tree, which is also the only time Conan runs.
- Both of those last two run through a generated wrapper script rather than
  directly — see the generator section below for why.
- `INSTALL_COMMAND ""` — the root owns the install.

The root's install is `install(CODE ...)` per half, running
`cmake --install build/<half> --config Release --prefix <prefix>`. Each half
decides what is user-facing; the root only decides where the halves meet.

The root project is `LANGUAGES NONE`, so it never looks for a compiler and has
no architecture of its own.

## Preset and Conan conventions

Both are needed, and they have to agree:

| Half | preset | `binaryDir` | `toolchainFile` |
|---|---|---|---|
| `pipeline_cpp` | `conan-windows-x64-release` | `${sourceDir}/../build/pipeline` | `${sourceDir}/../build/pipeline/generators/conan_toolchain.cmake` |
| `proxy_dll` | `conan-windows-x86-release` | `${sourceDir}/../build/proxy` | `${sourceDir}/../build/proxy/generators/conan_toolchain.cmake` |

`${sourceDir}` in a preset is the folder holding `CMakePresets.json`, i.e. the
half — hence the `../`.

Conan is invoked as

```
conan install <half> --output-folder=<game>/build/<half> --build=missing -pr=<profile> -s build_type=Release \
    -c "tools.cmake.cmaketoolchain:generator=Ninja Multi-Config"
```

The `-c` has to match the generator in the presets, or Conan writes a toolchain
for one generator while CMake configures with another.

and each half's `conanfile.py` declares

```python
def layout(self):
    cmake_layout(self, build_folder=".")
```

`build_folder="."` is the load-bearing part: it makes the output folder *be*
the binary directory, so the toolchain lands in `build/<half>/generators`,
which is where the preset looks. The default (`"build"`) would put it in
`build/<half>/build/generators` and the preset would not find it.

## The generator, and why the architecture comes from a batch file

Every preset here uses **`Ninja Multi-Config`**. Nothing generates `.sln` or
`.vcxproj` files any more; each tree gets a `build.ninja`.

Multi-**Config** specifically, not plain `Ninja`, and that is deliberate:

- Conan's `cmake_layout` only keeps the flat `generators/` folder for a
  multi-config generator. Under single-config `Ninja` it nests the output as
  `build/<half>/Release/generators`, which would break every `toolchainFile`
  path in the table above.
- `--config Release` keeps its meaning, so the three commands, the `install`,
  and `ctest --preset ... -C Release` all stay exactly as they were.

The cost is that **Ninja has no `architecture` field.** The Visual Studio
generator could be told `-A Win32`; Ninja takes its target architecture from
the compiler environment instead. So each half must configure and build inside
the vcvars environment its own Conan profile selects — x64 for the pipeline,
x86 for the proxy. Conan writes exactly that as `conanvcvars.bat`, beside the
toolchain.

A batch file cannot export its environment back into the calling process, so
the root writes two tiny wrappers per half into `build/_wrappers/` — one that
calls `conanvcvars.bat` and then configures, one that calls it and then builds
— and uses those as the `COMMAND` and `BUILD_COMMAND`.

**Consequence for anyone configuring a half by hand:** a bare
`cmake --preset conan-windows-x86-release` in a plain shell now fails with
`CMAKE_CXX_COMPILER: cl is not a full path and was not found in the PATH`.
Run `build/proxy/generators/conanvcvars.bat` first, or just use `build.py`.
The 32-bit guard in `proxy_dll/CMakeLists.txt` still catches the other failure
mode — it checks `CMAKE_SIZEOF_VOID_P`, which is generator-agnostic.

Both halves use a `conanfile.py`. A `conanfile.txt` cannot express a layout,
so a half that still has one must be converted before it can join the tree.

## Install set

`build/install` holds what the **user** runs, and nothing else.

- `bin/` — `00_run_all.exe`, `01_extract.exe`, `02_build_tsv.exe`,
  `03_find_narrative_cg.exe`
- `game/` — `winmm.dll`

Rules, both halves:

```cmake
install(TARGETS 00_run_all 01_extract 02_build_tsv 03_find_narrative_cg
        RUNTIME DESTINATION bin)          # pipeline_cpp
install(TARGETS proxy RUNTIME DESTINATION game)   # proxy_dll
```

Not installed: `snu_core` and the other internal libraries, every test binary
(`ut_*`, `*_ut`, `*_ct`, the SYSTEM driver and its gtest), and the import
library of the proxy DLL — `RUNTIME` alone excludes the `.lib`.

Not installed either: any `.bat` in the game. The survey found five, and all
five are developer-only —

| file | why not |
|---|---|
| `proxy_dll/build_and_deploy.bat` | build wrapper; it calls `build.py` by relative path, so a copy outside the source tree is broken by construction. An install tree should not contain the thing that produced it. |
| `analysys/analyze_dump.bat`, `run_text_paths.bat`, `run_verify.bat` | Ghidra headless runs against a process dump; reverse-engineering tools, and they need a Ghidra install and hard-coded local paths. |
| `tools/compile_extract_cbg.bat` | compiles a C# helper with `csc.exe`; a build step for a developer tool. |

`translations.tsv` is not installed either: it is generated by `02_build_tsv`,
not built by CMake, and does not exist on a fresh clone.

## Traps

- **A proxy DLL must not be installed next to the pipeline executables.** It
  carries the name of a system DLL (`winmm` here, `version` / `dsound` /
  `d3d9` elsewhere) and it is 32-bit while the executables are 64-bit. A
  `version.dll` sitting beside an x64 executable is a DLL the loader will pick
  up and then refuse to load. That is why the install tree is split
  `bin/` + `game/` rather than being one flat folder.
- **`cmake --preset` reads presets from the current working directory.** The
  root has to run it through `cmake -E chdir <half source dir> ...`.
- **Conan rewrites `CMakeUserPresets.json` in each half's source folder** with
  an `include` of the generated presets. After the layout changes, a stale one
  points into a tree that no longer exists and `cmake --preset` fails on a
  missing file — which reads like a broken preset rather than a stale one.
  `build.py --clean` deletes both, and Conan regenerates them.
- **ExternalProject scatters `<name>-prefix/` folders** into the parent binary
  dir unless `PREFIX`, `TMP_DIR` and `STAMP_DIR` are set. They are pointed at
  `build/<half>/_ep`, which keeps `build/` to the three folders above.
- **A relative Conan profile path resolves against the working directory**,
  and ExternalProject runs its steps from the binary dir. `profiles/x86` must
  be passed as an absolute path from the root `CMakeLists.txt`.
- **ctest cannot span the two halves**: separate CMake projects, separate
  binary dirs. `build.py --test` runs `ctest --preset` once per half.

## Rolling this out to another game

1. Add the root `CMakeLists.txt` and `CMakePresets.json`; only the half names,
   the presets and the profile path differ.
2. Point both halves' `binaryDir` / `toolchainFile` at `../build/<half>`.
3. Convert `proxy_dll/conanfile.txt` to a `conanfile.py`; give both halves
   `cmake_layout(self, build_folder=".")`.
4. Add the two `install(TARGETS ...)` rules.
5. Replace the game's `build.py` with this one; only `GAME_DIR` and the half
   names differ.
6. Update the deploy path inside the game's `00_run_all` to
   `build/proxy/Release/<dll>`.
7. Add `<Game>/build/` to the repository `.gitignore`, and delete the two old
   build folders.
8. Verify with `python build.py --test --install` from a cold `build/`.
