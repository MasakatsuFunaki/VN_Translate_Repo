# 櫻の詩 — Route Guide

Seven endings and two side branches, in three playthroughs and three saves.

No choice in this game changes which script runs. Fourteen chapter-1 choices add to four hidden
counters, one script reads those counters once and picks the route, and three later choices swap
or open scenes inside a chapter. Menu numbers below are the engine's own `selectNN`
labels, and every option is quoted in this project's shipped English.

The playthrough order and the picks come from the published community choice flowchart. The
tables below re-check them against the compiled scripts.

## What you collect

| Ending | Script | Where |
|--------|--------|-------|
| III — Pica Pica, Toritani Makoto | `ed01` | Save 1, first option |
| III — Olympia, Misakura Rin | `ed02` | Save 1, second option |
| III — Zypressen, Hikawa Rina | `ed03` | Save 2, first option |
| III — Märchen, Kawachino Yumi | none | Save 2, second option |
| III — A Nice Derangement of Epitaphs, Natsume Shizuku | `ed04` | playthrough 3 |
| IV — What Is Mind? No Matter. What Is Matter? Never Mind. | `ed05` | chapter IV |
| V — The Happy Prince and Other Tales | `ed06` | Save 3, first option |
| V — Natsume Ai | none | Save 3, second option |
| VI — the true ending | `ed07` | chapter VI |

Märchen and the Ai branch run no `ed` script of their own. Chapters IV, V and VI are separate
entries on the title screen, and finishing chapter III does not reach them.

---

## Playthrough 1 — Makoto, then Rin

Five menus are open on a first run. All five feed `points[1]`, and only one option of each pair
writes anything.

| Menu | Script | Pick |
|------|--------|------|
| 3 | `01_fruhlingsbeginn_01` | But she's also extremely soft-hearted. |
| 4 | `01_fruhlingsbeginn_01` | Just genuine curiosity. |
| 6 | `01_fruhlingsbeginn_02` | Don't drop it. It'd be a waste. |
| 8 | `01_fruhlingsbeginn_03` | It's too funny to forget. |

**Save 1**, at menu 13 in `01_fruhlingsbeginn_05`.

| Pick | `points[1]` | Route |
|------|-------------|-------|
| In one way or another, I'm grateful too. | 3 | Pica Pica — Makoto |
| If I don't do it now, who knows what I'll hear about it later. | 2 | Olympia — Rin |

Menus 3 and 4 carry the only two points on this run, and menu 13 adds the third the selector
needs. No menu anywhere writes `points[0]`, so Rin is not a route you pick — it is what you get
when nothing wins.

Play both branches. Rina's menus stay hidden until both Pica Pica and Olympia are cleared.

---

## Playthrough 2 — Rina and Märchen

Ten menus are open now. Four of these picks add to `points[2]`, and the other six write nothing,
which holds `points[1]` at zero.

| Menu | Script | Pick | Adds |
|------|--------|------|------|
| 2 | `01_fruhlingsbeginn_01` | Close as siblings, huh...... | `points[2]` |
| 3 | `01_fruhlingsbeginn_01` | But that's Toritani for you. | — |
| 4 | `01_fruhlingsbeginn_01` | Not that I had any ulterior motive. | — |
| 6 | `01_fruhlingsbeginn_02` | Don't drop it. It'd be a waste. | — |
| 7 | `01_fruhlingsbeginn_02` | Rina... as in Hikawa... | — |
| 8 | `01_fruhlingsbeginn_03` | It's too funny to forget. | — |
| 9 | `01_fruhlingsbeginn_03` | It's true that putting Hikawa in danger is also something to think about. | `points[2]` |
| 11 | `01_fruhlingsbeginn_05` | Well, she's a first-year -- there's no need to make Hikawa do this kind of thing. | `points[2]` |
| 12 | `01_fruhlingsbeginn_05` | It's fine because I have self-control. | `points[2]` |
| 13 | `01_fruhlingsbeginn_05` | If I don't do it now, who knows what I'll hear about it later. | — |

`points[2]` reaches 4. The rule needs 3 and needs the winner to be higher than all three other
counters, so the fifth Rina option, at menu 7, is not needed.

**Save 2**, at menu 15 in `03_zypressen_01`.

| Pick | Flag | What runs |
|------|------|-----------|
| Yumi is holding herself back. | `flag[3] = 0` | `03_zypressen_04` and `03_zypressen_06a` |
| When I consider how Yumi feels... | `flag[3] = 1` | `03_marchen_master` in place of both |

Take Märchen first. Then reload Save 2 and take the other option. That branch runs
`03_zypressen_04`, `_06a` and `ed03`, and it sets `progress[2]` — the slot that opens Shizuku's
menus. The flag also changes lines inside `03_zypressen_03`, `_05` and `_06`.

---

## Playthrough 3 — Shizuku

All fourteen menus are open. Menu 14 adds 2 and the other three Shizuku picks add 1 each, so
`points[5]` reaches 5 while `points[1]` and `points[2]` stay at zero.

| Menu | Script | Pick | Adds |
|------|--------|------|------|
| 1 | `01_fruhlingsbeginn_01` | It's not a debt. | `points[5]` |
| 2 | `01_fruhlingsbeginn_01` | Even so, why the hell would you call yourself my spiritual little sister? | — |
| 3 | `01_fruhlingsbeginn_01` | But that's Toritani for you. | — |
| 4 | `01_fruhlingsbeginn_01` | Not that I had any ulterior motive. | — |
| 5 | `01_fruhlingsbeginn_02` | Being able to see Shizuku half-asleep is nice. | `points[5]` |
| 6 | `01_fruhlingsbeginn_02` | Don't drop it. It'd be a waste. | — |
| 7 | `01_fruhlingsbeginn_02` | Rina... as in Hikawa... | — |
| 8 | `01_fruhlingsbeginn_03` | It's too funny to forget. | — |
| 9 | `01_fruhlingsbeginn_03` | Putting my own life in danger over something like this is something to think about. | — |
| 10 | `01_fruhlingsbeginn_04` | I like women! | `points[5]` |
| 11 | `01_fruhlingsbeginn_05` | Honestly, if Hikawa did it too, the customers would probably be even happier. | — |
| 12 | `01_fruhlingsbeginn_05` | It's fine because I'm not imagining anything. | — |
| 13 | `01_fruhlingsbeginn_05` | If I don't do it now, who knows what I'll hear about it later. | — |
| 14 | `01_fruhlingsbeginn_06` | If Shizuku is living her life believing that, then I'm glad. | `points[5]` **+2** |

Menu 14 is the only option in the game worth more than 1. The route ends on `ed04`, and
chapter IV then appears on the title screen.

---

## Chapters IV, V and VI

Start each from the title screen, in order. Each one opens the next.

**Chapter IV** holds no choices. It ends on `ed05`.

**Chapter V** holds two. At menu 16 in `05_the_happy_prince_04`, take *Well, at your skirt
length your panties won't show, so*. It adds `points[4]`, which opens content in this script, in
`05_the_happy_prince_08b` and in the Ai scene `h_ai_01`. Neither option changes the ending.

**Save 3**, at menu 17 in `05_the_happy_prince_07`.

| Pick | Flag | What runs |
|------|------|-----------|
| I don't know. | `flag[4] = 0` | `05_the_happy_prince_08`, then `ed06`. Chapter VI opens. |
| Ah, I was wrong. | `flag[4] = 1` | `05_the_happy_prince_08b` and `09b`. The chapter stops there. |

> **WARNING:** Take *Ah, I was wrong* first. That branch reaches no ending and does not open
> chapter VI. Reload Save 3 and take *I don't know* to continue.

Neither branch of menu 17 prints a line of text, so the game gives no sign that the choice
mattered.

**Chapter VI** holds no choices. It ends on `ed07`, the true ending.

---

## How the route is picked

`01_fruhlingsbeginn_end` runs after chapter 1. It reads four counters and applies one rule.

> A counter wins if it is at least 3, and if it is higher than all three others. A tie wins
> nothing. If nothing wins, the game takes Rin.

| Counter | Chapter 2 | Chapter 3 | Heroine |
|---------|-----------|-----------|---------|
| `points[0]` | `02_abend_rn_master` | `03_olympia` | Misakura Rin |
| `points[1]` | `02_abend_pi_master` | `03_picapica` | Toritani Makoto |
| `points[2]` | `02_abend_ze_master` | `03_zypressen` | Hikawa Rina, with Kawachino Yumi |
| `points[5]` | `02_abend_an_master` | `03_andoe` | Natsume Shizuku |

## What unlocks what

Clearing a route sets a progress slot. Each menu tier tests those slots, and the tests are
joined with AND.

| Tier | Menus | Counter | Needs |
|------|-------|---------|-------|
| 1 | 3, 4, 6, 8, 13 | `points[1]` | nothing |
| 2 | 2, 7, 9, 11, 12 | `points[2]` | `progress[0]` and `progress[1]` |
| 3 | 1, 5, 10, 14 | `points[5]` | `progress[0]`, `progress[1]` and `progress[2]` |

`03_olympia_master` sets `progress[0]`, `03_picapica_master` sets `progress[1]` and
`03_zypressen_master` sets `progress[2]`. So tier 2 needs both chapter-3 routes of playthrough 1,
and tier 3 needs Zypressen on top.

The chapter chain uses the same mechanism. `03_andoe_master` sets `progress[50]`,
`mainforrouteiv` sets `progress[60]`, and `mainforroutev` sets `progress[70]` — but only on the
`ed06` branch. No script reads those three slots, so the title screen reads them from inside the
engine.

---

## Verified mechanics

`analysys/dump_choice_menus.py` produces the choice tables, `analysys/dump_route_state.py` the
slot and gate tables, and `analysys/bgi_lin.py` is the disassembler under both.

**The disassembler proves itself before anything is read off it.** Instruction lengths are a
hypothesis, so `bgi_lin.py` run directly checks three things over all 167 scripts: a linear scan
from offset 0 must land on every compiler-emitted line anchor, every pushed code address must
land on an instruction boundary, and every script must end on its terminator. It reports
**142,509 of 142,509 anchors hit, 0 off-boundary jump targets, 0 bad terminators**.

**The census needs no guess about where a choice might be.** A string push whose consuming
function id is `0x09` is a plain string. Where such a push holds a label matching `selectNN-M`,
the next push holds that option's displayed text. Scanning all 167 scripts for that pairing
yields 17 menus and 34 options. Each branch body is then read from the dispatch, which is a
chain of `if (result == N)` tests, so "no durable write" means the whole body was scanned. All
34 bodies reconverge inside their own script, and none holds a script-transfer call.

**The gate joiner `0x38` is AND.** `03_zypressen_master`, `mainforroutev` and `mainforroutevi`
each set their own marker — `progress[3]`, `progress[4]`, `progress[230]` — and then test all
three markers before setting `progress[80]`. Under OR each of those tests is true the moment its
own marker is set, so all three sites would write `progress[80]` unconditionally and the other
two markers would carry no meaning. `dump_route_state.py --gates` shows that every menu gate
uses that same joiner.

**The threshold is 3, and the winner must be strictly highest.** `01_fruhlingsbeginn_end` loads
`points[0,1,2,5]` into a four-element local, tests each against 3, counts how many others it
beats, and requires that count to reach 3 — which only a unique maximum can. The loop that walks
the array fixes `0x34` as *less than*. Playthrough 1 fixes the rest: three picks reach Makoto,
which a threshold of 4 would not allow.

**Chapter VI needs the `ed06` branch.** In `mainforroutev` the body for `flag[4] == 1` jumps to
the script tail rather than to shared code, so it skips both `ed06` and `progress[70] = 1`.

**The heroine names are not guesswork.** Each chapter-2 master names its chapter-3 master
outright, which fixes all four arc mappings. The ending scripts then name the heroine in their
staff-roll assets: `ED01_staff_makoto`, `ED02_staff_rin`, `ED03_staff_rinayumi`,
`ED04_staff_sizuku`. `ed07` is the true ending because every ending from `ed01` to `ed06` closes
on the shared `ED_last_logo1` / `ED_last_logo2` pair, while `ed07` closes on `ED_last_logo_F`
and is the only script carrying `ED07_Fin` and the title logo `logo_sakuuta1` / `logo_sakuuta2`.

## Known limits

- **The counters are assumed to survive a save and a reload.** All three save points rest on
  that. The counters demonstrably survive a script change, because `01_fruhlingsbeginn_end`
  reads values accumulated over six separate chapter-1 scripts. A save and a reload would need
  the `0xE0` handler disassembled out of `BGI.exe`. That this walkthrough works in play is the
  evidence for it.
- **`0xF4` is not decoded.** It appears only in the five scripts that dispatch other scripts.
  Until it is, whether the Märchen branch also reaches `ed03` stays open. The instruction is the
  same under either reading: reload Save 2 and play the other branch.
- **`0xF0` is treated as a call that returns.** The masters list up to 29 dispatches in
  sequence, which only works if control comes back, but the return was not decoded.
- **The H-scene entry points are not statically encoded.** `setupforreplay` holds them as a
  table of 14 scene and thumbnail pairs, which the extras menu consumes at run time. A
  name-based graph cannot prove they are unreachable from the main story.
