# 神学校 ~Noli me tangere~ — Route Choice Tables

---

## レオニード — Leonid (Leo) route → GOOD END

### Common route (共通)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 1 | **Looked at the altar.** | — |
| 2 | **closed my mouth.** | — |
| 3 | **I decided I really couldn't tell him after all.** | — |
| 4 | **I thought I should run.** | — |
| 5 | **First, let's check out the cemetery.** | — |
| 6 | **I'll mark Eligos.** | ⚠️ "I'll mark Lucifer" = **BAD** |

### Leonid route (固有)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 7 | **Look for Leonid.** | 🔑 locks in Leonid |
| 8 | **Let's go see Leonid** | — |
| 9 | **I need to go to Leonid's room.** | — |
| 10 | **Shook my head.** | — |
| 11 | **Decided to try going up to the floor above.** | — |
| 12 | **I'll mark Lucifer.** | here Lucifer is correct |
| 13 | **Let me deliberately make a prophecy that could come true.** | ⚠️ "Let's avoid the danger." → BAD branch |
| 14 | **I think I'll go with Lucifer after all.** | ⚠️ "Or maybe I should mark all the rest of them." = **BAD** |
| 15 | *H-scene position (either):* **Get him back** = Michael×Leonid · **Leave it alone** = Leonid×Michael | not good/bad — both continue the route, only the scene differs |
| 16 | ✅ **I'll trust Chris after all.** | **GOOD END**. ⚠️ "It can't be helped. I have to go." = **BAD END** |

**Notes**
- main06's first menu ("Take Father August's advice" / "Gabby was right") is not route-critical — pick either.
- Demon-marking "correct" target changes per scene: **Eligos** at #6, but **Lucifer** at #12 and #14.

---

## セシル — Cecil route → GOOD END

### Common route (共通)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 1 | **Looked behind me.** | the **1st** of the five options (differs from Leonid, who picks "altar") |
| 2 | **closed my mouth.** | — |
| 3 | **I made up my mind and decided to tell Cecil everything.** | (differs from Leonid) |
| 4 | **I thought I should run.** | — |
| 5 | **First, let's check out the cemetery.** | — |
| 6 | **I'll mark Eligos.** | ⚠️ "I'll mark Lucifer" = **BAD** |

### Cecil route (固有)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 7 | **Look for Cecil.** | 🔑 locks in Cecil |
| 8 | **Let's go find Cecil** | — |
| 9 | **I wonder where Cecil is.** | — |
| 10 | **Nodded.** | (Leonid picks "Shook my head") |
| 11 | **I'll mark Lucifer.** | here Lucifer is correct |
| 12 | **Accept it.** | ⚠️ "No, I can't, not something like this." = **BAD** |
| 13 | *H-scene position (either):* **I reached my hand toward Cecil's lower belly.** = Michael×Cecil · **I simply let out a sigh.** = Cecil×Michael | not good/bad — both continue the route, only the scene differs |
| 14 | **Let me deliberately make a prophecy that could come true.** | ⚠️ "Let's avoid the danger." → BAD branch |
| 15 | **I think I'll go with Lucifer after all.** | ⚠️ "Or maybe I should mark all the rest of them." = **BAD** |
| 16 | ✅ **I just glared at him.** | **GOOD END**. ⚠️ "I took that hand." = **BAD END** |

---

## ニール — Neil route → GOOD END

### Common route (共通)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 1 | **Looked at the rear seats.** | (differs from Leonid "altar" / Cecil "behind me") |
| 2 | **After all, I wouldn't be satisfied until I'd said it all the way through.** | (Leonid & Cecil both pick "closed my mouth") |
| 3 | **I decided I really couldn't tell him after all.** | (same as Leonid) |
| 4 | **I thought I should run.** | — |
| 5 | **First, let's check out the cemetery.** | — |
| 6 | **I'll mark Eligos.** | ⚠️ marking **Astaroth** — or **Lucifer** — triggers a **delayed BAD END** (ed_main_bad3, at main09_4) |

### Neil route (固有)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 7 | **Look for Neil.** | 🔑 locks in Neil |
| 8 | **Let's go to the library after all** | (Leonid picks "go see Leonid") |
| 9 | **Maybe I'll head back to the dorm and relax.** | — |
| 10 | **Shook my head.** | (same as Leonid) |
| 11 | **I thought about heading to the library.** | (Leonid goes "up to the floor above") |
| 12 | **I'll mark Furcas.** | here Furcas is correct (Leonid marks Lucifer here) |
| 13 | **Let me deliberately make a prophecy that could come true.** | ⚠️ "Let's avoid the danger." → BAD branch |
| 14 | **I think I'll go with Lucifer after all.** | ⚠️ "Or maybe I should mark all the rest of them." = **BAD** |
| 15 | *H-scene position (either):* **I leaned against him quietly.** = Neil×Michael · **I reached for Neil's own once more.** = Michael×Neil | not good/bad — both continue the route, only the scene differs |
| 16 | **Nodded.** | ⚠️ "I shook my head firmly." = **BAD** |
| 17 | ✅ **I thought, no, I can't after all.** | **GOOD END**. ⚠️ "I decided to kill the demon." = **BAD** |

**Notes**
- main06's first menu ("Take Father August's advice" / "Gabby was right") is not route-critical — pick either.
- Demon-marking "correct" target: **Eligos** at #6 (common route), **Furcas** at #12 (Neil-specific).
- The bad mark at #6 (Astaroth) doesn't fail immediately. Neil marks Furcas at
  #12, so main04 is the route's only chance to mark Eligos, and missing it fires
  **ed_main_bad3** at the start of main09_4 — three scripts later.

---

## オーガスト — August route → GOOD END

**Unlocked by** the Cecil, Neil and Leonid GOOD ENDs. main02's 4th option is
hidden until all three clear flags are set.

```
main02 → main03 → main04 → leo01 → main05 → august01 → august02 → main06
→ august03 → main07_1 → main07_2 → main07_3 → august04 → august05 → august06
→ main08 → main09_1…4 → august07…august13 → august_ed_good
```

### Common route (共通)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 1 | **Looked at the door to the sacristy.** | the **4th** of the five options; hidden until Cecil + Neil + Leonid GOOD |
| 2 | **After all, I wouldn't be satisfied until I'd said it all the way through.** | required — main05's lake option tests this |
| 3 | **I decided I really couldn't tell him after all.** | free — the other option only adds the cecil01 scene |
| 4 | **I unsteadily stepped toward the shadow.** | required (every other route picks "I thought I should run.") |
| 5 | **First, let's head to the sacristy.** | required — the cemetery locks the lake out |
| 6 | **I'll mark Eligos.** | ⚠️ required — August and Gabriel both mark Lucifer later, so this is the run's only Eligos mark. Miss it and **ed_main_bad3** fires at main09_4 |
| 7 | **Look for Leonid.** | free — nothing tests this pick |
| 8 | **Let's go to the lake** | 🔑 the August/Gabriel branch. Greyed out unless #1, #2, #4 **and** #5 were all taken |

### August route (固有)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 9 | **"Sorry. I really don't feel up to it."** | ⚠️ the other option skips the august02 scene, leaving you one point short → **ed_main_bad1** |
| 10 | **I decided to take Father August's advice.** | ⚠️ "I thought Gabby was right." is Gabriel's pick — it skips august03, one point short → **ed_main_bad1** |
| 11 | **Shook my head.** | ⚠️ "Nodded." locks in the **Cecil** route and you lose August |
| 12 | **Decided to wander the hallway a bit.** | required. "…library" = Neil lock-in, "…floor above" = Leonid lock-in |
| 13 | **I'll mark Lucifer.** | — |
| 14 | **I pushed Gabby's hand away.** | free — the other option instead feeds Gabby's counter |
| 15 | **I bit down hard on my lip and rode out the urge.** | free |
| 16 | **Let me deliberately make a prophecy that could come true.** | ⚠️ "Let's avoid the danger." = **ed_main_bad2** at the end of main08 |
| 17 | **I think I'll go with Lucifer after all.** | either option is safe — see [Verified mechanics](#verified-mechanics) |
| 18 | *H-scene position (either):* **I'll do it with my mouth.** = Michael×August · **Please, give it to me.** = August×Michael | not good/bad — both continue the route, only the scene differs |
| 19 | **I turned my back on them both.** | ⚠️ "Biting my lip, I knelt where I stood." = **BAD END** (august_ed_bad1) |
| 20 | ✅ **「…………」** | **GOOD END**. ⚠️ "I raised the sword." = **BAD END** (august_ed_bad2) |

**Notes**
- The boathouse menu in august05 does **not** appear on this route — with fewer
  than 5 Gabby points the script walks you to the boathouse itself.
- #20's option text is the bare ellipsis 「…………」 in both languages; the
  pipeline leaves it untranslated on purpose.

---

## ガブリエル — Gabriel (Gabby) route → GOOD END

**Unlocked by** the August GOOD END, which in turn needs the other three. main02's
5th option is hidden until then.

```
main02 → main03 → main04 → leo01 → main05 → august01 → main06 → neil03
→ main07_1 → main07_2 → main07_3 → august04 → august05 → gabriel01 → gabriel02
→ main08 → gabriel03 → main09_1…4 → gabriel04 → main10 → gabriel05…gabriel10
→ gabriel_ed_good
```

### Common route (共通)

Same as August's #2–#8 above, except the first pick:

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 1 | **Looked behind me.** | ⚠️ take the **last (5th)** option — the **1st** one reads "Looked behind me." as well, because this translation collides. Count the slot, don't read it. Hidden until August GOOD |
| 2–8 | *as per the August table* | #1 is the only common-route difference |

### Gabriel route (固有)

| # | Pick (EN — this project) | Notes |
|---|--------------------------|-------|
| 9 | **"...That's true."** | required (+1 Gabby). The other option is August's pick |
| 10 | **I thought Gabby was right.** | required (+1 Gabby) |
| 11 | **Maybe I'll head back to the dorm and relax.** | free. "I need to go to Leonid's room." is greyed out here (it needs Leonid affection ≥ 2) |
| 12 | **Shook my head.** | ⚠️ "Nodded." locks in the **Cecil** route |
| 13 | **Decided to wander the hallway a bit.** | required |
| 14 | **I'll mark Lucifer.** | — |
| 15 | **I didn't push Gabby's hand away.** | required (+1 Gabby) |
| 16 | **I bit down hard on my lip and rode out the urge.** | required (+1 Gabby) |
| 17 | **No, I couldn't. I wouldn't go to the boathouse.** | 🔑 splits Gabriel off from August. ⚠️ **the menu only shows up with all 5 Gabby picks** (#1, #9, #10, #15, #16) — otherwise the game takes the boathouse for you and you finish on August's route |
| 18 | *H-scene position (either):* **"I'll pay you back one day."** = Michael×Gabby · **「…………」** = Gabby×Michael | not good/bad — both continue the route, only the scene differs |
| 19 | **Let me deliberately make a prophecy that could come true.** | ⚠️ "Let's avoid the danger." = **ed_main_bad2** |
| 20 | **Or maybe I should mark all the rest of them.** | the walkthrough's pick for this route; either option is safe — see [Verified mechanics](#verified-mechanics) |
| 21 | ✅ **Even so, I shook my head.** | **GOOD END**. ⚠️ "I was nodding at Father August's words." = **BAD END** (gabriel_ed_bad1) |

---

## Common-route BAD ENDs (ed_main_bad1–3)

These three are not a route. They are the common path's failure exits, and each
one has a single verified trigger.

| Ending | Fires at | Trigger |
|--------|----------|---------|
| **ed_main_bad1** | end of main07_1 | You reach main07_1's exit with no route locked in: August affection < 4 **and** Gabby points < 3. Only one pick can get you here — "Decided to wander the hallway a bit." is the one branch of main07_1 that sets no lock. Every other combination commits you to Cecil, Neil or Leonid and passes. In practice this is the August/Gabriel run that dropped one of the earlier picks. |
| **ed_main_bad2** | end of main08 | You picked **"Let's avoid the danger."** at main08's first menu. That single pick sets the flag; nothing else in the game sets or clears it. |
| **ed_main_bad3** | start of main09_4 | You never marked **Eligos** — neither at main04 nor at main07_2. The failure is silent for three whole scripts, which is why it looks like a late-firing trap. |

There is no separate "true" ending script. Gabriel's GOOD END is the last one
the game unlocks, and it is the closest thing the game has to one.

---

## Verified mechanics

`analysys/dump_choice_menus.py` lists every `2A 10 FF 00` choice table with its
option strings and branch targets; `analysys/dump_script_graph.py` interleaves
those with the script-to-script jumps and the flag writes. Between them they
cover all 32 menus in all 91 scripts. What that showed:

**Route unlock flags** — persistent, written by the GOOD END scripts.

| Var | Set by | Gates |
|-----|--------|-------|
| `0x384` / `0x385` / `0x386` | cecil / neil / leo `_ed_good` | main02 option **4** (August) |
| `0x387` | `august_ed_good` | main02 option **5** (Gabriel) |

**Per-run counters.**

| Var | Meaning | Where it is tested |
|-----|---------|--------------------|
| `0x320`–`0x323` | Cecil / Neil / Leonid / August affection | main07_1 and main07_3 exits |
| `0x325`–`0x329` | Cecil / Neil / Leonid / August / Gabriel route lock | main07_3, main08 and main09_4 dispatch |
| `0x32a` / `0x32b` | August / Gabby points | main05's lake option (`0x32a ≥ 2`), main07_1's exit (`0x32b ≥ 3`), august05's boathouse menu (needs all 5 Gabby points) |
| `0x32c`–`0x32f` | Lucifer / Eligos / Astaroth / Furcas marks | main07_3's Leonid gate (`0x32c ≥ 2`), main09_4's `ed_main_bad3` gate (`0x32d == 0`) |
| `0x334` | "avoided the danger" | main08's `ed_main_bad2` gate |

**Four choices carry a visibility condition** — everything else is always
selectable:

- main02 option 4 "Looked at the door to the sacristy." — the three GOOD flags
- main02 option 5 "Looked behind me." — August's GOOD flag
- main05 option 4 "Let's go to the lake" — August #2 taken, August #5 taken, August points ≥ 2
- main06 option 3 "I need to go to Leonid's room." — Leonid affection ≥ 2

**main08's second menu is not route-critical on any route.** "I think I'll go
with Lucifer after all." adds to the Lucifer mark counter, "Or maybe I should
mark all the rest of them." sets `0x330`; neither value is read after main09_1,
and no ending gate looks at either. The Leonid/Cecil/Neil tables above mark the
second option ⚠️ on the walkthrough's authority, and the Gabriel table follows
the same source in picking it — but the bytecode says both options only change
narration.
