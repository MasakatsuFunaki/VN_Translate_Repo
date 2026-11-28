// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "domain/tone_glossary.h"

#include "common/utf8_text.h"

#include <array>
#include <utility>

namespace spot_checker {

namespace {

constexpr std::string_view kFormattingRulesOpen = "<formatting_rules>";
constexpr std::string_view kFormattingRulesClose = "</formatting_rules>";
constexpr std::string_view kBatchRulesOpen = "<batch_rules>";
constexpr std::string_view kBatchRulesClose = "</batch_rules>";
constexpr std::string_view kOutputFormatOpen = "<output_format>";
constexpr std::string_view kOutputFormatClose = "</output_format>";

constexpr std::string_view kNameRefOpen = "<name_reference>";
constexpr std::string_view kNameRefClose = "</name_reference>";
constexpr std::string_view kCharDossierOpen = "<character_dossier>";
constexpr std::string_view kCharDossierClose = "</character_dossier>";
constexpr std::string_view kCharactersOpen = "<characters>";
constexpr std::string_view kCharactersClose = "</characters>";
constexpr std::string_view kTerminologyOpen = "<terminology>";
constexpr std::string_view kTerminologyClose = "</terminology>";
constexpr std::string_view kGlossaryOpen = "<glossary>";
constexpr std::string_view kGlossaryClose = "</glossary>";

constexpr std::string_view kAsciiArrow = "->";
constexpr std::string_view kUtf8RightArrow = "→";
constexpr std::string_view kFullwidthSlash = "／";
constexpr std::string_view kBoldMarker = "**";
constexpr std::size_t kMinSymmetricWrapBytes = 2;
constexpr std::size_t kMaxSpeakerKeyCodepoints = 24;

constexpr std::array<std::string_view, 10> kThirdPersonPronouns = {
    "he",
    "him",
    "his",
    "she",
    "her",
    "hers",
    "they",
    "them",
    "their",
    "theirs",
};

constexpr std::array<std::string_view, 23> kGenericCommonSpeakerNouns = {
    "みんな", "全員",   "少女", "少年", "女",   "男",   "女子", "男子",
    "声",     "？？？", "？",   "二人", "ふたり", "教師", "先生", "母",
    "父",     "客",     "店員", "生徒", "部員", "男の子", "女の子",
};

bool is_generic_common_noun(std::string_view jp) noexcept {
    for (const std::string_view common : kGenericCommonSpeakerNouns) {
        if (jp == common) {
            return true;
        }
    }
    return false;
}

void erase_xml_section(std::string& text,
                       std::string_view open_tag,
                       std::string_view close_tag) {
    for (;;) {
        const std::size_t open_pos = text.find(open_tag);
        if (open_pos == std::string::npos) {
            return;
        }
        const std::size_t close_pos = text.find(close_tag, open_pos + open_tag.size());
        if (close_pos == std::string::npos) {
            text.erase(open_pos);
            return;
        }
        text.erase(open_pos, close_pos + close_tag.size() - open_pos);
    }
}

std::string strip_markdown_decorations(std::string_view raw) {
    std::string s = trim_unicode(raw);
    for (;;) {
        if (s.size() >= kMinSymmetricWrapBytes && s.front() == '`' && s.back() == '`') {
            s = trim_unicode(
                std::string_view(s).substr(1U, s.size() - kMinSymmetricWrapBytes));
            continue;
        }
        if (s.size() >= kBoldMarker.size() * kMinSymmetricWrapBytes &&
            s.rfind(kBoldMarker, 0) == 0 &&
            s.compare(s.size() - kBoldMarker.size(),
                      kBoldMarker.size(),
                      kBoldMarker.data(),
                      kBoldMarker.size()) == 0) {
            s = trim_unicode(std::string_view(s).substr(
                kBoldMarker.size(),
                s.size() - kBoldMarker.size() * kMinSymmetricWrapBytes));
            continue;
        }
        if (s.size() >= kMinSymmetricWrapBytes && s.front() == '*' && s.back() == '*') {
            s = trim_unicode(
                std::string_view(s).substr(1U, s.size() - kMinSymmetricWrapBytes));
            continue;
        }
        return s;
    }
}

std::string clean_en_target(std::string_view raw_en) {
    std::string en = strip_markdown_decorations(raw_en);
    const std::size_t paren_pos = en.find('(');
    if (paren_pos != std::string::npos) {
        en = trim_unicode(std::string_view(en).substr(0, paren_pos));
    }
    return en;
}

std::size_t count_japanese_codepoints(std::string_view text) noexcept {
    std::size_t count = 0;
    std::size_t idx = 0;
    while (idx < text.size()) {
        const char32_t cp = utf8_next(text, idx);
        if (is_hiragana(cp) || is_lexical_katakana(cp) || is_kanji(cp)) {
            ++count;
        }
    }
    return count;
}

bool is_table_separator_cell(std::string_view cell) noexcept {
    const std::string_view trimmed = trim_ascii_view(cell);
    if (trimmed.empty()) {
        return true;
    }
    for (char c : trimmed) {
        if (c != '-' && c != ':' && c != ' ') {
            return false;
        }
    }
    return true;
}

bool is_valid_en_glossary_value(std::string_view en) noexcept {
    if (en.empty() || has_japanese(en)) {
        return false;
    }
    for (char c : en) {
        if (c == '/' || c == ',' || c == '"' || c == '|' || c == '\r' || c == '\n') {
            return false;
        }
    }
    return true;
}

bool is_valid_jp_speaker_key(std::string_view jp) noexcept {
    if (jp.empty() || !has_japanese(jp)) {
        return false;
    }
    if (utf8_codepoint_count(jp) > kMaxSpeakerKeyCodepoints) {
        return false;
    }
    for (char c : jp) {
        if (c == ':' || c == '(' || c == ')' || c == '"' || c == ';' || c == '|' ||
            c == '\r' || c == '\n') {
            return false;
        }
    }
    return true;
}

bool is_explicit_non_person_role(std::string_view role_col) {
    const std::string lower = to_lower_ascii(role_col);
    if (lower.empty()) {
        return false;
    }
    return lower.find("place") != std::string::npos ||
           lower.find("school") != std::string::npos ||
           lower.find("location") != std::string::npos ||
           lower.find("term") != std::string::npos ||
           lower.find("item") != std::string::npos ||
           lower.find("lore") != std::string::npos ||
           lower.find("org") != std::string::npos ||
           lower.find("facility") != std::string::npos ||
           lower.find("club") != std::string::npos;
}

bool classify_person_from_role_column(std::string_view role_col,
                                      bool fallback_is_person) {
    const std::string lower = to_lower_ascii(role_col);
    if (lower.empty()) {
        return fallback_is_person;
    }
    if (is_explicit_non_person_role(lower)) {
        return false;
    }
    if (lower.find("character") != std::string::npos ||
        lower.find("person") != std::string::npos ||
        lower.find("speaker") != std::string::npos ||
        lower.find("protagonist") != std::string::npos ||
        lower.find("heroine") != std::string::npos ||
        lower.find("girl") != std::string::npos ||
        lower.find("boy") != std::string::npos ||
        lower.find("student") != std::string::npos ||
        lower.find("sister") != std::string::npos ||
        lower.find("brother") != std::string::npos ||
        lower.find("friend") != std::string::npos) {
        return true;
    }
    return fallback_is_person;
}

std::size_t count_third_person_pronouns(std::string_view text) noexcept {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !is_word_byte(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && is_word_byte(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i == start) {
            continue;
        }
        const std::string_view token = text.substr(start, i - start);
        for (const std::string_view pron : kThirdPersonPronouns) {
            if (contains_word_ascii_ci(token, pron) && token.size() == pron.size()) {
                ++count;
                break;
            }
        }
    }
    return count;
}

}  // namespace

std::string ToneGlossary::strip_translator_formatting_rules(
    std::string_view raw_tone_md) {
    std::string out(raw_tone_md);
    erase_xml_section(out, kFormattingRulesOpen, kFormattingRulesClose);
    erase_xml_section(out, kBatchRulesOpen, kBatchRulesClose);
    erase_xml_section(out, kOutputFormatOpen, kOutputFormatClose);
    return trim_ascii(out);
}

ToneGlossary ToneGlossary::parse_markdown(std::string_view raw_tone_md) {
    ToneGlossary glossary;
    glossary.sanitized_guide_md_ = strip_translator_formatting_rules(raw_tone_md);
    if (glossary.sanitized_guide_md_.empty()) {
        return glossary;
    }

    std::string scan_text = glossary.sanitized_guide_md_;
    const std::array<std::pair<std::string_view, bool>, 10> xml_markers = {{
        {kNameRefOpen, true},
        {kNameRefClose, false},
        {kCharDossierOpen, true},
        {kCharDossierClose, false},
        {kCharactersOpen, true},
        {kCharactersClose, false},
        {kTerminologyOpen, false},
        {kTerminologyClose, false},
        {kGlossaryOpen, false},
        {kGlossaryClose, false},
    }};
    for (const auto& [tag, unused_flag] : xml_markers) {
        static_cast<void>(unused_flag);
        std::size_t pos = 0;
        while ((pos = scan_text.find(tag, pos)) != std::string::npos) {
            scan_text.insert(pos + tag.size(), "\n");
            scan_text.insert(pos, "\n");
            pos += tag.size() + 2U;
        }
    }

    bool xml_person_mode = false;
    bool xml_term_mode = false;
    bool heading_person_mode = false;
    bool heading_term_mode = false;

    auto register_parsed_pair = [&](std::string_view raw_jp,
                                    std::string_view raw_en,
                                    bool is_person,
                                    bool add_to_speaker_map) {
        std::string jp = strip_markdown_decorations(raw_jp);
        if (jp.rfind(kFullwidthSlash, 0) == 0) {
            jp = trim_unicode(std::string_view(jp).substr(kFullwidthSlash.size()));
        }
        const std::string en = clean_en_target(raw_en);
        if (jp.empty() || !is_valid_en_glossary_value(en)) {
            return;
        }
        if (add_to_speaker_map && is_valid_jp_speaker_key(jp)) {
            glossary.speaker_map_[jp] = en;
        }
        if (!is_generic_common_noun(jp)) {
            glossary.add_term(jp, en, is_person);
        }
    };

    std::size_t pos = 0;
    while (pos <= scan_text.size()) {
        const std::size_t nl = scan_text.find('\n', pos);
        const std::string_view raw_line =
            (nl == std::string::npos) ? std::string_view(scan_text).substr(pos)
                                      : std::string_view(scan_text).substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? (scan_text.size() + 1U) : (nl + 1U);

        std::string_view line = trim_ascii_view(raw_line);
        if (line.empty()) {
            continue;
        }

        if (line == kNameRefOpen || line == kCharDossierOpen || line == kCharactersOpen) {
            xml_person_mode = true;
            xml_term_mode = false;
            continue;
        }
        if (line == kNameRefClose || line == kCharDossierClose || line == kCharactersClose) {
            xml_person_mode = false;
            continue;
        }
        if (line == kTerminologyOpen || line == kGlossaryOpen) {
            xml_term_mode = true;
            xml_person_mode = false;
            continue;
        }
        if (line == kTerminologyClose || line == kGlossaryClose) {
            xml_term_mode = false;
            continue;
        }

        if (line.front() == '#') {
            const std::string lower_h = to_lower_ascii(line);
            if (lower_h.find("term") != std::string::npos ||
                lower_h.find("place") != std::string::npos ||
                lower_h.find("location") != std::string::npos ||
                lower_h.find("setting") != std::string::npos ||
                lower_h.find("world") != std::string::npos ||
                lower_h.find("lore") != std::string::npos ||
                lower_h.find("school") != std::string::npos ||
                lower_h.find("item") != std::string::npos ||
                lower_h.find("glossary") != std::string::npos) {
                heading_person_mode = false;
                heading_term_mode = true;
            } else if (lower_h.find("character") != std::string::npos ||
                       lower_h.find("cast") != std::string::npos ||
                       lower_h.find("speaker") != std::string::npos ||
                       lower_h.find("name") != std::string::npos ||
                       lower_h.find("protagonist") != std::string::npos ||
                       lower_h.find("heroine") != std::string::npos) {
                heading_person_mode = true;
                heading_term_mode = false;
            }
            continue;
        }

        const bool in_explicit_term_section =
            xml_term_mode || (!xml_person_mode && heading_term_mode);
        const bool default_is_person =
            xml_person_mode || (!xml_term_mode && heading_person_mode);
        const bool default_add_speaker = default_is_person || !in_explicit_term_section;

        if (line.front() == '|' && line.size() >= 3U) {
            const std::size_t p1 = line.find('|', 1U);
            if (p1 != std::string_view::npos && p1 + 1U < line.size()) {
                const std::size_t p2 = line.find('|', p1 + 1U);
                const std::string_view c1 = line.substr(1U, p1 - 1U);
                const std::string_view c2 =
                    (p2 == std::string_view::npos)
                        ? line.substr(p1 + 1U)
                        : line.substr(p1 + 1U, p2 - p1 - 1U);
                if (is_table_separator_cell(c1) || is_table_separator_cell(c2)) {
                    continue;
                }
                std::string_view c3;
                if (p2 != std::string_view::npos && p2 + 1U < line.size()) {
                    const std::size_t p3 = line.find('|', p2 + 1U);
                    c3 = (p3 == std::string_view::npos)
                             ? line.substr(p2 + 1U)
                             : line.substr(p2 + 1U, p3 - p2 - 1U);
                }
                const bool row_is_person =
                    classify_person_from_role_column(c3, default_is_person);
                const bool row_add_speaker =
                    row_is_person ||
                    (!in_explicit_term_section && !is_explicit_non_person_role(c3));
                register_parsed_pair(c1, c2, row_is_person, row_add_speaker);
            }
            continue;
        }

        if ((line.front() == '-' || line.front() == '*') && line.size() > 2U &&
            (line[1] == ' ' || line[1] == '\t')) {
            line = trim_ascii_view(line.substr(2U));
        }

        if (const std::size_t arrow_pos = line.find(kAsciiArrow);
            arrow_pos != std::string_view::npos) {
            register_parsed_pair(line.substr(0, arrow_pos),
                                 line.substr(arrow_pos + kAsciiArrow.size()),
                                 default_is_person,
                                 default_add_speaker);
            continue;
        }

        if (const std::size_t u8_arrow = line.find(kUtf8RightArrow);
            u8_arrow != std::string_view::npos) {
            register_parsed_pair(line.substr(0, u8_arrow),
                                 line.substr(u8_arrow + kUtf8RightArrow.size()),
                                 default_is_person,
                                 default_add_speaker);
            continue;
        }

        if (line.find('=') != std::string_view::npos) {
            std::size_t token_start = 0;
            while (token_start < line.size()) {
                const std::size_t comma = line.find(',', token_start);
                const std::string_view token =
                    (comma == std::string_view::npos)
                        ? line.substr(token_start)
                        : line.substr(token_start, comma - token_start);
                const std::size_t eq = token.find('=');
                if (eq != std::string_view::npos) {
                    const bool eq_is_person = !in_explicit_term_section &&
                                              (xml_person_mode || heading_person_mode ||
                                               comma != std::string_view::npos);
                    const bool eq_add_speaker =
                        eq_is_person || !in_explicit_term_section;
                    register_parsed_pair(token.substr(0, eq),
                                         token.substr(eq + 1U),
                                         eq_is_person,
                                         eq_add_speaker);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                token_start = comma + 1U;
            }
        }
    }

    return glossary;
}

void ToneGlossary::merge_speaker_pair(std::string_view jp_speaker,
                                      std::string_view en_speaker) {
    std::string jp = strip_markdown_decorations(jp_speaker);
    if (jp.rfind(kFullwidthSlash, 0) == 0) {
        jp = trim_unicode(std::string_view(jp).substr(kFullwidthSlash.size()));
    }
    const std::string en = clean_en_target(en_speaker);
    if (!is_valid_jp_speaker_key(jp) || !is_valid_en_glossary_value(en)) {
        return;
    }
    speaker_map_.emplace(jp, en);
    if (!is_generic_common_noun(jp)) {
        add_term(jp, en, true);
    }
}

void ToneGlossary::add_term(std::string_view jp_term,
                            std::string_view en_term,
                            bool is_person_name) {
    const std::string jp = trim_unicode(jp_term);
    const std::string en = trim_unicode(en_term);
    if (count_japanese_codepoints(jp) < kMinGlossaryJpCodepoints ||
        en.size() < kMinGlossaryEnBytes || has_japanese(en)) {
        return;
    }
    for (const auto& existing : entries_) {
        if (existing.jp_term == jp) {
            return;
        }
    }
    entries_.push_back(GlossaryEntry{jp, en, is_person_name});
}

std::string ToneGlossary::resolve_speaker(std::string_view raw_speaker) const {
    std::string trimmed = trim_unicode(raw_speaker);
    if (trimmed.empty()) {
        return "NARRATION";
    }
    if (const auto it = speaker_map_.find(trimmed); it != speaker_map_.end()) {
        return it->second;
    }
    if (trimmed.rfind(kFullwidthSlash, 0) == 0) {
        const std::string stripped =
            trim_unicode(std::string_view(trimmed).substr(kFullwidthSlash.size()));
        if (stripped.empty()) {
            return "NARRATION";
        }
        if (const auto it = speaker_map_.find(stripped); it != speaker_map_.end()) {
            return it->second;
        }
        return stripped;
    }
    return trimmed;
}

bool ToneGlossary::check_glossary_preservation(std::string_view jp_body,
                                               std::string_view old_en_body,
                                               std::string_view new_en_body,
                                               std::string* violated_term_out) const {
    if (entries_.empty()) {
        return true;
    }

    const std::size_t old_3p = count_third_person_pronouns(old_en_body);
    const std::size_t new_3p = count_third_person_pronouns(new_en_body);
    std::size_t pronoun_delta_budget = (new_3p > old_3p) ? (new_3p - old_3p) : 0U;

    for (const auto& entry : entries_) {
        if (entry.jp_term.empty() || entry.en_term.empty()) {
            continue;
        }
        if (jp_body.find(entry.jp_term) == std::string_view::npos) {
            continue;
        }
        if (!contains_word_ascii_ci(old_en_body, entry.en_term)) {
            continue;
        }
        if (contains_word_ascii_ci(new_en_body, entry.en_term)) {
            continue;
        }
        if (entry.is_person_name && pronoun_delta_budget > 0U) {
            --pronoun_delta_budget;
            continue;
        }
        if (violated_term_out != nullptr) {
            *violated_term_out = entry.jp_term + " -> " + entry.en_term;
        }
        return false;
    }
    return true;
}

}  // namespace spot_checker
