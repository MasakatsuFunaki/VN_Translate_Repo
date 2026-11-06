// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pinned reference data for the EXTRA_VA_MIZUNA_CE translation run: the
// character glossary, the system prompt, and the story order.
#include "cache/glossary.h"

namespace exm::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"水菜", "Mizuna"},
        {"瑞菜", "Mizuna"},
        {"史郎", "Shirou"},
        {"？？？", "???"},
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
        {"男Ｍ", "Man M"},
        {"男Ｎ", "Man N"},
        {"男Ｏ", "Man O"},
        {"男Ｐ", "Man P"},
        {"男Ｑ", "Man Q"},
        {"男Ｒ", "Man R"},
        {"男Ｓ", "Man S"},
        {"男Ｔ", "Man T"},
        {"男Ｕ", "Man U"},
        {"男Ｖ", "Man V"},
        {"男Ｗ", "Man W"},
        {"男Ｘ", "Man X"},
        {"男Ｙ", "Man Y"},
        {"男Ｚ", "Man Z"},
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
        "tantai3.spt",
        "koiEX_backscript.spt",
        "sys.spt",
    };
    return data;
}

const char* const SYSTEM_PROMPT =
    R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "EXTRA VA MIZUNA ~EXTRAVAGANZA~" (『EXTRA VA MIZUNA ～EXTRAVAGANZA～』) by BLACKCyc.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes -- this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<story_synopsis>
"EXTRA VA MIZUNA ~EXTRAVAGANZA~" is a dark side-story/fandisc in the EXTRAVAGANZA universe centered on Mizuna (水菜) and Shirou Kitabatake (北畠史郎 / 史郎) in the world of parasitic insects (蟲, bugs / insects) and insect users (蟲使い). The story follows Mizuna's devotion to Shirou and the harrowing ordeal she endures as a captive host and test subject.
</story_synopsis>

<translation_guidelines>
- PERSPECTIVE: The game switches between character perspectives (primarily Mizuna and Shirou). Infer the current perspective from [SPEAKER] tags and context.
- TONE: Write natural, atmospheric English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Japanese often omits subjects. Infer pronouns from speaker identity and context.
- HONORIFICS: Carry over Japanese honorifics (-san, -kun, -chan, -sensei, -sama) when they fit naturally.
- TERMINOLOGY: 蟲 = insect(s) / bug(s) (parasitic insects used as living weapons), 蟲使い = insect user(s) / Bug User(s), 苗床 = seedbed (a human host body for insects).
</translation_guidelines>

<character_dossier>
Main Characters:
- Mizuna (水菜 / 瑞菜): Central heroine of this story. Deeply devoted to Shirou; speaks gently and politely even under extreme captivity and torment. Use she/her.
- Shirou Kitabatake (北畠史郎 / 史郎): Mizuna's partner and a calculating young man involved in the world of insect users. Speaks in a direct, composed masculine register. Use he/him.

Supporting/Generic Speakers:
- Man A..Z (男Ａ～Ｚ): Generic male speakers (captors, researchers, onlookers). Use he/him.
- 名前なし = unnamed narrator (narration)
- ？？？ = ??? (unknown speaker)
</character_dossier>

<name_reference>
Japanese -> English: 水菜=Mizuna, 瑞菜=Mizuna, 北畠史郎=Shirou Kitabatake, 史郎=Shirou,
蟲=insect / bug, 蟲使い=insect user, 苗床=seedbed, 名前なし=(narrator), ？？？=???,
男Ａ=Man A, 男Ｂ=Man B, 男Ｃ=Man C, 男Ｄ=Man D, 男Ｅ=Man E, 男Ｆ=Man F,
男Ｇ=Man G, 男Ｈ=Man H, 男Ｉ=Man I, 男Ｊ=Man J, 男Ｋ=Man K, 男Ｌ=Man L,
男Ｍ=Man M, 男Ｎ=Man N, 男Ｏ=Man O, 男Ｐ=Man P, 男Ｑ=Man Q, 男Ｒ=Man R,
男Ｓ=Man S, 男Ｔ=Man T, 男Ｕ=Man U, 男Ｖ=Man V, 男Ｗ=Man W, 男Ｘ=Man X,
男Ｙ=Man Y, 男Ｚ=Man Z
</name_reference>

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
