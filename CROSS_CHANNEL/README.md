# CROSS†CHANNEL -FINAL COMPLETE-

| | |
|---|---|
| **Engine** | WillPlus/AdvHD (`cc.exe`, 5pb. `ph` port) |
| **Format** | `sn.bin` — LZSS compressed bytecode (CP932); `data\sys.cpk` — Criware CPK (`ITOC` + LZSS `.pak` + `xtx\0` textures) |
| **JP Strings** | 53,635 (28,787 dialogue + 24,848 narration; 43,181 unique translatable) |
| **Speakers** | 75 (太一, 見里, 美希, 霧, 冬子, 友貴, 桜庭, 曜子, 七香 …) |
| **Model** | `gemini-4-argon` (`thinkingBudget: 2048`, `BLOCK_NONE`, 60-line rolling + 20-line lookahead context) |
| **Encrypted State** | `script_output/translation_cache_gemini.json.enc` + `script_output/translated_text.json.enc` |
| **Delivery** | `xinput1_3.dll` + `translations.tsv` + `menu_patch.bin` |

## Runtime Hooks & UI Localization

- **Script & Choices (`sn.bin`)**: `xinput1_3.dll` hooks LZSS decompression (`0x45910`), dialogue/narration renderers (`0x2A7A0`, `0x2BCC0`), choice handlers (`0x260A0`, `0x26500`, `0x26AC0`), and silent-backlog `op 0x86` (`0x2EFB0`), plus seeds `script[0]` launcher menus, `CASE` cards, and stripped-prefix lines. `sn.bin` on disk is never modified.
- **Win32 & Engine UI (`cc.exe`)**: Detours `user32.dll` `MessageBox*` / `DialogBox*` / `CreateDialog*` with thread-scoped `WH_CALLWNDPROCRET` (`Segoe UI` 9pt), redirects `.data` pointer tables (20 Config tooltips, 37 Scene/Diary titles, Playing Log stats, Extra Stories, Save/Load prompts), patches short `.rdata` HUD format slots, and detours modal renderer `0x1F5B0` to center English prompts and `Yes / No` inside native hit-rects.
- **Menu & Options Graphics (`menu_patch.bin`)**: Keyless XOR-delta `CCMP` v1 patch localizing all 115 Japanese `xtx\0` sub-textures across `data\sys.cpk` (Title, Pause, Config, Save/Load, Backlog, Album, Music, Playing Log, Extra Story). Atomically backed up to `data\sys.cpk.bak` and auto-patched on first launch (`data\sys.cpk.applied` fast-path on subsequent launches).

## Pipeline

```
01_extract           → extracted_text.json
02_translate         → speaker gate → gemini-4-argon batches → translation_cache_gemini.json + translated_text.json + translations.tsv
02_build_tsv         → speaker gate → offline cache apply → translated_text.json + translations.tsv
00_run_all           → 01_extract → 02_build_tsv → deploy
03_find_narrative_cg → CG/UI images with JP text → narrative_patched/ (inspection-only)
```

`python build.py --deploy --game-dir <game_dir>` syncs `*.json.enc` and copies `xinput1_3.dll` + `translations.tsv` + `menu_patch.bin` into `<game_dir>`.

## Build & Translate

```bat
python build.py                                 # both halves (pipeline x64, proxy Win32)
python build.py --test --install                # build + tests + stage into build/install
python build.py --deploy --game-dir <game_dir>  # ship to game
python build.py --clean

build\install\bin\02_translate.exe --gate-only
build\install\bin\02_translate.exe --max-batches 1 --batch 120
build\install\bin\02_translate.exe --game-dir <game_dir>
build\install\bin\02_build_tsv.exe --game-dir <game_dir> --rebuild-table
build\install\bin\00_run_all.exe --game-dir <game_dir>
```

Every app accepts `--dir <repo_root>\CROSS_CHANNEL` and `--game-dir <game_dir>`.
`02_translate.exe` reads `GEMINI_API_KEY` from the environment, `%USERPROFILE%\.gemini\.env`, or `HKCU\Environment`, checkpoints `script_output\translation_cache_gemini.json` and `script_output\last_gemini_translate\last_translation.json` atomically after each batch, isolates any single persistently refused line in `script_output\blocked_lines.json` without poisoning the cache, and supports `--model`, `--batch`, `--context`, `--context-after`, `--thinking-budget`, `--max-batches`, `--max-budget-eur`, `--gate-only`, `--retranslate`, `--discard-cache`, and `--out`.
