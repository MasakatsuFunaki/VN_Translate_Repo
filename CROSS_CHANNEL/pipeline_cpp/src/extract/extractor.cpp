// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extractor.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <utility>

#include <boost/json.hpp>

#include "willplus/sn_archive.h"

namespace crc::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

bool is_bonus_density_char(char32_t cp) {
    // …―～　！？ -- punctuation dense enough in JP prose to count toward the
    // density gate.
    return cp == 0x2026 || cp == 0x2015 || cp == 0xFF5E || cp == 0x3000 || cp == 0xFF01 ||
           cp == 0xFF1F;
}

std::string fmt1(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

}  // namespace

bool is_real_japanese(char32_t cp) {
    return (cp >= 0x3040 && cp <= 0x309F) ||  // Hiragana
           (cp >= 0x30A0 && cp <= 0x30FF) ||  // Full-width katakana
           (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK Unified Ideographs
           (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK Extension A
           (cp >= 0x3000 && cp <= 0x303F);    // CJK Symbols (。、「」 etc.)
}

bool has_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size())
        if (is_real_japanese(utf8_next(utf8, i))) return true;
    return false;
}

int count_japanese(const std::string& utf8) {
    int n = 0;
    std::size_t i = 0;
    while (i < utf8.size())
        if (is_real_japanese(utf8_next(utf8, i))) ++n;
    return n;
}

double japanese_density(const std::string& utf8) {
    if (utf8.empty()) return 0.0;
    int jp = 0;
    std::size_t total = 0, i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        ++total;
        if (is_real_japanese(cp)) ++jp;
        if (is_bonus_density_char(cp)) ++jp;  // U+3000 lands in both -- counted twice
    }
    return static_cast<double>(jp) / static_cast<double>(total);
}

std::unordered_set<std::size_t> find_speaker_offsets(const Bytes& data) {
    static const std::array<std::uint8_t, 3> MARKER = {0x47, 0x0D, 0x00};
    std::unordered_set<std::size_t> offsets;
    std::size_t pos = 0;
    for (;;) {
        auto it = std::search(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end(),
                              MARKER.begin(), MARKER.end());
        if (it == data.end()) break;
        const std::size_t idx = static_cast<std::size_t>(it - data.begin());
        const std::size_t name_start = idx + 3;
        auto nul = std::find(data.begin() + static_cast<std::ptrdiff_t>(
                                                std::min(name_start, data.size())),
                             data.end(), std::uint8_t{0x00});
        if (nul != data.end()) {
            const std::size_t null_end = static_cast<std::size_t>(nul - data.begin());
            const std::size_t len = null_end - name_start;
            if (null_end > 0 && len > 0 && len <= 30) offsets.insert(name_start);
        }
        // Advance by 3, not past the terminator: consecutive markers overlap.
        pos = idx + 3;
        if (pos >= data.size()) break;
    }
    return offsets;
}

std::string strip_control_prefix(const std::string& utf8) {
    std::size_t first_kept_cp = 0;
    std::size_t first_kept_byte = std::string::npos;
    {
        std::size_t i = 0, n = 0;
        while (i < utf8.size()) {
            const std::size_t start = i;
            const char32_t cp = utf8_next(utf8, i);
            if (cp == 0x300C || cp == 0x300D || is_real_japanese(cp) || cp == 0x2026 ||
                cp == 0x2015 || cp == 0xFF5E) {
                first_kept_cp = n;
                first_kept_byte = start;
                break;
            }
            ++n;
        }
    }
    if (first_kept_byte == std::string::npos || first_kept_cp == 0) return utf8;

    // Only a prefix that actually carries a control byte is bytecode garbage;
    // printable leaders are content (CLAUDE.md §6).
    std::size_t i = 0;
    while (i < first_kept_byte)
        if (utf8_next(utf8, i) < 0x20) return utf8.substr(first_kept_byte);
    return utf8;
}

bool is_choice_option_at(std::size_t offset, const Bytes& data) {
    if (offset < 6) return false;
    if (offset > data.size()) return false;
    return data[offset - 6] == 0xFF && data[offset - 5] == 0xFF && data[offset - 2] == 0x00 &&
           data[offset - 1] == 0x00;
}

std::vector<RawString> extract_strings(const Bytes& data,
                                       const std::unordered_set<std::size_t>& speaker_offsets) {
    std::vector<RawString> strings;
    const std::size_t fsize = data.size();
    std::size_t pos = 0;

    while (pos < fsize) {
        if (data[pos] == 0x00) {
            ++pos;
            continue;
        }
        auto nul = std::find(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end(),
                             std::uint8_t{0x00});
        if (nul == data.end()) break;
        const std::size_t end = static_cast<std::size_t>(nul - data.begin());

        auto decoded = cp932_to_utf8_strict(data.data() + pos, end - pos);
        if (!decoded) {
            // CLAUDE.md §9: the chunk starts with opcode operand bytes that are
            // not valid CP932 but may still hold real text further in.  Sliding
            // one byte lets the walker find the next valid lead; jumping to
            // end + 1 silently dropped the dialogue inside.
            ++pos;
            continue;
        }

        if (speaker_offsets.count(pos) != 0) {
            strings.push_back(RawString{pos, *decoded, true});
        } else if (end - pos >= 2) {  // BYTE length of the chunk, not codepoints
            std::string cleaned = strip_control_prefix(*decoded);
            if (count_japanese(cleaned) >= 2 && japanese_density(cleaned) >= 0.3) {
                bool has_ctrl = false;
                std::size_t i = 0;
                while (i < cleaned.size()) {
                    const char32_t cp = utf8_next(cleaned, i);
                    if (cp < 0x20 && cp != '\n' && cp != '\r') {
                        has_ctrl = true;
                        break;
                    }
                }
                // The recorded offset is the ORIGINAL chunk start, not the
                // post-strip position -- it keys the runtime hook.
                if (!has_ctrl) strings.push_back(RawString{pos, std::move(cleaned), false});
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
        // narration, else drop.
        if (cur.is_speaker) {
            if (i + 1 < strings.size() &&
                strings[i + 1].text.find("\xE3\x80\x8C") != std::string::npos) {  // 「
                // The paired dialogue keeps the RAW next text; only the speaker
                // name is stripped.
                entries.push_back(
                    Entry{strings[i + 1].offset, "dialogue", text_stripped, strings[i + 1].text});
                i += 2;
                continue;
            }
            ++i;  // speaker with no dialogue -- the name itself is dropped
        } else if (text_stripped.find("\xE3\x80\x8C") != std::string::npos &&
                   text_stripped.size() >= 3 &&
                   text_stripped.compare(text_stripped.size() - 3, 3, "\xE3\x80\x8D") == 0) {
            entries.push_back(Entry{cur.offset, "dialogue", "", text_stripped});
            ++i;
        } else if (has_japanese(text_stripped) &&
                   (count_japanese(text_stripped) >= 3 ||
                    (count_japanese(text_stripped) >= 2 &&
                     is_choice_option_at(cur.offset, data)))) {
            entries.push_back(Entry{cur.offset, "narration", "", text_stripped});
            ++i;
        } else {
            ++i;
        }
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
    if (start_idx > 0)
        std::rotate(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(start_idx),
                    entries.end());
    return start_idx;
}

int run_extract(const std::string& sn_bin_path, const std::string& output_file) {
    log_info("Reading: " + sn_bin_path);

    if (!fs::exists(fs::u8path(sn_bin_path))) {
        log_info("ERROR: sn.bin not found at " + sn_bin_path);
        log_info("Check GAME_DIR path.");
        // Deliberately exit 0: a missing archive is reported to the operator,
        // not signalled as a run failure.
        return 0;
    }

    const Bytes compressed = read_file(sn_bin_path);
    log_info("  sn.bin size: " + comma(static_cast<long long>(compressed.size())) + " bytes (" +
             fmt1(static_cast<double>(compressed.size()) / 1024.0 / 1024.0) + " MB)");

    const std::uint32_t expected = willplus::sn_expected_size(compressed);
    log_info("  Expected decompressed size: " + comma(expected) + " bytes");

    log_info("  Decompressing (LZSS)...");
    const Bytes data = willplus::lzss_decompress(compressed);
    const long long diff =
        static_cast<long long>(data.size()) - static_cast<long long>(expected);
    log_info("  Decompressed: " + comma(static_cast<long long>(data.size())) + " bytes (expected " +
             comma(expected) + ", diff " + std::to_string(diff) + ")");
    if (diff > 100 || diff < -100) log_info("  WARNING: Decompressed size doesn't match header!");

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
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%X", static_cast<unsigned>(GAME_START_OFFSET));
        log_info("  Rotating entries: game opens at binary offset 0x" + std::string(hex) +
                 " (entry idx " + std::to_string(start_idx) + ")");
    }

    long long dialogue_count = 0, narration_count = 0;
    for (const auto& e : entries) {
        if (e.type == "dialogue") ++dialogue_count;
        if (e.type == "narration") ++narration_count;
    }

    // R5: insertion-ordered counter -- the top-15 print is a STABLE sort by
    // descending count, so ties must keep first-appearance order.
    std::vector<std::pair<std::string, long long>> speakers;
    std::map<std::string, std::size_t> speaker_index;
    for (const auto& e : entries) {
        if (e.speaker.empty()) continue;
        auto it = speaker_index.find(e.speaker);
        if (it == speaker_index.end()) {
            speaker_index.emplace(e.speaker, speakers.size());
            speakers.emplace_back(e.speaker, 1);
        } else {
            ++speakers[it->second].second;
        }
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

    write_file_text(output_file, json_pretty(bj::value(std::move(all_data)), 1));

    // Stat the file AFTER the text-mode write: the reported size is the
    // CRLF-expanded on-disk size, not the length of the serialized JSON.
    const double size_mb =
        static_cast<double>(fs::file_size(fs::u8path(output_file))) / 1024.0 / 1024.0;

    log_info("\nExtraction complete:");
    log_info("  Total entries: " + comma(static_cast<long long>(entries.size())));
    log_info("  Dialogue lines: " + comma(dialogue_count));
    log_info("  Narration lines: " + comma(narration_count));
    log_info("  Unique speakers: " + std::to_string(speakers.size()));

    std::vector<std::pair<std::string, long long>> top = speakers;
    std::stable_sort(top.begin(), top.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < top.size() && i < 15; ++i)
        log_info("    " + top[i].first + ": " + std::to_string(top[i].second));

    log_info("  Output: " + output_file + " (" + fmt1(size_mb) + " MB)");
    return 0;
}

}  // namespace crc::extract
