> **Historical:** this page describes the `iarsys.dll` attempt that `version.dll` replaced; for the strategy the code uses now, read `proxy_dll/src/dllmain.cpp`.

# Sakura no Toki — proxy iarsys.dll

Runtime translation DLL for the Artemis-engine build of Sakura no Toki
(サクラノ刻).  Replaces `iarsys.dll` in the game directory; forwards all
its exports to `iarsys_orig.dll`; loads `translations.tsv`; installs IAT
hooks on `gdi32!TextOutA` / `ExtTextOutA` of the unpacked engine module
to swap JP -> EN at draw time.

## Build

```bat
cd proxy_dll
build_and_deploy.bat
```

Requires VS 2022 Build Tools (for the MSVC compiler), CMake and ninja.  Builds **Win32 (x86)** to match
the engine's bitness.

## What it does

1. Game launches `sakuranotoki.exe` (DMM CrackProof-wrapped — 22 KB
   launcher + 5.3 MB encrypted overlay).
2. The launcher decrypts the engine in-memory; the unpacked engine
   imports `iarsys.dll`.
3. Windows loads our proxy.  `DllMain` runs:
    - Initialises the translator (loads `translations.tsv`).
    - Spawns a 1.5 s + 5 s deferred hook thread.
4. Deferred hook thread enumerates `EnumProcessModules`, IAT-patches
   `gdi32!TextOutA` and `gdi32!ExtTextOutA` in any non-system module.
5. When the engine calls those APIs, our hook runs the CP932 buffer
   through the lookup table; on hit it converts the EN UTF-8 back to
   CP932 and forwards it to GDI; on miss it passes through unchanged.

## Status — what's confirmed vs what needs RE

- ✅ PFS reader, key derivation, .ast extraction, translation pipeline.
- ✅ DLL builds & deploys; export forwarding stays valid.
- ⚠️ The hook target (TextOutA/ExtTextOutA) is a **first guess**.
  Static analysis of `sakuranotoki.exe` and `iarsys.dll` showed neither
  imports GDI text APIs, so the engine likely renders via D3D9 or via
  internal glyph rasterizers.  See "Finding the real hook" below.

## Finding the real hook target — current state

The engine has been dumped (see `analysys/sakuranotoki_unpacked.exe`,
5.3 MB) — at runtime it shows up as `sakuranotoki.log`, a CrackProof
anti-analysis trick.  Static analysis on the dump:

- **GDI/D3D imports are not statically referenced.**  Ghidra finds
  literal `"GetGlyphOutlineA"`, `"CreateFontA"`, `"D3DXCreateFontA"`,
  `"Direct3DCreate9"` strings in `.rdata`, but no PUSH / MOV
  instructions reference them.  The engine statically links D3DX9 and
  routes all drawing through `ID3DXFont::DrawText` (vtable index ~11)
  — the GDI APIs are pulled in by D3DX9 internals, not the engine
  proper.  That means the IAT-hook path in this DLL **probably will
  not fire**.
- **PFS handler is identified.**  3 sites push the `.pfs` literal
  (VAs `0x4a07d3`, `0x52a645`, `0x52a7cb`); two are inside
  `FUN_005296e0` (6.2 KB), which calls into `FUN_005fc4a0` /
  `FUN_005fc830` for keyword-table parsing.  This is the asset-config
  scanner, **not** the dialogue text walker.
- **Lua bytecode header (`\x1bLua`) appears at VA 0x638d6e** as a
  `mov dword ptr [ebp-10], 0x61754c1b` immediate — the engine
  initialises a Lua bytecode buffer in that area.  Adjacent functions
  are the script loader.
- **`block_state` / `savetitle` strings are present** at VA 0x70fa1d
  and surroundings — the AST schema we already extracted matches.

What's still missing for the DLL:
1. **The vtable hook on ID3DXFont::DrawText.**  Find the engine's
   `ID3DXFont*` instance (typically stored in a global / D3D-context
   struct), read `*(font + 0)` for the vtable pointer, replace
   `vtable[11]` (Win32 `DrawText` slot) with our trampoline.
2. **Or hook a higher-level engine function** that takes a UTF-8
   buffer and dispatches to D3D.  Candidates from the byte-pattern
   search are saved to `analysys/text_api_hits.json` for follow-up.

The proxy DLL builds, deploys (renames original `iarsys.dll` ->
`iarsys_orig.dll`, drops our DLL in place), forwards all 10 exports,
loads `translations.tsv` at startup, and writes `iarsys_proxy.log` so
runtime behaviour is observable.  The IAT hook tries `gdi32!TextOutA`
and `gdi32!ExtTextOutA` as a first guess; if the log shows
`hook: no candidate modules patched`, that confirms the engine doesn't
go through GDI directly and we need the D3D9 vtable approach above.

## File layout

```
proxy_dll/
├── CMakeLists.txt
├── build_and_deploy.bat
├── README.md            (this file)
├── src/
│   ├── iarsys.def       export forwards (Delete, GetWindowPosition, ...)
│   ├── dllmain.cpp      DllMain + deferred hook installer
│   ├── proxy.cpp        marker symbol
│   ├── iat_hook.{h,cpp} runtime IAT patcher
│   ├── translator.{h,cpp}        Win32 glue (table loader, CP932<->UTF-8)
│   ├── translator_logic.{h,cpp}  pure TSV parsing + lookup (testable)
│   └── log.{h,cpp}      stderr/file logger
└── tests/               (gtest harness for translator_logic — optional)
```

## Tests

The `translator_logic` translation core is platform-independent and has
its own gtest suite:

```bat
mkdir build_tests && cd build_tests
cmake -DBUILD_TESTING=ON ..
cmake --build . --target run_tests --config Debug
```
