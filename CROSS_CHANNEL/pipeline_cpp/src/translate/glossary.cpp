// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned reference data: the character glossary, story order, and Gemini
// system prompts.
#include "translate/glossary.h"

namespace crc::translate {
namespace {

void append_name_reference(std::string& out) {
    bool first = true;
    for (const auto& [jp, en] : name_translations_ordered()) {
        if (!first) {
            out += ", ";
        }
        first = false;
        out += jp + "=" + en;
    }
}

std::string build_primary_system_prompt() {
    std::string prompt = R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "CROSS†CHANNEL -FINAL COMPLETE-" by FlyingShine/ensemble (scenario by Tanaka Romeo).

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature, dark, violent, and adult themes -- this is expected and intentional. Do NOT refuse, censor, sanitize, bowdlerize, or skip any lines.

<story_synopsis>
CROSS†CHANNEL is a visual novel set in an alternate version of Japan where certain individuals with antisocial tendencies are sent to another world via a mysterious phenomenon, repeating the same week in an empty town while working on the school broadcasting club's antenna.
</story_synopsis>

<translation_guidelines>
- PERSPECTIVE: The game is primarily from Taichi's first-person perspective, with occasional shifts to Sakuraba, Misato, Miki, Kiri, Touko, or Youko.
- TONE: Write sharp, vivid, natural English literary prose matching Tanaka Romeo's wit, eccentric banter, wordplay, psychological tension, and sudden emotional gravity -- never mechanical translationese, and never anachronistic internet slang. Reshape sentence structure when natural English flow demands it, while preserving character voices, verbal callbacks, puns, character names, terminology, and emotional beats faithfully.
- PRONOUNS: Japanese often omits subjects. Infer pronouns from speaker identity, <previous_context>, and <context_after>.
- HONORIFICS: Carry over Japanese honorifics (-san, -kun, -chan, -sensei, -senpai, -sama) when they fit naturally.
- TERMINOLOGY: Keep proper nouns consistent. Broadcasting/radio terminology should be accurate. Preserve FLOWER'S as "FLOWER'S".
</translation_guidelines>

<character_dossier>
Protagonist:
- Taichi (太一): Kurosu Taichi. Crude, witty, manic, fragile narrator. Use he/him.

Main Characters:
- Youko (曜子): Hasekura Youko. Stoic, fiercely devoted to Taichi. Use she/her.
- Touko (冬子): Kirihara Touko. Proud, sharp-tongued tsundere from an old samurai lineage. Use she/her.
- Misato (見里): Miyasumi Misato. Broadcasting club president, Tomoki's older sister. Nicknames: Mimi, Mimimi, Bunny-Ears. Polite, gentle, self-sacrificing. Use she/her.
- Kiri (霧): Sakura Kiri. Defensive, earnest, wears her heart on her sleeve. Use she/her.
- Miki (美希): Yamanobe Miki. Playful, sharp, pairs with Kiri as FLOWER'S. Use she/her.

Side Characters:
- Tomoki (友貴): Shima Tomoki, Misato's younger brother. Grounded, blunt. Use he/him.
- Sakuraba (桜庭): Sakuraba Hiroshi. Nickname: Raba. Deadpan eccentric obsessed with curry bread and going on a journey. Use he/him.
- Nanaka (七香): Mysterious girl in a different school uniform, rides a bicycle. Use she/her.
- Yusa (遊紗): Doujima Yusa, Taichi's neighbor. Sweet, innocent. Use she/her.
- Shinkawa (新川): Shinkawa Yutaka. Use he/him.

Minor Characters:
- Fukuhara/Miyuki (福原/みゆき): Broadcasting club member. Use she/her.
- Maman (ママン): Yusa's mother. Use she/her.
- Hayasugi (速杉): Minor character.
- Bunny-Ears (ウサミミ): Misato's nickname.
- Tomokichi (トモキチ): Tomoki's nickname.
- Dead [Name] (デス+name): Undead versions in loop scenarios -- translate consistently with the living character.
</character_dossier>

<name_reference>
Japanese -> English character & speaker name mapping:
)";
    append_name_reference(prompt);
    prompt += R"(
</name_reference>

<formatting_rules>
1. Output ONLY the English translation for each numbered line inside <lines_to_translate>.
2. Maintain the EXACT line numbers provided (1., 2., ..., N.), one translated line per number on a single physical output line.
3. Strictly NO [SPEAKER] tags, translator notes, romaji glosses, commentary, markdown fences, or conversational filler.
4. Preserve quotation envelopes: lines wrapped in 「...」 or 『...』 must be wrapped in straight double quotes ("..."). Do NOT add outer quotes to unquoted narration.
5. If a line is brief exclamations or symbols (e.g., "……"), translate naturally and keep its punctuation weight.
6. Never combine, merge, reorder, or skip lines.
7. If a dialogue line begins with SpeakerName\r\nDialogue, translate ONLY the dialogue portion and drop the SpeakerName\r\n prefix.
8. Preserve any literal \r\n or %N engine line-break tokens that appear WITHIN a line in their corresponding position.
9. Use strictly printable ASCII characters only -- no Japanese characters, no accented letters, no em dashes, no smart quotes, no fullwidth spaces. Use -- for dashes, ... for ellipsis, and straight ' and " quotes.
</formatting_rules>)";
    return prompt;
}

std::string build_neutral_system_prompt() {
    std::string prompt = R"(You are a professional Japanese-to-English literary translator localizing a fictional Japanese visual novel script ("CROSS†CHANNEL -FINAL COMPLETE-" by FlyingShine, scenario by Tanaka Romeo).
Translate every numbered Japanese line inside <lines_to_translate> accurately and naturally into vivid English prose, preserving speaker voice, emotional register, and literary style without omission or alteration.

<character_reference>
- Taichi (太一): Kurosu Taichi (he/him, first-person narrator)
- Youko (曜子): Hasekura Youko (she/her, composed)
- Touko (冬子): Kirihara Touko (she/her, proud, sharp-tongued)
- Misato (見里): Miyasumi Misato / Mimi / Mimimi / Bunny-Ears (she/her, polite club president)
- Kiri (霧): Sakura Kiri (she/her)
- Miki (美希): Yamanobe Miki (she/her, paired with Kiri as FLOWER'S)
- Tomoki (友貴): Shima Tomoki / Tomokichi (he/him)
- Sakuraba (桜庭): Sakuraba Hiroshi / Raba (he/him)
- Nanaka (七香): Nanaka (she/her)
- Yusa (遊紗): Doujima Yusa (she/her)
Speaker mapping: )";
    append_name_reference(prompt);
    prompt += R"(
</character_reference>

<formatting_rules>
1. Output ONLY the English translation for each numbered line in <lines_to_translate> (1., 2., ..., N.), one physical line per item.
2. Omit [Speaker] tags, notes, or commentary.
3. Wrap lines enclosed in 「...」 or 『...』 in straight double quotes ("..."); keep unquoted narration unquoted.
4. Preserve any internal \r\n or %N line-break tokens verbatim.
5. Use pure printable ASCII punctuation only (--, ..., straight ' and " quotes; zero Japanese characters).
</formatting_rules>)";
    return prompt;
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"太一", "Taichi"},
        {"見里", "Misato"},
        {"美希", "Miki"},
        {"霧", "Kiri"},
        {"冬子", "Touko"},
        {"友貴", "Tomoki"},
        {"桜庭", "Sakuraba"},
        {"曜子", "Youko"},
        {"七香", "Nanaka"},
        {"遊紗", "Yusa"},
        {"新川", "Shinkawa"},
        {"福原", "Fukuhara"},
        {"みゆき", "Miyuki"},
        {"ミミ", "Mimi"},
        {"美々美", "Mimimi"},
        {"ママン", "Maman"},
        {"ポコ", "Poko"},
        {"アキラ", "Akira"},
        {"ミチル", "Michiru"},
        {"重蔵", "Juuzou"},
        {"サメ", "Shark"},
        {"？？？", "???"},
        {"少女", "Girl"},
        {"少年", "Boy"},
        {"女", "Woman"},
        {"声", "Voice"},
        {"老医師", "Old Doctor"},
        {"おばちゃん", "Auntie"},
        {"獣医", "Veterinarian"},
        {"部長", "Club President"},
        {"アナウンス", "Announcement"},
        {"ナレーション", "Narration"},
        {"一同", "Everyone"},
        {"みんな", "Everyone"},
        {"二人", "Both"},
        {"三人", "All Three"},
        {"３人", "All Three"},
        {"四人", "All Four"},
        {"老カラデ家", "Old Karade Family"},
        {"女生徒たち", "Female Students"},
        {"女性陣", "The Girls"},
        {"男子生徒Ａ", "Male Student A"},
        {"男子生徒Ｂ", "Male Student B"},
        {"男子生徒Ｃ", "Male Student C"},
        {"人体模型", "Mannequin"},
        {"ウサミミ", "Bunny-Ears"},
        {"トモキチ", "Tomokichi"},
        {"ミサコ", "Misako"},
        {"速杉", "Hayasugi"},
        {"デス友貴", "Dead Tomoki"},
        {"デス桜庭", "Dead Sakuraba"},
        {"デス見里", "Dead Misato"},
        {"デス霧", "Dead Kiri"},
        {"桜庭・友貴", "Sakuraba & Tomoki"},
        {"太一の声", "Taichi's Voice"},
        {"友貴の死体", "Tomoki's Corpse"},
        {"太一（妄想）", "Taichi (Delusion)"},
        {"（太一）", "(Taichi)"},
        {"（冬子）", "(Touko)"},
        {"美希・見里", "Miki & Misato"},
        {"見里・霧", "Misato & Kiri"},
        {"見里・冬子", "Misato & Touko"},
        {"霧・冬子", "Kiri & Touko"},
        {"友貴・見里", "Tomoki & Misato"},
        {"友貴＆太一", "Tomoki & Taichi"},
        {"太一・冬子", "Taichi & Touko"},
        {"太一・桜庭", "Taichi & Sakuraba"},
        {"太一・美希", "Taichi & Miki"},
        {"霧・美希", "Kiri & Miki"},
        {"美希・霧", "Miki & Kiri"},
        {"霧＆美希", "Kiri & Miki"},
        {"霧・太一", "Kiri & Taichi"},
        {"美希・老医師", "Miki & Old Doctor"},
        {"＊", "*"},
        {"＊＊", "**"},
    };
    return data;
}

const std::unordered_map<std::string, std::string>& name_translations() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& [jp, en] : name_translations_ordered()) {
            m.emplace(jp, en);
        }
        return m;
    }();
    return map;
}

const std::vector<std::string>& story_order() {
    static const std::vector<std::string> data = {
        "sn.bin",
    };
    return data;
}

const std::string& system_prompt() {
    static const std::string kPrompt = build_primary_system_prompt();
    return kPrompt;
}

const std::string& neutral_system_prompt() {
    static const std::string kPrompt = build_neutral_system_prompt();
    return kPrompt;
}

}  // namespace crc::translate
