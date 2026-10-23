// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation coverage: how many entries of a document carry real English and
// how many still carry the raw Japanese.
//
// The document keeps the Japanese in `translated` for a line nothing has
// answered for yet, so a run that lost half its answers still produces a
// complete-looking file.  These counts are what makes that visible, and the
// report names the document it counted because a run can hold two.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <boost/json.hpp>

namespace snt {

// True for a line of punctuation and symbols only.  Such a line translates to
// itself, so it can never register as translated and is kept out of the
// denominator rather than counted as a miss.
bool is_exempt_line(const std::string& text);

struct CoverageCounts {
    std::int64_t translated = 0;  // 'translated' differs from 'text'
    std::int64_t identity = 0;    // still the untouched Japanese, and should not be
    std::int64_t exempt = 0;      // untouched because the English IS the Japanese
    std::int64_t total = 0;

    // The denominator that can reach 100%.
    std::int64_t translatable() const { return total - exempt; }
};

struct CoverageReport {
    std::string source_file;  // the document these counts describe
    CoverageCounts totals;
    std::map<std::string, CoverageCounts> per_script;
};

// Count one translated_text.json document.  An entry with no `translated`
// field counts as identity: nothing has replaced the Japanese yet.
CoverageReport count_coverage(const boost::json::object& translated,
                              const std::string& source_file);

// coverage_report.json beside the counted document.
std::string coverage_report_path(const std::string& counted_file);

void write_coverage_report(const CoverageReport& report, const std::string& out_file);

// Totals plus the least-covered scripts, on stdout.
void log_coverage_summary(const CoverageReport& report);

// Count `document`, print the summary and write the report beside the file it
// came from.  This is what both the translation run and --rebuild-table call,
// so the two cannot report differently.
void report_coverage(const boost::json::object& document, const std::string& source_file);

}  // namespace snt
