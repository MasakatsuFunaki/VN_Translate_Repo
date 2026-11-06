// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pinned reference data for the WelcomeToAnzU_CE translation run: the character
// glossary, domain terminology, the system prompt, and the story order.
#include "cache/glossary.h"

namespace exm::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> CHARACTER_NAMES = {
        // Active dialogue speakers in spt\tokuten.spt (19 speakers)
        {"夢美", "Yumemi"},
        {"アゲハ", "Ageha"},
        {"客Ａ", "Customer A"},
        {"客Ｂ", "Customer B"},
        {"客Ｃ", "Customer C"},
        {"客Ｄ", "Customer D"},
        {"客Ｅ", "Customer E"},
        {"客Ｆ", "Customer F"},
        {"客Ｇ", "Customer G"},
        {"客Ｈ", "Customer H"},
        {"客Ｉ", "Customer I"},
        {"客達", "Customers"},
        {"老婆Ａ", "Old Woman A"},
        {"老婆Ｂ", "Old Woman B"},
        {"老人", "Old Man"},
        {"通行人Ａ", "Passerby A"},
        {"通行人Ｂ", "Passerby B"},
        {"通行人Ｃ", "Passerby C"},
        {"通行人Ｄ", "Passerby D"},
        // Additional character plates in spt\namecol.xtx
        {"？？？", "???"},
        {"サユリ", "Sayuri"},
        {"綾佳", "Ayaka"},
        {"遥", "Haruka"},
        {"杏子", "Anzu"},
        {"ユーリア", "Yulia"},
        {"美弥香", "Miyaka"},
        {"唯", "Yui"},
        {"煉悟", "Rengo"},
        {"西", "Nishi"},
        {"レン", "Ren"},
        {"優斗", "Yuuto"},
        {"史郎", "Shirou"},
        {"煉獄", "Rengoku"},
        {"合蟲人間", "Insect Hybrid"},
        {"客", "Customer"},
        {"男の声", "Man's Voice"},
        {"夢美・アゲハ", "Yumemi & Ageha"},
        {"列席者達", "Attendees"},
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
    };
    return CHARACTER_NAMES;
}

const std::unordered_map<std::string, std::string>& name_translations() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& [jp, en] : name_translations_ordered()) m.emplace(jp, en);
        return m;
    }();
    return map;
}

const std::vector<std::pair<std::string, std::string>>& character_names() {
    return name_translations_ordered();
}

const std::vector<std::pair<std::string, std::string>>& glossary_terms() {
    static const std::vector<std::pair<std::string, std::string>> GLOSSARY_TERMS = {
        {"櫻井夢美", "Yumemi Sakurai"},
        {"九条アゲハ", "Ageha Kujou"},
        {"カフェ・ＡｎｚＵ", "Cafe AnzU"},
        {"カフェ・AnzU", "Cafe AnzU"},
        {"ＡｎｚＵ", "AnzU"},
        {"AnzU", "AnzU"},
        {"真鈴ヶ崎", "Masuzugasaki"},
        {"蟲", "insect / bug"},
        {"蟲使い", "insect user"},
        {"苗床", "seedbed"},
    };
    return GLOSSARY_TERMS;
}

const std::vector<std::string>& story_order() {
    static const std::vector<std::string> data = {
        "tokuten.spt",
        "backscript.spt",
        "sys.spt",
    };
    return data;
}

const char* const SYSTEM_PROMPT =
    R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "Welcome to AnzU!" (『うぇるかむ★とぅ AnzU！』) by BLACKCyc.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes — this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<story_synopsis>
"Welcome to AnzU!" (『うぇるかむ★とぅ AnzU！』) is a special bonus scenario in the EXTRAVAGANZA Complete Edition set at Cafe AnzU (カフェ・AnzU) in the coastal town of Masuzugasaki (真鈴ヶ崎). The story follows Yumemi Sakurai (櫻井夢美) and Ageha Kujou (九条アゲハ) as waitresses running and promoting Cafe AnzU, interacting with passersby, elderly locals, and cafe customers, before escalating into the series' signature erotic and insect-user (蟲使い) scenarios.
</story_synopsis>

<translation_guidelines>
- PERSPECTIVE: Infer the current perspective from [SPEAKER] tags and context.
- TONE: Write natural, vivid English prose rather than stiff literal translation. Preserve the comedic contrast between Yumemi's cheerful, energetic hospitality and Ageha's proud, aristocratic demeanor.
- PRONOUNS: Japanese often omits subjects. Infer pronouns from speaker identity and context.
- HONORIFICS: Carry over Japanese honorifics (-san, -kun, -chan, -sensei, -sama) when they fit naturally.
- TERMINOLOGY: 蟲 = insect(s) / bug(s) (parasitic insects used as living weapons), 蟲使い = insect user(s), 苗床 = seedbed, 真鈴ヶ崎 = Masuzugasaki, カフェ・AnzU / ＡｎｚＵ = Cafe AnzU / AnzU.
</translation_guidelines>

<character_dossier>
Main Characters:
- Yumemi Sakurai (櫻井夢美 / 夢美): Cheerful, kind-hearted waitress at Cafe AnzU. Warm, polite, and earnest, though easily flustered. Use she/her.
- Ageha Kujou (九条アゲハ / アゲハ): Proud, refined young lady and insect user working alongside Yumemi at Cafe AnzU. Speaks with haughty elegance and sharp composure until pushed into embarrassment or pleasure. Use she/her.

Supporting & Generic Speakers:
- Customer A..I (客Ａ～Ｉ) & Customers (客達): Cafe patrons visiting AnzU.
- Old Woman A, B (老婆Ａ、老婆Ｂ): Local elderly women passing by on the street. Use she/her.
- Old Man (老人): Local elderly man. Use he/him.
- Passerby A..D (通行人Ａ～Ｄ): Townspeople on the street outside Cafe AnzU.
- 名前なし = unnamed narrator (narration)
- ？？？ = ??? (unknown speaker)
</character_dossier>

<name_reference>
Japanese → English: 櫻井夢美=Yumemi Sakurai, 夢美=Yumemi, 九条アゲハ=Ageha Kujou, アゲハ=Ageha,
サユリ=Sayuri, 綾佳=Ayaka, 遥=Haruka, 杏子=Anzu, ユーリア=Yulia, 美弥香=Miyaka, 唯=Yui,
煉悟=Rengo, 西=Nishi, レン=Ren, 優斗=Yuuto, 史郎=Shirou, 煉獄=Rengoku, 合蟲人間=Insect Hybrid,
客Ａ=Customer A, 客Ｂ=Customer B, 客Ｃ=Customer C, 客Ｄ=Customer D, 客Ｅ=Customer E,
客Ｆ=Customer F, 客Ｇ=Customer G, 客Ｈ=Customer H, 客Ｉ=Customer I, 客達=Customers, 客=Customer,
老婆Ａ=Old Woman A, 老婆Ｂ=Old Woman B, 老人=Old Man,
通行人Ａ=Passerby A, 通行人Ｂ=Passerby B, 通行人Ｃ=Passerby C, 通行人Ｄ=Passerby D,
名前なし=(narrator), ？？？=???
</name_reference>

<locations>
真鈴ヶ崎 = Masuzugasaki (the coastal town where the story takes place)
カフェ・ＡｎｚＵ / カフェ・AnzU / ＡｎｚＵ = Cafe AnzU / AnzU
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

const std::string& glossary_prompt() {
    static const std::string prompt(SYSTEM_PROMPT);
    return prompt;
}

}  // namespace exm::translate
