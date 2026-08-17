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

namespace frat::translate {

namespace bj = boost::json;

namespace {

// Only prose types can carry a speaker.
const std::set<std::string> TRANSLATABLE_TYPES = {"dialogue", "narrative"};
constexpr std::size_t SAMPLE_SCAN = 5000;    // entries scanned for check 1
constexpr std::size_t SAMPLE_PROMPT = 50;    // entries used for check 3
constexpr std::size_t MIN_SPEAKERS_FOUND = 10;
constexpr int HIGH_FREQ_THRESHOLD = 3;
constexpr std::size_t MAX_LISTED = 30;

std::string list_repr(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += quote_repr(v[i]);
    }
    return out + "]";
}

std::string json_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key))
        if (v->is_string()) return std::string(v->get_string());
    return {};
}

std::vector<std::string> collect_entries(const bj::object& data) {
    std::vector<std::string> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            if (!TRANSLATABLE_TYPES.count(json_str(s, "type"))) continue;
            out.push_back(json_str(s, "text"));
        }
    }
    return out;
}

std::string escape_line(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 2, "\r\n") == 0) { out += "\\r\\n"; ++i; }
        else if (text[i] == '\n') out += "\\n";
        else out += text[i];
    }
    return out;
}

std::string build_gate_prompt(const std::vector<std::string>& entries) {
    std::string prompt = "<lines_to_translate>\n";
    for (std::size_t i = 0; i < entries.size(); ++i)
        prompt += std::to_string(i + 1) + ". [" + extract_speaker(entries[i]) + "] " +
                  escape_line(entries[i]) + "\n";
    prompt += "</lines_to_translate>\n";
    return prompt;
}

// Must mirror translate::extract_speaker.
std::string raw_jp_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf != std::string::npos) {
        const std::string raw = trim(text.substr(0, crlf));
        if (!raw.empty() && char_len(raw) <= 20) return raw;
    }
    return inline_speaker_match(text).value_or(std::string());
}

SpeakerCheckReport fail(SpeakerCheckReport report, int check, const std::string& reason) {
    report.passed = false;
    report.failed_check = check;
    report.reason = reason;
    return report;
}

}  // namespace

SpeakerCheckReport run_speaker_checks(const bj::object& extracted) {
    SpeakerCheckReport report;

    const std::vector<std::string> entries_all = collect_entries(extracted);
    report.entries_total = entries_all.size();
    if (entries_all.size() < 100) {
        print_line("FAIL: only " + std::to_string(entries_all.size()) +
                   " translatable entries; extracted_text.json looks empty.");
        return fail(report, 1,
                    "only " + std::to_string(entries_all.size()) +
                        " translatable entries -- the extraction looks empty");
    }

    // ── Check 1: extract_speaker discovers real speakers, not all NARRATION ──
    const std::size_t sample_n = std::min(SAMPLE_SCAN, entries_all.size());
    report.entries_sampled = sample_n;
    std::set<std::string> distinct_set;
    std::size_t n_real = 0;
    for (std::size_t i = 0; i < sample_n; ++i) {
        const std::string sp = extract_speaker(entries_all[i]);
        if (sp == "NARRATION") continue;
        ++n_real;
        distinct_set.insert(sp);
    }
    const std::vector<std::string> distinct_real(distinct_set.begin(), distinct_set.end());
    print_line("[1/5] extract_speaker on first " + std::to_string(sample_n) +
               " translatable entries:");
    print_line("      real-speaker hits:  " + std::to_string(n_real));
    const std::vector<std::string> first10(
        distinct_real.begin(),
        distinct_real.begin() +
            static_cast<std::ptrdiff_t>(std::min<std::size_t>(10, distinct_real.size())));
    print_line("      distinct speakers:  " + list_repr(first10));
    if (distinct_real.size() < MIN_SPEAKERS_FOUND) {
        print_line("      FAIL: only " + std::to_string(distinct_real.size()) +
                   " distinct speakers found (need \xE2\x89\xA5" +
                   std::to_string(MIN_SPEAKERS_FOUND) +
                   "). _INLINE_SPEAKER_RE may be broken.");
        return fail(report, 1,
                    "only " + std::to_string(distinct_real.size()) +
                        " distinct speakers found -- the inline speaker parse is broken");
    }

    // ── Check 2: JP names map to EN via NAME_TRANSLATIONS ──
    print_line("[2/5] JP \xE2\x86\x92 EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"大智", "Daichi"}, {"紗英子", "Saeko"}, {"美桜", "Mio"},
        {"愛", "Ai"},       {"芽生", "Mei"},
    };
    for (const auto& [jp, en] : cases) {
        const std::string text = jp + "\xE3\x80\x8C" "dummy" "\xE3\x80\x8D";
        const std::string got = extract_speaker(text);
        print_line("      " + std::string(got == en ? "OK " : "FAIL") + "  " +
                   pad_left_cp(quote_repr(jp), 8) + " -> " + pad_right_cp(quote_repr(got), 10) +
                   " (expected " + quote_repr(en) + ")");
        if (got != en)
            return fail(report, 2,
                        quote_repr(jp) + " resolved to " + quote_repr(got) + ", not " +
                            quote_repr(en));
    }

    // ── Check 3: assembled API user-prompt contains real [Speaker] tags ──
    std::vector<std::string> speakered;
    for (const auto& text : entries_all) {
        if (extract_speaker(text) == "NARRATION") continue;
        speakered.push_back(text);
        if (speakered.size() == SAMPLE_PROMPT) break;
    }
    if (speakered.size() < 10) {
        print_line("[3/5] FAIL: only " + std::to_string(speakered.size()) +
                   " entries with detected speaker \xE2\x80\x94 "
                   "too few for a meaningful prompt sample.");
        return fail(report, 3,
                    "only " + std::to_string(speakered.size()) +
                        " entries with a detected speaker -- too few for a prompt sample");
    }
    {
        const std::vector<std::string> first_ten(speakered.begin(), speakered.begin() + 10);
        const std::string prompt = build_gate_prompt(first_ten);
        std::vector<std::string> lines;
        std::size_t pos = 0;
        for (;;) {
            const std::size_t nl = prompt.find('\n', pos);
            if (nl == std::string::npos) { lines.push_back(prompt.substr(pos)); break; }
            lines.push_back(prompt.substr(pos, nl - pos));
            pos = nl + 1;
        }
        std::size_t bad = 0;
        for (const auto& l : lines)
            if (l.find("[NARRATION]") != std::string::npos) ++bad;
        print_line("[3/5] reconstructed user-prompt for first 10 speakered entries:");
        for (std::size_t i = 0; i < lines.size() && i < 8; ++i) print_line("      " + lines[i]);
        print_line("      ...");
        if (bad) {
            print_line("      FAIL: " + std::to_string(bad) +
                       " entries tagged [NARRATION] in prompt.");
            return fail(report, 3,
                        std::to_string(bad) + " entries tagged [NARRATION] in the prompt");
        }
    }

    // ── Check 4 (sanity): NAME_TRANSLATIONS pre-population works ──
    const auto& name_dict = name_translations();
    report.glossary_entries = name_dict.size();
    if (!name_dict.count("大智")) {
        print_line("FAIL: NAME_TRANSLATIONS missing core character names.");
        return fail(report, 4, "the speaker table is missing the core character names");
    }
    print_line("[4/5] NAME_TRANSLATIONS contains core character names (大智 etc.): OK");

    // ── Check 5: exhaustive bidirectional speaker coverage ──
    std::map<std::string, int> seen;
    for (const auto& text : entries_all) {
        const std::string raw = raw_jp_speaker(text);
        if (!raw.empty()) ++seen[raw];
    }
    report.speakers_in_data = seen.size();
    std::vector<std::string> missing_all, missing_high_freq, missing_low_freq, unused;
    for (const auto& [jp, n] : seen)  // std::map iterates sorted, as the report wants
        if (!name_dict.count(jp)) missing_all.push_back(jp);
    for (const auto& jp : missing_all)
        (seen[jp] >= HIGH_FREQ_THRESHOLD ? missing_high_freq : missing_low_freq).push_back(jp);
    for (const auto& [jp, en] : name_translations_ordered())
        if (!seen.count(jp)) unused.push_back(jp);
    std::sort(unused.begin(), unused.end());

    print_line("[5/5] exhaustive speaker coverage scan:");
    print_line("      distinct JP speakers in data: " + std::to_string(seen.size()));
    print_line("      NAME_TRANSLATIONS entries:    " + std::to_string(name_dict.size()));
    if (!missing_low_freq.empty())
        print_line("      (ignoring " + std::to_string(missing_low_freq.size()) +
                   " one-off inline-regex matches with freq < " +
                   std::to_string(HIGH_FREQ_THRESHOLD) +
                   " \xE2\x80\x94 likely prose fragments)");

    if (missing_high_freq.empty() && unused.empty()) {
        print_line("      OK \xE2\x80\x94 full bidirectional coverage.");
    } else {
        if (!missing_high_freq.empty()) {
            print_line("      FAIL: " + std::to_string(missing_high_freq.size()) +
                       " recurring JP speakers in data not in NAME_TRANSLATIONS");
            print_line("            (copy-paste-ready, sorted by frequency):");
            std::vector<std::string> by_freq = missing_high_freq;
            std::stable_sort(by_freq.begin(), by_freq.end(),
                             [&](const std::string& a, const std::string& b) {
                                 return seen[a] > seen[b];
                             });
            for (std::size_t i = 0; i < by_freq.size() && i < MAX_LISTED; ++i)
                print_line("          " + pad_left_cp(quote_repr(by_freq[i]), 14) + ": '???',  "
                           "# appeared " + std::to_string(seen[by_freq[i]]) + "x");
            if (missing_high_freq.size() > MAX_LISTED)
                print_line("          ... and " +
                           std::to_string(missing_high_freq.size() - MAX_LISTED) + " more");
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " NAME_TRANSLATIONS entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < MAX_LISTED; ++i)
                print_line("          " + quote_repr(unused[i]) + " -> " +
                           quote_repr(name_dict.at(unused[i])));
            if (unused.size() > MAX_LISTED)
                print_line("          ... and " + std::to_string(unused.size() - MAX_LISTED) +
                           " more");
        }
        return fail(report, 5,
                    std::to_string(missing_high_freq.size()) +
                        " recurring speakers missing from the table and " +
                        std::to_string(unused.size()) + " table entries never spoken");
    }

    print_line("");
    print_line("  All checks passed.  Speaker pipeline is healthy \xE2\x80\x94 "
               "safe to run translation.");
    print_line("  Check 1 sample: " + std::to_string(sample_n) + " of " +
               comma(static_cast<long long>(entries_all.size())) + " translatable entries.");
    print_line("  Coverage scan (check 5):  all " +
               comma(static_cast<long long>(entries_all.size())) + " translatable entries.");
    report.passed = true;
    return report;
}

}  // namespace frat::translate
