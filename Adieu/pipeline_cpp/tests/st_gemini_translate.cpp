// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Game-specific Tier-2 live-API SYSTEM gate for `Adieu` (*Sayonara o Oshiete*,
// VisualArt's SiglusEngine) executing the compiled/staged `02_translate.exe`
// binary out-of-process inside an isolated `ScopedTempDir`:
// - Translates a realistic production-scale 40-line `Adieu` batch (`--batch 40
//   --max-batches 1`, `vn::llm::kDefaultBatchSize`) covering cast speaker plates
//   (`人見`, `こより`, `睦　月`, `まひる`, `望　美`, `御　幸`, `となえ`, `瀬美奈`,
//   `天　使`, `引　用`, `少女達`) and Siglus `#NAMAE` protagonist macros (`＊Ａ` / `＊Ｂ`).
// - Verifies Pass 1 latency `< 600s` (with `660s` process timeout), active
//   `gemini-4-carbon` `MEDIUM` reasoning (`thought_tokens >= 64`), `<context_after>`
//   lookahead disambiguation, 100% Siglus UTF-16LE cleanliness, `#NAMAE` macro
//   preservation, runtime `translation_table.tsv` invariants, and 0-API-call
//   2nd-pass resume idempotency.
#include "translate/gemini_client.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS)
#error "Windows header leaked through public translate headers"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace adi::translate {
namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr int kPass1BatchSize = vn::llm::kDefaultBatchSize;
constexpr int kSingleApiBatchCap = 1;
constexpr DWORD kSubprocessTimeoutMs = 660000u;
constexpr double kMaxPass1LatencySeconds = 600.0;
constexpr DWORD kExitSuccess = 0u;
constexpr int kMaxRepoWalkLevels = 6;
constexpr std::size_t kBufferGrowthFactor = 2u;
constexpr std::size_t kBackslashDoublingFactor = 2u;

constexpr std::size_t kExpectedBatchLineCount = 40u;
constexpr std::size_t kFirstIndex = 0u;
constexpr std::size_t kStartMacroBIndex = 2u;
constexpr std::size_t kMinStartSceneStrings = 4u;
constexpr std::int64_t kZeroTokens = 0;
constexpr std::int64_t kMinCalibratedMediumThoughtTokens = 64;
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
constexpr const char* kMahiruPlate = "まひる";
constexpr const char* kNozomiPlate = "望　美";
constexpr const char* kMiyukiPlate = "御　幸";
constexpr const char* kTonaePlate = "となえ";
constexpr const char* kSeminaPlate = "瀬美奈";
constexpr const char* kAngelPlate = "天　使";
constexpr const char* kGirlsPlate = "少女達";

struct SceneLineSpec {
    const char* text;
    const char* type;
    const char* speaker;
};

// 40 distinct authentic `Adieu` (*Sayonara o Oshiete*) NVL narrative & dialogue
// lines spanning the cast, register shifts, and a final `<context_after>`
// lookahead disambiguation canary (line 40: `白衣の彼女` resolved by trailing
// lookahead as school nurse Oomori Tonae stubbing out her cigarette in the infirmary).
constexpr std::array<SceneLineSpec, kExpectedBatchLineCount> kUncachedAdieuBatch40 = {{
    {"『恐いんです、人が。特に‥‥男の人が‥‥』", "dialogue", kQuotePlate},
    {"　僕が‥‥恐い‥‥？　怪物？　怪物だから？　怪物だからか？", "narrative", ""},
    {"『うーん‥‥先生って、見栄っ張りなんですねぇ』", "dialogue", kKoyoriPlate},
    {"「うっ！？」", "dialogue", kHitomiSurname},
    {"　知覚が乱れ、意識の焦点がふらふらと揺らぎ、そして‥‥グラデーションのようにじんわりと、現実感が戻ってくる。", "narrative", ""},
    {"　課題が待っている。教え子たちが待っている。となえが、瀬美奈が待っている。", "narrative", ""},
    {"　薄暗い洗面所で、僕は冷たい水を何度も顔に浴びせた。", "narrative", ""},
    {"　鏡の向こうから、ひどく疲れた目をした教育実習生がこちらを覗き込んでいる。", "narrative", ""},
    {"「おはようございます、人見先生」", "dialogue", kMutsukiPlate},
    {"　包帯の白い輪郭だけが、朝の廊下にぼんやりと浮かび上がっていた。", "narrative", ""},
    {"「あ、ああ‥‥おはよう、巣鴨さん。具合はもういいのかい？」", "dialogue", kHitomiSurname},
    {"「はい。痛くはありませんから」", "dialogue", kMutsukiPlate},
    {"　抑揚のない静かな声が、リノリウムの床に吸い込まれて消える。", "narrative", ""},
    {"「お兄ちゃーん！　やっと見つけたっ！」", "dialogue", kMahiruPlate},
    {"　弓道場のほうから駆け寄ってきたまひるが、遠慮なく僕の袖を引っ張った。", "narrative", ""},
    {"「こら、学校では先生と呼びなさいって言っただろう」", "dialogue", kHitomiSurname},
    {"「えー、だって二人きりの時はお兄ちゃんでいいじゃない」", "dialogue", kMahiruPlate},
    {"「騒々しいわね、朝から。ここは学び舎であって動物園ではないのよ」", "dialogue", kNozomiPlate},
    {"　階段の踊り場から、望美が腕を組んで冷ややかな視線を投げかけてくる。", "narrative", ""},
    {"「人見先生、本日の日誌と出欠簿をお持ちしました」", "dialogue", kMiyukiPlate},
    {"　背筋をぴんと伸ばした委員長の御幸が、寸分の狂いもない礼をした。", "narrative", ""},
    {"「ありがとう、目黒さん。いつも助かるよ」", "dialogue", kHitomiSurname},
    {"「当然の務めです。実習生の先生が頼りない分、私がしっかりしなければ」", "dialogue", kMiyukiPlate},
    {"「にひひっ、先生また怒られてるー！」", "dialogue", kKoyoriPlate},
    {"「『おはようございます、先生！』」", "dialogue", kGirlsPlate},
    {"　教室の扉を開けた途端、少女たちの澄んだ挨拶が一斉に降り注いだ。", "narrative", ""},
    {"　その眩しさに、僕の胸の奥で何かがぎしりと軋む。", "narrative", ""},
    {"「人見くん、次の授業の指導案はもう目を通しておいたわよ」", "dialogue", kSeminaPlate},
    {"「高島先生‥‥すみません、遅くなりました」", "dialogue", kHitomiSurname},
    {"「謝らなくていいから、教壇では堂々としていなさい。生徒はよく見ているわ」", "dialogue", kSeminaPlate},
    {"　瀬美奈のきびきびとした靴音が、職員室の向こうへ遠ざかっていく。", "narrative", ""},
    {"　昼休みのチャイムが鳴る頃には、僕のこめかみは鉛のように重くなっていた。", "narrative", ""},
    {"『汝、自らの影を恐れるなかれ‥‥すべては許されん』", "dialogue", kAngelPlate},
    {"　白昼夢の残響が、耳の奥で羽音のようにざわめいている。", "narrative", ""},
    {"　逃げるように辿り着いた保健室の扉を、僕はそっとノックした。", "narrative", ""},
    {"「あら、いらっしゃい。また悪い夢でも見たって顔ね、広介」", "dialogue", kTonaePlate},
    {"「‥‥大森先生には、何でもお見通しだな」", "dialogue", kHitomiSurname},
    {"「昔からの腐れ縁を舐めないでちょうだい。ほら、そこの丸椅子に座りなさいよ」", "dialogue", kTonaePlate},
    {"　消毒液の匂いに混じって、甘い紫煙がカーテンの隙間から漂ってくる。", "narrative", ""},
    {"　白衣の裾を揺らして、彼女は咥えていた『ピース』の火を灰皿へ押しつけた。", "narrative", ""},
}};

constexpr const char* kCastFidelityLine =
    "　課題が待っている。教え子たちが待っている。となえが、瀬美奈が待っている。";
constexpr const char* kKoyoriTeaseLine =
    "『うーん‥‥先生って、見栄っ張りなんですねぇ』";
constexpr const char* kMahiruGreetingLine =
    "「お兄ちゃーん！　やっと見つけたっ！」";
constexpr const char* kLookaheadCanaryLine =
    "　白衣の裾を揺らして、彼女は咥えていた『ピース』の火を灰皿へ押しつけた。";

// Pre-seeded trailing lines in `seen020` sitting immediately after line 40 so
// Pass 1 exercises `<context_after>` lookahead (disambiguating `『ピース』` as a
// Peace-brand cigarette in the infirmary rather than an abstract "peace") while
// leaving 0 uncached lines for the immediate 2nd-pass resume check.
constexpr std::pair<const char*, const char*> kPreCachedLookahead[] = {
    {"　吸い殻の山になった灰皿の横で、となえは新しい煙草の箱を指で弾いた。",
     "Beside the ashtray piled with cigarette butts, Tonae flicked a fresh pack of Peace cigarettes."},
    {"「保健室で堂々と喫煙するのはどうかと思うけどな」",
     "\"I'm not sure you should be smoking so openly in the infirmary.\""},
    {"「いいじゃない、換気扇は回してるんだし」",
     "\"What's the harm? I've got the exhaust fan running.\""},
    {"　未だはっきりと残る、生々しい感触。",
     "A vivid sensation that still lingers."},
    {"　天使の体躯、天使の感触、天使の味‥‥。",
     "The Angel's frame, the Angel's touch, the Angel's taste..."},
    {"　睦月の身体、睦月の柔肌、睦月の叫び‥‥。",
     "Mutsuki's body, Mutsuki's soft skin, Mutsuki's cry..."},
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

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE h = nullptr) noexcept : handle_(h) {}
    ~ScopedWinHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

bool is_github_actions() noexcept {
    const wchar_t* val = _wgetenv(L"GITHUB_ACTIONS");
    return val != nullptr && std::wcscmp(val, L"true") == 0;
}

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed in st_gemini_translate");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), out.data(), needed);
    if (written != needed) {
        throw std::runtime_error("MultiByteToWideChar length mismatch in st_gemini_translate");
    }
    return out;
}

bool needs_windows_arg_quotes(std::wstring_view arg) noexcept {
    if (arg.empty()) {
        return true;
    }
    for (wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\v' ||
            ch == L'\r' || ch == L'"') {
            return true;
        }
    }
    return false;
}

void append_quoted_windows_arg(std::wstring& out, std::wstring_view arg) {
    if (!needs_windows_arg_quotes(arg)) {
        out.append(arg);
        return;
    }
    out.push_back(L'"');
    std::size_t backslashes = 0u;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * kBackslashDoublingFactor + 1u, L'\\');
            out.push_back(L'"');
            backslashes = 0u;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0u;
        out.push_back(ch);
    }
    out.append(backslashes * kBackslashDoublingFactor, L'\\');
    out.push_back(L'"');
}

fs::path current_test_exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(
            nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0u) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path();
}

fs::path resolve_adieu_project_dir(const fs::path& start_dir) {
    std::error_code ec;
    fs::path cur = start_dir;
    for (int i = 0; i < kMaxRepoWalkLevels && !cur.empty(); ++i) {
        if (fs::is_regular_file(cur / "build.py", ec) &&
            fs::is_directory(cur / "pipeline_cpp", ec)) {
            return cur;
        }
        if (fs::is_regular_file(cur / "Adieu" / "build.py", ec)) {
            return cur / "Adieu";
        }
        if (cur == cur.root_path()) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

// Prefers the staged `build/install/bin/02_translate.exe` deployment layout when
// present, falling back to the freshly compiled `02_translate.exe` binary.
fs::path resolve_translate_exe() {
    const fs::path test_dir = current_test_exe_dir();
    const fs::path proj_dir = resolve_adieu_project_dir(
        test_dir.empty() ? fs::current_path() : test_dir);

    std::vector<fs::path> candidates;
    if (!proj_dir.empty()) {
        candidates.push_back(proj_dir / "build" / "install" / "bin" / "02_translate.exe");
    }
    if (!test_dir.empty()) {
        candidates.push_back(test_dir / "02_translate.exe");
        candidates.push_back(test_dir.parent_path() / "02_translate.exe");
        candidates.push_back(
            test_dir.parent_path().parent_path() / "Release" / "02_translate.exe");
    }
    if (!proj_dir.empty()) {
        candidates.push_back(proj_dir / "build" / "pipeline" / "Release" / "02_translate.exe");
    }

    std::error_code ec;
    for (const fs::path& cand : candidates) {
        if (fs::is_regular_file(cand, ec)) {
            return fs::weakly_canonical(cand, ec);
        }
    }
    return {};
}

struct SubprocessResult {
    DWORD exit_code = static_cast<DWORD>(-1);
    double elapsed_seconds = 0.0;
};

SubprocessResult run_translate_subprocess(const fs::path& exe_path,
                                          const std::vector<std::string>& args,
                                          const fs::path& working_dir) {
    const std::wstring wide_exe = exe_path.wstring();
    std::wstring cmd_line;
    cmd_line.push_back(L'"');
    cmd_line.append(wide_exe);
    cmd_line.push_back(L'"');
    for (const std::string& arg : args) {
        cmd_line.push_back(L' ');
        append_quoted_windows_arg(cmd_line, utf8_to_wide_strict(arg));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring wide_cwd = working_dir.wstring();

    const auto t0 = std::chrono::steady_clock::now();
    const BOOL created = ::CreateProcessW(
        wide_exe.c_str(), cmd_line.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, wide_cwd.c_str(), &si, &pi);
    if (!created) {
        ADD_FAILURE() << "CreateProcessW failed for " << exe_path.u8string()
                      << " (Win32 error " << ::GetLastError() << ")";
        return {};
    }

    const ScopedWinHandle proc_guard(pi.hProcess);
    const ScopedWinHandle thread_guard(pi.hThread);

    const DWORD wait_rc =
        ::WaitForSingleObject(proc_guard.get(), kSubprocessTimeoutMs);
    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed_s =
        std::chrono::duration<double>(t1 - t0).count();

    if (wait_rc != WAIT_OBJECT_0) {
        ::TerminateProcess(proc_guard.get(), 1u);
        ADD_FAILURE() << "02_translate.exe timed out after " << kSubprocessTimeoutMs
                      << " ms (" << elapsed_s << " s)";
        return SubprocessResult{static_cast<DWORD>(-1), elapsed_s};
    }

    DWORD exit_code = static_cast<DWORD>(-1);
    if (!::GetExitCodeProcess(proc_guard.get(), &exit_code)) {
        ADD_FAILURE() << "GetExitCodeProcess failed (Win32 error "
                      << ::GetLastError() << ")";
        return SubprocessResult{static_cast<DWORD>(-1), elapsed_s};
    }
    return SubprocessResult{exit_code, elapsed_s};
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

}  // namespace

TEST(GeminiTranslateSt,
     LiveAdieuCliBatch40TranslatesCleanlyPreservesNamaeMacrosAndResumesWithZeroApiCalls) {
    const std::string api_key = gemini::load_api_key();
    if (api_key.empty()) {
        if (is_github_actions()) {
            GTEST_SKIP() << "GEMINI_API_KEY is absent on GITHUB_ACTIONS=true; "
                            "skipping Adieu live Gemini SYSTEM test";
        }
        FAIL() << "GEMINI_API_KEY is missing locally (configure GEMINI_API_KEY "
                  "in the environment or ~/.gemini/.env)";
    }

    const fs::path translate_exe = resolve_translate_exe();
    ASSERT_FALSE(translate_exe.empty())
        << "Compiled/staged 02_translate.exe not found in build/install/bin or build/pipeline/Release";

    ScopedTempDir sandbox;
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path out_dir = project_dir / "script_output";
    fs::create_directories(out_dir);

    const fs::path input_path = out_dir / "extracted_text.json";
    const fs::path cache_path = out_dir / "translation_cache_gemini.json";
    const fs::path output_path = out_dir / "translated_text.json";
    const fs::path snapshot_path =
        out_dir / "last_gemini_translate" / "last_translation.json";
    const fs::path tsv_path = project_dir / "translation_table.tsv";

    // Seed the 6 trailing lookahead lines so Pass 1 exercises `<context_after>`
    // right after the 40-line batch boundary and completes all remaining uncached
    // work in 1 live API call, enabling Pass 2 to verify 0-API-call CLI resume.
    Cache seed_cache;
    for (const auto& [jp, en] : kPreCachedLookahead) {
        seed_cache.set(jp, en);
    }
    save_cache(seed_cache, cache_path.u8string());

    bj::array start_strings;
    start_strings.push_back(make_entry({kNamaeMacroA, "other", ""}));
    start_strings.push_back(make_entry({kHitomiSurname, "name", ""}));
    start_strings.push_back(make_entry({kNamaeMacroB, "other", ""}));
    start_strings.push_back(make_entry({kKousukeGiven, "name", ""}));

    bj::array prologue_strings;
    for (const char* plate : {kQuotePlate, kKoyoriPlate, kMutsukiPlate, kMahiruPlate,
                              kNozomiPlate, kMiyukiPlate, kGirlsPlate, kSeminaPlate,
                              kAngelPlate, kTonaePlate}) {
        prologue_strings.push_back(make_entry({plate, "name", ""}));
    }
    prologue_strings.push_back(make_entry({kNamaeMacroA, "other", ""}));
    for (const SceneLineSpec& spec : kUncachedAdieuBatch40) {
        prologue_strings.push_back(make_entry(spec));
    }
    prologue_strings.push_back(make_entry({kNamaeMacroB, "other", ""}));
    for (const auto& [jp, en] : kPreCachedLookahead) {
        (void)en;
        prologue_strings.push_back(make_entry({jp, "narrative", ""}));
    }

    bj::object extracted;
    extracted[kStartScene] = bj::object{{"strings", std::move(start_strings)}};
    extracted[kPrologueScene] = bj::object{{"strings", std::move(prologue_strings)}};
    write_file_text(input_path.u8string(), json_pretty(bj::value(std::move(extracted))));

    // Pass 1: Out-of-process `02_translate.exe --batch 40 --max-batches 1`.
    const SubprocessResult pass1 = run_translate_subprocess(
        translate_exe,
        {
            "--dir", project_dir.u8string(),
            "--batch", std::to_string(kPass1BatchSize),
            "--max-batches", std::to_string(kSingleApiBatchCap),
            "--out", tsv_path.u8string(),
        },
        sandbox.path());
    ASSERT_EQ(pass1.exit_code, kExitSuccess);
    EXPECT_LT(pass1.elapsed_seconds, kMaxPass1LatencySeconds)
        << "Pass 1 40-line live batch exceeded 600s budget";

    ASSERT_TRUE(fs::is_regular_file(snapshot_path))
        << "Missing batch telemetry snapshot: " << snapshot_path.u8string();
    const bj::object telemetry = json_parse_file(snapshot_path.u8string()).as_object();
    EXPECT_EQ(std::string(telemetry.at("model").as_string()), vn::llm::kDefaultModel);
    EXPECT_EQ(std::string(telemetry.at("thinking_level").as_string()),
              vn::llm::kDefaultThinkingLevel);
    EXPECT_EQ(telemetry.at("api_batches_done").as_int64(), kSingleApiBatchCap);
    EXPECT_EQ(telemetry.at("line_count").as_int64(),
              static_cast<std::int64_t>(kExpectedBatchLineCount));
    EXPECT_EQ(std::string(telemetry.at("stop_reason").as_string()), "end_turn");
    ASSERT_EQ(telemetry.at("items").as_array().size(), kExpectedBatchLineCount);

    const std::int64_t input_tokens = telemetry.at("input_tokens").as_int64();
    const std::int64_t output_tokens = telemetry.at("output_tokens").as_int64();
    const std::int64_t thought_tokens = telemetry.at("thought_tokens").as_int64();

    log_info("[SYSTEM] Adieu 02_translate.exe --batch 40 telemetry: input_tokens=" +
             std::to_string(input_tokens) +
             " output_tokens=" + std::to_string(output_tokens) +
             " thought_tokens=" + std::to_string(thought_tokens) +
             " elapsed_s=" + std::to_string(pass1.elapsed_seconds));

    ASSERT_GT(thought_tokens, kZeroTokens)
        << "gemini-4-carbon returned thought_tokens == 0; MEDIUM thinkingLevel was not active";
    EXPECT_GE(thought_tokens, kMinCalibratedMediumThoughtTokens)
        << "40-line MEDIUM batch returned suspiciously throttled thought_tokens="
        << thought_tokens;
    EXPECT_GT(input_tokens, kZeroTokens);
    EXPECT_GT(output_tokens, thought_tokens);

    const Cache persisted = load_cache(cache_path.u8string());
    static_assert(kUncachedAdieuBatch40.size() == kExpectedBatchLineCount);
    for (const SceneLineSpec& spec : kUncachedAdieuBatch40) {
        SCOPED_TRACE(spec.text);
        const std::string* en = persisted.get(spec.text);
        ASSERT_NE(en, nullptr) << "Missing translated cache entry";
        EXPECT_FALSE(is_failed_entry(spec.text, *en));
        EXPECT_TRUE(is_clean_english_for_siglus_utf16(*en))
            << "Target-charset / UTF-16LE cleanliness failed for: " << *en;
    }

    // Verify Adieu character glossary fidelity & `<context_after>` lookahead canary.
    const std::string low_cast = to_lower_ascii(*persisted.get(kCastFidelityLine));
    EXPECT_NE(low_cast.find("tonae"), std::string::npos) << low_cast;
    EXPECT_NE(low_cast.find("semina"), std::string::npos) << low_cast;

    const std::string low_koyori = to_lower_ascii(*persisted.get(kKoyoriTeaseLine));
    EXPECT_TRUE(contains_any(low_koyori, {"sensei", "teacher"})) << low_koyori;

    const std::string low_mahiru = to_lower_ascii(*persisted.get(kMahiruGreetingLine));
    EXPECT_TRUE(contains_any(low_mahiru, {"onii-chan", "brother"})) << low_mahiru;

    const std::string low_canary = to_lower_ascii(*persisted.get(kLookaheadCanaryLine));
    EXPECT_TRUE(contains_any(low_canary, {"ashtray", "cigarette", "peace", "smoke", "lit"}))
        << low_canary;

    // `#NAMAE` macro safety: `＊Ａ` / `＊Ｂ` must stay untouched in `translated_text.json`
    // and never enter `translation_cache_gemini.json` or `translation_table.tsv`.
    EXPECT_EQ(persisted.get(kNamaeMacroA), nullptr);
    EXPECT_EQ(persisted.get(kNamaeMacroB), nullptr);
    ASSERT_NE(persisted.get(kHitomiSurname), nullptr);
    EXPECT_EQ(*persisted.get(kHitomiSurname), "Hitomi");
    ASSERT_NE(persisted.get(kKousukeGiven), nullptr);
    EXPECT_EQ(*persisted.get(kKousukeGiven), "Kousuke");

    ASSERT_TRUE(fs::is_regular_file(output_path));
    const bj::object out_doc = json_parse_file(output_path.u8string()).get_object();
    const bj::array out_start_strings =
        out_doc.at(kStartScene).as_object().at("strings").as_array();
    ASSERT_GE(out_start_strings.size(), kMinStartSceneStrings);
    EXPECT_EQ(std::string(
                  out_start_strings[kFirstIndex].as_object().at("translated").as_string()),
              kNamaeMacroA);
    EXPECT_EQ(std::string(out_start_strings[kStartMacroBIndex]
                              .as_object()
                              .at("translated")
                              .as_string()),
              kNamaeMacroB);

    ASSERT_TRUE(fs::is_regular_file(tsv_path));
    const Bytes pass1_tsv_bytes = read_file(tsv_path.u8string());
    const Bytes pass1_cache_bytes = read_file(cache_path.u8string());
    const std::string pass1_tsv(pass1_tsv_bytes.begin(), pass1_tsv_bytes.end());
    ASSERT_FALSE(pass1_tsv.empty());
    EXPECT_NE(pass1_tsv.compare(0, kUtf8BomLen, "\xEF\xBB\xBF"), 0);
    EXPECT_EQ(pass1_tsv.find('\r'), std::string::npos);
    EXPECT_EQ(pass1_tsv.find(kNamaeMacroA), std::string::npos);
    EXPECT_EQ(pass1_tsv.find(kNamaeMacroB), std::string::npos);
    EXPECT_NE(pass1_tsv.find("人見\tHitomi\n"), std::string::npos);
    EXPECT_NE(pass1_tsv.find("広介\tKousuke\n"), std::string::npos);
    EXPECT_NE(pass1_tsv.find(std::string(kCastFidelityLine) + "\t" +
                             *persisted.get(kCastFidelityLine) + "\n"),
              std::string::npos);

    // Pass 2: Delete `last_translation.json` and immediately resume `02_translate.exe`
    // without `--max-batches` cap. Strictly 0 API calls must execute (verified by
    // `last_translation.json` NOT being recreated) and TSV must remain identical.
    std::error_code rm_ec;
    ASSERT_TRUE(fs::remove(snapshot_path, rm_ec)) << rm_ec.message();
    ASSERT_FALSE(fs::exists(snapshot_path));

    const SubprocessResult pass2 = run_translate_subprocess(
        translate_exe,
        {
            "--dir", project_dir.u8string(),
            "--batch", std::to_string(kPass1BatchSize),
            "--out", tsv_path.u8string(),
        },
        sandbox.path());
    ASSERT_EQ(pass2.exit_code, kExitSuccess);
    EXPECT_FALSE(fs::exists(snapshot_path))
        << "Expected 0 API calls on immediate 2nd-pass CLI resume, but last_translation.json was recreated";
    EXPECT_EQ(read_file(cache_path.u8string()), pass1_cache_bytes);
    EXPECT_EQ(read_file(tsv_path.u8string()), pass1_tsv_bytes);
}

}  // namespace adi::translate
