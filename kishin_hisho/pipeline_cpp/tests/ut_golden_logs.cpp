// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The headline regression: rebuild both analysis reports in-process and compare
// them against the committed logs under analysis/logs/.
//
// Those logs are the reference output for the whole PE + minidump stack, so the
// comparison runs on every build rather than by hand.  A mismatch prints the
// first differing line, since a 6 KB diff in a GoogleTest failure is unreadable.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "analysis/reports.h"
#include "common/util.h"

namespace fs = std::filesystem;
using namespace ksh;

namespace {

std::string project_dir() { return KSH_PROJECT_DIR; }

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// The committed logs hold CRLF (the apps convert on the way out) while the
// report builders emit LF, so compare line by line rather than byte by byte;
// write_file_text() is what owns the exact line endings.
void expect_matches_log(const std::string& produced, const std::string& log_name) {
    const std::string log_path = project_dir() + "\\analysis\\logs\\" + log_name;
    if (!fs::exists(fs::u8path(log_path))) GTEST_SKIP() << log_name << " not present";
    const Bytes raw = read_file(log_path);
    const auto want = split_lines(std::string(raw.begin(), raw.end()));
    const auto got = split_lines(produced);

    for (std::size_t i = 0; i < std::min(want.size(), got.size()); ++i)
        ASSERT_EQ(want[i], got[i]) << log_name << " diverges at line " << (i + 1);
    ASSERT_EQ(want.size(), got.size()) << log_name << " has a different number of lines";
}

}  // namespace

TEST(GoldenLogs, recon_report_reproduces_01_recon_txt) {
    const std::string exe = project_dir() + "\\analysis\\samples\\KishinHisho_orig.exe";
    if (!fs::exists(fs::u8path(exe))) GTEST_SKIP() << "sample exe not present";
    // 01_recon writes the report with no trailing newline; 03 ends with one.
    expect_matches_log(analysis::recon_report(exe), "01_recon.txt");
}

TEST(GoldenLogs, dump_report_reproduces_03_parse_dump_txt) {
    const std::string dump =
        project_dir() + "\\analysis\\dumps\\KishinHisho.exe.45764.dmp";
    if (!fs::exists(fs::u8path(dump))) GTEST_SKIP() << "crash dump not present";
    // One dump -> the report plus the blank line that separates dumps.
    expect_matches_log(analysis::dump_report(dump) + "\n", "03_parse_dump.txt");
}
