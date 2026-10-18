// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "glossary.h"

#include <set>
#include <stdexcept>

namespace snu::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        // --- Primary cast (in frequency order) ---
        {"直哉",       "Naoya"},          // Kusanagi Naoya — protagonist
        {"稟",         "Rin"},            // Naoya's childhood friend
        {"真琴",       "Makoto"},         // heroine
        {"里奈",       "Rina"},           // heroine
        {"雫",         "Shizuku"},        // heroine
        {"優美",       "Yumi"},           // senpai-type heroine (Amakasu Yumi)
        {"藍",         "Ai"},
        {"圭",         "Kei"},            // Natsume Kei (male, feminine-looking)
        {"健一郎",     "Ken'ichirou"},    // Naoya's late father
        {"香奈",       "Kana"},
        {"明石",       "Akashi"},         // senpai (Nagamori Akashi)
        // Furigana-confirmed reading: <r すい>吹</r>.
        {"吹",         "Sui"},
        {"校長",       "Headmaster"},
        {"桜子",       "Sakurako"},
        {"？？？",     "???"},
        {"葛",         "Kuzu"},
        {"フリッドマン", "Friedman"},
        {"水菜",       "Mizuna"},
        {"中村章一",   "Nakamura Shouichi"},
        {"小牧",       "Komaki"},
        {"若田",       "Wakata"},
        {"小沙智",     "Kosachi"},
        {"ルリヲ",     "Rurio"},
        {"トーマス",   "Thomas"},
        {"義貞",       "Yoshisada"},
        {"片貝",       "Katagai"},
        {"鈴菜",       "Suzuna"},
        {"紗希",       "Saki"},
        {"琴子",       "Kotoko"},
        {"ノノ未",     "Nonomi"},
        {"御桜父親",   "Misakura's Father"},
        {"伯奇",       "Hakuki"},         // Rin's alternate identity
        {"奈津子",     "Natsuko"},
        // Furigana-confirmed reading: <r ねい>寧</r>.
        {"寧",         "Nei"},
        {"霧乃",       "Kirino"},
        {"教師",       "Teacher"},
        {"村田",       "Murata"},
        {"神父",       "Father (Priest)"},
        {"坂本",       "Sakamoto"},
        {"丘沢",       "Okazawa"},
        {"村山理事",   "Director Murayama"},
        {"バイトの子", "Part-timer"},
        {"店主",       "Shopkeeper"},
        {"報道陣",     "Press"},
        {"御桜母親",   "Misakura's Mother"},
        {"方相氏",     "Hososhi"},
        {"麗華",       "Reika"},
        {"女弁護士",   "Female Lawyer"},
        // Nickname, not a description -- the man protests being called it on screen.
        {"おやっさん", "Pops"},
        // Furigana-confirmed reading: <r いえつな>宇綱</r>.
        {"宇綱",       "Ietsuna"},
        {"女性店主",   "Female Shopkeeper"},
        {"副会長",     "Vice-Chairman"},
        {"末竹",       "Suetake"},
        {"一宮",       "Ichinomiya"},
        {"琢磨",       "Takuma"},
        {"司会者",     "MC"},
        // Reception nurse; keeps the plain word -- the ward nurse below is disambiguated.
        {"看護婦",     "Nurse"},
        {"一同",       "Everyone"},
        {"生徒会長",   "Student Council President"},
        {"井上理事",   "Director Inoue"},
        {"中里",       "Nakazato"},
        {"東浪見",     "Tonami"},
        {"女司会者",   "Female MC"},
        // The plate withholds his name on purpose; stays the description it gives.
        {"中年",       "Middle-aged Man"},
        {"うなぎ",     "Eel-vendor"},
        {"男司会者",   "Male MC"},
        {"配達人",     "Delivery Person"},
        {"管理人",     "Caretaker"},
        {"咲崎",       "Sakizaki"},
        {"善のトーマス", "Good Thomas"},
        {"家臣一",     "Retainer 1"},
        {"男性Ｃ",     "Man C"},
        {"管財人",     "Trustee"},
        {"オランピア？", "Olympia?"},
        {"アナウンス", "Announcement"},
        {"少女",       "Girl"},
        {"店員",       "Clerk"},
        {"関係者",     "Person Concerned"},
        {"教頭",       "Vice Principal"},
        {"関係者Ａ",   "Person Concerned A"},
        {"関係者Ｂ",   "Person Concerned B"},
        // The museum director; narration confirms 老館長.
        {"老人",       "Old Man"},
        {"会長",       "Chairman"},
        {"愛のトーマス", "Loving Thomas"},
        {"配達員",     "Delivery Worker"},
        {"キャスター", "News Caster"},
        // Ward nurse, disambiguated from the reception nurse (看護婦) above.
        {"看護師",     "Ward Nurse"},
        {"車掌",       "Conductor"},
        {"中村",       "Nakamura"},
        // Narration calls him スーツ姿のおじさん; trade stays out of the label.
        {"中年男性",   "Man in a Suit"},
        {"男",         "Man"},
        {"ヤス",       "Yasu"},
        // ── Mob speakers — gender-coded school students ──
        {"女子校生",     "Schoolgirl"},
        {"女子校生Ａ",   "Schoolgirl A"},
        {"女子校生Ｂ",   "Schoolgirl B"},
        {"女子校生Ｃ",   "Schoolgirl C"},
        {"女子校生Ｄ",   "Schoolgirl D"},
        {"女子校生Ｅ",   "Schoolgirl E"},
        {"女子校生達",   "Schoolgirls"},
        {"男子校生Ａ",   "Schoolboy A"},
        {"男子校生Ｂ",   "Schoolboy B"},
        {"男子校生Ｃ",   "Schoolboy C"},
        {"女子学生Ａ",   "Female Student A"},
        {"女子学生Ｂ",   "Female Student B"},
        {"男子生徒Ａ",   "Male Student A"},
        {"男子生徒Ｂ",   "Male Student B"},
        {"男子生徒Ｃ",   "Male Student C"},
        {"男子生徒Ｄ",   "Male Student D"},
        {"生徒Ａ",       "Student A"},
        {"生徒Ｃ",       "Student C"},
        {"教師Ａ",       "Teacher A"},
        {"教師Ｂ",       "Teacher B"},
        // ── Mob speakers — adults / customers / others ──
        {"客Ａ",         "Customer A"},
        {"客Ｂ",         "Customer B"},
        {"客Ｃ",         "Customer C"},
        {"常連客Ａ",     "Regular A"},
        {"常連客Ｂ",     "Regular B"},
        {"常連客Ｃ",     "Regular C"},
        {"常連客Ｄ",     "Regular D"},
        {"黒服Ａ",       "Black-suit A"},
        {"黒服Ｂ",       "Black-suit B"},
        {"黒服Ａ＆黒服Ｂ", "Black-suits A & B"},
        {"記者Ａ",       "Reporter A"},
        {"記者Ｂ",       "Reporter B"},
        {"記者Ｃ",       "Reporter C"},
        {"サラリーマンＡ", "Salaryman A"},
        {"サラリーマンＢ", "Salaryman B"},
        {"洋服屋店員Ａ", "Clothing Shop Clerk A"},
        {"洋服屋店員Ｂ", "Clothing Shop Clerk B"},
        {"おっさんＡ",   "Old Geezer A"},
        {"おっさんＢ",   "Old Geezer B"},
        {"子供Ａ",       "Child A"},
        {"子供Ｂ",       "Child B"},
        {"家臣二",       "Retainer 2"},
        {"男子一同",     "All the Boys"},
        {"女子一同",     "All the Girls"},
        // ── Combined / dual speakers ──
        {"稟＆直哉",         "Rin & Naoya"},
        {"里奈＆優美",       "Rina & Yumi"},
        {"真琴＆里奈＆優美", "Makoto & Rina & Yumi"},
        {"直哉＆吹",         "Naoya & Sui"},
        {"稟＆真琴＆直哉",   "Rin & Makoto & Naoya"},
        {"真琴＆直哉",       "Makoto & Naoya"},
        {"桜子＆奈津子",     "Sakurako & Natsuko"},
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& extra_name_translations() {
    // Names from the cast list that never appear in the scripts.
    // 景 is deliberately excluded -- it only occurs inside common nouns (風景, 光景, …), never alone.
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"美和", "Miwa"},
        {"冬馬", "Touma"},
        {"篠田", "Shinoda"},
        {"弓子", "Yumiko"},
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& terminology_translations() {
    // Counts are entries / scripts. Inclusion bar: 2+ scripts or 5+ entries.
    static const std::vector<std::pair<std::string, std::string>> table = {
        // ── Works and artworks (the 『…』 class -- the proven drift case) ──
        {"横たわる櫻",       "The Reclining Sakura"},      //   44 / 13
        {"櫻達の足跡",       "The Footprints of the Sakura"},  // 72 / 17
        {"櫻日狂想",         "Sakura Day Rhapsody"},       //   26 / 10
        {"櫻七相図",         "The Seven Aspects of the Sakura"},  // 25 / 10
        {"九相図",           "the Nine Aspects of Death"}, //    8 /  1
        {"つきのうさぎ",     "The Moon Rabbit"},           //   16 /  6
        {"四つの星",         "The Four Stars"},            //   19 /  5
        {"オランピア",       "Olympia"},                   //  120 / 12
        {"カササギ",         "Magpie"},                    //   79 /  6
        {"春と修羅",         "Spring and Asura"},          //   10 /  4
        {"春日狂想",         "Spring Day Rhapsody"},       //    7 /  3
        {"在りし日の歌",     "Songs of Bygone Days"},      //    4 /  3
        {"銀河鉄道の夜",     "Night on the Galactic Railroad"},  //  4 /  2
        {"幸福な王子",       "The Happy Prince"},          //   17 /  3
        // ── Family names the cast is referred to by ──
        {"草薙",             "Kusanagi"},                  // 1945 / 106
        // Reading confirmed by トーマス saying トリタニ twice on screen.
        {"鳥谷",             "Toritani"},                  // 1550 / 68
        {"夏目",             "Natsume"},                   //  427 / 67
        {"氷川",             "Hikawa"},                    //  316 / 36
        {"川内野",           "Kawachino"},                 //  291 / 43
        {"御桜",             "Misakura"},                  //  158 / 37
        {"長山",             "Nagayama"},                  //  167 / 20
        // Furigana-confirmed reading: <r くすか>葛佳</r>.
        {"葛佳",             "Kusuka"},                    //   16 /  6
        // ── Places and institutions ──
        {"弓張",             "Yumihari"},                  //  427 / 82
        {"弓張学園",         "Yumihari Academy"},          //  176 / 59
        {"美術部",           "the art club"},              //  232 / 55
        {"美術室",           "the art room"},              //  135 / 39
        // Romanised, not translated -- the narration discusses the name's origin.
        {"夢浮坂",           "Yume-no-Ukisaka"},           //   50 / 29
        {"ムーア公募展",     "the Moore Open Exhibition"},  //   27 /  6
        // Short form of ムーア公募展; capitalised to read as the same institution.
        {"公募展",           "the Open Exhibition"},       //   41 / 11
        // Exhibition's top prize; ASCII-only (no acute accent per formatting rule 7).
        {"プラティヌ・エポラール", "Platine Epaulard"},      //   27 / 10
        {"ブルバギ",         "Bourbagi"},                  //   42 /  5
        // ── Recurring motifs ──
        {"キマイラ",         "Chimera"},                   //  115 / 21
        {"ムーア",           "Moore"},                     //  187 / 43
        // Ordinary 桜 (flower register), not archaic 櫻 (artwork register).
        {"千年桜",           "the thousand-year cherry"},  //  134 / 28
        // Cupressus (Italian cypress), not 檜.
        {"糸杉",             "cypress"},                   //   97 / 13
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& superseded_renderings() {
    // Old renderings rewritten at build time; replacement comes from terminology_translations().
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"横たわる櫻", "The Prostrate Cherry"},
        {"櫻七相図",   "The Seven Aspects of the Cherry"},
        {"鳥谷",       "Toriya"},
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& superseded_name_renderings() {
    // Old name renderings rewritten at build time; replacement comes from the name tables.
    // First three: furigana contradicted the old reading. Last three: shared an English label with another row.
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"吹",       "Fuki"},           // <r すい>吹</r>
        {"寧",       "Yasushi"},        // <r ねい>寧</r>
        {"宇綱",     "Utsuna"},         // <r いえつな>宇綱</r>
        {"おやっさん", "Old Man"},       // 老人 has it
        {"看護師",   "Nurse"},          // 看護婦 has it
        {"中年男性", "Middle-aged Man"},  // 中年 has it
    };
    return table;
}

const std::unordered_map<std::string, std::string>& name_translations() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& [jp, en] : name_translations_ordered())
            m.emplace(jp, en);
        return m;
    }();
    return map;
}

const std::vector<std::pair<std::string, std::string>>& prompt_name_translations() {
    // Person keys only; role/mob labels excluded so they stay lowercase in prose.
    // English and order are read from name_translations_ordered().
    static const std::set<std::string> people = {
        "直哉", "稟", "真琴", "里奈", "雫", "優美", "藍", "圭", "健一郎",
        "香奈", "明石", "吹", "桜子", "葛", "フリッドマン", "水菜",
        "中村章一", "小牧", "若田", "小沙智", "ルリヲ", "トーマス", "義貞",
        "片貝", "鈴菜", "紗希", "琴子", "ノノ未", "伯奇", "奈津子", "寧",
        "霧乃", "村田", "坂本", "丘沢", "方相氏", "麗華", "宇綱", "末竹",
        "一宮", "琢磨", "中里", "東浪見", "咲崎", "中村", "ヤス",
    };
    static const std::vector<std::pair<std::string, std::string>> table = [] {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [jp, en] : name_translations_ordered())
            if (people.count(jp)) out.emplace_back(jp, en);
        if (out.size() != people.size())
            throw std::logic_error(
                "prompt name selection names a key the speaker table does not have");
        return out;
    }();
    return table;
}

namespace {

// Prompt = PROMPT_HEAD + rendered names + rendered terminology + PROMPT_TAIL.
const char* const PROMPT_HEAD = R"__(You are a professional Japanese-to-English translator specializing in literary visual novels.

Current Project: "Sakura no Uta" by SCA-DI (Makura).
Setting: A coming-of-age literary VN built around art, philosophy, family, and the search for beauty. The prose is dense and literary -- think Mishima or Kawabata cadence: long sentences, metaphor, layered allusion. Translate with care for rhythm and register.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes -- this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<translation_guidelines>
- SPEAKER TAGS: Each line is prefixed with `[Speaker]` -- the character speaking, or `[NARRATION]` (no attributed speaker) / `[NAME]` (a speaker label string used by the engine's name window — translate as a name romanization). Use the tag to infer voice, gender, register; do NOT include the tag in your output.
- VOICE: First-person POV is usually Naoya (直哉), the protagonist. Other POV characters are introduced explicitly -- infer from context.
- TONE: Write English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Japanese omits subjects; infer from context. Do NOT default to "he" -- the speaker might be female, the addressee might be third-person, etc.
- HONORIFICS: Carry over -san, -kun, -chan, -senpai, -sensei, etc. when they fit naturally; drop them when they would feel awkward.
- RUBY (furigana): the engine writes it as <r かな>漢字</r> -- an <r> tag whose attribute is the reading and whose body is the base word. Translate the BASE WORD (the tag body), drop the reading, and emit no tag: <r うすくれない>薄紅</r> becomes "pale crimson". A few lines are marked up wrongly -- a missing r, a fullwidth ｒ, a closing tag standing in for an opener, a bare </> for the close. Read them the same way: the reading is the kana, the word is the rest. Your output carries NO tag and NO piece of one: no <r, no </r>, no </>, no leftover < or >.
</translation_guidelines>

<character_dossier>
Every speaker with more than a hundred lines, sex from VNDB (v562) and from the text:
- Naoya (直哉, 草薙直哉) -- Protagonist. Male, painter's son. Default narrator. Use "I/me/my".
- Rin (稟, 御桜稟 Misakura Rin) -- Naoya's childhood friend, not a relative. Sometimes called "Hakuki" (伯奇), her alt identity. Use she/her.
- Ken'ichirou (健一郎) -- Naoya's late father, famous painter. Use he/him.
- Mizuna (水菜, 草薙水菜, born 中村水菜) -- Naoya's late mother, Ken'ichirou's wife. Use she/her.
- Kei (圭, 夏目圭 Natsume Kei) -- Naoya's friend since junior high. **Male** (feminine-looking / cross-dressing "trap"). Use he/him.
- Ai (藍, 夏目藍 Natsume Ai) -- Kei's and Shizuku's older sister, Naoya's homeroom teacher. **Female**, although she talks rough (お前, だ, ぞ). Use she/her.
- Shizuku (雫, 夏目雫) -- Kei's and Ai's younger sister, a model and actress under the stage name 草薙葛佳 Kusanagi Kusuka. Use she/her.
- Makoto (真琴, 鳥谷真琴 Toritani Makoto) -- Art club president, usually called 鳥谷 Toritani. Use she/her.
- Headmaster (校長, 鳥谷校長, 鳥谷紗希 Toritani Saki) -- Yumihari Academy's headmaster, Makoto's mother, Nakamura Shouichi's ex-wife. **Female**, although she smokes and speaks plainly. Use she/her.
- Yumi (優美, 川内野優美 Kawachino Yuumi) -- Heroine, art club freshman. Use she/her.
- Rina (里奈, 氷川里奈 Hikawa Rina) -- Heroine, underclassman. Use she/her.
- Kana (香奈, 長山香奈 Nagayama Kana) -- Rival painter. Use she/her.
- Sui (吹) -- A girl. Use she/her.
- Kuzu (葛) -- A girl, a Hakuki born into the Nakamura family. Use she/her.
- Akashi (明石, 明石亘 Akashi Wataru) -- Senpai, former art club president. Use he/him. His younger sisters Komaki (小牧) and Kosachi (小沙智), the 明石姉妹, are girls: she/her.
- Thomas (トーマス) -- Art club member. Use he/him.
- Friedman (フリッドマン, Marty Friedman) -- Curator of the Moore Foundation. Use he/him.
- Nakamura Shouichi (中村章一) -- Head of the Nakamura family, the headmaster's ex-husband. Use he/him.
- Yoshisada (義貞), Wakata (若田), Katagai (片貝), the priest (神父) -- men. Use he/him.
- Sakurako (桜子, 咲崎桜子, addressed as 咲崎くん), Ruriwo (ルリヲ), Suzuna (鈴菜), Kotoko (琴子), Nonomi (ノノ未), Natsuko (奈津子), Kirino (霧乃), Nei (寧), Reika (麗華), Suetake (末竹, a lawyer) -- women. Use she/her.
</character_dossier>
)__";

const char* const PROMPT_TAIL = R"__(
<formatting_rules>
1. Output ONLY the English translation for each numbered line in <lines_to_translate>.
2. Maintain the exact line numbers in your output.
3. NO [SPEAKER] tags, NO commentary, NO meta notes -- just the numbered translations.
4. Preserve \\n exactly as written -- they are in-engine line breaks. Keep every \\n the source has, placed at the nearest English clause break. ONE physical line per numbered entry; a real line break inside an entry destroys it.
5. Preserve Japanese punctuation that has no natural English equivalent ONLY when it is structural:
   - Dialogue brackets 「 / 」 -> opening/closing quote marks " / "
   - Emphasis brackets 『 / 』 -> single-quotes ' / '
   - Heavy-period ・ between names ("A・B") -> keep as ASCII " . " spacing or join with hyphen.
6. NEVER combine, merge, or skip lines.
7. Use only ASCII characters. No smart quotes, em dashes, or accented letters. Use straight quotes "..." and double-hyphen -- in place of em dash.
8. If a line is empty or just symbols (e.g. "「……」"), translate it as the equivalent (""......"").
</formatting_rules>)__";

std::string render_names() {
    std::string s =
        "\n<character_names>\n"
        "One romanisation per person, for the whole project. Use it wherever the "
        "Japanese name appears -- as a speaker, in dialogue, in narration -- and "
        "never re-spell one you have already seen.\n"
        "Several of these names are also ordinary words: 吹 is \"to blow\", 藍 is "
        "\"indigo\", 葛 is the kudzu vine and lives inside 葛藤 \"inner conflict\", "
        "寧 lives inside 丁寧 \"polite\". Use the name only where a person is "
        "meant; elsewhere translate the ordinary word.\n";
    for (const auto& [jp, en] : prompt_name_translations())
        s += "- " + jp + " = " + en + "\n";
    s += "</character_names>\n";
    return s;
}

std::string render_terminology() {
    std::string s =
        "\n<terminology>\n"
        "These renderings are FIXED for the whole project. Whenever the Japanese "
        "on the left appears -- as a title, as a name, or inside a longer sentence -- "
        "use the English on the right, adapting only the surrounding grammar "
        "(articles, possessives, plurals). Do not re-coin a term you think reads "
        "better: a second spelling of the same title ships alongside the first.\n";
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

}  // namespace snu::translate
