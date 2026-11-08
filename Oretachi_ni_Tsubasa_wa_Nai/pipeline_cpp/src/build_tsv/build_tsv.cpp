// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "build_tsv/build_tsv.h"

#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"

namespace oretsuba::build_tsv {

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr int kExitOk = 0;
constexpr std::string_view kMidlinePauseToken = "$&";

std::string ReplaceAll(std::string input, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return input;
    }
    std::string out;
    out.reserve(input.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = input.find(from, pos);
        if (hit == std::string::npos) {
            out.append(input, pos, std::string::npos);
            return out;
        }
        out.append(input, pos, hit - pos);
        out.append(to.data(), to.size());
        pos = hit + from.size();
    }
}

bool IsAssetOrSpriteTag(std::string_view text) {
    if (text.empty()) {
        return true;
    }
    return text.front() == '_' || text.find('/') != std::string_view::npos;
}

std::string WrapSingleSegmentWords(std::string_view segment, std::size_t max_columns) {
    const std::string cleaned = FlattenText(segment);
    if (cleaned.empty() || max_columns == 0U || cleaned.size() <= max_columns) {
        return cleaned;
    }

    std::vector<std::string_view> words;
    std::size_t pos = 0;
    while (pos < cleaned.size()) {
        while (pos < cleaned.size() && cleaned[pos] == ' ') {
            ++pos;
        }
        if (pos >= cleaned.size()) {
            break;
        }
        const std::size_t next_space = cleaned.find(' ', pos);
        if (next_space == std::string::npos) {
            words.push_back(std::string_view(cleaned).substr(pos));
            break;
        }
        words.push_back(std::string_view(cleaned).substr(pos, next_space - pos));
        pos = next_space + 1U;
    }

    std::string out;
    out.reserve(cleaned.size() + cleaned.size() / max_columns + 4U);
    std::size_t line_len = 0;

    for (std::string_view word : words) {
        if (line_len > 0U && line_len + 1U + word.size() <= max_columns) {
            out += ' ';
            out.append(word.data(), word.size());
            line_len += 1U + word.size();
            continue;
        }

        if (line_len > 0U) {
            out += '\\';
            line_len = 0;
        }

        while (word.size() > max_columns) {
            out.append(word.data(), max_columns);
            word.remove_prefix(max_columns);
            if (!word.empty()) {
                out += '\\';
            }
        }

        if (!word.empty()) {
            out.append(word.data(), word.size());
            line_len = word.size();
        }
    }

    return out;
}

std::string WrapBackslashDelimitedClause(std::string_view clause, std::size_t max_columns) {
    if (clause.find('\\') == std::string_view::npos) {
        return WrapSingleSegmentWords(clause, max_columns);
    }

    std::string out;
    std::size_t pos = 0;
    bool first = true;
    while (pos <= clause.size()) {
        const std::size_t next_bs = clause.find('\\', pos);
        const std::string_view part =
            (next_bs == std::string_view::npos)
                ? clause.substr(pos)
                : clause.substr(pos, next_bs - pos);
        const std::string wrapped_part = WrapSingleSegmentWords(part, max_columns);
        if (!wrapped_part.empty()) {
            if (!first) {
                out += '\\';
            }
            out += wrapped_part;
            first = false;
        }
        if (next_bs == std::string_view::npos) {
            break;
        }
        pos = next_bs + 1U;
    }
    return out;
}

bool ShouldWrapEntryType(std::string_view entry_type) {
    if (entry_type == "choice" || entry_type == "title" || entry_type == "ui") {
        return false;
    }
    return true;
}

}  // namespace

std::string StripInlineControlWrappers(std::string_view text) {
    static const boost::regex kLeadingControlRe(R"re(^((?:<[-+]?\d+>\\?|\$&)+))re");
    static const boost::regex kAngleRubyRe(R"re(<"([^"|]+)\|([^"]+)">)re");
    static const boost::regex kSquareRubyRe(R"re(\[([^|\]]+)\|([^\]]+)\])re");

    std::string out(text);
    out = boost::regex_replace(out, kLeadingControlRe, "");
    out = boost::regex_replace(out, kAngleRubyRe, "$1");
    out = boost::regex_replace(out, kSquareRubyRe, "$2");
    return out;
}

std::string FlattenText(std::string_view text) {
    std::string s(text);
    s = ReplaceAll(std::move(s), "\r\n", " ");
    s = ReplaceAll(std::move(s), "\r", " ");
    s = ReplaceAll(std::move(s), "\n", " ");
    s = ReplaceAll(std::move(s), "\t", " ");
    return TrimUnicode(s);
}

std::string NormalizeTsvKey(std::string_view jp_text) {
    const std::string stripped = StripInlineControlWrappers(jp_text);
    const std::string flat = FlattenText(stripped);
    if (IsAssetOrSpriteTag(flat)) {
        return {};
    }
    return flat;
}

std::string SanitizeTsvValue(std::string_view en_text) {
    return FlattenText(translate::StripSpeakerTags(en_text));
}

std::string EscapeForTsv(std::string_view text) {
    std::string s(text);
    s = ReplaceAll(std::move(s), "\\", "\\\\");
    s = ReplaceAll(std::move(s), "\t", "\\t");
    return s;
}

std::string WrapEnglishLine(std::string_view text, std::size_t max_columns) {
    const std::string sanitized = SanitizeTsvValue(text);
    if (sanitized.empty() || max_columns == 0U) {
        return sanitized;
    }

    if (sanitized.find(kMidlinePauseToken) == std::string::npos) {
        return WrapBackslashDelimitedClause(sanitized, max_columns);
    }

    std::string out;
    std::size_t pos = 0;
    bool first = true;
    while (pos <= sanitized.size()) {
        const std::size_t hit = sanitized.find(kMidlinePauseToken, pos);
        const std::string_view clause =
            (hit == std::string::npos)
                ? std::string_view(sanitized).substr(pos)
                : std::string_view(sanitized).substr(pos, hit - pos);
        const std::string wrapped_clause = WrapBackslashDelimitedClause(clause, max_columns);
        if (!first) {
            out.append(kMidlinePauseToken.data(), kMidlinePauseToken.size());
        }
        out += wrapped_clause;
        first = false;
        if (hit == std::string::npos) {
            break;
        }
        pos = hit + kMidlinePauseToken.size();
    }
    return out;
}

struct TsvBuilder::Impl {
    std::size_t max_wrap_columns = kDefaultWrapColumns;
    translate::SpeakerGate speaker_gate;

    explicit Impl(std::size_t wrap_cols) : max_wrap_columns(wrap_cols) {}

    TsvBuildSummary BuildImpl(const std::string& translated_file,
                              const std::string& cache_file,
                              const std::string& out_tsv) const {
        LogInfo("Loading " + translated_file + "...");
        const bj::value root = JsonParseFile(translated_file);

        translate::Glossary glossary;
        const fs::path tr_path = fs::u8path(translated_file);
        if (tr_path.has_parent_path() && tr_path.parent_path().has_parent_path()) {
            const fs::path tone_candidate =
                tr_path.parent_path().parent_path() / "TONE_OF_NOVEL.md";
            glossary.LoadFromToneFile(tone_candidate.u8string());
        }

        std::unordered_map<std::string, std::string> cache;
        std::error_code ec;
        if (!cache_file.empty() && fs::is_regular_file(fs::u8path(cache_file), ec)) {
            const bj::value cache_val = JsonParseFile(cache_file);
            if (cache_val.is_object()) {
                for (const auto& kv : cache_val.as_object()) {
                    if (!kv.value().is_string()) {
                        continue;
                    }
                    const std::string raw_k(kv.key());
                    const std::string raw_v(kv.value().as_string());
                    cache.emplace(raw_k, raw_v);
                    const std::string norm_k = NormalizeTsvKey(raw_k);
                    if (!norm_k.empty()) {
                        cache.emplace(norm_k, raw_v);
                    }
                }
            }
            LogInfo("  Cache: " + FormatWithCommas(static_cast<long long>(cache.size())) +
                    " indexed keys");
        }

        std::vector<std::pair<std::string, std::string>> order;
        std::unordered_map<std::string, std::size_t> index;
        TsvBuildSummary summary;

        auto insert_row = [&](const std::string& key,
                              const std::string& val,
                              bool is_speaker) {
            if (key.empty() || val.empty() || key == val) {
                return;
            }
            const auto it = index.find(key);
            if (it == index.end()) {
                index.emplace(key, order.size());
                order.emplace_back(key, val);
                if (is_speaker) {
                    ++summary.speaker_entry_count;
                }
                return;
            }
            if (order[it->second].second != val && !is_speaker) {
                ++summary.conflict_count;
            }
        };

        for (const auto& [jp_spk, en_spk] : glossary.OrderedSpeakerTranslations()) {
            const std::string spk_key = NormalizeTsvKey(jp_spk);
            if (!spk_key.empty()) {
                insert_row(spk_key, SanitizeTsvValue(en_spk), true);
            }
        }

        if (root.is_object()) {
            const bj::object* scripts_container = &root.as_object();
            if (const auto* scripts_sub = scripts_container->if_contains("scripts");
                scripts_sub && scripts_sub->is_object()) {
                scripts_container = &scripts_sub->as_object();
            }

            for (const auto& file_kv : *scripts_container) {
                const bj::array* entries_arr = nullptr;
                if (file_kv.value().is_array()) {
                    entries_arr = &file_kv.value().as_array();
                } else if (file_kv.value().is_object()) {
                    const auto& file_obj = file_kv.value().as_object();
                    if (const auto* s_it = file_obj.if_contains("strings");
                        s_it && s_it->is_array()) {
                        entries_arr = &s_it->as_array();
                    } else if (const auto* l_it = file_obj.if_contains("lines");
                               l_it && l_it->is_array()) {
                        entries_arr = &l_it->as_array();
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

                    if (const auto* spk_v = entry.if_contains("speaker");
                        spk_v && spk_v->is_string()) {
                        const std::string spk_jp(spk_v->as_string());
                        const std::string spk_key = NormalizeTsvKey(spk_jp);
                        if (!spk_key.empty()) {
                            if (const auto spk_en = glossary.LookupSpeaker(spk_key)) {
                                insert_row(spk_key, SanitizeTsvValue(*spk_en), true);
                            } else if (const auto cit = cache.find(spk_key);
                                       cit != cache.end()) {
                                insert_row(spk_key, SanitizeTsvValue(cit->second), true);
                            }
                        }
                    }

                    std::string jp;
                    if (const auto* txt = entry.if_contains("text");
                        txt && txt->is_string()) {
                        jp = std::string(txt->as_string());
                    } else if (const auto* cnt = entry.if_contains("content");
                               cnt && cnt->is_string()) {
                        jp = std::string(cnt->as_string());
                    } else if (const auto* jp_f = entry.if_contains("jp");
                               jp_f && jp_f->is_string()) {
                        jp = std::string(jp_f->as_string());
                    }
                    if (jp.empty()) {
                        continue;
                    }

                    const std::string jp_key = NormalizeTsvKey(jp);
                    if (jp_key.empty()) {
                        continue;
                    }

                    std::string en;
                    if (const auto* tr = entry.if_contains("translated");
                        tr && tr->is_string()) {
                        en = std::string(tr->as_string());
                    } else if (const auto* tr2 = entry.if_contains("translation");
                               tr2 && tr2->is_string()) {
                        en = std::string(tr2->as_string());
                    } else if (const auto* en_f = entry.if_contains("en");
                               en_f && en_f->is_string()) {
                        en = std::string(en_f->as_string());
                    }

                    const std::string stripped_jp = StripInlineControlWrappers(jp);
                    if (en.empty() || en == jp || NormalizeTsvKey(en) == jp_key) {
                        auto cit = cache.find(jp);
                        if (cit == cache.end() || cit->second == jp) {
                            cit = cache.find(stripped_jp);
                        }
                        if (cit == cache.end() || cit->second == stripped_jp) {
                            cit = cache.find(jp_key);
                        }
                        if (cit != cache.end() &&
                            cit->second != jp &&
                            cit->second != stripped_jp &&
                            cit->second != jp_key) {
                            en = cit->second;
                            ++summary.cache_fill_count;
                        }
                    }

                    if (en.empty() || en == jp || NormalizeTsvKey(en) == jp_key) {
                        continue;
                    }

                    std::string entry_type;
                    if (const auto* tp = entry.if_contains("type");
                        tp && tp->is_string()) {
                        entry_type = std::string(tp->as_string());
                    }

                    const std::string preserved_en =
                        speaker_gate.PreserveInlineControls(jp, en);
                    const std::string final_en =
                        ShouldWrapEntryType(entry_type)
                            ? WrapEnglishLine(preserved_en, max_wrap_columns)
                            : SanitizeTsvValue(preserved_en);

                    if (final_en.empty() || final_en == jp_key) {
                        continue;
                    }

                    insert_row(jp_key, final_en, false);
                }
            }
        }

        summary.entry_count = order.size();

        std::string tsv_buffer;
        for (const auto& [jp_col, en_col] : order) {
            tsv_buffer += jp_col;
            tsv_buffer += '\t';
            tsv_buffer += en_col;
            tsv_buffer += '\n';
        }
        WriteFileBytes(out_tsv, tsv_buffer);

        LogInfo("Generated " + out_tsv);
        LogInfo("  Entries:    " + FormatWithCommas(static_cast<long long>(summary.entry_count)) +
                " (" + FormatWithCommas(static_cast<long long>(summary.speaker_entry_count)) +
                " speakers)");
        LogInfo("  Cache-fill: " + FormatWithCommas(static_cast<long long>(summary.cache_fill_count)));
        LogInfo("  Conflicts:  " + FormatWithCommas(static_cast<long long>(summary.conflict_count)));
        LogInfo("  File size:  " +
                FormatWithCommas(static_cast<long long>(fs::file_size(fs::u8path(out_tsv)))) +
                " bytes");
        return summary;
    }
};

TsvBuilder::TsvBuilder() : impl_(std::make_unique<Impl>(kDefaultWrapColumns)) {}

TsvBuilder::TsvBuilder(std::size_t max_wrap_columns)
    : impl_(std::make_unique<Impl>(max_wrap_columns)) {}

TsvBuilder::TsvBuilder(TsvBuilder&& other) noexcept = default;
TsvBuilder& TsvBuilder::operator=(TsvBuilder&& other) noexcept = default;
TsvBuilder::~TsvBuilder() = default;

TsvBuildSummary TsvBuilder::Build(const std::string& translated_file,
                                  const std::string& cache_file,
                                  const std::string& out_tsv) const {
    return impl_->BuildImpl(translated_file, cache_file, out_tsv);
}

int TsvBuilder::Run(const std::string& translated_file,
                    const std::string& cache_file,
                    const std::string& out_tsv) const {
    impl_->BuildImpl(translated_file, cache_file, out_tsv);
    return kExitOk;
}

int RunBuild(const std::string& translated_file,
             const std::string& cache_file,
             const std::string& out_tsv) {
    const TsvBuilder builder;
    return builder.Run(translated_file, cache_file, out_tsv);
}

}  // namespace oretsuba::build_tsv
