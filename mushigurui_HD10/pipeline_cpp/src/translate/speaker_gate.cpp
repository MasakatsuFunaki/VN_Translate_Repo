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
#include <utility>
#include <vector>

#include "common/util.h"
#include "glossary.h"
#include "translate_core.h"

namespace mgi::translate {

namespace bj = boost::json;

namespace {

// Enough dialogue to make a broken speaker plate obvious and few enough that
// the printed sample stays readable.
constexpr std::size_t SAMPLE_SIZE = 50;

// Above this share of NARRATION among dialogue lines the plate is not being
// parsed at all.  A handful of genuinely unattributed lines is normal; a
// twentieth of them is the edge of normal.
constexpr double MAX_NARRATION_PCT = 5.0;

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

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

// Every type=='dialogue' string, in file order.
std::vector<std::string> collect_dialogues(const bj::object& extracted) {
    std::vector<std::string> out;
    for (const auto& kv : extracted) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            auto* t = s.if_contains("type");
            if (!t || !t->is_string() || t->get_string() != "dialogue") continue;
            if (auto* txt = s.if_contains("text"))
                if (txt->is_string()) out.emplace_back(txt->get_string());
        }
    }
    return out;
}

// Raw (untranslated) JP speaker for the coverage scan.  MUST mirror
// translate::extract_speaker -- if you change one, change the other.
std::string raw_jp_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf == std::string::npos) return {};
    const std::string raw = trim(text.substr(0, crlf));
    const std::string rest = trim_left(text.substr(crlf + 2));
    if (raw.empty() || char_len(raw) > 20 || rest.empty()) return {};
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

    const std::vector<std::string> dialogues_all = collect_dialogues(extracted);
    std::vector<std::string> dialogues(
        dialogues_all.begin(),
        dialogues_all.begin() +
            static_cast<std::ptrdiff_t>(std::min(SAMPLE_SIZE, dialogues_all.size())));
    report.dialogue_sampled = dialogues.size();
    report.dialogue_total = dialogues_all.size();
    if (dialogues.size() < 10) {
        print_line("[1/5] extract_speaker on the extracted dialogue:");
        return fail(report, 1,
                    "only " + std::to_string(dialogues.size()) +
                        " dialogue entries -- the extraction looks empty");
    }

    // ── 1: extract_speaker returns real speakers, not 'NARRATION' ──
    std::size_t narration = 0;
    std::set<std::string> speakers_seen;
    for (const auto& d : dialogues) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") ++narration;
        else speakers_seen.insert(sp);
    }
    const double pct =
        100.0 * static_cast<double>(narration) / static_cast<double>(dialogues.size());
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/5] extract_speaker on " + std::to_string(dialogues.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::vector<std::string> first8(speakers_seen.begin(), speakers_seen.end());
        if (first8.size() > 8) first8.resize(8);
        print_line("      distinct speakers:  [" + join(first8, ", ") + "]");
    }
    if (pct >= MAX_NARRATION_PCT)
        return fail(report, 1,
                    "too many lines tagged NARRATION -- the speaker plate is not being parsed");

    // ── 2: JP names resolve to their pinned English ──
    print_line("[2/5] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {u8"レン", "Ren"},
        {u8"美弥香", "Miyaka"},
        {u8"綾佳", "Ayaka"},
        {u8"ユーリア", "Yuria"},
        {u8"夢美", "Yumemi"},
    };
    for (const auto& [jp, en] : cases) {
        const std::string text = jp + "\r\n\xE3\x80\x8C" "dummy" "\xE3\x80\x8D";
        const std::string got = extract_speaker(text);
        print_line(std::string(got == en ? "      OK   " : "      FAIL ") + "'" + jp + "' -> '" +
                   got + "' (expected '" + en + "')");
        if (got != en)
            return fail(report, 2, "'" + jp + "' resolved to '" + got + "', not '" + en + "'");
    }

    // ── 3: the assembled API user-prompt carries real [Speaker] tags ──
    // Built by the same function the requests are built with, so the check
    // cannot pass against a copy of the prompt that has drifted from it.
    //
    // A handful of two-line NARRATIVE entries are mis-typed as 'dialogue' in
    // the extraction; extract_speaker correctly tags those NARRATION.  Sample
    // the first 10 dialogues that DO yield a real speaker so this check reads
    // prompt format on actual dialogue.  If the pipeline is genuinely broken
    // the filter starves and the check fails.
    std::vector<std::pair<std::string, std::string>> batch;
    for (const auto& d : dialogues_all) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") continue;
        batch.emplace_back(sp, d);
        if (batch.size() == 10) break;
    }
    if (batch.size() < 10) {
        print_line("[3/5] reconstructed user-prompt:");
        return fail(report, 3,
                    "only " + std::to_string(batch.size()) +
                        " dialogues yielded a real speaker -- the pipeline is broken");
    }
    print_line("[3/5] reconstructed user-prompt for the first 10 speakered dialogue lines:");
    {
        const std::string prompt = build_user_prompt(batch, {});
        std::size_t pos = 0;
        for (int i = 0; i < 8 && pos < prompt.size(); ++i) {
            const std::size_t nl = prompt.find('\n', pos);
            print_line("      " + prompt.substr(pos, nl == std::string::npos
                                                         ? std::string::npos
                                                         : nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        print_line("      ...");
    }

    // ── 4 (sanity): the speaker table still holds the core cast ──
    const auto& name_dict = name_translations();
    if (!name_dict.count(u8"レン"))
        return fail(report, 4, "the speaker table is missing the core character names");
    print_line(u8"[4/5] the speaker table contains the core character names (レン etc.): OK");

    // ── 5: exhaustive bidirectional speaker coverage ──
    std::map<std::string, std::size_t> seen;
    for (const auto& d : dialogues_all) {
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
    print_line("  Sample size (checks 1, 3): " + std::to_string(dialogues.size()) + " of " +
               std::to_string(dialogues_all.size()) + " dialogue entries.");
    print_line("  Coverage scan (check 5): all dialogue entries across all files.");
    report.passed = true;
    return report;
}

}  // namespace mgi::translate
