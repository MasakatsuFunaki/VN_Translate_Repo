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
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"
#include "spt/spt_script.h"

namespace exc::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

std::string resolve_spt_dir(const std::string& game_dir) {
    const std::string backup = game_dir + "\\spt_backup";
    return fs::is_directory(fs::u8path(backup)) ? backup : game_dir + "\\spt";
}

int run_extract(const std::string& spt_dir, const std::string& output_file) {
    std::vector<std::string> spt_files;
    for (const auto& e : fs::directory_iterator(fs::u8path(spt_dir))) {
        const std::string name = e.path().filename().u8string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".spt") == 0)
            spt_files.push_back(name);
    }
    std::sort(spt_files.begin(), spt_files.end());

    bj::object all_data;
    long long total_strings = 0, total_jp = 0;

    for (const auto& fname : spt_files) {
        const Bytes data = read_file(spt_dir + "\\" + fname);
        const Bytes dec = spt::decrypt(data);

        if (!spt::is_valid(dec)) {
            log_info("  SKIP " + fname + ": not a valid SPT file");
            continue;
        }

        auto strings = spt::extract_strings(dec);
        long long jp_count = 0;
        bj::array arr;
        arr.reserve(strings.size());
        for (auto& s : strings) {
            s.type = spt::classify(s.text);
            if (s.has_jp) ++jp_count;
            bj::object o;
            o["offset"] = static_cast<std::int64_t>(s.offset);
            o["byte_len"] = static_cast<std::int64_t>(s.byte_len);
            o["text"] = s.text;
            o["has_jp"] = s.has_jp;
            o["type"] = s.type;
            arr.push_back(std::move(o));
        }

        bj::object fd;
        fd["file"] = fname;
        fd["file_size"] = static_cast<std::int64_t>(data.size());
        fd["header_string_count"] = static_cast<std::int64_t>(spt::header_string_count(dec));
        fd["strings_found"] = static_cast<std::int64_t>(strings.size());
        fd["jp_strings"] = jp_count;
        fd["strings"] = std::move(arr);
        all_data[fname] = std::move(fd);

        total_strings += static_cast<long long>(strings.size());
        total_jp += jp_count;
        log_info("  " + fname + ": " + std::to_string(strings.size()) + " strings (" +
                 std::to_string(jp_count) + " Japanese)");
    }

    const std::size_t file_count = all_data.size();
    // Indent 1: at ~141k strings, indent 2 costs megabytes for no legibility.
    write_file_text(output_file, json_pretty(bj::value(std::move(all_data)), 1));

    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(output_file))) / 1024.0 / 1024.0);
    log_info("\nExtraction complete:");
    log_info("  Files processed: " + std::to_string(file_count));
    log_info("  Total strings: " + std::to_string(total_strings));
    log_info("  Japanese strings: " + std::to_string(total_jp));
    log_info("  Output: " + output_file + " (" + sz + ")");
    return 0;
}

}  // namespace exc::extract
