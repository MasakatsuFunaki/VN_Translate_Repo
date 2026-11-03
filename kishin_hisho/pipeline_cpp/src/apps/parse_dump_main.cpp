// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// analysis/03_parse_dump -- pull the exception record, crash-time register
// context and module list out of a Windows minidump, and resolve the faulting
// addresses to module+offset.
//
// Output goes to analysis/logs/03_parse_dump.txt, which is committed and is the
// second regression fixture ut_golden_logs checks on every build.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "analysis/reports.h"
#include "common/util.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace ksh;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = ksh::apps::po;

    std::string project, out_override;
    std::vector<std::string> dumps;
    po::options_description desc("03_parse_dump -- parse a Windows minidump");
    desc.add(ksh::apps::common_options(project));
    desc.add_options()
        ("dump", po::value(&dumps)->multitoken(),
         "dump file(s); default: every analysis/dumps/*.dmp, sorted")
        ("out", po::value(&out_override),
         "write the report here instead of analysis/logs/03_parse_dump.txt");
    po::variables_map vm;
    if (auto rc = ksh::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string root = project + "\\analysis";
    const std::string out_path =
        out_override.empty() ? root + "\\logs\\03_parse_dump.txt" : out_override;

    try {
        if (dumps.empty()) {
            for (const auto& e : fs::directory_iterator(fs::u8path(root + "\\dumps"))) {
                const std::string name = e.path().filename().u8string();
                if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".dmp") == 0)
                    dumps.push_back(e.path().u8string());
            }
            std::sort(dumps.begin(), dumps.end());
        }

        // Each report is followed by a blank line, so the file ends with one.
        std::string txt;
        for (const auto& d : dumps) txt += analysis::dump_report(d) + "\n\n";
        if (!txt.empty()) txt.pop_back();

        print_line(txt);
        fs::create_directories(fs::u8path(root + "\\logs"));
        write_file_text(out_path, txt);
        print_line("");
        print_line("[wrote " + out_path + "]");
        return 0;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
