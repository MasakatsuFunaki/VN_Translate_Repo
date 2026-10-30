// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/spot_checker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "client/argon_client.h"
#include "common/util.h"

namespace spot_checker {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

std::string json_str(const bj::object& obj, const char* key) {
    if (const auto* v = obj.if_contains(key); v && v->is_string()) {
        return std::string(v->as_string());
    }
    return {};
}

bool is_excluded_entry_type(const std::string& type) {
    return type == "COMMAND" || type == "INPUT" || type == "other" ||
           type == "label" || type == "name" || type == "NAME";
}

// Extracts "Speaker" if `text` begins with `Speaker\r\n` where `Speaker` is a
// short name header rather than narrative prose.
std::string extract_speaker_prefix(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf == std::string::npos || crlf == 0 || crlf > 35) return {};
    const std::string prefix = trim(text.substr(0, crlf));
    if (prefix.empty() || utf8_char_len(prefix) > 20) return {};
    for (char c : prefix) {
        if (c == '"' || c == '\'' || c == '.' || c == '!' || c == '[' || c == '(') {
            return {};
        }
    }
    // Reject Japanese dialogue brackets or ideographic spaces as speaker prefixes
    if (prefix.rfind("「", 0) == 0 ||
        prefix.rfind("『", 0) == 0 ||
        prefix.rfind("　", 0) == 0 ||
        prefix.rfind("（", 0) == 0) {
        return {};
    }
    return prefix;
}

std::string strip_speaker_prefix_only(const std::string& text) {
    const std::string prefix = extract_speaker_prefix(text);
    if (prefix.empty()) return trim(text);
    const std::size_t crlf = text.find("\r\n");
    return (crlf != std::string::npos) ? trim(text.substr(crlf + 2)) : trim(text);
}

std::string escape_for_prompt(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out += "\\r\\n";
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else if (s[i] == '\n') {
            out += "\\n";
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string strip_markdown_json_fences(const std::string& raw) {
    std::string s = trim(raw);
    if (s.rfind("```", 0) == 0) {
        const std::size_t first_nl = s.find('\n');
        if (first_nl != std::string::npos) {
            s = s.substr(first_nl + 1);
        }
        const std::size_t last_fence = s.rfind("```");
        if (last_fence != std::string::npos) {
            s = s.substr(0, last_fence);
        }
        s = trim(s);
    }
    return s;
}

std::string normalize_restored_translation(const std::string& old_en,
                                           const std::string& candidate_en) {
    std::string raw = candidate_en;
    if (old_en.find("\r\n") != std::string::npos &&
        old_en.find("\\r\\n") == std::string::npos) {
        for (;;) {
            const std::size_t pos = raw.find("\\r\\n");
            if (pos == std::string::npos) break;
            raw.replace(pos, 4, "\r\n");
        }
    }
    std::string clean = sanitize_ascii(trim(raw));
    if (clean.empty()) return {};

    // Restore Speaker\r\n prefix if `old_en` had one and `candidate_en` omitted it
    const std::string old_prefix = extract_speaker_prefix(old_en);
    if (!old_prefix.empty()) {
        const std::string want_crlf = old_prefix + "\r\n";
        const std::string want_lf = old_prefix + "\n";
        if (clean.rfind(want_crlf, 0) == 0) {
            // Already has exact Speaker\r\n
        } else if (clean.rfind(want_lf, 0) == 0) {
            clean = want_crlf + clean.substr(want_lf.size());
        } else {
            clean = want_crlf + clean;
        }
    }

    // Preserve trailing \r\n if `old_en` ended with \r\n
    if (old_en.size() >= 2 &&
        old_en.compare(old_en.size() - 2, 2, "\r\n") == 0) {
        clean = trim_right(clean) + "\r\n";
    }
    return clean;
}

std::size_t count_substring(const std::string& str, const std::string& sub) {
    if (sub.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = str.find(sub, pos)) != std::string::npos) {
        ++count;
        pos += sub.size();
    }
    return count;
}

std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool has_romaji_gloss_or_tl_note(const std::string& s) {
    const std::string lower = to_lower_copy(s);
    if (lower.find("[tl") != std::string::npos ||
        lower.find("(tl") != std::string::npos ||
        lower.find("translator note") != std::string::npos) {
        return true;
    }
    const std::size_t open_p = lower.find('(');
    const std::size_t close_p =
        lower.find(')', open_p == std::string::npos ? 0 : open_p);
    if (open_p != std::string::npos && close_p != std::string::npos &&
        close_p > open_p + 2) {
        const std::string inside =
            trim(lower.substr(open_p + 1, close_p - open_p - 1));
        if (!inside.empty() && inside.size() <= 18 &&
            inside.find(' ') == std::string::npos && s.find('"') != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> extract_alnum_words_lower(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') {
            cur += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            cur += static_cast<char>(c);
        } else if (!cur.empty()) {
            words.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) {
        words.push_back(std::move(cur));
    }
    return words;
}

bool is_passthrough_or_non_jp_entry(const ScriptEntry& e) {
    const std::string jp_body = strip_speaker_prefix_only(e.jp_text);
    const std::string en_body = strip_speaker_prefix_only(e.current_en);
    if (!has_real_japanese(jp_body)) return true;
    if (en_body == jp_body || trim(e.current_en) == trim(e.jp_text)) return true;
    return false;
}

}  // namespace

std::string default_editorial_rules_prompt(bool fix_punctuation) {
    std::string rules =
        "<editorial_mandate>\n"
        "You are a master Japanese-to-English literary translator and senior visual novel editor.\n"
        "Your mission is to spot-check existing English translations against the original Japanese\n"
        "and propose surgical fixes ONLY where the current English has a clear defect.\n\n"
        "THE LITERARY SWEET SPOT (AVOID BOTH EXTREMES):\n"
        "- AVOID EXTREME 1 -- WOODEN MECHANICAL LITERALISM ('TRANSLATIONESE'):\n"
        "  Never force word-for-word Japanese syntax onto English. Fix robotic calques, stiff\n"
        "  clauses ('As for...', 'It is that...'), unnatural noun repetition, and dictionary-literal\n"
        "  dialogue. Take the natural syntactic leeway English needs so dialogue sounds genuinely\n"
        "  spoken and narration reads like published literary fiction.\n"
        "- AVOID EXTREME 2 -- TONE DRIFT & OVER-LOCALIZATION:\n"
        "  Never invent new meanings, inject Western slang/memes, alter the emotional register\n"
        "  (e.g. turning quiet deadpan into loud sarcasm), or rewrite lines that already read\n"
        "  naturally.\n\n"
        "CORE PILLARS (MANDATORY):\n"
        "1. PROFESSIONAL LITERARY PROSE: Every improved line must read like an accomplished English\n"
        "   author wrote it naturally -- vivid rhythm, natural spoken cadence, and sharp subtext.\n"
        "2. STRICTLY AUTHOR-INTENDED: Preserve the Japanese author's exact psychological nuance,\n"
        "   character voice tics (e.g. cat-puns like nya/meow, formal ojou-sama register, rough slang),\n"
        "   and multi-line banter callbacks (when line B echoes a verb/noun from line A in Japanese,\n"
        "   the English pair must preserve that verbal echo so the joke/callback lands).\n"
        "   If current_en includes a short quoted phonetic reading specifically to make an explicit\n"
        "   Japanese wordplay/pun intelligible when the following line reacts to that pun (e.g.\n"
        "   \"'Shokai housou'... 'Shoka, ihou housou'--an early-summer illegal broadcast...\"\n"
        "   followed by \"'let's leave out puns like that'\"), DO NOT strip the pun setup! Only\n"
        "   remove parenthetical translator notes like (hippu) when they are not part of a spoken pun.\n"
        "3. NOT WESTERNIZED: Never domesticate Japanese cultural concepts, honorific dynamics, or\n"
        "   setting details into out-of-place Western internet slang, memes, or Americanized idioms.\n"
        "4. NOT CENSORED: Never soften, bowdlerize, sanitize, or euphemize explicit adult/erotic\n"
        "   content, visceral violence, dark psychological themes, or harsh insults. Match the\n"
        "   exact intensity and register of the Japanese original.\n"
        "5. DO NO HARM (SURGICAL PRECISION): If a line is already accurate, natural, and faithful,\n"
        "   DO NOT touch it. Never make lateral synonym swaps or stylistic edits just for the sake\n"
        "   of changing words. Only output a fix for lines marked [EDITABLE SUSPECT] when there is\n"
        "   an objective improvement with confidence >= 0.85.\n"
        "6. PURE ASCII FORMATTING: Use '--' for em-dashes, '...' for ellipses, and straight ASCII\n"
        "   quotes (\" and '). Preserve outer quotation marks on spoken dialogue. Never include\n"
        "   parenthetical romaji glosses or translator notes.\n";
    if (!fix_punctuation) {
        rules +=
            "7. DO NOT FIX PUNCTUATION, DASHES, ELLIPSES, BRACKETS, OR PARENTHESES: Never output\n"
            "   edits that merely change punctuation ('---' vs '--', '...' vs '......', quotes,\n"
            "   tildes, or parenthetical notes). Focus strictly on substantive sentence\n"
            "   improvements: semantic mistranslations, broken multi-line jokes/callbacks, wrong\n"
            "   omitted pronouns/subjects, dropped character speech tics, and wooden/robotic\n"
            "   sentence phrasing.\n";
    }
    rules +=
        "</editorial_mandate>\n\n"
        "<output_schema>\n"
        "Return ONLY a valid JSON array of objects (or [] if no suspect lines require fixing):\n"
        "[\n"
        "  {\n"
        "    \"id\": <integer 1..N from target_window, MUST be an [EDITABLE SUSPECT] line>,\n"
        "    \"new_en\": \"<the improved English translation>\",\n"
        "    \"error_span\": \"<optional exact substring in current EN that contained the defect>\",\n"
        "    \"severity\": \"<critical|major|minor>\",\n"
        "    \"category\": \"<mistranslation|broken_callback|artifact_or_note|character_voice|censored_or_westernized|stiff_translationese>\",\n"
        "    \"confidence\": <number 0.0 to 1.0, must be >= 0.85>,\n"
        "    \"reason\": \"<concise explanation of the defect fixed>\"\n"
        "  }\n"
        "]\n"
        "</output_schema>";
    return rules;
}

std::string build_system_prompt(const std::string& tone_markdown, bool fix_punctuation) {
    std::string sys = default_editorial_rules_prompt(fix_punctuation);
    if (!trim(tone_markdown).empty()) {
        sys += "\n\n<novel_tone_and_glossary>\n" + trim(tone_markdown) +
               "\n</novel_tone_and_glossary>";
    }
    return sys;
}

std::vector<std::pair<std::string, std::string>> parse_glossary_from_tone_md(
    const std::string& tone_markdown) {
    std::vector<std::pair<std::string, std::string>> out;
    std::unordered_set<std::string> seen_jp;

    auto add_pair = [&](std::string jp, std::string en) {
        jp = trim(jp);
        en = trim(en);
        if (!jp.empty() && !en.empty() && has_japanese(jp) && !has_japanese(en)) {
            if (seen_jp.insert(jp).second) {
                out.emplace_back(std::move(jp), std::move(en));
            }
        }
    };

    std::size_t pos = 0;
    while (pos <= tone_markdown.size()) {
        const std::size_t nl = tone_markdown.find('\n', pos);
        std::string line = tone_markdown.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? tone_markdown.size() + 1 : nl + 1;
        line = trim(line);
        if (line.empty()) continue;

        // Pattern 1: Markdown table row `| `JP` | `EN` |` or `| JP | EN |`
        if (line.front() == '|' && line.back() == '|') {
            const std::size_t p1 = line.find('|', 1);
            if (p1 != std::string::npos && p1 + 1 < line.size()) {
                const std::size_t p2 = line.find('|', p1 + 1);
                if (p2 != std::string::npos) {
                    std::string c1 = trim(line.substr(1, p1 - 1));
                    std::string c2 = trim(line.substr(p1 + 1, p2 - p1 - 1));
                    auto strip_backticks = [](std::string s) {
                        if (s.size() >= 2 && s.front() == '`' && s.back() == '`') {
                            return s.substr(1, s.size() - 2);
                        }
                        return s;
                    };
                    add_pair(strip_backticks(c1), strip_backticks(c2));
                }
            }
            continue;
        }

        // Pattern 2: Comma-separated `JP=EN` items (e.g. inside <name_reference>)
        if (line.find('=') != std::string::npos) {
            std::size_t start = 0;
            while (start < line.size()) {
                const std::size_t comma = line.find(',', start);
                const std::string token = trim(line.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start));
                const std::size_t eq = token.find('=');
                if (eq != std::string::npos) {
                    add_pair(token.substr(0, eq), token.substr(eq + 1));
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }

    return out;
}

std::vector<ScriptEntry> extract_script_entries(const bj::value& root) {
    std::vector<ScriptEntry> out;
    if (!root.is_object()) return out;

    for (const auto& kv : root.as_object()) {
        if (!kv.value().is_object()) continue;
        const std::string script_name(kv.key());
        const auto& script_obj = kv.value().as_object();

        std::string array_key;
        const bj::array* arr_ptr = nullptr;
        if (const auto* s = script_obj.if_contains("strings"); s && s->is_array()) {
            array_key = "strings";
            arr_ptr = &s->as_array();
        } else if (const auto* l = script_obj.if_contains("lines"); l && l->is_array()) {
            array_key = "lines";
            arr_ptr = &l->as_array();
        }
        if (!arr_ptr) continue;

        std::string active_speaker = "NARRATION";
        for (std::size_t i = 0; i < arr_ptr->size(); ++i) {
            if (!(*arr_ptr)[i].is_object()) continue;
            const auto& entry_obj = (*arr_ptr)[i].as_object();
            const std::string type = json_str(entry_obj, "type");

            std::string text_key = "text";
            std::string jp_text = json_str(entry_obj, "text");
            if (jp_text.empty() && entry_obj.if_contains("content")) {
                text_key = "content";
                jp_text = json_str(entry_obj, "content");
            }
            const std::string translated = json_str(entry_obj, "translated");

            // Track standalone speaker definitions (e.g. amakano2pe / SakuraNoUta)
            if (type == "NAME" || type == "name") {
                if (!translated.empty() && translated[0] != '$') {
                    active_speaker = trim(translated);
                } else if (!jp_text.empty() && jp_text[0] != '$') {
                    active_speaker = trim(jp_text);
                } else {
                    active_speaker = "NARRATION";
                }
                continue;
            }
            if (type == "COMMAND" && jp_text == "fw 0") {
                active_speaker = "NARRATION";
                continue;
            }
            if (is_excluded_entry_type(type)) continue;

            if (const auto* hj = entry_obj.if_contains("has_jp");
                hj && hj->is_bool() && !hj->as_bool()) {
                continue;
            }
            if (!has_japanese(jp_text) || translated.empty()) continue;

            std::string speaker = trim(json_str(entry_obj, "speaker"));
            if (speaker.empty()) {
                speaker = extract_speaker_prefix(translated);
            }
            if (speaker.empty()) {
                speaker = extract_speaker_prefix(jp_text);
            }
            if (speaker.empty()) {
                speaker = active_speaker;
            }

            ScriptEntry se;
            se.global_index = out.size();
            se.script_name = script_name;
            se.array_key = array_key;
            se.entry_index = i;
            se.text_key = text_key;
            se.speaker = speaker;
            se.jp_text = jp_text;
            se.current_en = translated;
            out.push_back(std::move(se));
        }
    }
    return out;
}

WindowSlice build_window_slice(const std::vector<ScriptEntry>& entries,
                               std::size_t offset,
                               std::size_t window_size,
                               std::size_t context_before,
                               std::size_t context_after) {
    WindowSlice slice;
    if (entries.empty() || offset >= entries.size() || window_size == 0) {
        slice.target_start = std::min(offset, entries.size());
        return slice;
    }

    slice.target_start = offset;
    const std::size_t target_end = std::min(entries.size(), offset + window_size);
    const std::size_t before_start =
        (offset > context_before) ? (offset - context_before) : 0;
    const std::size_t after_end =
        std::min(entries.size(), target_end + context_after);

    slice.context_before.assign(
        entries.begin() + static_cast<std::ptrdiff_t>(before_start),
        entries.begin() + static_cast<std::ptrdiff_t>(offset));
    slice.target.assign(
        entries.begin() + static_cast<std::ptrdiff_t>(offset),
        entries.begin() + static_cast<std::ptrdiff_t>(target_end));
    slice.context_after.assign(
        entries.begin() + static_cast<std::ptrdiff_t>(target_end),
        entries.begin() + static_cast<std::ptrdiff_t>(after_end));
    return slice;
}

std::set<int> compute_editable_ids(const WindowSlice& slice,
                                   const std::vector<LineWeaknessScore>& novel_scores,
                                   bool review_all_in_window,
                                   bool skip_h_scenes,
                                   const std::set<std::size_t>& repaired_indices) {
    std::set<int> editable;
    const int n = static_cast<int>(slice.target.size());
    if (n == 0) return editable;

    auto is_blocked_line = [&](int id) {
        const auto& e = slice.target[static_cast<std::size_t>(id - 1)];
        if (repaired_indices.count(e.global_index) != 0) return true;
        if (is_passthrough_or_non_jp_entry(e)) return true;
        if (skip_h_scenes && is_h_scene_line(e.jp_text, e.current_en)) return true;
        return false;
    };

    if (review_all_in_window || novel_scores.empty()) {
        for (int id = 1; id <= n; ++id) {
            if (!is_blocked_line(id)) editable.insert(id);
        }
        return editable;
    }

    for (int id = 1; id <= n; ++id) {
        if (is_blocked_line(id)) continue;
        const std::size_t g_idx =
            slice.target[static_cast<std::size_t>(id - 1)].global_index;
        if (g_idx < novel_scores.size() && novel_scores[g_idx].raw_total > 0.0) {
            if (id - 1 >= 1 && !is_blocked_line(id - 1)) editable.insert(id - 1);
            editable.insert(id);
            if (id + 1 <= n && !is_blocked_line(id + 1)) editable.insert(id + 1);
        }
    }
    return editable;
}

std::string build_spot_check_user_prompt(const WindowSlice& slice,
                                         bool fix_punctuation) {
    std::string prompt;

    if (!slice.context_before.empty()) {
        prompt += "<context_before>\n";
        for (const auto& e : slice.context_before) {
            prompt += "[" + e.speaker + "] JP: " + escape_for_prompt(e.jp_text) +
                      " | EN: " + escape_for_prompt(e.current_en) + "\n";
        }
        prompt += "</context_before>\n\n";
    }

    const bool has_lock_list = !slice.editable_ids.empty() &&
                               slice.editable_ids.size() < slice.target.size();

    prompt += "<target_window>\n";
    for (std::size_t i = 0; i < slice.target.size(); ++i) {
        const int id = static_cast<int>(i + 1);
        const auto& e = slice.target[i];
        const bool is_editable =
            slice.editable_ids.empty() || slice.editable_ids.count(id) != 0;
        const char* status_tag =
            has_lock_list
                ? (is_editable ? " [EDITABLE SUSPECT]" : " [READ-ONLY CONTEXT]")
                : " [EDITABLE SUSPECT]";
        prompt += std::to_string(id) + ". [" + e.speaker + "]" + status_tag + "\n";
        prompt += "   JP: " + escape_for_prompt(e.jp_text) + "\n";
        prompt += "   EN: " + escape_for_prompt(e.current_en) + "\n";
    }
    prompt += "</target_window>\n\n";

    if (!slice.context_after.empty()) {
        prompt += "<context_after>\n";
        for (const auto& e : slice.context_after) {
            prompt += "[" + e.speaker + "] JP: " + escape_for_prompt(e.jp_text) +
                      " | EN: " + escape_for_prompt(e.current_en) + "\n";
        }
        prompt += "</context_after>\n\n";
    }

    prompt += "Review the " + std::to_string(slice.target.size()) +
              " numbered lines inside <target_window> using <context_before> and "
              "<context_after> for scene continuity, pronoun resolution, and banter callbacks.\n";
    if (has_lock_list) {
        prompt += "IMPORTANT LOCKING RULE: You may ONLY output fixes for IDs marked "
                  "[EDITABLE SUSPECT] (allowed IDs:";
        for (int id : slice.editable_ids) {
            prompt += " " + std::to_string(id);
        }
        prompt += "). Lines marked [READ-ONLY CONTEXT] are locked for context only.\n";
    }
    if (!fix_punctuation) {
        prompt += "PUNCTUATION NOISE FILTER: Do NOT output fixes that only change "
                  "hyphens/dashes (--- vs --), ellipsis length (... vs ......), quotes, "
                  "or parenthetical notes. Only output substantive lexical, grammatical, "
                  "voice, or discourse repairs.\n";
    }
    prompt += "DO NO HARM: Only return an object when there is an objective defect with "
              "confidence >= 0.85. Output a JSON array following <output_schema>.";
    return prompt;
}

std::string build_radar_pack_prompt(const std::vector<WindowSlice>& passages) {
    std::string prompt =
        "<radar_triage_mandate>\n"
        "STAGE 1 PACKED GEMBA-MQM RADAR PASS (Kocmi & Federmann WMT 2023):\n"
        "Rapidly triage the following candidate visual novel passages for objective JP->EN "
        "translation defects (mistranslation, broken banter callbacks, pronoun/benefactive "
        "reversal, dropped role-language tics, or leftover CJK/romaji artifacts).\n"
        "Score each passage using GEMBA-MQM severity weights: critical = 25, major = 5, "
        "minor = 1 (0 if clean).\n"
        "</radar_triage_mandate>\n\n";

    for (std::size_t p = 0; p < passages.size(); ++p) {
        const auto& slice = passages[p];
        prompt += "<passage id=\"" + std::to_string(p + 1) + "\" offset=\"" +
                  std::to_string(slice.target_start) + "\">\n";
        for (std::size_t i = 0; i < slice.target.size(); ++i) {
            const auto& e = slice.target[i];
            prompt += std::to_string(i + 1) + ". [" + e.speaker + "] JP: " +
                      escape_for_prompt(e.jp_text) + " | EN: " +
                      escape_for_prompt(e.current_en) + "\n";
        }
        prompt += "</passage>\n\n";
    }

    prompt +=
        "<radar_output_schema>\n"
        "Return ONLY a valid JSON array with one entry per passage:\n"
        "[\n"
        "  {\n"
        "    \"passage_id\": <integer 1.." + std::to_string(passages.size()) + ">,\n"
        "    \"mqm_score\": <number: sum of critical*25 + major*5 + minor*1, 0 if clean>,\n"
        "    \"suspect_ids\": [<1-based line IDs inside that passage needing repair>]\n"
        "  }\n"
        "]\n"
        "</radar_output_schema>";
    return prompt;
}

std::vector<RadarPassageResult> parse_radar_pack_response(
    const std::string& raw_response,
    const std::vector<WindowSlice>& passages) {
    std::vector<RadarPassageResult> results;
    if (passages.empty()) return results;

    const std::string cleaned = strip_markdown_json_fences(raw_response);
    if (cleaned.empty()) return results;

    bj::value parsed;
    try {
        parsed = bj::parse(cleaned);
    } catch (...) {
        return results;
    }
    if (!parsed.is_array()) return results;

    std::set<int> seen_passages;
    for (const auto& item_v : parsed.as_array()) {
        if (!item_v.is_object()) continue;
        const auto& obj = item_v.as_object();

        int pid = -1;
        if (const auto* pv = obj.if_contains("passage_id")) {
            if (pv->is_int64()) pid = static_cast<int>(pv->as_int64());
            else if (pv->is_uint64()) pid = static_cast<int>(pv->as_uint64());
        }
        if (pid < 1 || static_cast<std::size_t>(pid) > passages.size()) continue;
        if (!seen_passages.insert(pid).second) continue;

        const WindowSlice& slice = passages[static_cast<std::size_t>(pid - 1)];
        RadarPassageResult rpr;
        rpr.passage_id = pid;
        rpr.offset = slice.target_start;
        rpr.length = slice.target.size();

        if (const auto* mv = obj.if_contains("mqm_score")) {
            if (mv->is_double()) rpr.mqm_score = mv->as_double();
            else if (mv->is_int64()) rpr.mqm_score = static_cast<double>(mv->as_int64());
            else if (mv->is_uint64()) rpr.mqm_score = static_cast<double>(mv->as_uint64());
        } else {
            // Optional fallback if model returned critical/major/minor counts
            double crit = 0.0;
            double maj = 0.0;
            double minr = 0.0;
            if (const auto* c = obj.if_contains("critical"); c && c->is_number()) {
                crit = c->to_number<double>();
            }
            if (const auto* m = obj.if_contains("major"); m && m->is_number()) {
                maj = m->to_number<double>();
            }
            if (const auto* n = obj.if_contains("minor"); n && n->is_number()) {
                minr = n->to_number<double>();
            }
            rpr.mqm_score = 25.0 * crit + 5.0 * maj + 1.0 * minr;
        }

        if (const auto* s_arr = obj.if_contains("suspect_ids");
            s_arr && s_arr->is_array()) {
            const int max_id = static_cast<int>(slice.target.size());
            for (const auto& sv : s_arr->as_array()) {
                int sid = -1;
                if (sv.is_int64()) sid = static_cast<int>(sv.as_int64());
                else if (sv.is_uint64()) sid = static_cast<int>(sv.as_uint64());
                if (sid >= 1 && sid <= max_id) {
                    rpr.suspect_ids.insert(sid);
                }
            }
        }
        results.push_back(std::move(rpr));
    }
    return results;
}

bool is_valid_defect_category(const std::string& category) {
    static const std::unordered_set<std::string> allowed = {
        "mistranslation",
        "broken_callback",
        "artifact_or_note",
        "character_voice",
        "censored_or_westernized",
        "stiff_translationese",
    };
    return allowed.count(category) != 0;
}

ValidationResult validate_proposed_fix(
    const ScriptEntry& entry,
    const std::string& candidate_en,
    const std::vector<std::pair<std::string, std::string>>& glossary,
    const OlsModel& ols,
    const std::string& error_span,
    bool fix_punctuation) {
    ValidationResult res;
    const std::string jp_body = strip_speaker_prefix_only(entry.jp_text);
    const std::string old_body = strip_speaker_prefix_only(entry.current_en);
    const std::string new_body = strip_speaker_prefix_only(candidate_en);

    if (new_body.empty()) {
        res.rejection_reason = "empty translation body";
        return res;
    }

    // Never edit symbol-only / non-Japanese lines or untranslated passthrough lines
    if (is_passthrough_or_non_jp_entry(entry)) {
        res.rejection_reason =
            "passthrough or symbol-only non-Japanese line cannot be edited";
        return res;
    }

    const bool compressed_runaway =
        old_body.size() > 500 && new_body.size() * 2 < old_body.size();

    // When fix_punctuation == false (default), reject edits that only alter
    // punctuation/hyphens/ellipses/quotes while keeping identical alphanumeric words
    if (!fix_punctuation && !compressed_runaway && !has_real_japanese(old_body) &&
        extract_alnum_words_lower(old_body) == extract_alnum_words_lower(new_body)) {
        res.rejection_reason =
            "punctuation-only change rejected (fix_punctuation is false)";
        return res;
    }

    // 0. Stage 2 GEMBA-MQM Span-Grounded Surgical Verification:
    // If the LLM specified an offending `error_span` present in `old_body`,
    // `new_body` must actually modify/remove that span (unless compressing a
    // runaway repetition loop where a short prefix of repeated chars remains).
    const std::string span = trim(error_span);
    if (!compressed_runaway && !span.empty() &&
        old_body.find(span) != std::string::npos &&
        new_body.find(span) != std::string::npos) {
        res.rejection_reason =
            "proposed fix left GEMBA-MQM error_span '" + span + "' unchanged";
        return res;
    }

    // 1. Zero Leftover CJK / Full-Width Punctuation / Romaji Glosses
    if (has_japanese(new_body)) {
        res.rejection_reason = "contains leftover Japanese/CJK characters";
        return res;
    }
    if (new_body.find("「") != std::string::npos ||
        new_body.find("」") != std::string::npos ||
        new_body.find("『") != std::string::npos ||
        new_body.find("』") != std::string::npos ||
        new_body.find("…") != std::string::npos ||
        new_body.find("—") != std::string::npos ||
        new_body.find("。") != std::string::npos ||
        new_body.find("、") != std::string::npos) {
        res.rejection_reason = "contains non-ASCII fullwidth punctuation";
        return res;
    }
    if (has_romaji_gloss_or_tl_note(new_body) && !has_romaji_gloss_or_tl_note(old_body)) {
        res.rejection_reason = "introduces parenthetical romaji gloss or translator note";
        return res;
    }

    // 2. Quote Envelope Invariant
    const bool jp_spoken =
        jp_body.rfind("「", 0) == 0 ||
        jp_body.rfind("『", 0) == 0;
    const bool old_outer_quoted =
        old_body.size() >= 2 && old_body.front() == '"' && old_body.back() == '"';
    const bool new_has_quote = new_body.find('"') != std::string::npos;
    const bool new_outer_quoted =
        new_body.size() >= 2 && new_body.front() == '"' && new_body.back() == '"';

    if ((jp_spoken || old_outer_quoted) && !new_has_quote) {
        res.rejection_reason = "dropped spoken dialogue quotation marks";
        return res;
    }
    if (old_outer_quoted && jp_spoken && !new_outer_quoted) {
        res.rejection_reason = "broke outer quotation mark envelope on spoken dialogue";
        return res;
    }
    const bool jp_has_any_quote =
        jp_body.find("「") != std::string::npos ||
        jp_body.find("『") != std::string::npos;
    const bool old_has_any_quote = old_body.find('"') != std::string::npos;
    if (!jp_has_any_quote && !old_has_any_quote && new_outer_quoted) {
        res.rejection_reason = "invented outer spoken quotes on pure narration";
        return res;
    }

    // 3. Engine Tag / Placeholder Preservation (%s, %d, literal \n escapes)
    for (const char* tag : {"%s", "%d", "\\n"}) {
        if (count_substring(old_body, tag) > count_substring(new_body, tag)) {
            res.rejection_reason = std::string("dropped engine placeholder ") + tag;
            return res;
        }
    }

    // 4. Glossary Invariant Check
    if (!glossary.empty()) {
        const std::string old_lower = to_lower_copy(old_body);
        const std::string new_lower = to_lower_copy(new_body);
        for (const auto& [jp_term, en_term] : glossary) {
            if (jp_term.empty() || en_term.empty()) continue;
            if (entry.jp_text.find(jp_term) != std::string::npos) {
                const std::string en_lower = to_lower_copy(en_term);
                if (old_lower.find(en_lower) != std::string::npos &&
                    new_lower.find(en_lower) == std::string::npos) {
                    res.rejection_reason =
                        "dropped canonical glossary term '" + en_term + "' for '" + jp_term + "'";
                    return res;
                }
            }
        }
    }

    // 5. Strict Mathematical Improvement Check (Do-No-Harm non-regression)
    HotspotConfig hcfg;
    hcfg.fix_punctuation = fix_punctuation;
    HotspotDetector detector(hcfg);
    const LineWeaknessScore old_score =
        detector.score_single_line(entry.jp_text, entry.current_en, ols);
    const LineWeaknessScore new_score =
        detector.score_single_line(entry.jp_text, candidate_en, ols);
    if (new_score.raw_total > old_score.raw_total + 1e-6) {
        res.rejection_reason = "mathematical weakness score worsened (" +
                               std::to_string(new_score.raw_total) + " > " +
                               std::to_string(old_score.raw_total) + ")";
        return res;
    }

    res.accepted = true;
    return res;
}

std::vector<Improvement> parse_spot_check_response(
    const std::string& raw_response,
    const WindowSlice& slice,
    double min_confidence,
    const std::vector<std::pair<std::string, std::string>>& glossary,
    const OlsModel& ols,
    std::size_t* out_rejected_count,
    bool fix_punctuation) {
    std::vector<Improvement> improvements;
    if (out_rejected_count) *out_rejected_count = 0;
    if (slice.target.empty()) return improvements;

    const std::string cleaned = strip_markdown_json_fences(raw_response);
    if (cleaned.empty()) return improvements;

    bj::value parsed;
    try {
        parsed = bj::parse(cleaned);
    } catch (...) {
        return improvements;
    }
    if (!parsed.is_array()) return improvements;

    std::set<int> seen_ids;
    for (const auto& item_v : parsed.as_array()) {
        if (!item_v.is_object()) continue;
        const auto& obj = item_v.as_object();

        int id = -1;
        if (const auto* id_v = obj.if_contains("id")) {
            if (id_v->is_int64()) id = static_cast<int>(id_v->as_int64());
            else if (id_v->is_uint64()) id = static_cast<int>(id_v->as_uint64());
        }
        if (id < 1 || static_cast<std::size_t>(id) > slice.target.size()) {
            if (out_rejected_count) ++(*out_rejected_count);
            continue;
        }

        // Trick 1: Enforce editable_ids allowlist when populated
        if (!slice.editable_ids.empty() && slice.editable_ids.count(id) == 0) {
            if (out_rejected_count) ++(*out_rejected_count);
            continue;
        }
        if (!seen_ids.insert(id).second) continue;

        // Trick 2: Defect category taxonomy & confidence gate
        std::string category = "mistranslation";
        if (const auto* cat_v = obj.if_contains("category"); cat_v && cat_v->is_string()) {
            category = trim(std::string(cat_v->as_string()));
            if (!is_valid_defect_category(category)) {
                if (out_rejected_count) ++(*out_rejected_count);
                continue;
            }
        }

        std::string severity = trim(json_str(obj, "severity"));
        if (severity != "critical" && severity != "major" && severity != "minor") {
            severity = "major";
        }
        const std::string error_span = trim(json_str(obj, "error_span"));

        double confidence = 1.0;
        if (const auto* conf_v = obj.if_contains("confidence")) {
            if (conf_v->is_double()) confidence = conf_v->as_double();
            else if (conf_v->is_int64()) confidence = static_cast<double>(conf_v->as_int64());
            else if (conf_v->is_uint64()) confidence = static_cast<double>(conf_v->as_uint64());
        }
        if (confidence < min_confidence) {
            if (out_rejected_count) ++(*out_rejected_count);
            continue;
        }

        std::string raw_improved = json_str(obj, "new_en");
        if (raw_improved.empty()) {
            raw_improved = json_str(obj, "improved_en");
        }
        const std::string reason = trim(json_str(obj, "reason"));
        const ScriptEntry& target_entry =
            slice.target[static_cast<std::size_t>(id - 1)];

        const std::string normalized =
            normalize_restored_translation(target_entry.current_en, raw_improved);
        if (normalized.empty() || normalized == target_entry.current_en) {
            continue;
        }

        // Trick 3: Deterministic C++ Do-No-Harm post-edit validator + GEMBA-MQM error_span check
        const ValidationResult val = validate_proposed_fix(
            target_entry, normalized, glossary, ols, error_span, fix_punctuation);
        if (!val.accepted) {
            if (out_rejected_count) ++(*out_rejected_count);
            continue;
        }

        Improvement imp;
        imp.global_index = target_entry.global_index;
        imp.script_name = target_entry.script_name;
        imp.entry_index = target_entry.entry_index;
        imp.speaker = target_entry.speaker;
        imp.jp_text = target_entry.jp_text;
        imp.old_en = target_entry.current_en;
        imp.improved_en = normalized;
        imp.category = category;
        imp.severity = severity;
        imp.error_span = error_span;
        imp.confidence = confidence;
        imp.reason = reason;
        improvements.push_back(std::move(imp));
    }
    return improvements;
}

std::vector<std::pair<std::string, std::string>> load_ordered_cache(
    const std::string& cache_path) {
    std::vector<std::pair<std::string, std::string>> items;
    if (!fs::exists(fs::u8path(cache_path))) return items;
    const bj::value root = json_parse_file(cache_path);
    if (!root.is_object()) {
        throw std::runtime_error("cache file is not a JSON object: " + cache_path);
    }
    items.reserve(root.as_object().size());
    for (const auto& kv : root.as_object()) {
        if (kv.value().is_string()) {
            items.emplace_back(std::string(kv.key()),
                               std::string(kv.value().as_string()));
        }
    }
    return items;
}

void save_ordered_cache(
    const std::vector<std::pair<std::string, std::string>>& items,
    const std::string& cache_path) {
    bj::object obj;
    for (const auto& [jp, en] : items) {
        obj[jp] = en;
    }
    write_json_preserving_style(cache_path, bj::value(std::move(obj)), 2, true);
}

struct SpotChecker::Impl {
    SpotCheckConfig cfg;
    ModelCallback callback;
    std::string tone_md;
    std::string sys_prompt;
    std::vector<std::pair<std::string, std::string>> glossary;
    std::vector<ScriptEntry> script_entries;
    OlsModel ols_model;
    ApplyReport checkpoint_applied_;

    Impl(SpotCheckConfig c, ModelCallback cb)
        : cfg(std::move(c)), callback(std::move(cb)) {
        if (cfg.window_size == 0) cfg.window_size = 40;
        if (cfg.out_path.empty()) {
            cfg.out_path =
                (fs::u8path(cfg.game_dir) / "script_output" / "argon_spot_check.json")
                    .u8string();
        }

        const fs::path tone_path = fs::u8path(cfg.game_dir) / "TONE_OF_NOVEL.md";
        if (fs::exists(tone_path)) {
            tone_md = read_file_text(tone_path.u8string());
        }
        sys_prompt = build_system_prompt(tone_md, cfg.fix_punctuation);
        glossary = parse_glossary_from_tone_md(tone_md);

        const fs::path translated_path =
            fs::u8path(cfg.game_dir) / "script_output" / "translated_text.json";
        if (fs::exists(translated_path)) {
            const bj::value root = json_parse_file(translated_path.u8string());
            script_entries = extract_script_entries(root);
            HotspotConfig hcfg;
            hcfg.skip_h_scenes = cfg.skip_h_scenes;
            hcfg.fix_punctuation = cfg.fix_punctuation;
            HotspotDetector det(hcfg);
            ols_model = det.fit_length_regression(script_entries);
        }
    }

    std::string invoke_model(const std::string& user_prompt) {
        if (callback) {
            return callback(sys_prompt, user_prompt);
        }
        ArgonClient client;
        const ArgonResponse resp = client.generate(sys_prompt, user_prompt);
        return resp.text;
    }

    std::map<std::size_t, Improvement> load_existing_improvements() const {
        std::map<std::size_t, Improvement> map;
        if (!fs::exists(fs::u8path(cfg.out_path))) return map;
        try {
            const bj::value v = json_parse_file(cfg.out_path);
            if (!v.is_object()) return map;
            const auto* arr = v.as_object().if_contains("improvements");
            if (!arr || !arr->is_array()) return map;
            for (const auto& item : arr->as_array()) {
                if (!item.is_object()) continue;
                const auto& o = item.as_object();
                Improvement imp;
                if (const auto* gi = o.if_contains("global_index"); gi && gi->is_int64()) {
                    imp.global_index = static_cast<std::size_t>(gi->as_int64());
                }
                if (const auto* ei = o.if_contains("entry_index"); ei && ei->is_int64()) {
                    imp.entry_index = static_cast<std::size_t>(ei->as_int64());
                }
                imp.script_name = json_str(o, "script");
                imp.speaker = json_str(o, "speaker");
                imp.jp_text = json_str(o, "jp");
                imp.old_en = json_str(o, "old_en");
                imp.improved_en = json_str(o, "new_en");
                if (imp.improved_en.empty()) {
                    imp.improved_en = json_str(o, "improved_en");
                }
                imp.category = json_str(o, "category");
                if (imp.category.empty()) imp.category = "mistranslation";
                imp.severity = json_str(o, "severity");
                if (imp.severity.empty()) imp.severity = "major";
                imp.error_span = json_str(o, "error_span");
                if (const auto* cf = o.if_contains("confidence")) {
                    if (cf->is_double()) imp.confidence = cf->as_double();
                    else if (cf->is_int64()) imp.confidence = static_cast<double>(cf->as_int64());
                }
                imp.reason = json_str(o, "reason");
                if (!imp.improved_en.empty()) {
                    map[imp.global_index] = std::move(imp);
                }
            }
        } catch (...) {
        }
        return map;
    }

    std::set<std::size_t> load_existing_checked_offsets() const {
        std::set<std::size_t> offsets;
        if (!fs::exists(fs::u8path(cfg.out_path))) return offsets;
        try {
            const bj::value v = json_parse_file(cfg.out_path);
            if (!v.is_object()) return offsets;
            const auto* arr = v.as_object().if_contains("checked_offsets");
            if (!arr || !arr->is_array()) return offsets;
            for (const auto& item : arr->as_array()) {
                if (item.is_int64() && item.as_int64() >= 0) {
                    offsets.insert(static_cast<std::size_t>(item.as_int64()));
                } else if (item.is_uint64()) {
                    offsets.insert(static_cast<std::size_t>(item.as_uint64()));
                }
            }
        } catch (...) {
        }
        return offsets;
    }

    void save_report_file(const SpotCheckReport& rep) const {
        bj::object root;
        root["game_dir"] = cfg.game_dir;
        root["model"] = "gemini-4-argon";
        root["mode"] = (cfg.mode == SpotCheckMode::Hotspot) ? "hotspot" : "linear";
        root["total_entries"] = static_cast<std::int64_t>(rep.total_entries);
        root["hotspots_detected"] = static_cast<std::int64_t>(rep.hotspots_detected);
        root["h_windows_skipped"] = static_cast<std::int64_t>(rep.h_windows_skipped);
        root["radar_calls_made"] = static_cast<std::int64_t>(rep.radar_calls_made);
        root["radar_passages_triaged"] =
            static_cast<std::int64_t>(rep.radar_passages_triaged);
        root["adaptive_expansions"] = static_cast<std::int64_t>(rep.adaptive_expansions);
        root["windows_checked"] = static_cast<std::int64_t>(rep.windows_checked);
        root["lines_checked"] = static_cast<std::int64_t>(rep.lines_checked);
        root["editable_lines_unlocked"] =
            static_cast<std::int64_t>(rep.editable_lines_unlocked);
        root["candidates_rejected_by_validator"] =
            static_cast<std::int64_t>(rep.candidates_rejected_by_validator);
        root["improvements_count"] = static_cast<std::int64_t>(rep.improvements.size());
        root["script_entries_updated"] =
            static_cast<std::int64_t>(rep.script_entries_updated);
        root["cache_entries_updated"] =
            static_cast<std::int64_t>(rep.cache_entries_updated);

        bj::array co_arr;
        for (std::size_t off : rep.checked_offsets) {
            co_arr.push_back(static_cast<std::int64_t>(off));
        }
        root["checked_offsets"] = std::move(co_arr);

        bj::array hs_arr;
        for (const auto& hw : rep.ranked_hotspots) {
            hs_arr.push_back(bj::object{
                {"offset", static_cast<std::int64_t>(hw.offset)},
                {"length", static_cast<std::int64_t>(hw.length)},
                {"energy", hw.energy},
                {"z_score", hw.z_score},
                {"cusum_peak", hw.cusum_peak},
                {"peak_index", static_cast<std::int64_t>(hw.peak_index)},
                {"peak_line_score", hw.peak_line_score},
                {"dominant_signal", hw.dominant_signal},
            });
        }
        root["ranked_hotspots"] = std::move(hs_arr);

        bj::array imp_arr;
        for (const auto& imp : rep.improvements) {
            imp_arr.push_back(bj::object{
                {"global_index", static_cast<std::int64_t>(imp.global_index)},
                {"script", imp.script_name},
                {"entry_index", static_cast<std::int64_t>(imp.entry_index)},
                {"speaker", imp.speaker},
                {"jp", imp.jp_text},
                {"old_en", imp.old_en},
                {"improved_en", imp.improved_en},
                {"new_en", imp.improved_en},
                {"error_span", imp.error_span},
                {"severity", imp.severity},
                {"category", imp.category},
                {"confidence", imp.confidence},
                {"reason", imp.reason},
            });
        }
        root["improvements"] = std::move(imp_arr);
        write_file_atomic(cfg.out_path, json_pretty(bj::value(std::move(root)), 2));
    }

    ApplyReport sync_improvements_to_disk(
        const std::map<std::size_t, Improvement>& improvements_map) {
        ApplyReport report;
        report.improvements_loaded = improvements_map.size();
        if (improvements_map.empty()) return report;

        const std::string translated_path =
            (fs::u8path(cfg.game_dir) / "script_output" / "translated_text.json")
                .u8string();
        if (!fs::exists(fs::u8path(translated_path))) return report;

        bj::value root = json_parse_file(translated_path);
        if (!root.is_object()) {
            throw std::runtime_error("translated_text.json is not a JSON object");
        }
        auto& root_obj = root.as_object();

        std::unordered_map<std::string, std::string> jp_to_improved;
        for (const auto& [g_idx, imp] : improvements_map) {
            jp_to_improved[imp.jp_text] = imp.improved_en;
            if (g_idx < script_entries.size()) {
                script_entries[g_idx].current_en = imp.improved_en;
            }
            if (auto* script_v = root_obj.if_contains(imp.script_name);
                script_v && script_v->is_object()) {
                auto& script_obj = script_v->as_object();
                bj::array* arr = nullptr;
                if (auto* s = script_obj.if_contains("strings"); s && s->is_array()) {
                    arr = &s->as_array();
                } else if (auto* l = script_obj.if_contains("lines"); l && l->is_array()) {
                    arr = &l->as_array();
                }
                if (arr && imp.entry_index < arr->size() &&
                    (*arr)[imp.entry_index].is_object()) {
                    auto& entry_obj = (*arr)[imp.entry_index].as_object();
                    if (json_str(entry_obj, "translated") != imp.improved_en) {
                        entry_obj["translated"] = imp.improved_en;
                        ++report.script_entries_updated;
                    }
                }
            }
        }

        // Also update any duplicate occurrences of the same JP string across scripts
        for (auto& kv : root_obj) {
            if (!kv.value().is_object()) continue;
            auto& script_obj = kv.value().as_object();
            bj::array* arr = nullptr;
            if (auto* s = script_obj.if_contains("strings"); s && s->is_array()) {
                arr = &s->as_array();
            } else if (auto* l = script_obj.if_contains("lines"); l && l->is_array()) {
                arr = &l->as_array();
            }
            if (!arr) continue;
            for (auto& ev : *arr) {
                if (!ev.is_object()) continue;
                auto& entry_obj = ev.as_object();
                std::string jp = json_str(entry_obj, "text");
                if (jp.empty()) jp = json_str(entry_obj, "content");
                auto it = jp_to_improved.find(jp);
                if (it != jp_to_improved.end() &&
                    json_str(entry_obj, "translated") != it->second) {
                    entry_obj["translated"] = it->second;
                    ++report.script_entries_updated;
                }
            }
        }

        if (report.script_entries_updated > 0) {
            write_json_preserving_style(translated_path, root, 1, true);
        }

        // Update translation_cache_anthropic.json and translation_cache_gemini.json
        // preserving exact insertion order and NEVER inserting new keys!
        for (const char* cache_name : {"translation_cache_anthropic.json",
                                       "translation_cache_gemini.json"}) {
            const std::string cache_path =
                (fs::u8path(cfg.game_dir) / "script_output" / cache_name).u8string();
            if (!fs::exists(fs::u8path(cache_path))) continue;
            auto cache_items = load_ordered_cache(cache_path);
            std::unordered_map<std::string, std::size_t> key_index;
            key_index.reserve(cache_items.size());
            for (std::size_t i = 0; i < cache_items.size(); ++i) {
                key_index[cache_items[i].first] = i;
            }
            bool cache_dirty = false;
            for (const auto& [jp, new_en] : jp_to_improved) {
                auto it = key_index.find(jp);
                if (it != key_index.end()) {
                    if (cache_items[it->second].second != new_en) {
                        cache_items[it->second].second = new_en;
                        ++report.cache_entries_updated;
                        cache_dirty = true;
                    }
                }
            }
            if (cache_dirty) {
                save_ordered_cache(cache_items, cache_path);
            }
        }

        return report;
    }

    static bool is_h_scene_window(const WindowSlice& slice,
                                  std::size_t max_h_lines = 3) {
        std::size_t h_count = 0;
        for (const auto& e : slice.target) {
            if (e.current_en.size() > 500 &&
                e.current_en.size() > 4 * std::max<std::size_t>(1, e.jp_text.size())) {
                return false;
            }
            if (is_h_scene_line(e.jp_text, e.current_en)) {
                ++h_count;
            }
        }
        return h_count >= max_h_lines;
    }

    SpotCheckReport run() {
        SpotCheckReport rep;
        checkpoint_applied_ = {};
        rep.total_entries = script_entries.size();
        if (script_entries.empty()) {
            save_report_file(rep);
            return rep;
        }

        std::map<std::size_t, Improvement> merged = load_existing_improvements();
        std::set<std::size_t> visited_offsets = load_existing_checked_offsets();
        std::set<std::size_t> repaired_indices;
        for (const auto& [g_idx, _] : merged) {
            repaired_indices.insert(g_idx);
        }

        HotspotConfig hcfg;
        hcfg.window_size = cfg.window_size;
        hcfg.z_threshold = cfg.z_threshold;
        hcfg.skip_h_scenes = cfg.skip_h_scenes;
        hcfg.fix_punctuation = cfg.fix_punctuation;
        HotspotDetector detector(hcfg);

        std::vector<LineWeaknessScore> novel_scores;
        std::deque<std::pair<std::size_t, std::size_t>> work_queue;
        std::unordered_map<std::size_t, std::set<int>> radar_suspect_overrides;

        const bool live_log = !static_cast<bool>(callback);

        if (cfg.mode == SpotCheckMode::Hotspot) {
            rep.ranked_hotspots =
                detector.select_hotspots(script_entries, 0, &novel_scores);
            rep.hotspots_detected = rep.ranked_hotspots.size();
            for (const auto& hw : rep.ranked_hotspots) {
                if (visited_offsets.count(hw.offset) == 0) {
                    work_queue.emplace_back(hw.offset, hw.length);
                }
            }
            if (live_log) {
                log_info("Hotspot scan complete: " +
                         std::to_string(rep.hotspots_detected) +
                         " candidate windows (" +
                         std::to_string(visited_offsets.size()) +
                         " previously checked, " +
                         std::to_string(repaired_indices.size()) +
                         " lines already repaired)");
            }
        } else {
            novel_scores = detector.score_entries(script_entries);
            for (std::size_t off = cfg.offset; off < script_entries.size();
                 off += cfg.window_size) {
                if (visited_offsets.count(off) != 0) continue;
                const std::size_t len =
                    std::min(cfg.window_size, script_entries.size() - off);
                work_queue.emplace_back(off, len);
            }
        }

        // Stage 1 Packed LLM Radar Pass (Kocmi & Federmann WMT 2023):
        // Pack up to 6 candidate passages per Radar API call, skip clean passages
        // (mqm_score == 0), and prioritize high-MQM passages for Stage 2 repair.
        if (cfg.radar_calls > 0 && !work_queue.empty()) {
            constexpr std::size_t kPassagesPerRadarCall = 6;
            std::vector<RadarPassageResult> flagged_passages;

            for (std::size_t r = 0; r < cfg.radar_calls && !work_queue.empty(); ++r) {
                std::vector<WindowSlice> pack;
                pack.reserve(kPassagesPerRadarCall);
                while (pack.size() < kPassagesPerRadarCall && !work_queue.empty()) {
                    const auto [off, len] = work_queue.front();
                    work_queue.pop_front();
                    if (visited_offsets.count(off) != 0) continue;
                    WindowSlice ws = build_window_slice(script_entries, off, len, 0, 0);
                    if (cfg.skip_h_scenes && is_h_scene_window(ws)) {
                        visited_offsets.insert(off);
                        ++rep.h_windows_skipped;
                        continue;
                    }
                    if (!ws.target.empty()) {
                        pack.push_back(std::move(ws));
                    }
                }
                if (pack.empty()) break;

                if (live_log) {
                    log_info("[Radar " + std::to_string(r + 1) + "/" +
                             std::to_string(cfg.radar_calls) + "] Triaging " +
                             std::to_string(pack.size()) + " candidate passages...");
                }
                const std::string radar_prompt = build_radar_pack_prompt(pack);
                const std::string raw_radar = invoke_model(radar_prompt);
                ++rep.radar_calls_made;
                rep.radar_passages_triaged += pack.size();

                const auto batch_res = parse_radar_pack_response(raw_radar, pack);
                std::set<std::size_t> flagged_in_batch;
                for (const auto& rpr : batch_res) {
                    if (rpr.mqm_score > 0.0 || !rpr.suspect_ids.empty()) {
                        flagged_in_batch.insert(rpr.offset);
                        flagged_passages.push_back(rpr);
                        if (!rpr.suspect_ids.empty()) {
                            radar_suspect_overrides[rpr.offset] = rpr.suspect_ids;
                        }
                    }
                }
                // Any passage in `pack` that Radar marked clean (mqm_score == 0 and
                // suspect_ids.empty()) is recorded in visited_offsets so future runs
                // do not re-triage it.
                for (const auto& ws : pack) {
                    if (flagged_in_batch.count(ws.target_start) == 0) {
                        visited_offsets.insert(ws.target_start);
                    }
                }
            }

            // Sort surviving Radar-flagged passages by MQM score descending and push
            // to the front of work_queue (clean triaged passages with mqm_score == 0
            // remain dropped, saving Stage 2 calls).
            std::sort(flagged_passages.begin(), flagged_passages.end(),
                      [](const RadarPassageResult& a, const RadarPassageResult& b) {
                          if (std::abs(a.mqm_score - b.mqm_score) > 1e-9) {
                              return a.mqm_score > b.mqm_score;
                          }
                          return a.offset < b.offset;
                      });
            for (auto it = flagged_passages.rbegin(); it != flagged_passages.rend(); ++it) {
                work_queue.emplace_front(it->offset, it->length);
            }
        }

        while (!work_queue.empty()) {
            if (cfg.max_windows > 0 && rep.windows_checked >= cfg.max_windows) {
                break;
            }
            const auto [cur_offset, cur_len] = work_queue.front();
            work_queue.pop_front();

            if (!visited_offsets.insert(cur_offset).second) continue;

            WindowSlice slice = build_window_slice(
                script_entries, cur_offset, cur_len, cfg.context_before, cfg.context_after);
            if (slice.target.empty()) continue;

            if (cfg.skip_h_scenes && is_h_scene_window(slice)) {
                ++rep.h_windows_skipped;
                continue;
            }

            slice.editable_ids = compute_editable_ids(slice,
                                                      novel_scores,
                                                      cfg.review_all_in_window,
                                                      cfg.skip_h_scenes,
                                                      repaired_indices);
            if (auto it = radar_suspect_overrides.find(cur_offset);
                it != radar_suspect_overrides.end() && !cfg.review_all_in_window) {
                const int max_id = static_cast<int>(slice.target.size());
                for (int sid : it->second) {
                    for (int cand : {sid - 1, sid, sid + 1}) {
                        if (cand >= 1 && cand <= max_id) {
                            const auto& ce = slice.target[static_cast<std::size_t>(cand - 1)];
                            if (repaired_indices.count(ce.global_index) == 0 &&
                                !is_passthrough_or_non_jp_entry(ce) &&
                                (!cfg.skip_h_scenes ||
                                 !is_h_scene_line(ce.jp_text, ce.current_en))) {
                                slice.editable_ids.insert(cand);
                            }
                        }
                    }
                }
            }
            rep.editable_lines_unlocked += slice.editable_ids.size();

            // If Pinpointed Suspect-Only Locking finds zero mathematical suspects in
            // this window (and review_all_in_window is false), skip the API call!
            if (!cfg.review_all_in_window && slice.editable_ids.empty()) {
                continue;
            }

            const std::string user_prompt =
                build_spot_check_user_prompt(slice, cfg.fix_punctuation);
            const std::string raw_resp = invoke_model(user_prompt);

            std::size_t rejected = 0;
            const std::vector<Improvement> found = parse_spot_check_response(
                raw_resp,
                slice,
                cfg.min_confidence,
                glossary,
                ols_model,
                &rejected,
                cfg.fix_punctuation);
            rep.candidates_rejected_by_validator += rejected;

            std::vector<Improvement> accepted_new;
            for (const auto& imp : found) {
                if (repaired_indices.count(imp.global_index) != 0) {
                    ++rep.candidates_rejected_by_validator;
                    continue;
                }
                merged[imp.global_index] = imp;
                repaired_indices.insert(imp.global_index);
                accepted_new.push_back(imp);
            }

            ++rep.windows_checked;
            rep.lines_checked += slice.target.size();

            // Hybrid Adaptive Contagion: expand to adjacent windows when defect density is high
            if (cfg.mode == SpotCheckMode::Hotspot && !accepted_new.empty()) {
                const NeighborExpansion exp = detector.evaluate_contagion(
                    cur_offset, slice.target.size(), script_entries.size(), accepted_new);
                if (exp.expand_right && visited_offsets.count(exp.right_offset) == 0) {
                    work_queue.emplace_front(exp.right_offset, exp.right_length);
                    ++rep.adaptive_expansions;
                }
                if (exp.expand_left && visited_offsets.count(exp.left_offset) == 0) {
                    work_queue.emplace_front(exp.left_offset, exp.left_length);
                    ++rep.adaptive_expansions;
                }
            }

            if (cfg.auto_apply && !accepted_new.empty()) {
                const ApplyReport step_app = sync_improvements_to_disk(merged);
                checkpoint_applied_.improvements_loaded = step_app.improvements_loaded;
                checkpoint_applied_.script_entries_updated +=
                    step_app.script_entries_updated;
                checkpoint_applied_.cache_entries_updated +=
                    step_app.cache_entries_updated;
                rep.script_entries_updated = checkpoint_applied_.script_entries_updated;
                rep.cache_entries_updated = checkpoint_applied_.cache_entries_updated;
            }

            rep.checked_offsets.assign(visited_offsets.begin(), visited_offsets.end());
            rep.improvements.clear();
            rep.improvements.reserve(merged.size());
            for (const auto& [idx, imp] : merged) {
                rep.improvements.push_back(imp);
            }
            rep.improvements_found = rep.improvements.size();
            save_report_file(rep);

            if (live_log) {
                char pbuf[256];
                if (cfg.max_windows > 0) {
                    const double pct =
                        100.0 * static_cast<double>(rep.windows_checked) /
                        static_cast<double>(cfg.max_windows);
                    std::snprintf(
                        pbuf, sizeof(pbuf),
                        "[Window %zu/%zu (%.1f%%)] offset=%zu unlocked=%zu -> +%zu fixes (total=%zu)",
                        rep.windows_checked, cfg.max_windows, pct, cur_offset,
                        slice.editable_ids.size(), accepted_new.size(),
                        rep.improvements_found);
                } else {
                    std::snprintf(
                        pbuf, sizeof(pbuf),
                        "[Window %zu] offset=%zu unlocked=%zu -> +%zu fixes (total=%zu)",
                        rep.windows_checked, cur_offset, slice.editable_ids.size(),
                        accepted_new.size(), rep.improvements_found);
                }
                log_info(pbuf);
            }
        }

        if (cfg.auto_apply && !merged.empty()) {
            const ApplyReport final_app = sync_improvements_to_disk(merged);
            checkpoint_applied_.improvements_loaded = final_app.improvements_loaded;
            checkpoint_applied_.script_entries_updated +=
                final_app.script_entries_updated;
            checkpoint_applied_.cache_entries_updated +=
                final_app.cache_entries_updated;
            rep.script_entries_updated = checkpoint_applied_.script_entries_updated;
            rep.cache_entries_updated = checkpoint_applied_.cache_entries_updated;
        }

        rep.checked_offsets.assign(visited_offsets.begin(), visited_offsets.end());
        rep.improvements.clear();
        rep.improvements.reserve(merged.size());
        for (const auto& [idx, imp] : merged) {
            rep.improvements.push_back(imp);
        }
        rep.improvements_found = rep.improvements.size();
        save_report_file(rep);
        return rep;
    }

    ApplyReport apply_improvements() {
        const auto improvements_map = load_existing_improvements();
        ApplyReport report = sync_improvements_to_disk(improvements_map);
        report.script_entries_updated += checkpoint_applied_.script_entries_updated;
        report.cache_entries_updated += checkpoint_applied_.cache_entries_updated;
        checkpoint_applied_ = {};
        return report;
    }
};

SpotChecker::SpotChecker(SpotCheckConfig config, ModelCallback callback)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(callback))) {}

SpotChecker::~SpotChecker() = default;
SpotChecker::SpotChecker(SpotChecker&& other) noexcept = default;
SpotChecker& SpotChecker::operator=(SpotChecker&& other) noexcept = default;

SpotCheckReport SpotChecker::run() {
    return impl_->run();
}

ApplyReport SpotChecker::apply_improvements() {
    return impl_->apply_improvements();
}

const std::vector<ScriptEntry>& SpotChecker::entries() const {
    return impl_->script_entries;
}

const std::string& SpotChecker::system_prompt() const {
    return impl_->sys_prompt;
}

std::string SpotChecker::output_json_path() const {
    return impl_->cfg.out_path;
}

}  // namespace spot_checker
