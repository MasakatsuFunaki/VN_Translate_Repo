// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_tsv -- speaker gate, apply cached translations, write
// translated_text.json and the translations.tsv the game loads.
//
//   02_build_tsv --game-dir <install>
//   02_build_tsv --game-dir <install> --out <path>
//   02_build_tsv --game-dir <install> --rebuild-table
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr int kRefusedExitCode = 2;

}  // namespace

int main() {
    try {
        crc::setup_console_utf8();
        namespace po = crc::apps::po;

        std::string project;
        std::string game;
        std::string out_tsv;
        bool rebuild_table = false;
        po::options_description desc(
            "02_build_tsv -- speaker gate, apply cached translations, build translations.tsv");
        desc.add(crc::apps::common_options(project, game));
        desc.add_options()
            ("out", po::value(&out_tsv),
             "translation table path (default <game-dir>\\translations.tsv with --rebuild-table)")
            ("rebuild-table", po::bool_switch(&rebuild_table),
             "rebuild translations.tsv from the translated document already on "
             "disk and stop");
        po::variables_map vm;
        if (const auto rc = crc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (rebuild_table) {
            const std::string doc = crc::apps::translated_json(project);
            const std::string tsv = out_tsv.empty() ? crc::apps::game_tsv(game) : out_tsv;
            if (!fs::exists(fs::u8path(doc))) {
                crc::log_info("[ERROR] no translated document at " + doc);
                return kRefusedExitCode;
            }
            return crc::build_tsv::run_build(doc, tsv, "");
        }

        fs::create_directories(fs::u8path(crc::apps::output_dir(project)));

        crc::translate::TranslateOptions opt;
        opt.input_file = crc::apps::extracted_json(project);
        opt.cache_file = crc::apps::cache_json(project);
        opt.output_file = crc::apps::translated_json(project);
        opt.tsv_file = out_tsv.empty() ? crc::apps::out_tsv(project) : out_tsv;
        opt.game_tsv_file = crc::apps::game_tsv(game);

        return crc::translate::translate_all(opt);
    } catch (const std::exception& e) {
        crc::log_info(std::string("[ERROR] ") + e.what());
        return crc::apps::kExitError;
    }
}
