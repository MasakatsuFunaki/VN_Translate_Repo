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

namespace shin::translate {

namespace bj = boost::json;

namespace {

constexpr std::size_t SAMPLE_SIZE = 50;  // first N dialogue entries (checks 1 & 3)

std::string json_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key))
        if (v->is_string()) return std::string(v->get_string());
    return {};
}

// Quoted display form for a value: single quotes unless it contains ' and no ",
// and printable non-ASCII (i.e. every Japanese name here) stays literal rather
// than being escaped -- the report is read by humans checking the names.
std::string quote_repr(const std::string& s) {
    const bool has_sq = s.find('\'') != std::string::npos;
    const bool has_dq = s.find('"') != std::string::npos;
    const char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == q) { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    out += q;
    return out;
}

// Column padding counts CODEPOINTS, and the quotes count toward the width:
// quote_repr('マイケル') is 6 chars, so a width of 10 emits 4 spaces.  A byte-width
// implementation would emit none (12 bytes > 10) and the columns would collapse
// on exactly the Japanese rows this report exists to show.
std::string rjust(const std::string& s, std::size_t width) {
    const std::size_t n = char_len(s);
    return n >= width ? s : std::string(width - n, ' ') + s;
}
std::string ljust(const std::string& s, std::size_t width) {
    const std::size_t n = char_len(s);
    return n >= width ? s : s + std::string(width - n, ' ');
}

// List display form: ['a', 'b'] -- square brackets, ", " separator, quoted.
std::string list_repr(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += quote_repr(v[i]);
    }
    return out + "]";
}

// Every type=='dialogue' record, whole object, in file order.
std::vector<bj::object> collect_dialogues(const bj::object& data) {
    std::vector<bj::object> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            if (json_str(s, "type") == "dialogue") out.push_back(s);
        }
    }
    return out;
}

// Raw (untranslated) JP speaker for the coverage scan.  MUST mirror
// translate::extract_speaker -- if you change one, change the other.
// char_len, not size(): a byte test would drop the five speakers longer than
// 20 bytes and then report them as `unused` glossary entries.
std::string raw_jp_speaker(const bj::object& s) {
    const std::string raw = trim(json_str(s, "speaker"));
    return (!raw.empty() && char_len(raw) <= 20) ? raw : std::string();
}

// Reproduce exactly what call_anthropic would put in the API user message.
std::string build_gate_prompt(const std::vector<bj::object>& dialogs) {
    std::string prompt = "<lines_to_translate>\n";
    for (std::size_t i = 0; i < dialogs.size(); ++i) {
        const std::string speaker = json_str(dialogs[i], "type") == "dialogue"
                                        ? extract_speaker(dialogs[i])
                                        : std::string("NARRATION");
        // A no-op on this game's data (its line breaks are a literal
        // backslash-n), but call_anthropic escapes real CR/LF and this
        // reconstruction has to match what it would send, character for
        // character, or the assertions below prove nothing.
        std::string escaped;
        const std::string text = json_str(dialogs[i], "text");
        for (std::size_t k = 0; k < text.size(); ++k) {
            if (text.compare(k, 2, "\r\n") == 0) { escaped += "\\r\\n"; ++k; }
            else if (text[k] == '\n') escaped += "\\n";
            else escaped += text[k];
        }
        prompt += std::to_string(i + 1) + ". [" + speaker + "] " + escaped + "\n";
    }
    prompt += "</lines_to_translate>\n";
    return prompt;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) { out.push_back(s.substr(pos)); return out; }
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

SpeakerCheckReport fail(SpeakerCheckReport report, int check, std::string reason) {
    report.passed = false;
    report.failed_check = check;
    report.reason = std::move(reason);
    return report;
}

}  // namespace

SpeakerCheckReport run_speaker_checks(const bj::object& data) {
    SpeakerCheckReport report;

    const std::vector<bj::object> dialogs_all = collect_dialogues(data);
    std::vector<bj::object> dialogs(
        dialogs_all.begin(),
        dialogs_all.begin() +
            static_cast<std::ptrdiff_t>(std::min(SAMPLE_SIZE, dialogs_all.size())));
    report.dialogue_total = dialogs_all.size();
    report.dialogue_sampled = dialogs.size();
    if (dialogs.size() < 10) {
        print_line("FAIL: only " + std::to_string(dialogs.size()) +
                   " dialogue entries; extracted_text.json looks empty.");
        return fail(report, 1,
                    "only " + std::to_string(dialogs.size()) +
                        " dialogue entries; the extraction looks empty");
    }

    // ── Check 1: extract_speaker returns real speakers, not 'NARRATION' ──
    std::size_t narration = 0;
    std::set<std::string> speakers_seen;
    for (const auto& s : dialogs) {
        const std::string sp = extract_speaker(s);
        if (sp == "NARRATION") ++narration;
        else speakers_seen.insert(sp);
    }
    const double pct = 100.0 * static_cast<double>(narration) /
                       static_cast<double>(dialogs.size());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/5] extract_speaker on " + std::to_string(dialogs.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::vector<std::string> first8(speakers_seen.begin(), speakers_seen.end());
        if (first8.size() > 8) first8.resize(8);
        print_line("      distinct speakers:  " + list_repr(first8));
    }
    if (pct >= 5.0) {
        print_line("      FAIL: too many lines tagged NARRATION — speaker field not flowing.");
        return fail(report, 1,
                    std::string(buf) + "% of sampled dialogue is tagged NARRATION; "
                                       "the speaker field is not reaching the prompt");
    }

    // ── Check 2: JP names map to EN via NAME_TRANSLATIONS ──
    print_line("[2/5] JP → EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"マイケル", "Michael"},   {"ニール", "Neil"}, {"セシル", "Cecil"},
        {"オーガスト", "August"},  {"ガビィ", "Gabby"},
    };
    for (const auto& [jp, en] : cases) {
        bj::object s;
        s["speaker"] = jp;
        s["text"] = "「dummy」";
        s["type"] = "dialogue";
        const std::string got = extract_speaker(s);
        const std::string marker = got == en ? "OK " : "FAIL";
        print_line("      " + marker + "  " + rjust(quote_repr(jp), 10) + " -> " +
                   ljust(quote_repr(got), 10) + " (expected " + quote_repr(en) + ")");
        if (got != en)
            return fail(report, 2,
                        quote_repr(jp) + " resolved to " + quote_repr(got) + ", expected " +
                            quote_repr(en));
    }

    // ── Check 3: assembled API user-prompt contains real [Speaker] tags ──
    // The FIRST 10 dialogue entries, unfiltered -- shingakkou's speaker field
    // is populated for all of them, so there is nothing to filter for.
    {
        std::vector<bj::object> first10(
            dialogs.begin(),
            dialogs.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(10, dialogs.size())));
        const std::string prompt = build_gate_prompt(first10);
        const auto lines = split_lines(prompt);
        std::size_t bad = 0;
        for (const auto& l : lines)
            if (l.find("[NARRATION]") != std::string::npos) ++bad;
        print_line("[3/5] reconstructed user-prompt for first 10 dialogue lines:");
        for (std::size_t i = 0; i < lines.size() && i < 8; ++i) print_line("      " + lines[i]);
        print_line("      ...");
        if (bad) {
            print_line("      FAIL: " + std::to_string(bad) +
                       " dialogue lines tagged [NARRATION] in prompt.");
            return fail(report, 3,
                        std::to_string(bad) +
                            " of the first 10 dialogue lines reach the prompt as [NARRATION]");
        }
    }

    // ── Check 4 (sanity): assert NAME_TRANSLATIONS pre-population works ──
    const auto& name_dict = name_translations();
    report.glossary_entries = name_dict.size();
    if (!name_dict.count("マイケル")) {
        print_line("FAIL: NAME_TRANSLATIONS missing core character names.");
        return fail(report, 4, "NAME_TRANSLATIONS is missing the core character names");
    }
    print_line("[4/5] NAME_TRANSLATIONS contains core character names (マイケル etc.): OK");

    // ── Check 5: exhaustive bidirectional speaker coverage ──
    // Every JP speaker in the data MUST be a glossary key, and every glossary
    // key MUST appear at least once as a speaker.  Both directions hard-fail.
    std::map<std::string, int> seen;
    for (const auto& s : dialogs_all) {
        const std::string raw = raw_jp_speaker(s);
        if (!raw.empty()) ++seen[raw];
    }
    report.speakers_in_data = seen.size();

    // std::map already gives codepoint order here: UTF-8 byte order and
    // codepoint order agree.
    std::vector<std::string> missing, unused;
    for (const auto& [jp, n] : seen)
        if (!name_dict.count(jp)) missing.push_back(jp);
    for (const auto& [jp, en] : name_translations_ordered())
        if (!seen.count(jp)) unused.push_back(jp);
    std::sort(unused.begin(), unused.end());

    print_line("[5/5] exhaustive speaker coverage scan:");
    print_line("      distinct JP speakers in data: " + std::to_string(seen.size()));
    print_line("      NAME_TRANSLATIONS entries:    " + std::to_string(name_dict.size()));

    if (!missing.empty() || !unused.empty()) {
        if (!missing.empty()) {
            print_line("      FAIL: " + std::to_string(missing.size()) +
                       " JP speakers in data not in NAME_TRANSLATIONS");
            print_line("            (copy-paste-ready, sorted by frequency):");
            // The list is ALREADY alphabetical, and the sort by descending
            // count is stable, so equal counts keep alphabetical order.
            std::stable_sort(missing.begin(), missing.end(),
                             [&](const std::string& a, const std::string& b) {
                                 return seen[a] > seen[b];
                             });
            for (std::size_t i = 0; i < missing.size() && i < 30; ++i)
                print_line("          " + rjust(quote_repr(missing[i]), 14) +
                           ": '???',  # appeared " + std::to_string(seen[missing[i]]) + "x");
            if (missing.size() > 30)
                print_line("          ... and " + std::to_string(missing.size() - 30) + " more");
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " NAME_TRANSLATIONS entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < 30; ++i)
                print_line("          " + quote_repr(unused[i]) + " -> " +
                           quote_repr(name_dict.at(unused[i])));
            if (unused.size() > 30)
                print_line("          ... and " + std::to_string(unused.size() - 30) + " more");
        }
        return fail(report, 5,
                    std::to_string(missing.size()) + " speaker(s) missing from the glossary and " +
                        std::to_string(unused.size()) + " glossary entry(ies) never used");
    }
    print_line("      OK — full bidirectional coverage.");

    print_line("");
    print_line("  All checks passed.  Speaker pipeline is healthy — safe to run translation.");
    // The wording of these two lines is diffed run against run; leave it alone.
    print_line("  Sample size (tests 1, 3): " + std::to_string(dialogs.size()) + " of " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");
    print_line("  Coverage scan (test 5):  all " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");

    report.passed = true;
    return report;
}

}  // namespace shin::translate
