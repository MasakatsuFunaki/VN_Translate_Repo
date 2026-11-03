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
#include "fvp/fvp_hcb.h"

namespace fav::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

int run_extract(const std::string& hcb_path, const std::string& output_file) {
    log_info("Opening " + hcb_path + "...");
    std::vector<fvp::StringRef> items = fvp::load_all(hcb_path);
    log_info("  " + comma(static_cast<long long>(items.size())) +
             " strings extracted from bytecode");

    long long jp_count = 0;
    for (const auto& it : items)
        if (is_jp(it.decoded)) ++jp_count;
    log_info("  containing Japanese: " + comma(jp_count));

    bj::array lines;
    lines.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        bj::object line;
        line["idx"] = static_cast<std::int64_t>(i);
        line["type"] = "STRING";
        line["offset"] = static_cast<std::int64_t>(items[i].offset);
        line["content"] = items[i].decoded;
        lines.push_back(std::move(line));
    }

    bj::object script;
    script["name"] = "Sakura.hcb";
    script["size"] = static_cast<std::int64_t>(fs::file_size(fs::u8path(hcb_path)));
    script["lines"] = std::move(lines);

    bj::object out;
    out["Sakura.hcb"] = std::move(script);

    write_file(output_file, json_dump(bj::value(std::move(out))));

    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(output_file))) /
                      1024.0 / 1024.0);
    log_info("\nSaved " + output_file + " (" + sz + ")");
    return 0;
}

}  // namespace fav::extract
