// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation coverage: how many entries carry real English vs. raw Japanese.
// The report carries its source filename because the two documents can disagree.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <boost/json.hpp>

namespace snu {

// True for a line of only punctuation/symbols.  Excluded from the denominator
// because it translates as itself and can never register as "translated".
bool is_exempt_line(const std::string& text);

struct CoverageCounts {
    std::int64_t translated = 0;  // 'translated' differs from 'text'
    std::int64_t identity = 0;    // still the untouched Japanese, and shouldn't be
    std::int64_t exempt = 0;      // untouched because the English IS the Japanese
    std::int64_t total = 0;

    // The denominator that can actually reach 100%.
    std::int64_t translatable() const { return total - exempt; }
};

struct CoverageReport {
    std::string source_file;  // the document these counts describe
    CoverageCounts totals;
    std::map<std::string, CoverageCounts> per_script;
};

// Count one translated_text.json document.  Entries with no 'translated'
// field count as identity: nothing has replaced the Japanese yet.
CoverageReport count_coverage(const boost::json::object& translated,
                              const std::string& source_file);

// coverage_report.json beside the counted document.
std::string coverage_report_path(const std::string& counted_file);

void write_coverage_report(const CoverageReport& report, const std::string& out_file);

// Totals plus the least-covered scripts, on stdout.
void log_coverage_summary(const CoverageReport& report);

}  // namespace snu
