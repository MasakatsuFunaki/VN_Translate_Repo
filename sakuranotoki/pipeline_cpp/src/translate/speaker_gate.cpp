// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate/speaker_gate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace bj = boost::json;

namespace snt::translate {

namespace {

constexpr std::size_t SAMPLE_SIZE = 50;  // first N dialogue entries (checks 1 & 3)

struct Row {
    std::string script;
    std::size_t idx;
    std::string speaker;
    std::string text;
};

// Walk every script via iter_entries_with_speakers, yielding the first
// `limit` *dialogue* entries (so checks 1 & 3 see only attributed lines).
std::vector<Row> walk_dialogues(const bj::object& data, std::size_t limit) {
    std::vector<Row> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        const auto annotated = iter_entries_with_speakers(strings->get_array());
        for (std::size_t i = 0; i < annotated.size(); ++i) {
            const auto& [speaker, entry] = annotated[i];
            auto* t = entry->if_contains("type");
            if (!t || !t->is_string() || t->get_string() != "dialogue") continue;
            std::string text;
            if (auto* s = entry->if_contains("text"))
                if (s->is_string()) text = std::string(s->get_string());
            out.push_back(Row{std::string(kv.key()), i, speaker, text});
            if (out.size() >= limit) return out;
        }
    }
    return out;
}

// The prompt these rows would be sent in, built by the shipped builder rather
// than by a replica of it.  A replica can agree with the corpus while the
// builder the run uses is broken, which is the one thing this check exists to
// catch.
std::string build_gate_prompt(const std::vector<Row>& rows) {
    std::vector<std::pair<std::string, std::string>> batch;
    batch.reserve(rows.size());
    for (const auto& r : rows) batch.emplace_back(r.speaker, r.text);
    return build_user_prompt(batch, {});
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

// Feed a synthetic [name, dialogue] pair through the state machine and return
// the speaker tag the dialogue inherits.
std::string resolve_via_state_machine(const std::string& jp_name) {
    bj::object name_entry;
    name_entry["type"] = "name";
    name_entry["text"] = jp_name;
    bj::object dlg;
    dlg["type"] = "dialogue";
    // Split literal: MSVC would otherwise read "\x8Cd" as one hex escape.
    dlg["text"] = "\xE3\x80\x8C" "dummy" "\xE3\x80\x8D";
    bj::array entries{std::move(name_entry), std::move(dlg)};
    return iter_entries_with_speakers(entries)[1].first;
}

SpeakerCheckReport fail(SpeakerCheckReport report, int check, std::string reason) {
    report.passed = false;
    report.failed_check = check;
    report.reason = std::move(reason);
    return report;
}

std::string field_of(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key))
        if (v->is_string()) return trim(std::string(v->get_string()));
    return {};
}

// A character a romanised name is written with.  The blank is here because a
// family name and a given name are read back together.
bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\'' || c == ' ';
}

// The English the prose puts immediately in front of `open`, read backwards
// until anything that cannot belong to a name.
std::string english_before(const std::string& text, std::size_t open) {
    std::size_t b = open;
    while (b > 0 && is_name_char(text[b - 1])) --b;
    return trim(text.substr(b, open - b));
}

// Whether `spelling` ends in `name` on a word boundary.  "Nakamura Shouichi"
// ends in "Shouichi", so a family name in front of the given name agrees with
// the table; "Rurio" does not end in "Ruriwo".
bool ends_with_name(const std::string& spelling, const std::string& name) {
    if (spelling.size() < name.size()) return false;
    const std::size_t at = spelling.size() - name.size();
    if (spelling.compare(at, name.size(), name) != 0) return false;
    return at == 0 || spelling[at - 1] == ' ';
}

}  // namespace

std::map<std::string, std::size_t> dialogue_lines_by_speaker(const bj::object& data) {
    std::map<std::string, std::size_t> counts;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        std::string standing;
        for (const auto& sv : strings->get_array()) {
            if (!sv.is_object()) continue;
            const auto& entry = sv.get_object();
            const std::string type = field_of(entry, "type");
            if (type == "title") {
                standing.clear();
                continue;
            }
            if (type == "name") {
                standing = field_of(entry, "text");
                continue;
            }
            if (type != "dialogue") continue;
            // A present field answers, null included: a line the extractor
            // recorded as unattributed must not be counted for whoever spoke
            // last.
            const std::string label =
                entry.if_contains("speaker") ? field_of(entry, "speaker") : standing;
            if (!label.empty()) ++counts[label];
        }
    }
    return counts;
}

std::vector<DossierGap> dossier_gaps(
    const std::map<std::string, std::size_t>& lines_by_label, const std::string& dossier) {
    static const char* const SEX_MARKERS[] = {"**Male**", "**Female**", "he/him",
                                              "she/her"};
    std::vector<std::string> dossier_lines;
    for (std::size_t pos = 0; pos <= dossier.size();) {
        const std::size_t nl = dossier.find('\n', pos);
        dossier_lines.push_back(
            dossier.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    std::vector<DossierGap> gaps;
    for (const auto& [label, lines] : lines_by_label) {
        if (lines < DOSSIER_LINE_THRESHOLD) continue;
        bool named = false;
        bool sexed = false;
        for (const auto& line : dossier_lines) {
            if (line.find(label) == std::string::npos) continue;
            named = true;
            for (const char* marker : SEX_MARKERS)
                if (line.find(marker) != std::string::npos) sexed = true;
            if (sexed) break;
        }
        if (!named)
            gaps.push_back({label, lines, "no dossier line names it"});
        else if (!sexed)
            gaps.push_back({label, lines, "its dossier line states no sex"});
    }
    return gaps;
}

std::vector<DossierNameMismatch> dossier_name_mismatches(const std::string& dossier) {
    std::vector<DossierNameMismatch> out;
    for (const auto& [label, en] : prompt_name_translations()) {
        const std::string opener = "(" + label;
        for (std::size_t pos = dossier.find(opener); pos != std::string::npos;
             pos = dossier.find(opener, pos + 1)) {
            // The label has to end where the opener ends.  "(藍," introduces 藍;
            // a longer word that merely starts with it does not.
            const std::size_t after = pos + opener.size();
            const char next = after < dossier.size() ? dossier[after] : '\0';
            if (next != ',' && next != ')' && next != ' ') continue;
            const std::string spelling = english_before(dossier, pos);
            if (!ends_with_name(spelling, en)) out.push_back({label, spelling, en});
        }
    }
    return out;
}

std::string prompt_dossier() {
    static const char* const OPEN = "<character_dossier>";
    static const char* const CLOSE = "</character_dossier>";
    const std::string& prompt = system_prompt();
    const std::size_t a = prompt.find(OPEN);
    if (a == std::string::npos) return {};
    const std::size_t b = prompt.find(CLOSE, a);
    if (b == std::string::npos) return {};
    const std::size_t start = a + std::strlen(OPEN);
    return prompt.substr(start, b - start);
}

SpeakerCheckReport run_speaker_checks(const bj::object& data) {
    SpeakerCheckReport report;

    auto sample = walk_dialogues(data, SAMPLE_SIZE);
    report.dialogue_sampled = sample.size();
    if (sample.size() < 10) {
        print_line("FAIL: only " + std::to_string(sample.size()) +
                   " dialogue entries found - extracted_text.json looks empty.");
        return fail(report, 1,
                    "only " + std::to_string(sample.size()) +
                        " dialogue entries in the extraction");
    }

    // -- Check 1: state machine attributes dialogue to a real speaker --
    std::size_t n_narration = 0;
    std::set<std::string> distinct_real;
    for (const auto& r : sample) {
        if (r.speaker == "NARRATION") ++n_narration;
        else distinct_real.insert(r.speaker);
    }
    const double pct = 100.0 * static_cast<double>(n_narration) /
                       static_cast<double>(sample.size());
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/6] iter_entries_with_speakers on " + std::to_string(sample.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(n_narration) + " (" + buf + "%)");
    {
        std::vector<std::string> first8(distinct_real.begin(), distinct_real.end());
        if (first8.size() > 8) first8.resize(8);
        print_line("      distinct speakers:  [" + join(first8, ", ") + "]");
    }
    if (pct >= 5.0) {
        print_line("      FAIL: too many dialogue lines tagged NARRATION - "
                   "name->dialogue tracking broken.");
        return fail(report, 1,
                    std::string(buf) + "% of sampled dialogue tagged NARRATION");
    }

    // -- Check 2: NAME entry -> following-dialogue speaker resolution --
    print_line("[2/6] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"\xE7\x9B\xB4\xE5\x93\x89", "Naoya"},       // 直哉
        {"\xE5\xBF\x83\xE9\x88\xB4", "Misuzu"},      // 心鈴
        {"\xE7\x9C\x9F\xE7\x90\xB4", "Makoto"},      // 真琴
        {"\xE9\x9D\x99\xE6\xB5\x81", "Shizuru"},     // 静流
        {"\xE6\x94\xBE\xE5\x93\x89", "Housai"},      // 放哉
    };
    for (const auto& [jp, en] : cases) {
        const std::string got = resolve_via_state_machine(jp);
        print_line(std::string(got == en ? "      OK   " : "      FAIL ") + "'" + jp +
                   "' -> '" + got + "' (expected '" + en + "')");
        if (got != en)
            return fail(report, 2,
                        "'" + jp + "' resolved to '" + got + "', expected '" + en + "'");
    }

    // -- Check 3: assembled API user-prompt contains real [Speaker] tags --
    std::vector<Row> first10(sample.begin(),
                             sample.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min<std::size_t>(10, sample.size())));
    const std::string prompt = build_gate_prompt(first10);
    print_line("[3/6] reconstructed user-prompt for first 10 dialogue lines:");
    std::size_t bad = 0;
    {
        std::size_t pos = 0;
        int shown = 0;
        while (pos <= prompt.size()) {
            const std::size_t nl = prompt.find('\n', pos);
            const std::string line =
                prompt.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (line.find("[NARRATION]") != std::string::npos) ++bad;
            if (shown++ < 8) print_line("      " + line);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        print_line("      ...");
    }
    if (bad) {
        print_line("      FAIL: " + std::to_string(bad) +
                   " dialogue lines tagged [NARRATION] in prompt.");
        return fail(report, 3,
                    std::to_string(bad) + " prompt lines tagged [NARRATION]");
    }

    // -- Check 4 (sanity): the glossary contains core character names --
    const auto& name_dict = name_translations();
    report.glossary_entries = name_dict.size();
    if (!name_dict.count("\xE7\x9B\xB4\xE5\x93\x89")) {  // 直哉
        print_line("FAIL: NAME_TRANSLATIONS missing core character names.");
        return fail(report, 4, "NAME_TRANSLATIONS missing core character names");
    }
    print_line("[4/6] NAME_TRANSLATIONS contains core character names "
               "(\xE7\x9B\xB4\xE5\x93\x89 etc.): OK");

    // -- Check 5: exhaustive bidirectional speaker coverage --
    // Both surfaces are scanned: the plate a `name` entry shows, and the
    // `speaker` field the extractor put on every attributed line.  A label
    // that reaches the prompt through the field alone would otherwise never
    // be asked for here.
    std::map<std::string, int> seen;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            if (!sv.is_object()) continue;
            const auto& s = sv.get_object();
            const std::string type = field_of(s, "type");
            const std::string raw =
                type == "name" ? field_of(s, "text") : field_of(s, "speaker");
            if (!raw.empty()) ++seen[raw];
        }
    }
    report.speakers_in_data = seen.size();

    std::vector<std::string> missing, unused;
    for (const auto& [jp, n] : seen)
        if (!name_dict.count(jp)) missing.push_back(jp);
    for (const auto& [jp, en] : name_translations_ordered())
        if (!seen.count(jp)) unused.push_back(jp);
    std::sort(unused.begin(), unused.end());

    print_line("[5/6] exhaustive speaker coverage scan:");
    print_line("      distinct JP speakers in data: " + std::to_string(seen.size()));
    print_line("      NAME_TRANSLATIONS entries:    " + std::to_string(name_dict.size()));

    if (!missing.empty() || !unused.empty()) {
        if (!missing.empty()) {
            print_line("      FAIL: " + std::to_string(missing.size()) +
                       " JP speakers in data not in NAME_TRANSLATIONS");
            print_line("            (copy-paste-ready, sorted by frequency):");
            std::sort(missing.begin(), missing.end(),
                      [&](const std::string& a, const std::string& b) {
                          return seen[a] != seen[b] ? seen[a] > seen[b] : a < b;
                      });
            for (std::size_t i = 0; i < missing.size() && i < 30; ++i)
                print_line("          '" + missing[i] + "': '???',  # appeared " +
                           std::to_string(seen[missing[i]]) + "x");
            if (missing.size() > 30)
                print_line("          ... and " + std::to_string(missing.size() - 30) + " more");
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " NAME_TRANSLATIONS entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < 30; ++i)
                print_line("          '" + unused[i] + "' -> '" + name_dict.at(unused[i]) + "'");
            if (unused.size() > 30)
                print_line("          ... and " + std::to_string(unused.size() - 30) + " more");
        }
        return fail(report, 5,
                    std::to_string(missing.size()) + " speakers missing from the glossary, " +
                        std::to_string(unused.size()) + " glossary rows never used");
    }
    print_line("      OK - full bidirectional coverage.");

    // -- Check 6: the dossier tells the model who each big speaker is --
    const auto lines_by_label = dialogue_lines_by_speaker(data);
    std::size_t above = 0;
    for (const auto& [label, lines] : lines_by_label)
        if (lines >= DOSSIER_LINE_THRESHOLD) ++above;
    report.dossier_labels = above;
    const std::string dossier = prompt_dossier();
    const auto gaps = dossier_gaps(lines_by_label, dossier);

    print_line("[6/6] character dossier coverage:");
    print_line("      labels with " + std::to_string(DOSSIER_LINE_THRESHOLD) +
               "+ spoken lines: " + std::to_string(above));
    // An empty gap list means nothing if the line counter found no label to
    // check.  Check 5 has already proved this corpus uses every glossary row,
    // so no label above the threshold means the counter is broken, not that
    // the game is small -- and a check that cannot reach a defect must not
    // report OK.
    if (above == 0) {
        print_line("      FAIL: no label reached the threshold, on a corpus "
                   "that just passed the coverage scan.");
        return fail(report, 6,
                    "no speaker reached " + std::to_string(DOSSIER_LINE_THRESHOLD) +
                        " lines; the per-label line count read nothing");
    }
    if (!gaps.empty()) {
        print_line("      FAIL: " + std::to_string(gaps.size()) +
                   " of them cannot be voiced from the prompt:");
        for (const auto& g : gaps)
            print_line("          '" + g.label + "' (" + std::to_string(g.lines) +
                       " lines): " + g.reason);
        return fail(report, 6,
                    std::to_string(gaps.size()) + " speakers above " +
                        std::to_string(DOSSIER_LINE_THRESHOLD) +
                        " lines have no dossier line with a sex");
    }
    print_line("      OK - each one has a dossier line that states a sex.");

    // The prose introduces each lead by an English spelling of its own.  A
    // spelling that differs from the table sends the model one name and the
    // runtime another, and the cache keeps whichever the model was told.
    const auto mismatches = dossier_name_mismatches(dossier);
    if (!mismatches.empty()) {
        print_line("      FAIL: " + std::to_string(mismatches.size()) +
                   " dossier spelling(s) disagree with the name table:");
        for (const auto& m : mismatches)
            print_line("          '" + m.label + "': the dossier writes '" + m.dossier +
                       "', the table writes '" + m.table + "'");
        return fail(report, 6,
                    std::to_string(mismatches.size()) +
                        " dossier name spellings disagree with the name table");
    }
    print_line("      OK - every dossier spelling matches the name table.");

    print_line("");
    print_line("  All checks passed.  Speaker pipeline is healthy - safe to run translation.");
    print_line("  Sample size (checks 1, 3): " + std::to_string(sample.size()) +
               " dialogue entries.");
    print_line("  Coverage scan (check 5): every 'name' plate and every 'speaker' "
               "field across all scripts.");
    print_line("  Dossier scan (check 6): every label at or above " +
               std::to_string(DOSSIER_LINE_THRESHOLD) +
               " spoken lines, and every name the prose spells.");

    report.passed = true;
    return report;
}

}  // namespace snt::translate
