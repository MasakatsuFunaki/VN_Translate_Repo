// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/utf8_text.h"
#include "st_process_harness.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

using test_support::TempSandbox;
using test_support::locate_installed_spot_check_exe;
using test_support::run_process_utf8;

constexpr DWORD kCliTimeoutMs = 30000;
constexpr std::size_t kStandardScanWindow = 16;
constexpr std::size_t kCompactScanWindow = 12;
constexpr std::size_t kWideScanWindow = 24;

constexpr std::size_t kBaselineScriptLines = 48;
constexpr std::size_t kPureVocalHSceneLines = 34;
constexpr std::size_t kLiteraryHSceneLines = 32;
constexpr std::size_t kNormalDramaLines = 28;
constexpr std::size_t kNormalLibraryLines = 24;

constexpr std::size_t kVocalDefectIdxA = 8;
constexpr std::size_t kVocalDefectIdxB = 14;
constexpr std::size_t kVocalDefectIdxC = 21;
constexpr std::size_t kVocalDefectIdxD = 27;
constexpr std::size_t kDramaGaspIdx = 8;
constexpr std::size_t kDramaSighIdx = 9;

constexpr std::size_t kLiteraryDefectIdxA = 7;
constexpr std::size_t kLiteraryDefectIdxB = 15;
constexpr std::size_t kLiteraryDefectIdxC = 22;
constexpr std::size_t kLibraryDefectStart = 8;
constexpr std::size_t kLibraryDefectEnd = 12;

constexpr std::size_t kTransitionScriptLines = 120;
constexpr std::size_t kTransitionHStart = 36;
constexpr std::size_t kTransitionHEnd = 68;
constexpr std::size_t kTransitionPreDefectStart = 10;
constexpr std::size_t kTransitionPreDefectEnd = 13;
constexpr std::size_t kTransitionPostDefectStart = 92;
constexpr std::size_t kTransitionPostDefectEnd = 95;
constexpr std::size_t kTransitionInteriorDefectOffset = 14;
constexpr std::size_t kTransitionTailDefectBackset = 2;
constexpr std::size_t kNarrationCadenceModulo = 3;
constexpr std::size_t kNarrationCadenceRemainder = 2;

constexpr std::size_t kShortOpeningLines = 12;
constexpr std::size_t kShortOpeningDefectStart = 4;
constexpr std::size_t kShortOpeningDefectEnd = 7;
constexpr std::size_t kAbruptHSceneLines = 32;

constexpr std::size_t kDirtyBanterScriptLines = 36;
constexpr std::size_t kDirtyBanterBurstStart = 14;
constexpr std::size_t kDirtyBanterBurstEnd = 19;

constexpr std::size_t kBluntHSceneLines = 30;
constexpr std::size_t kBluntDefectIdxA = 6;
constexpr std::size_t kBluntDefectIdxB = 14;
constexpr std::size_t kBluntDefectIdxC = 22;

constexpr std::size_t kCombatChaseLines = 32;
constexpr std::size_t kDarkTraumaLines = 30;
constexpr std::size_t kSubSceneDefectStart = 12;
constexpr std::size_t kSubSceneDefectEnd = 16;

struct DialogueLine {
    std::string_view speaker;
    std::string_view jp;
    std::string_view en;
};

struct ScriptBlock {
    std::string_view name;
    std::vector<DialogueLine> lines;
};

constexpr std::array<DialogueLine, 12> kCleanBaselinePool{{
    {"蒼井 遥",
     "「おはよう、今朝は駅前まで歩いてきたから少し目が覚めたよ」",
     "\"Good morning. Walking all the way to the station woke me right up.\""},
    {"",
     "放課後の教室には夕陽が差し込み、静かな風が白いカーテンを揺らしていた。",
     "Evening sunlight slanted across the classroom after school, and a quiet breeze stirred the white curtains."},
    {"高瀬 蓮",
     "「次の各駅停車が来るまでホームのベンチで待とうか」",
     "\"Shall we wait on the platform bench until the next local train arrives?\""},
    {"",
     "古い石段を登りきった先には小さな展望台があり、夕暮れ時の港町と遠くの灯台が一望できた。",
     "At the top of the worn stone steps stood a small observation deck overlooking the harbor town at dusk and the distant lighthouse."},
    {"蒼井 遥",
     "「熱い紅茶を淹れたから、冷めないうちにカップを取ってね」",
     "\"I brewed a pot of hot black tea, so grab a cup before it cools down.\""},
    {"",
     "廊下の突き当たりにある音楽室から、誰かが練習しているピアノの旋律がかすかに聞こえてくる。",
     "From the music room at the far end of the hallway drifted the faint melody of someone practicing the piano."},
    {"高瀬 蓮",
     "「明日の朝は七時に校門へ集合すれば余裕で間に合うはずだ」",
     "\"Meeting at the school gate at seven tomorrow morning should get us there with plenty of time.\""},
    {"",
     "雨上がりの舗道には街灯の光が幾重にも反射し、通り過ぎる自転車のタイヤが水たまりを弾いていた。",
     "Streetlight reflections shimmered across the wet pavement after the rain while passing bicycle tires splashed through shallow puddles."},
    {"蒼井 遥",
     "「先週の講義でまとめたノート、試験が終わるまで貸してあげるよ」",
     "\"You can borrow my notes from last week's lecture until exams wrap up.\""},
    {"",
     "商店街の時計屋が八時の鐘を鳴らす頃には、買い物客の足音もすっかりまばらになっていた。",
     "By the time the clockmaker's shop on the arcade chimed eight, the footsteps of evening shoppers had grown sparse."},
    {"高瀬 蓮",
     "「折りたたみ傘を鞄に入れておいて正解だったな、急に雲が厚くなってきた」",
     "\"Keeping a folding umbrella in my bag paid off; those clouds thickened fast.\""},
    {"",
     "木製の本棚には背表紙の色褪せた辞書や古い天体図鑑が隙間なく並び、乾いた紙の匂いが漂っていた。",
     "Faded dictionaries and old star atlases lined every shelf of the wooden bookcase, filling the quiet room with the scent of dry paper."},
}};

// Pure vocalization / moan / kiss / fluid onomatopoeia lines (NO `(Ｈ)` speaker tags,
// NO anatomical nouns — purely erotic vocalizations, heart/music symbols, and wet SFX).
constexpr std::array<DialogueLine, 10> kPureVocalErogePool{{
    {"蒼井 遥",
     "「んっ……あっ、はぁっ……そんなふうに耳元で息をされたら……♡」",
     "\"Nngh... aah, haaah... if you breathe right against my ear like that...\""},
    {"",
     "んちゅ、ちゅぱ……じゅるるっ、と重なり合った唇から甘い水音が静かな部屋に響く。",
     "*Mmph*, *slurp*... wet kissing sounds echoed softly through the quiet room."},
    {"蒼井 遥",
     "「ふぁぁっ……んぅ、くぅぅっ……やぁ、声が勝手に漏れて……っ♪」",
     "\"Fwaaah... nngh, kuhhh... n-no, my voice keeps slipping out on its own...!\""},
    {"",
     "ぬちゅ、くちゅ……ぴちゃっ、と絡みつくような粘り気のある音が何度も繰り返された。",
     "*Squelch*, *schlick*... sticky, clinging wet sounds repeated again and again."},
    {"蒼井 遥",
     "「あっ、あっ、あんっ……！ひゃんっ、そこばかり強くしないでぇ……♡」",
     "\"Ah, ah, aghhhhh...! Hyan, don't press right there so hard...!\""},
    {"高瀬 蓮",
     "「くぅっ……はぁ、はぁっ……遥の身体、すごく熱くなってるぞ……」",
     "\"Kuhhh... haaah, haaah... Haruka, your whole body is burning up...\""},
    {"蒼井 遥",
     "「んんっ〜！はぁぁ、んちゅ……ぷぁっ、息が、追いつかないよぉ……♡」",
     "\"Mmmmph~! Haaah, mmph... hah, I can't... catch my breath...!\""},
    {"",
     "じゅぷ、ぐちゅっ、ずぷぷ……と深く沈み込むたびに甘い吐息がシーツへ零れ落ちる。",
     "*Squelch*, *slurp*... with every deep sink, sweet gasps spilled across the sheets."},
    {"蒼井 遥",
     "「やぁんっ、あぁぁっ……！くぅん、イッちゃう、頭がふわふわするぅ……♡」",
     "\"Yaaahn, aaaaah...! Kuhhh, I'm gonna... my head feels so dizzy...!\""},
    {"",
     "びゅるっ、びゅるるっ……と熱い震えが伝わり、二人の荒い吐息がいつまでも重なり合っていた。",
     "*Splurt*, *throb*... hot shudders rippled through them as their ragged breaths intertwined."},
}};

// Interleaved 1-dialogue + 2-narration commercial eroge lines mixing literary Japanese
// eroge phrasing (`秘裂`, `蜜壺`, `愛液`, `抽挿`, `結合部`, `白濁`, `絶頂`), blunt terms,
// and brief non-explicit emotional bridge sentences so cluster continuity is tested.
constexpr std::array<DialogueLine, 12> kInterleavedLiteraryErogePool{{
    {"蒼井 遥",
     "「んっ……あぁっ、そんな奥まで一気に来られたら……っ♡」",
     "\"Nngh... aaah, if you push all the way deep inside at once...!\""},
    {"",
     "濡れそぼった秘裂が熱い肉棒を根元まで咥え込み、愛液がシーツを濃く染めていく。",
     "Her drenched slit swallowed his hot shaft to the hilt as slick love juices darkened the sheets."},
    {"",
     "窓の外を叩く雨音さえ遠ざかり、頭の中が真っ白な熱に塗り潰されていくようだった。",
     "Even the sound of rain against the window faded away, her thoughts dissolving into white heat."},
    {"蒼井 遥",
     "「はぁっ、あんっ……！だめ、そこ擦れるたびに腰が跳ねちゃう……♡」",
     "\"Haaah, aghhhhh...! No, my hips jump every time you rub there...!\""},
    {"",
     "柔らかな蜜壺の肉襞が激しくうねり、結合部からじゅるじゅると淫らな水音が溢れ出す。",
     "The soft inner walls of her honeyed folds writhed tightly, lewd wet sounds spilling from where they were joined."},
    {"",
     "指先がシーツを強く握りしめ、繋いだ手のひらにじっとりと汗が滲んでいた。",
     "Her fingertips clutched the bedsheet tightly, damp sweat gathering between their clasped palms."},
    {"高瀬 蓮",
     "「くぅっ……もう限界そうだ、このまま中に出すぞ……！」",
     "\"Kuhhh... I'm at my limit, I'm coming inside you like this...!\""},
    {"",
     "激しい抽挿の果てに硬く尖った乳首が震え、甘い絶頂の波が全身を一気に駆け抜けた。",
     "At the peak of his fierce thrusting her taut nipples trembled, a wave of sweet climax surging through her."},
    {"",
     "視界がきらきらと明滅し、互いの鼓動だけが耳の奥で大きく鳴り響いている。",
     "Her vision flickered brightly while only their racing heartbeats pounded in her ears."},
    {"蒼井 遥",
     "「あぁぁっ〜♡ 中に、熱いのがいっぱい広がってるぅ……っ！」",
     "\"Aaaaah~! Something hot is pouring so deep inside me...!\""},
    {"",
     "脈打つ先端から濃厚な白濁の精液が解き放たれ、震える秘部の奥深くを何度も満たしていった。",
     "Thick white semen pulsed from his tip, filling the trembling depths of her core over and over."},
    {"",
     "嵐が過ぎ去ったあとのような余韻の中で、二人は肩を寄せ合いながら浅い呼吸を繰り返した。",
     "In the lingering afterglow once the storm subsided, the two leaned together breathing shallowly."},
}};

// Combat, injury pain, and breathless sprinting lines (intense `ぐあっ`/`くっ`/`はぁ、はぁ`
// panting & blood/blade imagery, ZERO erotic anchors — must NEVER false-positive as H-scene).
constexpr std::array<DialogueLine, 8> kCombatChasePool{{
    {"高瀬 蓮",
     "「はぁ、はぁ……止まるな、非常階段を抜ければ屋上へ出られるぞ！」",
     "\"Huff, huff... don't stop! Once we clear the fire stairs we can reach the roof!\""},
    {"",
     "荒い呼吸が肺を焼き、濡れたアスファルトを蹴る足音が暗い路地裏に激しく反響した。",
     "Ragged breaths burned their lungs as footsteps kicking off wet asphalt echoed down the dark alley."},
    {"高瀬 蓮",
     "「ぐあっ……！くそっ、脇腹を浅く斬られただけだ、まだ動ける！」",
     "\"Gah...! Damn it, the blade only grazed my side—I can still move!\""},
    {"蒼井 遥",
     "「あっ……蓮くん、袖から血が滲んでるよ！早く手当てしないと……！」",
     "\"Ah... Ren, blood is soaking through your sleeve! We have to bandage it fast...!\""},
    {"",
     "金属の刃が火花を散らして弾け飛び、衝撃で壁際の木箱が音を立てて崩れ落ちた。",
     "Steel blades clashed in a shower of sparks, the impact smashing a stack of wooden crates against the wall."},
    {"高瀬 蓮",
     "「くっ……うわあああっ！この扉さえ封鎖すれば追手は入ってこられない！」",
     "\"Kuh... uwaaaah! Once we barricade this iron door, our pursuers won't break through!\""},
    {"",
     "肩で息をしながら錆びた閂を押し込み、二人は冷たいコンクリート壁に背中を預けて座り込んだ。",
     "Heaving for breath, they jammed the rusted bolt shut and slumped with their backs against the cold concrete wall."},
    {"蒼井 遥",
     "「はぁ、はぁ……心臓が破れそうなくらい鳴ってる……でも、助かったんだね」",
     "\"Huff, huff... my heart is pounding like it might burst... but we made it out alive.\""},
}};

// Dark psychological / bereavement / horror confrontation lines (`CROSS_CHANNEL` /
// `shingakkou` tone: guilt, isolation, rain-drenched rooftop breakdown, zero eroticism).
constexpr std::array<DialogueLine, 8> kDarkPsychologicalPool{{
    {"",
     "鉛色の雲が低く垂れ込める屋上で、錆びた金網だけが冷たい雨風に軋んでいた。",
     "Beneath low leaden clouds on the rooftop, only the rusted chain-link fence creaked in the freezing wind."},
    {"高瀬 蓮",
     "「あの日、僕が保健室の鍵を閉め忘れなければ、誰も傷つかずに済んだのか？」",
     "\"If I hadn't left the infirmary unlocked that day, would nobody have gotten hurt?\""},
    {"",
     "返事のない廊下の暗闇を見つめたまま、震える指先が冷え切った手すりを白く握りしめる。",
     "Staring into the silent darkness of the corridor, trembling fingers gripped the icy railing until white."},
    {"蒼井 遥",
     "「自分を責め続けたって、壊れた時計の針は二度と後ろへは戻らないんだよ」",
     "\"No matter how brutally you blame yourself, the hands of a shattered clock never turn backward.\""},
    {"",
     "割れたガラス片が床の影に散らばり、消毒液の刺すような匂いが鼻腔の奥に張りついていた。",
     "Broken shards of glass lay scattered in the floor shadows while the sharp sting of antiseptic clung to the air."},
    {"高瀬 蓮",
     "「それでも僕は、あそこで見捨てた記憶を抱えたまま明日へ進むしかないんだ」",
     "\"Even so, I have no choice but to walk into tomorrow carrying the memory of abandoning them there.\""},
    {"",
     "遠くで鳴り響く踏切の警報機が、誰もいない放課後の校舎に空虚な余韻を残して消えていく。",
     "A distant railroad crossing alarm echoed across the deserted school building before fading into hollow silence."},
    {"蒼井 遥",
     "「……帰ろう、蓮くん。ここに立ち尽くしていたら心の底まで凍えてしまうから」",
     "\"...Let's go home, Ren. Standing out here any longer will freeze us straight to the bone.\""},
}};

std::string json_escape(std::string_view text) {
    return boost::json::serialize(boost::json::value(text));
}

std::vector<DialogueLine> make_clean_block(std::size_t count) {
    std::vector<DialogueLine> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kCleanBaselinePool[i % kCleanBaselinePool.size()]);
    }
    return out;
}

void inject_high_energy_defect_cluster(std::vector<DialogueLine>& lines,
                                       std::size_t start_idx,
                                       std::size_t end_idx) {
    constexpr std::array<DialogueLine, 4> kDefectTemplates{{
        {"高瀬 蓮",
         "「星見学園の図書室にある古い鍵を先輩に貸してもらったんだ」",
         "\"I lent my (senpai) the old Hoshimi Academy library 鍵 (TL: key).\""},
        {"蒼井 遥",
         "「旧校舎の地下倉庫には壊れた望遠鏡と日誌が残されていたよ」",
         "\"Broken 望遠鏡と日誌 were left inside the basement 倉庫 (Note: storage).\""},
        {"高瀬 蓮",
         "「終電が出る前に駅前の郵便局へこの小包を届けてくれないか」",
         "\"Before the last 終電 departs, deliver this 小包 to the 郵便局.\""},
        {"蒼井 遥",
         "「先生に保健室の予備の毛布を分けてもらえて助かったね」",
         "\"I gave (sensei) a spare 保健室の毛布 and it really saved us.\""},
    }};
    for (std::size_t i = start_idx; i < end_idx && i < lines.size(); ++i) {
        lines[i] = kDefectTemplates[(i - start_idx) % kDefectTemplates.size()];
    }
}

std::string serialize_scripts_json(const std::vector<ScriptBlock>& scripts) {
    std::ostringstream oss;
    oss << "{\r\n";
    for (std::size_t s = 0; s < scripts.size(); ++s) {
        const auto& block = scripts[s];
        oss << " " << json_escape(block.name) << ": {\r\n"
            << "  \"strings\": [\r\n";
        for (std::size_t i = 0; i < block.lines.size(); ++i) {
            const auto& row = block.lines[i];
            oss << "   {\r\n";
            if (row.speaker.empty()) {
                oss << "    \"speaker\": null,\r\n";
            } else {
                oss << "    \"speaker\": " << json_escape(row.speaker) << ",\r\n";
            }
            oss << "    \"text\": " << json_escape(row.jp) << ",\r\n"
                << "    \"translated\": " << json_escape(row.en) << "\r\n"
                << "   }" << (i + 1U < block.lines.size() ? "," : "") << "\r\n";
        }
        oss << "  ]\r\n"
            << " }" << (s + 1U < scripts.size() ? "," : "") << "\r\n";
    }
    oss << "}\r\n";
    return oss.str();
}

bool any_hotspot_overlaps_range(const boost::json::array& ranked_hotspots,
                                std::size_t range_begin,
                                std::size_t range_end) {
    return std::any_of(
        ranked_hotspots.begin(),
        ranked_hotspots.end(),
        [range_begin, range_end](const boost::json::value& item) {
            const auto& obj = item.as_object();
            const auto offset = obj.at("offset").to_number<std::size_t>();
            const auto length = obj.at("length").to_number<std::size_t>();
            const auto end = offset + length;
            return offset < range_end && end > range_begin;
        });
}

bool has_any_tmp_files(const std::filesystem::path& dir) {
    constexpr std::string_view kTmpSuffix = ".tmp";
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(dir, ec)) {
        const auto name = wide_to_utf8(entry.path().filename().wstring());
        if (name.size() >= kTmpSuffix.size() &&
            name.substr(name.size() - kTmpSuffix.size()) == kTmpSuffix) {
            return true;
        }
    }
    return false;
}

boost::json::object run_offline_scan_and_parse(
    const std::filesystem::path& exe,
    const std::filesystem::path& sandbox_root,
    const std::filesystem::path& game_dir,
    const std::filesystem::path& report_path,
    std::size_t window_size,
    bool include_h_scenes) {
    const std::vector<std::pair<std::string, std::string>> env_sentinel{
        {"GEMINI_API_KEY", "offline_sentinel_tripwire_key"}};

    std::vector<std::string> args{
        "--game-dir",
        wide_to_utf8(game_dir.wstring()),
        "--scan-only",
        "--window",
        std::to_string(window_size),
        "--z-threshold",
        "0.0",
        "--out",
        wide_to_utf8(report_path.wstring()),
    };
    if (include_h_scenes) {
        args.push_back("--include-h-scenes");
    }

    const auto res =
        run_process_utf8(exe, args, sandbox_root, kCliTimeoutMs, env_sentinel);
    EXPECT_FALSE(res.launch_failed) << res.launch_error;
    EXPECT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_text;
    EXPECT_TRUE(res.stderr_text.empty()) << res.stderr_text;
    EXPECT_FALSE(res.stdout_text.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(report_path));
    return boost::json::parse(TempSandbox::read_bytes(report_path)).as_object();
}

TEST(HSceneDetectionSystemTest,
     PureVocalizationMoanAndFluidOnomatopoeiaSceneSkippedByDefaultAndUnlockedWithIncludeFlag) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe))
        << "Missing staged packaged binary at: " << wide_to_utf8(exe.wstring());

    TempSandbox sandbox("st_h_pure_vocal");
    const auto game_dir = sandbox.path() / "PureVocalNovel";

    auto baseline = make_clean_block(kBaselineScriptLines);

    // 34-line untagged vocalization/moan/fluid-SFX H-scene with leaked kanji & TL notes
    // embedded directly inside the moan/fluid lines so H-scene continuity stays 100% intact!
    std::vector<DialogueLine> h_vocal;
    h_vocal.reserve(kPureVocalHSceneLines);
    for (std::size_t i = 0; i < kPureVocalHSceneLines; ++i) {
        h_vocal.push_back(kPureVocalErogePool[i % kPureVocalErogePool.size()]);
    }
    ASSERT_EQ(h_vocal.size(), kPureVocalHSceneLines);
    h_vocal[kVocalDefectIdxA] = {
        "蒼井 遥",
        "「んっ……あっ、はぁっ……そんなふうに耳元で熱く囁かれたら……♡」",
        "\"Nngh... aghhhhh, haaah...耳元で囁かれたら (TL: whispered)...!\""};
    h_vocal[kVocalDefectIdxB] = {
        "",
        "んちゅ、ちゅぱ……じゅるるっ、と重なり合った唇から甘い水音が響く。",
        "*Mmph*, *slurp*... 重なり合った唇甘い水音 (Note: wet kiss sfx)."};
    h_vocal[kVocalDefectIdxC] = {
        "蒼井 遥",
        "「あっ、あっ、あんっ……！ひゃんっ、もう声が止まらないよぉ……♡」",
        "\"Aghhhhh, nngh, hyan...! 声が止まらない (onee-chan) haaah...!\""};
    h_vocal[kVocalDefectIdxD] = {
        "",
        "ぬちゅ、くちゅ……びゅるるっ、と熱い震えが伝わり荒い吐息が重なった。",
        "*Squelch*, *splurt*... 熱い震えと荒い吐息が重なった (TL: untamed)."};

    // Normal mystery/drama scene containing isolated non-erotic gasps/sighs + real defects.
    auto drama = make_clean_block(kNormalDramaLines);
    ASSERT_EQ(drama.size(), kNormalDramaLines);
    drama[kDramaGaspIdx] = {
        "蒼井 遥",
        "「あっ……先生、階段の踊り場に生徒会室の封筒が落ちていましたよ！」",
        "\"Ah... Sensei, an envelope from the student council room fell on the landing!\""};
    drama[kDramaSighIdx] = {
        "高瀬 蓮",
        "「はぁ……ようやく文化祭の予算案の集計が終わったな」",
        "\"Sigh... we finally finished tallying the cultural festival budget.\""};
    inject_high_energy_defect_cluster(
        drama, kSubSceneDefectStart, kSubSceneDefectEnd);

    const std::string initial_tt = serialize_scripts_json({
        {"scene_01_baseline", baseline},
        {"scene_02_h_vocal", h_vocal},
        {"scene_03_drama", drama},
    });
    const auto tt_path = sandbox.write_bytes(
        "PureVocalNovel/script_output/translated_text.json", initial_tt);

    constexpr std::size_t kHVocalBegin = kBaselineScriptLines;
    constexpr std::size_t kHVocalEnd = kHVocalBegin + kPureVocalHSceneLines;
    constexpr std::size_t kDramaDefectBegin = kHVocalEnd + kSubSceneDefectStart;
    constexpr std::size_t kDramaDefectEnd = kHVocalEnd + kSubSceneDefectEnd;
    constexpr std::size_t kTotalEntries =
        kBaselineScriptLines + kPureVocalHSceneLines + kNormalDramaLines;

    // 1. Default (`skip_h_scenes == true`): 0 windows overlap `scene_02_h_vocal`;
    //    `scene_03_drama` (with isolated gasp/sigh + defects) IS detected!
    const auto default_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "default_vocal_scan.json",
        kStandardScanWindow,
        false);
    EXPECT_EQ(default_obj.at("total_entries").to_number<std::size_t>(),
              kTotalEntries);
    EXPECT_TRUE(default_obj.at("ols_valid").as_bool());
    const auto& default_hotspots = default_obj.at("ranked_hotspots").as_array();
    ASSERT_GE(default_hotspots.size(), 1U);
    EXPECT_FALSE(
        any_hotspot_overlaps_range(default_hotspots, kHVocalBegin, kHVocalEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        default_hotspots, kDramaDefectBegin, kDramaDefectEnd));

    // 2. Explicit `--include-h-scenes`: unlocks `scene_02_h_vocal` defect cluster!
    const auto optin_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "optin_vocal_scan.json",
        kStandardScanWindow,
        true);
    const auto& optin_hotspots = optin_obj.at("ranked_hotspots").as_array();
    ASSERT_GT(optin_hotspots.size(), default_hotspots.size());
    EXPECT_TRUE(
        any_hotspot_overlaps_range(optin_hotspots, kHVocalBegin, kHVocalEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        optin_hotspots, kDramaDefectBegin, kDramaDefectEnd));

    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(HSceneDetectionSystemTest,
     InterleavedDialogueAndLiteraryErogeNarrationMaintainsClusterContinuity) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_h_literary_interleaved");
    const auto game_dir = sandbox.path() / "LiteraryErogeNovel";

    auto baseline = make_clean_block(kBaselineScriptLines);

    // 32-line untagged interleaved dialogue + literary eroge narration + 1-2 line
    // emotional bridge sentences, starting (`0`) and ending (`31`, `31 % 12 == 7`)
    // on strong erotic anchors, seeded with high-energy defects inside the H-scene.
    std::vector<DialogueLine> h_literary;
    h_literary.reserve(kLiteraryHSceneLines);
    for (std::size_t i = 0; i < kLiteraryHSceneLines; ++i) {
        h_literary.push_back(
            kInterleavedLiteraryErogePool[i % kInterleavedLiteraryErogePool.size()]);
    }
    ASSERT_EQ(h_literary.size(), kLiteraryHSceneLines);
    h_literary[kLiteraryDefectIdxA] = {
        "",
        "濡れそぼった秘裂が熱い肉棒を根元まで咥え込み、愛液がシーツを染めていく。",
        "Her drenched 秘裂 swallowed his hot shaft as 愛液 stained the sheets (TL: literal)."};
    h_literary[kLiteraryDefectIdxB] = {
        "蒼井 遥",
        "「はぁっ、あんっ……！だめ、そこ擦れるたびに腰が跳ねちゃう……♡」",
        "\"Haaah, aghhhhh...! 腰が跳ねちゃう (senpai) every time you rub there...!\""};
    h_literary[kLiteraryDefectIdxC] = {
        "",
        "脈打つ先端から濃厚な白濁の精液が解き放たれ、震える蜜壺の奥深くを満たした。",
        "Thick 白濁の精液 pulsed from his tip, filling the depths of her 蜜壺."};

    auto library_scene = make_clean_block(kNormalLibraryLines);
    inject_high_energy_defect_cluster(
        library_scene, kLibraryDefectStart, kLibraryDefectEnd);

    const std::string initial_tt = serialize_scripts_json({
        {"scene_01_baseline", baseline},
        {"scene_02_h_literary", h_literary},
        {"scene_03_library", library_scene},
    });
    const auto tt_path = sandbox.write_bytes(
        "LiteraryErogeNovel/script_output/translated_text.json", initial_tt);

    constexpr std::size_t kHLiteraryBegin = kBaselineScriptLines;
    constexpr std::size_t kHLiteraryEnd = kHLiteraryBegin + kLiteraryHSceneLines;
    constexpr std::size_t kLibraryGlobalDefectBegin =
        kHLiteraryEnd + kLibraryDefectStart;
    constexpr std::size_t kLibraryGlobalDefectEnd =
        kHLiteraryEnd + kLibraryDefectEnd;

    // Default scan: alternating narration/thought lines do NOT break H-scene span continuity!
    const auto default_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "default_literary_scan.json",
        kStandardScanWindow,
        false);
    const auto& default_hotspots = default_obj.at("ranked_hotspots").as_array();
    ASSERT_GE(default_hotspots.size(), 1U);
    EXPECT_FALSE(any_hotspot_overlaps_range(
        default_hotspots, kHLiteraryBegin, kHLiteraryEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        default_hotspots, kLibraryGlobalDefectBegin, kLibraryGlobalDefectEnd));

    // Opt-in `--include-h-scenes`: detects `scene_02_h_literary` defect window!
    const auto optin_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "optin_literary_scan.json",
        kStandardScanWindow,
        true);
    const auto& optin_hotspots = optin_obj.at("ranked_hotspots").as_array();
    ASSERT_GT(optin_hotspots.size(), default_hotspots.size());
    EXPECT_TRUE(any_hotspot_overlaps_range(
        optin_hotspots, kHLiteraryBegin, kHLiteraryEnd));
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(HSceneDetectionSystemTest,
     SpeakerHTaggedScenesAndMidScriptTransitionsBlockHotspotAndLinearDryRunUntilOptIn) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_h_mid_transition");
    const auto game_dir = sandbox.path() / "MidTransitionNovel";

    // Single 120-line script:
    // - `[0, 36)`: normal pre-H dialogue with defects at `[10, 13)`.
    // - `[36, 68)` (32 lines): mid-script `(Ｈ)` / `(h)` / `Ｈ` speaker-tagged H-scene
    //   interleaved with untagged narration lines, seeded with boundary & interior defects!
    // - `[68, 120)`: post-H morning-after normal dialogue with defects at `[92, 95)`.
    auto lines = make_clean_block(kTransitionScriptLines);
    ASSERT_EQ(lines.size(), kTransitionScriptLines);
    inject_high_energy_defect_cluster(
        lines, kTransitionPreDefectStart, kTransitionPreDefectEnd);
    inject_high_energy_defect_cluster(
        lines, kTransitionPostDefectStart, kTransitionPostDefectEnd);

    constexpr std::array<std::string_view, 3> kHTaggedSpeakers{
        "蒼井 遥（Ｈ）", "Haruka (h)", "雪村 詩織Ｈ"};
    for (std::size_t i = kTransitionHStart; i < kTransitionHEnd; ++i) {
        const std::size_t rel = i - kTransitionHStart;
        if ((rel % kNarrationCadenceModulo) == kNarrationCadenceRemainder) {
            lines[i] = {
                "",
                "シーツの上で重なり合った影が揺れ、浅い吐息が夜の部屋に溶けていった。",
                "Overlapping shadows swayed across the sheets as shallow breaths melted into the night."};
            continue;
        }
        lines[i] = {
            kHTaggedSpeakers[rel % kHTaggedSpeakers.size()],
            "「んっ……もっと近くで抱きしめて、離さないでね……」",
            "\"Nngh... hold me closer and don't let go...\""};
    }
    lines[kTransitionHStart + 1U] = {
        "蒼井 遥（Ｈ）",
        "「んっ……先輩の指先、すごく熱いよ……」",
        "\"Nngh... (senpai) 指先がすごく熱い (TL: hot fingertips)...\""};
    lines[kTransitionHStart + kTransitionInteriorDefectOffset] = {
        "Haruka (h)",
        "「あぁっ……このまま朝までずっと一緒にいて……」",
        "\"Aaah... 朝までずっと一緒にいて (Note: stay until dawn)...\""};
    lines[kTransitionHEnd - kTransitionTailDefectBackset] = {
        "雪村 詩織Ｈ",
        "「はぁっ……まだ胸の鼓動が鳴りやまないの……」",
        "\"Haaah... 胸の鼓動が鳴りやまない (onee-chan)...\""};

    const std::string initial_tt =
        serialize_scripts_json({{"scene_transition", lines}});
    const auto tt_path = sandbox.write_bytes(
        "MidTransitionNovel/script_output/translated_text.json", initial_tt);

    // 1. Default `--scan-only`: detects pre-H and post-H defects, skips `[36, 68)`!
    const auto default_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "transition_default.json",
        kStandardScanWindow,
        false);
    const auto& default_hotspots = default_obj.at("ranked_hotspots").as_array();
    ASSERT_GE(default_hotspots.size(), 2U);
    EXPECT_TRUE(any_hotspot_overlaps_range(
        default_hotspots, kTransitionPreDefectStart, kTransitionPreDefectEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        default_hotspots, kTransitionPostDefectStart, kTransitionPostDefectEnd));
    EXPECT_FALSE(any_hotspot_overlaps_range(
        default_hotspots, kTransitionHStart, kTransitionHEnd));

    // 2. Default `--mode linear --offset 36 --window 16 --max-windows 2 --dry-run`
    //    with sentinel dummy key: both 16-line windows (`[36, 52)` and `[52, 68)`) lie
    //    inside the sustained H-scene, so 0 network calls fire and `h_windows_skipped >= 1`!
    const auto linear_dry_out = sandbox.path() / "transition_linear_dry.json";
    const auto linear_res = run_process_utf8(
        exe,
        {"--game-dir",
         wide_to_utf8(game_dir.wstring()),
         "--mode",
         "linear",
         "--offset",
         std::to_string(kTransitionHStart),
         "--window",
         std::to_string(kStandardScanWindow),
         "--max-windows",
         "2",
         "--radar-calls",
         "0",
         "--dry-run",
         "--out",
         wide_to_utf8(linear_dry_out.wstring())},
        sandbox.path(),
        kCliTimeoutMs,
        {{"GEMINI_API_KEY", "offline_sentinel_tripwire_key"}});
    ASSERT_FALSE(linear_res.launch_failed) << linear_res.launch_error;
    ASSERT_FALSE(linear_res.timed_out);
    EXPECT_EQ(linear_res.exit_code, 0) << "stderr: " << linear_res.stderr_text;
    EXPECT_TRUE(linear_res.stderr_text.empty()) << linear_res.stderr_text;
    ASSERT_TRUE(std::filesystem::is_regular_file(linear_dry_out));
    const auto linear_obj =
        boost::json::parse(TempSandbox::read_bytes(linear_dry_out)).as_object();
    EXPECT_EQ(linear_obj.at("windows_checked").to_number<std::size_t>(), 0U);
    EXPECT_GE(linear_obj.at("h_windows_skipped").to_number<std::size_t>(), 1U);
    EXPECT_EQ(linear_obj.at("token_usage")
                  .as_object()
                  .at("api_calls")
                  .to_number<std::size_t>(),
              0U);

    // 3. Opt-in `--scan-only --include-h-scenes`: unlocks `[36, 68)`!
    const auto optin_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "transition_optin.json",
        kStandardScanWindow,
        true);
    const auto& optin_hotspots = optin_obj.at("ranked_hotspots").as_array();
    EXPECT_TRUE(any_hotspot_overlaps_range(
        optin_hotspots, kTransitionHStart, kTransitionHEnd));
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(HSceneDetectionSystemTest,
     ShortPreHSceneOpeningSmallerThanWindowSizeIsClampedWithoutOverlappingHScene) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_h_short_opening_clamp");
    const auto game_dir = sandbox.path() / "ShortOpeningNovel";

    auto baseline = make_clean_block(kBaselineScriptLines);

    // Script `scene_abrupt`: 12 normal dialogue lines (`< kWideScanWindow == 24`)
    // containing defects at `[4, 7)`, immediately followed mid-script by a 32-line
    // untagged vocalization/eroge H-scene `[12, 44)`!
    auto abrupt = make_clean_block(kShortOpeningLines + kAbruptHSceneLines);
    ASSERT_EQ(abrupt.size(), kShortOpeningLines + kAbruptHSceneLines);
    inject_high_energy_defect_cluster(
        abrupt, kShortOpeningDefectStart, kShortOpeningDefectEnd);
    for (std::size_t i = 0; i < kAbruptHSceneLines; ++i) {
        abrupt[kShortOpeningLines + i] =
            kPureVocalErogePool[i % kPureVocalErogePool.size()];
    }
    abrupt[kShortOpeningLines + kVocalDefectIdxA] = {
        "蒼井 遥",
        "「んっ……あっ、はぁっ……声が漏れて止まらないよぉ……♡」",
        "\"Nngh... aghhhhh, haaah... 声が漏れて止まらない (TL: moan)...!\""};

    const std::string initial_tt = serialize_scripts_json({
        {"scene_01_baseline", baseline},
        {"scene_02_abrupt", abrupt},
    });
    const auto tt_path = sandbox.write_bytes(
        "ShortOpeningNovel/script_output/translated_text.json", initial_tt);

    constexpr std::size_t kOpeningGlobalBegin = kBaselineScriptLines;
    constexpr std::size_t kOpeningDefectGlobalBegin =
        kOpeningGlobalBegin + kShortOpeningDefectStart;
    constexpr std::size_t kOpeningDefectGlobalEnd =
        kOpeningGlobalBegin + kShortOpeningDefectEnd;
    constexpr std::size_t kAbruptHGlobalBegin =
        kOpeningGlobalBegin + kShortOpeningLines;
    constexpr std::size_t kAbruptHGlobalEnd =
        kAbruptHGlobalBegin + kAbruptHSceneLines;

    // Even with `--window 24` (`> 12` normal opening lines), the `[48, 60)` normal
    // opening defect window IS selected (clamped to `<= 12` lines) while 0 lines of
    // `[60, 92)` H-scene are ever included!
    const auto default_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "short_opening_default.json",
        kWideScanWindow,
        false);
    const auto& hotspots = default_obj.at("ranked_hotspots").as_array();
    ASSERT_GE(hotspots.size(), 1U);
    EXPECT_TRUE(any_hotspot_overlaps_range(
        hotspots, kOpeningDefectGlobalBegin, kOpeningDefectGlobalEnd));
    EXPECT_FALSE(any_hotspot_overlaps_range(
        hotspots, kAbruptHGlobalBegin, kAbruptHGlobalEnd));
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(HSceneDetectionSystemTest,
     DirtyBanterJokesAndShortExplicitExchangesAreNeverSkippedByDefault) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_dirty_banter_kept");
    const auto game_dir = sandbox.path() / "DirtyBanterNovel";

    auto baseline = make_clean_block(kBaselineScriptLines);
    auto banter = make_clean_block(kDirtyBanterScriptLines);
    ASSERT_EQ(banter.size(), kDirtyBanterScriptLines);

    // 5 consecutive lines (`[14, 19)`) of crude dirty jokes containing blunt explicit
    // words (`まんこ`, `ちんぽ`, `フェラ`, `中出し`, `cock`, `pussy`, `vagina`, `cum`,
    // `penis`, `clit`, `dick`, `blowjob`, `orgasm`) inside normal comedic banter,
    // seeded with translation defects so they MUST be flagged by default
    // (`skip_h_scenes == true`) and NEVER treated as a sustained H-scene!
    constexpr std::array<DialogueLine, 5> kDirtyBanterBurst{{
        {"高瀬 蓮",
         "「お前、部室のロッカーにフェラだの中出しだの書いてあるエロ本隠してただろ！」",
         "\"Didn't the class rep catch you hiding a blowjob and cum manga in the clubroom ロッカー?\""},
        {"蒼井 遥",
         "「ちょっと、朝からちんぽだのまんこだの最悪な下ネタを教室で大声で言わないでよ！」",
         "\"Hey, quit yelling crude jokes about cock, dick, and pussy first thing in the 朝 (senpai)!\""},
        {"高瀬 蓮",
         "「いや、保健体育のノートにペニスとヴァギナの単語テストがあるって騒いだのはあいつだぞ」",
         "\"Look, he's the idiot who yelled about penis, vagina, and clit on the health quiz in the 廊下 (TL: hall)!\""},
        {"蒼井 遥",
         "「どっちも同罪だよ、絶頂だの何だの変態扱いされたくなかったら少しは口を慎みなさい」",
         "\"You're both guilty! Quit talking about orgasm unless you want everyone calling you a 変態.\""},
        {"高瀬 蓮",
         "「悪かったって、放課後に購買の焼きそばパンおごるから許してくれ」",
         "\"Alright, my bad—I'll buy you yakisoba bread from the 購買部 after class, so forgive me.\""},
    }};
    for (std::size_t i = 0; i < kDirtyBanterBurst.size(); ++i) {
        banter[kDirtyBanterBurstStart + i] = kDirtyBanterBurst[i];
    }

    const std::string initial_tt = serialize_scripts_json({
        {"scene_01_baseline", baseline},
        {"scene_02_dirty_banter", banter},
    });
    const auto tt_path = sandbox.write_bytes(
        "DirtyBanterNovel/script_output/translated_text.json", initial_tt);

    constexpr std::size_t kBanterBurstGlobalBegin =
        kBaselineScriptLines + kDirtyBanterBurstStart;
    constexpr std::size_t kBanterBurstGlobalEnd =
        kBaselineScriptLines + kDirtyBanterBurstEnd;

    const auto default_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "banter_default_scan.json",
        kCompactScanWindow,
        false);
    EXPECT_EQ(default_obj.at("h_windows_skipped").to_number<std::size_t>(), 0U);
    const auto& hotspots = default_obj.at("ranked_hotspots").as_array();
    ASSERT_GE(hotspots.size(), 1U);
    EXPECT_TRUE(any_hotspot_overlaps_range(
        hotspots, kBanterBurstGlobalBegin, kBanterBurstGlobalEnd));
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(HSceneDetectionSystemTest,
     SustainedCombatPainBreathlessnessAndDarkPsychologicalScenesNeverFalsePositiveAsHScenes) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_combat_dark_kept");
    const auto game_dir = sandbox.path() / "CombatAndDarkNovel";

    auto baseline = make_clean_block(kBaselineScriptLines);

    // 32-line sustained combat, injury pain, and breathless sprinting scene (`ぐあっ`,
    // `くっ`, `はぁ、はぁ`, `"Gah!"`, `"Kuh..."`, `"Huff, huff..."`) with defects!
    std::vector<DialogueLine> combat_scene;
    combat_scene.reserve(kCombatChaseLines);
    for (std::size_t i = 0; i < kCombatChaseLines; ++i) {
        combat_scene.push_back(kCombatChasePool[i % kCombatChasePool.size()]);
    }
    inject_high_energy_defect_cluster(
        combat_scene, kSubSceneDefectStart, kSubSceneDefectEnd);

    // 30-line sustained dark psychological / bereavement / rooftop breakdown scene with defects!
    std::vector<DialogueLine> dark_scene;
    dark_scene.reserve(kDarkTraumaLines);
    for (std::size_t i = 0; i < kDarkTraumaLines; ++i) {
        dark_scene.push_back(
            kDarkPsychologicalPool[i % kDarkPsychologicalPool.size()]);
    }
    inject_high_energy_defect_cluster(
        dark_scene, kSubSceneDefectStart, kSubSceneDefectEnd);

    const std::string initial_tt = serialize_scripts_json({
        {"scene_01_baseline", baseline},
        {"scene_02_combat_chase", combat_scene},
        {"scene_03_dark_trauma", dark_scene},
    });
    const auto tt_path = sandbox.write_bytes(
        "CombatAndDarkNovel/script_output/translated_text.json", initial_tt);

    constexpr std::size_t kCombatDefectBegin =
        kBaselineScriptLines + kSubSceneDefectStart;
    constexpr std::size_t kCombatDefectEnd =
        kBaselineScriptLines + kSubSceneDefectEnd;
    constexpr std::size_t kDarkDefectBegin =
        kBaselineScriptLines + kCombatChaseLines + kSubSceneDefectStart;
    constexpr std::size_t kDarkDefectEnd =
        kBaselineScriptLines + kCombatChaseLines + kSubSceneDefectEnd;

    const auto default_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "combat_dark_default_scan.json",
        kStandardScanWindow,
        false);
    EXPECT_EQ(default_obj.at("h_windows_skipped").to_number<std::size_t>(), 0U);
    const auto& hotspots = default_obj.at("ranked_hotspots").as_array();
    ASSERT_GE(hotspots.size(), 2U);
    EXPECT_TRUE(any_hotspot_overlaps_range(
        hotspots, kCombatDefectBegin, kCombatDefectEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        hotspots, kDarkDefectBegin, kDarkDefectEnd));
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(HSceneDetectionSystemTest,
     BluntExplicitLexiconHSceneWithoutHeartsOrLiteraryTermsSkippedByDefaultAndUnlockedWithIncludeFlag) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_h_blunt_explicit");
    const auto game_dir = sandbox.path() / "BluntExplicitNovel";

    auto baseline = make_clean_block(kBaselineScriptLines);

    // 1. `scene_02_h_blunt_bilingual` (30 lines): NO `(Ｈ)` speaker tags, NO `♡`/`♪`,
    //    NO literary `秘裂`/`蜜壺`, and NO wet SFX (`んちゅ`/`じゅる`/`slurp`) — anchored
    //    strictly by blunt JP/EN explicit words (`まんこ`, `ちんぽ`, `フェラ`, `中出し`,
    //    `cock`, `pussy`, `vagina`, `cum`, `penis`, `clit`, `dick`, `blowjob`, `orgasm`)
    //    interleaved with 1-line plain bedroom narration!
    constexpr std::array<DialogueLine, 6> kBluntBilingualPool{{
        {"高瀬 蓮",
         "「硬くなったちんぽをそのまま濡れたまんこに押し込むぞ」",
         "\"I'm pushing my hard cock straight into your wet pussy.\""},
        {"",
         "カーテンの隙間から街灯の淡い光が静かなベッドの端を照らしていた。",
         "Faint streetlight filtered through the curtains across the edge of the quiet bed."},
        {"蒼井 遥",
         "「さっきフェラしたばかりなのに、まだペニスがこんなに熱いなんて……」",
         "\"Even right after that blowjob, your penis and dick still feel burning hot...\""},
        {"",
         "掛け時計の秒針が規則正しい音を刻み、シーツの皺が深く寄っていく。",
         "The second hand of the wall clock ticked steadily while the bedsheets creased."},
        {"蒼井 遥",
         "「クリトリスもヴァギナの奥も擦れて、もうすぐ絶頂しそうなの……」",
         "\"Rubbing my clit and deep inside my vagina like this is pushing me right to orgasm...\""},
        {"高瀬 蓮",
         "「このまま奥に中出しするから、最後の一滴まで全部受け止めてくれ」",
         "\"I'm going to cum deep inside your pussy, so take every last drop.\""},
    }};
    std::vector<DialogueLine> h_blunt_bilingual;
    h_blunt_bilingual.reserve(kBluntHSceneLines);
    for (std::size_t i = 0; i < kBluntHSceneLines; ++i) {
        h_blunt_bilingual.push_back(
            kBluntBilingualPool[i % kBluntBilingualPool.size()]);
    }
    ASSERT_EQ(h_blunt_bilingual.size(), kBluntHSceneLines);
    h_blunt_bilingual[kBluntDefectIdxA] = {
        "高瀬 蓮",
        "「硬くなったちんぽをそのまま濡れたまんこに押し込むぞ」",
        "\"I'm pushing my hard cock straight into your 濡れたまんこ (TL: blunt).\""};
    h_blunt_bilingual[kBluntDefectIdxB] = {
        "蒼井 遥",
        "「さっきフェラしたばかりなのに、まだペニスがこんなに熱いなんて……」",
        "\"Even after that blowjob, your penis is こんなに熱い (senpai)...\""};
    h_blunt_bilingual[kBluntDefectIdxC] = {
        "高瀬 蓮",
        "「このまま奥に中出しするから、最後の一滴まで全部受け止めてくれ」",
        "\"I'm going to cum inside your pussy 最後の一滴まで (Note: creampie).\""};

    // 2. `scene_03_h_blunt_en_only` (30 lines): Japanese text uses oblique pronouns
    //    ("そこ", "あれ", "中") with ZERO JP explicit words, ZERO `♡`, and ZERO moan SFX,
    //    while English uses blunt explicit words (`cock`, `vagina`, `pussy`, `cum`,
    //    `penis`, `clit`, `dick`, `blowjob`, `orgasm`) — proving English-side blunt
    //    explicit lexicon alone anchors and skips sustained H-scenes!
    constexpr std::array<DialogueLine, 6> kBluntEnglishOnlyPool{{
        {"高瀬 蓮",
         "「もう我慢できないから、そのまま奥まで入れるぞ」",
         "\"I can't hold back anymore—I'm sliding my cock deep into your vagina.\""},
        {"",
         "窓ガラスが夜風にわずかに揺れ、部屋の影がゆっくりと動いた。",
         "The windowpane trembled slightly in the night breeze as shadows shifted."},
        {"蒼井 遥",
         "「さっき口でしたときより、ずっと大きくなってるね」",
         "\"Your dick and penis feel way bigger than during the blowjob earlier.\""},
        {"",
         "枕元のランプが柔らかな輪郭を描き、静かな夜が更けていく。",
         "The bedside lamp cast soft outlines while the quiet night deepened."},
        {"蒼井 遥",
         "「敏感なところばかり触られたら、頭が真っ白になってしまうよ」",
         "\"If you keep rubbing my clit and wet pussy, you'll make me orgasm.\""},
        {"高瀬 蓮",
         "「離さないでくれ、このまま一番奥へ全部出すからな」",
         "\"Don't let go—I'm going to cum straight inside your pussy.\""},
    }};
    std::vector<DialogueLine> h_blunt_en_only;
    h_blunt_en_only.reserve(kBluntHSceneLines);
    for (std::size_t i = 0; i < kBluntHSceneLines; ++i) {
        h_blunt_en_only.push_back(
            kBluntEnglishOnlyPool[i % kBluntEnglishOnlyPool.size()]);
    }
    ASSERT_EQ(h_blunt_en_only.size(), kBluntHSceneLines);
    h_blunt_en_only[kBluntDefectIdxA] = {
        "高瀬 蓮",
        "「もう我慢できないから、そのまま奥まで入れるぞ」",
        "\"I can't hold back—I'm sliding my cock into your vagina 奥まで入れるぞ.\""};
    h_blunt_en_only[kBluntDefectIdxB] = {
        "蒼井 遥",
        "「敏感なところばかり触られたら、頭が真っ白になってしまうよ」",
        "\"Rubbing my clit and wet pussy makes my 頭が真っ白 (TL: orgasm)!\""};

    auto classroom_scene = make_clean_block(kNormalLibraryLines);
    inject_high_energy_defect_cluster(
        classroom_scene, kLibraryDefectStart, kLibraryDefectEnd);

    const std::string initial_tt = serialize_scripts_json({
        {"scene_01_baseline", baseline},
        {"scene_02_h_blunt_bilingual", h_blunt_bilingual},
        {"scene_03_h_blunt_en_only", h_blunt_en_only},
        {"scene_04_classroom", classroom_scene},
    });
    const auto tt_path = sandbox.write_bytes(
        "BluntExplicitNovel/script_output/translated_text.json", initial_tt);

    constexpr std::size_t kBluntBilingualBegin = kBaselineScriptLines;
    constexpr std::size_t kBluntBilingualEnd =
        kBluntBilingualBegin + kBluntHSceneLines;
    constexpr std::size_t kBluntEnOnlyBegin = kBluntBilingualEnd;
    constexpr std::size_t kBluntEnOnlyEnd =
        kBluntEnOnlyBegin + kBluntHSceneLines;
    constexpr std::size_t kClassroomDefectBegin =
        kBluntEnOnlyEnd + kLibraryDefectStart;
    constexpr std::size_t kClassroomDefectEnd =
        kBluntEnOnlyEnd + kLibraryDefectEnd;

    // 1. Default (`skip_h_scenes == true`): BOTH blunt-word sustained H-scenes
    //    (`scene_02_h_blunt_bilingual` and `scene_03_h_blunt_en_only`) are skipped!
    const auto default_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "blunt_default_scan.json",
        kStandardScanWindow,
        false);
    const auto& default_hotspots = default_obj.at("ranked_hotspots").as_array();
    ASSERT_GE(default_hotspots.size(), 1U);
    EXPECT_FALSE(any_hotspot_overlaps_range(
        default_hotspots, kBluntBilingualBegin, kBluntBilingualEnd));
    EXPECT_FALSE(any_hotspot_overlaps_range(
        default_hotspots, kBluntEnOnlyBegin, kBluntEnOnlyEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        default_hotspots, kClassroomDefectBegin, kClassroomDefectEnd));

    // 2. Opt-in (`--include-h-scenes`): unlocks BOTH blunt-word H-scenes!
    const auto optin_obj = run_offline_scan_and_parse(
        exe,
        sandbox.path(),
        game_dir,
        sandbox.path() / "blunt_optin_scan.json",
        kStandardScanWindow,
        true);
    const auto& optin_hotspots = optin_obj.at("ranked_hotspots").as_array();
    ASSERT_GT(optin_hotspots.size(), default_hotspots.size());
    EXPECT_TRUE(any_hotspot_overlaps_range(
        optin_hotspots, kBluntBilingualBegin, kBluntBilingualEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        optin_hotspots, kBluntEnOnlyBegin, kBluntEnOnlyEnd));
    EXPECT_TRUE(any_hotspot_overlaps_range(
        optin_hotspots, kClassroomDefectBegin, kClassroomDefectEnd));

    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

}  // namespace
}  // namespace spot_checker
