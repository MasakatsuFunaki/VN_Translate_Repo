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
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace bj = boost::json;

namespace crc::translate {
namespace {

constexpr std::size_t SAMPLE_SIZE = 50;  // first N dialogue entries (checks 1 & 3)

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

// Quoted display form: single quotes unless the value contains a single quote
// and no double quote; backslash and the chosen quote escaped; ASCII control
// characters escaped; printable non-ASCII passes through raw so JP names stay
// legible.  NAME_TRANSLATIONS holds "Taichi's Voice" and "Tomoki's Corpse", so
// the double-quote branch really does fire.
std::string quote_repr(const std::string& s) {
    const bool has_sq = s.find('\'') != std::string::npos;
    const bool has_dq = s.find('"') != std::string::npos;
    const char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        const char32_t cp = utf8_next(s, i);
        if (cp == '\\') out += "\\\\";
        else if (cp == static_cast<char32_t>(q)) { out += '\\'; out += q; }
        else if (cp == '\n') out += "\\n";
        else if (cp == '\r') out += "\\r";
        else if (cp == '\t') out += "\\t";
        else if (cp < 0x20 || cp == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned>(cp));
            out += buf;
        } else {
            out.append(s, start, i - start);
        }
    }
    out += q;
    return out;
}

// Field width counts CODEPOINTS, not bytes: '太一' repr'd is 4 wide, not 8, so
// the JP column lines up instead of being pushed three cells right per kanji.
std::string pad_left(const std::string& s, std::size_t width) {
    const std::size_t n = char_len(s);
    return n >= width ? s : std::string(width - n, ' ') + s;
}
std::string pad_right(const std::string& s, std::size_t width) {
    const std::size_t n = char_len(s);
    return n >= width ? s : s + std::string(width - n, ' ');
}

// "['a', 'b']" -- the same quoting as quote_repr, comma-space separated.
std::string quote_list_repr(const std::vector<std::string>& v) {
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

// Every type=='dialogue' entry object, in file order.  The whole object is
// kept, not just the text: extract_speaker reads the `speaker` field.
std::vector<const bj::object*> collect_dialogues(const bj::object& data) {
    std::vector<const bj::object*> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            if (!sv.is_object()) continue;
            const auto& s = sv.get_object();
            if (json_str(s, "type") == "dialogue") out.push_back(&s);
        }
    }
    return out;
}

// Raw (untranslated) JP speaker for the coverage scan.  MUST mirror
// translate::extract_speaker -- if you change one, change the other.
std::string raw_jp_speaker(const bj::object& s) {
    std::string raw = trim(json_str(s, "speaker"));
    if (raw.empty()) {
        const std::string text = json_str(s, "text");
        const std::size_t crlf = text.find("\r\n");
        if (crlf != std::string::npos) raw = trim(text.substr(0, crlf));
    }
    return (!raw.empty() && char_len(raw) <= 20) ? raw : std::string();
}

std::string build_gate_prompt(const std::vector<const bj::object*>& dialogs) {
    std::string prompt = "<lines_to_translate>\n";
    for (std::size_t i = 0; i < dialogs.size(); ++i) {
        const auto& s = *dialogs[i];
        const std::string speaker = json_str(s, "type") == "dialogue"
                                        ? extract_speaker(s)
                                        : std::string("NARRATION");
        const std::string text = json_str(s, "text");
        std::string escaped;
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

SpeakerCheckReport fail(SpeakerCheckReport report, int check, const std::string& reason) {
    report.passed = false;
    report.failed_check = check;
    report.reason = reason;
    return report;
}

}  // namespace

SpeakerCheckReport run_speaker_checks(const bj::object& extracted) {
    SpeakerCheckReport report;

    const std::vector<const bj::object*> dialogs_all = collect_dialogues(extracted);
    report.dialogue_total = dialogs_all.size();

    std::vector<const bj::object*> dialogs(
        dialogs_all.begin(),
        dialogs_all.begin() +
            static_cast<std::ptrdiff_t>(std::min(SAMPLE_SIZE, dialogs_all.size())));
    report.dialogue_sampled = dialogs.size();
    if (dialogs.size() < 10) {
        const std::string why = "only " + std::to_string(dialogs.size()) +
                                " dialogue entries; extracted_text.json looks empty.";
        print_line("FAIL: " + why);
        return fail(report, 1, why);
    }

    // -- Check 1: extract_speaker returns real speakers, not 'NARRATION' --
    std::size_t narration = 0;
    std::set<std::string> speakers_set;
    for (const auto* d : dialogs) {
        const std::string sp = extract_speaker(*d);
        if (sp == "NARRATION") ++narration;
        speakers_set.insert(sp);
    }
    speakers_set.erase("NARRATION");
    const double pct =
        100.0 * static_cast<double>(narration) / static_cast<double>(dialogs.size());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/5] extract_speaker on " + std::to_string(dialogs.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::vector<std::string> first8(speakers_set.begin(), speakers_set.end());
        if (first8.size() > 8) first8.resize(8);
        print_line("      distinct speakers:  " + quote_list_repr(first8));
    }
    if (pct >= 5.0) {
        print_line("      FAIL: too many lines tagged NARRATION \xE2\x80\x94 "
                   "speaker field not flowing.");
        return fail(report, 1,
                    std::string(buf) + "% of sampled dialogue lines tagged NARRATION.");
    }

    // -- Check 2: JP names map to EN via NAME_TRANSLATIONS --
    print_line("[2/5] JP \xE2\x86\x92 EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"\xE5\xA4\xAA\xE4\xB8\x80", "Taichi"},  // 太一
        {"\xE8\xA6\x8B\xE9\x87\x8C", "Misato"},  // 見里
        {"\xE9\x9C\xA7", "Kiri"},                // 霧
        {"\xE5\x86\xAC\xE5\xAD\x90", "Touko"},   // 冬子
        {"\xE7\xBE\x8E\xE5\xB8\x8C", "Miki"},    // 美希
    };
    for (const auto& [jp, en] : cases) {
        bj::object s;
        s["speaker"] = jp;
        s["text"] = "\xE3\x80\x8C" "dummy" "\xE3\x80\x8D";  // 「dummy」
        s["type"] = "dialogue";
        const std::string got = extract_speaker(s);
        print_line(std::string("      ") + (got == en ? "OK " : "FAIL") + "  " +
                   pad_left(quote_repr(jp), 8) + " -> " + pad_right(quote_repr(got), 10) +
                   " (expected " + quote_repr(en) + ")");
        if (got != en)
            return fail(report, 2,
                        quote_repr(jp) + " resolved to " + quote_repr(got) + ", expected " +
                            quote_repr(en) + ".");
    }

    // -- Check 3: assembled API user-prompt contains real [Speaker] tags --
    // The first 10 dialogue entries verbatim -- no pre-filtering, unlike the
    // BLACKCyc ports: CROSS_CHANNEL's speaker field is always populated.
    {
        std::vector<const bj::object*> first10(
            dialogs.begin(),
            dialogs.begin() +
                static_cast<std::ptrdiff_t>(std::min<std::size_t>(10, dialogs.size())));
        const std::string prompt = build_gate_prompt(first10);

        std::vector<std::string> lines;
        std::size_t pos = 0;
        for (;;) {
            const std::size_t nl = prompt.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(prompt.substr(pos));
                break;
            }
            lines.push_back(prompt.substr(pos, nl - pos));
            pos = nl + 1;
        }
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
                            " of the first 10 dialogue lines reach the prompt as [NARRATION].");
        }
    }

    // -- Check 4 (sanity): NAME_TRANSLATIONS pre-population works --
    const auto& name_dict = name_translations();
    report.glossary_entries = name_dict.size();
    if (!name_dict.count("\xE5\xA4\xAA\xE4\xB8\x80")) {  // 太一
        print_line("FAIL: NAME_TRANSLATIONS missing core character names.");
        return fail(report, 4, "NAME_TRANSLATIONS is missing the core character names.");
    }
    print_line("[4/5] NAME_TRANSLATIONS contains core character names "
               "(\xE5\xA4\xAA\xE4\xB8\x80 etc.): OK");

    // -- Check 5: exhaustive bidirectional speaker coverage --
    // std::map keeps keys in byte order, which for UTF-8 equals codepoint
    // order -- so `missing` and `unused` come out pre-sorted, which matters
    // because the frequency re-sort below is STABLE and would otherwise leave
    // equal-count names in an arbitrary order that churns between runs.
    std::map<std::string, long long> seen;
    for (const auto* d : dialogs_all) {
        const std::string raw = raw_jp_speaker(*d);
        if (!raw.empty()) ++seen[raw];
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
    print_line("      NAME_TRANSLATIONS entries:    " + std::to_string(name_dict.size()));

    if (!missing.empty() || !unused.empty()) {
        std::string why;
        if (!missing.empty()) {
            print_line("      FAIL: " + std::to_string(missing.size()) +
                       " JP speakers in data not in NAME_TRANSLATIONS");
            print_line("            (copy-paste-ready, sorted by frequency):");
            std::stable_sort(missing.begin(), missing.end(),
                             [&](const std::string& a, const std::string& b) {
                                 return seen[a] > seen[b];
                             });
            for (std::size_t i = 0; i < missing.size() && i < 30; ++i)
                print_line("          " + pad_left(quote_repr(missing[i]), 14) +
                           ": '???',  # appeared " + std::to_string(seen[missing[i]]) + "x");
            if (missing.size() > 30)
                print_line("          ... and " + std::to_string(missing.size() - 30) + " more");
            why = std::to_string(missing.size()) +
                  " JP speakers in the data have no NAME_TRANSLATIONS entry";
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " NAME_TRANSLATIONS entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < 30; ++i)
                print_line("          " + quote_repr(unused[i]) + " -> " +
                           quote_repr(name_dict.at(unused[i])));
            if (unused.size() > 30)
                print_line("          ... and " + std::to_string(unused.size() - 30) + " more");
            if (!why.empty()) why += "; ";
            why += std::to_string(unused.size()) +
                   " NAME_TRANSLATIONS entries never appear as a speaker";
        }
        return fail(report, 5, why + ".");
    }

    print_line("      OK \xE2\x80\x94 full bidirectional coverage.");
    print_line("");
    print_line("  All checks passed.  Speaker pipeline is healthy \xE2\x80\x94 "
               "safe to run translation.");
    print_line("  Sample size (checks 1, 3): " + std::to_string(dialogs.size()) + " of " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");
    print_line("  Coverage scan (check 5):  all " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");

    report.passed = true;
    return report;
}

}  // namespace crc::translate
