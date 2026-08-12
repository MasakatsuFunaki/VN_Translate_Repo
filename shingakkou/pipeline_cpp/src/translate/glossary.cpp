// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pinned data tables for step 2.  Editing an entry below changes the cache keys
// and the prompt bytes for the whole game, so treat them as fixed: names come
// from the glossary review, not from taste.
#include "glossary.h"

namespace shin::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered() {
    static const std::vector<std::pair<std::string, std::string>> data = {
        {"マイケル", "Michael"},
        {"ニール", "Neil"},
        {"オーガスト", "August"},
        {"セシル", "Cecil"},
        {"レオニード", "Leonid"},
        {"ガビィ", "Gabby"},
        {"ガブリエル", "Gabriel"},
        {"ジャック", "Jack"},
        {"ラザラス", "Lazarus"},
        {"ルシフェル", "Lucifer"},
        {"アベル", "Abel"},
        {"エイハブ", "Ahab"},
        {"ベンジャミン", "Benjamin"},
        {"デニス", "Dennis"},
        {"ロバート", "Robert"},
        {"ジョシュア", "Joshua"},
        {"ダニエル", "Daniel"},
        {"カレン", "Karen"},
        {"ミニー", "Minnie"},
        {"クリス", "Chris"},
        {"アグネス", "Agnes"},
        {"ポール", "Paul"},
        {"ジェイソン", "Jason"},
        {"校長", "Headmaster"},
        {"ミスター・モラン", "Mr. Moran"},
        {"ミスター・マコーリー", "Mr. Macauley"},
        {"ミセス・マコーリー", "Mrs. Macauley"},
        {"神父", "Father"},
        {"マスター", "Master"},
        {"アスタロト", "Astaroth"},
        {"エリゴール", "Eligos"},
        {"フルカス", "Furcas"},
        {"ベリアル", "Belial"},
        {"？？？", "???"},
        {"上級生", "Senior Student"},
        {"上級生２", "Senior Student 2"},
        {"上級生３", "Senior Student 3"},
        {"下級生", "Junior Student"},
        {"下級生２", "Junior Student 2"},
        {"下級生３", "Junior Student 3"},
        {"同級生", "Classmate"},
        {"同級生２", "Classmate 2"},
        {"同級生３", "Classmate 3"},
        {"少年", "Boy"},
        {"男", "Man"},
        {"男２", "Man 2"},
        {"男３", "Man 3"},
        {"女", "Woman"},
        {"男の子", "Boy"},
        {"男の子２", "Boy 2"},
        {"女の子", "Girl"},
        {"女の子２", "Girl 2"},
        {"記者", "Reporter"},
        {"兵士", "Soldier"},
        {"警官", "Policeman"},
        {"警官２", "Policeman 2"},
        {"アナウンサー", "Announcer"},
        {"ウェイトレス", "Waitress"},
        {"看護婦", "Nurse"},
        {"新聞売り", "Newsboy"},
        {"運転手", "Driver"},
        {"修道士", "Monk"},
        {"学生", "Student"},
        {"学生２", "Student 2"},
        {"不良の上級生", "Delinquent Senior"},
        {"見知らぬ女性", "Unknown Woman"},
        {"見知らぬ男", "Unknown Man"},
        {"見知らぬ少年", "Unknown Boy"},
        {"一同", "Everyone"},
        {"学生達", "Students"},
        {"亡者達", "The Dead"},
        {"同室者達", "Roommates"},
        {"マイケル・ガビィ", "Michael & Gabby"},
        {"セシルの父", "Cecil's Father"},
        {"レオニードの母", "Leonid's Mother"},
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
        "op05",
        "main01",
        "main02",
        "main03",
        "main04",
        "main05",
        "main06",
        "main07_1",
        "main07_2",
        "main07_3",
        "main08",
        "main09_1",
        "main09_2",
        "main09_3",
        "main09_4",
        "main10",
        "main11",
        "ed_main_bad1",
        "ed_main_bad2",
        "ed_main_bad3",
        "neil01",
        "neil02",
        "neil03",
        "neil04",
        "neil05",
        "neil06",
        "neil07",
        "neil08",
        "neil09",
        "neil10",
        "neil11",
        "neil12",
        "neil_ed_good",
        "neil_ed_bad1",
        "neil_ed_bad2",
        "leo01",
        "leo02",
        "leo03",
        "leo04",
        "leo05",
        "leo06",
        "leo07",
        "leo08",
        "leo09",
        "leo10",
        "leo11",
        "leo_ed_good",
        "leo_ed_bad1",
        "leo_ed_bad2",
        "cecil01",
        "cecil02",
        "cecil03",
        "cecil04",
        "cecil05",
        "cecil06",
        "cecil07",
        "cecil08",
        "cecil09",
        "cecil10",
        "cecil11",
        "cecil_ed_good",
        "cecil_ed_bad1",
        "cecil_ed_bad2",
        "gabriel01",
        "gabriel02",
        "gabriel03",
        "gabriel04",
        "gabriel05",
        "gabriel06",
        "gabriel07",
        "gabriel08",
        "gabriel09",
        "gabriel10",
        "gabriel_ed_good",
        "gabriel_ed_bad1",
        "august01",
        "august02",
        "august03",
        "august04",
        "august05",
        "august06",
        "august07",
        "august08",
        "august09",
        "august10",
        "august11",
        "august12",
        "august13",
        "august_ed_good",
        "august_ed_bad1",
        "august_ed_bad2",
    };
    return data;
}

const char* const SYSTEM_PROMPT =
    R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
Current Project: "Shingakkou ~Noli me tangere~" (神学校 ~Noli me tangere~) by PIL/SLASH.

IMPORTANT: This is a licensed commercial visual novel translation project. You MUST translate ALL lines regardless of content. The source material contains mature themes — this is expected and intentional. Do NOT refuse, censor, sanitize, or skip any lines.

<story_synopsis>
Shingakkou is set in a Catholic seminary school in a fictional European country during the early 20th century. The protagonist, Michael, is a devout student who becomes entangled in dark mysteries involving demonic possession, forbidden love, and theological conflicts. The story explores the tension between faith and desire, featuring multiple character routes with romantic and dramatic plotlines involving Michael and his fellow seminarians: Neil, Cecil, August, Gabriel (Gabby), and the enigmatic Leonid.
</story_synopsis>

<translation_guidelines>
- PERSPECTIVE: First-person narration from Michael's viewpoint (僕 = "I").
- TONE: Write English prose, not a mechanical translation. Reshape sentence structure when natural English flow demands it. Prioritize readability over literal fidelity when they conflict, EXCEPT for character names, terminology, and emotional beats which must be preserved exactly.
- PRONOUNS: Infer pronouns from context. Michael uses 僕 (I). Most characters are male.
- HONORIFICS: This is set in Europe, so do NOT use Japanese honorifics. Use Western forms of address (Mr., Father, etc.).
- TERMINOLOGY: Maintain consistency for religious terms (seminary, chapel, Mass, confession, etc.) and character names.
- QUOTES: Japanese「」brackets become standard English quotation marks.
</translation_guidelines>

<character_dossier>
Protagonist:
- Michael (マイケル): The protagonist. A serious, devout seminary student. Uses 僕. He/him.

Main Characters:
- Neil (ニール): A fellow student, one of Michael's close friends. He/him.
- August (オーガスト): A student with a complex personality. He/him.
- Cecil (セシル): A gentle, soft-spoken student. He/him.
- Gabriel / Gabby (ガブリエル / ガビィ): Michael's cheerful, easygoing friend. Gabby is his nickname. He/him.
- Leonid (レオニード): A strict, enigmatic senior student who serves as supervisor. He/him.

Supporting Characters:
- Jack (ジャック): A student. He/him.
- Lazarus (ラザラス): A student with a mysterious past. He/him.
- Lucifer (ルシフェル): A demonic figure. He/him.
- Abel (アベル): A student. He/him.
- Headmaster (校長): Head of the seminary. He/him.
- Mr. Moran (ミスター・モラン): A teacher. He/him.
- Mr. Macauley (ミスター・マコーリー): A teacher. He/him.
- Astaroth (アスタロト), Eligos (エリゴール), Furcas (フルカス), Belial (ベリアル): Demons.
</character_dossier>

<name_reference>
Japanese -> English character name mapping:
マイケル=Michael, ニール=Neil, オーガスト=August, セシル=Cecil,
レオニード=Leonid, ガビィ=Gabby, ガブリエル=Gabriel, ジャック=Jack,
ラザラス=Lazarus, ルシフェル=Lucifer, アベル=Abel, エイハブ=Ahab,
ベンジャミン=Benjamin, デニス=Dennis, ロバート=Robert, ジョシュア=Joshua,
ダニエル=Daniel, カレン=Karen, ミニー=Minnie, クリス=Chris,
アグネス=Agnes, ポール=Paul, ジェイソン=Jason, 校長=Headmaster,
ミスター・モラン=Mr. Moran, ミスター・マコーリー=Mr. Macauley,
ミセス・マコーリー=Mrs. Macauley, 神父=Father, マスター=Master,
アスタロト=Astaroth, エリゴール=Eligos, フルカス=Furcas, ベリアル=Belial,
？？？=???
マイケル・レヴィ=Michael Levi (protagonist's full name)
</name_reference>

<formatting_rules>
1. Output ONLY the English translation for each numbered line.
2. Maintain the exact line numbers provided in your output.
3. Strictly NO [SPEAKER] tags, notes, commentary, or conversational filler.
4. If a line is empty or just symbols (e.g., "……"), preserve the meaning.
5. Never combine, merge, or skip lines.
6. Dialogue text already has the speaker name stripped. Translate ONLY the dialogue content.
7. Preserve any \n that appear WITHIN narrative or dialogue text (they are line breaks in the game engine).
8. Use only ASCII characters -- no accented letters, em dashes, or smart quotes.
   Replace em dashes with --, ellipsis with ..., and curly quotes with straight quotes.
</formatting_rules>)";

}  // namespace shin::translate
