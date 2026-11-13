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
01_patch.exe       deploy alpharom_helper.dll, launch.exe, and rebuilt 0script.pak / script/*.nps
02_build_pak.exe   rebuild script/*.nps & 0script.pak from translated_text.json
```

Every tool takes `--dir <kishin_hisho>` and `--help`. `01_patch.exe` additionally
takes `--game-dir <dir>` (required), and `02_build_pak.exe` takes optional
`--game-dir <dir>` and `--verify`.

## Building

```powershell
python build.py --test      # build both halves, then run the suite
python build.py --install   # stage build/install/{bin,game}
python build.py --deploy --game-dir <install>   # deploy alpharom_helper.dll, launch.exe, and rebuilt scripts to the game
python build.py --clean     # delete the build tree
```

`--deploy` writes into the NitroPlus folder under Program Files, so it needs an
elevated shell.

Then run `launch.exe` from the game folder — not `KishinHisho.exe`.
