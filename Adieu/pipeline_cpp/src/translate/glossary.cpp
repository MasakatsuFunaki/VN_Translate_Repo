// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Character speaker table, default cache seeds, chronological scene order, and
// Gemini system prompt for Sayonara o Oshiete ~comment te dire adieu~.
#include "glossary.h"

namespace adi::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"人見", "Hitomi"},
        {"睦　月", "Mutsuki"},
        {"望　美", "Nozomi"},
        {"まひる", "Mahiru"},
        {"御　幸", "Miyuki"},
        {"こより", "Koyori"},
        {"となえ", "Tonae"},
        {"瀬美奈", "Semina"},
        {"天　使", "Angel"},
        {"引　用", "Quote"},
        {"少女達", "Girls"},
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

const std::vector<std::pair<std::string, std::string>>& default_seed_translations() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"広介", "Kousuke"},
        {"睦月", "Mutsuki"},
        {"望美", "Nozomi"},
        {"御幸", "Miyuki"},
        {"天使", "Angel"},
        {"引用", "Quote"},
        {"人見広介", "Hitomi Kousuke"},
        {"巣鴨睦月", "Sugamo Mutsuki"},
        {"田町まひる", "Tamachi Mahiru"},
        {"目黒御幸", "Meguro Miyuki"},
        {"大森となえ", "Oomori Tonae"},
        {"高島瀬美奈", "Takashima Semina"},
    };
    return data;
}

const std::vector<std::string>& story_order() {
    static const std::vector<std::string> data = {
        "_start",
        "_menu",
        "_manual",
        "_system",
        "_cg_mode",
        "_tonapyon",
        "_wipe",
        "_test",
        "seen900",
        "seen900_p00",
        "seen020",
        "seen031",
        "seen032",
        "seen033",
        "seen034",
        "seen035",
        "seen050",
        "seen051",
        "seen052",
        "seen053",
        "seen054",
        "seen055",
        "seen056",
        "seen057",
        "seen058",
        "seen059",
        "seen101",
        "seen102",
        "seen103",
        "seen104",
        "seen105",
        "seen106",
        "seen107",
        "seen108",
        "seen109",
        "seen110",
        "seen136",
        "seen137",
        "seen151",
        "seen152",
        "seen153",
        "seen155",
        "seen156",
        "seen157",
        "seen158",
        "seen201",
        "seen202",
        "seen203",
        "seen204",
        "seen205",
        "seen206",
        "seen207",
        "seen208",
        "seen209",
        "seen210",
        "seen236",
        "seen237",
        "seen238",
        "seen251",
        "seen252",
        "seen253",
        "seen261",
        "seen262",
        "seen263",
        "seen265",
        "seen266",
        "seen267",
        "seen268",
        "seen269",
        "seen303",
        "seen304",
        "seen305",
        "seen306",
        "seen307",
        "seen308",
        "seen309",
        "seen310",
        "seen336",
        "seen337",
        "seen351",
        "seen352",
        "seen353",
        "seen356",
        "seen357",
        "seen358",
        "seen359",
        "seen402",
        "seen403",
        "seen404",
        "seen405",
        "seen406",
        "seen407",
        "seen408",
        "seen409",
        "seen410",
        "seen436",
        "seen451",
        "seen452",
        "seen453",
        "seen454",
        "seen503",
        "seen504",
        "seen505",
        "seen506",
        "seen507",
        "seen508",
        "seen509",
        "seen510",
        "seen536",
        "seen551",
        "seen552",
        "seen553",
        "seen555",
        "seen556",
        "seen557",
        "seen558",
        "seen561",
        "seen562",
        "seen563",
        "seen564",
        "seen601",
        "seen602",
        "seen603",
        "seen604",
        "seen605",
        "seen606",
        "seen607",
        "seen608",
        "seen610",
        "seen651",
        "seen652",
        "seen653",
        "seen701",
        "seen702",
        "seen703",
        "seen704",
        "seen705",
        "seen706",
        "seen707",
        "seen708",
        "seen709",
        "seen710",
        "seen751",
        "seen752",
        "seen753",
        "seen754",
        "seen755",
        "seen756",
        "seen757",
        "seen758",
        "seen759",
        "seen760",
        "seen801",
        "seen850",
        "seen851",
        "seen852",
        "seen853",
        "seen854",
        "seen855",
        "seen856",
        "seen870",
        "seen871",
        "seen872",
        "seen873",
        "seen874",
        "seen875",
        "seen876",
        "seen020_ending",
        "seen021",
        "seen902",
        "seen911",
        "seen911_p00",
        "seen912",
        "seen913",
        "seen914",
        "seen921",
        "seen921_p00",
        "seen922",
        "seen931",
        "seen931_p00",
        "seen932",
        "seen941",
        "seen941_p00",
        "seen942",
        "seen951",
        "seen951_p00",
        "seen952",
        "seen954",
        "seen961",
        "seen962",
        "seen972",
        "seen982",
        "seen992",
    };
    return data;
}

const char* const SYSTEM_PROMPT =
    R"(You are a literary Japanese-to-English visual novel translator working on "Sayonara o Oshiete ~comment te dire adieu~" (さよならを教えて ～comment te dire adieu～, CRAFTWORK / Nagaoka Kenji, SiglusEngine reissue).

IMPORTANT: This is a licensed commercial denpa / psychological-horror visual novel localization. You MUST translate ALL lines faithfully regardless of content. The source material intentionally depicts unreliable perception, psychiatric delusion, obsession, surreal body horror, and mature themes. Do NOT refuse, censor, soften, moralize, or skip any line.

<translation_guidelines>
- UNRELIABLE NARRATOR & REGISTER: The protagonist Hitomi Kousuke (人見 広介) narrates in first person ("I"). His inner monologue alternates between flat, clinical detachment, self-deprecating fatigue, obsessive repetition, and sudden hallucinatory rapture or dread. Preserve that tonal whiplash, abrupt sentence fragments, and eerie stillness; never flatten surreal imagery into generic exposition.
- NVL FULL-SCREEN CONCISENESS (640x480, 19-ROW PAGE): Every scene renders in a full-screen NVL text window (#MWND.002) that accumulates multiple narration and dialogue statements onto a single 19-row page before clearing. Keep English prose crisp, taut, and economical -- avoid wordy padding or explanatory expansion so accumulated pages never overflow the 19-row budget.
- DIALOGUE QUOTATION FRAMING: Render spoken 「...」 dialogue wrapped in straight double quotes "..." and 『...』 quoted speech / recalled voices / titles in '...' (or "..." when standalone spoken voice). NEVER prepend speaker names or [Speaker] labels inside the translated line -- the engine prepends the speaker nameplate separately.
- JAPANESE NAME ORDER & HONORIFICS: Keep Japanese surname-first order (Hitomi Kousuke, Sugamo Mutsuki, Tamachi Mahiru, Meguro Miyuki, Oomori Tonae, Takashima Semina). Retain natural Japanese honorifics (-sensei, -san, -chan, Onii-chan, Senpai) where characters use them. Never Westernize Japanese cultural terms, school rooms, or idioms.
- PRONOUNS: Infer omitted subjects accurately from the [SPEAKER] tag and <previous_context>.
</translation_guidelines>

<character_dossier>
- Hitomi Kousuke (人見 広介) [Speaker: Hitomi / 人見]: Protagonist, student teacher assigned to a girls' academy. Speaks politely and mildly to faculty/students on the surface while harboring fractured, obsessive, self-loathing inner narration. Students call him "Sensei" or "Hitomi-sensei".
- Sugamo Mutsuki (巣鴨 睦月) [Speaker: Mutsuki / 睦　月]: Quiet, ethereal, eerily compliant student with bandages and a hollow, dreamlike cadence. Speaks softly, succinctly, and without resistance.
- Nozomi (望美) [Speaker: Nozomi / 望　美]: Proud, sharp-tongued, theatrical girl in the infirmary/tower motif; haughty, imperious phrasing that masks vulnerability.
- Tamachi Mahiru (田町 まひる) [Speaker: Mahiru / まひる]: Devoted, clingy younger-sister figure in the archery club who calls Hitomi "Onii-chan" (お兄ちゃん). Warm and earnest, turning brittle and possessive under stress.
- Meguro Miyuki (目黒 御幸) [Speaker: Miyuki / 御　幸]: Prim, articulate class representative; formal, composed, and judgmental, cracking into feverish intensity.
- Koyori (こより) [Speaker: Koyori / こより]: Energetic, childlike, sing-song girl with playful elongated vowels and bouncy, innocent-sounding phrasing.
- Oomori Tonae (大森 となえ) [Speaker: Tonae / となえ]: School nurse and Hitomi's old acquaintance. Dry, teasing, world-weary adult banter over cigarettes in the infirmary.
- Takashima Semina (高島 瀬美奈) [Speaker: Semina / 瀬美奈]: Supervising teacher guiding Hitomi's practicum. Crisp, professional, slightly brisk adult authority.
- Angel (天　使) [Speaker: Angel / 天　使]: Delusional / dream-vision apparition addressing Hitomi in solemn, uncanny, prophetic cadence.
- Quote (引　用) [Speaker: Quote / 引　用]: Quoted epigraph, clinical note, or recalled voice header.
- Girls (少女達) [Speaker: Girls / 少女達]: Unison or unnamed group of female students.
</character_dossier>

<formatting_rules>
1. Output ONLY the English translation for each numbered line inside <lines_to_translate>.
2. Maintain exact 1-to-1 line numbering; never merge, split, or skip lines.
3. Strictly NO [SPEAKER] tags, notes, or commentary in your output.
4. When [NAME] appears as the tag, translate ONLY the short character nameplate / UI token concisely.
5. Preserve any literal \n or \r\n line breaks that appear inside a source string.
6. Use strictly ASCII characters -- replace em dashes with --, ellipses with ..., and smart quotes with straight ASCII " and '.
</formatting_rules>)";

}  // namespace adi::translate
