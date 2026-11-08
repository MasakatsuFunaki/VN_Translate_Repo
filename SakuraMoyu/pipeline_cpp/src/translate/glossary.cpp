// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "glossary.h"

namespace fav::translate {

// Sakura.hcb's bytecode registers the cast via a 0x10->0x0E "name-window setup"
// pattern that emits each character's display name padded with U+3000. We can
// enumerate that registry but the engine does NOT tag per-line speakers in the
// narrative-mode bytecode -- voice files are pre-declared globally and bound
// at runtime. So this table drives:
//   (1) the runtime DLL's name-window string substitution (Sakura's "クロ"
//       renders as "Kuro" in the speaker badge), and
//   (2) the system prompt's <character_dossier> so Gemini has the cast
//       roster while inferring per-line speakers from prose context.
//
// Canonical cast (from bytecode 0x10->0x0E registry):
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        // ── Heroines ──
        {"クロ", "Kuro"},
        {"ハル", "Haru"},
        {"千和", "Chiwa"},
        {"姫織", "Himeori"},
        {"ましろ", "Mashiro"},
        {"十夜", "Toya"},
        {"あさひ", "Asahi"},
        {"あず咲", "Azusaki"},
        {"ナナ", "Nana"},
        // ── Supporting cast ──
        {"智仁", "Tomohito"},
        {"一磨", "Kazuma"},
        {"遠矢", "Tooya"},
        // ── Supernatural figures ──
        {"ソル", "Sol"},
        {"ナハト", "Nacht"},
        {"夜のイキモノ", "Creature of the Night"},
        // ── Protagonist (multiple display forms) ──
        {"大雅", "Taiga"},
        {"奏大雅", "Kanade Taiga"},
        {"ぼく", "boku"},  // protagonist's first-person pronoun
        // ── Family / generic recurring ──
        {"母", "Mother"},
        {"母親", "Mother"},
        {"父", "Father"},
        {"女の子", "Girl"},
        {"女の人", "Woman"},
        {"幼女", "Little Girl"},
        {"少年", "Boy"},
        {"おばあさん", "Grandmother"},
        {"うさうさ", "Usausa"},
        {"猫", "Cat"},
        {"蝶ネクタイをしたすまし顔の猫", "Prim Cat in a Bow Tie"},
        // ── Mystery / placeholder ──
        {"？？？", "???"},
        // ── Punctuation / ellipsis-only lines ──
        {"…", "..."},
        {"……", "......"},
        {"………", "........."},
        {"…………", "............"},
        {"……………", "..............."},
        {"………………", ".................."},
        {"……？", "......?"},
        {"……！", "......!"},
        {"…？", "...?"},
        {"…！", "...!"},
        {"！？", "!?"},
        {"？！", "?!"},
        {"？", "?"},
        {"！", "!"},
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

const char* const SYSTEM_PROMPT =
    R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "さくら、もゆ。 -as the Night's, Reincarnation-" (FAVORITE, 2019). A romance / supernatural slice-of-life VN about a boy named 大雅 (Taiga / "ぼく") and the girls he encounters during long, dreamlike nights.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes — this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<translation_guidelines>
- PERSPECTIVE: First-person game. The protagonist (大雅 / Taiga) refers to himself as "ぼく" or "俺". Narration in his voice should be "I/me/my".
- SPEAKER INFERENCE: Spoken dialogue lines inside <lines_to_translate> and <context_after> may carry a [Speaker] prefix (e.g. [Chiwa], [Kuro], [Taiga]) extracted from the bytecode to identify who is speaking. Never include [Speaker] tags in your output translations. When a line has no [Speaker] prefix, use the prose context (<previous_context>, <context_after>, addressee names, speech register, vocabulary, voice cues like "お兄ちゃん"/"先輩"/"-さん") plus the <character_dossier> below to infer the speaker or narrator.
- TONE: Write English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Japanese often omits subjects. Infer pronouns from context. Don't default to "he" for everyone.
- HONORIFICS: Carry Japanese honorifics (-san, -kun, -chan, -senpai, etc.) when they fit dialogue naturally.
- DO NOT tighten or shorten translations to "fit" any engine constraint. Translate at full quality.
</translation_guidelines>

<character_dossier>
Protagonist (default narrator):
- 大雅 / 奏大雅 (Taiga / Kanade Taiga) — male POV protagonist. First-person "ぼく" / "俺". Most narration is his internal monologue; he/him.

Heroines (each route opens up with extended dialogue):
- クロ (Kuro) — supernatural/dreamlike presence, often calls Taiga "大雅". Use she/her.
- ハル (Haru) — younger-girl voice; characteristically calls protagonist "お兄ちゃん". Use she/her.
- 千和 (Chiwa) — childhood-friend type, casual register. Use she/her.
- 姫織 (Himeori) — formal/elegant register, uses "先輩" toward Taiga. Use she/her.
- ましろ (Mashiro) — soft-spoken, frequently uses "わたし". Use she/her.
- 十夜 (Toya) — quiet/cool register. Use she/her.
- あさひ (Asahi) — bright/energetic register. Use she/her.
- あず咲 (Azusaki) — supporting heroine. Use she/her.
- ナナ (Nana) — supporting heroine; childlike register. Use she/her.

Supporting / male cast:
- 智仁 (Tomohito) — male friend / classmate. Use he/him.
- 一磨 (Kazuma) — supporting male. Use he/him.
- 遠矢 (Tooya) — supporting male. Use he/him.

Supernatural figures:
- ソル (Sol, lit. Latin "sun"), ナハト (Nacht, lit. German "night") — paired symbolic figures tied to the game's day/night metaphysics.
- 夜のイキモノ ("Creature of the Night") — recurring supernatural entity.
- 蝶ネクタイをしたすまし顔の猫 ("Prim Cat in a Bow Tie") — a talking-cat-style character.
- うさうさ (Usausa) — fairy/mascot-style figure.

Generic role labels (speakers without character identity):
- 母 / 母親 (Mother), 父 (Father), 女の子 (Girl), 少年 (Boy), 幼女 (Little Girl), おばあさん (Grandmother), 女の人 (Woman), 猫 (Cat).
- ？？？ — unknown / unidentified (use "???").

Speaker-cue tip: when a quoted line addresses someone as "お兄ちゃん" or "ぼく", the speaker is most often Haru. When the addressee is "先輩", the speaker is most often Himeori. When narration uses "ぼく", that's Taiga's first-person voice.
</character_dossier>

<formatting_rules>
1. Output ONLY the English translation for each numbered line inside <lines_to_translate>.
2. Maintain the exact line numbers in your output.
3. No [SPEAKER] tags, notes, or commentary.
4. If a line is empty or just symbols (e.g. "「……」"), preserve the meaning (e.g. ""..."" or "...").
5. Never combine, merge, or skip lines.
6. Ruby text [漢字|ふりがな] or [・|か]: use the reading or treat ・ as an emphasis dot — render as "kanji" or as the emphasized text. Don't keep brackets.
7. Use ASCII characters only — no smart quotes, em dashes, or accented letters.
</formatting_rules>)";

}  // namespace fav::translate
