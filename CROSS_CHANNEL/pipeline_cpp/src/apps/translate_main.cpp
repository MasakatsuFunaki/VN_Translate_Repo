// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- the whole translation step: the speaker gate, the batched
// translation via Anthropic Claude, translated_text.json, and the
// translations.tsv the game loads.
//
//   02_translate            # full run
//   02_translate --test     # 1 API batch then stop
//   02_translate --test 4   # 4 batches
#include <filesystem>
#include <string>

#include "common/util.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    crc::setup_console_utf8();
    namespace po = crc::apps::po;

    std::string project, game, out_tsv;
    int test_n = 0;
    po::options_description desc(
        "02_translate -- speaker gate, translate via Claude, build translations.tsv");
    desc.add(crc::apps::common_options(project, game));
    desc.add_options()
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "run only N API batches then stop (bare --test = 1)")
        ("out", po::value(&out_tsv),
         "translation table path (default <dir>\\translations.tsv)");
    po::variables_map vm;
    if (auto rc = crc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // Validated before anything is created, deleted or authenticated: a
    // mistyped cap must cost nothing.
    if (test_n < 0) {
        crc::log_info("[ERROR] --test takes a positive batch count.");
        return 2;
    }

    fs::create_directories(fs::u8path(crc::apps::output_dir(project)));

    crc::translate::TranslateOptions opt;
    opt.input_file = crc::apps::extracted_json(project);
    opt.cache_file = crc::apps::cache_json(project);
    opt.output_file = crc::apps::translated_json(project);
    opt.test_dir = crc::apps::test_dir(project);
    opt.test_batches = test_n;
    // --out defaults off --dir, so it cannot be a static default_value.
    opt.tsv_file = out_tsv.empty() ? crc::apps::out_tsv(project) : out_tsv;
    opt.game_tsv_file = crc::apps::game_tsv(game);

    try {
        return crc::translate::translate_all(opt);
    } catch (const std::exception& e) {
        crc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
