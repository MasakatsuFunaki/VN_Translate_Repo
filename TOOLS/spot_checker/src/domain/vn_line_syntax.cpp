// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "domain/vn_line_syntax.h"

#include "common/utf8_text.h"

#include <algorithm>
#include <array>

namespace spot_checker {

namespace {

constexpr std::string_view kCornerSingleOpen = "「";
constexpr std::string_view kCornerDoubleOpen = "『";
constexpr std::string_view kFullwidthSlash = "／";
constexpr std::string_view kFullwidthUpperH = "Ｈ";
constexpr std::string_view kFullwidthParenUpperH = "（Ｈ）";
constexpr std::string_view kLiteralNewlineEscape = "\\n";
constexpr std::string_view kLiteralCrlfEscape = "\\r\\n";
constexpr std::string_view kLiteralWaitEscape = "\\@";
constexpr std::string_view kCrlfLiteral = "\r\n";
constexpr std::string_view kPauseAmpersandToken = "$&";
constexpr std::string_view kStringVarPrefix = "$str";
constexpr std::string_view kTagLineBreak = "[r]";
constexpr std::string_view kTagPageWait = "[p]";
constexpr std::string_view kTagClickWait = "[l]";
constexpr std::string_view kTagNewPage = "[np]";
constexpr std::string_view kRubyAngleOpen = "<\"";
constexpr std::string_view kRubyAngleClose = "\">";

constexpr std::size_t kMaxNoteSpanBytes = 160;
constexpr std::size_t kEscapedPercentBytes = 2;
constexpr std::size_t kDoubleLengthModifierBytes = 2;
constexpr std::size_t kBackslashEscapeBytes = 2;
constexpr std::size_t kSymmetricAsciiPairBytes = 2;
constexpr std::size_t kMinSingleWordRomajiBytes = 4;
constexpr std::size_t kMaxSingleWordRomajiBytes = 14;
constexpr std::size_t kEstimatedBytesPerJpCp = 2;
constexpr std::size_t kMinRepeatedMoanPairCount = 2;
constexpr std::size_t kMinElongatedRepeatTwo = 2;
constexpr std::size_t kMinElongatedRepeatThree = 3;
constexpr int kMaxSpeakerStripPasses = 2;
constexpr char32_t kIdeographicSpaceCp = 0x3000U;
constexpr char32_t kBomCp = 0xFEFFU;

constexpr std::array<std::string_view, 24> kJapaneseExplicitMarkers = {
    "肉棒",     "亀頭",   "膣内",     "膣奥",       "陰茎",       "陰唇",
    "秘裂",     "蜜壺",   "射精",     "中出し",     "フェラ",     "パイズリ",
    "クリトリス", "アナル", "ペニス",   "ヴァギナ",   "まんこ",     "ちんぽ",
    "ちんこ",   "おまんこ", "おちんちん", "精液",     "愛液",       "絶頂",
};

constexpr std::array<std::string_view, 17> kEnglishExplicitWords = {
    "cock",     "cocks",   "pussy",    "cunt",    "penis",   "vagina",
    "clit",     "clitoris", "cum",     "cumming", "semen",   "blowjob",
    "fellatio", "paizuri", "orgasm",   "erection", "dick",
};

constexpr std::array<std::string_view, 28> kJapaneseLiteraryErogeMarkers = {
    "秘部",   "秘所",   "花弁",   "肉襞",     "粘膜",   "蜜液",   "淫液",
    "白濁",   "潮吹",   "抽挿",   "結合",     "交尾",   "性器",   "陰部",
    "股間",   "勃起",   "乳首",   "子宮",     "性感帯", "愛撫",   "快楽",
    "嬌声",   "淫ら",   "淫靡",   "淫音",     "肉欲",   "情事",   "突き上げ",
};

constexpr std::array<std::string_view, 20> kEnglishLiteraryErogeWords = {
    "shaft",       "folds",      "wetness",   "member",     "groin",
    "loins",       "climax",     "climaxed",  "thrust",     "thrusts",
    "thrusting",   "slick",      "nipple",    "nipples",    "ejaculate",
    "ejaculation", "precum",     "arousal",   "lovemaking", "dicks",
};

constexpr std::array<std::string_view, 25> kJapaneseWetOrHeartSfx = {
    "♡",        "♥",        "んちゅ", "ちゅっ", "ちゅぱ", "ちゅぷ",
    "じゅる",   "じゅぷ",   "じゅぽ", "れろれろ", "れろっ", "れろぉ",
    "ぴちゃ",   "くちゃ",   "くちゅ", "ぐちゅ", "ぬちゅ", "ぬぷ",
    "ずぷ",     "ずぶっ",   "ずぶずぶ", "びゅる", "びゅっ", "どぷ",
    "どぴゅ",
};

constexpr std::array<std::string_view, 15> kEnglishWetSfxWords = {
    "slurp",     "slurps",    "slurped",    "slurping",   "squelch",
    "squelched", "squelching", "schlick",   "schlicks",   "schlicking",
    "splurt",    "splurts",   "splurted",   "splurting",  "wetly",
};

constexpr std::array<std::string_view, 52> kJapaneseCombatOrChaseMarkers = {
    "ぐあっ",     "ぐあぁ",   "ぐわ",     "ぎゃあ",   "ぎゃぁ",   "うわあ",
    "うわぁ",     "うああ",   "きゃああ", "きゃぁ",   "ひいい",   "ひぃぃ",
    "くそっ",     "くそぉ",   "ちくしょう", "てめぇ", "野郎",     "貴様",
    "流血",       "出血",     "鮮血",     "血糊",     "血飛沫",   "血まみれ",
    "吐血",       "負傷",     "傷口",     "切り傷",   "致命傷",   "深手",
    "剣",         "刀",       "刃",       "銃",       "弾丸",     "斬り",
    "斬られ",     "斬撃",     "突き刺",   "刺され",   "殴り",     "殴られ",
    "蹴り",       "敵",       "怪物",     "化け物",   "死体",     "ぶっ殺",
    "殺してやる", "殺され",   "戦闘",     "攻撃",
};

constexpr std::array<std::string_view, 8> kJapaneseChaseMarkers = {
    "走っ", "走り", "全力で", "逃げ", "息切れ", "悲鳴", "追跡", "追手",
};

constexpr std::array<std::string_view, 52> kEnglishCombatOrChaseWords = {
    "gah",       "argh",     "aargh",    "gwah",     "gyah",      "ack",
    "oof",       "ouch",     "huff",     "huffing",  "wheeze",    "wheezing",
    "blood",     "bleeding", "wound",    "wounded",  "injury",    "blade",
    "sword",     "knife",    "dagger",   "gun",      "bullet",    "slash",
    "slashed",   "stab",     "stabbed",  "punch",    "punched",   "kick",
    "kicked",    "attack",   "attacked", "dodge",    "dodged",    "battle",
    "combat",    "enemy",    "enemies",  "monster",  "monsters",  "corpse",
    "scream",    "screamed", "screaming", "sprinted", "sprinting", "flee",
    "fled",      "chased",   "chasing",  "running",
};

constexpr std::array<std::string_view, 27> kJapaneseEroticVocalMarkers = {
    "んんっ",   "んぅ",     "あんっ", "あぁん",   "ああん",     "あぁっ",
    "あぁぁ",   "はぁぁ",   "くぅぅ", "はぁん",   "ふぁ",       "ふあぁ",
    "ひゃん",   "ひゃぅ",   "きゃん", "やぁん",   "やぁっ",     "くぅっ",
    "くぅん",   "いくぅ",   "イくっ", "イッちゃ", "イっちゃ",   "だめぇ",
    "そこぉ",   "気持ちいい", "気持ちよく",
};

constexpr std::array<std::string_view, 16> kEnglishEroticVocalWords = {
    "nngh",   "nnngh",   "hnng",    "hnngh",     "mmph", "mmmph",
    "ngh",    "ungh",    "fwaa",    "hyahn",     "kyahn", "nyaaah",
    "moan",   "moaned",  "moaning", "throbbing",
};

constexpr std::array<std::string_view, 6> kEnglishRepeatedMoanWords = {
    "ah", "ahh", "aah", "ooh", "mmh", "unh",
};

constexpr std::array<std::string_view, 15> kEnglishAmbiguousBreathWords = {
    "haah",    "haa",     "kuh",     "aah",       "ahh",
    "pant",    "panted",  "panting", "gasp",      "gasps",
    "gasped",  "gasping", "shudder", "shuddered", "shuddering",
};

constexpr std::array<std::string_view, 14> kTranslatorNotePrefixes = {
    "tl:",
    "tl ",
    "t/n",
    "tn:",
    "translator",
    "translation note",
    "note:",
    "ed:",
    "editor",
    "lit.",
    "lit:",
    "literally ",
    "romaji:",
    "pronounced ",
};

constexpr std::array<std::string_view, 42> kRomajiGlossTerms = {
    "onee-chan",   "oneechan",     "onee-san",    "oneesan",
    "onii-chan",   "oniichan",     "onii-san",    "oniisan",
    "nee-san",     "neesan",       "nee-chan",    "neechan",
    "nii-san",     "niisan",       "nii-chan",    "niichan",
    "imouto",      "otouto",       "senpai",      "sempai",
    "kouhai",      "kohai",        "sensei",      "ojou-sama",
    "ojousama",    "obaa-chan",    "obaachan",    "ojii-chan",
    "ojiichan",    "okaasan",      "otousan",     "okaa-san",
    "otou-san",    "itadakimasu",  "gochisousama", "tadaima",
    "okaeri",      "yoroshiku",    "nakama",      "keigo",
    "tsundere",    "kansai-ben",
};

bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool is_ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char lower_ascii_char(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool starts_with_sv(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with_sv(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with_ascii_ci(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower_ascii_char(text[i]) != lower_ascii_char(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool ends_with_ascii_ci(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (lower_ascii_char(text[offset + i]) != lower_ascii_char(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool equals_ascii_ci(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && starts_with_ascii_ci(a, b);
}

bool is_horizontal_trim_cp(char32_t cp) noexcept {
    return cp == U' ' || cp == U'\t' || cp == kIdeographicSpaceCp ||
           cp == kBomCp;
}

bool is_trim_cp(char32_t cp) noexcept {
    return is_horizontal_trim_cp(cp) || cp == U'\r' || cp == U'\n';
}

std::string_view trim_horizontal_unicode_view(std::string_view text) noexcept {
    std::size_t index = 0;
    std::size_t first_non_ws = 0;
    std::size_t after_last_non_ws = 0;
    bool found = false;
    while (index < text.size()) {
        const std::size_t start = index;
        const char32_t cp = utf8_next(text, index);
        if (!is_horizontal_trim_cp(cp)) {
            if (!found) {
                first_non_ws = start;
                found = true;
            }
            after_last_non_ws = index;
        }
    }
    if (!found) {
        return {};
    }
    return text.substr(first_non_ws, after_last_non_ws - first_non_ws);
}

std::string_view trim_unicode_view(std::string_view text) noexcept {
    std::size_t index = 0;
    std::size_t first_non_ws = 0;
    std::size_t after_last_non_ws = 0;
    bool found = false;
    while (index < text.size()) {
        const std::size_t start = index;
        const char32_t cp = utf8_next(text, index);
        if (!is_trim_cp(cp)) {
            if (!found) {
                first_non_ws = start;
                found = true;
            }
            after_last_non_ws = index;
        }
    }
    if (!found) {
        return {};
    }
    return text.substr(first_non_ws, after_last_non_ws - first_non_ws);
}

bool has_any_whitespace(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        if (is_trim_cp(utf8_next(text, index))) {
            return true;
        }
    }
    return false;
}

bool has_forbidden_speaker_header_punct(std::string_view header) noexcept {
    if (header.find(kLiteralNewlineEscape) != std::string_view::npos ||
        header.find(kLiteralWaitEscape) != std::string_view::npos) {
        return true;
    }
    std::size_t index = 0;
    while (index < header.size()) {
        const char32_t cp = utf8_next(header, index);
        if (cp == U'.' || cp == U'!' || cp == U'?' || cp == U'。' ||
            cp == U'！' || cp == U'？' || cp == U'"' || cp == U'「' ||
            cp == U'」' || cp == U'『' || cp == U'』') {
            return true;
        }
    }
    return false;
}

struct CoreEnvelopeSlice {
    std::string leading_escape;
    std::string trailing_escape;
    std::string_view core;
};

CoreEnvelopeSlice slice_envelope_escapes(std::string_view body) {
    CoreEnvelopeSlice result;
    std::string_view rem = trim_horizontal_unicode_view(body);

    while (!rem.empty()) {
        if (starts_with_sv(rem, kLiteralCrlfEscape)) {
            result.leading_escape.append(kLiteralCrlfEscape);
            rem = trim_horizontal_unicode_view(
                rem.substr(kLiteralCrlfEscape.size()));
            continue;
        }
        if (starts_with_sv(rem, kLiteralNewlineEscape)) {
            result.leading_escape.append(kLiteralNewlineEscape);
            rem = trim_horizontal_unicode_view(
                rem.substr(kLiteralNewlineEscape.size()));
            continue;
        }
        if (starts_with_sv(rem, kCrlfLiteral)) {
            result.leading_escape.append(kCrlfLiteral);
            rem = trim_horizontal_unicode_view(rem.substr(kCrlfLiteral.size()));
            continue;
        }
        if (rem.front() == '\n') {
            result.leading_escape.push_back('\n');
            rem = trim_horizontal_unicode_view(rem.substr(1U));
            continue;
        }
        break;
    }

    while (ends_with_sv(rem, kLiteralWaitEscape)) {
        result.trailing_escape.append(kLiteralWaitEscape);
        rem = trim_horizontal_unicode_view(
            rem.substr(0, rem.size() - kLiteralWaitEscape.size()));
    }

    result.core = rem;
    return result;
}

bool outer_pair_wraps_entire_span(std::string_view core,
                                  char32_t open_cp,
                                  char32_t close_cp) noexcept {
    std::size_t index = 0;
    if (index >= core.size()) {
        return false;
    }
    if (utf8_next(core, index) != open_cp) {
        return false;
    }
    int depth = 1;
    while (index < core.size()) {
        const char32_t cp = utf8_next(core, index);
        if (cp == open_cp) {
            ++depth;
        } else if (cp == close_cp) {
            --depth;
            if (depth == 0) {
                return index == core.size();
            }
        }
    }
    return false;
}

std::string_view strip_inline_slash_speaker_for_envelope(
    std::string_view core) noexcept {
    if (!starts_with_sv(core, kFullwidthSlash)) {
        return core;
    }
    const std::size_t single_open = core.find(kCornerSingleOpen);
    const std::size_t double_open = core.find(kCornerDoubleOpen);
    const std::size_t open_pos =
        (single_open < double_open) ? single_open : double_open;
    if (open_pos == std::string_view::npos ||
        open_pos > kMaxSpeakerPrefixHeaderBytes) {
        return core;
    }
    const std::string_view speaker_part =
        core.substr(kFullwidthSlash.size(), open_pos - kFullwidthSlash.size());
    if (speaker_part.empty() || has_forbidden_speaker_header_punct(speaker_part)) {
        return core;
    }
    return core.substr(open_pos);
}

std::size_t try_match_printf_specifier(std::string_view text,
                                       std::size_t pos) noexcept {
    if (pos + 1U >= text.size() || text[pos] != '%') {
        return 0;
    }
    if (text[pos + 1U] == '%') {
        return kEscapedPercentBytes;
    }

    std::size_t cur = pos + 1U;
    bool saw_space_flag = false;
    while (cur < text.size() &&
           (text[cur] == '+' || text[cur] == '-' || text[cur] == '0' ||
            text[cur] == ' ' || text[cur] == '#')) {
        if (text[cur] == ' ') {
            saw_space_flag = true;
        }
        ++cur;
    }

    while (cur < text.size() && is_ascii_digit(text[cur])) {
        ++cur;
    }

    if (cur < text.size() && text[cur] == '.') {
        ++cur;
        if (cur >= text.size() || !is_ascii_digit(text[cur])) {
            return 0;
        }
        while (cur < text.size() && is_ascii_digit(text[cur])) {
            ++cur;
        }
    }

    if (cur < text.size()) {
        if ((text[cur] == 'h' || text[cur] == 'l') && cur + 1U < text.size() &&
            text[cur + 1U] == text[cur]) {
            cur += kDoubleLengthModifierBytes;
        } else if (text[cur] == 'h' || text[cur] == 'l' || text[cur] == 'z') {
            ++cur;
        }
    }

    if (cur >= text.size()) {
        return 0;
    }
    const char conv = text[cur];
    if (conv != 's' && conv != 'd' && conv != 'i' && conv != 'f' &&
        conv != 'u' && conv != 'x' && conv != 'X') {
        return 0;
    }

    if (saw_space_flag) {
        const bool preceded_by_digit = pos > 0U && is_ascii_digit(text[pos - 1U]);
        const bool followed_by_alpha =
            (cur + 1U < text.size()) && is_ascii_alpha(text[cur + 1U]);
        if (preceded_by_digit || followed_by_alpha || conv == 's') {
            return 0;
        }
    }

    return (cur + 1U) - pos;
}

std::size_t try_match_signed_int_angle_tag(std::string_view text,
                                           std::size_t pos) noexcept {
    if (pos >= text.size() || text[pos] != '<') {
        return 0;
    }
    std::size_t cur = pos + 1U;
    if (cur < text.size() && (text[cur] == '+' || text[cur] == '-')) {
        ++cur;
    }
    const std::size_t digits_start = cur;
    while (cur < text.size() && is_ascii_digit(text[cur])) {
        ++cur;
    }
    if (cur == digits_start || cur >= text.size() || text[cur] != '>') {
        return 0;
    }
    return (cur + 1U) - pos;
}

std::size_t try_match_markup_angle_tag(std::string_view text,
                                       std::size_t pos) noexcept {
    if (pos >= text.size() || text[pos] != '<') {
        return 0;
    }
    std::size_t cur = pos + 1U;
    if (cur < text.size() && text[cur] == '/') {
        ++cur;
    }
    if (cur >= text.size() || (!is_ascii_alpha(text[cur]) && text[cur] != '!')) {
        return 0;
    }
    ++cur;
    while (cur < text.size() && text[cur] != '>' && text[cur] != '\r' &&
           text[cur] != '\n') {
        ++cur;
    }
    if (cur >= text.size() || text[cur] != '>') {
        return 0;
    }
    return (cur + 1U) - pos;
}

bool is_single_word_romaji_gloss(std::string_view inner) noexcept {
    if (inner.size() < kMinSingleWordRomajiBytes ||
        inner.size() > kMaxSingleWordRomajiBytes) {
        return false;
    }
    for (char c : inner) {
        if (!is_ascii_alpha(c)) {
            return false;
        }
    }
    constexpr std::array<std::string_view, 8> kEnglishUExceptions = {
        "menu", "guru", "thru", "tutu", "tofu", "haiku", "impromptu", "bayou",
    };
    for (const std::string_view ex : kEnglishUExceptions) {
        if (equals_ascii_ci(inner, ex)) {
            return false;
        }
    }
    if (lower_ascii_char(inner.back()) == 'u') {
        return true;
    }
    constexpr std::array<std::string_view, 7> kRomajiClusters = {
        "ppa", "ppi", "ppo", "cchi", "ccha", "sshi", "ssha",
    };
    for (const std::string_view cluster : kRomajiClusters) {
        for (std::size_t pos = 0; pos + cluster.size() <= inner.size(); ++pos) {
            if (starts_with_ascii_ci(inner.substr(pos), cluster)) {
                return true;
            }
        }
    }
    return false;
}

bool span_is_translator_or_romaji_note(std::string_view raw_inner) noexcept {
    const std::string_view inner = trim_ascii_view(raw_inner);
    if (inner.empty() || inner.size() > kMaxNoteSpanBytes) {
        return false;
    }
    for (const std::string_view prefix : kTranslatorNotePrefixes) {
        if (starts_with_ascii_ci(inner, prefix)) {
            return true;
        }
    }
    for (const std::string_view term : kRomajiGlossTerms) {
        if (contains_word_ascii_ci(inner, term)) {
            return true;
        }
    }
    constexpr std::array<std::string_view, 7> kHonorificSuffixes = {
        "-san", "-chan", "-kun", "-sama", "-dono", "-senpai", "-sensei",
    };
    for (const std::string_view suffix : kHonorificSuffixes) {
        if (ends_with_ascii_ci(inner, suffix)) {
            return true;
        }
    }
    return is_single_word_romaji_gloss(inner);
}

std::string replace_escaped_newlines(std::string_view text,
                                     std::string_view replacement) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (starts_with_sv(text.substr(i), kLiteralCrlfEscape)) {
            out.append(replacement);
            i += kLiteralCrlfEscape.size();
            continue;
        }
        if (starts_with_sv(text.substr(i), kLiteralNewlineEscape)) {
            out.append(replacement);
            i += kLiteralNewlineEscape.size();
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool matches_speaker_label(std::string_view candidate_label,
                           std::string_view speaker_a,
                           std::string_view speaker_b) noexcept {
    std::string_view clean = trim_unicode_view(candidate_label);
    if (clean.size() >= kSymmetricAsciiPairBytes && clean.front() == '[' &&
        clean.back() == ']') {
        clean = trim_unicode_view(
            clean.substr(1U, clean.size() - kSymmetricAsciiPairBytes));
    }
    if (clean.empty()) {
        return false;
    }
    if (!speaker_a.empty() && speaker_a != "NARRATION" &&
        equals_ascii_ci(clean, speaker_a)) {
        return true;
    }
    if (!speaker_b.empty() && speaker_b != "NARRATION" &&
        equals_ascii_ci(clean, speaker_b)) {
        return true;
    }
    return false;
}

std::string strip_echoed_speaker_labels(std::string_view raw_body,
                                        std::string_view speaker_a,
                                        std::string_view speaker_b,
                                        std::string_view original_body) {
    std::string current = trim_ascii(raw_body);
    for (int pass = 0; pass < kMaxSpeakerStripPasses; ++pass) {
        const SpeakerSplit split = split_speaker_prefix(current);
        if (!split.prefix.empty() &&
            matches_speaker_label(split.speaker_header, speaker_a, speaker_b)) {
            current = trim_ascii(split.body);
            continue;
        }

        const std::string_view view(current);
        if (!view.empty() && view.front() == '[') {
            const std::size_t close = view.find(']');
            if (close != std::string_view::npos &&
                close <= kMaxSpeakerPrefixHeaderBytes + kSymmetricAsciiPairBytes) {
                const std::string_view bracket_name = view.substr(1U, close - 1U);
                if (matches_speaker_label(bracket_name, speaker_a, speaker_b)) {
                    std::size_t rest_pos = close + 1U;
                    if (rest_pos < view.size() && view[rest_pos] == ':') {
                        ++rest_pos;
                    }
                    if (!starts_with_sv(trim_ascii_view(original_body),
                                        view.substr(0, close + 1U))) {
                        current = trim_ascii(view.substr(rest_pos));
                        continue;
                    }
                }
            }
        }

        const std::size_t colon = view.find(':');
        if (colon != std::string_view::npos && colon <= kMaxSpeakerPrefixHeaderBytes) {
            const std::string_view before_colon = view.substr(0, colon);
            if (matches_speaker_label(before_colon, speaker_a, speaker_b) &&
                !starts_with_ascii_ci(trim_ascii_view(original_body),
                                      view.substr(0, colon + 1U))) {
                current = trim_ascii(view.substr(colon + 1U));
                continue;
            }
        }
        break;
    }
    return current;
}

bool is_jp_interjection_open_boundary(char32_t cp) noexcept {
    return cp == U'「' || cp == U'『' || cp == U'（' || cp == U'(' ||
           cp == U'、' || cp == U'。' || cp == U'…' || cp == U'・' ||
           cp == U'！' || cp == U'？' || cp == U'!' || cp == U'?' ||
           cp == U'ー' || cp == U'〜' || cp == U'～' || cp == U'っ' ||
           is_trim_cp(cp);
}

bool is_jp_interjection_close_boundary(char32_t cp) noexcept {
    return cp == U'」' || cp == U'』' || cp == U'）' || cp == U')' ||
           cp == U'、' || cp == U'。' || cp == U'…' || cp == U'・' ||
           cp == U'！' || cp == U'？' || cp == U'!' || cp == U'?' ||
           cp == U'ー' || cp == U'〜' || cp == U'～' || cp == U'♡' ||
           cp == U'♥' || cp == U'♪' || is_trim_cp(cp);
}

std::size_t consume_char_run(std::string_view w,
                             std::size_t& pos,
                             char target) noexcept {
    const std::size_t start = pos;
    while (pos < w.size() && lower_ascii_char(w[pos]) == target) {
        ++pos;
    }
    return pos - start;
}

bool is_elongated_erotic_word(std::string_view w) noexcept {
    {
        std::size_t p = 0;
        const std::size_t a_cnt = consume_char_run(w, p, 'a');
        const std::size_t g_cnt = consume_char_run(w, p, 'g');
        const std::size_t h_cnt = consume_char_run(w, p, 'h');
        if (p == w.size() && a_cnt >= 1U && g_cnt >= 1U &&
            h_cnt >= kMinElongatedRepeatThree) {
            return true;
        }
    }
    {
        std::size_t p = 0;
        const std::size_t k_cnt = consume_char_run(w, p, 'k');
        const std::size_t u_cnt = consume_char_run(w, p, 'u');
        const std::size_t h_cnt = consume_char_run(w, p, 'h');
        if (p == w.size() && k_cnt >= 1U && u_cnt >= 1U &&
            (h_cnt >= kMinElongatedRepeatThree ||
             (u_cnt >= kMinElongatedRepeatTwo &&
              h_cnt >= kMinElongatedRepeatTwo))) {
            return true;
        }
    }
    {
        std::size_t p = 0;
        const std::size_t h1_cnt = consume_char_run(w, p, 'h');
        const std::size_t a_cnt = consume_char_run(w, p, 'a');
        const std::size_t h2_cnt = consume_char_run(w, p, 'h');
        if (p == w.size() && h1_cnt >= 1U &&
            ((h2_cnt >= 1U &&
              (a_cnt >= kMinElongatedRepeatThree ||
               h2_cnt >= kMinElongatedRepeatThree ||
               (a_cnt >= kMinElongatedRepeatTwo &&
                h2_cnt >= kMinElongatedRepeatTwo))) ||
             (h2_cnt == 0U && a_cnt >= kMinElongatedRepeatThree))) {
            return true;
        }
    }
    {
        std::size_t p = 0;
        const std::size_t a_cnt = consume_char_run(w, p, 'a');
        const std::size_t h_cnt = consume_char_run(w, p, 'h');
        if (p == w.size() && a_cnt >= 1U && h_cnt >= 1U &&
            (a_cnt >= kMinElongatedRepeatThree ||
             h_cnt >= kMinElongatedRepeatThree)) {
            return true;
        }
    }
    {
        std::size_t p = 0;
        static_cast<void>(consume_char_run(w, p, 'h'));
        const std::size_t n_cnt = consume_char_run(w, p, 'n');
        const std::size_t g_cnt = consume_char_run(w, p, 'g');
        const std::size_t h_cnt = consume_char_run(w, p, 'h');
        if (p == w.size() && n_cnt >= 1U && g_cnt >= 1U && h_cnt >= 1U) {
            return true;
        }
    }
    {
        std::size_t p = 0;
        const std::size_t m_cnt = consume_char_run(w, p, 'm');
        const std::size_t p_cnt = consume_char_run(w, p, 'p');
        const std::size_t h_cnt = consume_char_run(w, p, 'h');
        if (p == w.size() && m_cnt >= kMinElongatedRepeatTwo &&
            p_cnt >= 1U && h_cnt >= 1U) {
            return true;
        }
    }
    return false;
}

bool has_elongated_erotic_vocalization_en(std::string_view en_body) noexcept {
    std::size_t i = 0;
    while (i < en_body.size()) {
        while (i < en_body.size() && !is_ascii_alpha(en_body[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < en_body.size() && is_ascii_alpha(en_body[i])) {
            ++i;
        }
        if (i > start &&
            is_elongated_erotic_word(en_body.substr(start, i - start))) {
            return true;
        }
    }
    return false;
}

bool has_repeated_moan_pair_en(std::string_view en_body) noexcept {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < en_body.size()) {
        while (i < en_body.size() && !is_ascii_alpha(en_body[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < en_body.size() && is_ascii_alpha(en_body[i])) {
            ++i;
        }
        if (i == start) {
            break;
        }
        const std::string_view token = en_body.substr(start, i - start);
        for (const std::string_view moan : kEnglishRepeatedMoanWords) {
            if (equals_ascii_ci(token, moan)) {
                ++count;
                if (count >= kMinRepeatedMoanPairCount) {
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

bool has_wet_sfx_or_heart_cue(std::string_view jp_body,
                              std::string_view en_body) noexcept {
    for (const std::string_view sfx : kJapaneseWetOrHeartSfx) {
        if (jp_body.find(sfx) != std::string_view::npos) {
            return true;
        }
    }
    if (en_body.find("♡") != std::string_view::npos ||
        en_body.find("♥") != std::string_view::npos) {
        return true;
    }
    for (const std::string_view word : kEnglishWetSfxWords) {
        if (contains_word_ascii_ci(en_body, word)) {
            return true;
        }
    }
    return false;
}

bool has_combat_chase_or_horror_cue(std::string_view jp_body,
                                    std::string_view en_body) noexcept {
    for (const std::string_view marker : kJapaneseCombatOrChaseMarkers) {
        if (jp_body.find(marker) != std::string_view::npos) {
            return true;
        }
    }
    for (const std::string_view marker : kJapaneseChaseMarkers) {
        if (jp_body.find(marker) != std::string_view::npos) {
            return true;
        }
    }
    for (const std::string_view word : kEnglishCombatOrChaseWords) {
        if (contains_word_ascii_ci(en_body, word)) {
            return true;
        }
    }
    return false;
}

struct JpVocalScan {
    bool has_nasal_or_repeated_moan = false;
    bool has_bounded_breath_or_gasp = false;
};

JpVocalScan scan_jp_vocalizations(std::string_view jp_body) {
    JpVocalScan res;
    if (jp_body.empty()) {
        return res;
    }
    std::vector<char32_t> cps;
    cps.reserve(jp_body.size() / kEstimatedBytesPerJpCp + 1U);
    std::size_t idx = 0;
    while (idx < jp_body.size()) {
        cps.push_back(utf8_next(jp_body, idx));
    }

    std::size_t moan_interjection_count = 0;
    for (std::size_t i = 0; i + 1U < cps.size(); ++i) {
        const bool open_ok =
            (i == 0U) || is_jp_interjection_open_boundary(cps[i - 1U]);
        const bool close_ok =
            (i + kSymmetricAsciiPairBytes >= cps.size()) ||
            is_jp_interjection_close_boundary(cps[i + kSymmetricAsciiPairBytes]);

        if (cps[i] == U'ん' && cps[i + 1U] == U'っ' &&
            (open_ok || cps[i - 1U] == U'ん')) {
            res.has_nasal_or_repeated_moan = true;
            ++moan_interjection_count;
            continue;
        }

        if (open_ok && close_ok) {
            if ((cps[i] == U'あ' &&
                 (cps[i + 1U] == U'っ' || cps[i + 1U] == U'ぁ'))) {
                res.has_bounded_breath_or_gasp = true;
                ++moan_interjection_count;
            } else if ((cps[i] == U'は' && cps[i + 1U] == U'ぁ') ||
                       (cps[i] == U'く' &&
                        (cps[i + 1U] == U'ぅ' || cps[i + 1U] == U'っ'))) {
                res.has_bounded_breath_or_gasp = true;
            }
        }
    }

    if (moan_interjection_count >= kMinRepeatedMoanPairCount) {
        res.has_nasal_or_repeated_moan = true;
    }
    return res;
}

}  // namespace

SpeakerSplit split_speaker_prefix(std::string_view text) {
    const std::size_t nl = text.find('\n');
    if (nl == std::string_view::npos) {
        return {{}, {}, std::string(text)};
    }

    const std::size_t header_end = (nl > 0U && text[nl - 1U] == '\r') ? (nl - 1U) : nl;
    const std::string_view raw_header = text.substr(0, header_end);
    const std::string_view trimmed_header = trim_unicode_view(raw_header);
    if (trimmed_header.empty() ||
        trimmed_header.size() > kMaxSpeakerPrefixHeaderBytes ||
        has_forbidden_speaker_header_punct(trimmed_header)) {
        return {{}, {}, std::string(text)};
    }

    const std::string_view body_view = text.substr(nl + 1U);
    if (trim_unicode_view(body_view).empty()) {
        return {{}, {}, std::string(text)};
    }

    return SpeakerSplit{
        std::string(text.substr(0, nl + 1U)),
        std::string(trimmed_header),
        std::string(body_view),
    };
}

std::string extract_speaker_prefix(std::string_view text) {
    return split_speaker_prefix(text).prefix;
}

std::string strip_speaker_prefix_body(std::string_view text) {
    return split_speaker_prefix(text).body;
}

QuoteEnvelope analyze_quote_envelope(std::string_view body) {
    const CoreEnvelopeSlice sliced = slice_envelope_escapes(body);
    QuoteEnvelope env;
    env.leading_escape = sliced.leading_escape;
    env.trailing_escape = sliced.trailing_escape;

    const std::string_view core = sliced.core;
    for (char c : core) {
        if (c == '"') {
            ++env.ascii_double_quote_count;
        }
    }
    if (core.empty()) {
        return env;
    }

    const std::string_view corner_core = strip_inline_slash_speaker_for_envelope(core);
    if (outer_pair_wraps_entire_span(corner_core, U'「', U'」')) {
        env.outer_kind = OuterWrapKind::CornerQuoteSingle;
        return env;
    }
    if (outer_pair_wraps_entire_span(corner_core, U'『', U'』')) {
        env.outer_kind = OuterWrapKind::CornerQuoteDouble;
        return env;
    }
    if (outer_pair_wraps_entire_span(core, U'(', U')') ||
        outer_pair_wraps_entire_span(core, U'（', U'）')) {
        env.outer_kind = OuterWrapKind::AsciiParen;
        return env;
    }

    const bool starts_quote = core.front() == '"';
    const bool ends_quote = core.back() == '"';
    if (starts_quote && ends_quote && core.size() >= kSymmetricAsciiPairBytes &&
        env.ascii_double_quote_count == kSymmetricAsciiPairBytes) {
        env.outer_kind = OuterWrapKind::AsciiDoubleQuote;
        return env;
    }
    if (starts_quote && !ends_quote && !env.balanced_ascii_quotes()) {
        env.outer_kind = OuterWrapKind::UnclosedLeadingAsciiQuote;
        return env;
    }
    if (!starts_quote && ends_quote && !env.balanced_ascii_quotes()) {
        env.outer_kind = OuterWrapKind::UnclosedTrailingAsciiQuote;
        return env;
    }

    return env;
}

bool quote_envelopes_compatible(const QuoteEnvelope& jp_env,
                                const QuoteEnvelope& old_en_env,
                                const QuoteEnvelope& candidate_en_env) noexcept {
    if (candidate_en_env.leading_escape != old_en_env.leading_escape ||
        candidate_en_env.trailing_escape != old_en_env.trailing_escape) {
        return false;
    }

    if (old_en_env.outer_kind == OuterWrapKind::UnclosedLeadingAsciiQuote ||
        old_en_env.outer_kind == OuterWrapKind::UnclosedTrailingAsciiQuote) {
        return candidate_en_env.outer_kind == old_en_env.outer_kind &&
               !candidate_en_env.balanced_ascii_quotes() &&
               candidate_en_env.ascii_double_quote_count ==
                   old_en_env.ascii_double_quote_count;
    }

    if (old_en_env.balanced_ascii_quotes() &&
        !candidate_en_env.balanced_ascii_quotes()) {
        return false;
    }

    if (old_en_env.outer_kind == OuterWrapKind::CornerQuoteSingle ||
        old_en_env.outer_kind == OuterWrapKind::CornerQuoteDouble) {
        if (candidate_en_env.outer_kind != old_en_env.outer_kind) {
            return false;
        }
        if (old_en_env.ascii_double_quote_count == 0U &&
            candidate_en_env.ascii_double_quote_count > 0U) {
            return false;
        }
        return true;
    }

    if (candidate_en_env.outer_kind == old_en_env.outer_kind) {
        if (old_en_env.outer_kind == OuterWrapKind::None &&
            old_en_env.ascii_double_quote_count > 0U &&
            candidate_en_env.ascii_double_quote_count == 0U) {
            return false;
        }
        return true;
    }

    if (old_en_env.outer_kind == OuterWrapKind::None &&
        candidate_en_env.outer_kind == OuterWrapKind::AsciiDoubleQuote &&
        (jp_env.outer_kind == OuterWrapKind::CornerQuoteSingle ||
         jp_env.outer_kind == OuterWrapKind::CornerQuoteDouble)) {
        return true;
    }

    if (old_en_env.outer_kind == OuterWrapKind::None &&
        candidate_en_env.outer_kind == OuterWrapKind::AsciiParen &&
        jp_env.outer_kind == OuterWrapKind::AsciiParen) {
        return true;
    }

    return false;
}

std::vector<std::string> extract_engine_control_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];

        if (ch == '%') {
            const std::size_t len = try_match_printf_specifier(text, i);
            if (len > 0U) {
                tokens.emplace_back(text.substr(i, len));
                i += len;
                continue;
            }
        }

        if (ch == '$') {
            if (starts_with_sv(text.substr(i), kPauseAmpersandToken)) {
                tokens.emplace_back(kPauseAmpersandToken);
                i += kPauseAmpersandToken.size();
                continue;
            }
            if (i + kStringVarPrefix.size() < text.size() &&
                starts_with_sv(text.substr(i), kStringVarPrefix) &&
                is_ascii_digit(text[i + kStringVarPrefix.size()])) {
                std::size_t end = i + kStringVarPrefix.size() + 1U;
                while (end < text.size() && is_ascii_digit(text[end])) {
                    ++end;
                }
                tokens.emplace_back(text.substr(i, end - i));
                i = end;
                continue;
            }
        }

        if (ch == '<') {
            const std::size_t num_len = try_match_signed_int_angle_tag(text, i);
            if (num_len > 0U) {
                tokens.emplace_back(text.substr(i, num_len));
                i += num_len;
                continue;
            }
            const std::size_t tag_len = try_match_markup_angle_tag(text, i);
            if (tag_len > 0U) {
                tokens.emplace_back(text.substr(i, tag_len));
                i += tag_len;
                continue;
            }
        }

        if (ch == '[') {
            const std::string_view rem = text.substr(i);
            if (starts_with_sv(rem, kTagLineBreak) ||
                starts_with_sv(rem, kTagPageWait) ||
                starts_with_sv(rem, kTagClickWait)) {
                tokens.emplace_back(rem.substr(0, kTagLineBreak.size()));
                i += kTagLineBreak.size();
                continue;
            }
            if (starts_with_sv(rem, kTagNewPage)) {
                tokens.emplace_back(kTagNewPage);
                i += kTagNewPage.size();
                continue;
            }
        }

        if (ch == '\\' && i + 1U < text.size()) {
            const char esc = text[i + 1U];
            if (esc == 'n' || esc == '@' || esc == 'p' || esc == 'w' ||
                esc == 'c' || esc == 'f') {
                tokens.emplace_back(text.substr(i, kBackslashEscapeBytes));
                i += kBackslashEscapeBytes;
                continue;
            }
        }

        if (ch == '\r') {
            if (starts_with_sv(text.substr(i), kCrlfLiteral)) {
                tokens.emplace_back("\n");
                i += kCrlfLiteral.size();
                continue;
            }
            ++i;
            continue;
        }

        if (ch == '\n') {
            tokens.emplace_back("\n");
            ++i;
            continue;
        }

        ++i;
    }
    return tokens;
}

std::string strip_inline_control_wrappers(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (starts_with_sv(text.substr(pos), kPauseAmpersandToken)) {
            pos += kPauseAmpersandToken.size();
            continue;
        }
        const std::size_t angle_len = try_match_signed_int_angle_tag(text, pos);
        if (angle_len > 0U) {
            pos += angle_len;
            if (pos < text.size() && text[pos] == '\\') {
                ++pos;
            }
            continue;
        }
        break;
    }

    std::string out;
    out.reserve(text.size() - pos);
    while (pos < text.size()) {
        if (starts_with_sv(text.substr(pos), kRubyAngleOpen)) {
            const std::size_t inner_start = pos + kRubyAngleOpen.size();
            const std::size_t close = text.find(kRubyAngleClose, inner_start);
            if (close != std::string_view::npos) {
                const std::string_view inner =
                    text.substr(inner_start, close - inner_start);
                const std::size_t pipe = inner.find('|');
                if (pipe != std::string_view::npos &&
                    inner.find('\n') == std::string_view::npos &&
                    inner.find('\r') == std::string_view::npos) {
                    out.append(inner.substr(0, pipe));
                    pos = close + kRubyAngleClose.size();
                    continue;
                }
            }
        }

        if (text[pos] == '[') {
            const std::size_t close = text.find(']', pos + 1U);
            if (close != std::string_view::npos) {
                const std::string_view inner =
                    text.substr(pos + 1U, close - (pos + 1U));
                const std::size_t pipe = inner.find('|');
                if (pipe != std::string_view::npos && pipe > 0U &&
                    pipe + 1U < inner.size() &&
                    inner.find('[') == std::string_view::npos &&
                    inner.find('\n') == std::string_view::npos &&
                    inner.find('\r') == std::string_view::npos) {
                    out.append(inner.substr(pipe + 1U));
                    pos = close + 1U;
                    continue;
                }
            }
        }

        out.push_back(text[pos]);
        ++pos;
    }
    return out;
}

bool is_engine_asset_or_command_token(std::string_view jp_text,
                                      std::string_view en_text) noexcept {
    if (!has_real_japanese(jp_text)) {
        return true;
    }
    if (jp_text != en_text) {
        return false;
    }
    const std::string_view trimmed = trim_unicode_view(jp_text);
    if (trimmed.empty() || has_any_whitespace(trimmed)) {
        return false;
    }
    if (trimmed.front() == '_' || trimmed.find('/') != std::string_view::npos) {
        return true;
    }
    return trimmed.find('@') != std::string_view::npos &&
           trimmed.find('_') != std::string_view::npos;
}

bool has_speaker_h_tag_suffix(std::string_view speaker) noexcept {
    const std::string_view trimmed = trim_unicode_view(speaker);
    if (trimmed.empty()) {
        return false;
    }
    if (ends_with_sv(trimmed, kFullwidthUpperH) ||
        ends_with_sv(trimmed, kFullwidthParenUpperH) ||
        ends_with_sv(trimmed, "(Ｈ)") || ends_with_sv(trimmed, "（H）") ||
        ends_with_sv(trimmed, "（h）")) {
        return true;
    }
    return ends_with_ascii_ci(trimmed, "(h)") ||
           ends_with_ascii_ci(trimmed, " h)");
}

bool is_h_scene_entry(std::string_view speaker,
                      std::string_view jp_body,
                      std::string_view en_body) noexcept {
    if (has_speaker_h_tag_suffix(speaker)) {
        return true;
    }
    for (const std::string_view jp_marker : kJapaneseExplicitMarkers) {
        if (jp_body.find(jp_marker) != std::string_view::npos) {
            return true;
        }
    }
    for (const std::string_view en_word : kEnglishExplicitWords) {
        if (contains_word_ascii_ci(en_body, en_word)) {
            return true;
        }
    }
    return false;
}

HSceneCueTier classify_h_scene_cue(std::string_view speaker,
                                   std::string_view jp_body,
                                   std::string_view en_body) noexcept {
    if (is_h_scene_entry(speaker, jp_body, en_body)) {
        return HSceneCueTier::ExplicitOrTagged;
    }
    for (const std::string_view marker : kJapaneseLiteraryErogeMarkers) {
        if (jp_body.find(marker) != std::string_view::npos) {
            return HSceneCueTier::ExplicitOrTagged;
        }
    }
    for (const std::string_view word : kEnglishLiteraryErogeWords) {
        if (contains_word_ascii_ci(en_body, word)) {
            return HSceneCueTier::ExplicitOrTagged;
        }
    }

    if (has_wet_sfx_or_heart_cue(jp_body, en_body)) {
        return HSceneCueTier::EroticVocalOrSfx;
    }

    if (has_combat_chase_or_horror_cue(jp_body, en_body)) {
        return HSceneCueTier::None;
    }

    for (const std::string_view marker : kJapaneseEroticVocalMarkers) {
        if (jp_body.find(marker) != std::string_view::npos) {
            return HSceneCueTier::EroticVocalOrSfx;
        }
    }
    for (const std::string_view word : kEnglishEroticVocalWords) {
        if (contains_word_ascii_ci(en_body, word)) {
            return HSceneCueTier::EroticVocalOrSfx;
        }
    }
    if (has_elongated_erotic_vocalization_en(en_body) ||
        has_repeated_moan_pair_en(en_body)) {
        return HSceneCueTier::EroticVocalOrSfx;
    }

    const JpVocalScan jp_vocal = scan_jp_vocalizations(jp_body);
    if (jp_vocal.has_nasal_or_repeated_moan) {
        return HSceneCueTier::EroticVocalOrSfx;
    }

    if (jp_vocal.has_bounded_breath_or_gasp ||
        jp_body.find("はぁ") != std::string_view::npos ||
        jp_body.find("くぅ") != std::string_view::npos ||
        jp_body.find("吐息") != std::string_view::npos ||
        jp_body.find("快感") != std::string_view::npos) {
        return HSceneCueTier::AmbiguousBreath;
    }
    for (const std::string_view word : kEnglishAmbiguousBreathWords) {
        if (contains_word_ascii_ci(en_body, word)) {
            return HSceneCueTier::AmbiguousBreath;
        }
    }

    return HSceneCueTier::None;
}

std::vector<std::pair<std::size_t, std::size_t>> detect_h_scene_spans(
    const std::vector<ScriptEntry>& entries) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    if (entries.size() < kMinSustainedHSceneLines) {
        return spans;
    }

    std::vector<HSceneCueTier> tiers(entries.size(), HSceneCueTier::None);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string jp_body = strip_inline_control_wrappers(
            strip_speaker_prefix_body(entries[i].jp_text));
        const std::string en_body =
            strip_speaker_prefix_body(entries[i].current_en);
        tiers[i] = classify_h_scene_cue(entries[i].speaker, jp_body, en_body);
    }

    auto maybe_emit_cluster = [&](std::size_t first_cue, std::size_t last_cue) {
        const std::size_t length = (last_cue + 1U) - first_cue;
        if (length < kMinSustainedHSceneLines) {
            return;
        }
        std::size_t active_count = 0;
        std::size_t anchor_count = 0;
        for (std::size_t k = first_cue; k <= last_cue; ++k) {
            if (tiers[k] != HSceneCueTier::None) {
                ++active_count;
            }
            if (tiers[k] == HSceneCueTier::EroticVocalOrSfx ||
                tiers[k] == HSceneCueTier::ExplicitOrTagged) {
                ++anchor_count;
            }
        }
        const double density =
            static_cast<double>(active_count) / static_cast<double>(length);
        if (active_count >= kMinSustainedHSceneActiveLines &&
            anchor_count >= kMinSustainedHSceneAnchors &&
            density >= kMinSustainedHSceneDensity) {
            spans.emplace_back(first_cue, length);
        }
    };

    std::size_t seg_begin = 0;
    while (seg_begin < entries.size()) {
        std::size_t seg_end = seg_begin + 1U;
        while (seg_end < entries.size() &&
               entries[seg_end].script_name == entries[seg_begin].script_name) {
            ++seg_end;
        }

        if (seg_end - seg_begin >= kMinSustainedHSceneLines) {
            bool in_cluster = false;
            std::size_t cluster_first = 0;
            std::size_t cluster_last = 0;

            for (std::size_t i = seg_begin; i < seg_end; ++i) {
                if (tiers[i] == HSceneCueTier::None) {
                    continue;
                }
                if (!in_cluster) {
                    in_cluster = true;
                    cluster_first = i;
                    cluster_last = i;
                    continue;
                }
                const std::size_t non_cue_gap = i - cluster_last - 1U;
                if (non_cue_gap <= kMaxHSceneBridgeGapLines) {
                    cluster_last = i;
                } else {
                    maybe_emit_cluster(cluster_first, cluster_last);
                    cluster_first = i;
                    cluster_last = i;
                }
            }
            if (in_cluster) {
                maybe_emit_cluster(cluster_first, cluster_last);
            }
        }

        seg_begin = seg_end;
    }

    return spans;
}

std::vector<bool> compute_h_scene_line_mask(
    const std::vector<ScriptEntry>& entries) {
    std::vector<bool> mask(entries.size(), false);
    const std::vector<std::pair<std::size_t, std::size_t>> spans =
        detect_h_scene_spans(entries);
    for (const auto& [offset, length] : spans) {
        const std::size_t end =
            offset + std::min(length, entries.size() - offset);
        for (std::size_t i = offset; i < end; ++i) {
            mask[i] = true;
        }
    }
    return mask;
}

bool contains_translator_romaji_note(std::string_view en_body) noexcept {
    for (std::size_t i = 0; i < en_body.size(); ++i) {
        const char open_ch = en_body[i];
        if (open_ch != '(' && open_ch != '[') {
            continue;
        }
        const char close_ch = (open_ch == '(') ? ')' : ']';
        const std::size_t close = en_body.find(close_ch, i + 1U);
        if (close == std::string_view::npos) {
            continue;
        }
        const std::string_view inner = en_body.substr(i + 1U, close - (i + 1U));
        if (inner.find(open_ch) != std::string_view::npos) {
            continue;
        }
        if (span_is_translator_or_romaji_note(inner)) {
            return true;
        }
    }
    return false;
}

std::string unescape_llm_newlines(std::string_view candidate,
                                  std::string_view reference_old_en) {
    if (reference_old_en.find(kLiteralNewlineEscape) != std::string_view::npos ||
        reference_old_en.find('\n') == std::string_view::npos) {
        return std::string(candidate);
    }
    const std::string_view newline_seq =
        (reference_old_en.find(kCrlfLiteral) != std::string_view::npos)
            ? kCrlfLiteral
            : "\n";
    return replace_escaped_newlines(candidate, newline_seq);
}

std::string normalize_restored_translation(const ScriptEntry& entry,
                                           std::string_view raw_candidate) {
    const SpeakerSplit old_split = split_speaker_prefix(entry.current_en);
    const std::string_view unescape_ref =
        (!old_split.prefix.empty() &&
         old_split.body.find(kLiteralNewlineEscape) == std::string_view::npos)
            ? std::string_view(entry.current_en)
            : std::string_view(old_split.body);

    std::string working = unescape_llm_newlines(raw_candidate, unescape_ref);
    working = strip_echoed_speaker_labels(
        working, entry.speaker, old_split.speaker_header, old_split.body);
    working = sanitize_ascii_punctuation(working);

    const CoreEnvelopeSlice cand_sliced = slice_envelope_escapes(working);
    if (cand_sliced.core.empty()) {
        return {};
    }

    const CoreEnvelopeSlice old_sliced = slice_envelope_escapes(old_split.body);
    std::string restored;
    restored.reserve(old_split.prefix.size() + old_sliced.leading_escape.size() +
                     cand_sliced.core.size() + old_sliced.trailing_escape.size());
    restored.append(old_split.prefix);
    restored.append(old_sliced.leading_escape);
    restored.append(cand_sliced.core);
    restored.append(old_sliced.trailing_escape);
    return restored;
}

}  // namespace spot_checker
