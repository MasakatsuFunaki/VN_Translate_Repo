// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "cache/speaker_gate.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "common/util.h"

namespace exm::translate {

namespace bj = boost::json;

namespace {

constexpr std::size_t kSampleSize = 50;
constexpr std::size_t kMinDialogueSample = 10;
constexpr double kMaxNarrationPct = 20.0;
constexpr double kPercentScale = 100.0;
constexpr std::size_t kPromptLinesShown = 8;
constexpr std::size_t kMaxDistinctSpeakersShown = 8;
constexpr std::size_t kMaxListed = 30;
constexpr std::size_t kMaxSpeakerNameChars = 20;
constexpr std::size_t kPromptSampleBatchSize = 10;
constexpr std::size_t kCrlfLen = 2;
constexpr std::size_t kFormatBufSize = 64;

constexpr int kCheckExtractSpeaker = 1;
constexpr int kCheckNameResolution = 2;
constexpr int kCheckPromptTags = 3;
constexpr int kCheckCoreCast = 4;
constexpr int kCheckCoverage = 5;

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

void append_file_dialogue(const bj::value& file_val, std::vector<std::string>& out) {
    if (!file_val.is_object()) return;
    const auto* strings = file_val.get_object().if_contains("strings");
    if (!strings || !strings->is_array()) return;
    for (const auto& sv : strings->get_array()) {
        if (!sv.is_object()) continue;
        const auto& s = sv.get_object();
        const auto* t = s.if_contains("type");
        if (!t || !t->is_string() || t->get_string() != "dialogue") continue;
        if (const auto* txt = s.if_contains("text")) {
            if (txt->is_string()) out.emplace_back(txt->get_string());
        }
    }
}

// Collects every type=='dialogue' string, iterating story_order() first before
// any remaining keys in extracted so Check [1/5] samples the first 50 dialogue
// lines from main.spt rather than developer test strings in agh_test01.spt.
std::vector<std::string> collect_dialogue(const bj::object& extracted) {
    std::vector<std::string> out;
    std::set<std::string> visited;

    for (const auto& fname : story_order()) {
        if (const auto* fv = extracted.if_contains(fname)) {
            append_file_dialogue(*fv, out);
            visited.insert(fname);
        }
    }
    for (const auto& kv : extracted) {
        const std::string fname(kv.key());
        if (visited.count(fname)) continue;
        append_file_dialogue(kv.value(), out);
    }
    return out;
}

std::string raw_jp_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf == std::string::npos) return {};
    const std::string raw = trim(text.substr(0, crlf));
    const std::string rest = trim_left(text.substr(crlf + kCrlfLen));
    if (raw.empty() || char_len(raw) > kMaxSpeakerNameChars || rest.empty()) return {};
    if (rest.rfind("「", 0) != 0 && rest.rfind("『", 0) != 0) return {};
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
            static_cast<std::ptrdiff_t>(std::min(kSampleSize, dialogue_all.size())));
    report.dialogue_sampled = dialogue.size();
    report.dialogue_total = dialogue_all.size();
    if (dialogue.size() < kMinDialogueSample) {
        print_line("[1/5] extract_speaker on the extracted dialogue:");
        return fail(report, kCheckExtractSpeaker,
                    "only " + std::to_string(dialogue.size()) +
                        " dialogue entries -- the extraction looks empty");
    }

    std::size_t narration = 0;
    std::set<std::string> speakers_seen;
    for (const auto& d : dialogue) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") {
            ++narration;
            continue;
        }
        speakers_seen.insert(sp);
    }
    const double pct =
        kPercentScale * static_cast<double>(narration) / static_cast<double>(dialogue.size());
    print_line("[1/5] extract_speaker on " + std::to_string(dialogue.size()) +
               " dialogue entries:");
    char buf[kFormatBufSize];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::string list = "      distinct speakers:  [";
        std::size_t shown = 0;
        for (const auto& sp : speakers_seen) {
            if (shown >= kMaxDistinctSpeakersShown) break;
            if (shown) list += ", ";
            list += "'" + sp + "'";
            ++shown;
        }
        print_line(list + "]");
    }
    if (pct >= kMaxNarrationPct) {
        return fail(report, kCheckExtractSpeaker,
                    "too many lines tagged NARRATION -- speaker prefix not flowing");
    }

    print_line("[2/5] JP -> EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"夢美", "Yumemi"},
        {"アゲハ", "Ageha"},
        {"客Ａ", "Customer A"},
        {"老婆Ａ", "Old Woman A"},
        {"老人", "Old Man"},
    };
    for (const auto& [jp, en] : cases) {
        const std::string text = jp + "\r\n「dummy」";
        const std::string got = extract_speaker(text);
        print_line(std::string("      ") + (got == en ? "OK " : "FAIL") + "  '" + jp +
                   "' -> '" + got + "' (expected '" + en + "')");
        if (got != en) {
            return fail(report, kCheckNameResolution,
                        "'" + jp + "' resolved to '" + got + "', not '" + en + "'");
        }
    }

    std::vector<std::pair<std::string, std::string>> batch;
    for (const auto& d : dialogue_all) {
        const std::string sp = extract_speaker(d);
        if (sp == "NARRATION") continue;
        batch.emplace_back(sp, d);
        if (batch.size() == kPromptSampleBatchSize) break;
    }
    print_line("[3/5] reconstructed user-prompt for the first 10 speakered dialogue lines:");
    if (batch.size() < kPromptSampleBatchSize) {
        return fail(report, kCheckPromptTags,
                    "only " + std::to_string(batch.size()) +
                        " dialogue entries yielded a speaker -- the pipeline is broken");
    }
    {
        const std::string prompt = build_user_prompt(batch, {});
        std::size_t bad = 0;
        std::size_t pos = 0;
        std::size_t shown = 0;
        while (pos < prompt.size()) {
            const std::size_t nl = prompt.find('\n', pos);
            const std::string line = prompt.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? prompt.size() : nl + 1;
            if (line == "</lines_to_translate>") break;
            if (line.find("[NARRATION]") != std::string::npos) ++bad;
            if (shown < kPromptLinesShown) {
                print_line("      " + line);
                ++shown;
            }
        }
        print_line("      ...");
        if (bad) {
            return fail(report, kCheckPromptTags,
                        std::to_string(bad) +
                            " dialogue lines tagged [NARRATION] in the prompt");
        }
    }

    const auto& name_dict = name_translations();
    if (!name_dict.count("夢美") ||
        !name_dict.count("アゲハ") ||
        !name_dict.count("客Ａ")) {
        return fail(report, kCheckCoreCast,
                    "the speaker table is missing the core character names");
    }
    print_line("[4/5] the speaker table contains the core character names "
               "(夢美, アゲハ, etc.): OK");

    std::map<std::string, std::size_t> seen;
    for (const auto& d : dialogue_all) {
        const std::string raw = raw_jp_speaker(d);
        if (!raw.empty()) ++seen[raw];
    }

    report.speakers_in_data = seen.size();
    report.glossary_entries = name_dict.size();
    std::vector<std::string> missing;
    for (const auto& [jp, n] : seen) {
        if (!name_dict.count(jp)) missing.push_back(jp);
    }
    std::sort(missing.begin(), missing.end());

    print_line("[5/5] exhaustive speaker coverage scan:");
    print_line("      distinct JP speakers in data: " + std::to_string(seen.size()));
    print_line("      speaker table entries:        " + std::to_string(name_dict.size()));

    if (!missing.empty()) {
        print_line("      FAIL: " + std::to_string(missing.size()) +
                   " JP speakers in data not in the speaker table");
        print_line("            (copy-paste-ready, sorted by frequency):");
        std::vector<std::string> by_freq = missing;
        std::sort(by_freq.begin(), by_freq.end(),
                  [&](const std::string& a, const std::string& b) {
                      return seen[a] != seen[b] ? seen[a] > seen[b] : a < b;
                  });
        for (std::size_t i = 0; i < by_freq.size() && i < kMaxListed; ++i) {
            print_line("          '" + by_freq[i] + "': '???',  # appeared " +
                       std::to_string(seen[by_freq[i]]) + "x");
        }
        if (missing.size() > kMaxListed) {
            print_line("          ... and " + std::to_string(missing.size() - kMaxListed) +
                       " more");
        }
        return fail(report, kCheckCoverage,
                    std::to_string(missing.size()) + " speakers missing from the table");
    }
    print_line("      OK -- 100% of extracted speakers covered.");

    print_line("");
    print_line("  All checks passed.  The speaker pipeline is healthy -- safe to translate.");
    print_line("  Sample size (checks 1, 3): " + std::to_string(dialogue.size()) + " of " +
               std::to_string(dialogue_all.size()) + " dialogue entries.");
    report.passed = true;
    return report;
}

}  // namespace exm::translate
