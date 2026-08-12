// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Character glossary and system prompt.  Editing a name here changes the
// translation of every line that speaker owns AND the key order of the
// translation cache, so an edit means a cache rebuild -- add new names at the
// END of the table unless you intend that.
#include "glossary.h"

namespace exc::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"少女", "Girl"},
        {"少年", "Boy"},
        {"？？？", "???"},
        {"夢美", "Yumemi"},
        {"アゲハ", "Ageha"},
        {"ユーリア", "Yuria"},
        {"サユリ", "Sayuri"},
        {"杏子", "Kyouko"},
        {"美弥香", "Miyaka"},
        {"唯", "Yui"},
        {"綾佳", "Ayaka"},
        {"遥", "Haruka"},
        {"煉悟", "Rengo"},
        {"西", "Nishi"},
        {"優斗", "Yuuto"},
        {"史郎", "Shirou"},
        {"レン", "Ren"},
        {"煉獄", "Rengoku"},
        {"和泉", "Izumi"},
        {"和泉万夜", "Izumi Manya"},
        {"メタ", "Meta"},
        {"ばぐねこ", "Bagneko"},
        {"優斗の友人", "Yuuto's Friend"},
        {"男の声", "Man's Voice"},
        {"客", "Customer"},
        {"三人", "Three of Them"},
        {"列席者達", "Attendees"},
        {"客Ａ", "Customer A"},
        {"客Ｂ", "Customer B"},
        {"客Ｃ", "Customer C"},
        {"客Ｄ", "Customer D"},
        {"客Ｅ", "Customer E"},
        {"客Ｆ", "Customer F"},
        {"客Ｇ", "Customer G"},
        {"客Ｈ", "Customer H"},
        {"男Ａ", "Man A"},
        {"男Ｂ", "Man B"},
        {"男Ｃ", "Man C"},
        {"男Ｄ", "Man D"},
        {"男Ｅ", "Man E"},
        {"男Ｆ", "Man F"},
        {"男Ｇ", "Man G"},
        {"男Ｈ", "Man H"},
        {"男Ｉ", "Man I"},
        {"男Ｊ", "Man J"},
        {"男Ｋ", "Man K"},
        {"男Ｌ", "Man L"},
        {"列席者Ａ", "Attendee A"},
        {"列席者Ｂ", "Attendee B"},
        {"列席者Ｃ", "Attendee C"},
        {"列席者Ｄ", "Attendee D"},
        {"列席者Ｅ", "Attendee E"},
        {"列席者Ｆ", "Attendee F"},
        {"列席者Ｇ", "Attendee G"},
        {"列席者Ｈ", "Attendee H"},
        {"列席者Ｉ", "Attendee I"},
        {"列席者Ｊ", "Attendee J"},
        {"列席者Ｋ", "Attendee K"},
        {"列席者Ｌ", "Attendee L"},
        {"列席者Ｍ", "Attendee M"},
        {"列席者Ｎ", "Attendee N"},
        {"列席者Ｏ", "Attendee O"},
        {"列席者Ｐ", "Attendee P"},
        {"夢美・アゲハ", "Yumemi & Ageha"},
        {"夢美・杏子", "Yumemi & Kyouko"},
        {"夢美・唯", "Yumemi & Yui"},
        {"夢美・綾佳", "Yumemi & Ayaka"},
        {"夢美・遥", "Yumemi & Haruka"},
        {"美弥香・唯", "Miyaka & Yui"},
        {"綾佳・遥", "Ayaka & Haruka"},
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

const std::vector<std::string>& story_order() {
    static const std::vector<std::string> data = {
        "01.spt",
        "02.spt",
        "03.spt",
        "04.spt",
        "05.spt",
        "06.spt",
        "062.spt",
        "07.spt",
        "08.spt",
        "backscript.spt",
        "omake.spt",
        "_exgame.spt",
        "_meta.spt",
        "_exban.spt",
        "_staff.spt",
        "sys.spt",
    };
    return data;
}

const char* const SYSTEM_PROMPT =
    R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "EXTRAVAGANZA ~Mushi Mederu Shoujo~" (蟲愛でる少女 / Insect-Loving Girl) by BLACKCyc.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes — this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<story_synopsis>
A dark supernatural horror VN set in the coastal town of Masuzugasaki. Bug Users (蟲使い) wield parasitic insects called 蟲 (Bugs) as living weapons. The story spans 8 routes across multiple character perspectives — Yumemi (a cheerful girl), Ageha (a mysterious Bug User), Rengo (a hardened fighter), Miyaka (a gentle scientist), and others. Themes include horror, mystery, romance, body horror, and supernatural warfare.
</story_synopsis>

<translation_guidelines>
- PERSPECTIVE: The game switches between multiple POV characters across routes. Infer the current perspective from the [SPEAKER] tags and context.
- TONE: Write English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Japanese often omits subjects. Infer pronouns from speaker identity and context.
- HONORIFICS: Carry over Japanese honorifics (-san, -kun, -chan, -sensei, -sama) when they fit naturally.
- TERMINOLOGY: 蟲 = Bug (parasitic insects used as living weapons), 蟲使い = Bug User, 苗床 = Seedbed (a human host body for Bugs).
</translation_guidelines>

<character_dossier>
Main Characters:
- Yumemi (夢美): Cheerful girl, central protagonist. Use she/her.
- Ageha (アゲハ): Mysterious Bug User with a dark past. Use she/her.
- Rengo (煉悟): Male Bug User, hardened fighter. Use he/him.
- Miyaka (美弥香): Gentle scientist / researcher. Use she/her.
- Yuria (ユーリア): Foreign woman. Use she/her.
- Sayuri (サユリ): Female character. Use she/her.
- Kyouko (杏子): Female character. Use she/her.
- Yui (唯): Female character. Use she/her.
- Ayaka (綾佳): Female character. Use she/her.
- Haruka (遥): Female character. Use she/her.

Supporting Cast:
- Nishi / Dr. West (西 / ウエスト / 西正人 Nishi Masahito): Mad scientist. Same character — render whichever label the script uses. Use he/him.
- Ren (蓮): Male. Use he/him.
- Rin (凛): Female. Use she/her.
- Kaede (楓): Female. Use she/her.

Common Speaker Labels:
- 名前なし = unnamed narrator (narration)
- ？？？ = unknown speaker
- 蟲 = Bug (the creature speaking)
- 声 = Voice (disembodied)
- 少女 = Girl, 少年 = Boy, 男 = Man, 女 = Woman
- 老人 = Old man, 店員 = Clerk
</character_dossier>

<name_reference>
Japanese → English: 夢美=Yumemi, アゲハ=Ageha, ユーリア=Yuria, サユリ=Sayuri,
杏子=Kyouko, 美弥香=Miyaka, 唯=Yui, 綾佳=Ayaka, 遥=Haruka, 煉悟=Rengo,
蓮=Ren, 西=Nishi, 凛=Rin, 楓=Kaede, ウエスト=West, Ｄｒ．Ｗｅｓｔ=Dr. West,
少女=Girl, 少年=Boy, 男=Man, 女=Woman, 男性=Man, 女性=Woman,
老人=Old man, 店員=Clerk, 蟲=Bug, 声=Voice
</name_reference>

<locations>
真鈴ヶ崎 = Masuzugasaki (the coastal town where the story takes place)
カフェ・ＡｎｚＵ = Cafe AnzU
Ｄｒ．Ｗｅｓｔの研究室 = Dr. West's Laboratory
餌場の森 = Feeding Ground Forest
</locations>

<formatting_rules>
1. Output ONLY the English translation for each numbered line inside the <lines_to_translate> block.
2. Maintain the exact line numbers provided in your output.
3. Strictly NO [SPEAKER] tags, notes, commentary, or conversational filler in your response.
4. If a line is empty or just symbols (e.g., "……"), preserve the meaning.
5. Never combine, merge, or skip lines.
6. Dialogue lines have the format SpeakerName\r\nDialogue. Translate ONLY the dialogue portion; drop the speaker name and the \r\n prefix from your output.
7. Preserve any \r\n that appear WITHIN narrative or dialogue text (they are line breaks).
8. Use only ASCII characters -- no accented letters, em dashes, or smart quotes.
   Replace em dashes with --, ellipsis with ..., and curly quotes with straight quotes.
</formatting_rules>)";

}  // namespace exc::translate
