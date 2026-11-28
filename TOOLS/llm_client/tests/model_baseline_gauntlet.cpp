// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "model_baseline_gauntlet.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vn::llm::testing {
namespace {

constexpr int kDecimalBase = 10;
constexpr int kNonDigitSentinel = -1;
constexpr int kFirstExpectedLineNumber = 1;
constexpr std::size_t kZeroCount = 0U;
constexpr std::size_t kStartOffset = 0U;
constexpr std::size_t kSingleStep = 1U;
constexpr std::size_t kOneBasedDisplayOffset = 1U;
constexpr std::size_t kMaxMarkdownStars = 2U;
constexpr std::size_t kFencePairMultiplier = 2U;
constexpr std::size_t kMaxSpeakerTagLength = 32U;

constexpr std::string_view kMarkdownCodeFence = "```";
constexpr std::string_view kEngineLineBreakToken = "%N";

constexpr unsigned char kAsciiUpperExclusive = 0x80U;
constexpr unsigned char kUtf8TwoByteMask = 0xE0U;
constexpr unsigned char kUtf8TwoBytePrefix = 0xC0U;
constexpr unsigned char kUtf8TwoByteLeadPayloadMask = 0x1FU;
constexpr unsigned char kUtf8ThreeByteMask = 0xF0U;
constexpr unsigned char kUtf8ThreeBytePrefix = 0xE0U;
constexpr unsigned char kUtf8ThreeByteLeadPayloadMask = 0x0FU;
constexpr unsigned char kUtf8FourByteMask = 0xF8U;
constexpr unsigned char kUtf8FourBytePrefix = 0xF0U;
constexpr unsigned char kUtf8FourByteLeadPayloadMask = 0x07U;
constexpr unsigned char kUtf8TrailPayloadMask = 0x3FU;
constexpr unsigned int kUtf8ShiftOneTrail = 6U;
constexpr unsigned int kUtf8ShiftTwoTrails = 12U;
constexpr unsigned int kUtf8ShiftThreeTrails = 18U;
constexpr std::size_t kTwoExtraTrailBytes = 1U;
constexpr std::size_t kThreeExtraTrailBytes = 2U;
constexpr char32_t kEndOfInputCodepoint = 0U;
constexpr char32_t kReplacementCodepoint = 0xFFFDU;

constexpr char32_t kHiraganaFirst = 0x3040;
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkExtAFirst = 0x3400;
constexpr char32_t kCjkExtALast = 0x4DBF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;
constexpr char32_t kHalfwidthKatakanaFirst = 0xFF66;
constexpr char32_t kHalfwidthKatakanaLast = 0xFF9F;
constexpr char32_t kFullwidthZero = 0xFF10;
constexpr char32_t kFullwidthNine = 0xFF19;
constexpr char32_t kFullwidthRightParen = 0xFF09;
constexpr char32_t kFullwidthFullStop = 0xFF0E;
constexpr char32_t kFullwidthColon = 0xFF1A;

constexpr std::size_t kLineIdxFurigana = 0U;
constexpr std::size_t kLineIdxPluralSetup = 1U;
constexpr std::size_t kLineIdxPluralRetort = 2U;
constexpr std::size_t kLineIdxDeadpanAside = 3U;
constexpr std::size_t kLineIdxHomophone = 4U;
constexpr std::size_t kLineIdxEngineBreak = 5U;
constexpr std::size_t kLineIdxLookaheadPun = 6U;

constexpr std::int64_t kOffsetCanopyBed = 1894554;
constexpr std::int64_t kOffsetSenpaiGata = 1912000;
constexpr std::int64_t kOffsetGataRetort = 1912064;
constexpr std::int64_t kOffsetMiyukiLie = 1920000;
constexpr std::int64_t kOffsetTaichiConquest = 1930000;
constexpr std::int64_t kOffsetRankingDirective = 1902270;
constexpr std::int64_t kOffsetSamehada = 981231;
constexpr std::int64_t kOffsetSkincare = 981287;

constexpr std::int64_t kOffsetLookaheadScream = 981595;
constexpr std::int64_t kOffsetLookaheadSurface = 981728;
constexpr std::int64_t kOffsetLookaheadShark = 982308;
constexpr std::int64_t kOffsetLookaheadWhyShark = 982376;

struct NumberedLineMatch {
    bool matched = false;
    bool malformed = false;
    int number = 0;
    std::string body;
};

[[nodiscard]] constexpr bool is_ascii_space(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

[[nodiscard]] constexpr char to_lower_ascii_char(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] static std::string_view trim_ascii_view(
    std::string_view s) noexcept {
    const auto first_it = std::ranges::find_if_not(s, is_ascii_space);
    if (first_it == s.end()) {
        return {};
    }
    const auto last_rev_it = std::ranges::find_if_not(
        s | std::views::reverse, is_ascii_space);
    const auto first_offset =
        static_cast<std::size_t>(std::distance(s.begin(), first_it));
    const auto last_offset =
        static_cast<std::size_t>(std::distance(s.begin(), last_rev_it.base()));
    return s.substr(first_offset, last_offset - first_offset);
}

[[nodiscard]] static std::string to_lower_ascii(std::string_view s) {
    std::string out(s.size(), '\0');
    std::ranges::transform(s, out.begin(), to_lower_ascii_char);
    return out;
}

[[nodiscard]] static bool contains_any_substring(
    std::string_view haystack,
    std::initializer_list<std::string_view> needles) noexcept {
    return std::ranges::any_of(needles,
                               [haystack](std::string_view needle) noexcept {
                                   return haystack.find(needle) !=
                                          std::string_view::npos;
                               });
}

[[nodiscard]] static char32_t utf8_next_codepoint(
    std::string_view s,
    std::size_t& idx) noexcept {
    if (idx >= s.size()) {
        return kEndOfInputCodepoint;
    }
    const auto b0 = static_cast<unsigned char>(s[idx++]);
    if (b0 < kAsciiUpperExclusive) {
        return static_cast<char32_t>(b0);
    }
    if ((b0 & kUtf8TwoByteMask) == kUtf8TwoBytePrefix && idx < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[idx++]);
        return (static_cast<char32_t>(b0 & kUtf8TwoByteLeadPayloadMask)
                << kUtf8ShiftOneTrail) |
               static_cast<char32_t>(b1 & kUtf8TrailPayloadMask);
    }
    if ((b0 & kUtf8ThreeByteMask) == kUtf8ThreeBytePrefix &&
        idx + kTwoExtraTrailBytes < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[idx++]);
        const auto b2 = static_cast<unsigned char>(s[idx++]);
        return (static_cast<char32_t>(b0 & kUtf8ThreeByteLeadPayloadMask)
                << kUtf8ShiftTwoTrails) |
               (static_cast<char32_t>(b1 & kUtf8TrailPayloadMask)
                << kUtf8ShiftOneTrail) |
               static_cast<char32_t>(b2 & kUtf8TrailPayloadMask);
    }
    if ((b0 & kUtf8FourByteMask) == kUtf8FourBytePrefix &&
        idx + kThreeExtraTrailBytes < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[idx++]);
        const auto b2 = static_cast<unsigned char>(s[idx++]);
        const auto b3 = static_cast<unsigned char>(s[idx++]);
        return (static_cast<char32_t>(b0 & kUtf8FourByteLeadPayloadMask)
                << kUtf8ShiftThreeTrails) |
               (static_cast<char32_t>(b1 & kUtf8TrailPayloadMask)
                << kUtf8ShiftTwoTrails) |
               (static_cast<char32_t>(b2 & kUtf8TrailPayloadMask)
                << kUtf8ShiftOneTrail) |
               static_cast<char32_t>(b3 & kUtf8TrailPayloadMask);
    }
    return kReplacementCodepoint;
}

[[nodiscard]] static bool contains_japanese_kana_or_kanji(
    std::string_view utf8) noexcept {
    std::size_t idx = kStartOffset;
    while (idx < utf8.size()) {
        const char32_t cp = utf8_next_codepoint(utf8, idx);
        if ((cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
            (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
            (cp >= kHalfwidthKatakanaFirst && cp <= kHalfwidthKatakanaLast) ||
            (cp >= kCjkExtAFirst && cp <= kCjkExtALast) ||
            (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] static std::string resolve_gauntlet_speaker(
    std::string_view jp_speaker) {
    const std::string_view trimmed = trim_ascii_view(jp_speaker);
    if (trimmed.empty()) {
        return "NARRATION";
    }
    if (trimmed == "少女") {
        return "Girl";
    }
    if (trimmed == "友貴") {
        return "Tomoki";
    }
    if (trimmed == "太一") {
        return "Taichi";
    }
    return std::string(trimmed);
}

[[nodiscard]] static bool is_known_gauntlet_speaker_tag(
    std::string_view label) noexcept {
    const std::string lowered = to_lower_ascii(trim_ascii_view(label));
    return lowered == "narration" || lowered == "speaker" ||
           lowered == "girl" || lowered == "tomoki" || lowered == "taichi";
}

[[nodiscard]] static std::string strip_leading_speaker_tags(
    std::string_view raw) {
    std::string_view cur = trim_ascii_view(raw);
    while (!cur.empty() && cur.front() == '[') {
        const std::size_t close = cur.find(']');
        if (close == std::string_view::npos || close > kMaxSpeakerTagLength) {
            break;
        }
        const std::string_view tag_inner =
            cur.substr(kSingleStep, close - kSingleStep);
        if (!is_known_gauntlet_speaker_tag(tag_inner) &&
            tag_inner != "少女" && tag_inner != "友貴" && tag_inner != "太一") {
            break;
        }
        const std::string_view rest =
            trim_ascii_view(cur.substr(close + kSingleStep));
        if (rest.empty()) {
            break;
        }
        cur = rest;
    }
    return std::string(cur);
}

[[nodiscard]] static std::string_view unwrap_outer_markdown_fence(
    std::string_view text) noexcept {
    const std::string_view trimmed = trim_ascii_view(text);
    if (trimmed.size() < kMarkdownCodeFence.size() * kFencePairMultiplier ||
        !trimmed.starts_with(kMarkdownCodeFence) ||
        !trimmed.ends_with(kMarkdownCodeFence)) {
        return trimmed;
    }
    const std::size_t first_nl = trimmed.find('\n');
    if (first_nl == std::string_view::npos ||
        first_nl >= trimmed.size() - kMarkdownCodeFence.size()) {
        return trimmed;
    }
    return trim_ascii_view(trimmed.substr(
        first_nl + kSingleStep,
        trimmed.size() - first_nl - kSingleStep - kMarkdownCodeFence.size()));
}

[[nodiscard]] constexpr int digit_value(char32_t cp) noexcept {
    if (cp >= U'0' && cp <= U'9') {
        return static_cast<int>(cp - U'0');
    }
    if (cp >= kFullwidthZero && cp <= kFullwidthNine) {
        return static_cast<int>(cp - kFullwidthZero);
    }
    return kNonDigitSentinel;
}

[[nodiscard]] constexpr bool is_opener_delimiter(char32_t cp) noexcept {
    return cp == U'.' || cp == U')' || cp == U':' ||
           cp == kFullwidthFullStop || cp == kFullwidthRightParen ||
           cp == kFullwidthColon;
}

[[nodiscard]] static NumberedLineMatch parse_numbered_opener(
    std::string_view line) {
    NumberedLineMatch res;
    std::size_t idx = kStartOffset;
    std::size_t lead_stars = kZeroCount;
    while (idx < line.size() && line[idx] == '*' &&
           lead_stars < kMaxMarkdownStars) {
        ++idx;
        ++lead_stars;
    }

    bool negative = false;
    if (idx < line.size() && line[idx] == '-') {
        negative = true;
        ++idx;
    }

    int num = 0;
    bool has_digit = false;
    bool overflow = false;
    while (idx < line.size()) {
        const std::size_t before = idx;
        const char32_t cp = utf8_next_codepoint(line, idx);
        const int dv = digit_value(cp);
        if (dv < 0) {
            idx = before;
            break;
        }
        has_digit = true;
        if (num > (std::numeric_limits<int>::max() - dv) / kDecimalBase) {
            overflow = true;
            continue;
        }
        num = num * kDecimalBase + dv;
    }

    if (!has_digit) {
        return res;
    }

    std::size_t stars_used = kZeroCount;
    while (lead_stars > kZeroCount && idx < line.size() && line[idx] == '*' &&
           stars_used < lead_stars) {
        ++idx;
        ++stars_used;
    }

    if (idx >= line.size()) {
        res.malformed = true;
        return res;
    }

    const char32_t delim = utf8_next_codepoint(line, idx);
    if (!is_opener_delimiter(delim)) {
        return res;
    }

    if ((delim == U'.' || delim == U':') && idx < line.size() &&
        line[idx] >= '0' && line[idx] <= '9') {
        return res;
    }

    if (negative || overflow) {
        res.malformed = true;
        return res;
    }

    while (stars_used < lead_stars && idx < line.size() && line[idx] == '*') {
        ++idx;
        ++stars_used;
    }

    std::string cleaned_body = strip_leading_speaker_tags(line.substr(idx));
    if (cleaned_body.empty()) {
        res.malformed = true;
        return res;
    }

    res.matched = true;
    res.number = num;
    res.body = std::move(cleaned_body);
    return res;
}

}  // namespace

const std::vector<GauntletLine>& cross_channel_gauntlet_batch() {
    static const std::vector<GauntletLine> kBatch = {
        {"", "そこには天蓋つきの寝台も欧羅巴(ヨーロッパ)製の椅子もあった。",
         kOffsetCanopyBed},
        {"少女", "「あいかわらず先輩がたは……」", kOffsetSenpaiGata},
        {"友貴", "「がたって」", kOffsetGataRetort},
        {"", "みゆきがエクスタシーに達した（嘘）。", kOffsetMiyukiLie},
        {"太一",
         "「理論的には、自分が支払える代価で手に入る駒をもっとも友好に使える局面に冷静にぶつけていけば制覇は成立しうるわけですから」",
         kOffsetTaichiConquest},
        {"", "第一位『謎』22回%N第二位『衝撃』19回", kOffsetRankingDirective},
        {"", "ザラッとした感触。ひどいサメ肌。", kOffsetSamehada},
        {"", "どういうお手入れしてるんだ？", kOffsetSkincare},
    };
    return kBatch;
}

const std::vector<GauntletLine>& cross_channel_gauntlet_context_after() {
    static const std::vector<GauntletLine> kLookahead = {
        {"太一", "「うわあああああっ！」", kOffsetLookaheadScream},
        {"", "突き上げられるままに、俺と霧は水上に飛び出した。",
         kOffsetLookaheadSurface},
        {"", "巨大なサメが、そこに口を開いていた。", kOffsetLookaheadShark},
        {"太一", "「な、なんでサメがっ！」", kOffsetLookaheadWhyShark},
    };
    return kLookahead;
}

std::string build_gauntlet_user_prompt() {
    const auto& batch = cross_channel_gauntlet_batch();
    const auto& lookahead = cross_channel_gauntlet_context_after();

    std::string prompt = "<lines_to_translate>\n";
    for (std::size_t i = kStartOffset; i < batch.size(); ++i) {
        prompt += std::to_string(i + kOneBasedDisplayOffset) + ". [" +
                  resolve_gauntlet_speaker(batch[i].speaker) + "] " +
                  batch[i].jp + "\n";
    }
    prompt += "</lines_to_translate>\n\n<context_after>\n";
    for (const GauntletLine& after_line : lookahead) {
        prompt += "[" + resolve_gauntlet_speaker(after_line.speaker) + "] " +
                  after_line.jp + "\n";
    }
    prompt +=
        "</context_after>\n\n"
        "Translate the " + std::to_string(batch.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use <context_after> for lookahead context, but do NOT translate it. "
        "Output EXACTLY " + std::to_string(batch.size()) +
        " numbered translations, one per line. "
        "Do NOT include [SPEAKER] tags in your output.";
    return prompt;
}

std::string gauntlet_system_prompt() {
    return R"(You are a professional Japanese-to-English translator specializing in Visual Novels.
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
- Taichi (太一): Kurosu Taichi. Crude, witty, manic, fragile narrator. Use he/him.
- Youko (曜子): Hasekura Youko. Stoic, fiercely devoted to Taichi. Use she/her.
- Touko (冬子): Kirihara Touko. Proud, sharp-tongued tsundere. Use she/her.
- Misato (見里): Miyasumi Misato, club president, Tomoki's older sister. Use she/her.
- Kiri (霧): Sakura Kiri. Defensive, earnest. Use she/her.
- Miki (美希): Yamanobe Miki. Playful, sharp. Use she/her.
- Tomoki (友貴): Shima Tomoki, Misato's younger brother. Grounded, blunt. Use he/him.
- Sakuraba (桜庭): Sakuraba Hiroshi. Deadpan eccentric. Use he/him.
- Miyuki (みゆき): Fukuhara Miyuki, broadcasting club member. Use she/her.
- Girl (少女): Girl.
</character_dossier>

<formatting_rules>
1. Output ONLY the English translation for each numbered line inside <lines_to_translate>.
2. Maintain the EXACT line numbers provided (1., 2., ..., N.), one translated line per number on a single physical output line.
3. Strictly NO [SPEAKER] tags, translator notes, romaji glosses, commentary, markdown fences, or conversational filler.
4. Preserve quotation envelopes: lines wrapped in 「...」 or 『...』 must be wrapped in straight double quotes ("..."). Do NOT add outer quotes to unquoted narration.
5. Never combine, merge, reorder, or skip lines.
6. Preserve any literal \r\n or %N engine line-break tokens that appear WITHIN a line in their corresponding position.
7. Use strictly printable ASCII characters only -- no Japanese characters, no accented letters, no em dashes, no smart quotes, no fullwidth spaces. Use -- for dashes, ... for ellipsis, and straight ' and " quotes.
</formatting_rules>)";
}

std::vector<std::string> parse_gauntlet_numbered_lines(
    std::string_view response_text,
    std::size_t expected_count) {
    if (expected_count == kZeroCount || response_text.empty()) {
        return {};
    }

    const std::string_view body = unwrap_outer_markdown_fence(response_text);
    std::vector<std::string> results;
    results.reserve(expected_count);

    std::size_t pos = kStartOffset;
    int expected_next = kFirstExpectedLineNumber;
    while (pos <= body.size()) {
        const std::size_t nl = body.find('\n', pos);
        const std::string_view raw_line =
            (nl == std::string_view::npos) ? body.substr(pos)
                                           : body.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? (body.size() + kSingleStep)
                                             : (nl + kSingleStep);

        const std::string_view trimmed = trim_ascii_view(raw_line);
        if (trimmed.empty()) {
            continue;
        }

        const std::string stripped = strip_leading_speaker_tags(trimmed);
        const NumberedLineMatch parsed = parse_numbered_opener(
            stripped.empty() ? trimmed : std::string_view(stripped));
        if (parsed.malformed) {
            return {};
        }
        if (parsed.matched) {
            if (parsed.number != expected_next ||
                static_cast<std::size_t>(parsed.number) > expected_count) {
                return {};
            }
            results.push_back(parsed.body);
            ++expected_next;
            continue;
        }

        if (results.empty()) {
            return {};
        }
        const std::string cont = strip_leading_speaker_tags(trimmed);
        if (!cont.empty()) {
            results.back() += " " + cont;
        }
    }

    if (results.size() != expected_count) {
        return {};
    }
    return results;
}

GauntletVerdict evaluate_gauntlet_outputs(
    std::span<const std::string> eight_lines,
    std::int64_t thought_tokens) {
    GauntletVerdict verdict;

    if (thought_tokens < kBaselineMinThoughtTokens) {
        verdict.failures.push_back(
            "Gate 1 (reasoning floor): thought_tokens=" +
            std::to_string(thought_tokens) + " < " +
            std::to_string(kBaselineMinThoughtTokens));
    }

    if (eight_lines.size() != kGauntletLineCount) {
        verdict.failures.push_back(
            "Structural: expected " + std::to_string(kGauntletLineCount) +
            " lines, got " + std::to_string(eight_lines.size()));
        verdict.passed = false;
        return verdict;
    }

    std::array<std::string, kGauntletLineCount> lowered;
    for (std::size_t i = kStartOffset; i < kGauntletLineCount; ++i) {
        const std::string_view trimmed = trim_ascii_view(eight_lines[i]);
        if (trimmed.empty()) {
            verdict.failures.push_back(
                "Structural: line " +
                std::to_string(i + kOneBasedDisplayOffset) + " is empty");
        }
        if (contains_japanese_kana_or_kanji(eight_lines[i])) {
            verdict.failures.push_back(
                "Structural: line " +
                std::to_string(i + kOneBasedDisplayOffset) +
                " contains untranslated Japanese kana/kanji: " + eight_lines[i]);
        }
        lowered[i] = to_lower_ascii(eight_lines[i]);
    }

    if (lowered[kLineIdxFurigana].find("europ") == std::string::npos) {
        verdict.failures.push_back(
            "Gate 2 (furigana/vocab): line 1 missing 'europ': " +
            eight_lines[kLineIdxFurigana]);
    }
    if (!contains_any_substring(lowered[kLineIdxFurigana],
                                {"canop", "four-poster"})) {
        verdict.failures.push_back(
            "Gate 2 (furigana/vocab): line 1 missing 'canopy'/'four-poster': " +
            eight_lines[kLineIdxFurigana]);
    }
    if (contains_any_substring(lowered[kLineIdxFurigana],
                               {"yooroppa", "yoroppa", "ouropa"})) {
        verdict.failures.push_back(
            "Gate 2 (furigana/vocab): line 1 leaked romaji furigana: " +
            eight_lines[kLineIdxFurigana]);
    }

    if (!contains_any_substring(
            lowered[kLineIdxPluralSetup],
            {"two", "both", "guys", "senpais", "upperclassmen", "all", "lot"})) {
        verdict.failures.push_back(
            "Gate 3 (plural -gata): line 2 lost plural grouping: " +
            eight_lines[kLineIdxPluralSetup]);
    }
    if (contains_any_substring(lowered[kLineIdxPluralRetort],
                               {"gata", "rattle", "clatter"})) {
        verdict.failures.push_back(
            "Gate 3 (plural -gata): line 3 leaked romaji 'gata' or 'rattle': " +
            eight_lines[kLineIdxPluralRetort]);
    }
    if (!contains_any_substring(
            lowered[kLineIdxPluralRetort],
            {"two", "both", "guys", "senpais", "upperclassmen", "plural",
             "lump", "group", "together", "we", "us", "all", "lot"})) {
        verdict.failures.push_back(
            "Gate 3 (plural -gata): line 3 missing plural-grouping retort: " +
            eight_lines[kLineIdxPluralRetort]);
    }

    if (!contains_any_substring(
            lowered[kLineIdxDeadpanAside],
            {"lie", "lying", "not", "kidding", "false", "fake", "as if"})) {
        verdict.failures.push_back(
            "Gate 4 (deadpan aside): line 4 dropped '(lie)'/'(not)' aside: " +
            eight_lines[kLineIdxDeadpanAside]);
    }

    if (contains_any_substring(lowered[kLineIdxHomophone],
                               {"friend", "amicab"})) {
        verdict.failures.push_back(
            "Gate 5 (homophone typo): line 5 mistranslated 友好 as friendly: " +
            eight_lines[kLineIdxHomophone]);
    }
    if (!contains_any_substring(
            lowered[kLineIdxHomophone],
            {"effect", "advantag", "good use", "best use", "optim",
             "produc", "useful", "potent", "value", "favor", "efficac",
             "efficien"})) {
        verdict.failures.push_back(
            "Gate 5 (homophone typo): line 5 missing strategic effectiveness: " +
            eight_lines[kLineIdxHomophone]);
    }

    if (eight_lines[kLineIdxEngineBreak].find(kEngineLineBreakToken) ==
        std::string::npos) {
        verdict.failures.push_back(
            "Structural (%N directive): line 6 dropped '%N': " +
            eight_lines[kLineIdxEngineBreak]);
    }

    if (lowered[kLineIdxLookaheadPun].find("shark") == std::string::npos ||
        lowered[kLineIdxLookaheadPun].find("sandpaper") != std::string::npos) {
        verdict.failures.push_back(
            "Gate 6 (lookahead pun): line 7 failed to preserve 'shark' pun: " +
            eight_lines[kLineIdxLookaheadPun]);
    }

    verdict.passed = verdict.failures.empty();
    return verdict;
}

}  // namespace vn::llm::testing
