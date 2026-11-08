// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "translate/speaker_gate.h"

#include <algorithm>
#include <map>
#include <vector>

#include <boost/regex.hpp>

#include "common/util.h"
#include "translate/gemini_client.h"

namespace oretsuba::translate {

namespace {

namespace bj = boost::json;

constexpr int kCheckCorpusCoverage = 1;
constexpr int kCheckGlossaryIntegrity = 2;
constexpr int kCheckCoreCastPresent = 3;
constexpr int kCheckSystemPromptDossier = 4;
constexpr std::size_t kMaxErrorPreviewItems = 10U;
constexpr std::size_t kMidpointDivisor = 2U;

constexpr std::string_view kMidlinePauseToken = "$&";
constexpr std::string_view kDossierStartTag = "<character_dossier>";
constexpr std::string_view kDossierEndTag = "</character_dossier>";
constexpr std::string_view kTerminologyStartTag = "<terminology>";
constexpr std::string_view kTerminologyEndTag = "</terminology>";

const std::vector<std::string>& CoreCastList() {
    static const std::vector<std::string> kCoreCast = {
        "鷹志",
        "聖騎士ホーク",
        "鷲介",
        "隼人",
        "ヨージ",
        "ガルーダ",
        "翔",
        "小鳩",
        "明日香",
        "日和子",
        "鳴",
        "京",
        "亜衣",
        "狩男",
        "英里子",
        "紀奈子",
        "和馬",
        "春恵",
        "蔵人",
    };
    return kCoreCast;
}

std::string JoinStrings(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0U) {
            out += ", ";
        }
        out += items[i];
    }
    return out;
}

SpeakerCheckReport MakeFailure(SpeakerCheckReport report,
                               int check_index,
                               std::string reason) {
    report.passed = false;
    report.failed_check = check_index;
    report.reason = std::move(reason);
    return report;
}

std::size_t FindBestClauseSplitIndex(std::string_view text) {
    if (text.size() < kMidpointDivisor) {
        return std::string_view::npos;
    }
    const std::size_t mid = text.size() / kMidpointDivisor;
    std::size_t best_pos = std::string_view::npos;
    std::size_t best_dist = text.size();

    for (std::size_t i = 1U; i + 1U < text.size(); ++i) {
        const char ch = text[i];
        const bool is_punct_boundary =
            (ch == '.' || ch == ',' || ch == ';' || ch == '!' || ch == '?') &&
            text[i + 1U] == ' ';
        if (!is_punct_boundary) {
            continue;
        }
        const std::size_t split_at = i + 2U;
        const std::size_t dist = (split_at > mid) ? (split_at - mid) : (mid - split_at);
        if (dist < best_dist && split_at < text.size()) {
            best_dist = dist;
            best_pos = split_at;
        }
    }
    if (best_pos != std::string_view::npos) {
        return best_pos;
    }

    for (std::size_t i = 1U; i + 1U < text.size(); ++i) {
        if (text[i] != ' ') {
            continue;
        }
        const std::size_t split_at = i + 1U;
        const std::size_t dist = (split_at > mid) ? (split_at - mid) : (mid - split_at);
        if (dist < best_dist) {
            best_dist = dist;
            best_pos = split_at;
        }
    }
    return best_pos;
}

}  // namespace

struct SpeakerGate::Impl {
    boost::regex leading_control_re{R"re(^((?:<[-+]?\d+>\\?|\$&)+)(.*)$)re"};
    boost::regex angle_ruby_re{R"re(<"([^"|]+)\|([^"]+)">)re"};
    boost::regex square_ruby_re{R"re(\[([^|\]]+)\|([^\]]+)\])re"};

    InlineControlDecomposition DecomposeImpl(std::string_view text) const {
        InlineControlDecomposition out;
        std::string current(text);

        boost::smatch match;
        if (boost::regex_match(current, match, leading_control_re)) {
            out.leading_control_prefix = match[1].str();
            current = match[2].str();
        }

        current = boost::regex_replace(current, angle_ruby_re, "$1");
        current = boost::regex_replace(current, square_ruby_re, "$2");

        out.has_midline_pause = (current.find(kMidlinePauseToken) != std::string::npos);
        out.has_inline_linebreak = (current.find('\\') != std::string::npos);
        out.clean_text = std::move(current);
        return out;
    }

    std::string PreserveImpl(std::string_view source_jp,
                             std::string_view translated_en) const {
        const InlineControlDecomposition src_info = DecomposeImpl(source_jp);
        std::string cleaned_en = StripSpeakerTags(translated_en);
        if (cleaned_en.empty()) {
            return cleaned_en;
        }

        if (src_info.has_midline_pause &&
            cleaned_en.find(kMidlinePauseToken) == std::string::npos) {
            const std::size_t split_idx = FindBestClauseSplitIndex(cleaned_en);
            if (split_idx != std::string_view::npos && split_idx < cleaned_en.size()) {
                cleaned_en.insert(split_idx, kMidlinePauseToken);
            }
        }

        if (src_info.has_inline_linebreak &&
            cleaned_en.find('\\') == std::string::npos) {
            const std::size_t split_idx = FindBestClauseSplitIndex(cleaned_en);
            if (split_idx != std::string_view::npos && split_idx < cleaned_en.size()) {
                cleaned_en.insert(split_idx, "\\");
            }
        }

        return cleaned_en;
    }

    SpeakerCheckReport ValidateCorpusImpl(const bj::value& extracted_root,
                                          const Glossary& glossary) const {
        SpeakerCheckReport report;
        const auto& speaker_map = glossary.SpeakerTranslationMap();
        const auto& ordered_speakers = glossary.OrderedSpeakerTranslations();
        const std::string& sys_prompt = glossary.SystemPrompt();
        report.glossary_entries = speaker_map.size();

        if (!extracted_root.is_object()) {
            return MakeFailure(
                report,
                kCheckCorpusCoverage,
                "extracted_text.json root must be a JSON object");
        }

        std::map<std::string, int> speaker_counts;
        std::size_t inline_tag_count = 0U;

        const bj::object* scripts_container = &extracted_root.as_object();
        if (const auto* scripts_sub = scripts_container->if_contains("scripts");
            scripts_sub && scripts_sub->is_object()) {
            scripts_container = &scripts_sub->as_object();
        }

        for (const auto& script_kv : *scripts_container) {
            const bj::array* entries_arr = nullptr;
            if (script_kv.value().is_array()) {
                entries_arr = &script_kv.value().as_array();
            } else if (script_kv.value().is_object()) {
                const auto& script_obj = script_kv.value().as_object();
                if (const auto* strings_val = script_obj.if_contains("strings");
                    strings_val && strings_val->is_array()) {
                    entries_arr = &strings_val->as_array();
                } else if (const auto* lines_val = script_obj.if_contains("lines");
                           lines_val && lines_val->is_array()) {
                    entries_arr = &lines_val->as_array();
                }
            }
            if (!entries_arr) {
                continue;
            }
            for (const auto& entry_val : *entries_arr) {
                if (!entry_val.is_object()) {
                    continue;
                }
                const auto& entry = entry_val.as_object();
                if (const auto* spk_val = entry.if_contains("speaker");
                    spk_val && spk_val->is_string()) {
                    const std::string spk = TrimUnicode(std::string(spk_val->as_string()));
                    if (!spk.empty()) {
                        ++speaker_counts[spk];
                    }
                }
                if (const auto* txt_val = entry.if_contains("text");
                    txt_val && txt_val->is_string()) {
                    const InlineControlDecomposition decomp =
                        DecomposeImpl(std::string_view(txt_val->as_string()));
                    if (!decomp.leading_control_prefix.empty() ||
                        decomp.has_midline_pause ||
                        decomp.has_inline_linebreak) {
                        ++inline_tag_count;
                    }
                }
            }
        }

        report.corpus_speakers = speaker_counts.size();
        report.inline_tag_lines = inline_tag_count;

        std::vector<std::string> missing_speakers;
        for (const auto& [jp_spk, count] : speaker_counts) {
            if (speaker_map.find(jp_spk) == speaker_map.end()) {
                missing_speakers.push_back(jp_spk);
            }
        }

        PrintLine("[1/4] Extracted corpus speaker tags: " +
                  std::to_string(speaker_counts.size()) +
                  " distinct speakers (" + std::to_string(inline_tag_count) +
                  " inline-tagged lines)");
        PrintLine("      Glossary speaker entries:      " +
                  std::to_string(speaker_map.size()));
        if (!missing_speakers.empty()) {
            std::sort(missing_speakers.begin(), missing_speakers.end(),
                      [&](const std::string& lhs, const std::string& rhs) {
                          return speaker_counts[lhs] != speaker_counts[rhs]
                                     ? speaker_counts[lhs] > speaker_counts[rhs]
                                     : lhs < rhs;
                      });
            return MakeFailure(
                report,
                kCheckCorpusCoverage,
                std::to_string(missing_speakers.size()) +
                    " speaker tag(s) have no translation: " +
                    JoinStrings(missing_speakers));
        }
        PrintLine("      OK - every extracted speaker tag has a glossary translation.");

        std::vector<std::string> empty_entries;
        for (const auto& [jp, en] : ordered_speakers) {
            if (TrimUnicode(jp).empty() || TrimUnicode(en).empty()) {
                empty_entries.push_back(jp);
                if (empty_entries.size() >= kMaxErrorPreviewItems) {
                    break;
                }
            }
        }
        PrintLine("[2/4] Glossary integrity check:");
        if (!empty_entries.empty()) {
            return MakeFailure(
                report,
                kCheckGlossaryIntegrity,
                std::to_string(empty_entries.size()) +
                    " glossary entries are empty: " +
                    JoinStrings(empty_entries));
        }
        PrintLine("      OK - all " + std::to_string(speaker_map.size()) +
                  " entries have non-empty translations.");

        std::vector<std::string> missing_core;
        for (const auto& core_name : CoreCastList()) {
            if (speaker_map.find(core_name) == speaker_map.end()) {
                missing_core.push_back(core_name);
            }
        }
        PrintLine("[3/4] Core protagonists, alter-egos & heroines in glossary:");
        if (!missing_core.empty()) {
            return MakeFailure(
                report,
                kCheckCoreCastPresent,
                "core cast missing from glossary: " + JoinStrings(missing_core));
        }
        PrintLine("      OK - all " + std::to_string(CoreCastList().size()) +
                  " core cast members present.");

        const std::size_t dossier_start = sys_prompt.find(kDossierStartTag);
        const std::size_t dossier_end = sys_prompt.find(kDossierEndTag);
        const std::size_t term_start = sys_prompt.find(kTerminologyStartTag);
        const std::size_t term_end = sys_prompt.find(kTerminologyEndTag);
        if (dossier_start == std::string::npos || dossier_end == std::string::npos ||
            term_start == std::string::npos || term_end == std::string::npos) {
            return MakeFailure(
                report,
                kCheckSystemPromptDossier,
                "SYSTEM_PROMPT is missing <character_dossier> or <terminology> section");
        }

        const std::string dossier =
            sys_prompt.substr(dossier_start, dossier_end - dossier_start);
        std::vector<std::string> missing_in_dossier;
        for (const auto& core_name : CoreCastList()) {
            if (dossier.find(core_name) == std::string::npos) {
                missing_in_dossier.push_back(core_name);
            }
        }
        PrintLine("[4/4] Core cast referenced in <character_dossier>:");
        if (!missing_in_dossier.empty()) {
            return MakeFailure(
                report,
                kCheckSystemPromptDossier,
                "core cast missing from <character_dossier>: " +
                    JoinStrings(missing_in_dossier));
        }
        PrintLine("      OK - all core cast and Gretagik personas referenced in dossier.");

        report.passed = true;
        return report;
    }
};

SpeakerGate::SpeakerGate() : impl_(std::make_unique<Impl>()) {}
SpeakerGate::SpeakerGate(SpeakerGate&& other) noexcept = default;
SpeakerGate& SpeakerGate::operator=(SpeakerGate&& other) noexcept = default;
SpeakerGate::~SpeakerGate() = default;

SpeakerCheckReport SpeakerGate::ValidateCorpus(const bj::value& extracted_root,
                                               const Glossary& glossary) const {
    return impl_->ValidateCorpusImpl(extracted_root, glossary);
}

SpeakerCheckReport SpeakerGate::ValidateExtractedFile(const std::string& extracted_json_path,
                                                      const Glossary& glossary) const {
    const bj::value root = JsonParseFile(extracted_json_path);
    return impl_->ValidateCorpusImpl(root, glossary);
}

InlineControlDecomposition SpeakerGate::DecomposeInlineControls(std::string_view text) const {
    return impl_->DecomposeImpl(text);
}

std::string SpeakerGate::PreserveInlineControls(std::string_view source_jp,
                                                std::string_view translated_en) const {
    return impl_->PreserveImpl(source_jp, translated_en);
}

SpeakerCheckReport RunSpeakerChecks(const std::string& extracted_json_path,
                                    const Glossary& glossary) {
    const SpeakerGate gate;
    return gate.ValidateExtractedFile(extracted_json_path, glossary);
}

}  // namespace oretsuba::translate
