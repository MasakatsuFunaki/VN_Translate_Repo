// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extractor.h"

#include <cstdio>
#include <filesystem>

#include <boost/json.hpp>

#include "common/util.h"
#include "cs2/cs2_archive.h"

namespace ama::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

int run_extract(const std::string& scene_int, const std::string& output_file) {
    log_info("Opening " + scene_int + "...");
    cs2::KifArchive arc(scene_int);
    log_info("Found " + std::to_string(arc.entries().size()) + " files in scene.int");

    bj::object all_scripts;
    long long errors = 0;

    const auto& entries = arc.entries();
    for (std::size_t idx = 0; idx < entries.size(); ++idx) {
        const auto& entry = entries[idx];
        try {
            Bytes data = arc.extract(entry);
            if (data.empty()) continue;
            cs2::SceneScript scene(data);

            bj::array lines_data;
            lines_data.reserve(scene.lines().size());
            for (const auto& line : scene.lines()) {
                const std::string kind_name = cs2::scene_line_type_name(line.kind);
                bj::object o;
                o["idx"] = static_cast<std::int64_t>(line.idx);
                o["type"] = kind_name;
                o["type_val"] = static_cast<std::int64_t>(line.kind);
                o["content"] = line.content;
                lines_data.push_back(std::move(o));
            }

            bj::object script;
            script["arc_idx"] = static_cast<std::int64_t>(idx);
            script["name"] = entry.name;
            script["offset"] = static_cast<std::int64_t>(entry.offset);
            script["size"] = static_cast<std::int64_t>(entry.length);
            script["lines"] = std::move(lines_data);
            all_scripts[entry.name] = std::move(script);

            if ((idx + 1) % 500 == 0)
                log_info("  Processed " + std::to_string(idx + 1) + "/" +
                         std::to_string(entries.size()) + " files...");
        } catch (const std::exception& e) {
            ++errors;
            log_info("  ERROR parsing " + entry.name + ": " + e.what());
        }
    }

    // Counted off the final map, not the loop: a duplicate archive name
    // overwrites its predecessor, and only the survivor should be tallied.
    long long total_msg = 0, total_name = 0;
    for (const auto& kv : all_scripts)
        for (const auto& lv : kv.value().get_object().at("lines").get_array()) {
            const auto& t = lv.get_object().at("type").get_string();
            if (t == "MESSAGE") ++total_msg;
            else if (t == "NAME") ++total_name;
        }

    log_info("\nExtraction complete:");
    log_info("  Scripts: " + std::to_string(all_scripts.size()));
    log_info("  MESSAGE lines: " + std::to_string(total_msg));
    log_info("  NAME lines: " + std::to_string(total_name));
    log_info("  Errors: " + std::to_string(errors));

    log_info("\nSaving to " + output_file + "...");
    write_file(output_file, json_dump(bj::value(std::move(all_scripts))));

    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(output_file))) / 1024.0 / 1024.0);
    log_info(std::string("  Saved (") + sz + ")");
    return 0;
}

}  // namespace ama::extract
