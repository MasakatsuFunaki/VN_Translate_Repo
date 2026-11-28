// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/prompt_codec.h"

#include "common/utf8_text.h"
#include "domain/vn_line_syntax.h"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

namespace bj = boost::json;

constexpr std::string_view kMarkdownFence = "```";
constexpr std::string_view kJsonLangTag = "json";
constexpr std::string_view kNarrationSpeaker = "NARRATION";
constexpr std::string_view kRedactedPlaceholder = "[REDACTED EXPLICIT SCENE LINE]";
constexpr std::string_view kDefaultSeverity = "major";
constexpr double kMaxConfidence = 1.0;

std::string escape_for_prompt(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out += "\\r\\n";
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            continue;
        }
        if (text[i] == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string_view unwrap_markdown_json_fence(std::string_view raw) noexcept {
    const std::string_view trimmed = trim_ascii_view(raw);
    if (trimmed.size() < kMarkdownFence.size() ||
        trimmed.substr(0, kMarkdownFence.size()) != kMarkdownFence) {
        return trimmed;
    }

    std::string_view rest = trimmed.substr(kMarkdownFence.size());
    const std::size_t first_nl = rest.find('\n');
    if (first_nl == std::string_view::npos) {
        if (rest.size() < kMarkdownFence.size() ||
            rest.substr(rest.size() - kMarkdownFence.size()) != kMarkdownFence) {
            return {};
        }
        rest = trim_ascii_view(rest.substr(0, rest.size() - kMarkdownFence.size()));
        if (rest.size() >= kJsonLangTag.size() &&
            to_lower_ascii(rest.substr(0, kJsonLangTag.size())) == kJsonLangTag) {
            const std::string_view after_tag = rest.substr(kJsonLangTag.size());
            if (after_tag.empty() || after_tag.front() == ' ' ||
                after_tag.front() == '\t' || after_tag.front() == '[' ||
                after_tag.front() == '{') {
                return trim_ascii_view(after_tag);
            }
        }
        return rest;
    }

    rest = trim_ascii_view(rest.substr(first_nl + 1));
    if (rest.size() < kMarkdownFence.size() ||
        rest.substr(rest.size() - kMarkdownFence.size()) != kMarkdownFence) {
        return {};
    }
    return trim_ascii_view(rest.substr(0, rest.size() - kMarkdownFence.size()));
}

Result<bj::array> parse_json_array_strict(std::string_view raw_response,
                                          std::string_view context_label) {
    const std::string_view payload = unwrap_markdown_json_fence(raw_response);
    if (payload.empty()) {
        return Result<bj::array>::failure(
            ErrorCode::CorruptJson,
            std::string(context_label) + ": empty or unclosed JSON payload");
    }

    boost::system::error_code ec;
    bj::value parsed =
        bj::parse(bj::string_view(payload.data(), payload.size()), ec);
    if (ec) {
        return Result<bj::array>::failure(
            ErrorCode::CorruptJson,
            std::string(context_label) + ": JSON parse error: " + ec.message());
    }
    if (!parsed.is_array()) {
        return Result<bj::array>::failure(
            ErrorCode::CorruptJson,
            std::string(context_label) + ": root JSON value is not an array");
    }
    return Result<bj::array>::success(std::move(parsed.as_array()));
}

bool extract_exact_int(const bj::value& val, int& out) noexcept {
    constexpr std::int64_t kMinInt = std::numeric_limits<int>::min();
    constexpr std::int64_t kMaxInt = std::numeric_limits<int>::max();
    if (val.is_int64()) {
        const std::int64_t v = val.as_int64();
        if (v < kMinInt || v > kMaxInt) {
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }
    if (val.is_uint64()) {
        const std::uint64_t v = val.as_uint64();
        if (v > static_cast<std::uint64_t>(kMaxInt)) {
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }
    return false;
}

bool extract_finite_double(const bj::value& val, double& out) noexcept {
    if (val.is_double()) {
        out = val.as_double();
        return std::isfinite(out);
    }
    if (val.is_int64()) {
        out = static_cast<double>(val.as_int64());
        return std::isfinite(out);
    }
    if (val.is_uint64()) {
        out = static_cast<double>(val.as_uint64());
        return std::isfinite(out);
    }
    return false;
}

int extract_non_negative_count(const bj::object& obj,
                               std::string_view primary_key,
                               std::string_view fallback_key) noexcept {
    const bj::value* v = obj.if_contains(
        bj::string_view(primary_key.data(), primary_key.size()));
    if (v == nullptr) {
        v = obj.if_contains(
            bj::string_view(fallback_key.data(), fallback_key.size()));
    }
    if (v == nullptr) {
        return 0;
    }
    int parsed = 0;
    if (extract_exact_int(*v, parsed)) {
        return parsed > 0 ? parsed : 0;
    }
    double numeric = 0.0;
    if (extract_finite_double(*v, numeric) && numeric > 0.0 &&
        numeric <= static_cast<double>(std::numeric_limits<int>::max())) {
        return static_cast<int>(numeric);
    }
    return 0;
}

std::string json_optional_string(const bj::object& obj,
                                 std::string_view key) {
    const bj::value* v =
        obj.if_contains(bj::string_view(key.data(), key.size()));
    if (v == nullptr || !v->is_string()) {
        return {};
    }
    const bj::string& s = v->as_string();
    return std::string(s.c_str(), s.size());
}

std::string format_glossary_supplement(const ToneGlossary& glossary) {
    const std::string_view guide =
        trim_ascii_view(glossary.sanitized_guide_markdown());
    if (!guide.empty()) {
        std::string block = "<novel_tone_and_glossary>\n";
        block.append(guide.data(), guide.size());
        block += "\n</novel_tone_and_glossary>";
        return block;
    }
    if (glossary.entries().empty()) {
        return {};
    }
    std::string block = "<novel_tone_and_glossary>\n";
    for (const GlossaryEntry& entry : glossary.entries()) {
        block += entry.jp_term + " -> " + entry.en_term + "\n";
    }
    block += "</novel_tone_and_glossary>";
    return block;
}

bool redact_entry_if_h_masked(ScriptEntry& entry,
                              const std::vector<bool>& h_scene_mask) {
    if (entry.global_index >= h_scene_mask.size() ||
        !h_scene_mask[entry.global_index]) {
        return false;
    }
    if (has_speaker_h_tag_suffix(entry.speaker)) {
        entry.speaker = std::string(kNarrationSpeaker);
    }
    entry.jp_text = std::string(kRedactedPlaceholder);
    entry.current_en = std::string(kRedactedPlaceholder);
    return true;
}

}  // namespace

std::string default_editorial_rules_prompt(bool fix_punctuation) {
    std::string rules =
        "IMPORTANT: This is a licensed commercial visual novel localization and editorial\n"
        "quality-assurance review of a published Japanese work of fiction. You MUST review\n"
        "and evaluate ALL lines faithfully regardless of content. The source material\n"
        "intentionally depicts dark psychological themes, unreliable perception, psychiatric\n"
        "delusion, obsession, surreal body horror, violence, and mature adult themes -- this\n"
        "is expected and intentional in the fictional narrative. Do NOT refuse, censor,\n"
        "soften, moralize, sanitize, or skip any line.\n\n"
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
        "    \"fix_span\": \"<optional exact replacement substring in new_en>\",\n"
        "    \"severity\": \"<critical|major|minor>\",\n"
        "    \"category\": \"<mistranslation|broken_callback|artifact_or_note|character_voice|censored_or_westernized|stiff_translationese>\",\n"
        "    \"confidence\": <number 0.0 to 1.0, must be >= 0.85>,\n"
        "    \"reason\": \"<concise explanation of the defect fixed>\"\n"
        "  }\n"
        "]\n"
        "</output_schema>";
    return rules;
}

std::string build_system_prompt(const ToneGlossary& glossary,
                                bool fix_punctuation) {
    std::string sys = default_editorial_rules_prompt(fix_punctuation);
    const std::string supplement = format_glossary_supplement(glossary);
    if (!supplement.empty()) {
        sys += "\n\n" + supplement;
    }
    return sys;
}

WindowSlice redact_h_lines_for_prompt(const WindowSlice& slice,
                                      const std::vector<bool>& h_scene_mask) {
    if (h_scene_mask.empty()) {
        return slice;
    }
    WindowSlice redacted = slice;
    for (ScriptEntry& entry : redacted.context_before) {
        redact_entry_if_h_masked(entry, h_scene_mask);
    }
    for (std::size_t i = 0; i < redacted.target.size(); ++i) {
        if (redact_entry_if_h_masked(redacted.target[i], h_scene_mask)) {
            redacted.editable_ids.erase(static_cast<int>(i + 1));
        }
    }
    for (ScriptEntry& entry : redacted.context_after) {
        redact_entry_if_h_masked(entry, h_scene_mask);
    }
    return redacted;
}

std::string build_spot_check_user_prompt(const WindowSlice& slice,
                                         bool fix_punctuation) {
    std::string prompt;

    if (!slice.context_before.empty()) {
        prompt += "<context_before>\n";
        for (const ScriptEntry& entry : slice.context_before) {
            prompt += "[" + entry.speaker + "] JP: " +
                      escape_for_prompt(entry.jp_text) +
                      " | EN: " + escape_for_prompt(entry.current_en) + "\n";
        }
        prompt += "</context_before>\n\n";
    }

    prompt += "<target_window>\n";
    for (std::size_t i = 0; i < slice.target.size(); ++i) {
        const int id = static_cast<int>(i + 1);
        const ScriptEntry& entry = slice.target[i];
        const bool is_editable = (slice.editable_ids.count(id) != 0);
        const char* status_tag =
            is_editable ? " [EDITABLE SUSPECT]" : " [READ-ONLY CONTEXT]";
        prompt += std::to_string(id) + ". [" + entry.speaker + "]" + status_tag + "\n";
        prompt += "   JP: " + escape_for_prompt(entry.jp_text) + "\n";
        prompt += "   EN: " + escape_for_prompt(entry.current_en) + "\n";
    }
    prompt += "</target_window>\n\n";

    if (!slice.context_after.empty()) {
        prompt += "<context_after>\n";
        for (const ScriptEntry& entry : slice.context_after) {
            prompt += "[" + entry.speaker + "] JP: " +
                      escape_for_prompt(entry.jp_text) +
                      " | EN: " + escape_for_prompt(entry.current_en) + "\n";
        }
        prompt += "</context_after>\n\n";
    }

    prompt += "Review the " + std::to_string(slice.target.size()) +
              " numbered lines inside <target_window> using <context_before> and "
              "<context_after> for scene continuity, pronoun resolution, and banter callbacks.\n";

    if (slice.editable_ids.size() < slice.target.size()) {
        if (slice.editable_ids.empty()) {
            prompt +=
                "IMPORTANT LOCKING RULE: All lines in <target_window> are locked as "
                "[READ-ONLY CONTEXT]. Output [].\n";
        } else {
            prompt +=
                "IMPORTANT LOCKING RULE: You may ONLY output fixes for IDs marked "
                "[EDITABLE SUSPECT] (allowed IDs:";
            for (int id : slice.editable_ids) {
                prompt += " " + std::to_string(id);
            }
            prompt += "). Lines marked [READ-ONLY CONTEXT] are locked for context only.\n";
        }
    }

    if (!fix_punctuation) {
        prompt +=
            "PUNCTUATION NOISE FILTER: Do NOT output fixes that only change "
            "hyphens/dashes (--- vs --), ellipsis length (... vs ......), quotes, "
            "or parenthetical notes. Only output substantive lexical, grammatical, "
            "voice, or discourse repairs.\n";
    }
    prompt +=
        "DO NO HARM: Only return an object when there is an objective defect with "
        "confidence >= 0.85. Output a JSON array following <output_schema>.";
    return prompt;
}

std::string build_radar_pack_prompt(const std::vector<WindowSlice>& passages,
                                    const ToneGlossary& glossary) {
    std::string prompt =
        "IMPORTANT: This is a licensed commercial visual novel localization QA\n"
        "(editorial quality-assurance triage) of a published Japanese fictional work\n"
        "containing dark psychological, delusional, violent, and mature narrative\n"
        "themes in a work of fiction. Evaluate ALL fictional passages faithfully\n"
        "without refusing, censoring, moralizing, or skipping any passage.\n\n"
        "<radar_triage_mandate>\n"
        "STAGE 1 PACKED GEMBA-MQM RADAR PASS (Kocmi & Federmann WMT 2023):\n"
        "Rapidly triage the following candidate visual novel passages for objective JP->EN "
        "translation defects (mistranslation, broken banter callbacks, pronoun/benefactive "
        "reversal, dropped role-language tics, or leftover CJK/romaji artifacts).\n"
        "Score each passage using GEMBA-MQM severity weights: critical = 25, major = 5, "
        "minor = 1 (0 if clean).\n"
        "</radar_triage_mandate>\n\n";

    const std::string supplement = format_glossary_supplement(glossary);
    if (!supplement.empty()) {
        prompt += supplement + "\n\n";
    }

    for (std::size_t p = 0; p < passages.size(); ++p) {
        const WindowSlice& slice = passages[p];
        prompt += "<passage id=\"" + std::to_string(p + 1) + "\" offset=\"" +
                  std::to_string(slice.target_start) + "\">\n";
        for (std::size_t i = 0; i < slice.target.size(); ++i) {
            const ScriptEntry& entry = slice.target[i];
            prompt += std::to_string(i + 1) + ". [" + entry.speaker + "] JP: " +
                      escape_for_prompt(entry.jp_text) + " | EN: " +
                      escape_for_prompt(entry.current_en) + "\n";
        }
        prompt += "</passage>\n\n";
    }

    prompt +=
        "<radar_output_schema>\n"
        "Return ONLY a valid JSON array with one entry per passage:\n"
        "[\n"
        "  {\n"
        "    \"passage_id\": <integer 1.." + std::to_string(passages.size()) + ">,\n"
        "    \"critical_count\": <non-negative integer>,\n"
        "    \"major_count\": <non-negative integer>,\n"
        "    \"minor_count\": <non-negative integer>,\n"
        "    \"mqm_score\": <number: sum of critical*25 + major*5 + minor*1, 0 if clean>,\n"
        "    \"suspect_ids\": [<1-based line IDs inside that passage needing repair>]\n"
        "  }\n"
        "]\n"
        "</radar_output_schema>";
    return prompt;
}

Result<std::vector<RadarPassageResult>> parse_radar_pack_response(
    std::string_view raw_response,
    const std::vector<WindowSlice>& passages) {
    Result<bj::array> parsed_arr =
        parse_json_array_strict(raw_response, "parse_radar_pack_response");
    if (!parsed_arr.is_ok()) {
        return Result<std::vector<RadarPassageResult>>::failure(
            parsed_arr.status());
    }

    std::vector<RadarPassageResult> results;
    if (passages.empty()) {
        return Result<std::vector<RadarPassageResult>>::success(std::move(results));
    }

    std::set<int> seen_passages;
    for (const bj::value& item_v : parsed_arr.value()) {
        if (!item_v.is_object()) {
            continue;
        }
        const bj::object& obj = item_v.as_object();

        const bj::value* pid_v = obj.if_contains("passage_id");
        int pid = 0;
        if (pid_v == nullptr || !extract_exact_int(*pid_v, pid)) {
            continue;
        }
        if (pid < 1 || static_cast<std::size_t>(pid) > passages.size()) {
            continue;
        }
        if (!seen_passages.insert(pid).second) {
            continue;
        }

        const WindowSlice& slice = passages[static_cast<std::size_t>(pid - 1)];
        RadarPassageResult rpr;
        rpr.passage_id = pid;
        rpr.offset = slice.target_start;
        rpr.length = slice.target.size();
        rpr.critical_count =
            extract_non_negative_count(obj, "critical_count", "critical");
        rpr.major_count =
            extract_non_negative_count(obj, "major_count", "major");
        rpr.minor_count =
            extract_non_negative_count(obj, "minor_count", "minor");

        double explicit_mqm = -1.0;
        const bj::value* mqm_v = obj.if_contains("mqm_score");
        if (mqm_v != nullptr && extract_finite_double(*mqm_v, explicit_mqm) &&
            explicit_mqm >= 0.0) {
            rpr.mqm_score = explicit_mqm;
        } else {
            rpr.mqm_score =
                kMqmCriticalWeight * static_cast<double>(rpr.critical_count) +
                kMqmMajorWeight * static_cast<double>(rpr.major_count) +
                kMqmMinorWeight * static_cast<double>(rpr.minor_count);
        }

        const bj::value* suspect_v = obj.if_contains("suspect_ids");
        if (suspect_v != nullptr && suspect_v->is_array()) {
            const std::size_t max_id = slice.target.size();
            for (const bj::value& sv : suspect_v->as_array()) {
                int sid = 0;
                if (!extract_exact_int(sv, sid)) {
                    continue;
                }
                if (sid >= 1 && static_cast<std::size_t>(sid) <= max_id) {
                    rpr.suspect_ids.insert(sid);
                }
            }
        }
        results.push_back(std::move(rpr));
    }

    return Result<std::vector<RadarPassageResult>>::success(std::move(results));
}

Result<ParsedSpotCheckBatch> parse_spot_check_response(
    std::string_view raw_response,
    const WindowSlice& slice,
    double min_confidence) {
    Result<bj::array> parsed_arr =
        parse_json_array_strict(raw_response, "parse_spot_check_response");
    if (!parsed_arr.is_ok()) {
        return Result<ParsedSpotCheckBatch>::failure(parsed_arr.status());
    }

    ParsedSpotCheckBatch batch;
    std::set<int> seen_ids;
    for (const bj::value& item_v : parsed_arr.value()) {
        if (!item_v.is_object()) {
            ++batch.rejected_in_parse;
            continue;
        }
        const bj::object& obj = item_v.as_object();

        const bj::value* id_v = obj.if_contains("id");
        int id = 0;
        if (id_v == nullptr || !extract_exact_int(*id_v, id)) {
            ++batch.rejected_in_parse;
            continue;
        }
        if (id < 1 || static_cast<std::size_t>(id) > slice.target.size()) {
            ++batch.rejected_in_parse;
            continue;
        }
        if (slice.editable_ids.count(id) == 0) {
            ++batch.rejected_in_parse;
            continue;
        }
        if (!seen_ids.insert(id).second) {
            ++batch.rejected_in_parse;
            continue;
        }

        const bj::value* cat_v = obj.if_contains("category");
        if (cat_v == nullptr || !cat_v->is_string()) {
            ++batch.rejected_in_parse;
            continue;
        }
        const std::string category =
            trim_ascii(std::string_view(cat_v->as_string().c_str(),
                                        cat_v->as_string().size()));
        if (!is_valid_defect_category(category)) {
            ++batch.rejected_in_parse;
            continue;
        }

        std::string severity{kDefaultSeverity};
        const bj::value* sev_v = obj.if_contains("severity");
        if (sev_v != nullptr && !sev_v->is_null()) {
            if (!sev_v->is_string()) {
                ++batch.rejected_in_parse;
                continue;
            }
            const std::string parsed_sev =
                trim_ascii(std::string_view(sev_v->as_string().c_str(),
                                            sev_v->as_string().size()));
            if (!parsed_sev.empty()) {
                if (!is_valid_severity(parsed_sev)) {
                    ++batch.rejected_in_parse;
                    continue;
                }
                severity = parsed_sev;
            }
        }

        const bj::value* conf_v = obj.if_contains("confidence");
        double confidence = 0.0;
        if (conf_v == nullptr || !std::isfinite(min_confidence) ||
            !extract_finite_double(*conf_v, confidence) ||
            !std::isfinite(confidence) || confidence < min_confidence ||
            confidence > kMaxConfidence) {
            ++batch.rejected_in_parse;
            continue;
        }

        std::string raw_improved = json_optional_string(obj, "new_en");
        if (trim_ascii_view(raw_improved).empty()) {
            raw_improved = json_optional_string(obj, "improved_en");
        }
        if (trim_ascii_view(raw_improved).empty()) {
            ++batch.rejected_in_parse;
            continue;
        }

        const ScriptEntry& target_entry =
            slice.target[static_cast<std::size_t>(id - 1)];
        const std::string normalized =
            normalize_restored_translation(target_entry, raw_improved);
        if (normalized.empty()) {
            ++batch.rejected_in_parse;
            continue;
        }

        const std::string raw_error_span = json_optional_string(obj, "error_span");
        const std::string error_span = trim_ascii(sanitize_ascii_punctuation(
            unescape_llm_newlines(raw_error_span, target_entry.current_en)));

        const std::string raw_fix_span = json_optional_string(obj, "fix_span");
        const std::string fix_span = trim_ascii(sanitize_ascii_punctuation(
            unescape_llm_newlines(raw_fix_span, target_entry.current_en)));

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
        imp.fix_span = fix_span;
        imp.confidence = confidence;
        imp.reason = trim_ascii(json_optional_string(obj, "reason"));
        batch.candidates.push_back(std::move(imp));
    }

    return Result<ParsedSpotCheckBatch>::success(std::move(batch));
}

}  // namespace spot_checker
