// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv.h"

#include <filesystem>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"

namespace mgi::build_tsv {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

// Split on a literal separator, keeping empty fields -- an empty segment is a
// blank line the wrapper has to preserve.
std::vector<std::string> split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(sep, pos);
        if (hit == std::string::npos) {
            out.push_back(s.substr(pos));
            return out;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + sep.size();
    }
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

}  // namespace

std::string merge_speaker_line(const std::string& text) {
    const std::size_t idx = text.find("\r\n");
    if (idx == std::string::npos) return text;
    const std::string first_line = text.substr(0, idx);
    const std::string rest = text.substr(idx + 2);
    if (!first_line.empty() && char_len(first_line) <= 20 && !rest.empty() &&
        first_line.front() != '"')
        return first_line + ": " + rest;
    return text;
}

std::string sanitize_newlines(const std::string& text) {
    // Strip leading NEWLINES only, never whitespace: a leading space is part of
    // the line the engine draws.
    std::size_t b = 0;
    while (b < text.size() && text[b] == '\n') ++b;
    std::string s = text.substr(b);
    s = replace_all(s, "\r\n", "\n");
    s = replace_all(s, "\n", "\r\n");
    return s;
}

std::string word_wrap(const std::string& text, int max_chars) {
    const auto segments = split(text, "\r\n");
    std::vector<std::string> result;
    for (const auto& seg : segments) {
        if (char_len(seg) <= static_cast<std::size_t>(max_chars)) {
            result.push_back(seg);
            continue;
        }
        std::string current;
        for (const auto& word : split(seg, " ")) {
            if (word.empty()) {
                if (!current.empty()) current += ' ';
                continue;
            }
            if (!current.empty() &&
                char_len(current) + 1 + char_len(word) > static_cast<std::size_t>(max_chars)) {
                result.push_back(current);
                current = word;
            } else if (!current.empty()) {
                current += " " + word;
            } else {
                current = word;
            }
        }
        if (!current.empty()) result.push_back(current);
    }
    // Truncate to MAX_LINES so the engine's own cap never silently eats text.
    if (result.size() > static_cast<std::size_t>(MAX_LINES))
        result.resize(static_cast<std::size_t>(MAX_LINES));
    return join(result, "\r\n");
}

std::string escape_for_tsv(const std::string& text) {
    std::string s = replace_all(text, "\\", "\\\\");
    s = replace_all(s, "\r", "\\r");
    s = replace_all(s, "\n", "\\n");
    s = replace_all(s, "\t", "\\t");
    return s;
}

int run_build(const std::string& translated_file, const std::string& out_tsv) {
    log_info("Loading translated text...");
    bj::value root = json_parse_file(translated_file);

    std::vector<std::pair<std::string, std::string>> entries;
    long long skipped_encode = 0;

    for (const auto& fkv : root.get_object()) {
        if (!fkv.value().is_object()) continue;
        const auto* strings = fkv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            std::string translated;
            if (auto* t = s.if_contains("translated"))
                if (t->is_string()) translated = std::string(t->get_string());
            const std::string jp_text(s.at("text").get_string());
            if (translated.empty() || translated == jp_text) continue;

            std::string en_text =
                word_wrap(merge_speaker_line(sanitize_newlines(translated)));

            // Both sides must survive CP932 -- it's what the engine reads.
            if (!utf8_to_cp932_strict(jp_text)) {
                ++skipped_encode;
                continue;
            }
            if (!utf8_to_cp932_strict(en_text)) {
                // Round-trip through the lossy encoder so unmappable chars
                // become '?' rather than dropping the whole row.
                const Bytes enc = utf8_to_cp932_replace(en_text);
                en_text = cp932_to_utf8_replace(enc.data(), enc.size());
            }
            entries.emplace_back(jp_text, en_text);
        }
    }

    // The table is written in CP932, not UTF-8: the engine reads it directly.
    Bytes out;
    for (const auto& [jp, en] : entries) {
        const std::string line = escape_for_tsv(jp) + "\t" + escape_for_tsv(en) + "\n";
        const Bytes enc = utf8_to_cp932_replace(line);
        out.insert(out.end(), enc.begin(), enc.end());
    }
    fs::create_directories(fs::u8path(out_tsv).parent_path());
    write_file(out_tsv, out);

    log_info("Generated " + out_tsv);
    log_info("  Entries: " + std::to_string(entries.size()));
    if (skipped_encode)
        log_info("  Skipped (CP932 encode error): " + std::to_string(skipped_encode));
    log_info("  File size: " +
             comma(static_cast<long long>(fs::file_size(fs::u8path(out_tsv)))) + " bytes");
    return 0;
}

}  // namespace mgi::build_tsv
