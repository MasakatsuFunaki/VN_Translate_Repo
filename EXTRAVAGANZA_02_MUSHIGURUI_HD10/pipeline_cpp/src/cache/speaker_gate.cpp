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
#include <cstddef>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mgi::translate {

namespace bj = boost::json;

namespace {

// Enough dialogue to make a broken speaker plate obvious and few enough that
// the printed sample stays readable.
constexpr std::size_t kSampleSize = 50;

// Above this share of NARRATION among dialogue lines the plate is not being
// parsed at all.  A handful of genuinely unattributed lines is normal; a
// twentieth of them is the edge of normal.
constexpr double kMaxNarrationPercent = 5.0;

// How many entries of a failing list are worth printing.  The list exists to
// be pasted into the glossary, and beyond this it is a wall.
constexpr std::size_t kMaxListedEntries = 30;

constexpr std::size_t kCrlfByteLen = 2;
constexpr std::size_t kMaxSpeakerNameChars = 20;
constexpr std::size_t kMinSampledDialogues = 10;
constexpr double kPercentScale = 100.0;
constexpr std::size_t kPercentBufSize = 64;
constexpr std::size_t kMaxDisplayedSpeakers = 8;
constexpr std::size_t kPromptSampleCount = 10;
constexpr int kPromptPreviewLines = 8;

constexpr int kCheckSpeakerExtraction = 1;
constexpr int kCheckNameResolution = 2;
constexpr int kCheckPromptFormatting = 3;
constexpr int kCheckCoreCastPresent = 4;
constexpr int kCheckExhaustiveCoverage = 5;

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
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

// Every type=='dialogue' string, in file order.
std::vector<std::string> collect_dialogues(const bj::object& extracted) {
    std::vector<std::string> out;
    for (const auto& kv : extracted) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (strings == nullptr || !strings->is_array()) {
            continue;
        }
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            const auto* t = s.if_contains("type");
            if (t == nullptr || !t->is_string() || t->get_string() != "dialogue") {
                continue;
            }
            if (const auto* txt = s.if_contains("text");
                txt != nullptr && txt->is_string()) {
                out.emplace_back(txt->get_string());
            }
        }
    }
    return out;
}

// Raw (untranslated) JP speaker for the coverage scan.  MUST mirror
// translate::extract_speaker -- if you change one, change the other.
std::string raw_jp_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf == std::string::npos) {
        return {};
    }
    const std::string raw = trim(text.substr(0, crlf));
    const std::string rest = trim_left(text.substr(crlf + kCrlfByteLen));
    if (raw.empty() || char_len(raw) > kMaxSpeakerNameChars || rest.empty()) {
        return {};
    }
    if (rest.rfind("「", 0) != 0 && rest.rfind("『", 0) != 0) {
        return {};
    }
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
            static_cast<std::ptrdiff_t>(std::min(kSampleSize, dialogues_all.size())));
    report.dialogue_sampled = dialogues.size();
    report.dialogue_total = dialogues_all.size();
    if (dialogues.size() < kMinSampledDialogues) {
        print_line("[1/5] extract_speaker on the extracted dialogue:");
        return fail(report, kCheckSpeakerExtraction,
                    "only " + std::to_string(dialogues.size()) +
                        " dialogue entries -- the extraction looks empty");
    }

    // Check 1: extract_speaker returns real speakers, not 'NARRATION'.
    std::size_t narration = 0;
    std::set<std::string> speakers_seen;
    for (const auto& d : dialogues) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") {
            ++narration;
            continue;
        }
        speakers_seen.insert(sp);
    }
    const double pct = kPercentScale * static_cast<double>(narration) /
                       static_cast<double>(dialogues.size());
    char buf[kPercentBufSize];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/5] extract_speaker on " + std::to_string(dialogues.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::vector<std::string> preview(speakers_seen.begin(), speakers_seen.end());
        if (preview.size() > kMaxDisplayedSpeakers) {
            preview.resize(kMaxDisplayedSpeakers);
        }
        print_line("      distinct speakers:  [" + join(preview, ", ") + "]");
    }
    if (pct >= kMaxNarrationPercent) {
        return fail(report, kCheckSpeakerExtraction,
                    "too many lines tagged NARRATION -- the speaker plate is not being parsed");
    }

    // Check 2: JP names resolve to their pinned English.
    print_line("[2/5] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"レン", "Ren"},
        {"美弥香", "Miyaka"},
        {"綾佳", "Ayaka"},
        {"ユーリア", "Yuria"},
        {"夢美", "Yumemi"},
    };
    for (const auto& [jp, en] : cases) {
        const std::string text = jp + "\r\n「dummy」";
        const std::string got = extract_speaker(text);
        print_line(std::string(got == en ? "      OK   " : "      FAIL ") + "'" + jp + "' -> '" +
                   got + "' (expected '" + en + "')");
        if (got != en) {
            return fail(report, kCheckNameResolution,
                        "'" + jp + "' resolved to '" + got + "', not '" + en + "'");
        }
    }

    // Check 3: the assembled API user-prompt carries real [Speaker] tags.
    // A handful of two-line NARRATIVE entries are mis-typed as 'dialogue' in
    // the extraction; extract_speaker correctly tags those NARRATION.  Sample
    // the first 10 dialogues that DO yield a real speaker so this check reads
    // prompt format on actual dialogue.  If the pipeline is genuinely broken
    // the filter starves and the check fails.
    std::vector<std::pair<std::string, std::string>> batch;
    for (const auto& d : dialogues_all) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") {
            continue;
        }
        batch.emplace_back(sp, d);
        if (batch.size() == kPromptSampleCount) {
            break;
        }
    }
    if (batch.size() < kPromptSampleCount) {
        print_line("[3/5] reconstructed user-prompt:");
        return fail(report, kCheckPromptFormatting,
                    "only " + std::to_string(batch.size()) +
                        " dialogues yielded a real speaker -- the pipeline is broken");
    }
    print_line("[3/5] reconstructed user-prompt for the first 10 speakered dialogue lines:");
    {
        std::string prompt = "<lines_to_translate>\n";
        for (std::size_t i = 0; i < batch.size(); ++i) {
            std::string escaped;
            const std::string& text = batch[i].second;
            for (std::size_t k = 0; k < text.size(); ++k) {
                if (text.compare(k, kCrlfByteLen, "\r\n") == 0) {
                    escaped += "\\r\\n";
                    ++k;
                } else if (text[k] == '\n') {
                    escaped += "\\n";
                } else {
                    escaped += text[k];
                }
            }
            prompt += std::to_string(i + 1) + ". [" + batch[i].first + "] " + escaped + "\n";
        }
        prompt += "</lines_to_translate>\n";
        std::size_t pos = 0;
        for (int i = 0; i < kPromptPreviewLines && pos < prompt.size(); ++i) {
            const std::size_t nl = prompt.find('\n', pos);
            print_line("      " + prompt.substr(pos, nl == std::string::npos
                                                         ? std::string::npos
                                                         : nl - pos));
            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }
        print_line("      ...");
    }

    // Check 4: the speaker table still holds the core cast.
    const auto& name_dict = name_translations();
    if (name_dict.count("レン") == 0) {
        return fail(report, kCheckCoreCastPresent,
                    "the speaker table is missing the core character names");
    }
    print_line("[4/5] the speaker table contains the core character names (レン etc.): OK");

    // Check 5: exhaustive bidirectional speaker coverage.
    std::map<std::string, std::size_t> seen;
    for (const auto& d : dialogues_all) {
        const std::string raw = raw_jp_speaker(d);
        if (!raw.empty()) {
            ++seen[raw];
        }
    }

    report.speakers_in_data = seen.size();
    report.glossary_entries = name_dict.size();

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
            for (std::size_t i = 0; i < missing.size() && i < kMaxListedEntries; ++i) {
                print_line("          '" + missing[i] + "': '???',  # appeared " +
                           std::to_string(seen[missing[i]]) + "x");
            }
            if (missing.size() > kMaxListedEntries) {
                print_line("          ... and " +
                           std::to_string(missing.size() - kMaxListedEntries) + " more");
            }
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " speaker table entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < kMaxListedEntries; ++i) {
                print_line("          '" + unused[i] + "' -> '" + name_dict.at(unused[i]) + "'");
            }
            if (unused.size() > kMaxListedEntries) {
                print_line("          ... and " +
                           std::to_string(unused.size() - kMaxListedEntries) + " more");
            }
        }
        return fail(report, kCheckExhaustiveCoverage,
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
