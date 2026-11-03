// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The two analysis reports that are written to disk and committed as fixtures.
//
// They live here rather than in the app mains so ut_golden_logs can rebuild them
// in-process and diff against analysis/logs/*.txt without spawning anything.
// Both return the report body with LF endings; the apps convert to CRLF on the
// way out, which is what the committed files hold.
#pragma once

#include <string>

namespace ksh::analysis {

// 01_recon: PE layout, imports, TLS, entry-point disassembly, DRM strings.
std::string recon_report(const std::string& exe_path);

// 03_parse_dump: exception record, crash context, modules, faulting addresses.
std::string dump_report(const std::string& dump_path);

}  // namespace ksh::analysis
