// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extract/extractor.h"

#include "willplus/sn_archive.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string_view>
#include <utility>

namespace crc::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kFormatBufSize = 64;
constexpr std::size_t kHexBufSize = 32;
constexpr char32_t kHiraganaFirst = 0x3040;
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;
constexpr char32_t kCjkExtAFirst = 0x3400;
constexpr char32_t kCjkExtALast = 0x4DBF;
constexpr char32_t kCjkSymbolsFirst = 0x3000;
constexpr char32_t kCjkSymbolsLast = 0x303F;

constexpr std::array<std::uint8_t, 3> kSpeakerOpcodeMarker = {0x47, 0x0D, 0x00};
constexpr std::size_t kMaxSpeakerByteLength = 30;
constexpr char32_t kControlByteLimit = 0x20;
constexpr std::size_t kChoicePreambleBytes = 6;
constexpr std::size_t kChoiceMarkerLeadOffset = 6;
constexpr std::size_t kChoiceMarkerSecondOffset = 5;
constexpr std::size_t kChoiceTrailingNullFirstOffset = 2;
constexpr std::size_t kChoiceTrailingNullSecondOffset = 1;
constexpr std::uint8_t kChoiceLeadByte = 0xFF;
constexpr std::uint8_t kNullByte = 0x00;

constexpr std::size_t kMinRawChunkBytes = 2;
constexpr int kMinChunkJpChars = 2;
constexpr double kMinJapaneseDensity = 0.3;
constexpr std::size_t kPairedSpeakerAdvance = 2;
constexpr int kMinNarrationJpChars = 3;
constexpr int kMinChoiceJpChars = 2;
constexpr long long kMaxSizeToleranceBytes = 100;
constexpr int kExtractedJsonIndent = 1;
constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;
constexpr std::size_t kMaxTopSpeakersPrinted = 15;
constexpr int kExtractSuccessExitCode = 0;

constexpr std::string_view kOpenCornerQuote = "「";
constexpr std::string_view kCloseCornerQuote = "」";

bool is_bonus_density_char(char32_t cp) {
    // …―～　！？ -- punctuation dense enough in JP prose to count toward the
    // density gate.
    return cp == U'…' || cp == U'―' || cp == U'～' || cp == U'　' || cp == U'！' ||
           cp == U'？';
}

std::string fmt1(double v) {
    char buf[kFormatBufSize];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

}  // namespace

bool is_real_japanese(char32_t cp) {
    return (cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
           (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
           (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast) ||
           (cp >= kCjkExtAFirst && cp <= kCjkExtALast) ||
           (cp >= kCjkSymbolsFirst && cp <= kCjkSymbolsLast);
}

bool has_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        if (is_real_japanese(utf8_next(utf8, i))) {
            return true;
        }
    }
    return false;
}

int count_japanese(const std::string& utf8) {
    int n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        if (is_real_japanese(utf8_next(utf8, i))) {
            ++n;
        }
    }
    return n;
}

double japanese_density(const std::string& utf8) {
    if (utf8.empty()) {
        return 0.0;
    }
    int jp = 0;
    std::size_t total = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        ++total;
        if (is_real_japanese(cp)) {
            ++jp;
        }
        if (is_bonus_density_char(cp)) {
            ++jp;  // U+3000 lands in both predicates and counts twice.
        }
    }
    return static_cast<double>(jp) / static_cast<double>(total);
}

std::unordered_set<std::size_t> find_speaker_offsets(const Bytes& data) {
    std::unordered_set<std::size_t> offsets;
    std::size_t pos = 0;
    for (;;) {
        const auto it = std::search(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end(),
                                    kSpeakerOpcodeMarker.begin(), kSpeakerOpcodeMarker.end());
        if (it == data.end()) {
            break;
        }
        const std::size_t idx = static_cast<std::size_t>(it - data.begin());
        const std::size_t name_start = idx + kSpeakerOpcodeMarker.size();
        const auto nul = std::find(
            data.begin() + static_cast<std::ptrdiff_t>(std::min(name_start, data.size())),
            data.end(), kNullByte);
        if (nul != data.end()) {
            const std::size_t null_end = static_cast<std::size_t>(nul - data.begin());
            const std::size_t len = null_end - name_start;
            if (null_end > 0 && len > 0 && len <= kMaxSpeakerByteLength) {
                offsets.insert(name_start);
            }
        }
        // Advance by marker length rather than past the terminator so overlapping markers match.
        pos = idx + kSpeakerOpcodeMarker.size();
        if (pos >= data.size()) {
            break;
        }
    }
    return offsets;
}

std::string strip_control_prefix(const std::string& utf8) {
    std::size_t first_kept_cp = 0;
    std::size_t first_kept_byte = std::string::npos;
    {
        std::size_t i = 0;
        std::size_t n = 0;
        while (i < utf8.size()) {
            const std::size_t start = i;
            const char32_t cp = utf8_next(utf8, i);
            if (cp == U'「' || cp == U'」' || is_real_japanese(cp) || cp == U'…' ||
                cp == U'―' || cp == U'～') {
                first_kept_cp = n;
                first_kept_byte = start;
                break;
            }
            ++n;
        }
    }
    if (first_kept_byte == std::string::npos || first_kept_cp == 0) {
        return utf8;
    }

    // Only a prefix carrying an actual C0 control byte is bytecode garbage;
    // printable leaders such as ※ or Ａ are real script content.
    std::size_t i = 0;
    while (i < first_kept_byte) {
        if (utf8_next(utf8, i) < kControlByteLimit) {
            return utf8.substr(first_kept_byte);
        }
    }
    return utf8;
}

bool is_choice_option_at(std::size_t offset, const Bytes& data) {
    if (offset < kChoicePreambleBytes || offset > data.size()) {
        return false;
    }
    return data[offset - kChoiceMarkerLeadOffset] == kChoiceLeadByte &&
           data[offset - kChoiceMarkerSecondOffset] == kChoiceLeadByte &&
           data[offset - kChoiceTrailingNullFirstOffset] == kNullByte &&
           data[offset - kChoiceTrailingNullSecondOffset] == kNullByte;
}

std::vector<RawString> extract_strings(const Bytes& data,
                                       const std::unordered_set<std::size_t>& speaker_offsets) {
    std::vector<RawString> strings;
    const std::size_t fsize = data.size();
    std::size_t pos = 0;

    while (pos < fsize) {
        if (data[pos] == kNullByte) {
            ++pos;
            continue;
        }
        const auto nul = std::find(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end(),
                                   kNullByte);
        if (nul == data.end()) {
            break;
        }
        const std::size_t end = static_cast<std::size_t>(nul - data.begin());

        const auto decoded = cp932_to_utf8_strict(data.data() + pos, end - pos);
        if (!decoded) {
            // Slide one byte when leading opcode operands are not valid CP932
            // so real Japanese dialogue later in the same chunk is preserved.
            ++pos;
            continue;
        }

        if (speaker_offsets.count(pos) != 0) {
            strings.push_back(RawString{pos, *decoded, true});
            pos = end + 1;
            continue;
        }

        if (end - pos >= kMinRawChunkBytes) {
            std::string cleaned = strip_control_prefix(*decoded);
            if (count_japanese(cleaned) >= kMinChunkJpChars &&
                japanese_density(cleaned) >= kMinJapaneseDensity) {
                bool has_ctrl = false;
                std::size_t i = 0;
                while (i < cleaned.size()) {
                    const char32_t cp = utf8_next(cleaned, i);
                    if (cp < kControlByteLimit && cp != '\n' && cp != '\r') {
                        has_ctrl = true;
                        break;
                    }
                }
                if (!has_ctrl) {
                    strings.push_back(RawString{pos, std::move(cleaned), false});
                }
            }
        }
        pos = end + 1;
    }
    return strings;
}

std::vector<Entry> classify_and_pair(const std::vector<RawString>& strings, const Bytes& data) {
    std::vector<Entry> entries;
    std::size_t i = 0;
    while (i < strings.size()) {
        const RawString& cur = strings[i];
        const std::string text_stripped = trim(cur.text);
        if (text_stripped.empty()) {
            ++i;
            continue;
        }

        // Evaluation order is load-bearing: speaker, then 「…」 dialogue, then
        // narration, otherwise drop.
        if (cur.is_speaker) {
            if (i + 1 < strings.size() &&
                strings[i + 1].text.find(kOpenCornerQuote) != std::string::npos) {
                entries.push_back(
                    Entry{strings[i + 1].offset, "dialogue", text_stripped, strings[i + 1].text});
                i += kPairedSpeakerAdvance;
                continue;
            }
            ++i;
            continue;
        }

        if (text_stripped.find(kOpenCornerQuote) != std::string::npos &&
            text_stripped.size() >= kCloseCornerQuote.size() &&
            text_stripped.compare(text_stripped.size() - kCloseCornerQuote.size(),
                                  kCloseCornerQuote.size(), kCloseCornerQuote) == 0) {
            entries.push_back(Entry{cur.offset, "dialogue", "", text_stripped});
            ++i;
            continue;
        }

        if (has_japanese(text_stripped) &&
            (count_japanese(text_stripped) >= kMinNarrationJpChars ||
             (count_japanese(text_stripped) >= kMinChoiceJpChars &&
              is_choice_option_at(cur.offset, data)))) {
            entries.push_back(Entry{cur.offset, "narration", "", text_stripped});
        }
        ++i;
    }
    return entries;
}

std::size_t rotate_to_game_start(std::vector<Entry>& entries) {
    std::size_t start_idx = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].offset >= GAME_START_OFFSET) {
            start_idx = i;
            break;
        }
    }
    if (start_idx > 0) {
        std::rotate(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(start_idx),
                    entries.end());
    }
    return start_idx;
}

int run_extract(const std::string& sn_bin_path, const std::string& output_file) {
    log_info("Reading: " + sn_bin_path);

    if (!fs::exists(fs::u8path(sn_bin_path))) {
        log_info("ERROR: sn.bin not found at " + sn_bin_path);
        log_info("Check GAME_DIR path.");
        return kExtractSuccessExitCode;
    }

    const Bytes compressed = read_file(sn_bin_path);
    log_info("  sn.bin size: " + comma(static_cast<long long>(compressed.size())) + " bytes (" +
             fmt1(static_cast<double>(compressed.size()) / kBytesPerMegabyte) + " MB)");

    const std::uint32_t expected = willplus::sn_expected_size(compressed);
    log_info("  Expected decompressed size: " + comma(expected) + " bytes");

    log_info("  Decompressing (LZSS)...");
    const Bytes data = willplus::lzss_decompress(compressed);
    const long long diff =
        static_cast<long long>(data.size()) - static_cast<long long>(expected);
    log_info("  Decompressed: " + comma(static_cast<long long>(data.size())) + " bytes (expected " +
             comma(expected) + ", diff " + std::to_string(diff) + ")");
    if (diff > kMaxSizeToleranceBytes || diff < -kMaxSizeToleranceBytes) {
        log_info("  WARNING: Decompressed size doesn't match header!");
    }

    log_info("\n  Scanning for speaker name opcodes (47 0D 00)...");
    const auto speaker_offsets = find_speaker_offsets(data);
    log_info("  Speaker opcode locations: " + std::to_string(speaker_offsets.size()));

    log_info("  Extracting null-terminated CP932 strings...");
    const auto raw_strings = extract_strings(data, speaker_offsets);
    log_info("  Raw Japanese strings: " + comma(static_cast<long long>(raw_strings.size())));

    log_info("  Classifying and pairing speaker-dialogue...");
    std::vector<Entry> entries = classify_and_pair(raw_strings, data);

    const std::size_t start_idx = rotate_to_game_start(entries);
    if (start_idx > 0) {
        char hex[kHexBufSize];
        std::snprintf(hex, sizeof(hex), "%X", static_cast<unsigned>(GAME_START_OFFSET));
        log_info("  Rotating entries: game opens at binary offset 0x" + std::string(hex) +
                 " (entry idx " + std::to_string(start_idx) + ")");
    }

    long long dialogue_count = 0;
    long long narration_count = 0;
    for (const auto& e : entries) {
        if (e.type == "dialogue") {
            ++dialogue_count;
        }
        if (e.type == "narration") {
            ++narration_count;
        }
    }

    // Insertion-ordered counter so stable descending sort keeps first-appearance ties.
    std::vector<std::pair<std::string, long long>> speakers;
    std::map<std::string, std::size_t> speaker_index;
    for (const auto& e : entries) {
        if (e.speaker.empty()) {
            continue;
        }
        const auto it = speaker_index.find(e.speaker);
        if (it == speaker_index.end()) {
            speaker_index.emplace(e.speaker, speakers.size());
            speakers.emplace_back(e.speaker, 1);
            continue;
        }
        ++speakers[it->second].second;
    }

    bj::array json_entries;
    json_entries.reserve(entries.size());
    for (const auto& e : entries) {
        bj::object o;
        o["offset"] = static_cast<std::int64_t>(e.offset);
        o["type"] = e.type;
        o["speaker"] = e.speaker;
        o["text"] = e.text;
        json_entries.push_back(std::move(o));
    }

    bj::object file_obj;
    file_obj["file"] = "sn.bin";
    file_obj["compressed_size"] = static_cast<std::int64_t>(compressed.size());
    file_obj["decompressed_size"] = static_cast<std::int64_t>(data.size());
    file_obj["total_entries"] = static_cast<std::int64_t>(entries.size());
    file_obj["dialogue_count"] = dialogue_count;
    file_obj["narration_count"] = narration_count;
    file_obj["strings"] = std::move(json_entries);

    bj::object all_data;
    all_data["sn.bin"] = std::move(file_obj);

    write_file_text(output_file,
                    json_pretty(bj::value(std::move(all_data)), kExtractedJsonIndent));

    const double size_mb =
        static_cast<double>(fs::file_size(fs::u8path(output_file))) / kBytesPerMegabyte;

    log_info("\nExtraction complete:");
    log_info("  Total entries: " + comma(static_cast<long long>(entries.size())));
    log_info("  Dialogue lines: " + comma(dialogue_count));
    log_info("  Narration lines: " + comma(narration_count));
    log_info("  Unique speakers: " + std::to_string(speakers.size()));

    std::vector<std::pair<std::string, long long>> top = speakers;
    std::stable_sort(top.begin(), top.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < top.size() && i < kMaxTopSpeakersPrinted; ++i) {
        log_info("    " + top[i].first + ": " + std::to_string(top[i].second));
    }

    log_info("  Output: " + output_file + " (" + fmt1(size_mb) + " MB)");
    return kExtractSuccessExitCode;
}

}  // namespace crc::extract
