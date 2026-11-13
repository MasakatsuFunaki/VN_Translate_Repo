# 俺たちに翼はない Anniversary Edition (`Oretachi_ni_Tsubasa_wa_Nai` / *We Without Wings*)

| Property | Value |
|---|---|
| **Developer** | Navel (Scenario: Ou Jackson, Art: Naru Nanao) |
| **Engine** | Lucifen Library & Tools Version 1.2.2 (`800x600`, 32-bit x86) |
| **Archive Format** | `LPK1` (`SCRIPT.LPK`: `418` entries — `411` `SOB0`, `1` `SOB1`, `1` `SOB2`, `5` `.dat`/`.inf`/`.sys`) |
| **Extracted Corpus** | `58,851` total entries (`57,865` messages across `387` scenario scripts, `54` choices across `24` choice blocks, `388` scene titles, `388` scene skip synopses, `156` UI strings) |
| **Unique Speakers** | `324` (`鷹志`, `隼人`, `鷲介`, `日和子`, `鳴`, `明日香`, `京`, `小鳩`, `亜衣`, `狩男`, `英里子`, `翔`, `紀奈子`, `和馬`, `春恵`, `ガルーダ`, `蔵人`, `咲夜`, `優`, `聖騎士ホーク` …) |
| **Translator** | `gemini-4-argon` (`pipeline_cpp/src/translate/`, cached in `script_output/translation_cache_gemini.json`) |
| **Runtime Delivery** | `winmm.dll` proxy + `build/install/game/translation_table.tsv` + unwrapped inner Lucifen PE (`ORE_TUBA.exe`, preserving `ORE_TUBA_orig.exe`). `.LPK` archives are never modified. |

---

## Pipeline

```text
01_extract.exe     -> script_output/extracted_text.json
02_translate.exe   -> speaker gate -> script_output/translation_cache_gemini.json + script_output/translated_text.json
03_build_tsv.exe   -> build/install/game/translation_table.tsv
00_run_all.exe     -> runs 01..03, unwraps ORE_TUBA.exe (backing up ORE_TUBA_orig.exe), and deploys winmm.dll + translation_table.tsv
```

## Build (`build.py`)

`ORE_TUBA.exe` is a 32-bit x86 executable, while the offline extraction and translation pipeline runs natively on 64-bit Windows:
- `pipeline_cpp/` is **x64** (`conan-windows-x64-release`).
- `proxy_dll/` is **Win32** (`conan-windows-x86-release`).

Both halves are driven into a single `build/` tree by the root `CMakeLists.txt` and `build.py`:

```bat
python build.py                              # clean build/ and build both halves (tests included)
python build.py --test --install             # build, run all GoogleTest suites, and stage into build/install/
python build.py --deploy --game-dir "<DIR>"  # copy winmm.dll + translation_table.tsv into the game folder
python build.py --clean                      # remove build/ and stale CMakeUserPresets.json files
```

`00_run_all.exe` and `01_extract.exe` take `--dir <project>` and `--game-dir <install>`; `02_translate.exe` and `03_build_tsv.exe` take `--dir <project>`.

---

## Engine Formats, Hook Sites & Traps

### 1. SoftDenchi / DMM Outer Wrapper vs. Inner Lucifen PE (`ORE_TUBA.exe`)
- The shipped `ORE_TUBA.exe` is a SoftDenchi/DMM wrapper containing the unencrypted 32-bit Lucifen engine PE (`1,101,824` bytes / `0x10D000` bytes) embedded verbatim at file offset `0x031BC8..0x13EBC8`.
- When the outer wrapper runs, it extracts that inner PE to `%TEMP%\_ORE_TUBA.exe` and spawns it from `%TEMP%`, which causes Windows to search `%TEMP%` instead of the game folder for `winmm.dll`.
- Backing up `ORE_TUBA.exe` to `ORE_TUBA_orig.exe` and writing the `1,101,824`-byte inner PE to `ORE_TUBA.exe` runs the Lucifen engine directly in-place and loads `winmm.dll` from the game directory.

### 2. `LPK1` Archive Format (`SCRIPT.LPK`)
- **Stem Key Derivation**: Uppercases the archive basename stem (`"SCRIPT"`), starts with `kInitKey0 = 0xA5B9AC6Bu` and `kInitKey1 = 0x9A639DE5u`, and for each byte XORs the reverse byte into the low byte of `key0` (`ror32(key0, 7)`) and the forward byte into the low byte of `key1` (`rol32(key1, 7)`).
- **Index Table Decryption**: Rolling XOR over 32-bit words seeded with `eax = hdr_key` and `ecx = 0x31746285u` (`ecx = rol32(ecx, 4)`, `word ^= eax`, `eax = ror32(eax, ecx & 0xFF)`).
- **Prefix Trie & Shared Header Prefix**: `SCRIPT.LPK` stores a 4-byte shared prefix (`prefix_len = 4`, `"SOB0"`) only in the archive index for `SOB0` files (`eflags == 0`) or nibble-swaps/XORs the body when `eflags & 1`. Compressed entries use a 4096-byte ring-buffer LZSS decompressor (`ring_pos = 0xFEE`) followed by payload decryption on the first `min(len >> 2, 0x40)` dwords.

### 3. `SOB0` / `SOB2` / `SOB1` Bytecode Format
- **`SOB0` (`411` scenario/system scripts) & `SOB2` (`func_lib.sob`)**:
  - `magic` (`"SOB0"` or `"SOB2"`), `uint32_t reloc_size`, followed by `5` relocation sub-tables (`0: GLOBAL_VAR`, `1: SYSCALL`, `2: LOCAL_STACK`, `3: EXT_FUNC`, `4: SELF_OFFSET`) and a `uint32_t` zero terminator at `8 + reloc_size - 4`.
  - Followed by `uint32_t code_size`, `uint32_t total_size`, and the payload buffer (`code` at `[0..code_size)`, `data/strings` at `[code_size..total_size)`).
  - **Trap — Reverse String Pool Emission**: The Lucifen `.sob` compiler emits the string pool from the end of the data section backwards (`rec_off` decreases as `msg_idx` increases). Always walk `Sub-table 1` (`SYSCALL`) in ascending `code_offset` order so extracted lines follow chronological playback order (`msg_idx = 0, 1, 2, ...`).
  - **Translatable Records**:
    - `SYSCALL[462]` (`0x1CE`): Message records (`uint8_t block_id`, `uint16_t msg_index`, NUL-terminated CP932 string `#Speaker\VoiceTag$&$Body$` or `Narration$`).
    - `SYSCALL[491]` (`0x1EB`): Choice blocks (`uint8_t block_id`, `uint16_t choice_id`, `uint8_t num_options`, `uint8_t has_opt`, followed by `num_options` records of `uint8_t stride`, `uint8_t flag`, and NUL-terminated CP932 option string).
    - `EXT_FUNC[0x1C8D0]`: Scene titles (`388` records).
    - `GLOBAL_VAR[0x35D]`: Scene skip synopses (`388` records).
    - Unclaimed `SELF_OFFSET` (`Sub-table 4`) data pointers with valid CP932 Japanese strings: system/UI messages (`146` records).
- **`SOB1` (`user_var.sob`)**:
  - `magic` (`"SOB1"`), `uint32_t data_size` (`6,424` bytes), `data_buf[data_size]`, `uint32_t var_count` (`916`), `var_count * 10`-byte variable descriptors, `uint32_t reloc_count` (`18`), `reloc_count` pairs of `(ptr_offset, target_offset)` into `data_buf` (`10` Japanese right-click menu strings + `8` ASCII config strings), and a `uint32_t` zero terminator.

### 4. Runtime Hook Sites & Traps (`proxy_dll/`)
- **`0x00455ab0` (`CScriptMessageManager::ParseMessageHeaderAndBody`, `__thiscall`)**:
  - **Trap 1 (In-Place `'$' -> '\0'` Mutation at `0x00455b49`)**: The engine overwrites the first `'$'` delimiter in the input string with `'\0'` in-place to terminate the `#Header` substring. Never pass a pointer directly into `.SOB` memory or an immutable `TranslationCatalog` string; always copy the reconstructed `#TranslatedSpeaker\VoiceTag$&$WrappedEnglishBody$` into a mutable scratch buffer first.
  - **Trap 2 (Voice Playback Tag in `#Speaker\VoiceTag$&`)**: Splitting `#Header` on `'\\'` yields `field[0] = Speaker` and `field[1] = VoiceTag` (e.g. `Y101038`, `KAZ1079`). Translate **only** `field[0]` and preserve `\VoiceTag` verbatim so character voice audio plays normally.
  - Because `0x00455ab0` executes **before** `PushToBacklog` (`0x004555a0`), hooking `0x00455ab0` translates the live message window, speaker nameplate, backlog history, and save-slot comments in one place.
- **`0x00455f60` (`ParseChoicesFromScript`) & `0x00456110` (`AppendChoiceString`)**: Translates interactive choice options.
- **`0x00424d30` (`SOB` Loader `Sub-table 4` Relocation)**: Substitutes translated scene titles, scene skip synopses, and system UI strings.
- **`0x00458cd0` (`CMessageWindow::Init`) & `0x004590d0` (`CMessageWindow::DrawCharInternal`)**: Sets `byte [msg_wnd + 0x25c] = 1` (`m_bProportional = 1`), enabling the engine's built-in `GetTextExtentPoint32A` proportional glyph advance instead of the fixed 24px fullwidth grid.
- **`0x0044e5a0` (`CFontGGO::SelectFont` / `CreateFontA` at `0x0044e615`)**: Substitutes a clean proportional font face while keeping `SHIFTJIS_CHARSET` (`0x80`).
