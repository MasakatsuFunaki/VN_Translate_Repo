// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Game-specific Tier-2 live-API SYSTEM gate for `Adieu` (*Sayonara o Oshiete*,
// VisualArt's SiglusEngine) backed by shared `vn::llm_client`:
// - Translates authentic `Adieu` script lines, cast speaker plates (`人見`,
//   `こより`, `睦　月`, `引　用`, `となえ`, `瀬美奈`), and Siglus `#NAMAE`
//   protagonist macro tokens (`＊Ａ` / `＊Ｂ`) in an isolated `ScopedTempDir`.
// - Verifies `thought_tokens > 0`, `<context_after>` lookahead wiring, 100%
//   target-charset (UTF-8 / UTF-16LE) cleanliness, `#NAMAE` macro preservation,
//   runtime `translation_table.tsv` invariants, and 0-API-call 2nd-pass resume.
#include "translate/gemini_client.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS)
#error "Windows header leaked through public translate headers"
#endif

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace adi::translate {
namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr int kPass1BatchSize = 6;
constexpr int kSingleApiBatchCap = 1;
constexpr int kUnlimitedApiBatches = 0;
constexpr int kTranslateExitOk = 0;
constexpr std::size_t kExpectedPass1ApiCalls = 1u;
constexpr std::size_t kFirstIndex = 0u;
constexpr std::size_t kStartMacroBIndex = 2u;
constexpr std::size_t kMinStartSceneStrings = 4u;
constexpr std::int64_t kZeroTokens = 0;
constexpr std::size_t kUtf8BomLen = 3u;
constexpr char32_t kCjkSymbolsFirst = 0x3000u;
constexpr char32_t kKatakanaBlockLast = 0x30FFu;
constexpr char32_t kFullwidthFormsFirst = 0xFF01u;
constexpr char32_t kHalfwidthFormsLast = 0xFF9Fu;
constexpr char32_t kReplacementChar = 0xFFFDu;
constexpr char32_t kBmpMaxCodepoint = 0xFFFFu;
constexpr char32_t kSupplementaryPlaneBase = 0x10000u;
constexpr char32_t kSurrogateMin = 0xD800u;
constexpr char32_t kLowSurrogateBase = 0xDC00u;
constexpr char32_t kSurrogateMax = 0xDFFFu;
constexpr char32_t kUnicodeMax = 0x10FFFFu;
constexpr unsigned int kSurrogateHighShift = 10u;
constexpr char32_t kSurrogateLowMask = 0x3FFu;
constexpr unsigned int kByteShift = 8u;
constexpr std::uint32_t kLowByteMask = 0xFFu;

constexpr const char* kStartScene = "_start";
constexpr const char* kPrologueScene = "seen020";

// SiglusEngine `#NAMAE` protagonist macro keys (must NEVER be translated or
// emitted into `translation_table.tsv`) alongside default cast nameplates.
constexpr const char* kNamaeMacroA = "＊Ａ";
constexpr const char* kNamaeMacroB = "＊Ｂ";
constexpr const char* kHitomiSurname = "人見";
constexpr const char* kKousukeGiven = "広介";
constexpr const char* kQuotePlate = "引　用";
constexpr const char* kKoyoriPlate = "こより";
constexpr const char* kMutsukiPlate = "睦　月";

// 6 uncached authentic `Adieu` (*Sayonara o Oshiete*) dialogue & NVL lines.
constexpr const char* kAdieuQuoteFear =
    "『恐いんです、人が。特に‥‥男の人が‥‥』";
constexpr const char* kAdieuMonsterMonologue =
    "　僕が‥‥恐い‥‥？　怪物？　怪物だから？　怪物だからか？";
constexpr const char* kAdieuKoyoriTease =
    "『うーん‥‥先生って、見栄っ張りなんですねぇ』";
constexpr const char* kAdieuHitomiGasp = "「うっ！？」";
constexpr const char* kAdieuAwakening =
    "　知覚が乱れ、意識の焦点がふらふらと揺らぎ、そして‥‥グラデーションのようにじんわりと、現実感が戻ってくる。";
constexpr const char* kAdieuSchoolCast =
    "　課題が待っている。教え子たちが待っている。となえが、瀬美奈が待っている。";

// Pre-seeded trailing lines in `seen020` sitting immediately after the 6-line
// uncached batch so Pass 1 exercises `<context_after>` lookahead while leaving
// 0 uncached lines for the immediate 2nd-pass resume check.
constexpr const char* kLookaheadJp1 = "　未だはっきりと残る、生々しい感触。";
constexpr const char* kLookaheadEn1 = "A vivid sensation that still lingers.";
constexpr const char* kLookaheadJp2 = "　天使の体躯、天使の感触、天使の味‥‥。";
constexpr const char* kLookaheadEn2 =
    "The Angel's frame, the Angel's touch, the Angel's taste...";
constexpr const char* kLookaheadJp3 = "　睦月の身体、睦月の柔肌、睦月の叫び‥‥。";
constexpr const char* kLookaheadEn3 =
    "Mutsuki's body, Mutsuki's soft skin, Mutsuki's cry...";

struct SceneLineSpec {
    const char* text;
    const char* type;
    const char* speaker;
};

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        dir_ = fs::temp_directory_path() /
               fs::u8path("adi_st_gemini_translate_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

class LiveClientTelemetryObserver final : public gemini::IMessageClient {
public:
    explicit LiveClientTelemetryObserver(std::string api_key)
        : client_(std::move(api_key)) {}

    bj::value messages(const bj::value& body, int timeout_sec) override {
        requests.push_back(body);
        try {
            bj::value resp = client_.messages(body, timeout_sec);
            last_error.clear();
            responses.push_back(resp);
            return resp;
        } catch (const std::exception& e) {
            last_error = e.what();
            throw;
        }
    }

    std::vector<bj::value> requests;
    std::vector<bj::value> responses;
    std::string last_error;

private:
    gemini::Client client_;
};

bool is_github_actions() noexcept {
    const wchar_t* val = _wgetenv(L"GITHUB_ACTIONS");
    return val != nullptr && std::wcscmp(val, L"true") == 0;
}

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool contains_any(std::string_view haystack,
                  std::initializer_list<std::string_view> needles) {
    for (std::string_view needle : needles) {
        if (haystack.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// Verifies a translated string is 100% valid Unicode scalar UTF-8 (losslessly
// representable in SiglusEngine's UTF-16LE runtime) with zero replacement
// characters (`U+FFFD`) and zero untranslated Japanese kana/kanji/fullwidth forms.
bool is_clean_english_for_siglus_utf16(const std::string& utf8) {
    if (utf8.empty() || has_real_japanese(utf8)) {
        return false;
    }
    std::size_t i = 0u;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if (cp == kReplacementChar || cp > kUnicodeMax ||
            (cp >= kSurrogateMin && cp <= kSurrogateMax)) {
            return false;
        }
        if ((cp >= kCjkSymbolsFirst && cp <= kKatakanaBlockLast) ||
            (cp >= kFullwidthFormsFirst && cp <= kHalfwidthFormsLast)) {
            return false;
        }
    }
    // Verify lossless UTF-16LE encode -> decode round-trip.
    std::vector<std::uint8_t> utf16le;
    std::size_t j = kFirstIndex;
    while (j < utf8.size()) {
        const char32_t cp = utf8_next(utf8, j);
        if (cp <= kBmpMaxCodepoint) {
            utf16le.push_back(static_cast<std::uint8_t>(cp & kLowByteMask));
            utf16le.push_back(
                static_cast<std::uint8_t>((cp >> kByteShift) & kLowByteMask));
            continue;
        }
        const char32_t adj = cp - kSupplementaryPlaneBase;
        const std::uint16_t hi =
            static_cast<std::uint16_t>(kSurrogateMin + (adj >> kSurrogateHighShift));
        const std::uint16_t lo =
            static_cast<std::uint16_t>(kLowSurrogateBase + (adj & kSurrogateLowMask));
        utf16le.push_back(static_cast<std::uint8_t>(hi & kLowByteMask));
        utf16le.push_back(static_cast<std::uint8_t>((hi >> kByteShift) & kLowByteMask));
        utf16le.push_back(static_cast<std::uint8_t>(lo & kLowByteMask));
        utf16le.push_back(static_cast<std::uint8_t>((lo >> kByteShift) & kLowByteMask));
    }
    return utf16le_decode_replace(utf16le.data(), utf16le.size()) == utf8;
}

bj::object make_entry(const SceneLineSpec& spec) {
    bj::object o;
    o["text"] = spec.text;
    o["type"] = spec.type;
    o["speaker"] = spec.speaker;
    return o;
}

bj::object make_scene(std::initializer_list<SceneLineSpec> specs) {
    bj::array arr;
    for (const SceneLineSpec& spec : specs) {
        arr.push_back(make_entry(spec));
    }
    bj::object scene;
    scene["strings"] = std::move(arr);
    return scene;
}

}  // namespace

TEST(GeminiTranslateSt,
     LiveAdieuBatchTranslatesCleanlyPreservesNamaeMacrosAndResumesWithZeroApiCalls) {
    const std::string api_key = gemini::load_api_key();
    if (api_key.empty()) {
        if (is_github_actions()) {
            GTEST_SKIP() << "GEMINI_API_KEY is absent on GITHUB_ACTIONS=true; "
                            "skipping Adieu live Gemini SYSTEM test";
        }
        FAIL() << "GEMINI_API_KEY is missing locally (configure GEMINI_API_KEY "
                  "in the environment or ~/.gemini/.env)";
    }

    ScopedTempDir sandbox;
    const fs::path out_dir = sandbox.path() / "script_output";
    fs::create_directories(out_dir);

    TranslateOptions opt;
    opt.batch_size = kPass1BatchSize;
    opt.max_batches = kSingleApiBatchCap;
    opt.input_file = (out_dir / "extracted_text.json").u8string();
    opt.cache_file = (out_dir / "translation_cache_gemini.json").u8string();
    opt.output_file = (out_dir / "translated_text.json").u8string();
    opt.tsv_file = (sandbox.path() / "translation_table.tsv").u8string();
    opt.test_dir = (sandbox.path() / "test").u8string();

    // Seed the 3 trailing lookahead lines so Pass 1 exercises `<context_after>`
    // right at the 6-line batch boundary and completes all remaining uncached
    // work in 1 live API call, enabling Pass 2 to verify 0-API-call resume.
    Cache seed_cache;
    seed_cache.set(kLookaheadJp1, kLookaheadEn1);
    seed_cache.set(kLookaheadJp2, kLookaheadEn2);
    seed_cache.set(kLookaheadJp3, kLookaheadEn3);
    save_cache(seed_cache, opt.cache_file);

    const char* const kUncachedAdieuLines[] = {
        kAdieuQuoteFear,
        kAdieuMonsterMonologue,
        kAdieuKoyoriTease,
        kAdieuHitomiGasp,
        kAdieuAwakening,
        kAdieuSchoolCast,
    };

    bj::object extracted;
    extracted[kStartScene] = make_scene({
        {kNamaeMacroA, "other", ""},
        {kHitomiSurname, "name", ""},
        {kNamaeMacroB, "other", ""},
        {kKousukeGiven, "name", ""},
    });
    extracted[kPrologueScene] = make_scene({
        {kQuotePlate, "name", ""},
        {kAdieuQuoteFear, "dialogue", kQuotePlate},
        {kAdieuMonsterMonologue, "narrative", ""},
        {kKoyoriPlate, "name", ""},
        {kAdieuKoyoriTease, "dialogue", kKoyoriPlate},
        {kNamaeMacroA, "other", ""},
        {kAdieuHitomiGasp, "dialogue", kHitomiSurname},
        {kAdieuAwakening, "narrative", ""},
        {kAdieuSchoolCast, "narrative", ""},
        {kMutsukiPlate, "name", ""},
        {kLookaheadJp1, "narrative", ""},
        {kLookaheadJp2, "narrative", ""},
        {kLookaheadJp3, "narrative", ""},
    });
    write_file_text(opt.input_file, json_pretty(bj::value(std::move(extracted))));

    LiveClientTelemetryObserver observer(api_key);
    ASSERT_EQ(translate_all(opt, observer), kTranslateExitOk);
    ASSERT_TRUE(observer.last_error.empty()) << observer.last_error;
    ASSERT_EQ(observer.requests.size(), kExpectedPass1ApiCalls);
    ASSERT_EQ(observer.responses.size(), kExpectedPass1ApiCalls);

    const bj::object& req = observer.requests[kFirstIndex].as_object();
    EXPECT_EQ(std::string(req.at("model").as_string()), MODEL);
    const std::string sent_prompt(req.at("messages")
                                      .as_array()
                                      .at(kFirstIndex)
                                      .as_object()
                                      .at("content")
                                      .as_string());

    // Verify Adieu speaker glossary resolution, `#NAMAE` exclusion, and `<context_after>`.
    EXPECT_NE(sent_prompt.find("1. [Quote] "), std::string::npos);
    EXPECT_NE(sent_prompt.find("3. [Koyori] "), std::string::npos);
    EXPECT_NE(sent_prompt.find("4. [Hitomi] "), std::string::npos);
    EXPECT_EQ(sent_prompt.find(kNamaeMacroA), std::string::npos);
    EXPECT_EQ(sent_prompt.find(kNamaeMacroB), std::string::npos);
    ASSERT_NE(sent_prompt.find("<context_after>\n"), std::string::npos);
    EXPECT_NE(sent_prompt.find(kLookaheadEn3), std::string::npos);

    const bj::object& resp = observer.responses[kFirstIndex].as_object();
    EXPECT_EQ(std::string(resp.at("stop_reason").as_string()), "end_turn");

    const bj::object& usage = resp.at("usage").as_object();
    const std::int64_t input_tokens = usage.at("input_tokens").as_int64();
    const std::int64_t output_tokens = usage.at("output_tokens").as_int64();
    const std::int64_t thought_tokens = usage.at("thought_tokens").as_int64();

    log_info("[SYSTEM] Adieu live gemini-4-carbon telemetry: input_tokens=" +
             std::to_string(input_tokens) +
             " output_tokens=" + std::to_string(output_tokens) +
             " thought_tokens=" + std::to_string(thought_tokens));

    ASSERT_GT(thought_tokens, kZeroTokens)
        << "gemini-4-carbon returned thought_tokens == 0; MEDIUM thinkingLevel was not active";
    EXPECT_GT(input_tokens, kZeroTokens);
    EXPECT_GT(output_tokens, thought_tokens);

    const Cache persisted = load_cache(opt.cache_file);
    for (const char* jp_line : kUncachedAdieuLines) {
        SCOPED_TRACE(jp_line);
        const std::string* en = persisted.get(jp_line);
        ASSERT_NE(en, nullptr) << "Missing translated cache entry";
        EXPECT_FALSE(is_failed_entry(jp_line, *en));
        EXPECT_TRUE(is_clean_english_for_siglus_utf16(*en))
            << "Target-charset / UTF-16LE cleanliness failed for: " << *en;
    }

    // Verify Adieu character glossary fidelity (`となえ` -> Tonae, `瀬美奈` -> Semina,
    // `先生` -> Sensei / teacher).
    const std::string low_cast = to_lower_ascii(*persisted.get(kAdieuSchoolCast));
    EXPECT_NE(low_cast.find("tonae"), std::string::npos) << low_cast;
    EXPECT_NE(low_cast.find("semina"), std::string::npos) << low_cast;

    const std::string low_koyori = to_lower_ascii(*persisted.get(kAdieuKoyoriTease));
    EXPECT_TRUE(contains_any(low_koyori, {"sensei", "teacher"})) << low_koyori;

    // `#NAMAE` macro safety: `＊Ａ` / `＊Ｂ` must stay untouched in `translated_text.json`
    // and never enter `translation_cache_gemini.json` or `translation_table.tsv`.
    EXPECT_EQ(persisted.get(kNamaeMacroA), nullptr);
    EXPECT_EQ(persisted.get(kNamaeMacroB), nullptr);
    ASSERT_NE(persisted.get(kHitomiSurname), nullptr);
    EXPECT_EQ(*persisted.get(kHitomiSurname), "Hitomi");
    ASSERT_NE(persisted.get(kKousukeGiven), nullptr);
    EXPECT_EQ(*persisted.get(kKousukeGiven), "Kousuke");

    const bj::object out_doc = json_parse_file(opt.output_file).get_object();
    const bj::array start_strings =
        out_doc.at(kStartScene).as_object().at("strings").as_array();
    ASSERT_GE(start_strings.size(), kMinStartSceneStrings);
    EXPECT_EQ(std::string(
                  start_strings[kFirstIndex].as_object().at("translated").as_string()),
              kNamaeMacroA);
    EXPECT_EQ(std::string(start_strings[kStartMacroBIndex]
                              .as_object()
                              .at("translated")
                              .as_string()),
              kNamaeMacroB);

    ASSERT_TRUE(fs::is_regular_file(fs::u8path(opt.tsv_file)));
    const Bytes pass1_tsv_bytes = read_file(opt.tsv_file);
    const std::string pass1_tsv(pass1_tsv_bytes.begin(), pass1_tsv_bytes.end());
    ASSERT_FALSE(pass1_tsv.empty());
    EXPECT_NE(pass1_tsv.compare(0, kUtf8BomLen, "\xEF\xBB\xBF"), 0);
    EXPECT_EQ(pass1_tsv.find('\r'), std::string::npos);
    EXPECT_EQ(pass1_tsv.find(kNamaeMacroA), std::string::npos);
    EXPECT_EQ(pass1_tsv.find(kNamaeMacroB), std::string::npos);
    EXPECT_NE(pass1_tsv.find("人見\tHitomi\n"), std::string::npos);
    EXPECT_NE(pass1_tsv.find("広介\tKousuke\n"), std::string::npos);
    EXPECT_NE(pass1_tsv.find(std::string(kAdieuSchoolCast) + "\t" +
                             *persisted.get(kAdieuSchoolCast) + "\n"),
              std::string::npos);

    // Pass 2: Immediate resume with `max_batches = 0` (unlimited) must make
    // strictly 0 additional API calls and produce an identical runtime TSV.
    TranslateOptions resume_opt = opt;
    resume_opt.max_batches = kUnlimitedApiBatches;
    ASSERT_EQ(translate_all(resume_opt, observer), kTranslateExitOk);
    EXPECT_EQ(observer.requests.size(), kExpectedPass1ApiCalls)
        << "Expected 0 API calls on immediate 2nd-pass resume";
    EXPECT_EQ(read_file(resume_opt.tsv_file), pass1_tsv_bytes);
}

}  // namespace adi::translate
