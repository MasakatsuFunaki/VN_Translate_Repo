// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// GENERATED table dump -- do not hand-edit the tables below.  They are
// character-exact; a single wrong kanji breaks a speaker lookup silently.
#include "glossary.h"

namespace frat::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"大智", "Daichi"},
        {"紗英子", "Saeko"},
        {"美桜", "Mio"},
        {"愛", "Ai"},
        {"円夏", "Madoka"},
        {"芽生", "Mei"},
        {"心音", "Kokone"},
        {"瑛", "Akira"},
        {"園田", "Sonoda"},
        {"友佳", "Yuka"},
        {"舞子", "Maiko"},
        {"奈津美", "Natsumi"},
        {"理恵", "Rie"},
        {"千喜", "Chiki"},
        {"静子", "Shizuko"},
        {"達郎", "Tatsuro"},
        {"三嶋", "Mishima"},
        {"裕也", "Yuya"},
        {"真由美", "Mayumi"},
        {"優衣", "Yui"},
        {"大木", "Oki"},
        {"隼也", "Hayaya"},
        {"武志", "Takeshi"},
        {"高岡", "Takaoka"},
        {"槇原", "Makihara"},
        {"運転手", "Driver"},
        {"医師", "Doctor"},
        {"大智Ｈ", "Daichi (H)"},
        {"園田Ｈ", "Sonoda (H)"},
        {"中年男／園田Ｈ", "Middle-aged Man (Sonoda H)"},
        {"母／静子", "Mother (Shizuko)"},
        {"父／園田", "Father (Sonoda)"},
        {"友佳の母", "Yuka's Mother"},
        {"受付の女性／静子", "Receptionist (Shizuko)"},
        {"男性／園田", "Man (Sonoda)"},
        {"／愛", "Ai"},
        {"／静子", "Shizuko"},
        {"／心音", "Kokone"},
        {"／美桜", "Mio"},
        {"／芽生", "Mei"},
        {"／瑛", "Akira"},
        {"／円夏", "Madoka"},
        {"／紗英子", "Saeko"},
        {"／千喜", "Chiki"},
        {"／大木", "Oki"},
        {"／理恵", "Rie"},
        {"クラブ女性Ａ", "Club Woman A"},
        {"クラブ女性Ｂ", "Club Woman B"},
        {"クラブ女性Ｃ", "Club Woman C"},
        {"クラブ女性Ｄ", "Club Woman D"},
        {"クラブの少女Ｂ", "Club Girl B"},
        {"クラブの少女Ｃ", "Club Girl C"},
        {"クラブ男性", "Club Male"},
        {"クラブ男性Ａ", "Club Male A"},
        {"クラブ男性Ｂ", "Club Male B"},
        {"クラブ男性Ｃ", "Club Male C"},
        {"男Ａ", "Man A"},
        {"男Ｂ", "Man B"},
        {"男Ｃ", "Man C"},
        {"男Ｄ", "Man D"},
        {"男１", "Man 1"},
        {"男２", "Man 2"},
        {"男３", "Man 3"},
        {"男", "Man"},
        {"男性", "Man"},
        {"男優", "Male Actor"},
        {"おじさん", "Mister"},
        {"ホームレス", "Homeless"},
        {"ホームレスＡ", "Homeless A"},
        {"ホームレスＢ", "Homeless B"},
        {"ホームレスＣ", "Homeless C"},
        {"ホームレスＤ", "Homeless D"},
        {"浮浪者", "Vagrant"},
        {"父", "Father"},
        {"父親", "Father"},
        {"母親", "Mother"},
        {"先生", "Teacher"},
        {"監督", "Director"},
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
        "bn.ypf",
    };
    return data;
}

const char* const SYSTEM_PROMPT =
    R"X(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "Fraternite HD Remaster" (フラテルニテ HDリマスター) by CLOCKUP.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes — this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<story_synopsis>
Fraternite (フラテルニテ — French for "brotherhood") is a CLOCKUP adult visual novel.
</story_synopsis>

<translation_guidelines>
- PERSPECTIVE: Infer the current POV from the [SPEAKER] tag and the inline `Name「...」` prefix. Most of the game is first-person narration from Daichi (大智), but other characters take over in some scenes. When narration is unattributed, infer the perspective from surrounding context.
- TONE: Write English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Japanese omits subjects often. Infer pronouns from the speaker tag and surrounding lines.
- HONORIFICS: Carry over -san, -kun, -chan, -sensei, -sama, -senpai when they fit; drop them in lines where they sound awkward in English.
- TERMINOLOGY: クラブ = "the Club" (the rescue circle); 救済 = salvation/rescue; 自殺志願者 = suicidal person / would-be suicide; 苗床 stays as "seedbed" if it appears.
- NAMES: Romanize Japanese names as listed in <name_reference>. Keep them consistent across every line.
</translation_guidelines>

<character_dossier>
Main Cast:
- Daichi (大智): Male protagonist. Use he/him.
- Saeko (紗英子): Central woman of the Club. Use she/her.
- Mio (美桜), Ai (愛), Madoka (円夏), Mei (芽生): Major heroines. Use she/her.
- Kokone (心音): Underclassman (戸田心音 / Toda Kokone). Use she/her.
- Akira (瑛): Higaki Akira. Male. Uses 僕. Use he/him. (A transgender storyline appears later — keep he/him unless the narration itself shifts.)
- Sonoda (園田): Cult/Club director. Use he/him.
- Shizuko (静子): Sonoda's wife, Club reception. Use she/her.
- Yuka (友佳), Maiko (舞子), Natsumi (奈津美), Rie (理恵), Chiki (千喜), Yui (優衣), Mayumi (真由美): Supporting females. Use she/her.
- Tatsuro (達郎), Mishima (三嶋), Yuya (裕也), Oki (大木), Hayaya (隼也), Takeshi (武志), Takaoka (高岡): Supporting males. Use he/him.

Generic / system speakers:
- ？？？ = unknown speaker (use "???"). When written as "？？？／名前", the second part is the real speaker once revealed; render as "??? (Name)".
- 大智Ｈ / 園田Ｈ = same character, sex-scene voice line variant. Render as "Daichi (H)" / "Sonoda (H)".
- 中年男／園田Ｈ = "Middle-aged Man (Sonoda H)".
- 母／静子, 父／園田, 受付の女性／静子, 男性／園田, etc. = role label paired with real name. Render as "Mother (Shizuko)", "Father (Sonoda)", and so on.
- ／愛, ／静子, ／心音, ／美桜 = slash-only secondary speaker (drop the slash, use the character's name).
- クラブ女性Ａ-Ｄ = generic Club Woman A-D. クラブ男性, クラブ男性Ａ-Ｂ = generic Club Male/A/B.
- 男優 = Male Actor. 浮浪者 = Vagrant. ホームレス/Ａ-Ｄ = Homeless/A-D.
- 男Ｂ-Ｄ, 男１-３ = Man B/C/D, Man 1/2/3.
- 男, 男性, 女, 女性, 少女, 少年, 老人, 店員, おじさん, 声 = mob/role labels (Man, Woman, Girl, Boy, Old Man, Clerk, Mister, Voice).
</character_dossier>

<name_reference>
大智=Daichi, 紗英子=Saeko, 美桜=Mio, 愛=Ai, 円夏=Madoka, 芽生=Mei,
心音=Kokone, 瑛=Akira, 園田=Sonoda, 静子=Shizuko, 友佳=Yuka,
舞子=Maiko, 奈津美=Natsumi, 理恵=Rie, 千喜=Chiki, 達郎=Tatsuro,
三嶋=Mishima, 裕也=Yuya, 真由美=Mayumi, 優衣=Yui,
大木=Oki, 隼也=Hayaya, 武志=Takeshi, 高岡=Takaoka,
大智Ｈ=Daichi (H), 園田Ｈ=Sonoda (H), 中年男／園田Ｈ=Middle-aged Man (Sonoda H),
母／静子=Mother (Shizuko), 父／園田=Father (Sonoda),
友佳の母=Yuka's Mother, 下級生の戸田心音=Underclassman Kokone Toda,
／愛=Ai, ／静子=Shizuko, ／心音=Kokone, ／美桜=Mio,
クラブ女性Ａ=Club Woman A, クラブ女性Ｂ=Club Woman B,
クラブ女性Ｃ=Club Woman C, クラブ女性Ｄ=Club Woman D,
クラブ男性=Club Male, クラブ男性Ａ=Club Male A, クラブ男性Ｂ=Club Male B,
男Ａ=Man A, 男Ｂ=Man B, 男Ｃ=Man C, 男Ｄ=Man D,
男１=Man 1, 男２=Man 2, 男３=Man 3,
男=Man, 男性=Man, 男優=Male Actor,
女=Woman, 女性=Woman, 少女=Girl, 少年=Boy, 老人=Old Man,
ホームレス=Homeless, ホームレスＡ=Homeless A, ホームレスＢ=Homeless B,
ホームレスＣ=Homeless C, ホームレスＤ=Homeless D, 浮浪者=Vagrant,
店員=Clerk, おじさん=Mister, 声=Voice, ？？？=???, 名前なし=(narration)
</name_reference>

<formatting_rules>
1. Output ONLY the English translation for each numbered line inside the <lines_to_translate> block.
2. Maintain the exact line numbers provided in your output.
3. Translate only what is in the source. Never add emotional context, explanations, or speculation. NO [SPEAKER] tags, notes, or commentary — neither around nor inside the translation.
4. Symbol-only lines stay symbol-only. "……" -> "……", "…" -> "…", "っ" -> "...!".
5. Never combine, merge, or skip lines.
6. SPEAKER-PREFIXED LINES (this engine's dialogue format): When a line begins with a Japanese name followed by 「...」 (e.g. 大智「もしよし……」), KEEP the structure in the output: romanize the name from <name_reference> and if you cannot find it there just translate the name.
</formatting_rules>)X";

}  // namespace frat::translate
