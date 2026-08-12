// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- the whole translation step: the speaker gate, then the
// batched translation of the SPT strings via Anthropic Claude into
// script_output/translated_text.json.
//
// The gate runs first, inside translate_all, before a key is read and before
// the client is built: a run that cannot produce a correct answer costs
// nothing and writes nothing.
//
// Usage:
//   02_translate --batch 50
//   02_translate --test        # 1 API batch then stop
//   02_translate --test 4      # 4 batches
//   02_translate --file 01.spt
#include <filesystem>
#include <string>

#include "common/util.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace exc;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game, only;
    int batch = 0, test_n = 0;
    bool retranslate = false;
    po::options_description desc(
        "02_translate -- speaker gate, then translate SPT text via Claude");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("batch", po::value(&batch)->default_value(150), "lines per API request")
        ("retranslate", po::bool_switch(&retranslate), "ignore cache, retranslate everything")
        ("file", po::value(&only), "translate only this .spt file")
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "run only N API batches then stop (bare --test = 1)");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // Validated before anything is created or authenticated: a mistyped cap
    // must cost nothing.
    if (auto why = translate::validate_options(batch, test_n)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    const std::string out_dir = project + "\\script_output";
    fs::create_directories(fs::u8path(out_dir));

    translate::TranslateOptions opt;
    opt.input_file = out_dir + "\\extracted_text.json";
    opt.cache_file = out_dir + "\\translation_cache_anthropic.json";
    opt.output_file = out_dir + "\\translated_text.json";
    opt.test_dir = project + "\\test";
    opt.batch_size = batch;
    opt.retranslate = retranslate;
    if (!only.empty()) opt.only_file = only;
    opt.test_batches = test_n;

    try {
        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
