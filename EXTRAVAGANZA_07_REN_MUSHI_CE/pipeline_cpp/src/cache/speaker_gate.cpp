// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "speaker_gate.h"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "cache_core.h"
#include "common/util.h"
#include "glossary.h"

namespace exm::translate {

namespace bj = boost::json;

namespace {

constexpr int kTotalCheckCount = 5;
constexpr int kCheck1ExtractionId = 1;
constexpr int kCheck2GlossaryId = 2;
constexpr int kCheck3QuoteId = 3;
constexpr int kCheck4EndToEndId = 4;
constexpr int kCheck5CoverageId = 5;

constexpr int kMinExpectedDialogueLines = 30;
constexpr int kSampleWindowSize = 37;
constexpr int kMaxUnquotedDialogueSamples = 2;

}  // namespace

SpeakerCheckReport run_speaker_checks(const bj::object& extracted_data) {
    SpeakerCheckReport r;
    const auto& table = name_translations();
    r.glossary_entries = table.size();

    auto fail = [&](int n, const std::string& why) {
        r.passed = false;
        r.failed_check = n;
        r.reason = why;
        log_info("  FAIL (check " + std::to_string(n) + "/" +
                 std::to_string(kTotalCheckCount) + "): " + why);
        return r;
    };

    std::vector<const bj::array*> files_in_order;
    std::unordered_set<std::string> visited;
    for (const auto& fname : story_order()) {
        if (const auto* fval = extracted_data.if_contains(fname)) {
            if (fval->is_object()) {
                if (const auto* s = fval->get_object().if_contains("strings")) {
                    if (s->is_array()) {
                        files_in_order.push_back(&s->get_array());
                        visited.insert(fname);
                    }
                }
            }
        }
    }
    for (const auto& kv : extracted_data) {
        const std::string fname(kv.key());
        if (visited.count(fname) || !kv.value().is_object()) continue;
        if (const auto* s = kv.value().get_object().if_contains("strings")) {
            if (s->is_array()) files_in_order.push_back(&s->get_array());
        }
    }

    // [1/5] Extraction sanity
    int total_dialogue = 0;
    int total_narrative = 0;
    for (const auto* strings : files_in_order) {
        for (const auto& sv : *strings) {
            const auto& s = sv.get_object();
            if (!s.at("has_jp").as_bool()) continue;
            const std::string t(s.at("type").get_string());
            if (t == "dialogue") ++total_dialogue;
            else if (t == "narrative") ++total_narrative;
        }
    }
    log_info("  [1/5] Extracted: " + std::to_string(total_dialogue) + " dialogue, " +
             std::to_string(total_narrative) + " narrative");
    if (total_dialogue < kMinExpectedDialogueLines) {
        return fail(kCheck1ExtractionId,
                    "Only " + std::to_string(total_dialogue) +
                        " dialogue lines found (expected >= " +
                        std::to_string(kMinExpectedDialogueLines) +
                        "). Extraction may be broken.");
    }

    // [2/5] Glossary lookup
    const std::vector<std::pair<std::string, std::string>> test_pairs = {
        {"レン", "Ren"},
        {"夢美", "Yumemi"},
        {"司会者", "Emcee"},
    };
    for (const auto& [jp, expected] : test_pairs) {
        const auto it = table.find(jp);
        if (it == table.end() || it->second != expected) {
            const std::string got = (it == table.end()) ? "<missing>" : it->second;
            return fail(kCheck2GlossaryId,
                        "name_translations()['" + jp + "'] = '" + got +
                            "', expected '" + expected + "'");
        }
    }
    log_info("  [2/5] Glossary lookup: OK (" + std::to_string(table.size()) + " entries)");

    // [3/5] Quote char check in story_order() priority
    int bad_quotes = 0;
    int checked = 0;
    for (const auto* strings : files_in_order) {
        for (const auto& sv : *strings) {
            const auto& s = sv.get_object();
            if (s.at("type").get_string() != "dialogue" || !s.at("has_jp").as_bool()) continue;
            const std::string text(s.at("text").get_string());
            const std::size_t idx = text.find("\r\n");
            if (idx != std::string::npos) {
                const std::string rest = text.substr(idx + 2);
                const bool ok = rest.rfind("「", 0) == 0 || rest.rfind("『", 0) == 0 ||
                                rest.rfind("（", 0) == 0;
                if (!ok) ++bad_quotes;
                if (++checked >= kSampleWindowSize) break;
            }
        }
        if (checked >= kSampleWindowSize) break;
    }
    if (bad_quotes > kMaxUnquotedDialogueSamples) {
        return fail(kCheck3QuoteId,
                    std::to_string(bad_quotes) + "/" + std::to_string(checked) +
                        " dialogue lines don't start with 「/『/（ after speaker name.");
    }
    log_info("  [3/5] Dialogue quote chars: OK (" + std::to_string(checked - bad_quotes) +
             "/" + std::to_string(checked) + " standard)");

    // [4/5] End-to-end speaker extraction
    const std::vector<std::pair<std::string, std::string>> e2e = {
        {"レン\r\n「あーよく寝た。」", "Ren"},
        {"夢美\r\n「おはよう、レン」", "Yumemi"},
        {"司会者\r\n「それでは歌っていただきましょう。」", "Emcee"},
        {"レンはむしくんを見つめた。", "NARRATION"},
    };
    for (const auto& [sample, expected] : e2e) {
        const std::string got = extract_speaker(sample);
        if (got != expected) {
            return fail(kCheck4EndToEndId,
                        "extract_speaker() returned '" + got + "', expected '" + expected + "'");
        }
    }
    log_info("  [4/5] Speaker extraction end-to-end: OK");

    // [5/5] Full speaker coverage (bidirectional: every speaker in data is in
    // glossary AND every glossary entry appears in data)
    std::set<std::string> seen;
    for (const auto* strings : files_in_order) {
        for (const auto& sv : *strings) {
            const auto& s = sv.get_object();
            if (s.at("type").get_string() != "dialogue" || !s.at("has_jp").as_bool()) continue;
            const std::string text(s.at("text").get_string());
            const std::size_t idx = text.find("\r\n");
            if (idx == std::string::npos) continue;
            const std::string rest = text.substr(idx + 2);
            if (rest.rfind("「", 0) == 0 || rest.rfind("『", 0) == 0) {
                seen.insert(trim(text.substr(0, idx)));
            }
        }
    }
    r.speakers_in_data = seen.size();

    std::vector<std::string> missing;
    for (const auto& sp : seen) {
        if (table.find(sp) == table.end()) missing.push_back(sp);
    }
    std::vector<std::string> unused;
    for (const auto& [jp, en] : name_translations_ordered()) {
        (void)en;
        if (seen.find(jp) == seen.end()) unused.push_back(jp);
    }
    if (!missing.empty() || !unused.empty()) {
        std::string msg;
        if (!missing.empty()) {
            msg += "Speakers in data missing from glossary (" +
                   std::to_string(missing.size()) + "): ";
            for (std::size_t i = 0; i < missing.size(); ++i) {
                if (i) msg += ", ";
                msg += missing[i];
            }
        }
        if (!unused.empty()) {
            if (!msg.empty()) msg += "; ";
            msg += "Glossary entries not in data (" + std::to_string(unused.size()) + "): ";
            for (std::size_t i = 0; i < unused.size(); ++i) {
                if (i) msg += ", ";
                msg += unused[i];
            }
        }
        return fail(kCheck5CoverageId, msg);
    }
    log_info("  [5/5] Speaker coverage: EXACT MATCH (" + std::to_string(seen.size()) +
             " speakers == " + std::to_string(table.size()) + " glossary entries)");

    r.passed = true;
    return r;
}

}  // namespace exm::translate
