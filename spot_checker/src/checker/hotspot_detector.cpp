// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/hotspot_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "checker/spot_checker.h"
#include "common/util.h"

namespace spot_checker {

namespace {

bool is_kanji(char32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF);
}

bool is_katakana(char32_t cp) {
    return cp >= 0x30A0 && cp <= 0x30FF;
}

bool is_hiragana(char32_t cp) {
    return cp >= 0x3040 && cp <= 0x309F;
}

std::string strip_speaker_prefix_body(const std::string& s) {
    const std::size_t crlf = s.find("\r\n");
    if (crlf != std::string::npos && crlf <= 35) {
        const std::string prefix = s.substr(0, crlf);
        if (!prefix.empty() && prefix.front() != '"' && prefix.front() != '\'' &&
            prefix.find("「") == std::string::npos &&
            prefix.find("『") == std::string::npos) {
            return trim(s.substr(crlf + 2));
        }
    }
    return trim(s);
}

std::string to_lower_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool contains_ci(const std::string& haystack_lower, const char* needle_lower) {
    return haystack_lower.find(needle_lower) != std::string::npos;
}

bool contains_word_ci(const std::string& lower_text, const std::string& word) {
    std::size_t pos = 0;
    while ((pos = lower_text.find(word, pos)) != std::string::npos) {
        const bool left_ok =
            (pos == 0) || !(lower_text[pos - 1] >= 'a' && lower_text[pos - 1] <= 'z');
        const std::size_t end = pos + word.size();
        const bool right_ok =
            (end >= lower_text.size()) ||
            !(lower_text[end] >= 'a' && lower_text[end] <= 'z');
        if (left_ok && right_ok) return true;
        pos = end;
    }
    return false;
}

// Extract 2+ consecutive kanji compounds from a Japanese body string.
std::vector<std::string> extract_kanji_stems(const std::string& jp_body) {
    std::vector<std::string> stems;
    std::size_t i = 0;
    while (i < jp_body.size()) {
        const std::size_t start_byte = i;
        char32_t cp = utf8_next(jp_body, i);
        if (!is_kanji(cp)) continue;
        std::size_t count = 1;
        std::size_t end_byte = i;
        while (i < jp_body.size()) {
            const std::size_t peek = i;
            char32_t next_cp = utf8_next(jp_body, i);
            if (is_kanji(next_cp)) {
                ++count;
                end_byte = i;
            } else {
                i = peek;
                break;
            }
        }
        if (count >= 2) {
            stems.push_back(jp_body.substr(start_byte, end_byte - start_byte));
        }
    }
    return stems;
}

// Also extract 2-char kanji+hiragana verb/adjective roots (e.g. "割れ", "壊れ", "狂っ").
std::vector<std::string> extract_jp_callback_tokens(const std::string& jp_body) {
    std::vector<std::string> tokens = extract_kanji_stems(jp_body);
    std::size_t i = 0;
    while (i < jp_body.size()) {
        const std::size_t b0 = i;
        char32_t cp0 = utf8_next(jp_body, i);
        if (is_kanji(cp0) && i < jp_body.size()) {
            std::size_t b1 = i;
            char32_t cp1 = utf8_next(jp_body, b1);
            if (is_hiragana(cp1)) {
                tokens.push_back(jp_body.substr(b0, b1 - b0));
            }
        }
    }
    return tokens;
}

const std::unordered_set<std::string>& english_stopwords() {
    static const std::unordered_set<std::string> words = {
        "that", "this", "with", "from", "have", "what", "when", "where",
        "which", "there", "their", "they", "them", "then", "than", "your",
        "youre", "dont", "cant", "wont", "isnt", "arent", "wasnt", "werent",
        "would", "could", "should", "about", "into", "over", "after", "before",
        "just", "like", "some", "more", "very", "here", "only", "also",
        "well", "even", "back", "still", "much", "been", "were", "will",
    };
    return words;
}

// Extract 4-letter prefix stems of non-stopword English words.
std::unordered_set<std::string> extract_en_stems(const std::string& en_body) {
    std::unordered_set<std::string> stems;
    const std::string lower = to_lower_ascii(en_body);
    std::string cur;
    for (char c : lower) {
        if (c >= 'a' && c <= 'z') {
            cur += c;
        } else if (c == '\'') {
            continue;
        } else {
            if (cur.size() >= 4 && english_stopwords().count(cur) == 0) {
                stems.insert(cur.substr(0, std::min<std::size_t>(cur.size(), 5)));
            }
            cur.clear();
        }
    }
    if (cur.size() >= 4 && english_stopwords().count(cur) == 0) {
        stems.insert(cur.substr(0, std::min<std::size_t>(cur.size(), 5)));
    }
    return stems;
}

bool has_parenthetical_romaji_or_note(const std::string& en_body) {
    const std::string lower = to_lower_ascii(en_body);
    if (lower.find("[tl") != std::string::npos ||
        lower.find("(tl") != std::string::npos ||
        lower.find("translator note") != std::string::npos) {
        return true;
    }
    const std::size_t open_p = lower.find('(');
    const std::size_t close_p = lower.find(')', open_p == std::string::npos ? 0 : open_p);
    if (open_p != std::string::npos && close_p != std::string::npos && close_p > open_p + 2) {
        const std::string inside = trim(lower.substr(open_p + 1, close_p - open_p - 1));
        if (!inside.empty() && inside.size() <= 18 && inside.find(' ') == std::string::npos &&
            en_body.find('"') != std::string::npos) {
            return true;
        }
    }
    return false;
}

double shannon_entropy_ascii(const std::string& s) {
    if (s.empty()) return 0.0;
    std::array<std::size_t, 256> counts{};
    std::size_t total = 0;
    for (unsigned char c : s) {
        if (c >= 0x20) {
            ++counts[c];
            ++total;
        }
    }
    if (total == 0) return 0.0;
    double h = 0.0;
    const double inv_n = 1.0 / static_cast<double>(total);
    for (std::size_t cnt : counts) {
        if (cnt == 0) continue;
        const double p = static_cast<double>(cnt) * inv_n;
        h -= p * std::log2(p);
    }
    return h;
}

// Yule's K lexical repetition degeneracy check: flags unnatural content-word
// repetition inside a single short/medium clause (Koppel & Ordan 2011).
bool has_degenerate_word_repetition(const std::string& lower_en) {
    std::unordered_map<std::string, int> counts;
    std::string cur;
    int total_words = 0;
    auto flush_word = [&]() {
        if (cur.size() >= 5 && english_stopwords().count(cur) == 0) {
            ++counts[cur];
            ++total_words;
        }
        cur.clear();
    };
    for (char c : lower_en) {
        if (c >= 'a' && c <= 'z') cur += c;
        else if (c != '\'') flush_word();
    }
    flush_word();
    if (total_words >= 3 && total_words <= 14) {
        for (const auto& [w, cnt] : counts) {
            if (cnt >= 3) return true;
        }
    }
    return false;
}

// Directional benefactive (Nakaiwa & Ikehara 1992; Shimazu et al. 2020) and
// overt 1st-person pronoun contradiction check (Voita et al. 2019).
double score_benefactive_and_pronoun_traps(const std::string& jp_body,
                                           const std::string& lower_en) {
    double penalty = 0.0;
    // Inward benefactive: speaker/in-group receives favor (てくれる / てもらう / くださる)
    const bool jp_inward_benefactive =
        jp_body.find("てくれ") != std::string::npos ||
        jp_body.find("てもら") != std::string::npos ||
        jp_body.find("くださ") != std::string::npos;
    if (jp_inward_benefactive) {
        // Reversed vector in English: "I'll ... for you" / "I helped her/him/you"
        const bool en_i_subject =
            contains_word_ci(lower_en, "i") ||
            lower_en.find("i'll") != std::string::npos ||
            lower_en.find("i will") != std::string::npos ||
            lower_en.find("i've") != std::string::npos;
        const bool en_outward_target =
            lower_en.find("for you") != std::string::npos ||
            lower_en.find("helped her") != std::string::npos ||
            lower_en.find("helped him") != std::string::npos ||
            lower_en.find("helped you") != std::string::npos ||
            lower_en.find("saved her") != std::string::npos ||
            lower_en.find("saved him") != std::string::npos ||
            lower_en.find("saved you") != std::string::npos ||
            lower_en.find("gave you") != std::string::npos;
        if (en_i_subject && en_outward_target) {
            penalty += 2.8;
        }
    }

    // Overt 1st-person subject in JP ("俺は", "僕は", "私は", "あたしは") dropped or
    // inverted into 2nd/3rd-person subject with zero 1st-person reference in EN
    const bool jp_overt_first_person =
        jp_body.find("俺は") != std::string::npos ||
        jp_body.find("僕は") != std::string::npos ||
        jp_body.find("私は") != std::string::npos ||
        jp_body.find("あたしは") != std::string::npos;
    if (jp_overt_first_person && utf8_char_len(jp_body) >= 8) {
        const bool en_has_first =
            contains_word_ci(lower_en, "i") || contains_word_ci(lower_en, "me") ||
            contains_word_ci(lower_en, "my") || contains_word_ci(lower_en, "mine") ||
            contains_word_ci(lower_en, "we") || contains_word_ci(lower_en, "us") ||
            lower_en.find("i'm") != std::string::npos ||
            lower_en.find("i've") != std::string::npos ||
            lower_en.find("i'll") != std::string::npos ||
            lower_en.find("i'd") != std::string::npos;
        if (!en_has_first) {
            penalty += 3.0;
        }
    }
    return penalty;
}

bool is_jp_question_line(const std::string& jp_body) {
    return jp_body.find("？") != std::string::npos ||
           jp_body.find("?") != std::string::npos ||
           jp_body.find("のか") != std::string::npos ||
           jp_body.find("じゃない") != std::string::npos ||
           jp_body.find("かしら") != std::string::npos;
}

bool is_pure_silence_line(const std::string& jp_body) {
    if (jp_body.empty()) return false;
    std::size_t i = 0;
    bool has_dots = false;
    while (i < jp_body.size()) {
        const char32_t cp = utf8_next(jp_body, i);
        if (cp == 0x2026 || cp == 0x22EF || cp == '.') {
            has_dots = true;
            continue;
        }
        if (cp == 0x300C || cp == 0x300D || cp == 0x300E || cp == 0x300F ||
            cp == 0xFF08 || cp == 0xFF09 || cp == '(' || cp == ')' ||
            cp == 0x3001 || cp == 0x3002 || cp == 0x30FB || cp == 0x30FC ||
            cp == ' ' || cp == 0x3000 || cp == '\r' || cp == '\n') {
            continue;
        }
        return false;
    }
    return has_dots;
}

}  // namespace

double compute_script_info_bits(const std::string& jp_body) {
    double bits = 0.0;
    std::size_t i = 0;
    while (i < jp_body.size()) {
        const char32_t cp = utf8_next(jp_body, i);
        if (cp == '\r' || cp == '\n' || cp == ' ' || cp == 0x3000) continue;
        if (is_kanji(cp)) {
            bits += 2.4;
        } else if (is_katakana(cp)) {
            bits += 1.4;
        } else {
            bits += 1.0;
        }
    }
    return bits;
}

bool is_h_scene_line(const std::string& jp_text, const std::string& en_text) {
    const std::string jp_body = strip_speaker_prefix_body(jp_text);
    const std::string en_body = strip_speaker_prefix_body(en_text);

    for (const char* jp_term : {
             "肉棒", "秘部", "秘裂", "膣", "子宮", "精液", "射精", "絶頂",
             "陰茎", "亀頭", "乳首", "愛液", "淫乱", "陵辱", "挿入", "結合",
             "ペニス", "アナル", "フェラ",
             "んっ……", "あんっ", "はぁ、はぁ", "ひゃん", "くちゅ", "ぬちゅ",
             "じゅる", "びくびく", "あ、ああっ", "んぅ", "ふぁ"}) {
        if (jp_body.find(jp_term) != std::string::npos) {
            return true;
        }
    }

    const std::string lower_en = to_lower_ascii(en_body);
    for (const char* en_word : {
             "cock", "pussy", "dick", "cum", "cumming", "climax", "orgasm",
             "nipple", "clit", "vagina", "penis", "semen", "thrusting",
             "nngh", "mmph", "aaah", "haaah"}) {
        if (contains_word_ci(lower_en, en_word)) {
            return true;
        }
    }
    return false;
}

struct HotspotDetector::Impl {
    HotspotConfig cfg;

    explicit Impl(HotspotConfig c) : cfg(c) {
        if (cfg.window_size == 0) cfg.window_size = 40;
        if (cfg.cohesion_lookback == 0) cfg.cohesion_lookback = 5;
    }

    OlsModel fit_length_regression(const std::vector<ScriptEntry>& entries) const {
        OlsModel model;
        if (entries.size() < 4) return model;

        double sum_x = 0.0;
        double sum_y = 0.0;
        double sum_xx = 0.0;
        double sum_xy = 0.0;
        std::size_t n = 0;

        for (const auto& e : entries) {
            const std::string jp = strip_speaker_prefix_body(e.jp_text);
            const std::string en = strip_speaker_prefix_body(e.current_en);
            const double x = compute_script_info_bits(jp);
            const double y = static_cast<double>(en.size());
            if (x < 1.0) continue;
            sum_x += x;
            sum_y += y;
            sum_xx += x * x;
            sum_xy += x * y;
            ++n;
        }
        if (n < 4) return model;

        const double dn = static_cast<double>(n);
        const double mean_x = sum_x / dn;
        const double mean_y = sum_y / dn;
        model.mean_info_bits = std::max(10.0, mean_x);

        const double var_x = (sum_xx / dn) - (mean_x * mean_x);
        const double denom = dn * sum_xx - sum_x * sum_x;
        if (var_x > 9.0 && std::abs(denom) > 1e-6) {
            model.beta = std::clamp((dn * sum_xy - sum_x * sum_y) / denom, 0.6, 3.0);
            model.alpha = std::clamp(mean_y - model.beta * mean_x, -4.0, 10.0);
        } else {
            model.beta = std::clamp(mean_y / std::max(1.0, mean_x), 0.6, 3.0);
            model.alpha = 2.0;
        }

        double sq_err = 0.0;
        for (const auto& e : entries) {
            const std::string jp = strip_speaker_prefix_body(e.jp_text);
            const std::string en = strip_speaker_prefix_body(e.current_en);
            const double x = compute_script_info_bits(jp);
            const double y = static_cast<double>(en.size());
            if (x < 1.0) continue;
            const double scale =
                std::sqrt(std::max(10.0, x) / model.mean_info_bits);
            const double res = (y - (model.alpha + model.beta * x)) / scale;
            sq_err += res * res;
        }
        model.sigma = std::max(10.0, std::sqrt(sq_err / dn));
        return model;
    }

    LineWeaknessScore score_single_line(const std::string& jp_text,
                                        const std::string& en_text,
                                        const OlsModel& ols) const {
        LineWeaknessScore sc;
        const std::string jp_body = strip_speaker_prefix_body(jp_text);
        const std::string en_body = strip_speaker_prefix_body(en_text);
        // Never score symbol/punctuation-only lines or untranslated passthrough lines!
        if (!has_real_japanese(jp_body) || en_body == jp_body ||
            trim(en_text) == trim(jp_text)) {
            sc.dominant_signal = "clean";
            return sc;
        }
        if (cfg.skip_h_scenes && is_h_scene_line(jp_body, en_body)) {
            sc.dominant_signal = "clean";
            return sc;
        }

        const double l_jp = static_cast<double>(utf8_char_len(jp_body));
        const double info_bits = compute_script_info_bits(jp_body);
        const double l_en = static_cast<double>(en_body.size());

        // 1. Gale & Church (1993) + Pellegrino et al. (2011) Script-Aware
        //    Information-Load Residual Z_info (with generous literary leeway deadband)
        const double expected_en = std::max(6.0, ols.alpha + ols.beta * info_bits);
        const double mean_bits = std::max(10.0, ols.mean_info_bits);
        const double gc_scale =
            std::max(10.0, ols.sigma) * std::sqrt(std::max(10.0, info_bits) / mean_bits);
        const double delta_gc = std::abs(l_en - expected_en) / gc_scale;

        if (delta_gc > 3.0 && (l_jp >= 10.0 || l_en > 80.0)) {
            sc.length_z = (delta_gc - 3.0) * 1.4;
        }
        if (l_jp >= 22.0 && l_en < l_jp * 0.35) {
            sc.length_z += 3.5;  // Dropped clause / severe truncation
        } else if (l_en > expected_en * 3.5 && l_en > 140.0) {
            sc.length_z += 2.5;  // Extreme bloated explanation
        }

        // 2. Artifact Score (and optional Punctuation/Romaji/Quote Mismatch when fix_punctuation == true)
        if (has_real_japanese(en_body) && en_body != jp_body) {
            sc.artifact_score += 5.0;  // Actual untranslated CJK/kana leaked into English
        }
        if (cfg.fix_punctuation) {
            if (en_body.find("「") != std::string::npos ||
                en_body.find("」") != std::string::npos ||
                en_body.find("『") != std::string::npos ||
                en_body.find("』") != std::string::npos ||
                en_body.find("…") != std::string::npos ||
                en_body.find("—") != std::string::npos ||
                en_body.find("。") != std::string::npos ||
                en_body.find("、") != std::string::npos) {
                sc.artifact_score += 3.0;  // Fullwidth brackets or non-ASCII punctuation
            }
            if (has_parenthetical_romaji_or_note(en_body)) {
                sc.artifact_score += 4.5;  // Inline romaji gloss like "(hippu)" or "[TL:"
            }
            const bool jp_has_spoken_quote =
                jp_body.find("「") != std::string::npos ||
                jp_body.find("『") != std::string::npos;
            const bool en_has_double_quote = en_body.find('"') != std::string::npos;
            if (jp_has_spoken_quote && !en_has_double_quote && l_jp >= 4.0) {
                sc.artifact_score += 3.5;  // Spoken dialogue lost its quotes
            } else if (!jp_has_spoken_quote && !en_body.empty() &&
                       en_body.front() == '"' && en_body.back() == '"') {
                sc.artifact_score += 3.0;  // Narration erroneously wrapped in spoken quotes
            }
        }

        // 3. Discourse Benefactive & Overt Pronoun Contradiction
        const std::string lower_en = to_lower_ascii(en_body);
        sc.coupling_score += score_benefactive_and_pronoun_traps(jp_body, lower_en);

        // 4. Volansky et al. (2015) / Koppel & Ordan (2011) Translationese,
        //    Yule's K Repetition, Entropy & Westernization/Bowdlerization
        for (const char* awkward : {"as for ", "concerning ", "with regards to ",
                                    "regarding the fact", "it cannot be helped that",
                                    "is it not that ", "is it not being ", "it is that ",
                                    "the fact that "}) {
            if (contains_ci(lower_en, awkward)) {
                sc.style_score += 2.5;
            }
        }
        if (has_degenerate_word_repetition(lower_en)) {
            sc.style_score += 2.0;
        }
        for (const char* slang : {"bruh", "no cap", "rizz", "skibidi", "y'all",
                                  "my dude", "homie", "lowkey", "highkey"}) {
            if (contains_ci(lower_en, slang)) {
                sc.style_score += 4.0;
            }
        }
        if (jp_body.find("殺") != std::string::npos ||
            jp_body.find("死") != std::string::npos ||
            jp_body.find("犯") != std::string::npos ||
            jp_body.find("クソ") != std::string::npos) {
            for (const char* soft : {"darn", "heck", "gosh", "fudge", "frick", "poop"}) {
                if (contains_ci(lower_en, soft)) {
                    sc.style_score += 3.8;
                }
            }
        }
        if (en_body.size() >= 25) {
            const double h = shannon_entropy_ascii(en_body);
            if (h < 2.6) {
                sc.style_score += 2.0;  // Degenerate repetition / corrupted output
            }
        }

        sc.raw_total = sc.length_z + sc.artifact_score + sc.coupling_score + sc.style_score;
        sc.dominant_signal = pick_dominant_signal(sc);
        return sc;
    }

    static std::string pick_dominant_signal(const LineWeaknessScore& s) {
        double best = 0.0;
        std::string label = "clean";
        if (s.artifact_score > best) {
            best = s.artifact_score;
            label = "artifact_or_quote_mismatch";
        }
        if (s.coupling_score > best) {
            best = s.coupling_score;
            label = "broken_callback_or_voice_tic";
        }
        if (s.style_score > best) {
            best = s.style_score;
            label = "stiff_or_westernized_style";
        }
        if (s.length_z > best) {
            best = s.length_z;
            label = "length_ratio_outlier";
        }
        return label;
    }

    std::vector<LineWeaknessScore> score_entries(
        const std::vector<ScriptEntry>& entries) const {
        std::vector<LineWeaknessScore> scores(entries.size());
        if (entries.empty()) return scores;

        const OlsModel ols = fit_length_regression(entries);

        // Pre-extract callback tokens and English stems for 5-turn BlonDe cohesion
        std::vector<std::string> jp_bodies(entries.size());
        std::vector<std::string> en_bodies(entries.size());
        std::vector<bool> h_line(entries.size(), false);
        std::vector<std::vector<std::string>> jp_tokens(entries.size());
        std::vector<std::vector<std::string>> jp_kanji2(entries.size());
        std::vector<std::unordered_set<std::string>> en_stems(entries.size());

        for (std::size_t i = 0; i < entries.size(); ++i) {
            jp_bodies[i] = strip_speaker_prefix_body(entries[i].jp_text);
            en_bodies[i] = strip_speaker_prefix_body(entries[i].current_en);
            const bool is_passthrough =
                !has_real_japanese(jp_bodies[i]) || en_bodies[i] == jp_bodies[i] ||
                trim(entries[i].current_en) == trim(entries[i].jp_text);
            h_line[i] = is_passthrough ||
                        (cfg.skip_h_scenes && is_h_scene_line(jp_bodies[i], en_bodies[i]));
            jp_tokens[i] = extract_jp_callback_tokens(jp_bodies[i]);
            jp_kanji2[i] = extract_kanji_stems(jp_bodies[i]);
            en_stems[i] = extract_en_stems(en_bodies[i]);
        }

        for (std::size_t i = 0; i < entries.size(); ++i) {
            scores[i] = score_single_line(entries[i].jp_text, entries[i].current_en, ols);
            scores[i].global_index = entries[i].global_index;
            if (h_line[i]) {
                continue;
            }

            const std::string& jp_body = jp_bodies[i];
            const std::string& en_body = en_bodies[i];
            const std::string lower_en = to_lower_ascii(en_body);

            // Kinsui (2003) Yakuwarigo role-language drop detection
            if (jp_body.find("にゃ") != std::string::npos ||
                jp_body.find("かにゃ") != std::string::npos) {
                if (lower_en.find("nya") == std::string::npos &&
                    lower_en.find("meow") == std::string::npos &&
                    lower_en.find("purr") == std::string::npos &&
                    lower_en.find("cat") == std::string::npos) {
                    scores[i].coupling_score += 2.6;
                }
            }
            if (jp_body.find("でござる") != std::string::npos &&
                (lower_en.find("gonna") != std::string::npos ||
                 lower_en.find("wanna") != std::string::npos ||
                 lower_en.find("yeah") != std::string::npos)) {
                scores[i].coupling_score += 2.2;
            }

            // Immediate Q-A Adjacency Pair & Banter Verb-Echo Mismatch (i-1 -> i)
            if (i > 0 && !h_line[i - 1] &&
                entries[i].script_name == entries[i - 1].script_name) {
                if (utf8_char_len(jp_body) <= 32 && utf8_char_len(jp_bodies[i - 1]) <= 32) {
                    bool shared_jp_stem = false;
                    for (const auto& t1 : jp_tokens[i - 1]) {
                        for (const auto& t2 : jp_tokens[i]) {
                            if (t1 == t2) {
                                shared_jp_stem = true;
                                break;
                            }
                        }
                        if (shared_jp_stem) break;
                    }
                    if (shared_jp_stem) {
                        bool shared_en_stem = false;
                        for (const auto& s1 : en_stems[i - 1]) {
                            if (en_stems[i].count(s1) != 0) {
                                shared_en_stem = true;
                                break;
                            }
                        }
                        if (!shared_en_stem && !en_stems[i - 1].empty() &&
                            !en_stems[i].empty()) {
                            const double qa_bonus =
                                is_jp_question_line(jp_bodies[i - 1]) ? 0.8 : 0.0;
                            scores[i - 1].coupling_score += 2.2 + qa_bonus;
                            scores[i].coupling_score += 3.2 + qa_bonus;
                            scores[i - 1].raw_total = scores[i - 1].length_z +
                                                      scores[i - 1].artifact_score +
                                                      scores[i - 1].coupling_score +
                                                      scores[i - 1].style_score;
                            scores[i - 1].dominant_signal =
                                pick_dominant_signal(scores[i - 1]);
                        }
                    }
                }
            }

            // BlonDe (Jiang et al. NAACL 2022) Cross-Turn Kanji Cohesion over lookback 2..W_lex
            const std::size_t max_back = std::min(i, cfg.cohesion_lookback);
            for (std::size_t back = 2; back <= max_back; ++back) {
                const std::size_t prev = i - back;
                if (entries[i].script_name != entries[prev].script_name) break;
                if (h_line[prev]) continue;
                if (utf8_char_len(jp_body) > 28 || utf8_char_len(jp_bodies[prev]) > 28) {
                    continue;
                }
                bool shared_compound = false;
                for (const auto& k1 : jp_kanji2[prev]) {
                    for (const auto& k2 : jp_kanji2[i]) {
                        if (k1 == k2) {
                            shared_compound = true;
                            break;
                        }
                    }
                    if (shared_compound) break;
                }
                if (shared_compound && !en_stems[prev].empty() && !en_stems[i].empty()) {
                    bool shared_en = false;
                    for (const auto& s1 : en_stems[prev]) {
                        if (en_stems[i].count(s1) != 0) {
                            shared_en = true;
                            break;
                        }
                    }
                    if (!shared_en) {
                        scores[i].coupling_score += 1.4;
                        break;
                    }
                }
            }

            scores[i].raw_total = scores[i].length_z + scores[i].artifact_score +
                                  scores[i].coupling_score + scores[i].style_score;
            scores[i].dominant_signal = pick_dominant_signal(scores[i]);
        }

        // Page's (1954) CUSUM Sequential Change-Point Detection across the novel:
        // S_i = max(0, S_{i-1} + s_i - (mu_s + k_drift))
        double sum_s = 0.0;
        for (const auto& s : scores) sum_s += s.raw_total;
        const double mean_s = sum_s / static_cast<double>(scores.size());
        const double reference_val = mean_s + cfg.cusum_drift_k;
        double running_cusum = 0.0;
        for (std::size_t i = 0; i < scores.size(); ++i) {
            if (i > 0 && entries[i].script_name != entries[i - 1].script_name) {
                running_cusum = 0.0;
            }
            running_cusum =
                std::max(0.0, running_cusum + scores[i].raw_total - reference_val);
            scores[i].cusum_stat = running_cusum;
        }

        // Gaussian Kernel Density Estimation (KDE) across the novel
        const double sigma =
            std::max(1.0, static_cast<double>(cfg.window_size) / 4.0);
        const int radius = static_cast<int>(std::ceil(3.0 * sigma));
        std::vector<double> kernel(static_cast<std::size_t>(2 * radius + 1));
        double k_sum = 0.0;
        for (int k = -radius; k <= radius; ++k) {
            const double w =
                std::exp(-static_cast<double>(k * k) / (2.0 * sigma * sigma));
            kernel[static_cast<std::size_t>(k + radius)] = w;
            k_sum += w;
        }
        for (double& w : kernel) w /= k_sum;

        const int n = static_cast<int>(scores.size());
        for (int i = 0; i < n; ++i) {
            double acc = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                const int j = std::clamp(i + k, 0, n - 1);
                acc += scores[static_cast<std::size_t>(j)].raw_total *
                       kernel[static_cast<std::size_t>(k + radius)];
            }
            scores[static_cast<std::size_t>(i)].smoothed_density = acc;
        }

        return scores;
    }

    std::vector<HotspotWindow> select_hotspots(
        const std::vector<ScriptEntry>& entries,
        std::size_t max_windows,
        std::vector<LineWeaknessScore>* out_scores) const {
        std::vector<LineWeaknessScore> scores = score_entries(entries);
        if (entries.empty()) {
            if (out_scores) *out_scores = std::move(scores);
            return {};
        }

        const std::size_t n = entries.size();
        const std::size_t w = std::min(cfg.window_size, n);
        const std::size_t num_cand = n - w + 1;

        // Prefix sum of smoothed_density and H-scene line counts for O(1) window queries
        std::vector<double> prefix(n + 1, 0.0);
        std::vector<std::size_t> h_prefix(n + 1, 0);
        for (std::size_t i = 0; i < n; ++i) {
            prefix[i + 1] = prefix[i] + scores[i].smoothed_density;
            const bool is_h = cfg.skip_h_scenes &&
                              is_h_scene_line(entries[i].jp_text, entries[i].current_en);
            h_prefix[i + 1] = h_prefix[i] + (is_h ? 1u : 0u);
        }

        std::vector<double> energies(num_cand, 0.0);
        std::vector<bool> suppressed(num_cand, false);
        double sum_e = 0.0;
        std::size_t valid_cand = 0;
        for (std::size_t a = 0; a < num_cand; ++a) {
            if (cfg.skip_h_scenes &&
                (h_prefix[a + w] - h_prefix[a]) >= cfg.max_h_lines_per_window) {
                suppressed[a] = true;
                continue;
            }
            energies[a] = prefix[a + w] - prefix[a];
            sum_e += energies[a];
            ++valid_cand;
        }
        const double denom_cand = static_cast<double>(std::max<std::size_t>(1, valid_cand));
        const double mean_e = sum_e / denom_cand;
        double sq_diff = 0.0;
        for (std::size_t a = 0; a < num_cand; ++a) {
            if (suppressed[a]) continue;
            const double d = energies[a] - mean_e;
            sq_diff += d * d;
        }
        const double std_e = std::max(1e-6, std::sqrt(sq_diff / denom_cand));

        std::vector<std::size_t> order(num_cand);
        for (std::size_t a = 0; a < num_cand; ++a) order[a] = a;
        std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
            if (std::abs(energies[lhs] - energies[rhs]) > 1e-9) {
                return energies[lhs] > energies[rhs];
            }
            return lhs < rhs;
        });

        std::vector<HotspotWindow> selected;

        for (std::size_t a : order) {
            if (suppressed[a]) continue;
            const double kde_z = (energies[a] - mean_e) / std_e;

            std::size_t peak_idx = a;
            double peak_score = -1.0;
            double max_cusum = 0.0;
            for (std::size_t j = a; j < a + w; ++j) {
                if (scores[j].raw_total > peak_score) {
                    peak_score = scores[j].raw_total;
                    peak_idx = j;
                }
                if (scores[j].cusum_stat > max_cusum) {
                    max_cusum = scores[j].cusum_stat;
                }
            }

            // Combine KDE Z-score with Page's CUSUM sequential change-point boost
            const double combined_z = kde_z + 0.05 * max_cusum;
            if (!selected.empty() && combined_z < cfg.z_threshold) {
                break;
            }

            HotspotWindow hw;
            hw.offset = a;
            hw.length = w;
            hw.energy = energies[a];
            hw.cusum_peak = max_cusum;
            hw.z_score = combined_z;
            hw.peak_index = peak_idx;
            hw.peak_line_score = std::max(0.0, peak_score);
            hw.dominant_signal = scores[peak_idx].dominant_signal;
            selected.push_back(std::move(hw));

            if (max_windows > 0 && selected.size() >= max_windows) {
                break;
            }

            const std::size_t sup_begin = (a + 1 > w) ? (a + 1 - w) : 0;
            const std::size_t sup_end = std::min(num_cand, a + w);
            for (std::size_t s = sup_begin; s < sup_end; ++s) {
                suppressed[s] = true;
            }
        }

        if (out_scores) *out_scores = std::move(scores);
        return selected;
    }

    NeighborExpansion evaluate_contagion(
        std::size_t window_offset,
        std::size_t window_len,
        std::size_t total_entries,
        const std::vector<Improvement>& window_improvements) const {
        NeighborExpansion exp;
        if (window_len == 0 || total_entries == 0 || window_improvements.empty()) {
            return exp;
        }

        const double defect_rate =
            static_cast<double>(window_improvements.size()) /
            static_cast<double>(window_len);
        const bool high_density = defect_rate >= cfg.adaptive_defect_rate;

        bool near_left = false;
        bool near_right = false;
        const std::size_t window_end = std::min(total_entries, window_offset + window_len);
        for (const auto& imp : window_improvements) {
            if (imp.global_index >= window_offset &&
                imp.global_index < window_offset + cfg.boundary_margin) {
                near_left = true;
            }
            if (imp.global_index < window_end &&
                imp.global_index + cfg.boundary_margin >= window_end) {
                near_right = true;
            }
        }

        if ((high_density || near_left) && window_offset > 0) {
            exp.expand_left = true;
            exp.left_offset =
                (window_offset >= cfg.window_size) ? (window_offset - cfg.window_size) : 0;
            exp.left_length = window_offset - exp.left_offset;
        }
        if ((high_density || near_right) && window_end < total_entries) {
            exp.expand_right = true;
            exp.right_offset = window_end;
            exp.right_length = std::min(cfg.window_size, total_entries - window_end);
        }
        return exp;
    }
};

HotspotDetector::HotspotDetector(HotspotConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

HotspotDetector::~HotspotDetector() = default;
HotspotDetector::HotspotDetector(HotspotDetector&& other) noexcept = default;
HotspotDetector& HotspotDetector::operator=(HotspotDetector&& other) noexcept = default;

OlsModel HotspotDetector::fit_length_regression(
    const std::vector<ScriptEntry>& entries) const {
    return impl_->fit_length_regression(entries);
}

LineWeaknessScore HotspotDetector::score_single_line(
    const std::string& jp_text,
    const std::string& en_text,
    const OlsModel& ols) const {
    return impl_->score_single_line(jp_text, en_text, ols);
}

std::vector<LineWeaknessScore> HotspotDetector::score_entries(
    const std::vector<ScriptEntry>& entries) const {
    return impl_->score_entries(entries);
}

std::vector<HotspotWindow> HotspotDetector::select_hotspots(
    const std::vector<ScriptEntry>& entries,
    std::size_t max_windows,
    std::vector<LineWeaknessScore>* out_scores) const {
    return impl_->select_hotspots(entries, max_windows, out_scores);
}

NeighborExpansion HotspotDetector::evaluate_contagion(
    std::size_t window_offset,
    std::size_t window_len,
    std::size_t total_entries,
    const std::vector<Improvement>& window_improvements) const {
    return impl_->evaluate_contagion(
        window_offset, window_len, total_entries, window_improvements);
}

const HotspotConfig& HotspotDetector::config() const {
    return impl_->cfg;
}

}  // namespace spot_checker
