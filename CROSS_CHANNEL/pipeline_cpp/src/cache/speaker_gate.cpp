// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cache/speaker_gate.h"

#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "common/util.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bj = boost::json;

namespace crc::translate {
namespace {

constexpr std::size_t kSampleSize = 50;
constexpr char32_t kControlCharLimit = 0x20;
constexpr char32_t kAsciiDelete = 0x7F;
constexpr std::size_t kHexEscapeBufSize = 8;
constexpr std::size_t kMaxSpeakerCodepoints = 20;
constexpr std::size_t kCrlfByteLength = 2;
constexpr std::size_t kMinSampledDialogues = 10;
constexpr double kPercentScale = 100.0;
constexpr std::size_t kPercentBufSize = 32;
constexpr std::size_t kMaxSampledDistinctSpeakers = 8;
constexpr double kMaxNarrationPercent = 5.0;
constexpr std::size_t kJpPadWidth = 8;
constexpr std::size_t kEnPadWidth = 10;
constexpr std::size_t kPromptPreviewDialogues = 10;
constexpr std::size_t kPromptPrintedLines = 8;
constexpr std::size_t kMaxReportedMismatches = 30;
constexpr std::size_t kMissingPadWidth = 14;

constexpr int kCheckSpeakerExtraction = 1;
constexpr int kCheckGlossaryMapping = 2;
constexpr int kCheckPromptFormatting = 3;
constexpr int kCheckCoreGlossaryPresence = 4;
constexpr int kCheckBidirectionalCoverage = 5;

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

// Quoted display form: single quotes unless the value contains a single quote
// and no double quote; backslash and the chosen quote escaped; ASCII control
// characters escaped; printable non-ASCII passes through raw so JP names stay
// legible.
std::string quote_repr(const std::string& s) {
    const bool has_sq = s.find('\'') != std::string::npos;
    const bool has_dq = s.find('"') != std::string::npos;
    const char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        const char32_t cp = utf8_next(s, i);
        if (cp == '\\') {
            out += "\\\\";
            continue;
        }
        if (cp == static_cast<char32_t>(q)) {
            out += '\\';
            out += q;
            continue;
        }
        if (cp == '\n') {
            out += "\\n";
            continue;
        }
        if (cp == '\r') {
            out += "\\r";
            continue;
        }
        if (cp == '\t') {
            out += "\\t";
            continue;
        }
        if (cp < kControlCharLimit || cp == kAsciiDelete) {
            char buf[kHexEscapeBufSize];
            std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned>(cp));
            out += buf;
            continue;
        }
        out.append(s, start, i - start);
    }
    out += q;
    return out;
}

// Field width counts codepoints rather than bytes so JP columns align cleanly.
std::string pad_left(const std::string& s, std::size_t width) {
    const std::size_t n = char_len(s);
    return n >= width ? s : std::string(width - n, ' ') + s;
}

std::string pad_right(const std::string& s, std::size_t width) {
    const std::size_t n = char_len(s);
    return n >= width ? s : s + std::string(width - n, ' ');
}

std::string quote_list_repr(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += quote_repr(v[i]);
    }
    return out + "]";
}

std::string json_str(const bj::object& o, const char* key) {
    if (const auto* v = o.if_contains(key)) {
        if (v->is_string()) {
            return std::string(v->get_string());
        }
    }
    return {};
}

std::vector<const bj::object*> collect_dialogues(const bj::object& data) {
    std::vector<const bj::object*> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) {
            continue;
        }
        for (const auto& sv : strings->get_array()) {
            if (!sv.is_object()) {
                continue;
            }
            const auto& s = sv.get_object();
            if (json_str(s, "type") == "dialogue") {
                out.push_back(&s);
            }
        }
    }
    return out;
}

// Raw (untranslated) JP speaker for the coverage scan.  Mirrors
// translate::extract_speaker.
std::string raw_jp_speaker(const bj::object& s) {
    std::string raw = trim(json_str(s, "speaker"));
    if (raw.empty()) {
        const std::string text = json_str(s, "text");
        const std::size_t crlf = text.find("\r\n");
        if (crlf != std::string::npos) {
            raw = trim(text.substr(0, crlf));
        }
    }
    return (!raw.empty() && char_len(raw) <= kMaxSpeakerCodepoints) ? raw : std::string();
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
            if (text.compare(k, kCrlfByteLength, "\r\n") == 0) {
                escaped += "\\r\\n";
                ++k;
                continue;
            }
            if (text[k] == '\n') {
                escaped += "\\n";
                continue;
            }
            escaped += text[k];
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
            static_cast<std::ptrdiff_t>(std::min(kSampleSize, dialogs_all.size())));
    report.dialogue_sampled = dialogs.size();
    if (dialogs.size() < kMinSampledDialogues) {
        const std::string why = "only " + std::to_string(dialogs.size()) +
                                " dialogue entries; extracted_text.json looks empty.";
        print_line("FAIL: " + why);
        return fail(report, kCheckSpeakerExtraction, why);
    }

    // Check 1: extract_speaker returns character names rather than 'NARRATION'.
    std::size_t narration = 0;
    std::set<std::string> speakers_set;
    for (const auto* d : dialogs) {
        const std::string sp = extract_speaker(*d);
        if (sp == "NARRATION") {
            ++narration;
        }
        speakers_set.insert(sp);
    }
    speakers_set.erase("NARRATION");
    const double pct =
        kPercentScale * static_cast<double>(narration) / static_cast<double>(dialogs.size());
    char buf[kPercentBufSize];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/5] extract_speaker on " + std::to_string(dialogs.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::vector<std::string> sample(speakers_set.begin(), speakers_set.end());
        if (sample.size() > kMaxSampledDistinctSpeakers) {
            sample.resize(kMaxSampledDistinctSpeakers);
        }
        print_line("      distinct speakers:  " + quote_list_repr(sample));
    }
    if (pct >= kMaxNarrationPercent) {
        print_line("      FAIL: too many lines tagged NARRATION — "
                   "speaker field not flowing.");
        return fail(report, kCheckSpeakerExtraction,
                    std::string(buf) + "% of sampled dialogue lines tagged NARRATION.");
    }

    // Check 2: JP names map to EN via NAME_TRANSLATIONS.
    print_line("[2/5] JP → EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"太一", "Taichi"},
        {"見里", "Misato"},
        {"霧", "Kiri"},
        {"冬子", "Touko"},
        {"美希", "Miki"},
    };
    for (const auto& [jp, en] : cases) {
        bj::object s;
        s["speaker"] = jp;
        s["text"] = "「dummy」";
        s["type"] = "dialogue";
        const std::string got = extract_speaker(s);
        print_line(std::string("      ") + (got == en ? "OK " : "FAIL") + "  " +
                   pad_left(quote_repr(jp), kJpPadWidth) + " -> " +
                   pad_right(quote_repr(got), kEnPadWidth) +
                   " (expected " + quote_repr(en) + ")");
        if (got != en) {
            return fail(report, kCheckGlossaryMapping,
                        quote_repr(jp) + " resolved to " + quote_repr(got) + ", expected " +
                            quote_repr(en) + ".");
        }
    }

    // Check 3: assembled API user-prompt contains real [Speaker] tags.
    {
        std::vector<const bj::object*> preview(
            dialogs.begin(),
            dialogs.begin() +
                static_cast<std::ptrdiff_t>(std::min(kPromptPreviewDialogues, dialogs.size())));
        const std::string prompt = build_gate_prompt(preview);

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
        for (const auto& l : lines) {
            if (l.find("[NARRATION]") != std::string::npos) {
                ++bad;
            }
        }

        print_line("[3/5] reconstructed user-prompt for first 10 dialogue lines:");
        for (std::size_t i = 0; i < lines.size() && i < kPromptPrintedLines; ++i) {
            print_line("      " + lines[i]);
        }
        print_line("      ...");
        if (bad != 0) {
            print_line("      FAIL: " + std::to_string(bad) +
                       " dialogue lines tagged [NARRATION] in prompt.");
            return fail(report, kCheckPromptFormatting,
                        std::to_string(bad) +
                            " of the first 10 dialogue lines reach the prompt as [NARRATION].");
        }
    }

    // Check 4: NAME_TRANSLATIONS pre-population contains core cast names.
    const auto& name_dict = name_translations();
    report.glossary_entries = name_dict.size();
    if (name_dict.count("太一") == 0) {
        print_line("FAIL: NAME_TRANSLATIONS missing core character names.");
        return fail(report, kCheckCoreGlossaryPresence,
                    "NAME_TRANSLATIONS is missing the core character names.");
    }
    print_line("[4/5] NAME_TRANSLATIONS contains core character names (太一 etc.): OK");

    // Check 5: exhaustive bidirectional speaker coverage.
    std::map<std::string, long long> seen;
    for (const auto* d : dialogs_all) {
        const std::string raw = raw_jp_speaker(*d);
        if (!raw.empty()) {
            ++seen[raw];
        }
    }
    report.speakers_in_data = seen.size();

    std::vector<std::string> missing;
    std::vector<std::string> unused;
    for (const auto& [jp, n] : seen) {
        (void)n;
        if (name_dict.count(jp) == 0) {
            missing.push_back(jp);
        }
    }
    for (const auto& [jp, en] : name_translations_ordered()) {
        (void)en;
        if (seen.count(jp) == 0) {
            unused.push_back(jp);
        }
    }
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
            for (std::size_t i = 0; i < missing.size() && i < kMaxReportedMismatches; ++i) {
                print_line("          " + pad_left(quote_repr(missing[i]), kMissingPadWidth) +
                           ": '???',  # appeared " + std::to_string(seen[missing[i]]) + "x");
            }
            if (missing.size() > kMaxReportedMismatches) {
                print_line("          ... and " +
                           std::to_string(missing.size() - kMaxReportedMismatches) + " more");
            }
            why = std::to_string(missing.size()) +
                  " JP speakers in the data have no NAME_TRANSLATIONS entry";
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " NAME_TRANSLATIONS entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < kMaxReportedMismatches; ++i) {
                print_line("          " + quote_repr(unused[i]) + " -> " +
                           quote_repr(name_dict.at(unused[i])));
            }
            if (unused.size() > kMaxReportedMismatches) {
                print_line("          ... and " +
                           std::to_string(unused.size() - kMaxReportedMismatches) + " more");
            }
            if (!why.empty()) {
                why += "; ";
            }
            why += std::to_string(unused.size()) +
                   " NAME_TRANSLATIONS entries never appear as a speaker";
        }
        return fail(report, kCheckBidirectionalCoverage, why + ".");
    }

    print_line("      OK — full bidirectional coverage.");
    print_line("");
    print_line("  All checks passed.  Speaker pipeline is healthy — "
               "safe to run translation.");
    print_line("  Sample size (checks 1, 3): " + std::to_string(dialogs.size()) + " of " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");
    print_line("  Coverage scan (check 5):  all " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");

    report.passed = true;
    return report;
}

}  // namespace crc::translate
