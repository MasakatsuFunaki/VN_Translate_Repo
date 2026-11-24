# Sakura no Uta (サクラノ詩)

English translation patch for **サクラノ詩 －櫻の森の上を舞う－** (*Sakura no Uta*) by **SCA-DI / Makura**.

| | |
|---|---|
| **Engine** | BGI / Buriko General Interpreter (Ethornell) |
| **Delivery** | Deployed into `<install>`. Copies `winmm.dll`, `translations.tsv`, and `menu_patch.bin` to translate story dialogue, menus, startup splash screens, confirmation dialogs, Extra Music tracks, choice options, and Win32 window/dialog chrome at runtime (`sysgrp.arc` is backed up to `sysgrp.arc.bak` automatically on first launch). |

## Patching the Game

Close the game, then build and deploy the patch into your game folder `<install>`:

```powershell
cd SakuraNoUta
python build.py --deploy --game-dir <install>
```

Start the game as usual. On first launch, `winmm.dll` atomically backs up `sysgrp.arc` to `sysgrp.arc.bak`, applies `menu_patch.bin` (menus, `att01`/`att02` startup splash screens, and all 34 `select01-1..select17-2` choice plates), and writes `menu_patch.applied`.

## Removing the Patch

```bat
move /Y "<install>\sysgrp.arc.bak" "<install>\sysgrp.arc"
del "<install>\winmm.dll" "<install>\translations.tsv" "<install>\menu_patch.bin" "<install>\menu_patch.applied"
del "<install>\att01" "<install>\att02" "<install>\select??-?"
```

## Pipeline & Rebuilding Artifacts

```
01_extract.exe           → script_output/extracted_text.json
02_build_tsv.exe         → speaker gate, translated_text.json, and translations.tsv
03_find_narrative_cg.exe → renders & repacks translated narrative CG overlays
04_patch_menus.exe       → builds keyless XOR-delta menu_patch.bin from <install>
00_run_all.exe           → extracts, builds translations.tsv, and deploys
```

```powershell
cd SakuraNoUta
python build.py --test --install

build\install\bin\00_run_all.exe --game-dir <install>
build\install\bin\02_build_tsv.exe --rebuild-table
build\install\bin\04_patch_menus.exe --game-dir <install>
build\install\bin\03_find_narrative_cg.exe --game-dir <install> --repack
python build.py --deploy --game-dir <install>
```

### Encrypted Script Storage (`*.json.enc`)

`script_output/translated_text.json` and `script_output/translation_cache_anthropic.json` are tracked in git as `VNE2`-encrypted `.json.enc` files (`TRANSLATIONS_KEY` in CI or git-ignored `translations_key.txt` locally) and synced automatically by `python build.py`.
