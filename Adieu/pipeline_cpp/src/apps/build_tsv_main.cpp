// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_tsv -- standalone runtime translation-table builder for Adieu.
//
// Usage:
//   02_build_tsv --rebuild-table --dir <AdieuDir>
//   02_build_tsv --rebuild-table --dir <AdieuDir> --game-dir <GameDataDir>
//   02_build_tsv --rebuild-table --dir <AdieuDir> --out <CustomTsvPath>
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"

#include <exception>
#include <string>

int main() {
    adi::setup_console_utf8();
    try {
        return adi::build_tsv::run_build_tsv_args(
            adi::apps::process_utf8_args(),
            adi::apps::default_project_dir());
    } catch (const std::exception& e) {
        adi::log_info(std::string("[ERROR] ") + e.what());
        return adi::apps::kExitError;
    }
}
