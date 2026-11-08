// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "extract/extractor.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"
#include "lpk/lpk_archive.h"
#include "sob/sob_parser.h"

namespace oretsuba::extract {

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr const char* kScriptArchiveName = "SCRIPT.LPK";
constexpr std::string_view kSobExtension = ".sob";
constexpr std::size_t kScenarioPrefixMinLen = 3U;
constexpr std::string_view kUtf8ReplacementSeq = "\xEF\xBF\xBD";
constexpr double kBytesPerKilobyte = 1024.0;
constexpr double kBytesPerMegabyte = kBytesPerKilobyte * kBytesPerKilobyte;
constexpr std::size_t kFileSizeFormatBufSize = 64U;
constexpr int kExitOk = 0;

char ToLowerAscii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

std::string NormalizeLowerAscii(std::string_view input) {
    std::string out(input.size(), '\0');
    for (std::size_t i = 0; i < input.size(); ++i) {
        out[i] = ToLowerAscii(input[i]);
    }
    return out;
}

bool EndsWithSobExtension(std::string_view name) {
    if (name.size() < kSobExtension.size()) {
        return false;
    }
    const std::string lower = NormalizeLowerAscii(
        name.substr(name.size() - kSobExtension.size()));
    return lower == kSobExtension;
}

bool IsScenarioScriptName(std::string_view name) {
    if (name.size() < kScenarioPrefixMinLen) {
        return false;
    }
    const char first = ToLowerAscii(name[0]);
    const char second = name[1];
    return first == 's' && second >= '0' && second <= '9';
}

std::size_t CountReplacementChars(std::string_view utf8) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = utf8.find(kUtf8ReplacementSeq, pos)) != std::string_view::npos) {
        ++count;
        pos += kUtf8ReplacementSeq.size();
    }
    return count;
}

bj::object RecordToJson(const sob::SobStringRecord& rec) {
    bj::object obj;
    obj["offset"] = static_cast<std::int64_t>(rec.offset);
    obj["code_offset"] = static_cast<std::int64_t>(rec.code_offset);
    obj["type"] = std::string(sob::ToEntryTypeString(rec.type));
    obj["text"] = rec.text;
    obj["raw"] = rec.raw;
    obj["has_jp"] = rec.has_jp;

    if (rec.type == sob::SobEntryType::kDialogue) {
        obj["block_id"] = static_cast<std::int64_t>(rec.block_id);
        obj["msg_index"] = static_cast<std::int64_t>(rec.record_index);
        obj["speaker"] = rec.speaker;
        obj["voice_id"] = rec.voice_id;
        return obj;
    }

    if (rec.type == sob::SobEntryType::kNarration) {
        obj["block_id"] = static_cast<std::int64_t>(rec.block_id);
        obj["msg_index"] = static_cast<std::int64_t>(rec.record_index);
        return obj;
    }

    if (rec.type == sob::SobEntryType::kChoice) {
        obj["block_id"] = static_cast<std::int64_t>(rec.block_id);
        obj["choice_id"] = static_cast<std::int64_t>(rec.record_index);
        obj["option_index"] = static_cast<std::int64_t>(rec.option_index);
        return obj;
    }

    return obj;
}

bj::object ScriptToJson(const sob::SobParsedScript& parsed) {
    bj::array strings;
    strings.reserve(parsed.records.size());
    for (const auto& rec : parsed.records) {
        strings.push_back(RecordToJson(rec));
    }

    bj::object script;
    script["file"] = parsed.script_name;
    script["arc"] = kScriptArchiveName;
    script["magic"] = std::string(sob::ToFormatString(parsed.format));
    script["file_size"] = static_cast<std::int64_t>(parsed.file_size);
    script["code_size"] = static_cast<std::int64_t>(parsed.code_size);
    script["total_size"] = static_cast<std::int64_t>(parsed.total_size);
    script["entry_count"] = static_cast<std::int64_t>(parsed.records.size());
    script["strings"] = std::move(strings);
    return script;
}

}  // namespace

struct Extractor::Impl {
    sob::SobScriptParser parser;

    ExtractionSummary ExtractArchiveImpl(const std::string& lpk_path,
                                         const std::string& output_json_path) const {
        LogInfo("Opening " + lpk_path + "...");
        const lpk::LpkArchiveReader reader(lpk_path);
        const auto& entries = reader.Entries();

        ExtractionSummary summary;
        summary.total_archive_entries = entries.size();

        std::vector<std::size_t> sob_indices;
        sob_indices.reserve(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (EndsWithSobExtension(entries[i].name)) {
                sob_indices.push_back(i);
            }
        }
        summary.sob_file_count = sob_indices.size();

        std::sort(sob_indices.begin(), sob_indices.end(),
                  [&](std::size_t lhs, std::size_t rhs) {
                      const std::string lhs_lower = NormalizeLowerAscii(entries[lhs].name);
                      const std::string rhs_lower = NormalizeLowerAscii(entries[rhs].name);
                      const bool lhs_scen = IsScenarioScriptName(lhs_lower);
                      const bool rhs_scen = IsScenarioScriptName(rhs_lower);
                      if (lhs_scen != rhs_scen) {
                          return lhs_scen;
                      }
                      return lhs_lower < rhs_lower;
                  });

        std::unordered_set<std::string> unique_speakers;
        std::unordered_set<std::string> unique_jp_strings;
        bj::object root;

        for (std::size_t entry_idx : sob_indices) {
            const std::string& script_name = entries[entry_idx].name;
            const Bytes payload = reader.ReadEntry(entry_idx);
            const sob::SobParsedScript parsed = parser.Parse(script_name, payload);

            if (parsed.format == sob::SobFormat::kSob0) {
                ++summary.sob0_count;
            }
            if (parsed.format == sob::SobFormat::kSob1) {
                ++summary.sob1_count;
            }
            if (parsed.format == sob::SobFormat::kSob2) {
                ++summary.sob2_count;
            }
            if (parsed.has_messages) {
                ++summary.scenario_script_count;
            }
            summary.choice_block_count += parsed.choice_block_count;

            for (const auto& rec : parsed.records) {
                ++summary.total_entry_count;
                summary.replacement_char_count += CountReplacementChars(rec.text);
                summary.replacement_char_count += CountReplacementChars(rec.speaker);

                if (!rec.text.empty()) {
                    unique_jp_strings.insert(rec.text);
                }
                if (!rec.speaker.empty()) {
                    unique_speakers.insert(rec.speaker);
                    unique_jp_strings.insert(rec.speaker);
                }

                if (rec.type == sob::SobEntryType::kDialogue) {
                    ++summary.message_count;
                    if (!rec.speaker.empty() && !rec.voice_id.empty()) {
                        ++summary.voiced_dialogue_count;
                        continue;
                    }
                    if (!rec.speaker.empty() && rec.voice_id.empty()) {
                        ++summary.unvoiced_dialogue_count;
                        continue;
                    }
                    ++summary.voice_only_dialogue_count;
                    continue;
                }

                if (rec.type == sob::SobEntryType::kNarration) {
                    ++summary.message_count;
                    ++summary.narration_count;
                    continue;
                }

                if (rec.type == sob::SobEntryType::kChoice) {
                    ++summary.choice_option_count;
                    continue;
                }

                if (rec.type == sob::SobEntryType::kTitle) {
                    ++summary.title_count;
                    continue;
                }

                if (rec.type == sob::SobEntryType::kSynopsis) {
                    ++summary.synopsis_count;
                    continue;
                }

                if (rec.type == sob::SobEntryType::kUi) {
                    ++summary.ui_string_count;
                }
            }

            if (!parsed.records.empty()) {
                root[script_name] = ScriptToJson(parsed);
            }
        }

        summary.unique_speaker_count = unique_speakers.size();
        summary.unique_jp_string_count = unique_jp_strings.size();

        WriteFileBytes(output_json_path, JsonDump(bj::value(std::move(root))));

        char size_buf[kFileSizeFormatBufSize];
        const double megabytes =
            static_cast<double>(fs::file_size(fs::u8path(output_json_path))) / kBytesPerMegabyte;
        std::snprintf(size_buf, sizeof(size_buf), "%.1f MB", megabytes);

        LogInfo("Archive entries: " + FormatWithCommas(static_cast<long long>(summary.total_archive_entries)) +
                " (" + FormatWithCommas(static_cast<long long>(summary.sob_file_count)) + " .sob)");
        LogInfo("Scenario scripts with messages: " +
                FormatWithCommas(static_cast<long long>(summary.scenario_script_count)));
        LogInfo("Messages (SYSCALL[462]): " +
                FormatWithCommas(static_cast<long long>(summary.message_count)));
        LogInfo("Choices (SYSCALL[491]): " +
                FormatWithCommas(static_cast<long long>(summary.choice_option_count)) +
                " across " + FormatWithCommas(static_cast<long long>(summary.choice_block_count)) +
                " blocks");
        LogInfo("Scene titles: " + FormatWithCommas(static_cast<long long>(summary.title_count)) +
                ", synopses: " + FormatWithCommas(static_cast<long long>(summary.synopsis_count)) +
                ", UI strings: " + FormatWithCommas(static_cast<long long>(summary.ui_string_count)));
        LogInfo("Total extracted entries: " +
                FormatWithCommas(static_cast<long long>(summary.total_entry_count)) +
                " (unique JP strings: " +
                FormatWithCommas(static_cast<long long>(summary.unique_jp_string_count)) +
                ", unique speakers: " +
                FormatWithCommas(static_cast<long long>(summary.unique_speaker_count)) + ")");
        LogInfo("Saved " + output_json_path + " (" + size_buf + ")");

        return summary;
    }
};

Extractor::Extractor() : impl_(std::make_unique<Impl>()) {}
Extractor::Extractor(Extractor&& other) noexcept = default;
Extractor& Extractor::operator=(Extractor&& other) noexcept = default;
Extractor::~Extractor() = default;

ExtractionSummary Extractor::ExtractArchive(const std::string& lpk_path,
                                            const std::string& output_json_path) const {
    return impl_->ExtractArchiveImpl(lpk_path, output_json_path);
}

int Extractor::Run(const std::string& game_dir,
                   const std::string& output_json_path) const {
    const fs::path lpk_fs = fs::u8path(game_dir) / kScriptArchiveName;
    if (!fs::is_regular_file(lpk_fs)) {
        throw std::runtime_error("SCRIPT.LPK not found in game directory: " + lpk_fs.u8string());
    }
    impl_->ExtractArchiveImpl(lpk_fs.u8string(), output_json_path);
    return kExitOk;
}

}  // namespace oretsuba::extract
