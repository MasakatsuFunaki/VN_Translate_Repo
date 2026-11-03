// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// analysis/01_recon -- static recon of KishinHisho.exe: PE layout, imports,
// entry-point disassembly.
//
// Output goes to analysis/logs/01_recon.txt, which is committed and is the
// regression fixture ut_golden_logs rebuilds and diffs on every build.
#include <filesystem>
#include <string>

#include "analysis/reports.h"
#include "common/util.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace ksh;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = ksh::apps::po;

    std::string project, exe_override, out_override;
    po::options_description desc("01_recon -- static recon of KishinHisho.exe");
    desc.add(ksh::apps::common_options(project));
    desc.add_options()
        ("exe", po::value(&exe_override),
         "read this exe instead of analysis/samples/KishinHisho_orig.exe")
        ("out", po::value(&out_override),
         "write the report here instead of analysis/logs/01_recon.txt "
         "(so a parity run does not clobber the committed fixture)");
    po::variables_map vm;
    if (auto rc = ksh::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string root = project + "\\analysis";
    const std::string exe_path =
        exe_override.empty() ? root + "\\samples\\KishinHisho_orig.exe" : exe_override;
    const std::string out_path =
        out_override.empty() ? root + "\\logs\\01_recon.txt" : out_override;

    try {
        const std::string txt = analysis::recon_report(exe_path);
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
