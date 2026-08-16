// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "speaker_gate.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"
#include "glossary.h"
#include "translate_core.h"

namespace ama::translate {

namespace bj = boost::json;

namespace {

constexpr std::size_t SAMPLE_RUNS = 1000;          // runs scanned for checks 1 and 3
constexpr std::size_t MIN_DISTINCT_SPEAKERS = 10;  // check 1 fail threshold
constexpr std::size_t PROMPT_SAMPLE = 10;          // runs check 3 builds a prompt from
constexpr std::size_t MAX_LISTED = 30;             // a failing list beyond this is a wall

struct SampledRun {
    std::string script;
    int line_idx = 0;
    std::string speaker;
    std::string content;
};

// Walk extract_message_runs() across every script in stored order -- i.e.
// exactly what feeds call_anthropic.
std::vector<SampledRun> walk_runs(const bj::object& data, std::size_t limit) {
    std::vector<SampledRun> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto& script = kv.value().get_object();
        const auto* lines = script.if_contains("lines");
        if (!lines || !lines->is_array() || lines->get_array().empty()) continue;
        for (const auto& run : extract_message_runs(script)) {
            out.push_back(SampledRun{std::string(kv.key()), run.line_idx, run.speaker,
                                     run.content});
            if (out.size() >= limit) return out;
        }
    }
    return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

// A message run may carry a real line break.  The prompt sends it as-is; the
// printed sample writes it as \n so one run stays on one console line.
std::string one_line(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "\r\n") == 0) { out += "\\r\\n"; ++i; }
        else if (s[i] == '\n') out += "\\n";
        else if (s[i] == '\r') out += "\\r";
        else out += s[i];
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

SpeakerCheckReport run_speaker_checks(const bj::object& data) {
    SpeakerCheckReport report;
    report.scripts_scanned = data.size();

    // ── 1: the state machine yields real speakers, not all NARRATION ──
    // Most Amakano 2 lines are legitimate narration (the narrative VN style),
    // so a strict "<=5% NARRATION" threshold does not apply.  Require a healthy
    // VARIETY of named speakers instead: if get_speaker_tag breaks or NAME
    // tracking regresses, the distinct-speaker count collapses.
    const std::vector<SampledRun> sample = walk_runs(data, SAMPLE_RUNS);
    report.runs_sampled = sample.size();
    print_line("[1/5] extract_message_runs on the first " + std::to_string(sample.size()) +
               " runs:");
    if (sample.size() < 100)
        return fail(report, 1,
                    "only " + std::to_string(sample.size()) +
                        " message runs -- extracted_text.json looks empty");

    std::set<std::string> distinct_real;
    std::size_t n_real = 0;
    for (const auto& r : sample)
        if (r.speaker != "NARRATION") {
            distinct_real.insert(r.speaker);
            ++n_real;
        }
    print_line("      real-speaker hits:  " + std::to_string(n_real) + "  (" +
               std::to_string(100 * n_real / sample.size()) + "%)");
    {
        std::vector<std::string> first10(distinct_real.begin(), distinct_real.end());
        if (first10.size() > 10) first10.resize(10);
        print_line("      distinct speakers:  [" + join(first10, ", ") + "]");
    }
    if (distinct_real.size() < MIN_DISTINCT_SPEAKERS)
        return fail(report, 1,
                    "only " + std::to_string(distinct_real.size()) +
                        " distinct speakers found (need >=" +
                        std::to_string(MIN_DISTINCT_SPEAKERS) +
                        ") -- the state machine may be broken");

    // ── 2: get_speaker_tag resolves JP names to EN via the glossary ──
    print_line("[2/5] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"\xE3\x81\xA1\xE3\x81\xA8\xE3\x81\x9B", "Chitose"},   // ちとせ
        {"\xE7\xB5\x90\xE7\x81\xAF", "Yuuhi"},                 // 結灯
        {"\xE7\x8E\xB2", "Rei"},                               // 玲
        {"\xE6\xB7\xB5\xE4\xB8\x8A", "Fuchigami"},             // 淵上
        {"\xE5\xAF\xB6\xE6\xB3\x89\xE8\xB7\xAF", "Housenji"},  // 寶泉路
    };
    for (const auto& [jp, en] : cases) {
        const std::string got = get_speaker_tag(jp);
        print_line(std::string("      ") + (got == en ? "OK  " : "FAIL") + " '" + jp +
                   "' -> '" + got + "' (expected '" + en + "')");
        if (got != en)
            return fail(report, 2,
                        "'" + jp + "' resolved to '" + got + "', not '" + en + "'");
    }

    // ── 3: the assembled API user-prompt carries real [Speaker] tags ──
    // Built by the same function the requests are built with, so the check
    // cannot pass against a copy of the prompt that has drifted from it.
    std::vector<SampledRun> speakered;
    for (const auto& r : sample) {
        if (r.speaker == "NARRATION") continue;
        speakered.push_back(r);
        if (speakered.size() == PROMPT_SAMPLE) break;
    }
    print_line("[3/5] reconstructed user-prompt for the first " +
               std::to_string(PROMPT_SAMPLE) + " speakered runs:");
    if (speakered.size() < PROMPT_SAMPLE)
        return fail(report, 3,
                    "only " + std::to_string(speakered.size()) + " speakered runs in the first " +
                        std::to_string(sample.size()) + " -- the pipeline is broken");

    std::vector<std::pair<std::string, std::string>> batch;
    for (const auto& r : speakered) batch.emplace_back(r.speaker, r.content);
    const std::string prompt = build_user_prompt(batch, {});
    std::size_t bad = 0;
    {
        std::size_t pos = 0, shown = 0;
        while (pos < prompt.size()) {
            const std::size_t nl = prompt.find('\n', pos);
            const std::string line =
                prompt.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? prompt.size() : nl + 1;
            if (line.find("[NARRATION]") != std::string::npos) ++bad;
            if (shown < 8) {
                print_line("      " + one_line(line));
                ++shown;
            }
        }
        print_line("      ...");
    }
    if (bad)
        return fail(report, 3,
                    std::to_string(bad) + " speakered lines reached the prompt as [NARRATION]");

    // ── 4 (sanity): the glossary still holds the core cast ──
    const auto& name_dict = name_translations();
    report.glossary_entries = name_dict.size();
    if (!name_dict.count("\xE3\x81\xA1\xE3\x81\xA8\xE3\x81\x9B"))  // ちとせ
        return fail(report, 4, "the speaker table is missing the core character names");
    print_line("[4/5] the speaker table contains the core character names (\xE3\x81\xA1"
               "\xE3\x81\xA8\xE3\x81\x9B etc.): OK");

    // ── 5: exhaustive bidirectional speaker coverage ──
    // Every NAME-line content other than a $-prefixed protagonist tag is a raw
    // JP speaker.  It MUST be a glossary key, and every glossary key MUST
    // appear at least once as a NAME line.
    std::map<std::string, std::size_t> seen;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* lines = kv.value().get_object().if_contains("lines");
        if (!lines || !lines->is_array()) continue;
        for (const auto& lv : lines->get_array()) {
            const auto& line = lv.get_object();
            const auto* t = line.if_contains("type");
            if (!t || !t->is_string() || t->get_string() != "NAME") continue;
            std::string content;
            if (const auto* c = line.if_contains("content"))
                if (c->is_string()) content = std::string(c->get_string());
            content = trim(content);
            if (content.empty() || content[0] == '$') continue;
            ++seen[content];
        }
    }
    report.speakers_in_data = seen.size();

    std::vector<std::string> missing, unused;
    for (const auto& [jp, n] : seen)
        if (!name_dict.count(jp)) missing.push_back(jp);
    for (const auto& [jp, en] : name_translations_ordered())
        if (!seen.count(jp)) unused.push_back(jp);
    std::sort(unused.begin(), unused.end());

    print_line("[5/5] exhaustive speaker coverage scan:");
    print_line("      distinct JP speakers in data: " + std::to_string(seen.size()));
    print_line("      speaker table entries:        " + std::to_string(name_dict.size()));

    if (!missing.empty() || !unused.empty()) {
        if (!missing.empty()) {
            print_line("      FAIL: " + std::to_string(missing.size()) +
                       " JP speakers in data not in the speaker table");
            print_line("            (copy-paste-ready, sorted by frequency):");
            std::sort(missing.begin(), missing.end(),
                      [&](const std::string& a, const std::string& b) {
                          return seen[a] != seen[b] ? seen[a] > seen[b] : a < b;
                      });
            for (std::size_t i = 0; i < missing.size() && i < MAX_LISTED; ++i)
                print_line("          '" + missing[i] + "': '???',  # appeared " +
                           std::to_string(seen[missing[i]]) + "x");
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
    print_line("  Sample size (checks 1, 3): " + std::to_string(sample.size()) +
               " message runs across " + comma(static_cast<long long>(data.size())) +
               " scripts.");
    report.passed = true;
    return report;
}

}  // namespace ama::translate
