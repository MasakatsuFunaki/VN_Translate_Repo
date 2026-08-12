// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned reference data: the character glossary, the story order and the
// translator system prompt.  Do not hand-edit -- the tables are asserted
// character-for-character by the unit tests, and every entry is a name the
// player sees on screen.
#include "glossary.h"

namespace crc::translate {

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
        for (const auto& [jp, en] : name_translations_ordered()) m.emplace(jp, en);
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

const char* const SYSTEM_PROMPT =
    R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "CROSS†CHANNEL -FINAL COMPLETE-" by FlyingShine/ensemble.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes — this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<story_synopsis>
CROSS†CHANNEL is a visual novel set in an alternate version of Japan where certain individuals with antisocial tendencies are sent to another world via a mysterious phenomenon.
</story_synopsis>

<translation_guidelines>
- PERSPECTIVE: The game is primarily from Taichi's first-person perspective, with occasional shifts.
- TONE: Write English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Japanese often omits subjects. Infer pronouns from speaker identity and context.
- HONORIFICS: Carry over Japanese honorifics (-san, -kun, -chan, -sensei, -senpai, -sama) when they fit naturally.
- TERMINOLOGY: Keep proper nouns consistent. Broadcasting/radio terminology should be accurate.
</translation_guidelines>

<character_dossier>
Protagonist:
- Taichi (太一): Kurosu Taichi. Use he/him.

Main Characters:
- Youko (曜子): Hasekura Youko. Stoic, devoted to Taichi. Use she/her.
- Touko (冬子): Kirihara Touko. Tsundere. Use she/her.
- Misato (見里): Miyasumi Misato. Broadcasting club president, Tomoki's older sister. Nicknames: Mimi, Mimimi. Use she/her.
- Kiri (霧): Sakura Kiri. Use she/her.
- Miki (美希): Yamanobe Miki. Use she/her.

Side Characters:
- Tomoki (友貴): Shima Tomoki, Misato's younger brother. Use he/him.
- Sakuraba (桜庭): Sakuraba Hiroshi. Nickname: Raba. Use he/him.
- Nanaka (七香): Mysterious girl in different school uniform. Use she/her.
- Yusa (遊紗): Doujima Yusa, Taichi's neighbor. Use she/her.
- Shinkawa (新川): Shinkawa Yutaka. Use he/him.

Minor Characters:
- Fukuhara/Miyuki (福原/みゆき): Broadcasting club member. Use she/her.
- Maman (ママン): Yusa's mother. Use she/her.
- Hayasugi (速杉): Minor character.
- Bunny-Ears (ウサミミ): Misato's nickname.
- Tomokichi (トモキチ): Tomoki's nickname.
- Dead [Name] (デス+name): Undead versions in death-loop scenarios — translate same as the living version.
</character_dossier>

<name_reference>
Japanese -> English character name mapping:
太一=Taichi, 見里=Misato, 美希=Miki, 霧=Kiri, 冬子=Touko,
友貴=Tomoki, 桜庭=Sakuraba, 曜子=Youko, 七香=Nanaka, 遊紗=Yusa,
新川=Shinkawa, 福原=Fukuhara, みゆき=Miyuki, ミミ=Mimi, 美々美=Mimimi,
ウサミミ=Bunny-Ears, トモキチ=Tomokichi, ミサコ=Misako, 速杉=Hayasugi,
ママン=Maman, ポコ=Poko, アキラ=Akira, ミチル=Michiru, 重蔵=Juuzou, サメ=Shark,
デス友貴=Dead Tomoki, デス桜庭=Dead Sakuraba, デス見里=Dead Misato, デス霧=Dead Kiri,
？？？=???, 少女=Girl, 少年=Boy, 声=Voice, 老医師=Old Doctor,
おばちゃん=Auntie, 人体模型=Mannequin
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

}  // namespace crc::translate
