// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extract/extractor.h"

#include "siglus/scene_pck.h"
#include "siglus/siglus_decoder.h"

#include <boost/regex.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adi::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kSizeFormatBufLen = 64;
constexpr std::size_t kIntFormatBufLen = 32;
constexpr std::size_t kHeaderWarningBufLen = 96;
constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;
constexpr unsigned kHexCharsPerByte = 2;
constexpr unsigned kNibbleShift = 4;
constexpr std::uint8_t kLowNibbleMask = 0x0Fu;
constexpr char32_t kHiraganaFirst = 0x3040;
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;
constexpr char32_t kCjkExtAFirst = 0x3400;
constexpr char32_t kCjkExtALast = 0x4DBF;
constexpr char32_t kCjkBracketFirst = 0x3008;
constexpr char32_t kCjkBracketLast = 0x3011;
constexpr char32_t kCjkTortoiseBracketFirst = 0x3014;
constexpr char32_t kCjkTortoiseBracketLast = 0x301F;
constexpr std::size_t kEscapeSequenceLen = 2;
constexpr std::string_view kSsTxtSuffix = ".ss.txt";
constexpr int kDecimalBase = 10;
constexpr std::size_t kUtf16LeBomLen = 2;
constexpr std::uint8_t kUtf16LeBom0 = 0xFF;
constexpr std::uint8_t kUtf16LeBom1 = 0xFE;
constexpr int kJsonIndent = 1;
constexpr int kSectionIndexPadWidth = 2;
constexpr int kSceneIndexPadWidth = 3;
constexpr std::size_t kMaxPreviewScenes = 20;
constexpr int kExtractExitOk = 0;
constexpr int kExtractExitError = 1;

constexpr std::string_view kNamaeSurnameMacro = "＊Ａ";
constexpr std::string_view kNamaeGivenMacro = "＊Ｂ";
constexpr std::string_view kDefaultProtagonistSurname = "人見";
constexpr std::string_view kDefaultProtagonistGiven = "広介";

std::string mb_of(const std::string& path) {
    char buf[kSizeFormatBufLen];
    std::snprintf(buf, sizeof(buf), "%.1f",
                  static_cast<double>(fs::file_size(fs::u8path(path))) / kBytesPerMegabyte);
    return buf;
}

std::string pad_int(long long v, int width) {
    char buf[kIntFormatBufLen];
    std::snprintf(buf, sizeof(buf), "%*lld", width, v);
    return buf;
}

std::string hex08(std::uint64_t v) {
    char buf[kIntFormatBufLen];
    std::snprintf(buf, sizeof(buf), "%08llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string hex_lower(const std::uint8_t* p, std::size_t n) {
    static constexpr const char* kHexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(n * kHexCharsPerByte);
    for (std::size_t i = 0; i < n; ++i) {
        out += kHexDigits[p[i] >> kNibbleShift];
        out += kHexDigits[p[i] & kLowNibbleMask];
    }
    return out;
}

bool is_forbidden_plate_codepoint(char32_t cp) {
    if (cp >= kCjkBracketFirst && cp <= kCjkBracketLast) {
        return true;
    }
    if (cp >= kCjkTortoiseBracketFirst && cp <= kCjkTortoiseBracketLast) {
        return true;
    }
    switch (cp) {
    case '\r':
    case '\n':
    case '\t':
    case '"':
    case '\'':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '<':
    case '>':
    case '!':
    case '?':
    case '.':
    case ',':
    case ':':
    case ';':
    case U'、':
    case U'。':
    case U'—':
    case U'―':
    case U'‥':
    case U'…':
    case U'！':
    case U'（':
    case U'）':
    case U'，':
    case U'．':
    case U'：':
    case U'；':
    case U'？':
    case U'［':
    case U'］':
    case U'｛':
    case U'｝':
    case U'～':
        return true;
    default:
        return false;
    }
}

bool has_closed_quote_pair(const std::string& text,
                           std::string_view open_q,
                           std::string_view close_q) {
    const std::size_t open_pos = text.find(open_q);
    if (open_pos == std::string::npos) {
        return false;
    }
    const std::size_t close_pos = text.find(close_q, open_pos + open_q.size());
    return close_pos != std::string::npos;
}

bool is_quoted_utterance(const std::string& text) {
    return has_closed_quote_pair(text, "「", "」") ||
           has_closed_quote_pair(text, "『", "』");
}

bool is_protagonist_seed_in_system_scene(std::string_view scene_name,
                                         std::string_view text) {
    if (scene_name != "_start" && scene_name != "_menu") {
        return false;
    }
    return text == kDefaultProtagonistSurname || text == kDefaultProtagonistGiven;
}

// Insertion order of fields in every extracted_text.json entry is load-bearing.
bj::object make_entry(std::size_t idx,
                      const std::string& text,
                      const std::string& type,
                      const std::string& speaker) {
    bj::object e;
    e["idx"] = static_cast<std::int64_t>(idx);
    e["text"] = text;
    e["type"] = type;
    e["has_jp"] = has_japanese(text);
    e["speaker"] = speaker;
    return e;
}

std::int64_t count_jp_entries(const bj::object& all_data) {
    std::int64_t n = 0;
    for (const auto& kv : all_data) {
        for (const auto& sv : kv.value().get_object().at("strings").get_array()) {
            if (auto* h = sv.get_object().if_contains("has_jp")) {
                if (h->is_bool() && h->get_bool()) {
                    ++n;
                }
            }
        }
    }
    return n;
}

std::int64_t count_strings(const bj::object& all_data) {
    std::int64_t n = 0;
    for (const auto& kv : all_data) {
        n += static_cast<std::int64_t>(
            kv.value().get_object().at("strings").get_array().size());
    }
    return n;
}

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

}  // namespace

bool has_japanese(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t cp = utf8_next(text, i);
        if ((cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
            (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
            (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast) ||
            (cp >= kCjkExtAFirst && cp <= kCjkExtALast)) {
            return true;
        }
    }
    return false;
}

bool is_speaker_plate_candidate(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    // Reject leading or trailing Unicode whitespace (including leading/trailing
    // U+3000 on indented narration like "　夢だ。") while preserving internal
    // U+3000 in 2-kanji plates ("睦　月", "望　美", "御　幸", "天　使", "引　用").
    if (trim(text) != text) {
        return false;
    }
    const std::size_t len = char_len(text);
    if (len == 0 || len > kMaxSpeakerPlateCodepoints) {
        return false;
    }
    if (!has_japanese(text)) {
        return false;
    }
    std::size_t i = 0;
    while (i < text.size()) {
        if (is_forbidden_plate_codepoint(utf8_next(text, i))) {
            return false;
        }
    }
    return true;
}

std::string classify_text(const std::string& text) {
    if (text.empty() || trim(text).empty()) {
        return "empty";
    }
    if (is_quoted_utterance(text)) {
        return "dialogue";
    }
    if (is_speaker_plate_candidate(text)) {
        return "name";
    }
    if (has_japanese(text)) {
        return "narrative";
    }
    return "other";
}

bj::array extract_scene_strings(const std::vector<std::string>& raw_strings,
                                std::string_view scene_name) {
    const std::size_t count = raw_strings.size();
    std::vector<bool> present(count, false);
    std::vector<std::string> types(count);
    std::vector<std::string> speakers(count);

    for (std::size_t j = 0; j < count; ++j) {
        const std::string& text = raw_strings[j];
        if (text.empty() || trim(text).empty()) {
            continue;
        }
        present[j] = true;
        if (has_japanese(text)) {
            types[j] = is_protagonist_seed_in_system_scene(scene_name, text)
                           ? "name"
                           : "narrative";
            continue;
        }
        types[j] = "other";
    }

    for (std::size_t j = 1; j < count; ++j) {
        if (!present[j] || !present[j - 1]) {
            continue;
        }
        if (!is_quoted_utterance(raw_strings[j])) {
            continue;
        }
        const std::string& prev = raw_strings[j - 1];
        if (prev == kNamaeSurnameMacro) {
            types[j] = "dialogue";
            speakers[j] = std::string(kDefaultProtagonistSurname);
            continue;
        }
        if (prev == kNamaeGivenMacro) {
            types[j] = "dialogue";
            speakers[j] = std::string(kDefaultProtagonistGiven);
            continue;
        }
        if (is_speaker_plate_candidate(prev)) {
            types[j - 1] = "name";
            types[j] = "dialogue";
            speakers[j] = prev;
        }
    }

    bj::array out;
    for (std::size_t j = 0; j < count; ++j) {
        if (!present[j]) {
            continue;
        }
        out.push_back(make_entry(j, raw_strings[j], types[j], speakers[j]));
    }
    return out;
}

std::vector<std::string> ingest_capture_log(const std::string& path) {
    const Bytes raw = read_file(path);
    const std::string text = utf8_decode_replace(raw.data(), raw.size());

    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }

    std::vector<std::string> out;
    std::set<std::string> seen;
    for (std::string line : lines) {
        line = trim_right_chars(line, "\n");
        line = trim_right_chars(line, "\r");
        if (line.empty()) {
            continue;
        }
        std::string buf;
        std::size_t i = 0;
        while (i < line.size()) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                const char nxt = line[i + 1];
                if (nxt == 'n') {
                    buf += '\n';
                    i += kEscapeSequenceLen;
                    continue;
                }
                if (nxt == 'r') {
                    buf += '\r';
                    i += kEscapeSequenceLen;
                    continue;
                }
                if (nxt == 't') {
                    buf += '\t';
                    i += kEscapeSequenceLen;
                    continue;
                }
                if (nxt == '\\') {
                    buf += '\\';
                    i += kEscapeSequenceLen;
                    continue;
                }
            }
            buf += line[i];
            ++i;
        }
        if (seen.insert(buf).second) {
            out.push_back(buf);
        }
    }
    return out;
}

bj::object ingest_siglus_extract_dump(const std::string& dir_path) {
    bj::object out;
    if (!fs::is_directory(fs::u8path(dir_path))) {
        return out;
    }

    std::vector<std::string> txts;
    for (const auto& de : fs::directory_iterator(fs::u8path(dir_path))) {
        const std::string name = de.path().filename().u8string();
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.size() >= kSsTxtSuffix.size() &&
            lower.compare(lower.size() - kSsTxtSuffix.size(), kSsTxtSuffix.size(),
                          kSsTxtSuffix.data()) == 0) {
            txts.push_back(name);
        }
    }
    std::sort(txts.begin(), txts.end());
    log_info("     SiglusExtract dump: " + std::to_string(txts.size()) + " .ss.txt files");

    static const boost::regex name_re(R"RE(^(\d+)\.(.+)\.ss\.txt$)RE");
    for (const auto& fname : txts) {
        boost::smatch m;
        if (!boost::regex_match(fname, m, name_re)) {
            continue;
        }
        const std::int64_t idx = std::strtoll(m[1].str().c_str(), nullptr, kDecimalBase);
        const std::string scene_name = m[2].str();

        Bytes raw;
        try {
            raw = read_file(dir_path + "\\" + fname);
        } catch (const std::exception&) {
            continue;
        }
        std::size_t start = 0;
        if (raw.size() >= kUtf16LeBomLen && raw[0] == kUtf16LeBom0 &&
            raw[1] == kUtf16LeBom1) {
            start = kUtf16LeBomLen;
        }
        const std::string text =
            utf16le_decode_replace(raw.data() + start, raw.size() - start);

        std::vector<std::string> raw_lines = split(text, "\r\n");
        for (std::string& line : raw_lines) {
            line = trim_right(trim_right_chars(line, std::string(1, '\0')));
            if (trim(line).empty()) {
                line.clear();
            }
        }
        bj::array strings = extract_scene_strings(raw_lines, scene_name);

        bj::object scene;
        scene["name"] = scene_name;
        scene["siglus_idx"] = idx;
        scene["source"] = fname;
        scene["strings"] = std::move(strings);
        out[scene_name] = std::move(scene);
    }
    return out;
}

int run_extract(const std::string& game_dir, const std::string& output_file) {
    const std::string scene_pck = game_dir + "\\Scene.pck";
    const std::string capture_log = game_dir + "\\adieu_jp_strings.txt";
    const std::string siglus_extract_dir = game_dir + "\\__Unpack__\\Scene";

    if (fs::is_regular_file(fs::u8path(scene_pck))) {
        log_info("Opening " + scene_pck + "...");
        const Bytes data = read_file(scene_pck);
        log_info("  size: " + comma(static_cast<long long>(data.size())) + " bytes");

        const siglus::Header hdr = siglus::parse_header(data);
        if (hdr.header_size != siglus::HEADER_SIZE) {
            char buf[kHeaderWarningBufLen];
            std::snprintf(buf, sizeof(buf), "[!] Unexpected header_size 0x%X (expected 0x%X)",
                          hdr.header_size, siglus::HEADER_SIZE);
            log_info(buf);
        }

        log_info("\n--- Header sections ---");
        for (int i = 0; i < siglus::SECTION_COUNT; ++i) {
            log_info("  Sec[" + pad_int(i, kSectionIndexPadWidth) + "]: offset=0x" +
                     hex08(hdr.sections[i].offset) +
                     "  count=" + std::to_string(hdr.sections[i].count));
        }

        const auto macros = siglus::read_name_table(
            data,
            hdr.sections[siglus::SEC_MACRO_NAME_IDX].offset,
            hdr.sections[siglus::SEC_MACRO_NAME_IDX].count,
            hdr.sections[siglus::SEC_MACRO_NAME_BLOB].offset);
        const auto script_names = siglus::read_name_table(
            data,
            hdr.sections[siglus::SEC_SCRIPT_NAME_IDX].offset,
            hdr.sections[siglus::SEC_SCRIPT_NAME_IDX].count,
            hdr.sections[siglus::SEC_SCRIPT_NAME_BLOB].offset);

        log_info("\n--- " + std::to_string(macros.size()) + " macros: ---");
        for (std::size_t i = 0; i < macros.size(); ++i) {
            log_info("  [" + pad_int(static_cast<long long>(i), kSectionIndexPadWidth) + "] " +
                     macros[i]);
        }
        log_info("\n--- " + std::to_string(script_names.size()) + " scenes: ---");
        for (std::size_t i = 0; i < script_names.size() && i < kMaxPreviewScenes; ++i) {
            log_info("  [" + pad_int(static_cast<long long>(i), kSceneIndexPadWidth) + "] " +
                     script_names[i]);
        }
        if (script_names.size() > kMaxPreviewScenes) {
            log_info("  ... and " +
                     std::to_string(script_names.size() - kMaxPreviewScenes) + " more");
        }

        log_info("\nScript data area starts at: 0x" +
                 hex08(hdr.sections[siglus::SEC_SCRIPT_KEY].offset));

        const auto script_meta = siglus::read_script_meta(data, hdr, script_names);

        log_info("\n[OK] Using the built-in SiglusEngine decoder for per-scene extraction");
        if (!script_meta.empty()) {
            const auto& k = siglus::warm_global_key();
            log_info("     warm_global_key -> " + hex_lower(k.data(), k.size()));
        }

        bj::object all_data;
        for (const auto& meta : script_meta) {
            const Bytes chunk = siglus::slice_clamped(data, meta.abs_offset, meta.size);
            std::vector<std::string> strings;
            try {
                strings = siglus::decode_scene(chunk);
            } catch (const LoneSurrogateError&) {
                throw;
            } catch (const std::exception& e) {
                log_info("  [!] decode_scene(" + meta.name + ") failed: " + e.what());
                strings.clear();
            }
            bj::array scene_strings = extract_scene_strings(strings, meta.name);
            bj::object scene;
            scene["name"] = meta.name;
            scene["rel_offset"] = static_cast<std::int64_t>(meta.rel_offset);
            scene["size"] = static_cast<std::int64_t>(meta.size);
            scene["strings"] = std::move(scene_strings);
            all_data[meta.name] = std::move(scene);
        }

        std::int64_t non_underscore = 0;
        for (const auto& kv : all_data) {
            if (kv.key().empty() || kv.key()[0] != '_') {
                ++non_underscore;
            }
        }

        log_info("\nExtraction summary:");
        log_info("  Scenes:          " + std::to_string(non_underscore));
        log_info("  Total strings:   " + comma(count_strings(all_data)));
        log_info("  Japanese strings:" + comma(count_jp_entries(all_data)));

        write_file_text(output_file, json_pretty(bj::value(std::move(all_data)), kJsonIndent));
        log_info("  Output: " + output_file + " (" + mb_of(output_file) + " MB)");
        return kExtractExitOk;
    }

    if (fs::is_directory(fs::u8path(siglus_extract_dir))) {
        log_info("[OK] Found SiglusExtract dump: " + siglus_extract_dir);
        bj::object all_data = ingest_siglus_extract_dump(siglus_extract_dir);
        if (!all_data.empty()) {
            log_info("     Scenes: " + std::to_string(all_data.size()));
            log_info("     Total strings: " + comma(count_strings(all_data)));
            log_info("     Japanese strings: " + comma(count_jp_entries(all_data)));
            write_file_text(output_file,
                            json_pretty(bj::value(std::move(all_data)), kJsonIndent));
            log_info("     Output: " + output_file);
            return kExtractExitOk;
        }
        log_info("     (dump dir is empty -- falling through)");
    }

    if (fs::is_regular_file(fs::u8path(capture_log))) {
        log_info("[OK] Found runtime capture log: " + capture_log);
        const auto captured = ingest_capture_log(capture_log);
        log_info("     " + comma(static_cast<long long>(captured.size())) +
                 " unique JP strings captured");
        if (!captured.empty()) {
            bj::array strings = extract_scene_strings(captured, "_runtime_capture");
            bj::object scene;
            scene["name"] = "_runtime_capture";
            scene["note"] = "Strings captured live by proxy_dll/winmm.dll from " + capture_log;
            scene["strings"] = std::move(strings);
            bj::object all_data;
            all_data["_runtime_capture"] = std::move(scene);
            write_file_text(output_file,
                            json_pretty(bj::value(std::move(all_data)), kJsonIndent));
            log_info("     Output: " + output_file + " (" + mb_of(output_file) + " MB)");
            return kExtractExitOk;
        }
        log_info("     (log is empty -- no fallback sources remain)");
    }

    log_info("[X] Scene.pck not found at " + scene_pck);
    return kExtractExitError;
}

}  // namespace adi::extract
