// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "detector/weakness_signals.h"

#include "common/utf8_text.h"
#include "domain/vn_line_syntax.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace spot_checker {

namespace {

constexpr char32_t kIdeographicSpaceCp = 0x3000U;
constexpr char32_t kBomCp = 0xFEFFU;

constexpr std::size_t kMinRunawayCharRepeat = 8;
constexpr std::size_t kMinWordsForSingleLineYule = 6;
constexpr std::size_t kMinContentWordsForYuleFormula = 2;
constexpr std::size_t kMinRepeatedWordOccurrences = 2;
constexpr std::size_t kMinMonotonyMaxFreq = 3;
constexpr std::size_t kMinSingleLineRepeatedTypes = 2;
constexpr std::size_t kMinWindowRepeatedTypes = 3;
constexpr std::size_t kMinCharsForBigramEntropy = 20;
constexpr std::size_t kMinKatakanaCallbackCodepoints = 3;
constexpr std::size_t kMinKanjiCompoundCodepoints = 2;
constexpr std::size_t kEstimatedBytesPerJpCp = 2;
constexpr std::size_t kBlondeLookbackTurns = 5;
constexpr std::size_t kBlondeCloseTurnDistance = 2;
constexpr std::size_t kSlidingYuleWindowTurns = 5;
constexpr std::size_t kMinSlidingYuleActiveLines = 3;
constexpr std::size_t kMinSlidingYuleWords = 10;
constexpr std::size_t kMinSpeakerHistoryForDrift = 6;

constexpr std::string_view kSuffixIng = "ing";
constexpr std::string_view kSuffixIed = "ied";
constexpr std::string_view kSuffixIes = "ies";
constexpr std::string_view kSuffixEd = "ed";
constexpr std::string_view kSuffixEs = "es";
constexpr std::string_view kPossessiveSuffix = "'s";

constexpr std::size_t kMinWordLenForIngStrip = 6;
constexpr std::size_t kMinStemLenForDoubledConsonant = 4;
constexpr std::size_t kPenultimateCharBackOffset = 2;
constexpr std::size_t kMinWordLenForMultiCharSuffix = 5;
constexpr std::size_t kMinWordLenForPluralSStrip = 4;
constexpr std::size_t kMinContentTokenBytes = 3;
constexpr std::size_t kMinNormalizedStemBytes = 2;

constexpr std::size_t kMinWordsForTrigramLoop = 9;
constexpr std::size_t kTrigramSecondOffset = 2;
constexpr std::size_t kTrigramSpaceSeparators = 2;
constexpr int kMinTrigramLoopCount = 3;

constexpr double kYulesKScaleFactor = 10000.0;
constexpr unsigned int kBitsPerByte = 8U;
constexpr std::size_t kUtf8CornerBracketBytes = 3;
constexpr std::size_t kUtf8CornerPairBytes = 2 * kUtf8CornerBracketBytes;
constexpr std::size_t kNyaDigraphCodepoints = 2;
constexpr std::size_t kPrevTwoCpOffset = 2;

constexpr double kOlsRssCrossTermFactor = 2.0;
constexpr std::size_t kOlsLinearDofLoss = 2;

constexpr double kMaxSingleLineYulePenalty = 5.0;
constexpr double kSingleLineYuleDivisor = 75.0;
constexpr double kSingleLineYuleBasePenalty = 1.5;
constexpr double kLowEntropyPenaltyScale = 2.5;

constexpr double kInitialSpeakerVarRatio = 0.25;
constexpr double kShortCallbackMaxJpBits = 24.0;
constexpr double kBlondeShortLineMaxJpBits = 20.0;
constexpr double kSlidingYuleThresholdMultiplier = 1.2;
constexpr double kMaxSlidingYulePenalty = 3.0;
constexpr double kSlidingYuleDivisor = 100.0;
constexpr double kSlidingYuleBasePenalty = 1.0;

constexpr double kMinSpeakerDriftJpBits = 8.0;
constexpr double kMinSpeakerStdRatioFloor = 0.45;
constexpr double kSpeakerRatioDriftSigmaMultiplier = 4.0;
constexpr double kPoliteContractionRateCeiling = 0.05;
constexpr double kCasualLineIndicatorThreshold = 0.5;
constexpr double kMultiTurnDominantSignalShare = 0.5;

constexpr double kEwmaAlpha = 0.15;
constexpr double kMinLengthZJpBits = 4.0;
constexpr double kMinExpectedEnBytes = 4.0;
constexpr double kWesternizedSlangPenalty = 3.0;
constexpr double kBowdlerizedPenalty = 3.0;
constexpr double kUnpromptedExplicitPenalty = 4.5;

constexpr std::array<std::string_view, 28> kJpPronounAndLightNounRoots = {
    "俺", "私", "僕", "君", "彼", "誰", "何", "今", "前", "後",
    "中", "上", "下", "目", "手", "顔", "声", "人", "事", "物",
    "時", "所", "方", "気", "日", "年", "一", "二",
};

constexpr std::array<std::string_view, 92> kEnglishStopwordsAndContractions = {
    "a",         "an",        "the",       "and",       "or",        "but",
    "if",        "in",        "on",        "at",        "to",        "for",
    "of",        "by",        "as",        "is",        "am",        "are",
    "was",       "were",      "be",        "been",      "being",     "do",
    "does",      "did",       "have",      "has",       "had",       "having",
    "will",      "would",     "shall",     "should",    "can",       "could",
    "may",       "might",     "must",      "not",       "no",        "nor",
    "so",        "too",       "very",      "just",      "only",      "also",
    "even",      "still",     "well",      "then",      "than",      "that",
    "this",      "these",     "those",     "what",      "which",     "who",
    "whom",      "whose",     "where",     "when",      "why",       "how",
    "with",      "from",      "into",      "onto",      "upon",      "over",
    "under",     "about",     "after",     "before",    "there",     "here",
    "i",         "me",        "my",        "mine",      "myself",    "you",
    "your",      "yours",     "yourself",  "he",        "him",       "his",
    "she",       "her",
};

constexpr std::array<std::string_view, 60> kMoreEnglishStopwords = {
    "hers",      "it",        "its",       "we",        "us",        "our",
    "ours",      "they",      "them",      "their",     "theirs",    "all",
    "any",       "some",      "such",      "each",      "both",      "much",
    "many",      "more",      "most",      "other",     "another",   "own",
    "same",      "out",       "off",       "up",        "down",      "back",
    "away",      "again",     "once",      "yeah",      "yes",       "yep",
    "nope",      "nah",       "oh",        "ah",        "uh",        "um",
    "hey",       "huh",       "well",      "okay",      "really",    "like",
    "that's",    "what's",    "it's",      "he's",      "she's",     "there's",
    "here's",    "where's",   "who's",     "how's",     "let's",     "i'm",
};

constexpr std::array<std::string_view, 28> kContractionStopwords = {
    "i've",      "i'll",      "i'd",       "you're",    "you've",    "you'll",
    "you'd",     "we're",     "we've",     "we'll",     "we'd",      "they're",
    "they've",   "they'll",   "they'd",    "isn't",     "aren't",    "wasn't",
    "weren't",   "don't",     "doesn't",   "didn't",    "won't",     "wouldn't",
    "can't",     "couldn't",  "shouldn't", "haven't",
};

constexpr std::array<std::string_view, 18> kExplicitFirstPersonJp = {
    "俺は",     "俺が",     "俺も",     "僕は",     "僕が",     "僕も",
    "私は",     "私が",     "私も",     "あたしは", "あたしが", "あたしも",
    "ウチは",   "ウチが",   "ワシは",   "ワシが",   "わたくしは", "わたくしが",
};

constexpr std::array<std::string_view, 10> kEnglishFirstPersonWords = {
    "i", "me", "my", "mine", "myself", "we", "us", "our", "ours", "ourselves",
};

constexpr std::array<std::string_view, 9> kCatTicEnglishWords = {
    "nya", "nyaa", "nyan", "meow", "mew", "purr", "purring", "kitty", "cat",
};

constexpr std::array<std::string_view, 12> kAnachronisticSlangWords = {
    "bruh",    "lmao",    "lmfao", "rofl", "yolo", "yeet",
    "skibidi", "rizz",    "deadass", "lowkey", "highkey", "finna",
};

bool is_whitespace_cp(char32_t cp) noexcept {
    return cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' ||
           cp == kIdeographicSpaceCp || cp == kBomCp;
}

bool is_ascii_alnum_cp(char32_t cp) noexcept {
    return (cp >= U'0' && cp <= U'9') || (cp >= U'A' && cp <= U'Z') ||
           (cp >= U'a' && cp <= U'z');
}

bool is_ascii_lower_or_digit(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool starts_with_sv(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with_sv(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_jp_light_root(std::string_view root) noexcept {
    for (const std::string_view stop : kJpPronounAndLightNounRoots) {
        if (root == stop) {
            return true;
        }
    }
    return false;
}

bool is_bare_case_or_topic_particle(char32_t cp) noexcept {
    return cp == U'は' || cp == U'が' || cp == U'を' || cp == U'に' ||
           cp == U'へ' || cp == U'と' || cp == U'で' || cp == U'の' ||
           cp == U'も' || cp == U'や' || cp == U'か' || cp == U'ね' ||
           cp == U'よ' || cp == U'な' || cp == U'ぞ' || cp == U'ぜ' ||
           cp == U'わ' || cp == U'さ';
}

bool is_english_stopword(std::string_view lower_tok) noexcept {
    for (const std::string_view w : kEnglishStopwordsAndContractions) {
        if (lower_tok == w) {
            return true;
        }
    }
    for (const std::string_view w : kMoreEnglishStopwords) {
        if (lower_tok == w) {
            return true;
        }
    }
    for (const std::string_view w : kContractionStopwords) {
        if (lower_tok == w) {
            return true;
        }
    }
    if (ends_with_sv(lower_tok, "n't") || ends_with_sv(lower_tok, "'re") ||
        ends_with_sv(lower_tok, "'ve") || ends_with_sv(lower_tok, "'ll") ||
        ends_with_sv(lower_tok, "'d")) {
        return true;
    }
    return false;
}

std::string normalize_irregular_or_stem(std::string_view word) {
    if (word == "went" || word == "gone" || word == "going" || word == "goes") {
        return "go";
    }
    if (word == "came" || word == "coming" || word == "comes") {
        return "come";
    }
    if (word == "ate" || word == "eaten" || word == "eating" || word == "eats") {
        return "eat";
    }
    if (word == "saw" || word == "seen" || word == "seeing" || word == "sees") {
        return "see";
    }
    if (word == "took" || word == "taken" || word == "taking" || word == "takes") {
        return "take";
    }
    if (word == "knew" || word == "known" || word == "knowing" || word == "knows") {
        return "know";
    }
    if (word == "left" || word == "leaving" || word == "leaves") {
        return "leav";
    }
    if (word == "thought" || word == "thinking" || word == "thinks") {
        return "think";
    }
    if (word == "told" || word == "telling" || word == "tells") {
        return "tell";
    }
    if (word == "bought" || word == "buying" || word == "buys") {
        return "buy";
    }

    std::string s(word);
    if (s.size() >= kMinWordLenForIngStrip && ends_with_sv(s, kSuffixIng)) {
        s.resize(s.size() - kSuffixIng.size());
        if (s.size() >= kMinStemLenForDoubledConsonant &&
            s.back() == s[s.size() - kPenultimateCharBackOffset]) {
            s.pop_back();
        }
        return s;
    }
    if (s.size() >= kMinWordLenForMultiCharSuffix &&
        ends_with_sv(s, kSuffixIed)) {
        s.resize(s.size() - kSuffixIed.size());
        s.push_back('y');
        return s;
    }
    if (s.size() >= kMinWordLenForMultiCharSuffix &&
        ends_with_sv(s, kSuffixIes)) {
        s.resize(s.size() - kSuffixIes.size());
        s.push_back('y');
        return s;
    }
    if (s.size() >= kMinWordLenForMultiCharSuffix &&
        ends_with_sv(s, kSuffixEd)) {
        s.resize(s.size() - kSuffixEd.size());
        return s;
    }
    if (s.size() >= kMinWordLenForMultiCharSuffix &&
        ends_with_sv(s, kSuffixEs)) {
        s.resize(s.size() - kSuffixEs.size());
        return s;
    }
    if (s.size() >= kMinWordLenForPluralSStrip && s.back() == 's' &&
        s[s.size() - kPenultimateCharBackOffset] != 's') {
        s.pop_back();
        return s;
    }
    return s;
}

void push_unique(std::vector<std::string>& vec, std::string_view item) {
    if (item.empty()) {
        return;
    }
    for (const std::string& existing : vec) {
        if (existing == item) {
            return;
        }
    }
    vec.emplace_back(item);
}

void extract_jp_features(std::string_view jp_body,
                         std::vector<std::string>& out_kanji_compounds,
                         std::vector<std::string>& out_callback_tokens) {
    struct DecodedCp {
        char32_t cp;
        std::size_t byte_start;
        std::size_t byte_end;
    };
    std::vector<DecodedCp> cps;
    cps.reserve(jp_body.size() / kEstimatedBytesPerJpCp + 1U);
    std::size_t idx = 0;
    while (idx < jp_body.size()) {
        const std::size_t start = idx;
        const char32_t cp = utf8_next(jp_body, idx);
        cps.push_back(DecodedCp{cp, start, idx});
    }

    std::size_t i = 0;
    while (i < cps.size()) {
        if (is_kanji(cps[i].cp)) {
            const std::size_t run_start = i;
            while (i < cps.size() && is_kanji(cps[i].cp)) {
                ++i;
            }
            const std::size_t kanji_len = i - run_start;
            const std::string_view kanji_span = jp_body.substr(
                cps[run_start].byte_start, cps[i - 1U].byte_end - cps[run_start].byte_start);
            if (kanji_len >= kMinKanjiCompoundCodepoints) {
                push_unique(out_kanji_compounds, kanji_span);
            }

            if (i < cps.size() && is_hiragana(cps[i].cp) &&
                !is_jp_light_root(kanji_span)) {
                const char32_t first_hira = cps[i].cp;
                const char32_t second_hira =
                    (i + 1U < cps.size() && is_hiragana(cps[i + 1U].cp))
                        ? cps[i + 1U].cp
                        : U'\0';
                const bool okurigana_verb_like =
                    !is_bare_case_or_topic_particle(first_hira) ||
                    (first_hira == U'か' &&
                     (second_hira == U'な' || second_hira == U'せ' ||
                      second_hira == U'れ')) ||
                    (first_hira == U'わ' &&
                     (second_hira == U'な' || second_hira == U'れ' ||
                      second_hira == U'せ')) ||
                    (first_hira == U'さ' &&
                     (second_hira == U'な' || second_hira == U'せ' ||
                      second_hira == U'れ'));
                if (okurigana_verb_like) {
                    push_unique(out_callback_tokens, kanji_span);
                }
            }
            continue;
        }

        if (is_lexical_katakana(cps[i].cp)) {
            const std::size_t run_start = i;
            while (i < cps.size() &&
                   (is_lexical_katakana(cps[i].cp) || cps[i].cp == U'ー' ||
                    cps[i].cp == U'ｰ')) {
                ++i;
            }
            if (i - run_start >= kMinKatakanaCallbackCodepoints) {
                const std::string_view kata_span = jp_body.substr(
                    cps[run_start].byte_start,
                    cps[i - 1U].byte_end - cps[run_start].byte_start);
                push_unique(out_callback_tokens, kata_span);
            }
            continue;
        }

        ++i;
    }
}

std::vector<std::string> extract_en_content_stems(std::string_view lower_en) {
    std::vector<std::string> stems;
    std::size_t i = 0;
    while (i < lower_en.size()) {
        while (i < lower_en.size() && !is_ascii_lower_or_digit(lower_en[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < lower_en.size()) {
            if (is_ascii_lower_or_digit(lower_en[i])) {
                ++i;
                continue;
            }
            if (lower_en[i] == '\'' && i + 1U < lower_en.size() &&
                is_ascii_lower_or_digit(lower_en[i + 1U])) {
                ++i;
                continue;
            }
            break;
        }
        if (i == start) {
            continue;
        }

        std::string_view tok = lower_en.substr(start, i - start);
        if (is_english_stopword(tok)) {
            continue;
        }
        if (ends_with_sv(tok, kPossessiveSuffix)) {
            tok = tok.substr(0, tok.size() - kPossessiveSuffix.size());
        }
        if (tok.size() < kMinContentTokenBytes || is_english_stopword(tok)) {
            continue;
        }
        if (tok.find('\'') != std::string_view::npos) {
            continue;
        }

        const std::string stem = normalize_irregular_or_stem(tok);
        if (stem.size() >= kMinNormalizedStemBytes &&
            !is_english_stopword(stem)) {
            push_unique(stems, stem);
        }
    }
    return stems;
}

bool has_runaway_char_repeat(std::string_view en_body) noexcept {
    std::size_t run = 0;
    char prev = '\0';
    for (char c : en_body) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            run = 0;
            prev = '\0';
            continue;
        }
        if (c == prev) {
            ++run;
            if (run >= kMinRunawayCharRepeat) {
                return true;
            }
        } else {
            prev = c;
            run = 1U;
        }
    }
    return false;
}

std::vector<std::string_view> tokenize_lower_words(std::string_view lower_en) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < lower_en.size()) {
        while (i < lower_en.size() && !is_ascii_lower_or_digit(lower_en[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < lower_en.size() &&
               (is_ascii_lower_or_digit(lower_en[i]) ||
                (lower_en[i] == '\'' && i + 1U < lower_en.size() &&
                 is_ascii_lower_or_digit(lower_en[i + 1U])))) {
            ++i;
        }
        if (i > start) {
            words.push_back(lower_en.substr(start, i - start));
        }
    }
    return words;
}

bool has_runaway_trigram_loop(
    const std::vector<std::string_view>& words) {
    if (words.size() < kMinWordsForTrigramLoop) {
        return false;
    }
    std::unordered_map<std::string, int> trigram_counts;
    trigram_counts.reserve(words.size());
    for (std::size_t i = 0; i + kTrigramSecondOffset < words.size(); ++i) {
        std::string key;
        key.reserve(words[i].size() + words[i + 1U].size() +
                    words[i + kTrigramSecondOffset].size() +
                    kTrigramSpaceSeparators);
        key.append(words[i]);
        key.push_back(' ');
        key.append(words[i + 1U]);
        key.push_back(' ');
        key.append(words[i + kTrigramSecondOffset]);
        if (++trigram_counts[key] >= kMinTrigramLoopCount) {
            return true;
        }
    }
    return false;
}

struct ContentWordStats {
    std::vector<std::string_view> words;
    std::size_t max_freq = 0;
    std::size_t repeated_types = 0;
    double yules_k = 0.0;
};

ContentWordStats analyze_content_yules_k(
    const std::vector<std::string_view>& lower_words) {
    ContentWordStats stats;
    stats.words.reserve(lower_words.size());
    for (const std::string_view w : lower_words) {
        if (!is_english_stopword(w)) {
            stats.words.push_back(w);
        }
    }
    if (stats.words.size() < kMinContentWordsForYuleFormula) {
        return stats;
    }

    std::unordered_map<std::string_view, std::size_t> freqs;
    freqs.reserve(stats.words.size());
    for (const std::string_view w : stats.words) {
        ++freqs[w];
    }

    double sum_f2 = 0.0;
    for (const auto& [word, f] : freqs) {
        static_cast<void>(word);
        if (f > stats.max_freq) {
            stats.max_freq = f;
        }
        if (f >= kMinRepeatedWordOccurrences) {
            ++stats.repeated_types;
        }
        const auto df = static_cast<double>(f);
        sum_f2 += df * df;
    }

    const auto n = static_cast<double>(stats.words.size());
    stats.yules_k = kYulesKScaleFactor * std::max(0.0, sum_f2 - n) / (n * n);
    return stats;
}

double compute_char_bigram_entropy(std::string_view lower_en) {
    std::string compact;
    compact.reserve(lower_en.size());
    for (char c : lower_en) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            compact.push_back(c);
        }
    }
    if (compact.size() < kMinCharsForBigramEntropy) {
        return kLowEntropyThreshold + 1.0;
    }

    std::unordered_map<std::uint16_t, std::size_t> bigram_counts;
    const std::size_t total = compact.size() - 1U;
    bigram_counts.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        const auto key = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(static_cast<unsigned char>(compact[i]))
             << kBitsPerByte) |
            static_cast<std::uint16_t>(
                static_cast<unsigned char>(compact[i + 1U])));
        ++bigram_counts[key];
    }

    const auto denom = static_cast<double>(total);
    double entropy = 0.0;
    for (const auto& [bigram, count] : bigram_counts) {
        static_cast<void>(bigram);
        const double p = static_cast<double>(count) / denom;
        if (p > 0.0) {
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

bool has_leaked_japanese_in_en_body(std::string_view en_body,
                                    const QuoteEnvelope& en_env) noexcept {
    if (has_real_japanese(en_body)) {
        return true;
    }
    if (en_env.outer_kind == OuterWrapKind::CornerQuoteSingle ||
        en_env.outer_kind == OuterWrapKind::CornerQuoteDouble) {
        const std::string_view trimmed = trim_ascii_view(en_body);
        if (trimmed.size() >= kUtf8CornerPairBytes) {
            const std::string_view inner = trimmed.substr(
                kUtf8CornerBracketBytes,
                trimmed.size() - kUtf8CornerPairBytes);
            return has_japanese(inner);
        }
    }
    return has_japanese(en_body);
}

bool contains_phrase_word_bounded(std::string_view lower_text,
                                  std::string_view lower_phrase) noexcept {
    if (lower_phrase.empty() || lower_phrase.size() > lower_text.size()) {
        return false;
    }
    std::size_t pos = 0;
    while ((pos = lower_text.find(lower_phrase, pos)) != std::string_view::npos) {
        const bool left_ok =
            pos == 0U || !is_ascii_lower_or_digit(lower_text[pos - 1U]);
        const std::size_t end = pos + lower_phrase.size();
        const bool right_ok =
            end == lower_text.size() || !is_ascii_lower_or_digit(lower_text[end]);
        if (left_ok && right_ok) {
            return true;
        }
        ++pos;
    }
    return false;
}

bool has_benefactive_inversion(std::string_view jp_body,
                               std::string_view en_body,
                               std::string_view lower_en) noexcept {
    if (jp_body.find("てあげ") != std::string_view::npos ||
        jp_body.find("であげ") != std::string_view::npos ||
        jp_body.find("てさしあげ") != std::string_view::npos ||
        jp_body.find("でさしあげ") != std::string_view::npos ||
        jp_body.find("てやる") != std::string_view::npos ||
        jp_body.find("てやっ") != std::string_view::npos ||
        jp_body.find("てやろ") != std::string_view::npos ||
        jp_body.find("でやる") != std::string_view::npos ||
        jp_body.find("でやっ") != std::string_view::npos ||
        jp_body.find("ため") != std::string_view::npos) {
        return false;
    }

    const bool has_kure = jp_body.find("てくれ") != std::string_view::npos ||
                          jp_body.find("でくれ") != std::string_view::npos ||
                          jp_body.find("てくださ") != std::string_view::npos ||
                          jp_body.find("でくださ") != std::string_view::npos;
    const bool has_morau = jp_body.find("てもら") != std::string_view::npos ||
                           jp_body.find("でもら") != std::string_view::npos ||
                           jp_body.find("ていただ") != std::string_view::npos ||
                           jp_body.find("でいただ") != std::string_view::npos;
    if (!has_kure && !has_morau) {
        return false;
    }

    constexpr std::array<std::string_view, 16> kExplicitGiverPhrases = {
        "let me help",     "let me teach",    "let me give",     "let me lend",
        "let me show",     "let me buy",      "let me explain",  "let me save",
        "i'll help you",   "i will help you", "i helped you",    "i saved you",
        "i taught you",    "i gave you",      "i did it for you", "for your sake, i",
    };
    for (const std::string_view phrase : kExplicitGiverPhrases) {
        if (contains_phrase_word_bounded(lower_en, phrase)) {
            return true;
        }
    }

    if (contains_phrase_word_bounded(lower_en, "for you") &&
        (contains_word_ascii_ci(en_body, "i") ||
         contains_phrase_word_bounded(lower_en, "let me"))) {
        return true;
    }

    if (en_body.find('?') == std::string_view::npos &&
        !contains_word_ascii_ci(en_body, "me") &&
        !contains_word_ascii_ci(en_body, "us") &&
        !contains_word_ascii_ci(en_body, "thank") &&
        !contains_word_ascii_ci(en_body, "thanks")) {
        constexpr std::array<std::string_view, 48> kFirstPersonActiveGivers = {
            "i taught",    "i teach",     "i'll teach",   "i will teach",
            "i gave",      "i give",      "i'll give",    "i will give",
            "i lent",      "i lend",      "i'll lend",    "i will lend",
            "i showed",    "i show",      "i'll show",    "i will show",
            "i helped",    "i help",      "i'll help",    "i will help",
            "i bought",    "i buy",       "i'll buy",     "i will buy",
            "i sent",      "i send",      "i'll send",    "i will send",
            "i told",      "i tell",      "i'll tell",    "i will tell",
            "i explained", "i explain",   "i'll explain", "i will explain",
            "i saved",     "i save",      "i'll save",    "i will save",
            "i rescued",   "i rescue",    "i'll rescue",  "i will rescue",
            "i protected", "i protect",   "i'll protect", "i will protect",
        };
        for (const std::string_view phrase : kFirstPersonActiveGivers) {
            if (contains_phrase_word_bounded(lower_en, phrase)) {
                return true;
            }
        }
    }

    const bool has_en_first_subject = contains_word_ascii_ci(en_body, "i");
    const bool has_en_you = contains_word_ascii_ci(en_body, "you");
    if (!has_en_first_subject || !has_en_you) {
        return false;
    }
    if (contains_word_ascii_ci(en_body, "me") ||
        contains_word_ascii_ci(en_body, "my") ||
        contains_word_ascii_ci(en_body, "us") ||
        contains_word_ascii_ci(en_body, "our") ||
        contains_word_ascii_ci(en_body, "thank") ||
        contains_word_ascii_ci(en_body, "thanks") ||
        contains_word_ascii_ci(en_body, "glad") ||
        contains_word_ascii_ci(en_body, "ask") ||
        contains_word_ascii_ci(en_body, "asked") ||
        contains_word_ascii_ci(en_body, "beg") ||
        contains_word_ascii_ci(en_body, "begged") ||
        (has_morau && (contains_word_ascii_ci(en_body, "had") ||
                       contains_word_ascii_ci(en_body, "got") ||
                       contains_word_ascii_ci(en_body, "want") ||
                       contains_word_ascii_ci(en_body, "wanted")))) {
        return false;
    }
    return en_body.find('?') == std::string_view::npos;
}

bool has_dropped_explicit_first_person(std::string_view jp_body,
                                       std::string_view en_body) noexcept {
    bool jp_has_explicit_1p = false;
    for (const std::string_view marker : kExplicitFirstPersonJp) {
        if (jp_body.find(marker) != std::string_view::npos) {
            jp_has_explicit_1p = true;
            break;
        }
    }
    if (!jp_has_explicit_1p) {
        return false;
    }
    for (const std::string_view pron : kEnglishFirstPersonWords) {
        if (contains_word_ascii_ci(en_body, pron)) {
            return false;
        }
    }
    return true;
}

bool is_sentence_boundary_cp(char32_t cp) noexcept {
    return cp == U'\0' || cp == U'。' || cp == U'！' || cp == U'？' ||
           cp == U'」' || cp == U'』' || cp == U'…' || cp == U'!' ||
           cp == U'?' || cp == U'♪' || cp == U'♡' || cp == U'〜' ||
           cp == U'～';
}

bool has_cat_yakuwarigo_jp(std::string_view jp_body) {
    std::vector<char32_t> cps;
    cps.reserve(jp_body.size() / kEstimatedBytesPerJpCp + 1U);
    std::size_t idx = 0;
    while (idx < jp_body.size()) {
        cps.push_back(utf8_next(jp_body, idx));
    }

    for (std::size_t i = 0; i + 1U < cps.size(); ++i) {
        if (cps[i] != U'に' || cps[i + 1U] != U'ゃ') {
            continue;
        }
        std::size_t tail = i + kNyaDigraphCodepoints;
        if (tail < cps.size() &&
            (cps[tail] == U'あ' || cps[tail] == U'ー' ||
             cps[tail] == U'ん' || cps[tail] == U'っ')) {
            ++tail;
        }
        const char32_t next_cp = (tail < cps.size()) ? cps[tail] : U'\0';
        if (!is_sentence_boundary_cp(next_cp)) {
            continue;
        }
        const char32_t prev1 = (i >= 1U) ? cps[i - 1U] : U'\0';
        const char32_t prev2 =
            (i >= kPrevTwoCpOffset) ? cps[i - kPrevTwoCpOffset] : U'\0';
        if (prev1 == U'ぐ' || prev1 == U'ふ' || prev1 == U'む' ||
            prev1 == U'く' || prev1 == U'ぷ') {
            continue;
        }
        if (prev1 == U'俺' || prev1 == U'私' || prev1 == U'僕' ||
            prev1 == U'君' || prev1 == U'中' || prev1 == U'人' ||
            (prev2 == U'こ' && prev1 == U'こ') ||
            (prev2 == U'そ' && prev1 == U'こ') ||
            (prev2 == U'ど' && prev1 == U'こ')) {
            continue;
        }
        if (is_kanji(prev2) &&
            (prev1 == U'か' || prev1 == U'わ' || prev1 == U'ら' ||
             prev1 == U'な' || prev1 == U'ま' || prev1 == U'た' ||
             prev1 == U'さ' || prev1 == U'ば' || prev1 == U'が')) {
            continue;
        }
        if (prev1 == U'せ' || (prev2 == U'し' && prev1 == U'な')) {
            continue;
        }
        return true;
    }
    return false;
}

bool has_ojou_english_cue(std::string_view en_body,
                          std::string_view lower_en) noexcept {
    constexpr std::array<std::string_view, 7> kOjouSubstrings = {
        "my,", "oh,", "oh my", "dear me", "do declare", "shan't", "shant",
    };
    for (const std::string_view sub : kOjouSubstrings) {
        if (lower_en.find(sub) != std::string_view::npos) {
            return true;
        }
    }
    constexpr std::array<std::string_view, 15> kOjouWords = {
        "indeed",     "quite",     "shall",   "goodness", "heavens",
        "gracious",   "delightful", "exquisite", "divine", "darling",
        "pardon",     "fufu",      "ohoho",   "hohoho",   "pray",
    };
    for (const std::string_view word : kOjouWords) {
        if (contains_word_ascii_ci(en_body, word)) {
            return true;
        }
    }
    return false;
}

bool has_samurai_english_cue(std::string_view en_body,
                             std::string_view lower_en) noexcept {
    constexpr std::array<std::string_view, 5> kSamuraiPhrases = {
        "this one", "this humble", "my lord", "my lady", "at your service",
    };
    for (const std::string_view phrase : kSamuraiPhrases) {
        if (contains_phrase_word_bounded(lower_en, phrase)) {
            return true;
        }
    }
    constexpr std::array<std::string_view, 18> kSamuraiWords = {
        "humbly",  "humble",   "verily",  "indeed",  "doth",    "dost",
        "hath",    "thy",      "thou",    "thee",    "thine",   "shall",
        "milord",  "milady",   "forsooth", "prithee", "pray",   "aye",
    };
    for (const std::string_view word : kSamuraiWords) {
        if (contains_word_ascii_ci(en_body, word)) {
            return true;
        }
    }
    return false;
}

bool has_yakuwarigo_loss(std::string_view jp_body,
                         std::string_view en_body,
                         std::string_view lower_en) {
    if (has_cat_yakuwarigo_jp(jp_body)) {
        for (const std::string_view cat_word : kCatTicEnglishWords) {
            if (contains_word_ascii_ci(en_body, cat_word)) {
                return false;
            }
        }
        return true;
    }

    const bool has_samurai =
        jp_body.find("でござる") != std::string_view::npos ||
        jp_body.find("ござるよ") != std::string_view::npos ||
        jp_body.find("ござるか") != std::string_view::npos ||
        jp_body.find("ござるな") != std::string_view::npos ||
        jp_body.find("ござらん") != std::string_view::npos ||
        jp_body.find("拙者") != std::string_view::npos ||
        jp_body.find("それがし") != std::string_view::npos ||
        jp_body.find("なのじゃ") != std::string_view::npos;
    const bool has_ojou =
        jp_body.find("ですわ") != std::string_view::npos ||
        jp_body.find("ますわ") != std::string_view::npos;
    if (!has_samurai && !has_ojou) {
        return false;
    }

    constexpr std::array<std::string_view, 10> kCasualSlang = {
        "yeah", "yep", "nope", "nah", "gonna", "wanna", "gotta", "ain't", "dude", "bro",
    };
    for (const std::string_view slang : kCasualSlang) {
        if (contains_word_ascii_ci(en_body, slang)) {
            return true;
        }
    }

    if (has_ojou && !has_ojou_english_cue(en_body, lower_en)) {
        return true;
    }
    if (has_samurai && !has_samurai_english_cue(en_body, lower_en)) {
        return true;
    }

    return false;
}

bool has_westernized_slang(std::string_view en_body,
                           std::string_view lower_en) noexcept {
    for (const std::string_view slang : kAnachronisticSlangWords) {
        if (contains_word_ascii_ci(en_body, slang)) {
            return true;
        }
    }
    return lower_en.find("no cap") != std::string_view::npos;
}

bool has_bowdlerization(std::string_view jp_body,
                        std::string_view en_body) noexcept {
    const bool jp_harsh = jp_body.find("クソ") != std::string_view::npos ||
                          jp_body.find("くそっ") != std::string_view::npos ||
                          jp_body.find("死ね") != std::string_view::npos ||
                          jp_body.find("ぶっ殺") != std::string_view::npos ||
                          jp_body.find("殺してやる") != std::string_view::npos ||
                          jp_body.find("ふざけんな") != std::string_view::npos ||
                          jp_body.find("てめぇ") != std::string_view::npos;
    if (!jp_harsh) {
        return false;
    }
    constexpr std::array<std::string_view, 7> kMincedOaths = {
        "darn", "heck", "fudge", "frick", "gee", "gosh", "meanie",
    };
    for (const std::string_view oath : kMincedOaths) {
        if (contains_word_ascii_ci(en_body, oath)) {
            return true;
        }
    }
    return false;
}

bool is_jp_question_line(std::string_view jp_body) noexcept {
    if (jp_body.find("？") != std::string_view::npos ||
        jp_body.find('?') != std::string_view::npos) {
        return true;
    }
    return ends_with_sv(jp_body, "か") || ends_with_sv(jp_body, "か。") ||
           ends_with_sv(jp_body, "か」") || ends_with_sv(jp_body, "かな」") ||
           ends_with_sv(jp_body, "かしら」") || ends_with_sv(jp_body, "かい」");
}

bool shares_any_string(const std::vector<std::string>& a,
                       const std::vector<std::string>& b) noexcept {
    for (const std::string& x : a) {
        for (const std::string& y : b) {
            if (x == y) {
                return true;
            }
        }
    }
    return false;
}

bool has_casual_contraction(std::string_view lower_en) noexcept {
    return lower_en.find("n't") != std::string_view::npos ||
           lower_en.find("i'm") != std::string_view::npos ||
           lower_en.find("you're") != std::string_view::npos ||
           lower_en.find("gonna") != std::string_view::npos ||
           lower_en.find("wanna") != std::string_view::npos ||
           lower_en.find("yeah") != std::string_view::npos;
}

constexpr double kMinOlsSampleJpBits = 1.0;

constexpr std::array<std::string_view, 16> kJapaneseCrudenessPromptMarkers = {
    "エロ",     "エッチ", "セックス", "オナニー", "自慰",   "ザーメン",
    "金玉",     "玉袋",   "下ネタ",   "変態",     "淫",     "童貞",
    "処女",     "ヤリマン", "やりチン", "ヤる",
};

bool has_jp_sexual_or_vulgar_prompt(std::string_view speaker,
                                    std::string_view jp_body) noexcept {
    if (classify_h_scene_cue(speaker, jp_body, {}) ==
        HSceneCueTier::ExplicitOrTagged) {
        return true;
    }
    for (const std::string_view marker : kJapaneseCrudenessPromptMarkers) {
        if (jp_body.find(marker) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

double compute_script_info_bits(std::string_view jp_body) noexcept {
    double bits = 0.0;
    std::size_t index = 0;
    while (index < jp_body.size()) {
        const char32_t cp = utf8_next(jp_body, index);
        if (cp == U'\0' || is_whitespace_cp(cp)) {
            continue;
        }
        if (is_kanji(cp)) {
            bits += kKanjiInfoBits;
            continue;
        }
        if (is_lexical_katakana(cp)) {
            bits += kKatakanaInfoBits;
            continue;
        }
        if (is_hiragana(cp)) {
            bits += kHiraganaInfoBits;
            continue;
        }
        if (is_ascii_alnum_cp(cp)) {
            bits += kAsciiAlnumInfoBits;
            continue;
        }
        bits += kOtherCharInfoBits;
    }
    return bits;
}

PreparedLine prepare_line(const ScriptEntry& entry) {
    PreparedLine prep;
    prep.jp_body =
        strip_inline_control_wrappers(strip_speaker_prefix_body(entry.jp_text));
    prep.en_body = strip_speaker_prefix_body(entry.current_en);
    prep.lower_en_body = to_lower_ascii(prep.en_body);
    prep.jp_info_bits = compute_script_info_bits(prep.jp_body);
    prep.is_passthrough_or_silence =
        is_engine_asset_or_command_token(entry.jp_text, entry.current_en) ||
        is_pure_silence_or_punctuation(prep.jp_body) ||
        entry.current_en == entry.jp_text ||
        (!prep.jp_body.empty() && prep.en_body == prep.jp_body);
    prep.is_h_scene =
        is_h_scene_entry(entry.speaker, prep.jp_body, prep.en_body);
    if (!prep.is_passthrough_or_silence) {
        extract_jp_features(
            prep.jp_body, prep.kanji_compounds, prep.jp_callback_tokens);
        prep.en_content_stems = extract_en_content_stems(prep.lower_en_body);
    }
    return prep;
}

OlsModel fit_length_regression(const std::vector<ScriptEntry>& entries,
                               bool skip_h_scenes) {
    const std::vector<bool> h_mask =
        skip_h_scenes ? compute_h_scene_line_mask(entries)
                      : std::vector<bool>{};

    std::size_t n = 0;
    double mean_x = 0.0;
    double mean_y = 0.0;
    double m2_x = 0.0;
    double m2_y = 0.0;
    double c_xy = 0.0;

    for (std::size_t idx = 0; idx < entries.size(); ++idx) {
        if (skip_h_scenes && idx < h_mask.size() && h_mask[idx]) {
            continue;
        }
        const ScriptEntry& entry = entries[idx];
        if (is_engine_asset_or_command_token(entry.jp_text, entry.current_en) ||
            entry.current_en == entry.jp_text) {
            continue;
        }
        const std::string jp_body =
            strip_inline_control_wrappers(strip_speaker_prefix_body(entry.jp_text));
        const std::string en_body = strip_speaker_prefix_body(entry.current_en);
        if (en_body == jp_body || trim_ascii_view(en_body).empty() ||
            en_body.size() > kMaxOlsCleanEnBytes ||
            is_pure_silence_or_punctuation(jp_body)) {
            continue;
        }
        const double x = compute_script_info_bits(jp_body);
        if (x < kMinOlsSampleJpBits) {
            continue;
        }
        const auto y = static_cast<double>(en_body.size());

        ++n;
        const auto dn = static_cast<double>(n);
        const double dx = x - mean_x;
        mean_x += dx / dn;
        const double dx2 = x - mean_x;
        m2_x += dx * dx2;

        const double dy = y - mean_y;
        mean_y += dy / dn;
        const double dy2 = y - mean_y;
        m2_y += dy * dy2;

        c_xy += dx * dy2;
    }

    OlsModel model;
    model.sample_count = n;
    if (n < kMinOlsSampleCount ||
        (m2_x / static_cast<double>(n)) <= kMinOlsVarianceX) {
        return model;
    }

    const double beta = std::clamp(c_xy / m2_x, kOlsBetaMin, kOlsBetaMax);
    const double alpha = mean_y - beta * mean_x;
    const double rss = std::max(
        0.0, m2_y - kOlsRssCrossTermFactor * beta * c_xy + beta * beta * m2_x);
    const double sigma = std::clamp(
        std::sqrt(rss / static_cast<double>(n - kOlsLinearDofLoss)),
        kOlsSigmaMin,
        kOlsSigmaMax);

    model.alpha = alpha;
    model.beta = beta;
    model.sigma = sigma;
    model.valid = true;
    return model;
}

LineWeaknessScore score_single_line(const ScriptEntry& entry,
                                    const OlsModel& ols_model,
                                    bool apply_h_skip,
                                    bool fix_punctuation) {
    LineWeaknessScore score;
    score.global_index = entry.global_index;

    const std::string jp_body =
        strip_inline_control_wrappers(strip_speaker_prefix_body(entry.jp_text));
    const std::string en_body = strip_speaker_prefix_body(entry.current_en);

    if (apply_h_skip && is_h_scene_entry(entry.speaker, jp_body, en_body)) {
        return score;
    }
    if (is_engine_asset_or_command_token(entry.jp_text, entry.current_en) ||
        is_pure_silence_or_punctuation(jp_body) ||
        entry.current_en == entry.jp_text ||
        (!jp_body.empty() && en_body == jp_body)) {
        return score;
    }

    double top_weight = 0.0;
    auto record_dominant = [&](double weight, std::string_view name) {
        if (weight > top_weight) {
            top_weight = weight;
            score.dominant_signal = std::string(name);
        }
    };

    const double jp_bits = compute_script_info_bits(jp_body);
    const double safe_sigma = std::max(ols_model.sigma, kOlsSigmaMin);
    const double expected_en =
        std::max(kMinExpectedEnBytes, ols_model.alpha + ols_model.beta * jp_bits);
    const auto en_len = static_cast<double>(en_body.size());

    if (jp_bits >= kMinLengthZJpBits) {
        score.length_z = std::clamp(
            (en_len - expected_en) / safe_sigma, kLengthZMin, kLengthZMax);
        const double abs_z = std::abs(score.length_z);
        if (abs_z > kLengthZDeadband) {
            const double len_pen = abs_z - kLengthZDeadband;
            score.total_score += len_pen;
            record_dominant(
                len_pen,
                (score.length_z < 0.0) ? "under_length_truncation"
                                       : "over_length_bloat");
        }
    }

    if (jp_bits >= kDroppedClauseMinJpBits) {
        const double min_viable_en =
            std::max(expected_en, jp_bits) * kDroppedClauseRatio;
        if (en_len < min_viable_en) {
            score.length_z = std::min(score.length_z, -kDroppedClausePenaltyZ);
            score.total_score += kDroppedClausePenaltyZ;
            record_dominant(kDroppedClausePenaltyZ, "dropped_clause");
        }
    }

    const std::string lower_en = to_lower_ascii(en_body);
    const std::vector<std::string_view> lower_words =
        tokenize_lower_words(lower_en);

    if (has_runaway_char_repeat(en_body)) {
        score.repetition_score += kRunawayCharRepeatPenalty;
        record_dominant(kRunawayCharRepeatPenalty, "runaway_repetition");
    }
    if (has_runaway_trigram_loop(lower_words)) {
        score.repetition_score += kRunawayTrigramLoopPenalty;
        record_dominant(kRunawayTrigramLoopPenalty, "runaway_repetition");
    }
    const ContentWordStats yule_stats = analyze_content_yules_k(lower_words);
    if (yule_stats.words.size() >= kMinWordsForSingleLineYule &&
        (yule_stats.max_freq >= kMinMonotonyMaxFreq ||
         yule_stats.repeated_types >= kMinSingleLineRepeatedTypes) &&
        yule_stats.yules_k > kYulesKThreshold) {
        const double yule_pen = std::min(
            kMaxSingleLineYulePenalty,
            (yule_stats.yules_k - kYulesKThreshold) / kSingleLineYuleDivisor +
                kSingleLineYuleBasePenalty);
        score.repetition_score += yule_pen;
        record_dominant(yule_pen, "lexical_monotony");
    }

    const double entropy = compute_char_bigram_entropy(lower_en);
    if (entropy < kLowEntropyThreshold) {
        const double ent_pen =
            (kLowEntropyThreshold - entropy) * kLowEntropyPenaltyScale;
        score.entropy_score += ent_pen;
        record_dominant(ent_pen, "low_entropy");
    }
    score.total_score += score.repetition_score + score.entropy_score;

    const QuoteEnvelope en_env = analyze_quote_envelope(en_body);
    if (has_leaked_japanese_in_en_body(en_body, en_env)) {
        score.total_score += kLeakedJpInEnPenalty;
        record_dominant(kLeakedJpInEnPenalty, "leaked_japanese");
    }

    if (has_benefactive_inversion(jp_body, en_body, lower_en)) {
        score.discourse_score += kBenefactiveInversionPenalty;
        record_dominant(kBenefactiveInversionPenalty, "benefactive_inversion");
    }
    if (has_dropped_explicit_first_person(jp_body, en_body)) {
        score.discourse_score += kDroppedFirstPersonPenalty;
        record_dominant(kDroppedFirstPersonPenalty, "dropped_first_person");
    }
    if (has_yakuwarigo_loss(jp_body, en_body, lower_en)) {
        score.discourse_score += kYakuwarigoLossPenalty;
        record_dominant(kYakuwarigoLossPenalty, "yakuwarigo_loss");
    }
    if (has_westernized_slang(en_body, lower_en)) {
        score.discourse_score += kWesternizedSlangPenalty;
        record_dominant(kWesternizedSlangPenalty, "westernized_slang");
    }
    if (has_bowdlerization(jp_body, en_body)) {
        score.discourse_score += kBowdlerizedPenalty;
        record_dominant(kBowdlerizedPenalty, "bowdlerized");
    }
    if (!has_jp_sexual_or_vulgar_prompt(entry.speaker, jp_body) &&
        is_h_scene_entry({}, {}, en_body)) {
        score.discourse_score += kUnpromptedExplicitPenalty;
        record_dominant(kUnpromptedExplicitPenalty,
                        "unprompted_explicit_lexicon");
    }
    score.total_score += score.discourse_score;

    if (fix_punctuation) {
        if (contains_translator_romaji_note(en_body)) {
            score.total_score += kRomajiGlossPenalty;
            record_dominant(kRomajiGlossPenalty, "romaji_or_tl_note");
        }
        const QuoteEnvelope jp_env = analyze_quote_envelope(jp_body);
        const bool dropped_dialogue_quotes =
            (jp_env.outer_kind == OuterWrapKind::CornerQuoteSingle ||
             jp_env.outer_kind == OuterWrapKind::CornerQuoteDouble) &&
            en_env.outer_kind == OuterWrapKind::None;
        const bool invented_outer_quotes =
            jp_env.outer_kind == OuterWrapKind::None &&
            jp_body.find("「") == std::string::npos &&
            jp_body.find("『") == std::string::npos &&
            en_env.outer_kind == OuterWrapKind::AsciiDoubleQuote;
        const bool unbalanced_quotes =
            jp_env.outer_kind != OuterWrapKind::None &&
            !en_env.balanced_ascii_quotes();
        const bool leaked_cjk_punct =
            en_env.outer_kind != OuterWrapKind::CornerQuoteSingle &&
            en_env.outer_kind != OuterWrapKind::CornerQuoteDouble &&
            has_cjk_or_fullwidth_punctuation(en_body);
        if (dropped_dialogue_quotes || invented_outer_quotes ||
            unbalanced_quotes || leaked_cjk_punct) {
            score.total_score += kQuoteFramingMismatchPenalty;
            record_dominant(kQuoteFramingMismatchPenalty,
                            "quote_or_punct_mismatch");
        }
    }

    return score;
}

std::vector<LineWeaknessScore> score_corpus_lines(
    const std::vector<ScriptEntry>& entries,
    const OlsModel& ols_model,
    const HotspotConfig& cfg) {
    std::vector<LineWeaknessScore> scores;
    scores.reserve(entries.size());
    std::vector<PreparedLine> preps;
    preps.reserve(entries.size());

    const std::vector<bool> h_mask =
        cfg.skip_h_scenes ? compute_h_scene_line_mask(entries)
                          : std::vector<bool>(entries.size(), false);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        PreparedLine prep = prepare_line(entries[i]);
        prep.is_h_scene = h_mask[i];
        preps.push_back(std::move(prep));

        if (cfg.skip_h_scenes && h_mask[i]) {
            LineWeaknessScore clean_score;
            clean_score.global_index = entries[i].global_index;
            scores.push_back(std::move(clean_score));
        } else {
            scores.push_back(score_single_line(
                entries[i], ols_model, false, cfg.fix_punctuation));
        }
    }

    struct SpeakerEwma {
        std::size_t count = 0;
        double mean_ratio = 0.0;
        double var_ratio = kInitialSpeakerVarRatio;
        double contraction_rate = 0.0;
    };
    std::unordered_map<std::string, SpeakerEwma> speaker_states;

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const PreparedLine& cur = preps[i];
        if (cur.is_passthrough_or_silence ||
            (cfg.skip_h_scenes && cur.is_h_scene)) {
            continue;
        }

        double added_discourse = 0.0;
        std::string_view added_label;
        double added_label_weight = 0.0;
        auto note_multi_signal = [&](double w, std::string_view label) {
            if (w > added_label_weight) {
                added_label_weight = w;
                added_label = label;
            }
        };

        bool fired_adjacent_qa = false;
        if (i > 0U && entries[i].script_name == entries[i - 1U].script_name) {
            const PreparedLine& prev = preps[i - 1U];
            if (!prev.is_passthrough_or_silence &&
                !(cfg.skip_h_scenes && prev.is_h_scene)) {
                const bool shared_jp_cb =
                    shares_any_string(prev.jp_callback_tokens, cur.jp_callback_tokens) ||
                    shares_any_string(prev.kanji_compounds, cur.kanji_compounds);
                if (shared_jp_cb && !prev.en_content_stems.empty() &&
                    !cur.en_content_stems.empty() &&
                    !shares_any_string(prev.en_content_stems, cur.en_content_stems)) {
                    if (is_jp_question_line(prev.jp_body)) {
                        double pen = kBrokenQaEchoPenalty;
                        if (cur.jp_info_bits <= kShortCallbackMaxJpBits) {
                            pen += kBrokenCallbackShortLineBonus;
                        }
                        added_discourse += pen;
                        note_multi_signal(pen, "broken_qa_callback");
                        fired_adjacent_qa = true;
                    } else if (cur.jp_info_bits <= kShortCallbackMaxJpBits &&
                               shares_any_string(prev.jp_callback_tokens,
                                                 cur.jp_callback_tokens)) {
                        added_discourse += kBrokenCallbackShortLineBonus;
                        note_multi_signal(kBrokenCallbackShortLineBonus,
                                          "broken_short_callback");
                        fired_adjacent_qa = true;
                    }
                }
            }
        }

        if (!fired_adjacent_qa && !cur.kanji_compounds.empty() &&
            !cur.en_content_stems.empty()) {
            const std::size_t min_j =
                (i > kBlondeLookbackTurns) ? (i - kBlondeLookbackTurns) : 0U;
            for (const std::string& compound : cur.kanji_compounds) {
                bool appeared_in_window = false;
                bool any_prior_shared_en = false;
                bool has_non_empty_prior_en = false;
                std::size_t closest_dist = kBlondeLookbackTurns + 1U;

                for (std::size_t j = min_j; j < i; ++j) {
                    if (entries[j].script_name != entries[i].script_name) {
                        continue;
                    }
                    const PreparedLine& prior = preps[j];
                    if (prior.is_passthrough_or_silence ||
                        (cfg.skip_h_scenes && prior.is_h_scene)) {
                        continue;
                    }
                    bool prior_has_compound = false;
                    for (const std::string& pc : prior.kanji_compounds) {
                        if (pc == compound) {
                            prior_has_compound = true;
                            break;
                        }
                    }
                    if (!prior_has_compound) {
                        continue;
                    }
                    appeared_in_window = true;
                    closest_dist = std::min(closest_dist, i - j);
                    if (!prior.en_content_stems.empty()) {
                        has_non_empty_prior_en = true;
                        if (shares_any_string(prior.en_content_stems,
                                              cur.en_content_stems)) {
                            any_prior_shared_en = true;
                            break;
                        }
                    }
                }

                if (appeared_in_window && has_non_empty_prior_en &&
                    !any_prior_shared_en) {
                    double pen = kBrokenLexicalCohesionPenalty;
                    if (closest_dist <= kBlondeCloseTurnDistance &&
                        cur.jp_info_bits <= kBlondeShortLineMaxJpBits) {
                        pen += kBrokenCallbackShortLineBonus;
                    }
                    added_discourse += pen;
                    note_multi_signal(pen, "broken_lexical_cohesion");
                    break;
                }
            }
        }

        if (scores[i].repetition_score == 0.0) {
            const std::size_t w_start =
                (i + 1U > kSlidingYuleWindowTurns)
                    ? (i + 1U - kSlidingYuleWindowTurns)
                    : 0U;
            std::vector<std::string_view> win_words;
            std::size_t active_lines = 0;
            for (std::size_t j = w_start; j <= i; ++j) {
                if (entries[j].script_name != entries[i].script_name ||
                    preps[j].is_passthrough_or_silence ||
                    (cfg.skip_h_scenes && preps[j].is_h_scene)) {
                    continue;
                }
                ++active_lines;
                const std::vector<std::string_view> lw =
                    tokenize_lower_words(preps[j].lower_en_body);
                win_words.insert(win_words.end(), lw.begin(), lw.end());
            }
            if (active_lines >= kMinSlidingYuleActiveLines) {
                const ContentWordStats win_yule =
                    analyze_content_yules_k(win_words);
                const double win_yule_threshold =
                    kYulesKThreshold * kSlidingYuleThresholdMultiplier;
                if (win_yule.words.size() >= kMinSlidingYuleWords &&
                    (win_yule.max_freq >= kMinMonotonyMaxFreq ||
                     win_yule.repeated_types >= kMinWindowRepeatedTypes) &&
                    win_yule.yules_k > win_yule_threshold) {
                    const double yule_pen = std::min(
                        kMaxSlidingYulePenalty,
                        (win_yule.yules_k - win_yule_threshold) /
                                kSlidingYuleDivisor +
                            kSlidingYuleBasePenalty);
                    scores[i].repetition_score += yule_pen;
                    scores[i].total_score += yule_pen;
                    note_multi_signal(yule_pen, "window_lexical_monotony");
                }
            }
        }

        if (!entries[i].speaker.empty() && entries[i].speaker != "NARRATION" &&
            cur.jp_info_bits >= kMinSpeakerDriftJpBits && !cur.en_body.empty()) {
            SpeakerEwma& st = speaker_states[entries[i].speaker];
            const double ratio =
                static_cast<double>(cur.en_body.size()) / cur.jp_info_bits;
            const double casual =
                has_casual_contraction(cur.lower_en_body) ? 1.0 : 0.0;
            const bool jp_polite =
                cur.jp_body.find("です") != std::string_view::npos ||
                cur.jp_body.find("ます") != std::string_view::npos;

            if (st.count >= kMinSpeakerHistoryForDrift) {
                const double std_ratio =
                    std::max(kMinSpeakerStdRatioFloor, std::sqrt(st.var_ratio));
                const bool ratio_drift =
                    std::abs(ratio - st.mean_ratio) >
                    kSpeakerRatioDriftSigmaMultiplier * std_ratio;
                const bool polite_register_break =
                    jp_polite &&
                    st.contraction_rate < kPoliteContractionRateCeiling &&
                    casual > kCasualLineIndicatorThreshold;
                if (ratio_drift || polite_register_break) {
                    added_discourse += kSpeakerRegisterDriftPenalty;
                    note_multi_signal(kSpeakerRegisterDriftPenalty,
                                      "speaker_register_drift");
                }
            }

            if (st.count == 0U) {
                st.mean_ratio = ratio;
                st.contraction_rate = casual;
            } else {
                const double diff = ratio - st.mean_ratio;
                st.mean_ratio += kEwmaAlpha * diff;
                st.var_ratio =
                    (1.0 - kEwmaAlpha) * (st.var_ratio + kEwmaAlpha * diff * diff);
                st.contraction_rate =
                    (1.0 - kEwmaAlpha) * st.contraction_rate + kEwmaAlpha * casual;
            }
            ++st.count;
        }

        if (added_discourse > 0.0) {
            scores[i].discourse_score += added_discourse;
            scores[i].total_score += added_discourse;
        }
        if (!added_label.empty() &&
            (scores[i].dominant_signal == "clean" ||
             added_label_weight >=
                 scores[i].total_score * kMultiTurnDominantSignalShare)) {
            scores[i].dominant_signal = std::string(added_label);
        }
    }

    return scores;
}

}  // namespace spot_checker
