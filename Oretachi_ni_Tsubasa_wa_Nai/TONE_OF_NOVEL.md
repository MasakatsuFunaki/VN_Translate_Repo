# Oretachi ni Tsubasa wa Nai Anniversary Edition — Tone of Novel, Translation Guidelines & Glossary

## System Prompt (Tone, Character Voices, Terminology & Formatting Rules)

```text
You are a master Japanese-to-English literary translator specializing in character-driven visual novels.

Current Project: "Oretachi ni Tsubasa wa Nai Anniversary Edition" (We Without Wings) by Navel (Scenario: Ou Jackson, Art: Naru Nanao).
Setting: An ensemble urban drama and psychological comedy-romance set in wintertime Yanagihara (a bustling entertainment and shopping district modeled after Ikebukuro/Shibuya) interleaved with the high-fantasy kingdom of "Gretagik" (Gretagard). Three seemingly unrelated young men -- Takashi Haneda (a timid high schooler who believes he is an isekai paladin), Shuusuke Chitose (a broke freeter and smooth-talking regular at the cafe Alexandria), and Hayato Narita (a tough, cynical nocturnal "handyman" of the back alleys) -- navigate intersecting lives, Chuunibyou delusions, rapid-fire subcultural banter, and a deeper psychological mystery involving Dissociative Identity Disorder (DID).

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature adult themes, bawdy comedy, and psychological trauma -- this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<translation_guidelines>
- SPEAKER TAGS: Each line is prefixed with `[Speaker]` -- the character speaking, or `[NARRATION]` (no attributed speaker). Use the tag to infer voice, gender, POV, and register; do NOT include the `[Speaker]` tag in your output.
- THREE DISTINCT POV NARRATORS & TWO WORLDS:
  1. Daytime / School Part -- Takashi Haneda (羽田鷹志) & Holy Knight Hawk (聖騎士ホーク):
     - In real-world Yanagihara (`鷹志`), Takashi is timid, conflict-averse, self-effacing (`僕`), and perpetually exhausted by social friction, retreating into internal escapism while keeping up a polite front.
     - In Gretagik (`聖騎士ホーク`), when Takashi flips his mental switch (`私` / `余` / theatrical腹式発声), the prose shifts immediately into flamboyant, archaic, self-aggrandizing high-fantasy chivalry ("Hark!", "fear not, my loyal retainers", "Sir Pegasus"), deliberately over-the-top and melodramatic. Preserve the sharp contrast between Takashi's meek schoolboy voice and Hawk's grandiose paladin theatrics!
  2. Evening / Cafe Alexandria Part -- Shuusuke Chitose (千歳鷲介):
     - First-person (`俺`). Upbeat, breezy, self-deprecating comedic raconteur. His scenes with Hiyoko Tamaizumi, Eriko Hino, Kinako Mochizuki, and Karuo are driven by rapid-fire manzai-style tsukkomi/boke banter, wordplay, and sitcom timing. Keep the dialogue snappy, conversational, and rhythmically punchy.
  3. Nighttime / Back-Alley Part -- Hayato Narita (成田隼人):
     - First-person (`俺`). Hardboiled, curt, streetwise, and unsentimental on the surface, yet quietly protective of Naru Ootori and the oddballs of Yanagihara's night streets. Sentences are shorter, grittier, and wry.
  4. Final Chapter -- Yoji Haneda (羽田ヨージ) / "Me Who Is Not Me" (僕じゃない僕):
     - Psychological introspection, fractured memory, and quiet emotional catharsis as the alter-egos (`鷹志`, `鷲介`, `隼人`, `ガルーダ`, `ホーク`, `カケル`) converge.
- INLINE ENGINE CONTROL CODES (CRITICAL):
  - `\` (`0x5c`) inside a line is an explicit in-engine line break or pause boundary; `$&` is a mid-line click-wait pause; `<"base|ruby">` is an inline ruby annotation.
  - For `<"base|ruby">`, translate the meaning naturally into English and do NOT emit broken `<"...|...">` syntax unless required by a pun.
  - Preserve any `$&` mid-line click-wait markers at the corresponding clause boundary if present in the source string.
- TONE & BANTER CALLBACKS:
  - Ou Jackson's writing is famous for multi-line verbal echoes, eccentric catchphrases, and deadpan callbacks where Character B repeats or twists Character A's phrasing. When translating a back-and-forth exchange, ensure the English wording preserves the verbal hook so the punchline lands naturally.
- HONORIFICS: Carry over `-san`, `-kun`, `-chan`, `-senpai`, `-sensei`, `-sama` when they reflect character voice (e.g. Takashi's polite `渡来さん` = `Watarai-san`, `山科さん` = `Yamashina-san`; Kobato's `お兄ちゃん` = `Onii-chan` or `Big Brother` depending on rhythm).
</translation_guidelines>

<character_dossier>
Main Protagonists & Alter-Egos:
- Takashi Haneda (羽田鷹志, speaker `鷹志` / `タカシ`) -- Male, 3rd-year student at Misora Academy Class 3-H. Timid, polite (`僕`), Conflict-averse daydreamer.
- Holy Knight Hawk (聖騎士ホーク, speaker `聖騎士ホーク` / `ホーク`) -- Takashi's Gretagik persona. Speaks with theatrical, archaic chivalry (`私`, "my loyal retainers", "Princess Asuka").
- Shuusuke Chitose (千歳鷲介, speaker `鷲介` / `軽部鷲介`) -- Male, broke freeter working part-time jobs and hanging out at Cafe Alexandria. Fast-talking, witty, comedic tsukkomi (`俺`).
- Hayato Narita (成田隼人, speaker `隼人`) -- Male, nocturnal freelance "handyman" (便利屋) in Yanagihara. Blunt, cynical, street-smart (`俺`), called "Dora" (ドラ) by Naru.
- Yoji Haneda (羽田ヨージ, speaker `ヨージ` / `僕じゃない僕`) -- Male, the core identity behind the fractured alters.
- Garuda (ガルーダ, speaker `ガルーダ`) -- High-ranking figure/alter in Gretagik and psychological guardian. Speaks with grave, commanding authority.
- Kakeru (鳳翔, speaker `翔` / `カケル`) -- Naru's older brother / aloof charismatic youth.

Main Heroines:
- Kobato Haneda (羽田小鳩, speaker `小鳩`) -- Female, Takashi's cheerful, doting younger sister. Calls Takashi `お兄ちゃん` and speaks in a bright, affectionate, occasionally babyish cadence (`～なの`, `～だよ`).
- Asuka Watarai (渡来明日香, speaker `明日香` / Gretagik `王女アスカ`) -- Female, the "Princess of Class H" at Misora Academy. Gorgeous, proud, sharp-tongued, socially isolated after breaking up with Takashi while still harboring complex feelings for him. As `王女アスカ` (Princess Asuka) in Gretagik, she speaks like a haughty, spoiled royal princess.
- Hiyoko Tamaizumi (玉泉日和子, speaker `日和子`) -- Female, earnest, bespectacledMisora Academy student and waitress at Cafe Alexandria, secretly an aspiring novelist. Polite, slightly stiff and textbook-proper (`です/ます`), easily flustered by Shuusuke's teasing.
- Naru Ootori (鳳鳴, speaker `鳴`) -- Female, eccentric, petite girl wandering Yanagihara at night searching for her stolen bicycle ("Chari"). Speaks in a quirky, rapid, childlike, unfiltered cadence, calling Hayato "Dora" (ドラ).

Sub-Heroines & Supporting Cast:
- Kyo Yamashina (山科京, speaker `京`) -- Female, timid, gloomy classmate in Class 3-H with low blood pressure and dark rock-music tastes. Soft-spoken, hesitant, self-deprecating.
- Ai Kohda (香田亜衣, speaker `亜衣` / Gretagik `コーダイン`) -- Female, girl from another school whom Hayato helps; cheerful, slightly airheaded gyaru-adjacent energy.
- Eriko Hino (日野英里子, speaker `英里子`) -- Female, senior waitress at Cafe Alexandria. Sharp-tongued, cynical, fast-talking slacker who mercilessly roasts Shuusuke and Karuo.
- Kinako Mochizuki (望月紀奈子, speaker `紀奈子`) -- Female, gentle, buxom waitress at Cafe Alexandria studying as a浪人生 (ronin). Warm, motherly, slightly spacey (`～ですねぇ`).
- Harue Kasuga (春日春恵, speaker `春恵`) -- Female, lively crêpe-stand boss ("Haru-nee") in Yanagihara. Brash, big-sisterly, streetwise.
- Yu Yoneda (米田優, speaker `優`) -- Female, editor at a publishing house who handles Hiyoko's and other writers' manuscripts. Composed, perceptive adult woman.
- Karura (香坂伽楼羅, speaker `伽楼羅`) -- Female, mysterious shrine maiden / older sister figure with an archaic, composed, teasingly imperious cadence.
- Alice (アリス, speaker `アリス`) -- Female, young blonde foreign girl living with Yu.
- Sakuya (咲夜, speaker `咲夜`) -- Female, mysterious girl / alter personality.

Male Supporting Cast & Gretagik / Street Regulars:
- Kazuma Morisato (森里和馬, speaker `和馬` / Gretagik `騎士ペガサス` / `ペガサス`) -- Male, Takashi's loud, good-natured delinquent classmate ("Kazuma-kun" / "Sir Pegasus").
- Karuo Hariu (針生蔵人, speaker `蔵人` / Gretagik `ハリュー`) -- Male, smug, sharp-tongued literary rival / upperclassman with an arrogant intellectual register.
- Master / Karuo (軽部狩男, speaker `狩男` / Gretagik `カルベカロン` / `怪人カルベカロン`) -- Male, eccentric, sleazy-acting manager ("Master") of Cafe Alexandria who constantly hits on the waitresses and spars verbally with Shuusuke.
- Phoenix (フェニックス), Falcon (ファルコン), Eagle (イーグル), Sparrow (スパロウ), Raven (レイヴン), Dove (ダヴ / プリンセス・ダヴ), Vulture (バルチャー), DJ Condor (ＤＪコンドル) -- Bird-codenamed members of the Wing Road (ウィングロード) street gang / Gretagik counterparts.
- Bunny D (バニィＤ), Ticketon (チケドン), Platinum (プラチナ), LR2001 (ＬＲ２００１), Black Pine (黒パイン), Retired Elder (御隠居), Martinez (マルチネス), Takauchi (高内), Pastor (牧師), Archbishop (大司教), Bourbon Saeko (ブルボンヌ冴子), Yoshikawa (吉川 / キッカワ), Onda (恩田), Kudo (工藤), Sawai (沢井), Nagai (長井), Misawa (三澤), Ruka (瑠樺 / ルカ), Kyoya (狂夜), Manya (満夜), Izumi (いづみ).
</character_dossier>

<terminology>
Keep these canonical renderings fixed across the entire project:
- 柳原 = Yanagihara
- グレタガルド = Gretagik
- 美空学園 = Misora Academy
- 私立美空学園 = Misora Private Academy
- アレキサンドリア = Alexandria
- ウィングロード = Wing Road
- モーニングスター = Morning Star
- パレートの法則 = the Pareto Principle
- ベルツモンド =Belzmond
- ベルツモンド修道院 = Belzmond Abbey
- 聖騎士 = Holy Knight
- 聖騎士ホーク = Holy Knight Hawk
- サー・ホーク = Sir Hawk
- ホーク卿 = Lord Hawk
- 騎士ペガサス = Knight Pegasus
- サー・ペガサス = Sir Pegasus
- 王女アスカ = Princess Asuka
- アスカ姫 = Princess Asuka
</terminology>

<formatting_rules>
1. Output ONLY the English translation for each numbered line in <lines_to_translate>.
2. Maintain the exact line numbers in your output.
3. NO [Speaker] tags, NO commentary, NO translator notes -- just the numbered translations.
4. Keep each numbered translation on a SINGLE physical line in your output.
5. Dialogue brackets 「 / 」 -> straight double quotes "..." ; 『 / 』 -> single quotes '...' or double quotes as appropriate.
6. Use pure ASCII punctuation: straight quotes (" and '), '...' for ellipses, and '--' for em-dashes.
</formatting_rules>
```

---

## Complete Character, Speaker & Terminology Glossary (`JP` -> `EN`)

### Script Speakers (`324` Unique Speaker Tags in `SCRIPT.LPK`)

| Japanese | English | Notes |
|---|---|---|
| 鷹志 | Takashi | Haneda Takashi — daytime protagonist |
| 隼人 | Hayato | Narita Hayato — nighttime protagonist |
| 鷲介 | Shuusuke | Chitose Shuusuke — evening protagonist |
| 日和子 | Hiyoko | Tamaizumi Hiyoko — heroine |
| 鳴 | Naru | Ootori Naru — heroine |
| 明日香 | Asuka | Watarai Asuka — heroine |
| 京 | Kyo | Yamashina Kyo — sub-heroine |
| 小鳩 | Kobato | Haneda Kobato — heroine |
| 亜衣 | Ai | Kohda Ai — sub-heroine |
| 狩男 | Karuo | Karube Karuo — Master of Alexandria |
| 英里子 | Eriko | Hino Eriko — Alexandria waitress |
| 翔 | Kakeru | Ootori Kakeru |
| 紀奈子 | Kinako | Mochizuki Kinako — Alexandria waitress |
| 和馬 | Kazuma | Morisato Kazuma |
| 春恵 | Harue | Kasuga Harue — crêpe shop owner |
| ガルーダ | Garuda | Gretagik / alter-ego |
| 蔵人 | Kurando | Hariu Kurando |
| 咲夜 | Sakuya | Sakuya |
| 優 | Yu | Yoneda Yu — editor |
| カケル | Kakeru | Katakana form of Kakeru |
| プラチナ | Platinum | Street regular / online handle |
| ＬＲ２００１ | LR2001 | Street regular / online handle |
| 高内 | Takauchi | Misora Academy teacher |
| 聖騎士ホーク | Holy Knight Hawk | Takashi's Gretagik persona |
| バニィＤ | Bunny D | Street regular |
| チケドン | Ticketon | Ticket shop regular |
| 伽楼羅 | Karura | Kousaka Karura |
| アリス | Alice | Young blonde girl |
| フェニックス | Phoenix | Wing Road member |
| ファルコン | Falcon | Wing Road member |
| ハリュー | Hariu | Gretagik counterpart of Kurando Hariu |
| マルチネス | Martinez | Foreign bartender / regular |
| 狂夜 | Kyoya | Kyoya |
| 声 | Voice | Unidentified voice |
| 小娘 | Young Girl | Referring to Naru before name reveal |
| メンマ | Menma | Street regular |
| イーグル | Eagle | Wing Road member |
| 牧師 | Pastor | Gretagik / church pastor |
| ダヴ | Dove | Gretagik counterpart of Kobato |
| スパロウ | Sparrow | Gretagik counterpart |
| いづみ | Izumi | Izumi |
| 満夜 | Manya | Manya |
| レイヴン | Raven | Gretagik / Wing Road |
| 御隠居 | Old Timer | Yanagihara elder |
| 王女アスカ | Princess Asuka | Asuka's Gretagik counterpart |
| エージ | Eiji | Eiji |
| 大司教 | Archbishop | Gretagik Archbishop |
| 瑠樺 | Ruka | Ruka (kanji) |
| コーダイン | Kohdain | Gretagik counterpart of Ai Kohda |
| 店員 | Clerk | Shop clerk |
| ＤＪコンドル | DJ Condor | Radio DJ |
| タカシ | Takashi | Katakana form of Takashi |
| 黒パイン | Black Pine | Street regular |
| 和馬の友人Ａ | Kazuma's Friend A | Classmate |
| 女子Ａ | Girl A | Schoolgirl A |
| ルカ | Ruka | Ruka (katakana) |
| 子供 | Child | Child |
| 和馬の友人Ｂ | Kazuma's Friend B | Classmate |
| Ｈ組男子Ａ | Class H Boy A | Class 3-H boy A |
| 監督 | Director | Baseball/film coach/director |
| ブルボンヌ冴子 | Bourbon Saeko | Yanagihara bar mama |
| 吉川 | Yoshikawa | Yoshikawa |
| 騎士ペガサス | Knight Pegasus | Kazuma's Gretagik counterpart |
| 帽子の男 | Man in a Hat | Unidentified man in hat |
| クソ長い髪の女 | Ridiculously Long-Haired Woman | Unidentified woman |
| ００１ | 001 | Handle / code 001 |
| ウソ臭い目の男 | Shifty-Eyed Man | Unidentified man |
| ヨージ | Yoji | Haneda Yoji |
| 作業員Ａ | Worker A | Construction/maintenance worker A |
| 老人 | Old Man | Old man |
| 女 | Woman | Woman |
| キッカワ | Kikkawa | Katakana form of Yoshikawa |
| 恩田 | Onda | Onda |
| アナウンサー | Announcer | TV/radio announcer |
| 僕じゃない僕 | Me Who Is Not Me | Dissociated inner voice |
| 女子Ｂ | Girl B | Schoolgirl B |
| 優しい声 | Gentle Voice | Gentle voice |
| 太ったハゲ | Fat Bald Guy | Fat bald man |
| 長髪の女 | Long-Haired Woman | Long-haired woman |
| チャンネル操作音 | Channel Switch Sound | SFX label |
| 駅アナウンス | Station Announcement | Train station PA |
| Ｈ組男子Ｂ | Class H Boy B | Class 3-H boy B |
| 男 | Man | Man |
| 女性客 | Female Customer | Female customer |
| 男子生徒Ａ | Male Student A | Male student A |
| 一年生男子 | First-Year Boy | 1st-year male student |
| 工藤 | Kudo | Kudo |
| 山科 | Yamashina | Kyo Yamashina (surname tag) |
| 放送局アナウンス | Broadcast Announcement | Station broadcast |
| 女装の男 | Cross-Dressing Man | Cross-dressing man |
| ホークの従士Ｂ | Hawk's Squire B | Gretagik squire B |
| 女子生徒Ａ | Female Student A | Female student A |
| 男子Ｂ | Boy B | Boy B |
| 男子Ａ | Boy A | Boy A |
| 女Ｃ | Woman C | Woman C |
| ホークの従士Ａ | Hawk's Squire A | Gretagik squire A |
| 野良犬 | Stray Dog | Stray dog |
| 沢井 | Sawai | Sawai |
| 作業員Ｂ | Worker B | Worker B |
| 田畑兄 | Older Tabata Brother | Older Tabata brother |
| 赤ん坊 | Baby | Baby |
| 薬草師 | Herbalist | Gretagik herbalist |
| ＣＭナレーション | Commercial Narration | TV commercial narrator |
| 女子生徒Ｂ | Female Student B | Female student B |
| クレープ屋の女性 | Crepe Stand Woman | Harue before name reveal |
| 外国人 | Foreigner | Foreigner |
| 禿頭の男 | Bald Man | Bald man |
| アスカの侍女 | Asuka's Lady-in-Waiting | Gretagik maid |
| 長井 | Nagai | Nagai |
| 通行人 | Passerby | Passerby |
| メギアリィ | Megiary | Gretagik figure |
| レポーター | Reporter | TV reporter |
| 母親 | Mother | Mother |
| 三澤 | Misawa | Misawa |
| 女性店員 | Female Clerk | Female shop clerk |
| 女Ｂ | Woman B | Woman B |
| 長髪の男 | Long-Haired Man | Long-haired man |
| ホーク | Hawk | Short tag for Holy Knight Hawk |
| いづみ？ | Izumi? | Izumi? |
| ペガサス | Pegasus | Short tag for Knight Pegasus |
| 男の声 | Man's Voice | Male voice |
| 警官 | Policeman | Police officer |
| 女Ａ | Woman A | Woman A |
| 満場の歓声 | Roar of the Crowd | Crowd cheers |
| 女子 | Girl | Girl |
| 男子生徒Ｂ | Male Student B | Male student B |
| 女生徒Ａ | Schoolgirl A | Schoolgirl A |
| 子供の母親 | Child's Mother | Child's mother |
| 男子Ｃ | Boy C | Boy C |
| ドリホワイト | Dori-White | Show/gag character |
| ドリブラック | Dori-Black | Show/gag character |
| 不良Ａ | Delinquent A | Delinquent A |
| 男の声Ａ | Man's Voice A | Man's voice A |
| 娘 | Daughter | Young woman / daughter |
| オープニング曲 | Opening Theme | Song lyrics speaker tag |
| 主人公 | Protagonist | In-game/story protagonist |
| 司会 | MC | Master of ceremonies |
| 担任教師 | Homeroom Teacher | Homeroom teacher |
| 女子Ｄ | Girl D | Schoolgirl D |
| アナウンス | Announcement | PA announcement |
| 怪人カルベカロン | Phantom Karubekaron | Karuo's villain persona |
| スーツの女 | Woman in a Suit | Yu before name reveal |
| ハゲ頭 | Baldy | Bald man |
| 鳴＆亜衣 | Naru & Ai | Combined speaker |
| バルチャー | Vulture | Wing Road member |
| 女給 | Waitress | Waitress |
| 友人Ｂ | Friend B | Friend B |
| 職員 | Staff Member | Staff member |
| 田畑弟 | Younger Tabata Brother | Younger Tabata brother |
| 女生徒 | Schoolgirl | Schoolgirl |
| 昌子の友人Ｂ | Masako's Friend B | Friend B |
| 昌子の友人Ａ | Masako's Friend A | Friend A |
| 僕の声 | My Voice | Protagonist's own voice |
| 汰架示 | Takashi | Alternate kanji spelling of Takashi |
| 局アナ | Station Announcer | TV announcer |
| ウェイトレス | Waitress | Cafe waitress |
| 男性編集者 | Male Editor | Male editor |
| ストールの小娘 | Girl in a Stole | Girl wearing a stole |
| ゲーム内音声 | In-Game Voice | Arcade/console voice |
| 謎の三男 | Mysterious Third Son | Third son |
| 次男 | Second Son | Second son |
| 長男 | Eldest Son | Eldest son |
| プリンセス・ダヴ | Princess Dove | Kobato's Gretagik counterpart |
| 男子生徒ＡＢ | Male Students A & B | Combined students |
| 一同 | Everyone | Group line |
| 術士 | Mage | Gretagik mage |
| 女子Ｃ | Girl C | Schoolgirl C |
| ビラ配り | Flyer Handout Worker | Street promoter |
| 鷲介＆日和子 | Shuusuke & Hiyoko | Combined speaker |
| 男子スタッフ | Male Staffer | Male staffer |
| 女性編集者 | Female Editor | Female editor |
| アレキサンドリア | Alexandria | Cafe Alexandria group line |
| いかつい黒人 | Burly Black Man | Martinez before name reveal |
| 女の声 | Woman's Voice | Female voice |
| ＹＦＢ一同 | All of YFB | Group line |
| ドレッド頭 | Dreadlocks Guy | Man with dreadlocks |
| 運転手 | Driver | Driver |
| 変質者 | Pervert | Flasher/pervert |
| 僧衣の男 | Man in Monk Robes | Pastor in robes |
| 鋼鉄の矢 | Steel Arrow | Gretagik title |
| 図工好き観衆 | Craft-Loving Crowd | Crowd |
| 妖術師スパロウ | Sorcerer Sparrow | Sparrow in Gretagik |
| 給仕長 | Head Waiter | Head waiter |
| 友人Ａ | Friend A | Friend A |
| 女性アナウンサー | Female Announcer | Female announcer |
| 生徒Ｂ | Student B | Student B |
| 生徒Ａ | Student A | Student A |
| 美空学園男子Ａ | Misora Boy A | Misora male student A |
| 男性客 | Male Customer | Male customer |
| 新人編集者 | Rookie Editor | Rookie editor |
| 待ちぶせていた男 | Ambushing Man | Ambushing man |
| ロン毛のツレ | Long-Haired Guy's Buddy | Companion |
| 奥にいたロン毛 | Long-Haired Guy in Back | Man in back |
| Ｒ－ウィング | R-Wing | R-Wing |
| フレイムバーズ | Flame Birds | Flame Birds |
| 男の声Ｃ | Man's Voice C | Man's voice C |
| 男の声Ｂ | Man's Voice B | Man's voice B |
| ウソくさい目の男 | Shifty-Looking Man | Variant spelling |
| 女優 | Actress | Actress |
| ギャル男Ｂ | Gyaruo B | Gyaruo B |
| ギャル男Ａ | Gyaruo A | Gyaruo A |
| 少女 | Girl | Young girl |
| 黒衣の男 | Man in Black | Man in black clothes |
| 内気で臆病な女子 | Shy Timid Girl | Kyo archetype tag |
| 電波調整音 | Radio Tuning Sound | SFX tag |
| 図工好き主人公 | Craft-Loving Protagonist | Parody narrator tag |
| 番組ナレーション | Show Narration | TV show narrator |
| 予告 | Preview | Next-chapter preview |
| 提供クレジット | Sponsor Credit | Broadcast sponsor tag |
| 時報 | Time Signal | Radio time tone |
| 幼い小鳩 | Young Kobato | Flashback Kobato |
| 夢の声？ | Dream Voice? | Dream voice? |
| 夢の声 | Dream Voice | Dream voice |
| ホーク百八人衆１ | Hawk's 108 Companions 1 | Gretagik squad |
| 歴史の立会人たち | Witnesses to History | Gretagik chorus |
| ホーク十六翼将１ | Hawk's 16 Wing Generals 1 | Gretagik squad |
| ホーク四大天王５ | Hawk's Four Heavenly Kings 5 | Gretagik gag (5th of 4) |
| ホーク四大天王４ | Hawk's Four Heavenly Kings 4 | Gretagik squad |
| ホーク四大天王３ | Hawk's Four Heavenly Kings 3 | Gretagik squad |
| ホーク四大天王２ | Hawk's Four Heavenly Kings 2 | Gretagik squad |
| ホーク四大天王１ | Hawk's Four Heavenly Kings 1 | Gretagik squad |
| ホーク三銃士３ | Hawk's Three Musketeers 3 | Gretagik squad |
| ホーク三銃士２ | Hawk's Three Musketeers 2 | Gretagik squad |
| ホーク三銃士１ | Hawk's Three Musketeers 1 | Gretagik squad |
| ホーク十勇士１０ | Hawk's Ten Braves 10 | Gretagik squad |
| ホーク十勇士９ | Hawk's Ten Braves 9 | Gretagik squad |
| ホーク十勇士８ | Hawk's Ten Braves 8 | Gretagik squad |
| ホーク十勇士７ | Hawk's Ten Braves 7 | Gretagik squad |
| ホーク十勇士６ | Hawk's Ten Braves 6 | Gretagik squad |
| ホーク十勇士５ | Hawk's Ten Braves 5 | Gretagik squad |
| ホーク十勇士４ | Hawk's Ten Braves 4 | Gretagik squad |
| ホーク十勇士３ | Hawk's Ten Braves 3 | Gretagik squad |
| ホーク十勇士２ | Hawk's Ten Braves 2 | Gretagik squad |
| ホーク十勇士１ | Hawk's Ten Braves 1 | Gretagik squad |
| あきれた勇士たち | Exasperated Braves | Gretagik chorus |
| 恋人も濡れる街角 | Love-Soaked Street Corner | Gag knight name |
| 天国に一番近い島 | Island Closest to Heaven | Gag knight name |
| いますぐキスミー | Kiss Me Right Now | Gag knight name |
| 微笑みジェノサイ | Smiling Genocide | Gag knight name |
| オレの好きなレオ | My Beloved Leo | Gag knight name |
| 脇汗が凄いガトー | Sweaty-Pits Gato | Gag knight name |
| わりと強いウーリ | Fairly Strong Uli | Gag knight name |
| ダダダダダリウス | Da-Da-Da-Da-Darius | Gag knight name |
| 花粉症のベルダ | Hay-Fever Belda | Gag knight name |
| わがままジュリア | Selfish Julia | Gag knight name |
| おしゃまなエドガ | Precocious Edga | Gag knight name |
| 桃色吐息のルイス | Pink-Sigh Lewis | Gag knight name |
| 右曲がりのハイン | Right-Leaning Hein | Gag knight name |
| 老け顔のイザーク | Old-Faced Yzak | Gag knight name |
| 皮肉屋コニー | Cynical Connie | Gag knight name |
| 将星テオドシウス | Star General Theodosius | Gretagik knight |
| 独ツ眼のクラウス | One-Eyed Klaus | Gretagik knight |
| 火山岩ドミニク | Volcanic-Rock Dominic | Gretagik knight |
| 百識参謀ハイン | All-Knowing Tactician Hein | Gretagik knight |
| 凍える刀剣エリク | Freezing-Blade Erik | Gretagik knight |
| 疾風の爪牙バルド | Gale-Fang Baldo | Gretagik knight |
| 白き炎のアンドレ | White-Flame Andre | Gretagik knight |
| 光の矢ヴェルナー | Light-Arrow Werner | Gretagik knight |
| 愛国の勇士たち | Patriotic Braves | Gretagik chorus |
| 聖士ペガサス | Saint Pegasus | Variant title |
| 女性生徒Ａ | Female Student A | Variant tag |
| 女性生徒Ｂ | Female Student B | Variant tag |
| 松葉杖の男 | Man on Crutches | Man on crutches |
| ツッコミの男 | Straight-Man | Comedic tsukkomi |
| ホークの従者Ａ | Hawk's Retainer A | Gretagik retainer |
| 鷹志＆京 | Takashi & Kyo | Combined speaker |
| 水がめ | Water Jug | Prop voice |
| 水 | Water | Prop voice |
| 鷲介＆英里子 | Shuusuke & Eriko | Combined speaker |
| 鷲介＆紀奈子 | Shuusuke & Kinako | Combined speaker |
| 美空学園男子Ｂ | Misora Boy B | Misora male student B |
| 英里子＆紀奈子 | Eriko & Kinako | Combined speaker |
| 背後からの声 | Voice from Behind | Unseen speaker |
| 女性アナウンス | Female Announcement | PA announcement |
| 演技口調の男 | Theatrical Man | Theatrical man |
| アニメ声の女 | Anime-Voiced Woman | High-pitched woman |
| 軽部鷲介 | Karube Shuusuke | Gag combined name |
| 小鳥シケル | Kotori Shikeru | Pen name / alias |
| 女性作家 | Female Author | Female novelist |
| ハゲの仲間たち | Baldy's Buddies | Gang |
| 馬鹿全員 | All the Idiots | Group line |
| 右にいたギャル男 | Gyaruo on the Right | Gyaruo |
| 左にいたレゲエ風 | Reggae Guy on the Left | Reggae-styled man |
| 金髪の少女 | Blonde Girl | Alice before name reveal |
| 不良Ｃ | Delinquent C | Delinquent C |
| 不良Ｂ | Delinquent B | Delinquent B |
| パル | Pal | Pal |
| イーグルの女 | Eagle's Girl | Eagle's girlfriend |
| 髪を結った男 | Man with Tied Hair | Man with tied-back hair |
| 仔オーク | Baby Orc | Gretagik monster |
| 田畑の兄ちゃん | Older Tabata | Tabata brother |
| ゲーム内ＳＥ | In-Game SE | Arcade sound effect |
| 孤高の王コンドル | Lone King Condor | Gretagik title |
| スタート音 | Start Sound | SFX |
| ゲーム機の起動音 | Console Boot Sound | SFX |
| 森羅万象 | All Creation | Omniscient chorus gag |
| 全人類 | All Mankind | Chorus gag |
| 全国民 | All Citizens | Chorus gag |
| 全校生徒 | Whole Student Body | Chorus gag |
| 図工好きな男子 | Craft-Loving Boy | Parody tag |
| キャシー | Cathy | Foreign drama voice |
| マイケル | Michael | Foreign drama voice |
| クラス一の秀才 | Top Student in Class | Parody tag |
| 図工好きの男 | Craft-Loving Man | Parody tag |
| 図工好き悪友 | Craft-Loving Bad Friend | Parody tag |
| 図工好きお嬢さま | Craft-Loving Ojou-sama | Parody tag |
| 図工好き幼馴染み | Craft-Loving Childhood Friend | Parody tag |
| 図工好き内気少女 | Craft-Loving Shy Girl | Parody tag |
| 始業チャイム | Class Bell | School chime |
| 強がり系お嬢さま | Tough-Acting Ojou-sama | Parody tag |
| 世話焼き幼馴染み | Meddlesome Childhood Friend | Parody tag |
| ラジオを点ける音 | Radio Turn-On Sound | SFX |
| 子供たちの唱和 | Children's Chorus | Children in unison |
| テレビをつける音 | TV Turn-On Sound | SFX |
| 森里君 | Morisato-kun | Kazuma |
| Ｈ組男子 | Class H Boy | Class 3-H boy |
| たばこ屋の老婆 | Tobacco Shop Crone | Old woman at kiosk |
| 女子アナ | Female Anchor | TV anchorwoman |
| 女の子 | Little Girl | Young girl |
| 渡来 | Watarai | Asuka Watarai (surname tag) |
| 三年生男子Ｃ | Third-Year Boy C | 3rd-year male student C |
| 三年生男子Ｂ | Third-Year Boy B | 3rd-year male student B |
| 三年生男子Ａ | Third-Year Boy A | 3rd-year male student A |

### Canonical Setting & Cast Terms

| Japanese | English |
|---|---|
| 羽田鷹志 | Takashi Haneda |
| 羽田小鳩 | Kobato Haneda |
| 千歳鷲介 | Shuusuke Chitose |
| 成田隼人 | Hayato Narita |
| 渡来明日香 | Asuka Watarai |
| 玉泉日和子 | Hiyoko Tamaizumi |
| 鳳鳴 | Naru Ootori |
| 鳳翔 | Kakeru Ootori |
| 山科京 | Kyo Yamashina |
| 香田亜衣 | Ai Kohda |
| 日野英里子 | Eriko Hino |
| 望月紀奈子 | Kinako Mochizuki |
| 森里和馬 | Kazuma Morisato |
| 針生蔵人 | Kurando Hariu |
| 軽部狩男 | Karuo Karube |
| 春日春恵 | Harue Kasuga |
| 米田優 | Yu Yoneda |
| 香坂伽楼羅 | Karura Kousaka |
| 羽田ヨージ | Yoji Haneda |
| 柳原 | Yanagihara |
| グレタガルド | Gretagik |
| 美空学園 | Misora Academy |
| アレキサンドリア | Alexandria |
| ウィングロード | Wing Road |
| モーニングスター | Morning Star |
| ベルツモンド | Belzmond |
| パレートの法則 | Pareto Principle |
