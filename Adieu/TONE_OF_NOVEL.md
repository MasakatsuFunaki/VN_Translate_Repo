# Tone, Voice & Glossary Guide — *Sayonara o Oshiete ~comment te dire adieu~* (`さよならを教えて ～comment te dire adieu～`)

## 1. Overview & Psychological Atmosphere

*Sayonara o Oshiete ~comment te dire adieu~* (CRAFTWORK, scenario & art by Nagaoka Kenji, SiglusEngine reissue) is a landmark *denpa* / psychological-horror visual novel told strictly through the fractured first-person perspective of **Hitomi Kousuke** (`人見 広介`), a student teacher assigned to a girls' academy for his teaching practicum.

What appears on the surface to be a quiet, sunlit routine of classrooms, infirmary smoke breaks, library shelves, and archery practice is filtered entirely through an **unreliable narrator** whose grip on reality steadily unravels. Mundane conversations bleed seamlessly into clinical paranoia, obsessive self-loathing, grotesque visceral hallucinations, and solemn angelic visitations without explicit scene-transition warnings.

### Core Tonal Principles

- **Never smooth out tonal dissonance.** Hitomi's narration shifts without warning from dry, detached observation to feverish repetition or serene horror. Preserve his abrupt sentence fragments, clinical nouns, and unsettling calm — do not rewrite surreal or disturbing passages into conventional melodrama.
- **Unreliable first-person interiority (`"I"`).** Render Hitomi's narration in immediate, unvarnished first person. When his inner monologue contradicts what another character appears to be saying or doing, trust the literal Japanese phrasing rather than "correcting" the contradiction; the contradiction *is* the narrative mechanism.
- **Uncensored psychological & body-horror fidelity.** Translate psychiatric, anatomical, obsessive, and mature passages accurately and unflinchingly, matching the cold literary precision of the Japanese prose without euphemism or sensationalism.
- **Japanese setting & surname-first order.** Keep Japanese name order (`Hitomi Kousuke`, `Sugamo Mutsuki`, `Tamachi Mahiru`, `Meguro Miyuki`, `Oomori Tonae`, `Takashima Semina`) and natural honorifics (`-sensei`, `-san`, `-chan`, `Onii-chan`, `Senpai`). Never Westernize Japanese school architecture, idioms, or cultural references.

<formatting_rules>
## 2. Full-Screen NVL (`640x480`) Presentation & Formatting Rules

1. **Full-Screen NVL Page Budget (`19` rows at `640x480`):** Every scene renders in SiglusEngine's full-screen NVL window (`#MWND.002`), accumulating multiple narration blocks and spoken lines onto a single `19`-row page before a page clear. Keep English sentences taut, natural, and economical — avoid bloated filler or explanatory expansion that inflates line wraps across multi-statement pages.
2. **Spoken Quote Framing:** Render `「...」` dialogue inside straight ASCII double quotes `"..."`, and `『...』` (recalled voices, inner echoes, book passages, titles) inside `'...'` (or `"..."` when used as an isolated spoken line).
3. **No Speaker Prefixes in Output:** Never emit `[Speaker]` or `Hitomi:` prefixes in translated dialogue strings; the runtime engine prepends speaker nameplates separately.
4. **Line Breaks & ASCII Hygiene:** Preserve literal `\n` / `\r\n` line breaks inside multi-line source strings. Use strictly ASCII punctuation (`--` for dashes, `...` for ellipses, straight `"` and `'`).
</formatting_rules>

## 3. Character Voice Dossiers

### Hitomi Kousuke (`人見 広介` / Speaker plate: `人見`)
- **Role:** Protagonist; student teacher conducting his teaching practicum (customizable via `#NAMAE` macros `＊Ａ` = surname `人見`, `＊Ｂ` = given name `広介`).
- **Spoken Voice:** Polite, mild, slightly diffident young adult (`"Yes, ma'am"`, `"Right..."`, restrained `desu/masu` with faculty and courteous plain speech with students), occasionally dry and self-deprecating around Tonae.
- **Inner Monologue:** Exhausted, hyper-observant, clinically detached, and obsessive. Uses short declarative sentences, abrupt self-corrections, and unsettlingly matter-of-fact descriptions when hallucinations intrude.
- **Forms of Address:** Addressed as `Hitomi-sensei` or `Sensei` by students, `Hitomi-kun` by Takashima Semina, `Kousuke` or `Hitomi` by Oomori Tonae, and `Onii-chan` by Tamachi Mahiru.

### Sugamo Mutsuki (`巣鴨 睦月` / Speaker plate: `睦　月`)
- **Role:** Quiet, bandaged girl encountered in the school corridors and empty rooms; the haunting emotional axis of Hitomi's delusions.
- **Voice & Register:** Whisper-quiet, weightless, detached, and unsettlingly compliant. Short, simple sentences with trailing pauses (`"..."`), devoid of anger or self-defense even in disturbing moments. Never give her snappy slang or modern sarcasm.

### Nozomi (`望美` / Speaker plate: `望　美`)
- **Role:** Solitary, imperious girl associated with the infirmary bed and fairy-tale tower imagery.
- **Voice & Register:** Haughty, theatrical, archaic-leaning pride (`"Hmph"`, commanding imperatives, aristocratic cadence) that flickers into raw, childlike terror and loneliness when pressed. Keep her diction poised and sharp.

### Tamachi Mahiru (`田町 まひる` / Speaker plate: `まひる`)
- **Role:** Younger student in the archery club (`弓道場`) who treats Hitomi as her beloved older brother.
- **Voice & Register:** Earnest, affectionate, slightly anxious devotion; always calls Hitomi `Onii-chan` (`お兄ちゃん`). Warm and eager to please on the surface, sharpening into brittle, desperate possessiveness whenever she feels neglected or threatened.

### Meguro Miyuki (`目黒 御幸` / Speaker plate: `御　幸`)
- **Role:** Prim, serious class representative (`委員長`) fixated on order, rules, and moral propriety.
- **Voice & Register:** Crisp, formal, articulate, and censorious (`"Hitomi-sensei, that is inappropriate"`). When her composure fractures, her formal diction twists into breathless, feverish intensity rather than casual slang.

### Koyori (`こより` / Speaker plate: `こより`)
- **Role:** Bright, childlike girl whose cheerful presence contrasts sharply with the surrounding dread.
- **Voice & Register:** Bouncy, sing-song, playful cadence with drawn-out vowels and innocent-sounding wonder (`"Senseeei!"`, `"Ehehe"`). Keep her phrasing light, rhythmic, and juvenile without turning incomprehensible.

### Oomori Tonae (`大森 となえ` / Speaker plate: `となえ`)
- **Role:** The academy's school nurse (`保健医`) and Hitomi's longtime acquaintance / confidante.
- **Voice & Register:** Lazy, sardonic, perceptive adult woman smoking in the infirmary (`保健室`). Speaks in relaxed, colloquial, teasing banter, probing Hitomi's mental state with clinical sharpness disguised as offhand jokes.

### Takashima Semina (`高島 瀬美奈` / Speaker plate: `瀬美奈`)
- **Role:** Veteran faculty member supervising Hitomi's student-teaching practicum.
- **Voice & Register:** Composed, brisk, no-nonsense professional educator (`"Hitomi-kun"`). Clear, structured sentences carrying effortless institutional authority.

### Angel (`天　使`) / Quote (`引　用`) / Girls (`少女達`)
- **Angel (`天　使`):** Solemn, uncanny, liturgical cadence addressing Hitomi during dreams and ecstatic/nightmarish visions.
- **Quote (`引　用`):** Epigraphs, clinical notes, or quoted textual fragments; translate with crisp literary precision.
- **Girls (`少女達`):** Brief chorus/group lines from female students in the classroom or courtyard.

## 4. Speaker Name Reference

<name_reference>
人見=Hitomi, 広介=Kousuke, 睦　月=Mutsuki, 望　美=Nozomi, まひる=Mahiru, 御　幸=Miyuki, こより=Koyori, となえ=Tonae, 瀬美奈=Semina, 天　使=Angel, 引　用=Quote, 少女達=Girls
</name_reference>

## 5. Canonical Glossary

| Japanese | English | Notes |
|---|---|---|
| 人見広介 | Hitomi Kousuke | Protagonist full name (surname-first) |
| 人見 | Hitomi | Protagonist surname & `＊Ａ` speaker plate |
| 広介 | Kousuke | Protagonist given name (`＊Ｂ` default) |
| 巣鴨睦月 | Sugamo Mutsuki | Heroine full name (surname-first) |
| 巣鴨 | Sugamo | Mutsuki surname |
| 睦月 | Mutsuki | Mutsuki given name (unspaced prose form) |
| 睦　月 | Mutsuki | Mutsuki two-kanji speaker plate (internal fullwidth space) |
| 望美 | Nozomi | Nozomi given name (unspaced prose form) |
| 望　美 | Nozomi | Nozomi two-kanji speaker plate (internal fullwidth space) |
| 田町まひる | Tamachi Mahiru | Heroine full name (surname-first) |
| 田町 | Tamachi | Mahiru surname |
| まひる | Mahiru | Mahiru given name & speaker plate |
| 目黒御幸 | Meguro Miyuki | Heroine full name (surname-first) |
| 目黒 | Meguro | Miyuki surname |
| 御幸 | Miyuki | Miyuki given name (unspaced prose form) |
| 御　幸 | Miyuki | Miyuki two-kanji speaker plate (internal fullwidth space) |
| こより | Koyori | Koyori given name & speaker plate |
| 大森となえ | Oomori Tonae | School nurse full name (surname-first) |
| 大森 | Oomori | Tonae surname |
| となえ | Tonae | Tonae given name & speaker plate |
| 高島瀬美奈 | Takashima Semina | Supervising teacher full name (surname-first) |
| 高島 | Takashima | Semina surname |
| 瀬美奈 | Semina | Semina given name & speaker plate |
| 天　使 | Angel | Two-kanji speaker plate (internal fullwidth space) |
| 引　用 | Quote | Two-kanji epigraph/recall plate (internal fullwidth space) |
| 少女達 | Girls | Minor group speaker plate |
| お兄ちゃん | Onii-chan | Mahiru's form of address for Hitomi |
| 先生 | Sensei | Student form of address for Hitomi / teachers |
| 人見先生 | Hitomi-sensei | Polite student/staff address for Hitomi |
| 教育実習生 | student teacher | Hitomi's practicum role at the academy |
| 教育実習 | teaching practicum | Hitomi's assignment at the academy |
| 保健室 | infirmary | Tonae's office and Nozomi's room |
| 図書室 | library | School library |
| 弓道場 | archery range | School kyudo hall (Mahiru's haunt) |
| 職員室 | staff room | Faculty office |
| 屋上 | rooftop | School building rooftop |
