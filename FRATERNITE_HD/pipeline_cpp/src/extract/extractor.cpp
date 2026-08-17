// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extract/extractor.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "common/util.h"
#include "yuris/cp932_scan.h"
#include "yuris/ypf_archive.h"

namespace frat::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr char32_t kIdeoFullStop = 0x3002;  // 。
constexpr char32_t kFullExclam = 0xFF01;    // ！
constexpr char32_t kFullQuestion = 0xFF1F;  // ？
constexpr char32_t kCloseBracket = 0x300D;  // 」
constexpr char32_t kOpenBracket = 0x300C;   // 「
constexpr char32_t kIdeoComma = 0x3001;     // 、
constexpr char32_t kOpenDouble = 0x300E;    // 『
constexpr char32_t kCloseDouble = 0x300F;   // 』
constexpr char32_t kFullParenL = 0xFF08;    // （

bool is_terminator(char32_t cp) {
    return cp == kIdeoFullStop || cp == kFullExclam || cp == kFullQuestion;
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// All-ASCII-digits test over the script numbering; an empty string is False.
bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

std::size_t cp932_len(const std::string& s) { return utf8_to_cp932_replace(s).size(); }

std::vector<Run> scan_selected(const Bytes& file, const std::vector<yuris::YpfEntry>& entries,
                               bool require_ystb) {
    std::vector<Run> out;
    for (const auto& entry : entries) {
        const Bytes data = yuris::read_entry(file, entry);
        if (require_ystb && (data.size() < 4 || std::string(data.begin(), data.begin() + 4) != "YSTB"))
            continue;
        for (const auto& [off, text] : yuris::scan_cp932_jp(data, 4, 16384))
            out.push_back(Run{entry.name, off, text});
    }
    return out;
}

std::vector<Run> dialogue_runs(const Bytes& file, const std::string& path) {
    std::vector<std::pair<long long, yuris::YpfEntry>> picked;
    for (const auto& entry : yuris::parse_ypf_index_bytes(file, path)) {
        const std::string base = lower_ascii(yuris::basename(entry.name));
        if (base.rfind("yst", 0) != 0 || base == "yst.ybn" || base == "yst_list.ybn" ||
            !ends_with(base, ".ybn"))
            continue;
        const std::string digits = base.size() >= 7 ? base.substr(3, base.size() - 7) : "";
        if (!all_digits(digits)) continue;
        picked.emplace_back(std::stoll(digits), entry);
    }
    // Story scripts (>= 156) first, both groups ascending.
    std::stable_sort(picked.begin(), picked.end(),
                     [](const auto& a, const auto& b) {
                         const int ga = a.first >= STORY_SCRIPT_START ? 0 : 1;
                         const int gb = b.first >= STORY_SCRIPT_START ? 0 : 1;
                         return ga != gb ? ga < gb : a.first < b.first;
                     });

    std::vector<yuris::YpfEntry> entries;
    entries.reserve(picked.size());
    for (auto& [key, entry] : picked) entries.push_back(std::move(entry));
    return scan_selected(file, entries, /*require_ystb=*/true);
}

std::vector<Run> sidecar_runs(const Bytes& file, const std::string& path) {
    std::vector<yuris::YpfEntry> entries;
    for (const auto& entry : yuris::parse_ypf_index_bytes(file, path)) {
        if (plaintext_ybn_basenames().count(yuris::basename(entry.name)))
            entries.push_back(entry);
    }
    return scan_selected(file, entries, /*require_ystb=*/false);
}

// Insertion-ordered per-YBN string counter.
class FileCounter {
public:
    void bump(const std::string& name) {
        auto it = index_.find(name);
        if (it == index_.end()) {
            index_.emplace(name, order_.size());
            order_.emplace_back(name, 1);
        } else {
            ++order_[it->second].second;
        }
    }
    const std::vector<std::pair<std::string, long long>>& items() const { return order_; }
    long long count(const std::string& name) const {
        auto it = index_.find(name);
        return it == index_.end() ? 0 : order_[it->second].second;
    }

private:
    std::vector<std::pair<std::string, long long>> order_;
    std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace

const std::set<std::string>& plaintext_ybn_basenames() {
    static const std::set<std::string> names = {
        "yscfg.ybn",     // game title
        "yse.ybn",       // engine error messages
        "yst_list.ybn",  // script list / mode labels
        "ysc.ybn",       // Windows errno messages
        "ysl.ybn",       // labels (mostly one-kanji fragments, noisy)
        "ysv.ybn",       // variables (no strings in this game)
        "yst.ybn",       // empty stub
    };
    return names;
}

std::vector<Run> extract_ystb_dialogue(const std::string& ypf_path) {
    return dialogue_runs(read_file(ypf_path), ypf_path);
}

std::vector<Run> extract_plaintext_sidecars(const std::string& ypf_path) {
    return sidecar_runs(read_file(ypf_path), ypf_path);
}

std::vector<std::string> split_into_messages(const std::string& text) {
    const std::vector<char32_t> cps = utf8_decode(text);
    const bool any_terminator =
        std::any_of(cps.begin(), cps.end(),
                    [](char32_t cp) { return is_terminator(cp) || cp == kCloseBracket; });
    if (!any_terminator) return {text};

    std::vector<std::string> out;
    std::size_t buf_start = 0;
    for (std::size_t i = 0; i < cps.size();) {
        std::size_t sep_len = 0;
        if (is_terminator(cps[i]) && i + 1 < cps.size() && cps[i + 1] == kCloseBracket)
            sep_len = 2;
        else if (cps[i] == kCloseBracket || is_terminator(cps[i]))
            sep_len = 1;
        if (sep_len == 0) {
            ++i;
            continue;
        }
        const std::size_t end = i + sep_len;
        const std::string buf = utf8_encode(
            std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(buf_start),
                                  cps.begin() + static_cast<std::ptrdiff_t>(end)));
        // Trim tests emptiness but the kept buffer is unstripped: U+3000 is rendered.
        if (!trim(buf).empty()) out.push_back(buf);
        buf_start = end;
        i = end;
    }
    if (buf_start < cps.size()) {
        const std::string tail = utf8_encode(
            std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(buf_start), cps.end()));
        if (!trim(tail).empty()) out.push_back(tail);
    }
    return out;
}

bool has_japanese(const std::string& t) { return jp_char_count(t) > 0; }

std::string classify(const std::string& t) {
    const std::vector<char32_t> cps = utf8_decode(t);
    const bool has_punct = std::any_of(cps.begin(), cps.end(), [](char32_t cp) {
        return cp == kIdeoFullStop || cp == kIdeoComma || cp == kFullExclam ||
               cp == kFullQuestion || cp == kOpenBracket || cp == kCloseBracket ||
               cp == kOpenDouble || cp == kCloseDouble;
    });
    if (cps.size() <= 8 && !has_punct) return "name";
    if (!cps.empty() && (cps[0] == kOpenBracket || cps[0] == kOpenDouble ||
                         cps[0] == kFullParenL || cps[0] == '('))
        return "dialogue";
    return "narrative";
}

bj::object extract_from_archives(const std::string& pac_dir) {
    bj::object result;
    std::unordered_set<std::string> global_seen;  // dedup only; order not observable

    for (const char* archive_name : {"bn.ypf"}) {
        const std::string path = pac_dir + "\\" + archive_name;
        if (!fs::exists(fs::u8path(path))) {
            log_info(std::string("[archive] skip ") + archive_name + ": not found");
            continue;
        }
        log_info(std::string("[archive] ") + archive_name +
                 ": parsing sidecar + dialogue YBN files...");

        const Bytes file = read_file(path);
        std::vector<Run> runs = dialogue_runs(file, path);
        for (auto& r : sidecar_runs(file, path)) runs.push_back(std::move(r));

        bj::array flat;
        FileCounter per_file_count;
        long long skipped_no_jp = 0;
        for (const auto& run : runs) {
            std::size_t piece_offset = run.offset;
            for (const auto& piece : split_into_messages(run.text)) {
                if (global_seen.count(piece)) {
                    piece_offset += cp932_len(piece);
                    continue;
                }
                if (!has_japanese(piece)) {
                    ++skipped_no_jp;
                    piece_offset += cp932_len(piece);
                    continue;
                }
                global_seen.insert(piece);
                const std::size_t byte_len = cp932_len(piece);
                bj::object e;
                e["offset"] = static_cast<std::int64_t>(piece_offset);
                e["byte_len"] = static_cast<std::int64_t>(byte_len);
                e["text"] = piece;
                e["has_jp"] = true;
                e["type"] = classify(piece);
                e["ybn"] = run.ybn;
                flat.push_back(std::move(e));
                per_file_count.bump(run.ybn);
                piece_offset += byte_len;
            }
        }

        // Sidecars listed individually, per-scene scripts grouped.
        std::vector<std::string> sidecar_files;
        for (const auto& [yn, count] : per_file_count.items())
            if (plaintext_ybn_basenames().count(lower_ascii(yuris::basename(yn))))
                sidecar_files.push_back(yn);
        std::sort(sidecar_files.begin(), sidecar_files.end());
        long long scene_count = 0, total_scene = 0;
        for (const auto& [yn, count] : per_file_count.items()) {
            if (std::find(sidecar_files.begin(), sidecar_files.end(), yn) != sidecar_files.end())
                continue;
            ++scene_count;
            total_scene += count;
        }
        for (const auto& yn : sidecar_files)
            log_info("           " + pad_right_cp(yn, 28) + " " +
                     pad_left_cp(std::to_string(per_file_count.count(yn)), 5) + " strings");
        if (scene_count)
            log_info("           yst*.ybn (" + std::to_string(scene_count) + " scripts)    " +
                     pad_left_cp(std::to_string(total_scene), 5) + " strings");
        log_info("           " + std::string(archive_name) + ": " +
                 std::to_string(flat.size()) + " unique JP strings (skipped " +
                 std::to_string(skipped_no_jp) + " non-JP)");

        if (!flat.empty()) {
            bj::object src;
            src["file"] = archive_name;
            src["source"] = "ypf_archive";
            src["strings"] = std::move(flat);
            result[archive_name] = std::move(src);
        }
    }
    return result;
}

int run_extract(const std::string& pac_dir, const std::string& output_file) {
    log_info(std::string(60, '='));
    log_info("FRATERNITE_HD -- extract Japanese text");
    log_info(std::string(60, '='));

    bj::object data = extract_from_archives(pac_dir);

    std::size_t total = 0;
    for (const auto& kv : data)
        total += kv.value().get_object().at("strings").get_array().size();
    log_info("\n[total] " + std::to_string(total) + " JP strings across " +
             std::to_string(data.size()) + " source(s)");

    if (total == 0) {
        log_info("\n[!] No strings extracted.");
        log_info("    Check that " + pac_dir + " exists and contains bn.ypf.");
        return 1;
    }

    write_file_text(output_file, json_pretty(bj::value(std::move(data)), 2));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f",
                  static_cast<double>(fs::file_size(fs::u8path(output_file))) / (1024.0 * 1024.0));
    log_info("[write] " + output_file + " (" + buf + " MB)");
    return 0;
}

}  // namespace frat::extract
