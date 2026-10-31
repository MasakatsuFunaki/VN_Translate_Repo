# Post-Mortem Analysis: `SakuraNoUta` Positioned Message (`yuzu_2g` / `SET_CURSOR`) Text Rendering Bug

## Executive Summary

All three broken scenes in `SakuraNoUta` occur in `03_zypressen_02` (`data01099.arc`), which renders poetic/staggered prose through 18 positioned-message helper subroutines in `yuzu_2g` (`data01999.arc`: `iMessageLeft*`, `iMessageCenter*`, `iMessageRight*`, `MessageLeft*`, `MessageCenter*`, `MessageRight*`) invoked via BGI opcode `0x1C` (`call_func` by name).

These helpers measure the **unsubstituted Japanese byte length** via `STRLEN (0x94)`, compute horizontal column offsets and stock `42 px` vertical rows, and position the text pen via `SET_CURSOR (0x146)` before invoking `TEXT (0x140)`. Because our runtime hook substituted longer English strings and a `30 px` line pitch without normalizing `SET_CURSOR` coordinates (`p4 = int cursor_xy[2]`), suppressing the dummy `"\u3000"` (`\x81\x40`) box initializer, or stripping the resulting `\x05` hanging-indent prefix, the engine's pixel-based auto-wrapper and our character-based word wrapper collided—producing mid-box indentation, double line breaks, overlapping lines, and right/bottom clipping.

---

## 1. Exact Script Identification (`03_zypressen_02` in `data01099.arc`)

| Screenshot | Script (`data01099.arc`) | String Index | `yuzu_2g` Helper (`op1C`) | Script Args (`y_row`, `ruby_bytes`) | Japanese Source | English Translation |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1 (Line 1)** | `03_zypressen_02` | `[146]` | `iMessageLeftIg` | `y=0, ruby=0` | `ガゼルのその愛くるしい瞳も、その美しい毛並みも、` (48 B) | `The gazelle's endearing eyes, its beautiful coat--` (50 chars) |
| **1 (Line 2)** | `03_zypressen_02` | `[147]` | `MessageRightIg` | `y=1, ruby=2` | `ライオンにとってはさほど重要では無い。` (38 B) | `to the lion, none of it matters much.` (37 chars) |
| **1 (Line 3)** | `03_zypressen_02` | `[148]` | `MessageCenter` | `y=2, ruby=0` | `外見など、彼にとってはただ肉と血の詰まった袋でしかない。` (56 B) | `Looks mean nothing to him; a gazelle is only a sack stuffed with flesh and blood.` (81 chars) |
| **2 (Line 1)** | `03_zypressen_02` | `[160]` | `iMessageLeftIg` | `y=0, ruby=0` | `肉食獣が、必然の様に弱い動物の肉を喰らう様に、` (46 B) | `Just as a carnivore devours the flesh of weaker animals as if by necessity,` (75 chars) |
| **2 (Line 2)** | `03_zypressen_02` | `[161]` | `MessageRight` | `y=2, ruby=2` | `私もまた、同性を愛した。` (24 B) | `so too did I love my own sex.` (29 chars) |
| **3 (Line 1)** | `03_zypressen_02` | `[194]` | `iMessageCenter` | `y=1, ruby=0` | `里奈を見たのは、私も彼女もまだ幼い子供の時だ。` (46 B) | `It was when both she and I were still small children that I first laid eyes on Rina.` (84 chars) |

### Corpus-Wide Scope
A full bytecode scan of all winning scripts across `data01099.arc`–`data01999.arc` identified **538 `op1C` calls** to `yuzu_2g` positioned-message helpers across **13 scripts**, forming **382 message boxes** (275 single-segment boxes and 107 multi-segment boxes of 2 to 6 segments):
- `03_zypressen_02`: 511 calls (357 boxes)
- `03_zypressen_14`: 9 calls (`MessageCenter`)
- `03_zypressen_05`: 3 calls (`iMessageCenter`)
- `03_zypressen_06a`: 3 calls (`iMessageLeftIg`, `MessageCenterIg`, `MessageRight`)
- `03_zypressen_09`, `05_the_happy_prince_08`, `06_Ⅵ_05`: 2 calls each
- `03_andoe_04`, `05_the_happy_prince_05`, `06_Ⅵ_01`, `06_Ⅵ_03`, `06_Ⅵ_06`, `06_Ⅵ_08`: 1 call each

---

## 2. Engine Architecture & The Five Interacting Root Causes

### Root Cause 1: Dummy `"\u3000"` (`\x81\x40`) Initializer in `iMessage*` & Forced `\x05` Prefix
Every `iMessage*` subroutine in `yuzu_2g` begins by clearing the message box with a dummy fullwidth space:
```text
op140(mode=0, a1=0, a2=0, speaker=arg2, text="　")  // CP932: \x81\x40
```
1. Because `"\x81\x40"` had no entry in `translations.tsv`, `BuildSubstitution` returned `NotFound`, causing `FUN_007c3900` to render a `28 px` fullwidth space at `(48, 24)` and charge `BoxLineBudget` **1 used line** (`stays_open = false`) before the actual sentence even arrived.
2. Worse, `scrmsg._bp` (`0x0853..0x093b`) inspects `segments[0].text`. When `segments[0].text` starts with `"\x81\x40"`, `scrmsg._bp` prepends control byte **`\x05`** to every subsequent segment (`segments[1..7]`) in that box. In `FUN_007c3900`, `\x05` activates a `28 px` (2-column) hanging indent on wrapped lines (`x = 76` instead of `x = 48`) and narrows `WrapForMessageBox` from `71` columns to `69` columns.

### Root Cause 2: `SET_CURSOR (0x146)` Computes Coordinates from Japanese Byte Length & Stock `42 px` Pitch
Before calling `op140(mode=1, ...)`, `yuzu_2g` calls `op94` (`STRLEN`) on the **unsubstituted Japanese string** (`L` bytes, minus `ruby_bytes`) and computes character cell coordinates `(row_y, col_x)`:
- **`*Left*`**: `col_x = 0` -> `cursor_x = 48 + 0 * 28 = 48`
- **`*Center*`**: `col_x = (76 - (L - ruby)) / 4 = 19 - (L - ruby) / 4` -> `cursor_x = 48 + col_x * 28`
- **`*Right*`**: `col_x = (72 - (L - ruby)) / 2 = 36 - (L - ruby) / 2` -> `cursor_x = 48 + col_x * 28`

Then `scrmsg._bp` (`0x0767..0x0790`) converts `(row_y, col_x)` to pixel coordinates using the **stock `42 px` vertical pitch**:
```text
cursor_x = 48 + col_x * 28
cursor_y = 24 + row_y * 42
```
Because `HookedTextDisplay` substituted `p6` (`line_pitch`) from `42` to `30` without normalizing `p4` (`int cursor_xy[2]`), segments placed on `row_y = 1` (`y = 66`) and `row_y = 2` (`y = 108`) were misaligned with the `30 px` row grid (`y = 24, 54, 84, 114`).

### Root Cause 3: Double Line-Breaking (`cursor_x > 48` + Engine Auto-Wrap + `WrapForMessageBox` `\n`)
`FUN_007c3900` (`RVA 0x033900`) takes `p4 = int cursor_xy[2]` (`[ebp+0x14]`) and `p5 = const int box_rect[4]` (`[ebp+0x18]`, `{48, 24, 1083, 151}`). While rendering glyphs (`14 px` per halfwidth char), `FUN_007c3900` checks if `cursor_x + glyph_width > box_right_x` (`1083`):
- **Screenshot 1 (`[147]` at `cursor_xy = {552, 66}`)**: Only `(1083 - 552) / 14 = 37.9` columns fit before `box_right_x = 1083`, and kinsoku lookahead on `"much."` triggers an engine auto-wrap after `"matters"`, dropping `"much."` onto `y = 66 + 30 = 96` (`x = 76`). Then `[148]` starts at `cursor_xy = {188, 108}`—vertically overlapping `"much."` (`y = 96..121` vs `y = 108..133`), auto-wrapping `"with flesh"` to `y = 138`, and then hitting `WrapForMessageBox`'s injected `\n` after `"flesh"`, which pushes `"and blood."` to `y = 168 > box_bottom_y (151)`, where `FUN_007c3900` clips it completely.
- **Screenshot 2 (`[161]` at `cursor_xy = {748, 108}`)**: Only `23` chars fit between `x = 748` and `box_right_x = 1083`. After `"so too did I love my "` (21 chars), `"own sex."` wraps to `y = 108 + 30 = 138`, where `138 + 28 = 166 > 151`, clipping `"own sex."` completely.
- **Screenshot 3 (`[194]` at `cursor_xy = {272, 66}`)**: Starting at `x = 272, y = 66`, line 1 auto-wraps after `"children"` (`55` chars) to `y = 96`, then hits `WrapForMessageBox`'s `\n` after `"laid"` to jump a second time to `y = 126`, where `126 + 28 = 154 > 151` clips the bottom half of `"laid eyes on Rina."`.

### Root Cause 4: Unchained Multi-Segment `yuzu_2g` Boxes in `BoxLineBudget` & `01_extract`
In normal BGI scripts, multi-segment boxes chain via a trailing `0x0A` (`\n`) on each non-final segment. In `yuzu_2g`, `*Ig` helpers (`iMessageLeftIg`, `MessageRightIg`, etc.) omit the trailing `0x0A` and rely on `SET_CURSOR` instead. Consequently, `BuildSubstitution` saw `out.tail == 0` (`out.more_follows == false`) and closed `BoxLineBudget` after every segment, resetting `used_lines` to `0` on the next segment. Meanwhile, `01_extract` only checked for `op == 0x140` (`TEXT_FCN`) and classified all 538 `op1C` `yuzu_2g` calls as `type = "other"`.

### Root Cause 5: Backlog (`logwnd._bp`) Concatenation Misses
When the player opens the Backlog (`HISTORY_WINDOW`), `logwnd._bp` (`0x1157..0x1223`) concatenates all `seg[0..k]` strings of a message box directly without delimiters (`"\x81\x40" + jp_seg1` for single-segment `iMessage*` boxes, and `"\x81\x40" + jp_seg1 + jp_seg2 + ...` for multi-segment `yuzu_2g` boxes) and calls `HookedTextDisplay` once. Neither form matched `translations.tsv`, leaving `yuzu_2g` lines untranslated in the backlog.

### Root Cause 6: `*NC` ("No Clear") Continuation Overwriting the Open Message Box
In `03_zypressen_02` (*"Premonition II"*), `yuzu_2g` uses `*NC` ("No Clear") helpers (`iMessageLeftNC`, `iMessageCenterNC`, `iMessageRightNC`, `MessageLeftNC`, `MessageCenterNC`, `MessageRightNC`), which call `op140(clear_after_click = 0, wait_for_click = 1)` followed by `op142`. When the player clicks after a `*NC` call, the engine emits a standalone `"\n"` (`0x0A`) on `p4 = 0xEFE958` (`surface.flows == true`) without clearing the visible dialogue panel, and the immediately following `iMessage*` helper emits its dummy `"\x81\x40"` on `0xEFED00` with `cursor_xy` still below `top_y` (e.g. `(48, 54)` or `(48, 84)`). Unconditionally resetting `BoxLineBudget` to `lines = 0` and `(48, 24)` on `"\x81\x40"` caused the post-`*NC` sentence to draw at `(48, 24)` directly on top of the still-visible `*NC` sentence.

---

## 3. Solution Implemented

### A. Runtime Fix in `SakuraNoUta/proxy_dll/src/`
1. **`proxy_dll/src/text_display_abi.h`**:
   Documented the verified ABI types of `p4` (`int cursor_xy[2]: { cursor_x, cursor_y }`), `p5` (`const int box_rect[4]: { left, top, right, bottom }`), and `p6` (`int line_pitch`) while preserving the C signature for full compatibility with `text_display_abi_ut.cpp`.
2. **`proxy_dll/src/translator_logic.h` & `proxy_dll/src/translator_logic.cpp`**:
   - **Dummy `"\x81\x40"` Suppression, Positioned Box Opening & `*NC` Continuation**: When `wire_text == "\x81\x40"` arrives on `DIALOGUE_PANEL` (`surface.flows`), `BoxLineBudget::BeginPositionedBox` checks whether `cursor_xy` is at `{left_x, top_y}` (`{48, 24}`). For a fresh cleared box (`cursor_xy == {48, 24}` or after a 1-line Save/Load preview without a subsequent `"\n"` no-clear click), it opens a fresh positioned box (`lines = 0`). When `cursor_xy != {left_x, top_y}` on an already-open positioned box (`b.positioned && b.open && b.state.lines > 0 && !saw_save_load_preview`), it preserves `b.state` across the `*NC` click so the following `iMessage*` segment continues the open paragraph instead of overwriting `(48, 24)`.
   - **Prefix Stripping, Cursor Normalization & Continuous Paragraph Flow**: For positioned segments (both `iMessage*` chains and standalone `SET_CURSOR` calls), `Build` strips the bogus `\x05` prefix (restoring all `71` columns and `x = 48` alignment). Opening segments normalize `cursor_xy` to `{left_x, top_y}` (`{48, 24}`), while continuation segments in multi-segment `yuzu_2g` boxes (including both `*Ig` and `*NC` chains) join the open line at `cursor_xy = {left_x + (column + 1) * 14, top_y + (used_lines - 1) * 30}` (or start at `{left_x, top_y + used_lines * 30}` when the open line cannot fit the next word), flowing the sentence as a continuous paragraph with zero mid-sentence breaks and zero truncations across all 378 `yuzu_2g` boxes (including all 33 `*NC` screen-boxes).
   - **Window Redraw (`msgwndctrl._bp`) Support**: `BoxLineBudget::BeginSegment` tracks whether the positioned box is in its initial `scrmsg._bp` pass (`\x05`-prefixed) or a `msgwndctrl._bp` window redraw pass (unprefixed with advancing `SET_CURSOR` coordinates), reproducing the exact same layout on window hide/show and alt-tab.
   - **Backlog (`HISTORY_WINDOW`) Lookup Fallbacks**: `LookupTranslation` strips leading `"\x81\x40"` on single-segment `iMessage*` backlog calls and performs greedy CP932 prefix decomposition across `translations` for multi-segment concatenated backlog strings.
3. **`proxy_dll/src/translator.cpp`**:
   Passes `cursor_xy` (`reinterpret_cast<int*>(p4)`), `box_rect` (`reinterpret_cast<const int*>(p5)`), and `DIALOGUE_LINE_PITCH` (`30`) into `BuildSubstitution` when rendering on the dialogue panel (`pitch_in == DIALOGUE_LINE_PITCH_STOCK`), and logs any cursor coordinate normalization.

### B. Extractor Fix in `SakuraNoUta/pipeline_cpp/src/`
- **`pipeline_cpp/src/extract/extractor.cpp`**: Added recognition of `op1C` (`CALL_FCN`) calls to all 18 `yuzu_2g` `iMessage*` / `Message*` helpers (`is_yuzu_message_helper`), classifying their text entries as `"dialogue"` / `"narrative"` (with speaker resolution) instead of `"other"`, while keeping extracted string indices 100% invariant.

---

## 4. Regression Test Suite

- **Proxy DLL Unit Tests (`proxy_dll/tests/UT/substitution_ut.cpp`)**:
  - `PositionedBox.DummyFullwidthSpaceOpensPositionedBoxAndDrawsNothing`
  - `PositionedBox.Screenshot1ThreeSegmentPositionedBoxLayout`
  - `PositionedBox.Screenshot2TwoSegmentPositionedBoxLayout`
  - `PositionedBox.Screenshot3SingleSegmentCenteredBoxLayout`
  - `PositionedBox.WindowRedrawWithoutControlPrefixReproducesLayout`
  - `PositionedBox.SameRowAndFiveSegmentBoxesJoinOpenLineWithoutDropping`
  - `PositionedBox.PremonitionScreenshotsFlowAsContinuousParagraphs`
  - `PositionedBox.LongThreeSegmentBoxesFlowWithoutTruncation`
  - `PositionedBox.BacklogHistoryStripsLeadingFullwidthSpaceAndJoinsSegments`
  - `PositionedBox.NoClearContinuationDoesNotOverwritePreviousSentences`
  - `PositionedBox.SaveLoadPreviewResetsMidNoClearBoxWhenNoNewlineFollows`
- **Pipeline Extractor Unit Tests (`pipeline_cpp/tests/ut_extract.cpp`)**:
  - `Extract.walker_classifies_yuzu_2g_positioned_message_helpers`
