// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned tables: the glossary seeds the translation cache and the system
// prompt is sent verbatim on every batch, so any edit here changes output the
// existing artifacts were built from.  See glossary.h.
#include "glossary.h"

#include <set>
#include <stdexcept>

namespace snt::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"直哉", "Naoya"},
        {"心鈴", "Misuzu"},
        {"真琴", "Makoto"},
        {"藍", "Ai"},
        {"静流", "Shizuru"},
        {"紗希", "Saki"},
        {"放哉", "Housai"},
        {"圭", "Kei"},
        {"麗華", "Reika"},
        {"香奈", "Kana"},
        {"寧", "Nei"},             // 恩田寧, a girl; the reading is Nei, not Yasushi
        {"健一郎", "Ken'ichirou"},
        {"優美", "Yumi"},
        {"桜子", "Sakurako"},
        {"フリッドマン", "Friedman"},
        {"鈴菜", "Suzuna"},
        // The script spells it out: script\05_03_2.ast offset 1216 writes
        // 「氷川ルリヲをローマ字で書くと"hikawa ruriwo"」 and offset 1246 makes
        // the anagram "A Raw Hour Kiwi" from it; 03_mak03_1 offset 1343 says
        // the romanization has twelve letters.  HIKAWARURIWO has twelve and
        // supplies every letter of the anagram; HIKAWARURIO has eleven.
        {"ルリヲ", "Ruriwo"},
        {"坂本", "Sakamoto"},
        {"里奈", "Rina"},
        {"ノノ未", "Nonomi"},
        {"礼次郎", "Reijirou"},
        {"奈津子", "Natsuko"},
        {"章一", "Shouichi"},
        {"片貝", "Katagai"},
        {"稟", "Rin"},
        {"破戒", "Hakai"},
        {"トーマス", "Thomas"},
        {"依瑠", "Eru"},            // 夏目依瑠, VNDB reading
        {"吉田", "Yoshida"},
        {"雫", "Shizuku"},
        {"村田", "Murata"},
        {"明石", "Akashi"},
        {"霧乃", "Kirino"},
        {"心佐夫", "Shinsabu"},
        {"吉沢", "Yoshizawa"},
        {"田村", "Tamura"},
        {"若田", "Wakata"},
        {"静流&藍", "Shizuru & Ai"},
        {"直哉&心鈴", "Naoya & Misuzu"},
        {"使用人", "Servant"},
        {"女性教員", "Female Teacher"},
        // Walk-on roles the two-element plates name.  The letter is the
        // script's own way of telling one extra from another, so it is kept:
        // a row keyed on "Schoolgirl" alone would answer for four of them.
        {"女子生徒A", "Schoolgirl A"},
        {"女子生徒B", "Schoolgirl B"},
        {"女子生徒C", "Schoolgirl C"},
        {"女子生徒D", "Schoolgirl D"},
        // 女生徒 is the script's other spelling of the same role.  It reads
        // differently on purpose: one English string for both would collide.
        {"女生徒A", "Female Student A"},
        {"男子生徒A", "Schoolboy A"},
        {"男子生徒B", "Schoolboy B"},
        {"男子生徒C", "Schoolboy C"},
        {"若者男A", "Young Man A"},
        {"若者男B", "Young Man B"},
        {"若者男C", "Young Man C"},
        {"若者女A", "Young Woman A"},
        {"若者女B", "Young Woman B"},
        {"黒服B", "Man in Black B"},
        {"黒服C", "Man in Black C"},
        {"子供", "Child"},
        {"メイド", "Maid"},
        {"一同", "Everyone"},
        {"マスター", "Master"},
        {"司会者", "MC"},
        {"揚げ物屋", "Fried Foods Vendor"},
        {"美術商", "Art Dealer"},
        {"看護師", "Nurse"},
        {"猫", "Cat"},
        {"美術教師", "Art Teacher"},
        {"ルーアンの校長", "Headmaster of Rouen"},
        {"魚屋", "Fish Vendor"},
        {"老人", "Old Man"},
        {"少年", "Boy"},
        {"審査員A", "Judge A"},
        {"審査員B", "Judge B"},
        {"肉屋", "Butcher"},
        {"店主", "Shopkeeper"},
        {"スタッフ", "Staff"},
        // 女性教員 above renders "Female Teacher".  This second label needs a
        // second string: a name row keyed on one English string cannot answer
        // for two Japanese plates.
        {"女教師", "Woman Teacher"},
        {"画家", "Painter"},
        {"警備員", "Security Guard"},
        {"来客A", "Visitor A"},
        {"来客B", "Visitor B"},
        {"画家A", "Painter A"},
        {"画家B", "Painter B"},
        {"救急隊員", "Paramedic"},
        // The plate Artemis shows for a speaker the scene has not introduced.
        // It is a real plate, so it ends the previous speaker's run instead of
        // letting those lines be filed under whoever spoke last.
        {"？？？", "???"},
    };
    return data;
}

const std::unordered_map<std::string, std::string>& name_translations() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& [jp, en] : name_translations_ordered()) m.emplace(jp, en);
        return m;
    }();
    return map;
}

const std::vector<std::pair<std::string, std::string>>& prompt_name_translations() {
    // Which labels name a person.  Everything else in the speaker table is a
    // role, a walk-on or a two-name plate, and a prompt row for one of those
    // would push a capitalised English word into prose that wants the common
    // noun.
    static const std::set<std::string> people = {
        "直哉", "心鈴", "真琴", "藍",   "静流", "紗希", "放哉", "圭",
        "麗華", "香奈", "寧",   "健一郎", "優美", "桜子", "フリッドマン",
        "鈴菜", "ルリヲ", "坂本", "里奈", "ノノ未", "礼次郎", "奈津子",
        "章一", "片貝", "稟",   "破戒", "トーマス", "依瑠", "吉田", "雫",
        "村田", "明石", "霧乃", "心佐夫", "吉沢", "田村", "若田",
    };
    static const std::vector<std::pair<std::string, std::string>> table = [] {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [jp, en] : name_translations_ordered())
            if (people.count(jp)) out.emplace_back(jp, en);
        if (out.size() != people.size())
            throw std::logic_error(
                "the prompt name selection names a key the speaker table does not have");
        return out;
    }();
    return table;
}

const std::vector<std::pair<std::string, std::string>>& terminology_translations() {
    // Counts beside each row are entries / scripts over the current
    // extraction, measured with analysys/corpus_terminology_survey.py.  The
    // bar for a row is ten entries: below that the rolling context window
    // carries the term and a pin buys nothing.
    static const std::vector<std::pair<std::string, std::string>> table = {
        // ── Works and artworks (the 『…』 class) ──
        // Furigana せっけいかささぎずかびん: 雪景 snow scene, 鵲 magpie,
        // 図 design, 花瓶 vase.  Makoto's central work.
        {"雪景鵲図花瓶",   "The Snowscape Magpie Vase"},   // 116 / 21
        {"カササギ",       "Magpie"},                      // 112 / 14
        {"横たわる櫻",     "The Reclining Sakura"},        //  25 / 11
        {"櫻達の足跡",     "The Footprints of the Sakura"},//  23 / 13
        {"二本の向日葵",   "Two Sunflowers"},              //  21 /  9
        {"櫻日狂想",       "Sakura Day Rhapsody"},         //  20 /  9
        {"火水",           "Fire and Water"},              //  18 / 10
        {"蝶を夢む",       "Dreaming of a Butterfly"},     //  15 /  9
        // The stained-glass companion to 櫻達の足跡; the scene names the two
        // against each other, so they have to read as a pair.
        {"櫻達の色彩の足跡", "The Colored Footprints of the Sakura"},  // 13 / 11
        // ── Family names the cast is referred to by ──
        {"草薙",           "Kusanagi"},                    // 1621 / 202
        {"鳥谷",           "Toritani"},                    //  688 /  88
        {"恩田",           "Onda"},                        //  680 /  90
        {"宮崎",           "Miyazaki"},                    //  619 /  96
        {"本間",           "Honma"},                       //  568 /  80
        {"中村",           "Nakamura"},                    //  442 /  76
        {"夏目",           "Natsume"},                     //  409 / 120
        {"長山",           "Nagayama"},                    //  236 /  29
        {"御桜",           "Misakura"},                    //  191 /  44
        {"氷川",           "Hikawa"},                      //  139 /  35
        {"咲崎",           "Sakizaki"},                    //  135 /  28
        {"川内野",         "Kawachino"},                   //   67 /  21
        {"円木",           "Tsuburagi"},                   //   16 /   7
        // A real Nihonga painter the lectures name; furigana おくむらとぎゅう.
        {"奥村土牛",       "Okumura Togyu"},               //   15 /   1
        // ── Places and institutions ──
        {"弓張",           "Yumihari"},                    //  720 / 144
        {"弓張学園",       "Yumihari Academy"},            //  263 / 103
        {"宮崎絵画学校",   "the Miyazaki School of Painting"},  // 161 / 53
        {"美術部",         "the art club"},                //  154 /  50
        {"美術室",         "the art room"},                //  105 /  40
        {"ルーアン",       "Rouen"},                       //  106 /  35
        {"リモージュ",     "Limoges"},                     //   90 /  17
        // 文部省美術展覧会, the historical exhibition; kept as its own name.
        {"文展",           "the Bunten"},                  //   90 /  27
        {"公募展",         "the Open Exhibition"},         //   73 /  27
        {"キマイラ",       "Chimera"},                     //   60 /  29
        {"ムーア公募展",   "the Moore Open Exhibition"},   //   51 /  19
        // The Limoges porcelain house; the text spells the founder ダビランド.
        {"ダビラント",     "Haviland"},                    //   26 /   8
        {"桜ヶ原",         "Sakuragahara"},                //   24 /  17
        // Romanised rather than translated: the narration discusses the name.
        {"夢浮坂",         "Yume-no-Ukisaka"},             //   14 /  13
        // ── Prizes and groups ──
        {"ムーア",         "Moore"},                       //  287 /  56
        // The prize names are French animals.  ASCII only, per formatting
        // rule 7: epaulard (orca), otarie (sea lion), phoque (seal).
        {"プラティヌ・エポラール", "Platine Epaulard"},     //   53 /  23
        {"ブルバギ",       "Bourbagi"},                    //   22 /   8
        {"エラン・オタリ", "Elan Otarie"},                 //   18 /  10
        {"エラン・フォク", "Elan Phoque"},                 //   12 /   4
        // ── Motifs and recurring vocabulary ──
        {"アリア",         "Aria"},                        //  146 /  25
        {"碧緋",           "Hekihi"},                      //  119 /  19
        {"日本画",         "Nihonga"},                     //   70 /  35
        {"アリア・ホー・インク", "Aria Ho Ink"},            //   70 /  23
        {"ミサゴ",         "osprey"},                      //   45 /  13
        // Ruriwo's pen name is an anagram of HIKAWARURIWO, and the scene turns
        // on reading it as English words.
        {"ア・ロウアワーキウイ", "A Raw Hour Kiwi"},        //   31 /  12
        {"ペトゥロー",     "Petro"},                       //   24 /   3
        // Furigana ゆみへきひ: the Yumihari form of the 碧緋 glaze.
        {"弓碧緋",         "Yumi-hekihi"},                 //   21 /   2
        {"千年桜",         "the thousand-year cherry"},    //   21 /   8
        // Louis Leroy, the critic who coined "Impressionism".
        {"ルロワ",         "Leroy"},                       //   20 /   4
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& superseded_renderings() {
    // The May pilot table spells 鳥谷 "Toriya"; D8 pins Toritani.  Every row
    // is rewritten when the table is built, so a rebuild repairs the spelling
    // without re-buying the line.
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"鳥谷", "Toriya"},
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& superseded_name_renderings() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"寧",     "Yasushi"},         // 恩田寧 reads Nei; VNDB v20431
        {"女教師", "Female Teacher"},  // 女性教員 keeps that English
        // The May pilot spelled 氷川ルリヲ "Rurio"; the anagram scene quoted in
        // name_translations_ordered() needs the twelve letters of Ruriwo.
        {"ルリヲ", "Rurio"},
    };
    return table;
}

namespace {

// The prompt is PROMPT_HEAD + the rendered names + the rendered terminology +
// PROMPT_TAIL, so the pins the builder repairs and the pins the model is given
// are one table.
const char* const PROMPT_HEAD =
    R"__(You are a professional Japanese-to-English translator specializing in literary visual novels.

Current Project: "Sakura no Toki" (サクラノ刻 -- "The Time of Cherry Blossoms") by SCA-DI / Makura.
Setting: Direct sequel to "Sakura no Uta", continuing the story of Kusanagi Naoya years after his graduation.  Setting blends contemporary art-world drama, philosophy, and the lingering echoes of the Sakura no Uta cast.  Prose is dense and literary -- think Mishima or Kawabata cadence: long sentences, metaphor, layered allusion (Wittgenstein, Kant, Cezanne, Picasso, Rilke, classical Nihonga painters).  Translate with care for rhythm and register.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes -- this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<translation_guidelines>
- SPEAKER TAGS: Each line is prefixed with `[Speaker]` -- the character speaking, or `[NARRATION]` / `[NAME]` (a speaker label, e.g. "静流" -> "Shizuru") / `[TITLE]` (chapter title) / `[???]` (the scene has not introduced this speaker yet -- keep the line free of anything that would name them). Use the tag to infer voice, gender, register; do NOT include the tag in your output.
- VOICE: First-person POV is usually Naoya (直哉), the protagonist. Other POV characters are introduced explicitly -- infer from context.
- TONE: Write English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Japanese omits subjects; infer from context. Do NOT default to "he" -- the speaker might be female, the addressee might be third-person.
- HONORIFICS: Carry over -san, -kun, -chan, -senpai, -sensei when they fit naturally; drop when awkward.
- ART / PHILOSOPHY VOCABULARY: Steeped in references to Wittgenstein, Kant, Cezanne, Picasso, Rilke, Okumura Togyu, Hayami Gyoshu, Ito Jakuchu, etc. Preserve proper names exactly; do not "domesticate" them.
- RUBY (furigana): The engine sometimes splits a kanji compound and its reading across two adjacent strings (e.g. line A = "「dialog start ", line B = "kanji_compound"). Translate each fragment as you see it -- the engine reassembles them at runtime.
- LENGTH: Never compress. English that runs longer than the Japanese is expected and is normal; where a line has to fit a box, that is solved in the engine and never in the translation. Do not drop a clause, a name, an honorific or an emotional beat to make a line shorter.
</translation_guidelines>

<character_dossier>
Every speaker with more than a hundred lines, sex from VNDB (v20431) and from the text.
Family names are pinned in <terminology> below; use those spellings.

Returning cast (from Sakura no Uta):
- Naoya (直哉, 草薙直哉) -- Protagonist, painter, now a lecturer at Yumihari. **Male**. Default narrator. Use "I/me/my" for his own lines and he/him when others speak about him.
- Kei (圭, 夏目圭 Natsume Kei) -- Naoya's friend since junior high. **Male** (feminine-looking / cross-dressing "trap"). Use he/him. The label is 圭; 景 is not a person.
- Ai (藍, 夏目藍 Natsume Ai) -- Kei's and Shizuku's older sister, a teacher. **Female**, although she talks rough (お前, だ, ぞ). Use she/her.
- Ken'ichirou (健一郎) -- Naoya's late father, famous painter. Use he/him.
- Makoto (真琴, 鳥谷真琴 Toritani Makoto) -- Heroine, potter. Use she/her.
- Saki (紗希, 鳥谷紗希 Toritani Saki) -- Makoto's mother, Yumihari's headmaster in the first game, Nakamura Shouichi's ex-wife. **Female**, although she smokes and speaks plainly. Use she/her.
- Reika (麗華, 中村麗華, once 本間麗華) -- Heroine. Use she/her.
- Rin (稟, 御桜稟 Misakura Rin) -- Naoya's childhood friend, not a relative. Sometimes "Hakuki" (伯奇), her alt identity. Use she/her.
- Yumi (優美, 川内野優美), Rina (里奈, 氷川里奈), Shizuku (雫, 夏目雫), Kana (香奈, 長山香奈), Sakurako (桜子, 咲崎桜子), Suzuna (鈴菜, 川内野鈴菜), Ruriwo (ルリヲ, 氷川ルリヲ, a girl despite the name), Nonomi (ノノ未, 柊ノノ未), Natsuko (奈津子, 栗山奈津子), Kirino (霧乃, 恩田霧乃) -- women. Use she/her.
- Akashi (明石, 明石亘 Akashi Wataru), Thomas (トーマス), Friedman (フリッドマン, Marty Friedman, a curator), Nakamura Shouichi (章一), Katagai (片貝) -- men. Use he/him.

New in Sakura no Toki:
- Shizuru (静流, 鳥谷静流 Toritani Shizuru) -- Runs the cafe Chimera, Makoto's cousin. **Female**. Use she/her.
- Misuzu (心鈴, 本間心鈴 Honma Misuzu) -- Heroine. **Female**. Use she/her. Her twin brother Shinsabu (心佐夫) is male.
- Nei (寧, 恩田寧 Onda Nei) -- A first-year art student. **Female** even when a teacher calls her 寧くん. Use she/her. The name reads Nei, never Yasushi.
- Eru (依瑠, 夏目依瑠 Natsume Eru) -- A girl. Use she/her.
- Housai (放哉, 恩田放哉 Onda Housai) -- Male. Use he/him.
- Reijirou (礼次郎, 本間礼次郎), Hakai (破戒, 宮崎破戒), Sakamoto (坂本, 坂本昭三, a lecturer), Murata (村田, 村田清彦), Yoshida (吉田, 吉田茂樹) -- men. Use he/him.
- Yoshizawa (吉沢, 吉沢緑寺) -- A master potter addressed as 先生; sex not recorded on VNDB and not settled by the text. Avoid a gendered pronoun unless the lines around it show one.
</character_dossier>
)__";

const char* const PROMPT_TAIL =
    R"__(
<formatting_rules>
1. Output ONLY the English translation for each numbered line in <lines_to_translate>.
2. Maintain the exact line numbers in your output.
3. NO [SPEAKER] tags, NO commentary, NO meta notes -- just the numbered translations.
4. Preserve in-line control codes EXACTLY:
   - Bare \n is a SOFT line break (textbox wrap). Place at a natural English clause break.
   - 　 (U+3000 fullwidth space) at line starts is paragraph indent -- preserve it.
5. Convert Japanese structural punctuation:
   - 「 / 」 -> opening / closing double-quotes "
   - 『 / 』 -> single-quotes '
   - ・ between names -> hyphen or " . " spacing
6. NEVER combine, merge, or skip lines.
7. Use only ASCII characters -- straight quotes "...", double-hyphen -- in place of em dash.
8. If a line is empty or just symbols (e.g. "「……」"), translate it literally (""......"").
9. SHORT KANJI fragments (often 2-4 chars without context, sandwiched between ruby tags):
   These are usually proper nouns -- a person/place/artwork name. Keep the romanization
   from the glossary, or transliterate as a name (e.g. 円木 -> "Tsuburagi", 碧緋 -> "Hekihi").
   Do NOT translate them as common nouns.
</formatting_rules>)__";

std::string render_names() {
    std::string s =
        "\n<character_names>\n"
        "One romanisation per person, for the whole project. Use it wherever "
        "the Japanese name appears -- as a speaker, in dialogue, in narration "
        "-- and never re-spell one you have already seen.\n"
        "Several of these names are also ordinary words: 藍 is the colour "
        "indigo, 寧 lives inside 丁寧 \"polite\", and 雫 is a drop of water. "
        "Use the name only where a person is meant; elsewhere translate the "
        "ordinary word.\n";
    for (const auto& [jp, en] : prompt_name_translations())
        s += "- " + jp + " = " + en + "\n";
    s += "</character_names>\n";
    return s;
}

std::string render_terminology() {
    std::string s =
        "\n<terminology>\n"
        "These renderings are FIXED for the whole project. Wherever the Japanese "
        "on the left appears -- as a title, as a name, or inside a longer "
        "sentence -- use the English on the right, adapting only the surrounding "
        "grammar (articles, possessives, plurals). Do not re-coin a term you "
        "think reads better: a second spelling of the same title ships alongside "
        "the first.\n";
    for (const auto& [jp, en] : terminology_translations())
        s += "- " + jp + " = " + en + "\n";
    s +=
        "Note on the two cherry spellings: 櫻 is the archaic form and marks the "
        "title/artwork register (render it \"Sakura\"), while the ordinary 桜 is "
        "the flower or the tree (\"cherry blossom\", \"cherry tree\").\n"
        "</terminology>\n";
    return s;
}

}  // namespace

const std::string& system_prompt() {
    static const std::string prompt =
        std::string(PROMPT_HEAD) + render_names() + render_terminology() + PROMPT_TAIL;
    return prompt;
}

}  // namespace snt::translate
