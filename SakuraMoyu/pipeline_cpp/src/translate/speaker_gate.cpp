// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "speaker_gate.h"

#include <algorithm>
#include <string>
#include <vector>

#include "fvp/fvp_hcb.h"
#include "translate/glossary.h"

namespace fav::translate {

namespace {

// Core cast expected in the glossary (a sanity check beyond the registry:
// the registry only lists who the bytecode happens to register a name window
// for, and a heroine who never gets one would slip through check 1).
const std::vector<std::string> CORE_HEROINES = {
    "クロ", "ハル", "千和", "姫織", "ましろ", "十夜", "あさひ", "あず咲", "ナナ"};
const std::string PROTAGONIST = "大雅";

// The gate's own JP test -- a single wide range, deliberately looser than
// util.h's is_jp (which excludes the CJK punctuation block).
bool gate_has_jp(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        char32_t cp = utf8_next(s, i);
        if (cp >= 0x3040 && cp <= 0x9FFF) return true;
    }
    return false;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : ", ") + p;
    return out;
}

SpeakerCheckReport fail(SpeakerCheckReport report, int check, std::string reason) {
    report.passed = false;
    report.failed_check = check;
    report.reason = std::move(reason);
    return report;
}

}  // namespace

std::map<std::string, int> cast_registry(const Bytes& hcb) {
    std::vector<fvp::Event> events = fvp::walk_events(hcb);
    std::map<std::string, int> cast;
    for (std::size_t i = 0; i + 1 < events.size(); ++i) {
        if (events[i].op != 0x10 || events[i + 1].op != 0x0E) continue;
        const auto& payload = events[i + 1].payload;
        if (!payload || payload->empty()) continue;
        const std::string& s = *payload;
        // Costume/emotion tags ('_'-prefixed), hidden states ('？'), and
        // non-JP noise (filenames, button labels, texture IDs) are not cast.
        if (s.rfind("_", 0) == 0) continue;
        if (contains(s, "？") || contains(s, "?")) continue;
        // Registered names carry U+3000 padding as well as ASCII whitespace;
        // trim covers the full Unicode whitespace set, so one pass is enough.
        const std::string stripped = trim(s);
        if (stripped.empty() || !gate_has_jp(stripped)) continue;
        ++cast[stripped];
    }
    return cast;
}

std::unordered_map<std::uint32_t, std::string> map_line_speakers(const Bytes& hcb) {
    std::unordered_map<std::uint32_t, std::string> line_speakers;
    if (hcb.size() < 4) return line_speakers;

    const std::uint32_t script_len =
        static_cast<std::uint32_t>(hcb[0]) |
        (static_cast<std::uint32_t>(hcb[1]) << 8) |
        (static_cast<std::uint32_t>(hcb[2]) << 16) |
        (static_cast<std::uint32_t>(hcb[3]) << 24);
    const std::size_t end = std::min<std::size_t>(script_len, hcb.size());

    struct DecodedInsn {
        std::uint32_t offset = 0;
        std::uint8_t op = 0;
        std::uint32_t d_arg = 0;
        std::string s_arg;
    };

    std::vector<DecodedInsn> insns;
    std::size_t pos = 4;
    while (pos < end) {
        const std::uint32_t instr_off = static_cast<std::uint32_t>(pos);
        const std::uint8_t op = hcb[pos];
        const std::vector<fvp::Arg>* args = fvp::opcode_args(op);
        if (!args) break;
        ++pos;
        DecodedInsn insn;
        insn.offset = instr_off;
        insn.op = op;
        for (fvp::Arg a : *args) {
            switch (a) {
            case fvp::Arg::B: pos += 1; break;
            case fvp::Arg::W: pos += 2; break;
            case fvp::Arg::D:
                if (pos + 4 <= hcb.size()) {
                    insn.d_arg = static_cast<std::uint32_t>(hcb[pos]) |
                                 (static_cast<std::uint32_t>(hcb[pos + 1]) << 8) |
                                 (static_cast<std::uint32_t>(hcb[pos + 2]) << 16) |
                                 (static_cast<std::uint32_t>(hcb[pos + 3]) << 24);
                }
                pos += 4;
                break;
            case fvp::Arg::S: {
                const std::size_t str_len = pos < hcb.size() ? hcb[pos] : 0;
                ++pos;
                const std::size_t take = str_len > 0 ? str_len - 1 : 0;
                const std::size_t lo = std::min(pos, hcb.size());
                const std::size_t hi = std::min(pos + take, hcb.size());
                if (auto s = cp932_to_utf8_strict(hcb.data() + lo, hi - lo))
                    insn.s_arg = std::move(*s);
                pos += str_len;
                break;
            }
            }
        }
        insns.push_back(std::move(insn));
    }

    const auto& dict = name_translations();
    constexpr std::uint32_t SPEAKER_SUB_LIMIT = 0x2000;
    std::unordered_map<std::uint32_t, std::string> sub_to_speaker;

    std::uint32_t cur_func = 0;
    bool in_func = false;
    std::vector<std::string> cur_jp_names;

    auto flush_func = [&]() {
        if (!in_func || cur_func >= SPEAKER_SUB_LIMIT || cur_jp_names.empty()) return;
        bool all_protagonist = true;
        std::string unique_en;
        bool ambiguous = false;
        for (const auto& jp : cur_jp_names) {
            if (jp != "ぼく" && jp != "大雅") all_protagonist = false;
            auto it = dict.find(jp);
            if (it == dict.end() || it->second == "???") continue;
            if (unique_en.empty()) unique_en = it->second;
            else if (unique_en != it->second) ambiguous = true;
        }
        if (all_protagonist) {
            sub_to_speaker[cur_func] = "Taiga";
        } else if (!ambiguous && !unique_en.empty()) {
            sub_to_speaker[cur_func] = unique_en;
        }
    };

    for (const auto& insn : insns) {
        if (insn.op == 0x01) {
            flush_func();
            cur_func = insn.offset;
            in_func = true;
            cur_jp_names.clear();
        } else if (in_func && cur_func < SPEAKER_SUB_LIMIT && insn.op == 0x0e) {
            if (insn.s_arg.rfind("_", 0) == 0) continue;
            if (contains(insn.s_arg, "？") || contains(insn.s_arg, "?")) continue;
            const std::string stripped = trim(insn.s_arg);
            if (!stripped.empty() && dict.count(stripped))
                cur_jp_names.push_back(stripped);
        }
    }
    flush_func();

    for (std::size_t i = 0; i + 1 < insns.size(); ++i) {
        if (insns[i].op == 0x02 && insns[i + 1].op == 0x0e) {
            auto it = sub_to_speaker.find(insns[i].d_arg);
            if (it != sub_to_speaker.end())
                line_speakers.emplace(insns[i + 1].offset, it->second);
        }
    }
    return line_speakers;
}

SpeakerCheckReport run_speaker_checks(const Bytes& hcb) {
    SpeakerCheckReport report;
    const auto& name_dict = name_translations();
    const std::string sys_prompt = SYSTEM_PROMPT;
    report.glossary_entries = name_dict.size();

    // -- Check 1: every registered cast name has a translation --
    std::map<std::string, int> cast = cast_registry(hcb);
    report.registry_names = cast.size();
    std::vector<std::string> missing;
    for (const auto& [jp, n] : cast)
        if (!name_dict.count(jp)) missing.push_back(jp);
    print_line("[1/4] Bytecode name-window registry: " + std::to_string(cast.size()) +
               " distinct cast names");
    print_line("      NAME_TRANSLATIONS entries:    " + std::to_string(name_dict.size()));
    if (!missing.empty()) {
        print_line("      FAIL: " + std::to_string(missing.size()) +
                   " registered cast names not in NAME_TRANSLATIONS:");
        std::sort(missing.begin(), missing.end(),
                  [&](const std::string& a, const std::string& b) {
                      return cast[a] != cast[b] ? cast[a] > cast[b] : a < b;
                  });
        for (const auto& jp : missing)
            print_line("          '" + jp + "': '???',  # appeared " +
                       std::to_string(cast[jp]) + "x in registry");
        return fail(report, 1,
                    std::to_string(missing.size()) +
                        " registered cast name(s) have no translation: " + join(missing));
    }
    print_line("      OK - every registered cast name has a translation.");

    // -- Check 2: dict integrity (no empty values) --
    std::vector<std::string> bad;
    for (const auto& [jp, en] : name_translations_ordered())
        if (trim(en).empty()) bad.push_back(jp);
    print_line("[2/4] NAME_TRANSLATIONS integrity check:");
    if (!bad.empty()) {
        print_line("      FAIL: " + std::to_string(bad.size()) +
                   " entries with empty translations:");
        for (std::size_t i = 0; i < bad.size() && i < 10; ++i)
            print_line("          '" + bad[i] + "'");
        return fail(report, 2,
                    std::to_string(bad.size()) + " glossary entries are empty: " + join(bad));
    }
    print_line("      OK - all " + std::to_string(name_dict.size()) +
               " entries have non-empty string translations.");

    // -- Check 3: protagonist + core heroines all present --
    std::vector<std::string> cast_missing;
    if (!name_dict.count(PROTAGONIST)) cast_missing.push_back(PROTAGONIST);
    for (const auto& c : CORE_HEROINES)
        if (!name_dict.count(c)) cast_missing.push_back(c);
    print_line("[3/4] Protagonist + core heroines in NAME_TRANSLATIONS:");
    if (!cast_missing.empty()) {
        print_line("      FAIL: " + join(cast_missing) + " missing from NAME_TRANSLATIONS.");
        return fail(report, 3,
                    "core cast missing from NAME_TRANSLATIONS: " + join(cast_missing));
    }
    print_line("      OK - '" + PROTAGONIST + "' (protagonist) and " +
               std::to_string(CORE_HEROINES.size()) + " core heroines all present.");

    // -- Check 4: each cast member is also referenced in the system prompt's
    // <character_dossier>, so Gemini actually sees the romanization in context --
    const std::size_t start = sys_prompt.find("<character_dossier>");
    const std::size_t end = sys_prompt.find("</character_dossier>");
    if (start == std::string::npos || end == std::string::npos) {
        print_line("[4/4] FAIL: SYSTEM_PROMPT has no <character_dossier> section.");
        return fail(report, 4, "SYSTEM_PROMPT has no <character_dossier> section");
    }
    const std::string dossier = sys_prompt.substr(start, end - start);
    std::vector<std::string> not_in_dossier;
    if (!contains(dossier, PROTAGONIST)) not_in_dossier.push_back(PROTAGONIST);
    for (const auto& c : CORE_HEROINES)
        if (!contains(dossier, c)) not_in_dossier.push_back(c);
    print_line("[4/4] Core cast referenced in <character_dossier>:");
    if (!not_in_dossier.empty()) {
        print_line("      FAIL: " + join(not_in_dossier) +
                   " not mentioned in <character_dossier>.");
        return fail(report, 4,
                    "core cast missing from <character_dossier>: " + join(not_in_dossier));
    }
    print_line("      OK - protagonist + all core heroines named in the dossier.");

    print_line("");
    print_line("  All checks passed.  Cast roster is healthy - safe to translate.");
    print_line("  NOTE: FAVORITE's FVP engine doesn't tag per-line speakers.  Gemini");
    print_line("  infers them from prose context using <character_dossier> + rolling");
    print_line("  context window.  Quality depends on dossier richness, not per-line tags.");

    report.passed = true;
    return report;
}

}  // namespace fav::translate
