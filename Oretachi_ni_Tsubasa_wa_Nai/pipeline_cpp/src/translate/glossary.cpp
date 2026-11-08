// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "translate/glossary.h"

#include <filesystem>
#include <sstream>

#include "common/util.h"

namespace oretsuba::translate {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kFenceStartMarker = "```text";
constexpr std::string_view kFenceEndMarker = "```";
constexpr std::string_view kSpeakerSectionHeader = "### Script Speakers";
constexpr std::string_view kTermSectionHeader = "### Canonical Setting";
constexpr std::size_t kMinMarkdownTablePipes = 3U;

constexpr const char* kBuiltinSystemPrompt = R"(You are a master Japanese-to-English literary translator specializing in character-driven visual novels.

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
- Hiyoko Tamaizumi (玉泉日和子, speaker `日和子`) -- Female, earnest, bespectacled Misora Academy student and waitress at Cafe Alexandria, secretly an aspiring novelist. Polite, slightly stiff and textbook-proper (`です/ます`), easily flustered by Shuusuke's teasing.
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
- ベルツモンド = Belzmond
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
</formatting_rules>)";

constexpr const char* kBuiltinNeutralSystemPrompt = R"(You are a professional Japanese-to-English literary translator working on a fictional Japanese visual novel script ("Oretachi ni Tsubasa wa Nai" by Navel).
Translate each numbered Japanese line in <lines_to_translate> accurately and naturally into English.
Maintain exact line numbers, omit [Speaker] prefixes from your output, keep each numbered translation on a single line, and use pure ASCII punctuation ("...", '...', ..., --).

<character_dossier>
- 鷹志 / タカシ = Takashi Haneda (he/him, polite schoolboy)
- 聖騎士ホーク / ホーク = Holy Knight Hawk (he/him, theatrical chivalric knight)
- 鷲介 = Shuusuke Chitose (he/him, witty cafe regular)
- 隼人 = Hayato Narita (he/him, curt handyman)
- 小鳩 = Kobato Haneda (she/her, Takashi's younger sister)
- 明日香 / 王女アスカ = Asuka Watarai / Princess Asuka (she/her)
- 日和子 = Hiyoko Tamaizumi (she/her)
- 鳴 = Naru Ootori (she/her)
- 京 = Kyo Yamashina (she/her)
- 亜衣 = Ai Kohda (she/her)
- 狩男 = Karuo Karube (he/him)
- 英里子 = Eriko Hino (she/her)
- 紀奈子 = Kinako Mochizuki (she/her)
- 和馬 / 騎士ペガサス = Kazuma Morisato / Knight Pegasus (he/him)
- 春恵 = Harue Kasuga (she/her)
- ガルーダ = Garuda (he/him)
- 蔵人 = Kurando Hariu (he/him)
</character_dossier>

<terminology>
- 柳原 = Yanagihara
- グレタガルド = Gretagik
- 美空学園 = Misora Academy
- アレキサンドリア = Alexandria
- ウィングロード = Wing Road
</terminology>)";

std::vector<std::pair<std::string, std::string>> BuildDefaultSpeakers() {
    return {
        {"鷹志", "Takashi"},
        {"隼人", "Hayato"},
        {"鷲介", "Shuusuke"},
        {"日和子", "Hiyoko"},
        {"鳴", "Naru"},
        {"明日香", "Asuka"},
        {"京", "Kyo"},
        {"小鳩", "Kobato"},
        {"亜衣", "Ai"},
        {"狩男", "Karuo"},
        {"英里子", "Eriko"},
        {"翔", "Kakeru"},
        {"紀奈子", "Kinako"},
        {"和馬", "Kazuma"},
        {"春恵", "Harue"},
        {"ガルーダ", "Garuda"},
        {"蔵人", "Kurando"},
        {"咲夜", "Sakuya"},
        {"優", "Yu"},
        {"カケル", "Kakeru"},
        {"プラチナ", "Platinum"},
        {"ＬＲ２００１", "LR2001"},
        {"高内", "Takauchi"},
        {"聖騎士ホーク", "Holy Knight Hawk"},
        {"バニィＤ", "Bunny D"},
        {"チケドン", "Ticketon"},
        {"伽楼羅", "Karura"},
        {"アリス", "Alice"},
        {"フェニックス", "Phoenix"},
        {"ファルコン", "Falcon"},
        {"ハリュー", "Hariu"},
        {"マルチネス", "Martinez"},
        {"狂夜", "Kyoya"},
        {"声", "Voice"},
        {"小娘", "Young Girl"},
        {"メンマ", "Menma"},
        {"イーグル", "Eagle"},
        {"牧師", "Pastor"},
        {"ダヴ", "Dove"},
        {"スパロウ", "Sparrow"},
        {"いづみ", "Izumi"},
        {"満夜", "Manya"},
        {"レイヴン", "Raven"},
        {"御隠居", "Old Timer"},
        {"王女アスカ", "Princess Asuka"},
        {"エージ", "Eiji"},
        {"大司教", "Archbishop"},
        {"瑠樺", "Ruka"},
        {"コーダイン", "Kohdain"},
        {"店員", "Clerk"},
        {"ＤＪコンドル", "DJ Condor"},
        {"タカシ", "Takashi"},
        {"黒パイン", "Black Pine"},
        {"和馬の友人Ａ", "Kazuma's Friend A"},
        {"女子Ａ", "Girl A"},
        {"ルカ", "Ruka"},
        {"子供", "Child"},
        {"和馬の友人Ｂ", "Kazuma's Friend B"},
        {"Ｈ組男子Ａ", "Class H Boy A"},
        {"監督", "Director"},
        {"ブルボンヌ冴子", "Bourbon Saeko"},
        {"吉川", "Yoshikawa"},
        {"騎士ペガサス", "Knight Pegasus"},
        {"帽子の男", "Man in a Hat"},
        {"クソ長い髪の女", "Ridiculously Long-Haired Woman"},
        {"００１", "001"},
        {"ウソ臭い目の男", "Shifty-Eyed Man"},
        {"ヨージ", "Yoji"},
        {"作業員Ａ", "Worker A"},
        {"老人", "Old Man"},
        {"女", "Woman"},
        {"キッカワ", "Kikkawa"},
        {"恩田", "Onda"},
        {"アナウンサー", "Announcer"},
        {"僕じゃない僕", "Me Who Is Not Me"},
        {"女子Ｂ", "Girl B"},
        {"優しい声", "Gentle Voice"},
        {"太ったハゲ", "Fat Bald Guy"},
        {"長髪の女", "Long-Haired Woman"},
        {"チャンネル操作音", "Channel Switch Sound"},
        {"駅アナウンス", "Station Announcement"},
        {"Ｈ組男子Ｂ", "Class H Boy B"},
        {"男", "Man"},
        {"女性客", "Female Customer"},
        {"男子生徒Ａ", "Male Student A"},
        {"一年生男子", "First-Year Boy"},
        {"工藤", "Kudo"},
        {"山科", "Yamashina"},
        {"放送局アナウンス", "Broadcast Announcement"},
        {"女装の男", "Cross-Dressing Man"},
        {"ホークの従士Ｂ", "Hawk's Squire B"},
        {"女子生徒Ａ", "Female Student A"},
        {"男子Ｂ", "Boy B"},
        {"男子Ａ", "Boy A"},
        {"女Ｃ", "Woman C"},
        {"ホークの従士Ａ", "Hawk's Squire A"},
        {"野良犬", "Stray Dog"},
        {"沢井", "Sawai"},
        {"作業員Ｂ", "Worker B"},
        {"田畑兄", "Older Tabata Brother"},
        {"赤ん坊", "Baby"},
        {"薬草師", "Herbalist"},
        {"ＣＭナレーション", "Commercial Narration"},
        {"女子生徒Ｂ", "Female Student B"},
        {"クレープ屋の女性", "Crepe Stand Woman"},
        {"外国人", "Foreigner"},
        {"禿頭の男", "Bald Man"},
        {"アスカの侍女", "Asuka's Lady-in-Waiting"},
        {"長井", "Nagai"},
        {"通行人", "Passerby"},
        {"メギアリィ", "Megiary"},
        {"レポーター", "Reporter"},
        {"母親", "Mother"},
        {"三澤", "Misawa"},
        {"女性店員", "Female Clerk"},
        {"女Ｂ", "Woman B"},
        {"長髪の男", "Long-Haired Man"},
        {"ホーク", "Hawk"},
        {"いづみ？", "Izumi?"},
        {"ペガサス", "Pegasus"},
        {"男の声", "Man's Voice"},
        {"警官", "Policeman"},
        {"女Ａ", "Woman A"},
        {"満場の歓声", "Roar of the Crowd"},
        {"女子", "Girl"},
        {"男子生徒Ｂ", "Male Student B"},
        {"女生徒Ａ", "Schoolgirl A"},
        {"子供の母親", "Child's Mother"},
        {"男子Ｃ", "Boy C"},
        {"ドリホワイト", "Dori-White"},
        {"ドリブラック", "Dori-Black"},
        {"不良Ａ", "Delinquent A"},
        {"男の声Ａ", "Man's Voice A"},
        {"娘", "Daughter"},
        {"オープニング曲", "Opening Theme"},
        {"主人公", "Protagonist"},
        {"司会", "MC"},
        {"担任教師", "Homeroom Teacher"},
        {"女子Ｄ", "Girl D"},
        {"アナウンス", "Announcement"},
        {"怪人カルベカロン", "Phantom Karubekaron"},
        {"スーツの女", "Woman in a Suit"},
        {"ハゲ頭", "Baldy"},
        {"鳴＆亜衣", "Naru & Ai"},
        {"バルチャー", "Vulture"},
        {"女給", "Waitress"},
        {"友人Ｂ", "Friend B"},
        {"職員", "Staff Member"},
        {"田畑弟", "Younger Tabata Brother"},
        {"女生徒", "Schoolgirl"},
        {"昌子の友人Ｂ", "Masako's Friend B"},
        {"昌子の友人Ａ", "Masako's Friend A"},
        {"僕の声", "My Voice"},
        {"汰架示", "Takashi"},
        {"局アナ", "Station Announcer"},
        {"ウェイトレス", "Waitress"},
        {"男性編集者", "Male Editor"},
        {"ストールの小娘", "Girl in a Stole"},
        {"ゲーム内音声", "In-Game Voice"},
        {"謎の三男", "Mysterious Third Son"},
        {"次男", "Second Son"},
        {"長男", "Eldest Son"},
        {"プリンセス・ダヴ", "Princess Dove"},
        {"男子生徒ＡＢ", "Male Students A & B"},
        {"一同", "Everyone"},
        {"術士", "Mage"},
        {"女子Ｃ", "Girl C"},
        {"ビラ配り", "Flyer Handout Worker"},
        {"鷲介＆日和子", "Shuusuke & Hiyoko"},
        {"男子スタッフ", "Male Staffer"},
        {"女性編集者", "Female Editor"},
        {"アレキサンドリア", "Alexandria"},
        {"いかつい黒人", "Burly Black Man"},
        {"女の声", "Woman's Voice"},
        {"ＹＦＢ一同", "All of YFB"},
        {"ドレッド頭", "Dreadlocks Guy"},
        {"運転手", "Driver"},
        {"変質者", "Pervert"},
        {"僧衣の男", "Man in Monk Robes"},
        {"鋼鉄の矢", "Steel Arrow"},
        {"図工好き観衆", "Craft-Loving Crowd"},
        {"妖術師スパロウ", "Sorcerer Sparrow"},
        {"給仕長", "Head Waiter"},
        {"友人Ａ", "Friend A"},
        {"女性アナウンサー", "Female Announcer"},
        {"生徒Ｂ", "Student B"},
        {"生徒Ａ", "Student A"},
        {"美空学園男子Ａ", "Misora Boy A"},
        {"男性客", "Male Customer"},
        {"新人編集者", "Rookie Editor"},
        {"待ちぶせていた男", "Ambushing Man"},
        {"ロン毛のツレ", "Long-Haired Guy's Buddy"},
        {"奥にいたロン毛", "Long-Haired Guy in Back"},
        {"Ｒ－ウィング", "R-Wing"},
        {"フレイムバーズ", "Flame Birds"},
        {"男の声Ｃ", "Man's Voice C"},
        {"男の声Ｂ", "Man's Voice B"},
        {"ウソくさい目の男", "Shifty-Looking Man"},
        {"女優", "Actress"},
        {"ギャル男Ｂ", "Gyaruo B"},
        {"ギャル男Ａ", "Gyaruo A"},
        {"少女", "Girl"},
        {"黒衣の男", "Man in Black"},
        {"内気で臆病な女子", "Shy Timid Girl"},
        {"電波調整音", "Radio Tuning Sound"},
        {"図工好き主人公", "Craft-Loving Protagonist"},
        {"番組ナレーション", "Show Narration"},
        {"予告", "Preview"},
        {"提供クレジット", "Sponsor Credit"},
        {"時報", "Time Signal"},
        {"幼い小鳩", "Young Kobato"},
        {"夢の声？", "Dream Voice?"},
        {"夢の声", "Dream Voice"},
        {"ホーク百八人衆１", "Hawk's 108 Companions 1"},
        {"歴史の立会人たち", "Witnesses to History"},
        {"ホーク十六翼将１", "Hawk's 16 Wing Generals 1"},
        {"ホーク四大天王５", "Hawk's Four Heavenly Kings 5"},
        {"ホーク四大天王４", "Hawk's Four Heavenly Kings 4"},
        {"ホーク四大天王３", "Hawk's Four Heavenly Kings 3"},
        {"ホーク四大天王２", "Hawk's Four Heavenly Kings 2"},
        {"ホーク四大天王１", "Hawk's Four Heavenly Kings 1"},
        {"ホーク三銃士３", "Hawk's Three Musketeers 3"},
        {"ホーク三銃士２", "Hawk's Three Musketeers 2"},
        {"ホーク三銃士１", "Hawk's Three Musketeers 1"},
        {"ホーク十勇士１０", "Hawk's Ten Braves 10"},
        {"ホーク十勇士９", "Hawk's Ten Braves 9"},
        {"ホーク十勇士８", "Hawk's Ten Braves 8"},
        {"ホーク十勇士７", "Hawk's Ten Braves 7"},
        {"ホーク十勇士６", "Hawk's Ten Braves 6"},
        {"ホーク十勇士５", "Hawk's Ten Braves 5"},
        {"ホーク十勇士４", "Hawk's Ten Braves 4"},
        {"ホーク十勇士３", "Hawk's Ten Braves 3"},
        {"ホーク十勇士２", "Hawk's Ten Braves 2"},
        {"ホーク十勇士１", "Hawk's Ten Braves 1"},
        {"あきれた勇士たち", "Exasperated Braves"},
        {"恋人も濡れる街角", "Love-Soaked Street Corner"},
        {"天国に一番近い島", "Island Closest to Heaven"},
        {"いますぐキスミー", "Kiss Me Right Now"},
        {"微笑みジェノサイ", "Smiling Genocide"},
        {"オレの好きなレオ", "My Beloved Leo"},
        {"脇汗が凄いガトー", "Sweaty-Pits Gato"},
        {"わりと強いウーリ", "Fairly Strong Uli"},
        {"ダダダダダリウス", "Da-Da-Da-Da-Darius"},
        {"花粉症のベルダ", "Hay-Fever Belda"},
        {"わがままジュリア", "Selfish Julia"},
        {"おしゃまなエドガ", "Precocious Edga"},
        {"桃色吐息のルイス", "Pink-Sigh Lewis"},
        {"右曲がりのハイン", "Right-Leaning Hein"},
        {"老け顔のイザーク", "Old-Faced Yzak"},
        {"皮肉屋コニー", "Cynical Connie"},
        {"将星テオドシウス", "Star General Theodosius"},
        {"独ツ眼のクラウス", "One-Eyed Klaus"},
        {"火山岩ドミニク", "Volcanic-Rock Dominic"},
        {"百識参謀ハイン", "All-Knowing Tactician Hein"},
        {"凍える刀剣エリク", "Freezing-Blade Erik"},
        {"疾風の爪牙バルド", "Gale-Fang Baldo"},
        {"白き炎のアンドレ", "White-Flame Andre"},
        {"光の矢ヴェルナー", "Light-Arrow Werner"},
        {"愛国の勇士たち", "Patriotic Braves"},
        {"聖士ペガサス", "Saint Pegasus"},
        {"女性生徒Ａ", "Female Student A"},
        {"女性生徒Ｂ", "Female Student B"},
        {"松葉杖の男", "Man on Crutches"},
        {"ツッコミの男", "Straight-Man"},
        {"ホークの従者Ａ", "Hawk's Retainer A"},
        {"鷹志＆京", "Takashi & Kyo"},
        {"水がめ", "Water Jug"},
        {"水", "Water"},
        {"鷲介＆英里子", "Shuusuke & Eriko"},
        {"鷲介＆紀奈子", "Shuusuke & Kinako"},
        {"美空学園男子Ｂ", "Misora Boy B"},
        {"英里子＆紀奈子", "Eriko & Kinako"},
        {"背後からの声", "Voice from Behind"},
        {"女性アナウンス", "Female Announcement"},
        {"演技口調の男", "Theatrical Man"},
        {"アニメ声の女", "Anime-Voiced Woman"},
        {"軽部鷲介", "Karube Shuusuke"},
        {"小鳥シケル", "Kotori Shikeru"},
        {"女性作家", "Female Author"},
        {"ハゲの仲間たち", "Baldy's Buddies"},
        {"馬鹿全員", "All the Idiots"},
        {"右にいたギャル男", "Gyaruo on the Right"},
        {"左にいたレゲエ風", "Reggae Guy on the Left"},
        {"金髪の少女", "Blonde Girl"},
        {"不良Ｃ", "Delinquent C"},
        {"不良Ｂ", "Delinquent B"},
        {"パル", "Pal"},
        {"イーグルの女", "Eagle's Girl"},
        {"髪を結った男", "Man with Tied Hair"},
        {"仔オーク", "Baby Orc"},
        {"田畑の兄ちゃん", "Older Tabata"},
        {"ゲーム内ＳＥ", "In-Game SE"},
        {"孤高の王コンドル", "Lone King Condor"},
        {"スタート音", "Start Sound"},
        {"ゲーム機の起動音", "Console Boot Sound"},
        {"森羅万象", "All Creation"},
        {"全人類", "All Mankind"},
        {"全国民", "All Citizens"},
        {"全校生徒", "Whole Student Body"},
        {"図工好きな男子", "Craft-Loving Boy"},
        {"キャシー", "Cathy"},
        {"マイケル", "Michael"},
        {"クラス一の秀才", "Top Student in Class"},
        {"図工好きの男", "Craft-Loving Man"},
        {"図工好き悪友", "Craft-Loving Bad Friend"},
        {"図工好きお嬢さま", "Craft-Loving Ojou-sama"},
        {"図工好き幼馴染み", "Craft-Loving Childhood Friend"},
        {"図工好き内気少女", "Craft-Loving Shy Girl"},
        {"始業チャイム", "Class Bell"},
        {"強がり系お嬢さま", "Tough-Acting Ojou-sama"},
        {"世話焼き幼馴染み", "Meddlesome Childhood Friend"},
        {"ラジオを点ける音", "Radio Turn-On Sound"},
        {"子供たちの唱和", "Children's Chorus"},
        {"テレビをつける音", "TV Turn-On Sound"},
        {"森里君", "Morisato-kun"},
        {"Ｈ組男子", "Class H Boy"},
        {"たばこ屋の老婆", "Tobacco Shop Crone"},
        {"女子アナ", "Female Anchor"},
        {"女の子", "Little Girl"},
        {"渡来", "Watarai"},
        {"三年生男子Ｃ", "Third-Year Boy C"},
        {"三年生男子Ｂ", "Third-Year Boy B"},
        {"三年生男子Ａ", "Third-Year Boy A"},
        {"？？？", "???"},
        {"…", "..."},
        {"……", "......"},
        {"………", "........."},
        {"…………", "............"},
        {"……………", "..............."},
        {"………………", ".................."},
        {"…。", "..."},
        {"……。", "......"},
        {"………。", "........."},
        {"…………。", "............"},
        {"　…………。", "............"},
        {"……？", "......?"},
        {"……！", "......!"},
        {"…？", "...?"},
        {"…！", "...!"},
        {"！？", "!?"},
        {"？！", "?!"},
        {"？", "?"},
        {"！", "!"},
        {"「…」", "\"...\""},
        {"「……」", "\"......\""},
        {"「………」", "\".........\""},
        {"「…………」", "\"............\""},
        {"「………………」", "\"..................\""},
        {"「……？」", "\"......?\""},
        {"「……！」", "\"......!\""},
        {"「……！？」", "\"......!?\""},
        {"「…………？」", "\"............?\""},
        {"「…………！」", "\"............!\""},
        {"「…………！？」", "\"............!?\""},
        {"（……）", "(......)"},
        {"（…………）", "(............)"},
        {"（………………）", "(..................)"},
    };
}

std::vector<std::pair<std::string, std::string>> BuildDefaultTerms() {
    return {
        {"羽田鷹志", "Takashi Haneda"},
        {"羽田小鳩", "Kobato Haneda"},
        {"千歳鷲介", "Shuusuke Chitose"},
        {"成田隼人", "Hayato Narita"},
        {"渡来明日香", "Asuka Watarai"},
        {"玉泉日和子", "Hiyoko Tamaizumi"},
        {"鳳鳴", "Naru Ootori"},
        {"鳳翔", "Kakeru Ootori"},
        {"山科京", "Kyo Yamashina"},
        {"香田亜衣", "Ai Kohda"},
        {"日野英里子", "Eriko Hino"},
        {"望月紀奈子", "Kinako Mochizuki"},
        {"森里和馬", "Kazuma Morisato"},
        {"針生蔵人", "Kurando Hariu"},
        {"軽部狩男", "Karuo Karube"},
        {"春日春恵", "Harue Kasuga"},
        {"米田優", "Yu Yoneda"},
        {"香坂伽楼羅", "Karura Kousaka"},
        {"羽田ヨージ", "Yoji Haneda"},
        {"柳原", "Yanagihara"},
        {"グレタガルド", "Gretagik"},
        {"美空学園", "Misora Academy"},
        {"アレキサンドリア", "Alexandria"},
        {"ウィングロード", "Wing Road"},
        {"モーニングスター", "Morning Star"},
        {"ベルツモンド", "Belzmond"},
        {"パレートの法則", "Pareto Principle"},
    };
}

bool ParseMarkdownRow(std::string_view line, std::string& jp_out, std::string& en_out) {
    const std::string trimmed = TrimUnicode(line);
    if (trimmed.empty() || trimmed.front() != '|') {
        return false;
    }
    std::vector<std::string> cells;
    std::size_t pos = 1U;
    std::size_t pipe_count = 1U;
    while (pos <= trimmed.size()) {
        const std::size_t next_pipe = trimmed.find('|', pos);
        if (next_pipe == std::string_view::npos) {
            break;
        }
        ++pipe_count;
        cells.push_back(TrimUnicode(trimmed.substr(pos, next_pipe - pos)));
        pos = next_pipe + 1U;
    }
    if (pipe_count < kMinMarkdownTablePipes || cells.size() < 2U) {
        return false;
    }
    const std::string& col0 = cells[0];
    const std::string& col1 = cells[1];
    if (col0.empty() || col1.empty()) {
        return false;
    }
    if (col0 == "Japanese" || col0 == "Property" || col0.front() == '-') {
        return false;
    }
    jp_out = col0;
    en_out = col1;
    return true;
}

}  // namespace

struct Glossary::Impl {
    std::string system_prompt = kBuiltinSystemPrompt;
    std::string neutral_system_prompt = kBuiltinNeutralSystemPrompt;
    std::vector<std::pair<std::string, std::string>> ordered_speakers;
    std::unordered_map<std::string, std::string> speaker_map;
    std::vector<std::pair<std::string, std::string>> ordered_terms;

    Impl()
        : ordered_speakers(BuildDefaultSpeakers()),
          ordered_terms(BuildDefaultTerms()) {
        RebuildSpeakerMap();
    }

    void RebuildSpeakerMap() {
        speaker_map.clear();
        speaker_map.reserve(ordered_speakers.size());
        for (const auto& [jp, en] : ordered_speakers) {
            speaker_map.emplace(jp, en);
        }
    }

    void UpsertSpeaker(const std::string& jp, const std::string& en) {
        const auto it = speaker_map.find(jp);
        if (it != speaker_map.end()) {
            it->second = en;
            for (auto& pair : ordered_speakers) {
                if (pair.first == jp) {
                    pair.second = en;
                    break;
                }
            }
            return;
        }
        ordered_speakers.emplace_back(jp, en);
        speaker_map.emplace(jp, en);
    }

    void UpsertTerm(const std::string& jp, const std::string& en) {
        for (auto& pair : ordered_terms) {
            if (pair.first == jp) {
                pair.second = en;
                return;
            }
        }
        ordered_terms.emplace_back(jp, en);
    }

    bool LoadFromFile(const std::string& path) {
        std::error_code ec;
        if (path.empty() || !fs::is_regular_file(fs::u8path(path), ec)) {
            return false;
        }
        const Bytes raw = ReadFileBytes(path);
        const std::string content(reinterpret_cast<const char*>(raw.data()), raw.size());

        const std::size_t fence_start = content.find(kFenceStartMarker);
        if (fence_start != std::string::npos) {
            const std::size_t body_start = content.find('\n', fence_start);
            if (body_start != std::string::npos) {
                const std::size_t fence_end =
                    content.find(kFenceEndMarker, body_start + 1U);
                if (fence_end != std::string::npos && fence_end > body_start + 1U) {
                    std::string extracted =
                        TrimUnicode(content.substr(body_start + 1U, fence_end - body_start - 1U));
                    if (!extracted.empty()) {
                        system_prompt = std::move(extracted);
                    }
                }
            }
        }

        enum class TableSection {
            kNone,
            kSpeakers,
            kTerms,
        };
        TableSection section = TableSection::kNone;

        std::istringstream stream(content);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.find(kSpeakerSectionHeader) != std::string::npos) {
                section = TableSection::kSpeakers;
                continue;
            }
            if (line.find(kTermSectionHeader) != std::string::npos) {
                section = TableSection::kTerms;
                continue;
            }
            if (section == TableSection::kNone) {
                continue;
            }
            std::string jp;
            std::string en;
            if (!ParseMarkdownRow(line, jp, en)) {
                continue;
            }
            if (section == TableSection::kSpeakers) {
                UpsertSpeaker(jp, en);
                continue;
            }
            UpsertTerm(jp, en);
        }
        return true;
    }
};

Glossary::Glossary() : impl_(std::make_unique<Impl>()) {}

Glossary::Glossary(const std::string& tone_markdown_path)
    : impl_(std::make_unique<Impl>()) {
    impl_->LoadFromFile(tone_markdown_path);
}

Glossary::Glossary(Glossary&& other) noexcept = default;
Glossary& Glossary::operator=(Glossary&& other) noexcept = default;
Glossary::~Glossary() = default;

bool Glossary::LoadFromToneFile(const std::string& tone_markdown_path) {
    return impl_->LoadFromFile(tone_markdown_path);
}

const std::string& Glossary::SystemPrompt() const {
    return impl_->system_prompt;
}

const std::string& Glossary::NeutralSystemPrompt() const {
    return impl_->neutral_system_prompt;
}

const std::vector<std::pair<std::string, std::string>>& Glossary::OrderedSpeakerTranslations() const {
    return impl_->ordered_speakers;
}

const std::unordered_map<std::string, std::string>& Glossary::SpeakerTranslationMap() const {
    return impl_->speaker_map;
}

const std::vector<std::pair<std::string, std::string>>& Glossary::OrderedTermTranslations() const {
    return impl_->ordered_terms;
}

std::optional<std::string> Glossary::LookupSpeaker(std::string_view jp_speaker) const {
    const std::string trimmed = TrimUnicode(jp_speaker);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    const auto it = impl_->speaker_map.find(trimmed);
    if (it == impl_->speaker_map.end()) {
        return std::nullopt;
    }
    return it->second;
}

const Glossary& DefaultGlossary() {
    static const Glossary kInstance;
    return kInstance;
}

const std::vector<std::pair<std::string, std::string>>& NameTranslationsOrdered() {
    return DefaultGlossary().OrderedSpeakerTranslations();
}

const std::unordered_map<std::string, std::string>& NameTranslations() {
    return DefaultGlossary().SpeakerTranslationMap();
}

const std::string& DefaultSystemPrompt() {
    return DefaultGlossary().SystemPrompt();
}

}  // namespace oretsuba::translate
