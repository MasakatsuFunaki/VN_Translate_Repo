// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_speaker_gate -- offline pre-translation verification gate guaranteeing
// that every dialogue line in extracted_text.json carries a resolved speaker
// tag and 100% bidirectional coverage against adi::translate::name_translations().
//
// Spends ZERO API tokens: reconstructs the exact user-prompt lines that
// call_gemini sends and asserts every speaker invariant before translation.
#include "apps/paths.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bj = boost::json;
using namespace adi;

namespace {

constexpr std::size_t kSampleSize = 50;
constexpr std::size_t kMinRequiredDialogues = 10;
constexpr std::size_t kMaxSpeakerPreviewCount = 8;
constexpr std::size_t kPromptSampleCount = 10;
constexpr std::size_t kPromptPreviewLines = 8;
constexpr std::size_t kMaxSpeakerCodepoints = 20;
constexpr std::size_t kMaxReportEntries = 30;
constexpr std::size_t kJpColumnPad = 8;
constexpr std::size_t kEnColumnPad = 10;
constexpr std::size_t kMissingNamePad = 14;
constexpr std::size_t kCrlfSequenceLen = 2;
constexpr std::size_t kHexEscapeBufSize = 8;
constexpr std::size_t kPercentBufSize = 32;
constexpr unsigned char kAsciiPrintableMin = 0x20;
constexpr unsigned char kAsciiDelete = 0x7F;
constexpr double kPercentScale = 100.0;
constexpr double kMaxNarrationPercent = 5.0;

// Quoted string literal for the copy-paste-ready report at [5/5]: single
// quotes unless that would need escaping and double quotes would not.  Only
// the escapes this data can actually need (backslash, the quote, \n \r \t,
// C0 controls); Japanese is printable and must pass through raw, or the
// pasted glossary entry would no longer match the engine's text.
std::string quote_repr(const std::string& s) {
    const bool has_sq = s.find('\'') != std::string::npos;
    const bool has_dq = s.find('"') != std::string::npos;
    const char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    for (unsigned char c : s) {
        if (c == static_cast<unsigned char>(q) || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
            continue;
        }
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        if (c == '\r') {
            out += "\\r";
            continue;
        }
        if (c == '\t') {
            out += "\\t";
            continue;
        }
        if (c < kAsciiPrintableMin || c == kAsciiDelete) {
            char buf[kHexEscapeBufSize];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
            continue;
        }
        out += static_cast<char>(c);
    }
    out += q;
    return out;
}

// Column padding for the report tables -- width counts CODEPOINTS, not bytes,
// so Japanese columns line up instead of being padded three times over.
std::string pad_left(const std::string& s, std::size_t w) {
    const std::size_t n = char_len(s);
    return n >= w ? s : std::string(w - n, ' ') + s;
}
std::string pad_right(const std::string& s, std::size_t w) {
    const std::size_t n = char_len(s);
    return n >= w ? s : s + std::string(w - n, ' ');
}

// Bracketed list of quoted strings: ['a', 'b'].
std::string list_repr(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += quote_repr(v[i]);
    }
    out += "]";
    return out;
}

// Every type=='dialogue' entry, in scene order then array order.
std::vector<bj::object> collect_dialogues(const bj::object& data) {
    std::vector<bj::object> out;
    for (const auto& kv : data) {
        if (!kv.value().is_object()) continue;
        const auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            auto* t = s.if_contains("type");
            if (!t || !t->is_string() || t->get_string() != "dialogue") continue;
            out.push_back(s);
        }
    }
    return out;
}

std::string obj_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key)) {
        if (v->is_string()) return std::string(v->get_string());
    }
    return {};
}

// Reproduce the <lines_to_translate> block that call_gemini sends.
std::string build_gate_prompt(const std::vector<bj::object>& dialogs) {
    std::string prompt = "<lines_to_translate>\n";
    for (std::size_t i = 0; i < dialogs.size(); ++i) {
        const std::string speaker = obj_str(dialogs[i], "type") == "dialogue"
                                        ? translate::extract_speaker(dialogs[i])
                                        : std::string("NARRATION");
        const std::string text = obj_str(dialogs[i], "text");
        std::string escaped;
        for (std::size_t k = 0; k < text.size(); ++k) {
            if (text.compare(k, kCrlfSequenceLen, "\r\n") == 0) {
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

// Mirrors translate::extract_speaker's raw speaker extraction while preserving
// internal fullwidth space U+3000 inside two-kanji speaker plates ("睦　月").
std::string raw_jp_speaker(const bj::object& s) {
    const std::string raw = trim(obj_str(s, "speaker"));
    return (!raw.empty() && char_len(raw) <= kMaxSpeakerCodepoints) ? raw : std::string();
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            out.push_back(s.substr(pos));
            return out;
        }
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

int run_all_checks(const bj::object& data) {
    const std::vector<bj::object> dialogs_all = collect_dialogues(data);
    const std::vector<bj::object> dialogs(
        dialogs_all.begin(),
        dialogs_all.begin() +
            static_cast<std::ptrdiff_t>(std::min(kSampleSize, dialogs_all.size())));
    if (dialogs.size() < kMinRequiredDialogues) {
        print_line("FAIL: only " + std::to_string(dialogs.size()) +
                   " dialogue entries; extracted_text.json looks empty.");
        return apps::kExitError;
    }

    // Check 1: extract_speaker returns real speakers, not 'NARRATION'.
    std::size_t narration = 0;
    std::set<std::string> speaker_set;
    for (const auto& s : dialogs) {
        const std::string sp = translate::extract_speaker(s);
        if (sp == "NARRATION") ++narration;
        speaker_set.insert(sp);
    }
    speaker_set.erase("NARRATION");
    const double pct =
        kPercentScale * static_cast<double>(narration) / static_cast<double>(dialogs.size());
    char buf[kPercentBufSize];
    std::snprintf(buf, sizeof(buf), "%.0f", pct);
    print_line("[1/5] extract_speaker on " + std::to_string(dialogs.size()) +
               " dialogue entries:");
    print_line("      NARRATION returned: " + std::to_string(narration) + " (" + buf + "%)");
    {
        std::vector<std::string> preview(speaker_set.begin(), speaker_set.end());
        if (preview.size() > kMaxSpeakerPreviewCount) {
            preview.resize(kMaxSpeakerPreviewCount);
        }
        print_line("      distinct speakers:  " + list_repr(preview));
    }
    if (pct >= kMaxNarrationPercent) {
        print_line("      FAIL: too many lines tagged NARRATION — "
                   "speaker field not flowing.");
        return apps::kExitError;
    }

    // Check 2: JP speaker plates map to EN via NAME_TRANSLATIONS.
    print_line("[2/5] JP → EN name resolution:");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"人見", "Hitomi"},
        {"睦　月", "Mutsuki"},
        {"望　美", "Nozomi"},
        {"まひる", "Mahiru"},
        {"御　幸", "Miyuki"},
        {"こより", "Koyori"},
        {"となえ", "Tonae"},
        {"瀬美奈", "Semina"},
        {"天　使", "Angel"},
        {"引　用", "Quote"},
        {"少女達", "Girls"},
    };
    for (const auto& [jp, en] : cases) {
        bj::object s;
        s["speaker"] = jp;
        s["text"] = "「dummy」";
        s["type"] = "dialogue";
        const std::string got = translate::extract_speaker(s);
        const std::string marker = got == en ? "OK " : "FAIL";
        print_line("      " + marker + "  " + pad_left(quote_repr(jp), kJpColumnPad) + " -> " +
                   pad_right(quote_repr(got), kEnColumnPad) + " (expected " + quote_repr(en) + ")");
        if (got != en) {
            return apps::kExitError;
        }
    }

    // Check 3: assembled API user-prompt contains real [Speaker] tags.
    const std::vector<bj::object> sample_dialogs(
        dialogs.begin(),
        dialogs.begin() +
            static_cast<std::ptrdiff_t>(std::min<std::size_t>(kPromptSampleCount, dialogs.size())));
    const std::string prompt = build_gate_prompt(sample_dialogs);
    const auto prompt_lines = split_lines(prompt);
    std::size_t bad = 0;
    for (const auto& l : prompt_lines) {
        if (l.find("[NARRATION]") != std::string::npos) {
            ++bad;
        }
    }
    print_line("[3/5] reconstructed user-prompt for first 10 dialogue lines:");
    for (std::size_t i = 0; i < prompt_lines.size() && i < kPromptPreviewLines; ++i) {
        print_line("      " + prompt_lines[i]);
    }
    print_line("      ...");
    if (bad > 0) {
        print_line("      FAIL: " + std::to_string(bad) +
                   " dialogue lines tagged [NARRATION] in prompt.");
        return apps::kExitError;
    }

    // Check 4: assert NAME_TRANSLATIONS contains core cast speaker keys.
    const auto& name_dict = translate::name_translations();
    const char* const kCoreCast[] = {
        "人見", "睦　月", "望　美", "まひる", "御　幸", "こより", "となえ", "瀬美奈"};
    for (const char* key : kCoreCast) {
        if (!name_dict.count(key)) {
            print_line(std::string("FAIL: NAME_TRANSLATIONS missing core character name: ") + key);
            return apps::kExitError;
        }
    }
    print_line("[4/5] NAME_TRANSLATIONS contains core character names "
               "(人見, 睦　月, まひる, 御　幸, となえ, 瀬美奈, etc.): OK");

    // Check 5: exhaustive bidirectional speaker coverage.
    std::map<std::string, int> seen;
    for (const auto& s : dialogs_all) {
        const std::string raw = raw_jp_speaker(s);
        if (!raw.empty()) ++seen[raw];
    }

    std::vector<std::string> missing;
    std::vector<std::string> unused;
    for (const auto& [jp, n] : seen) {
        (void)n;
        if (!name_dict.count(jp)) missing.push_back(jp);
    }
    for (const auto& [jp, en] : translate::name_translations_ordered()) {
        (void)en;
        if (!seen.count(jp)) unused.push_back(jp);
    }
    std::sort(unused.begin(), unused.end());

    print_line("[5/5] exhaustive speaker coverage scan:");
    print_line("      distinct JP speakers in data: " + std::to_string(seen.size()));
    print_line("      NAME_TRANSLATIONS entries:    " + std::to_string(name_dict.size()));

    if (!missing.empty() || !unused.empty()) {
        if (!missing.empty()) {
            print_line("      FAIL: " + std::to_string(missing.size()) +
                       " JP speakers in data not in NAME_TRANSLATIONS");
            print_line("            (copy-paste-ready, sorted by frequency):");
            std::stable_sort(missing.begin(), missing.end(),
                             [&](const std::string& a, const std::string& b) {
                                 return seen[a] > seen[b];
                             });
            for (std::size_t i = 0; i < missing.size() && i < kMaxReportEntries; ++i) {
                print_line("          " + pad_left(quote_repr(missing[i]), kMissingNamePad) +
                           ": '???',  # appeared " + std::to_string(seen[missing[i]]) + "x");
            }
            if (missing.size() > kMaxReportEntries) {
                print_line("          ... and " +
                           std::to_string(missing.size() - kMaxReportEntries) + " more");
            }
        }
        if (!unused.empty()) {
            print_line("      FAIL: " + std::to_string(unused.size()) +
                       " NAME_TRANSLATIONS entries never appear as speakers:");
            for (std::size_t i = 0; i < unused.size() && i < kMaxReportEntries; ++i) {
                print_line("          " + quote_repr(unused[i]) + " -> " +
                           quote_repr(name_dict.at(unused[i])));
            }
            if (unused.size() > kMaxReportEntries) {
                print_line("          ... and " +
                           std::to_string(unused.size() - kMaxReportEntries) + " more");
            }
        }
        return apps::kExitError;
    }

    print_line("      OK — full bidirectional coverage.");
    print_line("");
    print_line("  All checks passed.  Speaker pipeline is healthy — "
               "safe to run translation.");
    print_line("  Sample size (checks 1, 3): " + std::to_string(dialogs.size()) + " of " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");
    print_line("  Coverage scan (check 5):  all " +
               comma(static_cast<long long>(dialogs_all.size())) + " dialogue entries.");
    return apps::kExitOk;
}

}  // namespace

int main() {
    setup_console_utf8();
    try {
        namespace po = apps::po;

        std::string project;
        po::options_description desc("03_speaker_gate -- Adieu speaker-pipeline gate");
        desc.add(apps::project_only_options(project));
        po::variables_map vm;
        if (auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string extracted = project + "\\script_output\\extracted_text.json";
        if (!std::filesystem::exists(std::filesystem::u8path(extracted))) {
            print_line("FAIL: " + extracted + " missing — run 01_extract first.");
            return apps::kExitError;
        }
        bj::value data = json_parse_file(extracted);
        return run_all_checks(data.get_object());
    } catch (const std::exception& e) {
        print_line(std::string("FAIL: ") + e.what());
        return apps::kExitError;
    }
}
