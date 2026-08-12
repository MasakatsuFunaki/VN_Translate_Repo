// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extractor.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <boost/json.hpp>

#include "ddp/ddp_archive.h"

namespace shin::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr std::uint8_t MARKER[3] = {0xFF, 0x01, 0x80};

}  // namespace

bool has_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= 0x3040 && cp <= 0x309F) ||   // Hiragana
            (cp >= 0x30A0 && cp <= 0x30FF) ||   // Katakana
            (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK
            (cp >= 0xFF00 && cp <= 0xFFEF))     // Fullwidth
            return true;
    }
    return false;
}

std::vector<Str> extract_strings_from_script(const Bytes& dec) {
    std::vector<Str> strings;
    const auto n = static_cast<std::ptrdiff_t>(dec.size());

    // Signed index: for a script shorter than 4 bytes the body must not run at
    // all, and an unsigned `n - 3` would wrap to a huge bound.
    std::ptrdiff_t i = 0;
    while (i < n - 3) {
        const auto* begin = dec.data() + i;
        const auto* hit = std::search(begin, dec.data() + n, MARKER, MARKER + 3);
        if (hit == dec.data() + n) break;
        const std::ptrdiff_t idx = hit - dec.data();

        const std::uint8_t pre_byte = idx > 0 ? dec[static_cast<std::size_t>(idx - 1)] : 0;

        // Null-terminated UTF-16LE.  Each code unit maps to exactly one
        // codepoint -- see append_utf16_unit_as_utf8.
        std::string text;
        std::ptrdiff_t pos = idx + 3;
        while (pos + 1 < n) {
            const std::uint16_t ch =
                static_cast<std::uint16_t>(dec[static_cast<std::size_t>(pos)] |
                                           (dec[static_cast<std::size_t>(pos + 1)] << 8));
            if (ch == 0) break;
            append_utf16_unit_as_utf8(ch, text);
            pos += 2;
        }

        // Unconditional, and BEFORE every filter below: the walk must advance
        // past this record's terminator whether or not the record is kept.
        i = pos + 2;

        // Filter order is load-bearing: a choice record is emitted and the
        // iteration continues, so it never reaches the 0x36/0x07 gate.
        if (pre_byte == 0x01 && !text.empty() && has_japanese(text)) {
            strings.push_back(Str{static_cast<std::size_t>(idx), "choice", "", text});
            continue;
        }

        if (pre_byte != 0x36 && pre_byte != 0x07) continue;
        if (text.empty() || !has_japanese(text)) continue;

        // "SpeakerName\nDialogueText", where \n is the two ASCII characters
        // 0x5C 0x6E -- not a newline.
        std::string speaker;
        std::string dialogue_text = text;
        if (text.rfind("\\n", 0) != 0) {
            const std::size_t bs = text.find("\\n");
            // The 20-character bound is on CODEPOINTS.  A byte test would reject the
            // 311 dialogue lines whose speaker exceeds 20 bytes (the longest
            // legitimate name, ミスター・マコーリー, is 10 chars / 30 bytes) and
            // silently re-file them as narration with no speaker.
            if (bs != std::string::npos && bs > 0 && char_len(text.substr(0, bs)) <= 20) {
                speaker = text.substr(0, bs);
                dialogue_text = text.substr(bs + 2);
            }
            // else: the whole string is the content (rare edge case)
        }

        const std::string str_type = speaker.empty() ? "narration" : "dialogue";
        if (str_type == "narration" && dialogue_text.rfind("\\n", 0) == 0)
            dialogue_text = dialogue_text.substr(2);

        strings.push_back(
            Str{static_cast<std::size_t>(idx), str_type, speaker, dialogue_text});
    }

    return strings;
}

std::string resolve_source_file(const std::string& game_dir) {
    const std::string dat = game_dir + "\\Data\\sin_text.dat";
    const std::string bak = dat + ".bak";
    // Always prefer the unpatched original when one exists.
    return fs::exists(fs::u8path(bak)) ? bak : dat;
}

int run_extract(const std::string& game_dir, const std::string& output_file) {
    const std::string dat_file = game_dir + "\\Data\\sin_text.dat";
    const std::string backup_file = dat_file + ".bak";

    log_info("Game directory: " + game_dir);
    log_info("Archive: " + dat_file);

    const std::string source_file = resolve_source_file(game_dir);
    if (source_file == backup_file) log_info("Reading from backup: " + backup_file);

    if (!fs::exists(fs::u8path(source_file))) {
        log_info("ERROR: " + source_file + " not found!");
        return 1;
    }

    const Bytes data = read_file(source_file);
    log_info("Archive size: " + comma(static_cast<long long>(data.size())) + " bytes");

    const auto scripts = ddp::parse_ddp3(data);
    log_info("Scripts found: " + std::to_string(scripts.size()));

    bj::object result;
    long long total_dialogue = 0, total_narration = 0, total_choice = 0;

    for (const auto& s : scripts) {
        // Clamp rather than fail on an entry whose payload runs past EOF.
        const std::size_t begin = std::min<std::size_t>(s.offset, data.size());
        const std::size_t end = std::min<std::size_t>(
            static_cast<std::size_t>(s.offset) + s.comp_size, data.size());
        const Bytes compressed(data.begin() + static_cast<std::ptrdiff_t>(begin),
                               data.begin() + static_cast<std::ptrdiff_t>(end));
        const Bytes decompressed = ddp::shs_decompress(compressed, s.decomp_size);

        if (decompressed.size() != s.decomp_size)
            log_info("  WARNING: " + s.name + " decompressed size mismatch (" +
                     std::to_string(decompressed.size()) + " != " +
                     std::to_string(s.decomp_size) + ")");

        const Bytes decrypted = ddp::decrypt_hxb(decompressed);
        const auto strings = extract_strings_from_script(decrypted);

        long long dialogue_count = 0, narration_count = 0, choice_count = 0;
        bj::array arr;
        arr.reserve(strings.size());
        for (const auto& st : strings) {
            if (st.type == "dialogue") ++dialogue_count;
            else if (st.type == "narration") ++narration_count;
            else if (st.type == "choice") ++choice_count;
            bj::object o;
            o["offset"] = static_cast<std::int64_t>(st.offset);
            o["type"] = st.type;
            o["speaker"] = st.speaker;
            o["text"] = st.text;
            arr.push_back(std::move(o));
        }
        total_dialogue += dialogue_count;
        total_narration += narration_count;
        total_choice += choice_count;

        bj::object fd;
        fd["file"] = s.name;
        fd["decompressed_size"] = static_cast<std::int64_t>(s.decomp_size);
        fd["compressed_size"] = static_cast<std::int64_t>(s.comp_size);
        fd["total_entries"] = static_cast<std::int64_t>(strings.size());
        fd["dialogue_count"] = dialogue_count;
        fd["narration_count"] = narration_count;
        fd["choice_count"] = choice_count;
        fd["strings"] = std::move(arr);
        result[s.name] = std::move(fd);

        const std::string choice_str =
            choice_count ? ", " + std::to_string(choice_count) + " choice" : "";
        log_info("  " + s.name + ": " + std::to_string(strings.size()) + " strings (" +
                 std::to_string(dialogue_count) + " dialogue, " +
                 std::to_string(narration_count) + " narration" + choice_str + ")");
    }

    const long long total = total_dialogue + total_narration + total_choice;
    log_info("\nTotal: " + std::to_string(total) + " strings (" +
             std::to_string(total_dialogue) + " dialogue, " +
             std::to_string(total_narration) + " narration, " +
             std::to_string(total_choice) + " choice)");

    write_file_text(output_file, json_pretty(bj::value(std::move(result)), 1));

    // Report the size of the file ON DISK, i.e. after CRLF expansion -- 9.0 MB,
    // not the 8.7 MB the in-memory string would give.
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(output_file))) / 1024.0 /
                      1024.0);
    log_info("\nSaved to " + output_file + " (" + sz + ")");
    return 0;
}

}  // namespace shin::extract
