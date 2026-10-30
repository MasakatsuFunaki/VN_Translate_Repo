// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "speaker_gate.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"
#include "glossary.h"
#include "cache_core.h"

namespace snu::translate {

namespace bj = boost::json;

namespace {

// Enough dialogue to make a broken speaker field obvious and few enough that
// the printed sample stays readable.
constexpr std::size_t SAMPLE_SIZE = 50;

// Above this percentage of [NARRATION] among dialogue lines, the speaker
// field is considered broken.
constexpr double MAX_NARRATION_PCT = 5.0;

// How many entries of a failing list are worth printing.  The list exists to
// be pasted into the glossary, and beyond this it is a wall.
constexpr std::size_t MAX_LISTED = 30;

std::vector<bj::object> collect_dialogue(const bj::object& extracted) {
    std::vector<bj::object> out;
    for (const auto& kv : extracted) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            if (auto* t = s.if_contains("type"))
                if (t->is_string() && t->get_string() == "dialogue")
                    out.push_back(s);
        }
    }
    return out;
}

SpeakerCheckReport fail(SpeakerCheckReport report, int check, const std::string& reason) {
    report.passed = false;
    report.failed_check = check;
    report.reason = reason;
    print_line("      FAIL: " + reason);
    return report;
}

}  // namespace

SpeakerCheckReport run_speaker_checks(const bj::object& extracted) {
    SpeakerCheckReport report;

    std::vector<bj::object> dialogue_all = collect_dialogue(extracted);
    std::vector<bj::object> dialogue(
        dialogue_all.begin(),
        dialogue_all.begin() +
            static_cast<std::ptrdiff_t>(std::min(SAMPLE_SIZE, dialogue_all.size())));
    report.dialogue_sampled = dialogue.size();
    report.dialogue_total = dialogue_all.size();
    if (dialogue.size() < 10) {
        print_line("[1/5] extract_speaker on the extracted dialogue:");
        return fail(report, 1,
                    "only " + std::to_string(dialogue.size()) +
                        " dialogue entries -- the extraction looks empty");
    }

    // ── 1: extract_speaker returns real speakers, not 'NARRATION' ──
    std::size_t narration = 0;
    std::set<std::string> speakers_seen;
    for (const auto& s : dialogue) {
        std::string sp = extract_speaker(s);
        if (sp == "NARRATION") ++narration;
        else speakers_seen.insert(sp);
    }
    double pct = 100.0 * static_cast<double>(narration) / static_cast<double>(dialogue.size());
    print_line("[1/5] extract_speaker on " + std::to_string(dialogue.size()) +
               " dialogue entries:");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::string list = "      distinct speakers:  [";
        int shown = 0;
        for (const auto& sp : speakers_seen) {
            if (shown >= 8) break;
            if (shown) list += ", ";
            list += "'" + sp + "'";
            ++shown;
        }
        print_line(list + "]");
    }
    if (pct >= MAX_NARRATION_PCT)
        return fail(report, 1,
                    "too many lines tagged NARRATION -- speaker field not flowing");

    // ── 2: JP names resolve to their pinned English ──
    print_line("[2/5] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"直哉", "Naoya"}, {"稟", "Rin"}, {"真琴", "Makoto"},
        {"里奈", "Rina"}, {"優美", "Yumi"}};
    for (const auto& [jp, en] : cases) {
        bj::object s;
        s["speaker"] = jp;
        s["text"] = "「dummy」";
        s["type"] = "dialogue";
        std::string got = extract_speaker(s);
        print_line(std::string("      ") + (got == en ? "OK " : "FAIL") + "  '" + jp +
                   "' -> '" + got + "' (expected '" + en + "')");
        if (got != en)
            return fail(report, 2, "'" + jp + "' resolved to '" + got + "', not '" + en + "'");
    }

    // ── 3: the assembled user prompt carries real [Speaker] tags ──
    std::vector<std::pair<std::string, std::string>> batch;
    for (std::size_t i = 0; i < dialogue.size() && i < 10; ++i)
        batch.emplace_back(extract_speaker(dialogue[i]),
                           std::string(dialogue[i].at("text").get_string()));
    std::string prompt = build_user_prompt(batch, {});
    std::size_t bad = 0;
    {
        std::size_t pos = 0, shown = 0;
        print_line("[3/5] reconstructed user-prompt for first 10 dialogue lines:");
        while (pos < prompt.size()) {
            std::size_t nl = prompt.find('\n', pos);
            std::string line = prompt.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? prompt.size() : nl + 1;
            if (line.find("[NARRATION]") != std::string::npos) ++bad;
            if (shown < 8) {
                print_line("      " + line);
                ++shown;
            }
        }
        print_line("      ...");
    }
    if (bad)
        return fail(report, 3,
                    std::to_string(bad) + " dialogue lines tagged [NARRATION] in the prompt");

    // ── 4 (sanity): the speaker table still holds the core cast ──
    if (!name_translations().count("直哉"))
        return fail(report, 4, "the speaker table is missing the core character names");
    print_line("[4/5] the speaker table contains the core character names (直哉 etc.): OK");

    // ── 5: exhaustive bidirectional speaker coverage ──
    std::map<std::string, std::size_t> seen;
    for (const auto& kv : extracted) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            std::string type;
            if (auto* t = s.if_contains("type"))
                if (t->is_string()) type = std::string(t->get_string());
            if (type == "name") {
                std::string raw;
                if (auto* t = s.if_contains("text"))
                    if (t->is_string()) raw = strip(std::string(t->get_string()));
                if (!raw.empty()) ++seen[raw];
            } else {
                std::string spk;
                if (auto* t = s.if_contains("speaker"))
                    if (t->is_string()) spk = strip(std::string(t->get_string()));
                if (!spk.empty()) ++seen[spk];
            }
        }
    }

    const auto& name_dict = name_translations();
    report.speakers_in_data = seen.size();
    report.glossary_entries = name_dict.size();
    std::vector<std::string> missing, unused;
    for (const auto& [k, n] : seen)
        if (!name_dict.count(k)) missing.push_back(k);
    for (const auto& [k, v] : name_translations_ordered())
        if (!seen.count(k)) unused.push_back(k);
    std::sort(missing.begin(), missing.end());
    std::sort(unused.begin(), unused.end());

    print_line("[5/5] exhaustive speaker coverage scan:");
    print_line("      distinct JP speakers in data: " + std::to_string(seen.size()));
    print_line("      speaker table entries:        " + std::to_string(name_dict.size()));

    if (!missing.empty() || !unused.empty()) {
        if (!missing.empty()) {
            print_line("      FAIL: " + std::to_string(missing.size()) +
                       " JP speakers in data not in the speaker table");
            print_line("            (copy-paste-ready, sorted by frequency):");
            std::vector<std::string> by_freq = missing;
            std::sort(by_freq.begin(), by_freq.end(),
                      [&](const std::string& a, const std::string& b) {
                          return seen[a] > seen[b];
                      });
            for (std::size_t i = 0; i < by_freq.size() && i < MAX_LISTED; ++i)
                print_line("          '" + by_freq[i] + "': '???',  # appeared " +
                           std::to_string(seen[by_freq[i]]) + "x");
            if (missing.size() > MAX_LISTED)
                print_line("          ... and " + std::to_string(missing.size() - MAX_LISTED) +
                           " more");
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " speaker table entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < MAX_LISTED; ++i)
                print_line("          '" + unused[i] + "' -> '" + name_dict.at(unused[i]) + "'");
            if (unused.size() > MAX_LISTED)
                print_line("          ... and " + std::to_string(unused.size() - MAX_LISTED) +
                           " more");
        }
        return fail(report, 5,
                    std::to_string(missing.size()) + " speakers missing from the table and " +
                        std::to_string(unused.size()) + " table entries never spoken");
    }
    print_line("      OK -- full bidirectional coverage.");

    print_line("");
    print_line("  All checks passed.  The speaker pipeline is healthy -- safe to translate.");
    print_line("  Sample size (checks 1, 3): " + std::to_string(dialogue.size()) + " of " +
               std::to_string(dialogue_all.size()) + " dialogue entries.");
    report.passed = true;
    return report;
}

}  // namespace snu::translate
