// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate/speaker_gate.h"

#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bj = boost::json;

namespace snt::translate {

namespace {

constexpr std::size_t kSampleSize = 50;
constexpr std::size_t kMinSampledDialogues = 10;
constexpr double kPercentScale = 100.0;
constexpr double kMaxNarrationPercent = 5.0;
constexpr std::size_t kPercentBufChars = 64;
constexpr std::size_t kMaxDisplayedSpeakers = 8;
constexpr std::size_t kPromptSampleCount = 10;
constexpr int kMaxDisplayedPromptLines = 8;
constexpr std::size_t kMaxReportedListItems = 30;
constexpr int kCheckSampleAttribution = 1;
constexpr int kCheckNameResolution = 2;
constexpr int kCheckPromptTags = 3;
constexpr int kCheckCoreGlossary = 4;
constexpr int kCheckBidirectionalCoverage = 5;
constexpr int kCheckDossier = 6;
constexpr std::size_t kResolvedDialogueIndex = 1;

struct Row {
    std::string script;
    std::size_t idx;
    std::string speaker;
    std::string text;
};

std::vector<Row> walk_dialogues(const bj::object& data, std::size_t limit) {
    std::vector<Row> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (strings == nullptr || !strings->is_array()) {
            continue;
        }
        const auto annotated = iter_entries_with_speakers(strings->get_array());
        for (std::size_t i = 0; i < annotated.size(); ++i) {
            const auto& [speaker, entry] = annotated[i];
            const auto* t = entry->if_contains("type");
            if (t == nullptr || !t->is_string() || t->get_string() != "dialogue") {
                continue;
            }
            std::string text;
            if (const auto* s = entry->if_contains("text");
                s != nullptr && s->is_string()) {
                text = std::string(s->get_string());
            }
            out.push_back(Row{std::string(kv.key()), i, speaker, text});
            if (out.size() >= limit) {
                return out;
            }
        }
    }
    return out;
}

std::string build_gate_prompt(const std::vector<Row>& rows) {
    std::vector<std::pair<std::string, std::string>> batch;
    batch.reserve(rows.size());
    for (const auto& r : rows) {
        batch.emplace_back(r.speaker, r.text);
    }
    return build_user_prompt(batch, {});
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) {
            out += sep;
        }
        out += s;
    }
    return out;
}

std::string resolve_via_state_machine(const std::string& jp_name) {
    bj::object name_entry;
    name_entry["type"] = "name";
    name_entry["text"] = jp_name;
    bj::object dlg;
    dlg["type"] = "dialogue";
    dlg["text"] = "「dummy」";
    bj::array entries{std::move(name_entry), std::move(dlg)};
    return iter_entries_with_speakers(entries)[kResolvedDialogueIndex].first;
}

SpeakerCheckReport fail(SpeakerCheckReport report, int check, std::string reason) {
    report.passed = false;
    report.failed_check = check;
    report.reason = std::move(reason);
    return report;
}

std::string field_of(const bj::object& o, const char* key) {
    if (const auto* v = o.if_contains(key); v != nullptr && v->is_string()) {
        return trim(std::string(v->get_string()));
    }
    return {};
}

bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\'' || c == ' ';
}

std::string english_before(const std::string& text, std::size_t open) {
    std::size_t b = open;
    while (b > 0 && is_name_char(text[b - 1])) {
        --b;
    }
    return trim(text.substr(b, open - b));
}

bool ends_with_name(const std::string& spelling, const std::string& name) {
    if (spelling.size() < name.size()) {
        return false;
    }
    const std::size_t at = spelling.size() - name.size();
    if (spelling.compare(at, name.size(), name) != 0) {
        return false;
    }
    return at == 0 || spelling[at - 1] == ' ';
}

}  // namespace

std::map<std::string, std::size_t> dialogue_lines_by_speaker(const bj::object& data) {
    std::map<std::string, std::size_t> counts;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (strings == nullptr || !strings->is_array()) {
            continue;
        }
        std::string standing;
        for (const auto& sv : strings->get_array()) {
            if (!sv.is_object()) {
                continue;
            }
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
            if (type != "dialogue") {
                continue;
            }
            const std::string label =
                entry.if_contains("speaker") ? field_of(entry, "speaker") : standing;
            if (!label.empty()) {
                ++counts[label];
            }
        }
    }
    return counts;
}

std::vector<DossierGap> dossier_gaps(
    const std::map<std::string, std::size_t>& lines_by_label,
    const std::string& dossier) {
    static constexpr const char* kSexMarkers[] = {
        "**Male**", "**Female**", "he/him", "she/her"};
    std::vector<std::string> dossier_lines;
    for (std::size_t pos = 0; pos <= dossier.size();) {
        const std::size_t nl = dossier.find('\n', pos);
        dossier_lines.push_back(
            dossier.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }

    std::vector<DossierGap> gaps;
    for (const auto& [label, lines] : lines_by_label) {
        if (lines < DOSSIER_LINE_THRESHOLD) {
            continue;
        }
        bool named = false;
        bool sexed = false;
        for (const auto& line : dossier_lines) {
            if (line.find(label) == std::string::npos) {
                continue;
            }
            named = true;
            for (const char* marker : kSexMarkers) {
                if (line.find(marker) != std::string::npos) {
                    sexed = true;
                }
            }
            if (sexed) {
                break;
            }
        }
        if (!named) {
            gaps.push_back({label, lines, "no dossier line names it"});
            continue;
        }
        if (!sexed) {
            gaps.push_back({label, lines, "its dossier line states no sex"});
        }
    }
    return gaps;
}

std::vector<DossierNameMismatch> dossier_name_mismatches(const std::string& dossier) {
    std::vector<DossierNameMismatch> out;
    for (const auto& [label, en] : prompt_name_translations()) {
        const std::string opener = "(" + label;
        for (std::size_t pos = dossier.find(opener); pos != std::string::npos;
             pos = dossier.find(opener, pos + 1)) {
            const std::size_t after = pos + opener.size();
            const char next = after < dossier.size() ? dossier[after] : '\0';
            if (next != ',' && next != ')' && next != ' ') {
                continue;
            }
            const std::string spelling = english_before(dossier, pos);
            if (!ends_with_name(spelling, en)) {
                out.push_back({label, spelling, en});
            }
        }
    }
    return out;
}

std::string prompt_dossier() {
    constexpr std::string_view kOpenTag = "<character_dossier>";
    constexpr std::string_view kCloseTag = "</character_dossier>";
    const std::string& prompt = system_prompt();
    const std::size_t a = prompt.find(kOpenTag);
    if (a == std::string::npos) {
        return {};
    }
    const std::size_t b = prompt.find(kCloseTag, a);
    if (b == std::string::npos) {
        return {};
    }
    const std::size_t start = a + kOpenTag.size();
    return prompt.substr(start, b - start);
}

SpeakerCheckReport run_speaker_checks(const bj::object& data) {
    SpeakerCheckReport report;

    const auto sample = walk_dialogues(data, kSampleSize);
    report.dialogue_sampled = sample.size();
    if (sample.size() < kMinSampledDialogues) {
        print_line("FAIL: only " + std::to_string(sample.size()) +
                   " dialogue entries found - extracted_text.json looks empty.");
        return fail(report, kCheckSampleAttribution,
                    "only " + std::to_string(sample.size()) +
                        " dialogue entries in the extraction");
    }

    std::size_t n_narration = 0;
    std::set<std::string> distinct_real;
    for (const auto& r : sample) {
        if (r.speaker == "NARRATION") {
            ++n_narration;
            continue;
        }
        distinct_real.insert(r.speaker);
    }
    const double pct = kPercentScale * static_cast<double>(n_narration) /
                       static_cast<double>(sample.size());
    char buf[kPercentBufChars];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/6] iter_entries_with_speakers on " + std::to_string(sample.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(n_narration) +
               " (" + buf + "%)");
    {
        std::vector<std::string> preview(distinct_real.begin(), distinct_real.end());
        if (preview.size() > kMaxDisplayedSpeakers) {
            preview.resize(kMaxDisplayedSpeakers);
        }
        print_line("      distinct speakers:  [" + join(preview, ", ") + "]");
    }
    if (pct >= kMaxNarrationPercent) {
        print_line("      FAIL: too many dialogue lines tagged NARRATION - "
                   "name->dialogue tracking broken.");
        return fail(report, kCheckSampleAttribution,
                    std::string(buf) + "% of sampled dialogue tagged NARRATION");
    }

    print_line("[2/6] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"直哉", "Naoya"},
        {"心鈴", "Misuzu"},
        {"真琴", "Makoto"},
        {"静流", "Shizuru"},
        {"放哉", "Housai"},
    };
    for (const auto& [jp, en] : cases) {
        const std::string got = resolve_via_state_machine(jp);
        print_line(std::string(got == en ? "      OK   " : "      FAIL ") + "'" + jp +
                   "' -> '" + got + "' (expected '" + en + "')");
        if (got != en) {
            return fail(report, kCheckNameResolution,
                        "'" + jp + "' resolved to '" + got + "', expected '" + en + "'");
        }
    }

    const std::vector<Row> prompt_sample(
        sample.begin(),
        sample.begin() + static_cast<std::ptrdiff_t>(
                             std::min<std::size_t>(kPromptSampleCount, sample.size())));
    const std::string prompt = build_gate_prompt(prompt_sample);
    print_line("[3/6] reconstructed user-prompt for first 10 dialogue lines:");
    std::size_t bad = 0;
    {
        std::size_t pos = 0;
        int shown = 0;
        while (pos <= prompt.size()) {
            const std::size_t nl = prompt.find('\n', pos);
            const std::string line =
                prompt.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (line.find("[NARRATION]") != std::string::npos) {
                ++bad;
            }
            if (shown++ < kMaxDisplayedPromptLines) {
                print_line("      " + line);
            }
            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }
        print_line("      ...");
    }
    if (bad > 0) {
        print_line("      FAIL: " + std::to_string(bad) +
                   " dialogue lines tagged [NARRATION] in prompt.");
        return fail(report, kCheckPromptTags,
                    std::to_string(bad) + " prompt lines tagged [NARRATION]");
    }

    const auto& name_dict = name_translations();
    report.glossary_entries = name_dict.size();
    if (!name_dict.count("直哉")) {
        print_line("FAIL: NAME_TRANSLATIONS missing core character names.");
        return fail(report, kCheckCoreGlossary,
                    "NAME_TRANSLATIONS missing core character names");
    }
    print_line("[4/6] NAME_TRANSLATIONS contains core character names (直哉 etc.): OK");

    std::map<std::string, int> seen;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (strings == nullptr || !strings->is_array()) {
            continue;
        }
        for (const auto& sv : strings->get_array()) {
            if (!sv.is_object()) {
                continue;
            }
            const auto& s = sv.get_object();
            const std::string type = field_of(s, "type");
            const std::string raw =
                type == "name" ? field_of(s, "text") : field_of(s, "speaker");
            if (!raw.empty()) {
                ++seen[raw];
            }
        }
    }
    report.speakers_in_data = seen.size();

    std::vector<std::string> missing;
    std::vector<std::string> unused;
    for (const auto& [jp, n] : seen) {
        if (!name_dict.count(jp)) {
            missing.push_back(jp);
        }
    }
    for (const auto& [jp, en] : name_translations_ordered()) {
        if (!seen.count(jp)) {
            unused.push_back(jp);
        }
    }
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
            for (std::size_t i = 0;
                 i < missing.size() && i < kMaxReportedListItems; ++i) {
                print_line("          '" + missing[i] + "': '???',  # appeared " +
                           std::to_string(seen[missing[i]]) + "x");
            }
            if (missing.size() > kMaxReportedListItems) {
                print_line("          ... and " +
                           std::to_string(missing.size() - kMaxReportedListItems) +
                           " more");
            }
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " NAME_TRANSLATIONS entries never appear as speakers:");
            for (std::size_t i = 0;
                 i < unused.size() && i < kMaxReportedListItems; ++i) {
                print_line("          '" + unused[i] + "' -> '" +
                           name_dict.at(unused[i]) + "'");
            }
            if (unused.size() > kMaxReportedListItems) {
                print_line("          ... and " +
                           std::to_string(unused.size() - kMaxReportedListItems) +
                           " more");
            }
        }
        return fail(report, kCheckBidirectionalCoverage,
                    std::to_string(missing.size()) +
                        " speakers missing from the glossary, " +
                        std::to_string(unused.size()) + " glossary rows never used");
    }
    print_line("      OK - full bidirectional coverage.");

    const auto lines_by_label = dialogue_lines_by_speaker(data);
    std::size_t above = 0;
    for (const auto& [label, lines] : lines_by_label) {
        if (lines >= DOSSIER_LINE_THRESHOLD) {
            ++above;
        }
    }
    report.dossier_labels = above;
    const std::string dossier = prompt_dossier();
    const auto gaps = dossier_gaps(lines_by_label, dossier);

    print_line("[6/6] character dossier coverage:");
    print_line("      labels with " + std::to_string(DOSSIER_LINE_THRESHOLD) +
               "+ spoken lines: " + std::to_string(above));
    if (above == 0) {
        print_line("      FAIL: no label reached the threshold, on a corpus "
                   "that just passed the coverage scan.");
        return fail(report, kCheckDossier,
                    "no speaker reached " + std::to_string(DOSSIER_LINE_THRESHOLD) +
                        " lines; the per-label line count read nothing");
    }
    if (!gaps.empty()) {
        print_line("      FAIL: " + std::to_string(gaps.size()) +
                   " of them cannot be voiced from the prompt:");
        for (const auto& g : gaps) {
            print_line("          '" + g.label + "' (" + std::to_string(g.lines) +
                       " lines): " + g.reason);
        }
        return fail(report, kCheckDossier,
                    std::to_string(gaps.size()) + " speakers above " +
                        std::to_string(DOSSIER_LINE_THRESHOLD) +
                        " lines have no dossier line with a sex");
    }
    print_line("      OK - each one has a dossier line that states a sex.");

    const auto mismatches = dossier_name_mismatches(dossier);
    if (!mismatches.empty()) {
        print_line("      FAIL: " + std::to_string(mismatches.size()) +
                   " dossier spelling(s) disagree with the name table:");
        for (const auto& m : mismatches) {
            print_line("          '" + m.label + "': the dossier writes '" +
                       m.dossier + "', the table writes '" + m.table + "'");
        }
        return fail(report, kCheckDossier,
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
