# Kishin Hishou Demonbane — AlphaROM / Win11 Compatibility & Fan-Translation Spot-Check Patch

機神飛翔デモンベイン (Nitroplus, 2006) ships with the AlphaROM/SETTEC copy protection, whose runtime crashes on Windows 11, and has an English fan translation (*Double Wings Demonbane*). This project provides both the Windows 11 AlphaROM bypass launcher/VEH helper and the C++ script rebuilder (`02_build_pak.exe`) applying 957 `spot_checker` translation & box-width wrapping improvements:

| Artifact | Role |
|----------|------|
| `helper_dll/launch.exe` | 32-bit launcher: `CreateProcess` suspended, inject, resume |
| `helper_dll/alpharom_helper.dll` | VEH & IAT hooks that fix the AVs the AlphaROM stub takes on Win11 |
| `0script.pak` / `script/*.nps` | Rebuilt `.pak` v4 (`KishinHisyouDB`) & loose `.nps` scripts with 957 spot-check fixes and `<BOX type="0..13">` word-wrapping |

## Tooling

The tooling lives in `pipeline_cpp/` (Conan 2 + Boost + capstone + zlib + CMake):

```
01_patch.exe                        build helper_dll/ and deploy runtime & script artifacts
02_build_pak.exe                    rebuild script/*.nps & 0script.pak from translated_text.json
analysis_01_recon.exe               PE layout, imports, TLS, entry-point disasm
analysis_02_decrypt_devnames.exe    decrypt the anti-debug device-name table
analysis_03_parse_dump.exe          exception record + registers + modules
analysis_04_extract_thunk.exe       disassemble the stack thunk EIP landed in
analysis_05_trace_stage3.exe        follow stage-2's jump into stage 3
analysis_06_parse_recognizedisc.exe export table of the dumped recognizedisc.dll
```

Every tool takes `--dir <kishin_hisho>` and `--help`. `01_patch.exe` additionally
takes `--game-dir` (required), `--clean`, and `--mode auto|dev|dist`.

## Building

```powershell
python build.py --test      # build both halves, then run the suite
python build.py --install   # stage build/install/{bin,game}
python build.py --deploy    # copy alpharom_helper.dll + launch.exe to the game
python build.py --clean     # delete the build tree
```

`--deploy` writes into the NitroPlus folder under Program Files, so it needs an
elevated shell.

Then run `launch.exe` from the game folder — not `KishinHisho.exe`.
