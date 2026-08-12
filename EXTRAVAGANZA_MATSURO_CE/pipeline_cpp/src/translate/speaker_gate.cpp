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
#include "translate_core.h"

namespace exm::translate {

namespace bj = boost::json;

namespace {

// Enough dialogue to make a broken speaker prefix obvious and few enough that
// the printed sample stays readable.
constexpr std::size_t SAMPLE_SIZE = 50;

// Above this share of [NARRATION] among dialogue lines the speaker prefix is
// not flowing at all.  A handful of genuinely unattributed dialogue lines is
// normal; a twentieth of them is not.
constexpr double MAX_NARRATION_PCT = 5.0;

// How many lines of the prompt sample are worth printing.
constexpr std::size_t PROMPT_LINES_SHOWN = 8;

// How many entries of a failing list are worth printing.  The list exists to
// be pasted into the glossary, and beyond this it is a wall.
constexpr std::size_t MAX_LISTED = 30;

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

// Every type=='dialogue' string, in file order.
std::vector<std::string> collect_dialogue(const bj::object& extracted) {
    std::vector<std::string> out;
    for (const auto& kv : extracted) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            const auto* t = s.if_contains("type");
            if (!t || !t->is_string() || t->get_string() != "dialogue") continue;
            if (const auto* txt = s.if_contains("text"))
                if (txt->is_string()) out.emplace_back(txt->get_string());
        }
    }
    return out;
}

// The raw (untranslated) JP speaker plate, for the coverage scan.  MUST
// mirror extract_speaker -- if you change one, change the other.
std::string raw_jp_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf == std::string::npos) return {};
    const std::string raw = trim(text.substr(0, crlf));
    const std::string rest = trim_left(text.substr(crlf + 2));
    if (raw.empty() || char_len(raw) > 20 || rest.empty()) return {};
    // 「 and 『 -- the two quote marks the engine opens dialogue with.
    if (rest.rfind("\xE3\x80\x8C", 0) != 0 && rest.rfind("\xE3\x80\x8E", 0) != 0) return {};
    return raw;
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

    const std::vector<std::string> dialogue_all = collect_dialogue(extracted);
    const std::vector<std::string> dialogue(
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

    // -- 1: extract_speaker returns real speakers, not 'NARRATION' --
    std::size_t narration = 0;
    std::set<std::string> speakers_seen;
    for (const auto& d : dialogue) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") ++narration;
        else speakers_seen.insert(sp);
    }
    const double pct =
        100.0 * static_cast<double>(narration) / static_cast<double>(dialogue.size());
    print_line("[1/5] extract_speaker on " + std::to_string(dialogue.size()) +
               " dialogue entries:");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::string list = "      distinct speakers:  [";
        std::size_t shown = 0;
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
                    "too many lines tagged NARRATION -- speaker prefix not flowing");

    // -- 2: JP names resolve to their pinned English --
    print_line("[2/5] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99", "Miyaka"},  // 美弥香
        {"\xE7\xB6\xBE\xE4\xBD\xB3", "Ayaka"},               // 綾佳
        {"\xE7\x85\x89\xE7\x8D\x84", "Rengoku"},             // 煉獄
        {"\xE5\x8F\xB2\xE9\x83\x8E", "Shirou"},              // 史郎
        {"\xE3\x82\xB3\xE3\x83\xAC\xE3\x82\xAF\xE3\x82\xBF\xE3\x83\xBC", "Collector"},
    };
    for (const auto& [jp, en] : cases) {
        // The engine's own shape: name plate, CRLF, then 「dialogue」.
        const std::string text = jp + "\r\n\xE3\x80\x8C" "dummy" "\xE3\x80\x8D";
        const std::string got = extract_speaker(text);
        print_line(std::string("      ") + (got == en ? "OK " : "FAIL") + "  '" + jp +
                   "' -> '" + got + "' (expected '" + en + "')");
        if (got != en)
            return fail(report, 2, "'" + jp + "' resolved to '" + got + "', not '" + en + "'");
    }

    // -- 3: the assembled user prompt carries real [Speaker] tags --
    // Built by the same function the requests are built with, so the check
    // cannot pass against a copy of the prompt that has drifted from it.
    //
    // A handful of two-line NARRATIVE entries are mis-typed as 'dialogue' in
    // extracted_text.json, and extract_speaker correctly tags those NARRATION.
    // Sampling the first ten that DO yield a speaker keeps the check on actual
    // dialogue; if the pipeline is genuinely broken the filter starves.
    std::vector<std::pair<std::string, std::string>> batch;  // (speaker, jp)
    for (const auto& d : dialogue_all) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") continue;
        batch.emplace_back(sp, d);
        if (batch.size() == 10) break;
    }
    print_line("[3/5] reconstructed user-prompt for the first 10 speakered dialogue lines:");
    if (batch.size() < 10)
        return fail(report, 3,
                    "only " + std::to_string(batch.size()) +
                        " dialogue entries yielded a speaker -- the pipeline is broken");
    {
        const std::string prompt = build_user_prompt(batch, {});
        std::size_t bad = 0, pos = 0, shown = 0;
        while (pos < prompt.size()) {
            const std::size_t nl = prompt.find('\n', pos);
            const std::string line = prompt.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? prompt.size() : nl + 1;
            if (line.find("[NARRATION]") != std::string::npos) ++bad;
            if (shown < PROMPT_LINES_SHOWN) {
                print_line("      " + line);
                ++shown;
            }
        }
        print_line("      ...");
        if (bad)
            return fail(report, 3,
                        std::to_string(bad) +
                            " dialogue lines tagged [NARRATION] in the prompt");
    }

    // -- 4 (sanity): the speaker table still holds the core cast --
    const auto& name_dict = name_translations();
    if (!name_dict.count("\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99"))  // 美弥香
        return fail(report, 4, "the speaker table is missing the core character names");
    print_line("[4/5] the speaker table contains the core character names "
               "(\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99 etc.): OK");

    // -- 5: exhaustive bidirectional speaker coverage --
    std::map<std::string, std::size_t> seen;
    for (const auto& d : dialogue_all) {
        const std::string raw = raw_jp_speaker(d);
        if (!raw.empty()) ++seen[raw];
    }

    report.speakers_in_data = seen.size();
    report.glossary_entries = name_dict.size();
    std::vector<std::string> missing, unused;
    for (const auto& [jp, n] : seen)
        if (!name_dict.count(jp)) missing.push_back(jp);
    for (const auto& [jp, en] : name_translations_ordered())
        if (!seen.count(jp)) unused.push_back(jp);
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
                          return seen[a] != seen[b] ? seen[a] > seen[b] : a < b;
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

}  // namespace exm::translate
