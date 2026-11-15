// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace translator_logic {

bool IsChoiceLikeJp(const std::string& jp) {
    return jp.size() >= 10 && jp.size() <= 14;
}

std::string FitToSlot(const std::string& en, std::size_t slot) {
    if (en.size() <= slot) return en;
    if (slot == 0) return std::string();
    for (std::size_t i = slot; i > 0; --i) {
        if (en[i - 1] == ' ') return en.substr(0, i - 1);
    }
    return en.substr(0, slot);
}

namespace {

struct SeedEntry {
    const char* jp_utf8;
    const char* en;
};

// Built-in CP932 -> EN entries for sn.bin strings missing from or prefix-clipped
// in translations.tsv.  Inserted via `emplace` so existing TSV translations
// always win on collision while exact unstripped keys beat stripped fallbacks.
constexpr SeedEntry kScriptMenuSeedTable[] = {
    // --- script[0] op 0x32 scenario-launcher menus & op 0x45 legend ----------
    {"本編スタート", "Start Main Story"},
    {"ファイル別実行", "Run by Script File"},
    {"アナザーを始める", "Start Another Story"},
    {"ＸＢＯＸシナリオを始める", "Start Xbox Scenario"},
    {"その他シナリオ／実験場", "Other Scenarios / Test Room"},
    {"Ａルート", "Route A"},
    {"Ｂルート", "Route B"},
    {"Ｃルート", "Route C"},
    {"Ｄルート", "Route D"},
    {"その他", "Other"},
    {"ＸＢＯＸ", "Xbox"},
    {"CCB0010B（未使用）", "CCB0010B (Unused)"},
    {"CCB1017（未使用）", "CCB1017 (Unused)"},
    {"CCB1101（未使用）", "CCB1101 (Unused)"},
    {"CCB2003B（未使用）", "CCB2003B (Unused)"},
    {"CCB2018（未使用）", "CCB2018 (Unused)"},
    {"CCC0010B（未使用）", "CCC0010B (Unused)"},
    {"CCC4013（未使用）", "CCC4013 (Unused)"},
    {"CCD0005B（未使用）", "CCD0005B (Unused)"},
    {"CCD4003B（未使用）", "CCD4003B (Unused)"},
    {"Ａボタン　　：決定%NＢボタン　　：上階層に復帰%N%N←→　　　　：ページ切り替え",
     "A Button: Confirm%NB Button: Back%N%NLeft / Right: Switch Page"},

    // --- All 35 CASE chapter cards & Bad-End failure cards (op 0x47) ---------
    {"ＣＡＳＥ　１", "CASE 1"},
    {"ＣＡＳＥ　２", "CASE 2"},
    {"ＣＡＳＥ　３", "CASE 3"},
    {"ＣＡＳＥ　３（ＲＥＴＲＹ）", "CASE 3 (RETRY)"},
    {"ＣＡＳＥ　３　（ＲＥＴＲＹ２）", "CASE 3 (RETRY 2)"},
    {"ＣＡＳＥ　４", "CASE 4"},
    {"ＣＡＳＥ　５", "CASE 5"},
    {"ＣＡＳＥ　６", "CASE 6"},
    {"ＣＡＳＥ　７", "CASE 7"},
    {"ＣＡＳＥ　８", "CASE 8"},
    {"ＣＡＳＥ　１５", "CASE 15"},
    {"ＣＡＳＥ　３５", "CASE 35"},
    {"ＣＡＳＥ　６９", "CASE 69"},
    {"ＣＡＳＥ　１０７", "CASE 107"},
    {"ＣＡＳＥ　１０８", "CASE 108"},
    {"ＣＡＳＥ　１　失敗", "CASE 1 - Failure"},
    {"ＣＡＳＥ　２　失敗", "CASE 2 - Failure"},
    {"ＣＡＳＥ　３　失敗", "CASE 3 - Failure"},
    {"ＣＡＳＥ　３（ＲＥＴＲＹ）　失敗", "CASE 3 (RETRY) - Failure"},
    {"ＣＡＳＥ　３（ＲＥＴＲＹ２）　失敗", "CASE 3 (RETRY 2) - Failure"},
    {"ＣＡＳＥ　４　失敗", "CASE 4 - Failure"},
    {"ＣＡＳＥ　５　失敗", "CASE 5 - Failure"},
    {"ＣＡＳＥ　６　失敗", "CASE 6 - Failure"},
    {"ＣＡＳＥ　７　失敗", "CASE 7 - Failure"},
    {"ＣＡＳＥ　８　失敗", "CASE 8 - Failure"},
    {"ＣＡＳＥ　９　失敗", "CASE 9 - Failure"},
    {"ＣＡＳＥ　１０　失敗", "CASE 10 - Failure"},
    {"ＣＡＳＥ　１１　失敗", "CASE 11 - Failure"},
    {"ＣＡＳＥ　１２　失敗", "CASE 12 - Failure"},
    {"ＣＡＳＥ　１３　失敗", "CASE 13 - Failure"},
    {"ＣＡＳＥ　１４　失敗", "CASE 14 - Failure"},
    {"ＣＡＳＥ　１５　失敗", "CASE 15 - Failure"},
    {"ＣＡＳＥ　４８　失敗", "CASE 48 - Failure"},
    {"ＣＡＳＥ　６９　失敗", "CASE 69 - Failure"},
    {"ＣＡＳＥ　１０７　失敗", "CASE 107 - Failure"},

    // --- All 31 op 0x86 Miki silent-backlog lines ----------------------------
    {"「先輩……っ！」", "\"Senpai...!\""},
    {"「な、何を言ってるんです？」", "\"W-what are you talking about?\""},
    {"「それにその格好……」", "\"And that outfit...\""},
    {"「……霧はどこですか」", "\"...Where is Kiri?\""},
    {"「……その血」", "\"...That blood...\""},
    {"「霧は、どこです」", "\"Where is Kiri?\""},
    {"「……凄いですね」", "\"...It's amazing.\""},
    {"「あの……ところで霧ちんが材料に使われてないみたいですけど……彼女はどこに？」",
     "\"Um... by the way, it doesn't seem like Kiri-chin was used as material... where is she?\""},
    {"「そうですか」", "\"I see.\""},
    {"「いえ……ではわたしはこれで」", "\"No... well then, I'll be going.\""},
    {"「……動け……動け……動け……動け……わたしの足、動け……」",
     "\"...Move... move... move... move... my legs, move...\""},
    {"「……動け……動け……動け……動け……動け……動け……動け……動け……」",
     "\"...Move... move... move... move... move... move... move... move...\""},
    {"「……先輩？　え……嘘？」", "\"...Senpai? Eh... no way?\""},
    {"「痛い……足……いたぁ……」", "\"It hurts... my leg... it huuurts...\""},
    {"「来ないで……来ないでぇぇぇっ！！」", "\"Don't come... don't come near meee!!\""},
    {"「ああぁぁぁっ！！」", "\"Aaaaah!!\""},
    {"「あ…………」", "\"Ah...\""},
    {"「どうして……どうして？」", "\"Why... why?\""},
    {"「どうして……わたしは……すぐに逃げなかったのかな……ね、先輩？」",
     "\"Why... didn't I... run right away... hey, senpai?\""},
    {"「自分が大事……なのに……」", "\"I cared about myself... and yet...\""},
    {"「っ……いたっ……さっさと、逃げてれば……安全だったのになぁ…あはは……」",
     "\"Ngh... it hurts... if I'd just run away quickly... I'd have been safe... ahaha...\""},
    {"「いたっ……ううぅ……」", "\"Ow... uuugh...\""},
    {"「……一人で生きる自信が……なかったから……」",
     "\"I just... didn't have the confidence to live alone...\""},
    {"「だからもしかしたら霧が生きてるって、思って……」",
     "\"So I thought maybe Kiri was still alive...\""},
    {"「でもわたし……焼き払う準備もしてきた……」",
     "\"But I... also came prepared to burn it all down...\""},
    {"「あははははははははは」", "\"Ahahahahahahahaha.\""},
    {"「あはははは、はは、はははは……」", "\"Ahahahaha, haha, hahahaha...\""},
    {"「うわあああああッッッ！！」", "\"Uwaaaaaahhh!!\""},
    {"「……うぐっ」", "\"...Urgh.\""},
    {"「好きだったのに───」", "\"Even though I liked you---\""},
    {"「好きだったのに―――」", "\"Even though I liked you---\""},
    {"「……先輩のこと、ちょっと好きだったのに……」",
     "\"Even though I... kind of liked you, senpai...\""},

    // --- Stripped-prefix lines where stripping drops semantic prefixes -------
    {"%N一緒に食べたい一緒に食べたい一緒に食べたい一緒に食べたい。",
     "%NI want to eat together, I want to eat together, I want to eat together, I want to eat together."},
    {"30階建てのビルの螺旋階段を一気に駆け降りた後のように、三半規管が揺れている。",
     "My semicircular canals are spinning, like I just sprinted down the spiral staircase of a 30-story building."},
    {"FLOWER'S、解散の危機である。", "FLOWER'S is facing a breakup crisis."},
    {"FLOWER'S……いつも一緒の一組の花。ずっと一対の花であると、無邪気に信じている子たち……そんなものは、いずれ一方的に断ち切られるのに。",
     "FLOWER'S... a pair of flowers, always together. Girls who innocently believe they'll stay a matched pair forever... even though that bond will one day be severed from one side."},
    {"FLOWER'Sのもう片方。", "The other half of FLOWER'S."},
    {"FLOWER'Sの二人が、下着姿でベッドに寝そべる。",
     "The two of FLOWER'S sprawl across the bed in their underwear."},
    {"FLOWER'Sの二人も心配そうについてきた。",
     "The two from FLOWER'S followed along anxiously, too."},
    {"FLOWER'Sを２人同時に────しかも、裸同然の姿を並べて、思いのままに触診できるチャンスが、勝手に転がってきたのだ。",
     "A chance to palpate both of FLOWER'S at once---practically naked, side by side, to my heart's content---had just fallen right into my lap."},
    {"FLOWER'Sを２人同時に――――しかも、裸同然の姿を並べて、思いのままに触診できるチャンスが、勝手に転がってきたのだ。",
     "A chance to palpate both of FLOWER'S at once---practically naked, side by side, to my heart's content---had just fallen right into my lap."},
    {"２人合わせてFLOWER'S。", "Together, the two of them are FLOWER'S."},
    {"０。", "0."},
    {"０．１秒で起動だ。脳内だから。", "Booted in 0.1 seconds. All in my head, after all."},
    {"０．５秒で奈落に叩き戻した。", "Smashed it back into the abyss in 0.5 seconds."},
    {"１。", "1."},
    {"１００％、彼女のために殺ったのだ。", "I killed 100% for her sake."},
    {"１０００円也。", "1,000 yen flat."},
    {"１０勝０敗だった。", "10 wins, 0 losses."},
    {"１２時にセットされてる。", "It's set for 12 o'clock."},
    {"１６回戦は、いくらエロの権化たる俺でもキツイ。",
     "16 rounds is rough even for me, the incarnation of eros."},
    {"１と題されたノートを開いてみる。", "I open the notebook titled '1'."},
    {"１クラスで、１０人くらいだろうか。", "About 10 students per class, maybe?"},
    {"１億円の分け前のためなら、霧といえども俺にキスぐらいはするだろう。",
     "For a cut of 100 million yen, even Kiri would at least give me a kiss."},
    {"１年の教室に向かう途中、霧を発見。",
     "On my way to the first-year classroom, I spot Kiri."},
    {"１分後。", "1 minute later."},
    {"２。", "2."},
    {"２０代の頃ＪＲへの就職に失敗し時代遅れのヤンキーへと失墜した氏だが（三十路までこの愚行は続いた）、ご両親の死後しばらくを経て、険の取れた豊かな顔つきへと変わっていった。",
     "In his twenties, after failing to land a job at JR, he degenerated into an outdated delinquent (a folly that lasted until thirty), yet some time after his parents passed away, his harsh features softened into a warm, gentle expression."},
    {"２１世紀、世紀末をはずして人類滅亡までしている定番崩しがもてはやされる昨今、そこまであからさまなのはないだろうと何の根拠もなく思う。",
     "In the 21st century, when subverting tropes by skipping the fin-de-siecle apocalypse and wiping out humanity anyway is all the rage, I baselessly figure it couldn't be anything that blatant."},
    {"２１世紀だぞ？", "It's the 21st century, you know?"},
    {"２１世紀ですよ？", "This is the 21st century, you know?"},
    {"２４時間作業しているわけでもないのか。", "Guess they aren't working 24 hours a day."},
    {"２リットルの天然水。", "Two liters of mineral water."},
    {"２億４千万画素ムービーとして、アイボールカメラが記録していく。",
     "My eyeball camera records it all as a 240-megapixel movie."},
    {"２年の教室近くの廊下を、意味もなく歩く。",
     "I walk aimlessly down the hallway near the second-year classrooms."},
    {"２年教室へ行ってみる。", "I head over to the second-year classroom."},
    {"２分後。", "2 minutes later."},
    {"３。", "3."},
    {"３０代の頃は泣く子を殺すと言われるほどに悪顔だった氏だが、ご両親の死後しばらくを経て、険の取れた豊かな顔つきへと変わっていった。",
     "In his thirties, his face was said to be fierce enough to scare a crying child to death, yet some time after his parents passed away, his harsh features softened into a warm, gentle expression."},
    {"３０分後──", "30 minutes later---"},
    {"３０分後――", "30 minutes later---"},
    {"３日後──", "3 days later---"},
    {"３日後――", "3 days later---"},
    {"３分後。", "3 minutes later."},
    {"５０万だと。", "500,000, huh."},
    {"７／８ならまあＯＫか、と俺は思っていた。", "7 out of 8 is good enough, I figured."},
    {"７人集めるのに。", "Just to gather seven people."},
    {"８４％───", "84%---"},
    {"８４％―――", "84%---"},
    {"８つの気配。", "Eight presences."},
    {"８の字を描く。", "Tracing a figure eight."},
    {"８人しか、いないのに。", "Even though there are only eight of us."},
    {"９９９１。", "9991."},
    {"９月１２日。", "September 12."},
    {"９月１２日の午後。ようやくアンテナの修理は完成した。",
     "Afternoon of September 12. At last, repairs on the antenna were complete."},
    {"９月５日午後６時　カレーパン刑事殉職",
     "September 5, 6:00 PM - Detective Curry Bread Killed in Action"},
    {"ＡＫ４７から空になった弾倉を取り外す。",
     "I detach the spent magazine from the AK-47."},
    {"Ａ定食は三時間目には売り切れるから、確実に食べたい時は一時間目の終わりと同時に食堂に赴くことになる。",
     "Set meal A sells out by third period, so if you want to be sure of getting it, you have to head to the cafeteria the moment first period ends."},
    {"Ｂ定食の食券を買い、カウンターに持っていく。",
     "I buy a meal ticket for Set B and take it to the counter."},
    {"ＣＲＯＳＳ。", "CROSS."},
    {"Ｃランチのトレイを抱えた友貴が、隣に座る。",
     "Tomoki, carrying a Lunch C tray, sits down beside me."},
    {"ＤＡＮＧＥＲＯＵＳ？", "DANGEROUS?"},
    {"ＤＪ、俺が？", "Me, a DJ?"},
    {"Ｈするとき、だいたい一時間はゆるゆると触りあう。",
     "When having sex, we usually spend about an hour slowly touching each other."},
    {"ＩＱが下がった。", "My IQ dropped."},
    {"ＬＤのことが気になって、ラバに話を聞くことにした。",
     "Curious about the LD, I decided to ask Raba about it."},
    {"Ｓ→Ａ→Ｂ→Ｃ→Ｄと区分けされている。", "They're ranked S -> A -> B -> C -> D."},
    {"ＳＭＧが水を噴出した。", "The SMG sprayed a jet of water."},
    {"ＳＭＧを腰だめに構えた先輩は素敵だ。",
     "Senpai looks dashing holding the SMG at his hip."},
    {"ＳＯＳねぇ。", "An SOS, huh."},
    {"ＳＯＳの効果はともかく。", "Leaving aside whether the SOS actually worked."},
    {"Ｓが最強。Ｄが最低。", "S is the highest. D is the lowest."},
    {"Ｓの息子だ。", "He's S's son."},
    {"Ｓは人にさせるのも好きだった。", "S liked making other people do things, too."},
    {"ＷＣ。俺の求めるもの。", "WC. What I seek."},
    {"ＷＣ。人の尊厳。", "WC. Human dignity."},
    {"ＷＣ。人間らしさの九割を占めるもの。",
     "WC. Ninety percent of what makes us human."},
    {"Ｗアイアンクロー炸裂の現状、他に答える余地もなく。",
     "With a Double Iron Claw clamped on, there's no room for any other answer."},
    {"Ｘ組は他のクラスとは違う。", "Class X is different from the other classes."},
    {"ＺＥＲＯ距離。", "ZERO range."},
    {"⑪炊く", "(11) Cook"},
    {"○に入るのは『ビ』だよ、ビ！", "What goes in the O is 'bi'---'bi'!"},
    {"○ンコー！", "*nko!"},
    {"○月×日", "Month X, Day Y"},
    {"＋涙。", "+ Tears."},
    {"＝今の俺が存続できる可能性が減少することを意味する。",
     "= That means the odds of the current me surviving go down."},
    {"†が閃く。", "The cross flashes."},
    {"†が止まる。", "The cross stops."},
    {"────今だ！", "---Now!"},
    {"――――今だ！", "---Now!"},
    {"「……１Ａの１５」", "\"...1-A, #15.\""},
    {"「３０００円」", "\"3,000 yen.\""},

    // --- Stripped speaker tags & short parenthesized tags --------------------
    {"？？？", "???"},
    {"（太一）", "(Taichi)"},
    {"（冬子）", "(Touko)"},
    {"３人", "All Three"},
    {"＊", "*"},
    {"＊＊", "**"},
    {"（笑）", "(laughs)"},
    {"（泣）", "(cries)"},
    {"（怒）", "(angry)"},
    {"（†）", "(+)"},

    // --- All in-game op 0x31 choice options (non-overwriting safety seeds) ---
    {"霧", "Kiri"},
    {"美希", "Miki"},
    {"部室", "Clubroom"},
    {"屋上", "Rooftop"},
    {"廊下", "Hallway"},
    {"教室", "Classroom"},
    {"学食", "Cafeteria"},
    {"帰る", "Go home"},
    {"迫る", "Press closer"},
    {"降伏", "Surrender"},
    {"手伝う", "Help out"},
    {"しない", "Don't"},
    {"ボケる", "Play the fool"},
    {"食べる", "Eat it myself"},
    {"冬子の家", "Touko's house"},
    {"一年教室", "First-year classroom"},
    {"二年教室", "Second-year classroom"},
    {"接近する", "Approach"},
    {"突っ込む", "Play the straight man"},
    {"はり倒す", "Smack him down"},
    {"手当する", "Treat the wound"},
    {"あげない", "Don't give it"},
    {"霧を追う", "Chase after Kiri"},
    {"追わない", "Don't chase her"},
    {"ぶつける", "Vent at her"},
    {"我慢する", "Hold back"},
    {"使用しない", "Don't use it"},
    {"冬子と話す", "Talk to Touko"},
    {"屋上に行く", "Go to the rooftop"},
    {"屋上へ行く", "Go to the rooftop"},
    {"緑茶女帝が", "The Green Tea Empress"},
    {"食堂へ行く", "Go to the cafeteria"},
    {"部室へ行く", "Go to the clubroom"},
    {"それはない", "Not happening"},
    {"贖罪させる", "Make him atone"},
    {"話しかける", "Talk to her"},
    {"部活に行く", "Go to club activities"},
    {"さらに迫る", "Press even closer"},
    {"パンツ見る", "Look at her panties"},
    {"教室に行く", "Go to the classroom"},
    {"桜庭を誘う", "Invite Sakuraba"},
    {"友貴を誘う", "Invite Tomoki"},
    {"一人で食う", "Eat alone"},
    {"学校に行く", "Go to school"},
    {"声をかける", "Call out"},
    {"食べさせる", "Feed her"},
    {"受け入れる", "Accept"},
    {"霧に話を聞く", "Ask Kiri"},
    {"掃除を手伝う", "Help clean"},
    {"受け入れない", "Refuse"},
    {"ためしに攻撃", "Try attacking"},
    {"デートに誘う", "Ask her on a date"},
    {"ミニ電マがな！", "The mini massager, that is!"},
    {"お礼にミニ電マ", "Mini massager as thanks"},
    {"美希に話を聞く", "Ask Miki"},
    {"冬子に話を聞く", "Ask Touko"},
    {"見里に話を聞く", "Ask Misato"},
    {"ラバに話を聞く", "Ask Raba"},
    {"廊下で掃除する", "Clean the hallway"},
    {"祠に行ってみる", "Check out the shrine"},
    {"田崎食料に行く", "Go to Tasaki Groceries"},
    {"校門で霧待ちする", "Wait for Kiri at the school gate"},
    {"誰か他の人と食う", "Eat with someone else"},
    {"部室に行ってみる", "Check the clubroom"},
    {"霧の家につきあう", "Accompany Kiri home"},
    {"ミニ電マを使用する", "Use the mini massager"},
    {"セクハラ ＆ サイン", "Sexual Harassment & Autograph"},
    {"デッド or アライブ", "Dead or Alive"},
    {"サンドイッチあげる", "Give her a sandwich"},
    {"保健室に行ってみる", "Check the infirmary"},
    {"カレーパンで我慢する", "Make do with curry bread"},
    {"二年教室に行ってみる", "Check the second-year classroom"},
    {"一年教室に行ってみる", "Check the first-year classroom"},
    {"パンツパンをちょっと……", "Try the panty bread..."},
    {"しゅーるすとれみんぐパンに挑戦する", "Challenge the surstromming bread"},
    {"ついうっかり不可抗力で偶然胸に手が伸びる",
     "Purely by accident, my hand slips toward her chest"},
    {"不可抗力により偶然スカートがめくれてしまう",
     "By sheer force majeure, her skirt flips up"},
};

void AppendWrappedParagraph(std::string seg,
                            std::size_t wrap_cols,
                            std::string& out) {
    if (wrap_cols == 0) {
        out += seg;
        return;
    }
    while (seg.size() > wrap_cols) {
        std::size_t split = 0;
        for (std::size_t k = wrap_cols; k > 0; --k) {
            if (seg[k] == ' ') {
                split = k;
                break;
            }
        }
        if (split > 0) {
            out.append(seg, 0, split);
            out += "%N";
            std::size_t next = split;
            while (next < seg.size() && seg[next] == ' ') {
                ++next;
            }
            seg.erase(0, next);
        } else {
            out.append(seg, 0, wrap_cols);
            out += "%N";
            seg.erase(0, wrap_cols);
        }
    }
    out += seg;
}

}  // namespace

int SeedScriptMenuTranslations(TranslationMap& out) {
    int inserted = 0;
    for (const auto& entry : kScriptMenuSeedTable) {
        const std::string key = Utf8ToCp932(entry.jp_utf8);
        if (!key.empty() && out.emplace(key, entry.en).second) {
            ++inserted;
        }
    }
    return inserted;
}

std::size_t BuildChoiceBuffer(const unsigned char* cursor,
                              std::size_t max_in_bytes,
                              unsigned char* out_buf,
                              std::size_t out_capacity,
                              const TranslationMap& translations,
                              ChoicePrefixLookupFn prefix_lookup) {
    if (!cursor || !out_buf ||
        max_in_bytes < kChoiceHeaderBytes ||
        out_capacity < kChoiceHeaderBytes) {
        return 0;
    }

    const std::size_t count =
        static_cast<std::size_t>(cursor[2]) |
        (static_cast<std::size_t>(cursor[3]) << 8);
    if (count == 0 || count > kMaxChoiceOptions) {
        return 0;
    }

    const unsigned char* const in_end  = cursor + max_in_bytes;
    unsigned char* const       out_end = out_buf + out_capacity;
    const unsigned char*       in      = cursor;
    unsigned char*             out     = out_buf;

    std::memcpy(out, in, kChoiceHeaderBytes);
    in  += kChoiceHeaderBytes;
    out += kChoiceHeaderBytes;

    constexpr std::size_t kMaxOptionPayload = kMaxChoiceOptionOutBytes - 1;

    for (std::size_t i = 0; i < count; ++i) {
        if (static_cast<std::size_t>(in_end - in) < kChoiceOptionPreambleBytes + 1) {
            return 0;
        }

        const unsigned char* const opt_text = in + kChoiceOptionPreambleBytes;
        const std::size_t rem_in = static_cast<std::size_t>(in_end - opt_text);

        std::size_t src_len = 0;
        while (src_len < rem_in &&
               src_len <= kMaxChoiceOptionSrcBytes &&
               opt_text[src_len] != 0) {
            ++src_len;
        }
        if (src_len >= rem_in ||
            src_len > kMaxChoiceOptionSrcBytes ||
            opt_text[src_len] != 0) {
            return 0;
        }

        const std::string raw_opt(reinterpret_cast<const char*>(opt_text), src_len);
        std::string resolved;
        bool have_match = false;

        if (!raw_opt.empty()) {
            if (const std::string* hit = LookupWithStrippedPrefix(translations, raw_opt)) {
                resolved = *hit;
                have_match = true;
            }
        }

        if (!have_match && !raw_opt.empty()) {
            std::string trimmed = raw_opt;
            while (!trimmed.empty() && trimmed.back() == ' ') {
                trimmed.pop_back();
            }
            if (!trimmed.empty()) {
                if (trimmed.size() != raw_opt.size()) {
                    if (const std::string* hit =
                            LookupWithStrippedPrefix(translations, trimmed)) {
                        resolved = *hit;
                        have_match = true;
                    }
                }
                if (!have_match && prefix_lookup && prefix_lookup(trimmed, resolved)) {
                    have_match = true;
                }
            }
            if (!have_match) {
                resolved = HasJPLeadByte(raw_opt.data(), raw_opt.size())
                               ? raw_opt
                               : trimmed;
            }
        }

        if (resolved.size() > kMaxOptionPayload) {
            resolved = FitToSlot(resolved, kMaxOptionPayload);
        }

        const std::size_t needed_out =
            kChoiceOptionPreambleBytes + resolved.size() + 1;
        if (static_cast<std::size_t>(out_end - out) < needed_out) {
            return 0;
        }

        std::memcpy(out, in, kChoiceOptionPreambleBytes);
        out += kChoiceOptionPreambleBytes;
        if (!resolved.empty()) {
            std::memcpy(out, resolved.data(), resolved.size());
            out += resolved.size();
        }
        *out++ = 0;

        in = opt_text + src_len + 1;
    }

    return static_cast<std::size_t>(in - cursor);
}

std::size_t BuildChoiceBuffer(const unsigned char* cursor,
                              unsigned char* out_buf,
                              std::size_t out_capacity,
                              const TranslationMap& translations,
                              ChoicePrefixLookupFn prefix_lookup) {
    return BuildChoiceBuffer(cursor, kDefaultMaxChoiceInBytes,
                             out_buf, out_capacity,
                             translations, prefix_lookup);
}

bool FormatBacklogSlotText(const std::string& jp_cp932,
                           const TranslationMap& translations,
                           std::string& out_formatted,
                           std::size_t wrap_cols,
                           std::size_t slot_capacity) {
    if (jp_cp932.empty() || slot_capacity < 2) {
        return false;
    }
    const std::string* hit = LookupWithStrippedPrefix(translations, jp_cp932);
    if (!hit || hit->empty()) {
        return false;
    }

    const std::string& src = *hit;
    std::string wrapped;
    wrapped.reserve(src.size() + 16);

    std::size_t pos = 0;
    bool first_paragraph = true;
    while (pos <= src.size()) {
        std::size_t brk = pos;
        std::size_t token_len = 0;
        while (brk < src.size()) {
            if (src[brk] == '%' && brk + 1 < src.size() && src[brk + 1] == 'N') {
                token_len = 2;
                break;
            }
            if (src[brk] == '\n') {
                token_len = 1;
                break;
            }
            ++brk;
        }

        std::string paragraph = src.substr(pos, brk - pos);
        if (!paragraph.empty() && paragraph.back() == '\r') {
            paragraph.pop_back();
        }

        if (!first_paragraph) {
            wrapped += "%N";
        }
        first_paragraph = false;
        AppendWrappedParagraph(std::move(paragraph), wrap_cols, wrapped);

        if (token_len == 0) {
            break;
        }
        pos = brk + token_len;
    }

    const std::size_t max_payload = slot_capacity - 1;
    if (wrapped.size() > max_payload) {
        wrapped = FitToSlot(wrapped, max_payload);
        while (!wrapped.empty() && wrapped.back() == ' ') {
            wrapped.pop_back();
        }
        if (!wrapped.empty() && wrapped.back() == '%') {
            wrapped.pop_back();
        }
        while (wrapped.size() >= 2 &&
               wrapped.compare(wrapped.size() - 2, 2, "%N") == 0) {
            wrapped.erase(wrapped.size() - 2);
        }
    }

    if (wrapped.empty()) {
        return false;
    }
    out_formatted = std::move(wrapped);
    return true;
}

// Lookup with leading-decorator fallback.
//
// The TSV is built from the extract step's control-prefix strip, which walks
// past any leading char that isn't 「」, a Japanese character, …, ―,
// or ～. The engine still emits the un-stripped form at runtime
// ("A定食は…", "※男心を…"), so a literal m.find() misses. Mirror the
// same strip here as a fallback: skip leading non-kept bytes and retry.
namespace {

std::size_t NonKeptCharByteLen(const std::string& jp, std::size_t i) {
    const unsigned char b0 = static_cast<unsigned char>(jp[i]);
    if (b0 < 0x80) return 1;

    if (!IsCp932LeadByte(b0)) return 0;
    if (i + 1 >= jp.size()) return 0;
    const unsigned char b1 = static_cast<unsigned char>(jp[i + 1]);

    if (b0 == 0x81) {
        switch (b1) {
            case 0x40: case 0x41: case 0x42:
            case 0x45: case 0x5B: case 0x5C:
            case 0x60: case 0x63:
            case 0x6B: case 0x6C:
            case 0x75: case 0x76: case 0x77: case 0x78:
            case 0x79: case 0x7A:
                return 0;
            default:
                return 2;
        }
    }
    if (b0 == 0x82) {
        if ((b1 >= 0x4F && b1 <= 0x58) ||
            (b1 >= 0x60 && b1 <= 0x79) ||
            (b1 >= 0x81 && b1 <= 0x9A)) {
            return 2;
        }
        return 0;
    }
    return 0;
}

}  // namespace

const std::string* LookupWithStrippedPrefix(const TranslationMap& m,
                                            const std::string& jp) {
    const auto it = m.find(jp);
    if (it != m.end()) return &it->second;

    constexpr std::size_t kMaxSkipBytes = 16;
    std::size_t i = 0;
    while (i < jp.size() && i < kMaxSkipBytes) {
        const std::size_t step = NonKeptCharByteLen(jp, i);
        if (step == 0) break;
        i += step;
    }
    if (i == 0 || i >= jp.size()) return nullptr;

    const auto it2 = m.find(jp.substr(i));
    if (it2 != m.end()) return &it2->second;
    return nullptr;
}

PatchStats PatchTranslationsInPlace(unsigned char* begin, unsigned char* end,
                                    const TranslationMap& translations) {
    PatchStats s;
    if (!begin || !end || end <= begin) return s;

    for (const auto& kv : translations) {
        const std::string& jp = kv.first;
        const std::string& en = kv.second;
        if (jp.empty()) continue;

        const unsigned char* jpData =
            reinterpret_cast<const unsigned char*>(jp.data());
        const std::size_t jpLen = jp.size();

        unsigned char* it = begin;
        while (it + jpLen < end) {
            unsigned char* found = std::search(it, end, jpData, jpData + jpLen);
            if (found == end) break;

            unsigned char* afterJp = found + jpLen;
            const bool validEnd = (afterJp < end) && (*afterJp == 0);
            const bool validStart = (found == begin) || (*(found - 1) == 0);
            if (validEnd && validStart) {
                std::size_t slotLen = jpLen;
                if (en.size() > jpLen) {
                    const std::size_t need = en.size() - jpLen;
                    unsigned char* scan = found + jpLen + 1;
                    std::size_t avail = 0;
                    while (avail < need && scan + avail < end &&
                           *(scan + avail) == 0) {
                        ++avail;
                    }
                    if (avail >= need) {
                        slotLen = jpLen + 1 + avail;
                    }
                }

                std::string chosen;
                if (en.size() <= slotLen) {
                    chosen = en;
                }
                if (!chosen.empty()) {
                    std::memcpy(found, chosen.data(), chosen.size());
                    if (chosen.size() < slotLen) {
                        std::memset(found + chosen.size(), 0x20,
                                    slotLen - chosen.size());
                    }
                    found[slotLen] = 0;
                    ++s.hits;
                } else {
                    ++s.too_long;
                }
            }
            it = found + 1;
        }
    }
    return s;
}

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            switch (n) {
                case 'n':  out += '\n'; ++i; continue;
                case 'r':  out += '\r'; ++i; continue;
                case 't':  out += '\t'; ++i; continue;
                case '\\': out += '\\'; ++i; continue;
            }
        }
        out += c;
    }
    s.swap(out);
}

int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out) {
    int added = 0;
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && data[eol] != '\n') ++eol;
        size_t line_end = eol;
        if (line_end > i && data[line_end - 1] == '\r') --line_end;

        size_t tab = i;
        while (tab < line_end && data[tab] != '\t') ++tab;

        if (tab < line_end) {
            std::string jp(data + i, tab - i);
            std::string en(data + tab + 1, line_end - tab - 1);
            UnescapeInPlace(jp);
            UnescapeInPlace(en);
            if (!jp.empty() && !en.empty()) {
                const std::string key = Utf8ToCp932(jp);
                out[key] = en;
                ++added;
            }
        }

        i = eol + 1;
    }
    return added;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out) {
    if (path.empty()) {
        return false;
    }
    std::ifstream f;
    try {
        f.open(std::filesystem::u8path(path), std::ios::binary);
    } catch (const std::exception&) {
        return false;
    }
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string buf = ss.str();
    const int n = ParseTsvBuffer(buf.data(), buf.size(), out);
    return n > 0;
}

bool IsCp932LeadByte(unsigned char b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

bool HasJPLeadByte(const char* data, size_t len) {
    for (size_t i = 0; i + 1 < len; ++i) {
        const unsigned char b = static_cast<unsigned char>(data[i]);
        if (IsCp932LeadByte(b)) return true;
    }
    return false;
}

std::string Utf8ToCp932(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                       static_cast<int>(utf8.size()),
                                       nullptr, 0);
    if (wn <= 0) return utf8;
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    const int w_written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                              static_cast<int>(utf8.size()),
                                              &w[0], wn);
    if (w_written != wn) return utf8;
    const int bn = WideCharToMultiByte(932, 0, w.data(), wn, nullptr, 0,
                                       "?", nullptr);
    if (bn <= 0) return utf8;
    std::string out(static_cast<std::size_t>(bn), '\0');
    const int b_written = WideCharToMultiByte(932, 0, w.data(), wn, &out[0], bn,
                                              "?", nullptr);
    if (b_written != bn) return utf8;
    return out;
#else
    return utf8;
#endif
}

std::string Cp932ToUtf8(const std::string& cp932) {
#ifdef _WIN32
    if (cp932.empty()) return {};
    const int wn = MultiByteToWideChar(932, 0, cp932.data(),
                                       static_cast<int>(cp932.size()),
                                       nullptr, 0);
    if (wn <= 0) return cp932;
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    const int w_written = MultiByteToWideChar(932, 0, cp932.data(),
                                              static_cast<int>(cp932.size()),
                                              &w[0], wn);
    if (w_written != wn) return cp932;
    const int bn = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), wn, nullptr, 0,
                                       nullptr, nullptr);
    if (bn <= 0) return cp932;
    std::string out(static_cast<std::size_t>(bn), '\0');
    const int b_written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), wn, &out[0], bn,
                                              nullptr, nullptr);
    if (b_written != bn) return cp932;
    return out;
#else
    return cp932;
#endif
}

}  // namespace translator_logic
