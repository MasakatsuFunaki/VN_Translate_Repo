// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- speaker gate, batched Gemini translation, cache persistence,
// and translations.tsv generation for CROSS†CHANNEL -FINAL COMPLETE-.
//
//   02_translate --gate-only
//   02_translate --max-batches 1
//   02_translate --game-dir <install>
#include "apps/paths.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

#include <cstdint>
#include <exception>
#include <string>

namespace {

constexpr int kUnlimitedBatches = 0;
constexpr double kDefaultMaxBudgetEur = 50.0;

}  // namespace

int main() {
    try {
        crc::setup_console_utf8();
        namespace po = crc::apps::po;
        namespace tr = crc::translate;

        std::string project;
        std::string game;
        std::string out_tsv;
        std::string model{tr::kDefaultModel};
        std::string thinking_level;
        int batch_size = tr::kDefaultBatchSize;
        int context_window = tr::kDefaultContextWindow;
        int context_after_window = tr::kDefaultContextAfterWindow;
        std::int64_t thinking_budget = tr::kDefaultThinkingBudget;
        int max_batches = kUnlimitedBatches;
        double max_budget_eur = kDefaultMaxBudgetEur;
        bool gate_only = false;
        bool retranslate = false;
        bool discard_cache = false;
        bool build_partial_tsv = false;

        po::options_description desc(
            "02_translate -- speaker gate, translate CROSS_CHANNEL via Gemini, "
            "and build translations.tsv");
        desc.add(crc::apps::optional_game_options(project, game));
        desc.add_options()
            ("model", po::value(&model)->default_value(std::string(tr::kDefaultModel)),
             "Gemini model identifier")
            ("batch", po::value(&batch_size)->default_value(tr::kDefaultBatchSize),
             "unique uncached lines per request")
            ("context", po::value(&context_window)->default_value(tr::kDefaultContextWindow),
             "previous rolling context lines")
            ("context-after",
             po::value(&context_after_window)->default_value(tr::kDefaultContextAfterWindow),
             "lookahead context lines after each batch")
            ("thinking-budget",
             po::value(&thinking_budget)->default_value(tr::kDefaultThinkingBudget),
             "Gemini thinkingBudget tokens (<0 disables)")
            ("thinking-level", po::value(&thinking_level),
             "optional discrete thinkingLevel (LOW, MEDIUM, HIGH)")
            ("max-batches", po::value(&max_batches)->default_value(kUnlimitedBatches),
             "stop after N top-level batches (0 = unlimited)")
            ("max-budget-eur",
             po::value(&max_budget_eur)->default_value(kDefaultMaxBudgetEur),
             "hard cumulative EUR budget cap (<=0 disables)")
            ("gate-only", po::bool_switch(&gate_only),
             "run speaker gate and corpus summary without API calls or writes")
            ("retranslate", po::bool_switch(&retranslate),
             "discard existing cache and translate from scratch")
            ("discard-cache", po::bool_switch(&discard_cache),
             "confirm deleting a paid translation cache with --retranslate")
            ("build-partial-tsv", po::bool_switch(&build_partial_tsv),
             "write translated_text.json and translations.tsv even when stopped early")
            ("out", po::value(&out_tsv),
             "override output translations.tsv path");

        po::variables_map vm;
        if (const auto rc = crc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        tr::GeminiTranslateOptions opt;
        opt.input_file = crc::apps::extracted_json(project);
        opt.cache_file = crc::apps::cache_json(project);
        opt.output_file = crc::apps::translated_json(project);
        opt.tsv_file = out_tsv.empty() ? crc::apps::out_tsv(project) : out_tsv;
        opt.game_tsv_file = game.empty() ? std::string{} : crc::apps::game_tsv(game);
        opt.blocked_file = crc::apps::blocked_lines_json(project);
        opt.raw_log_file = crc::apps::raw_answers_log(project);
        opt.last_batch_file = crc::apps::last_batch_json(project);
        opt.model = model;
        opt.batch_size = batch_size;
        opt.context_window = context_window;
        opt.context_after_window = context_after_window;
        opt.thinking_budget = thinking_budget;
        opt.thinking_level = thinking_level;
        opt.max_batches = max_batches;
        opt.max_budget_eur = max_budget_eur;
        opt.gate_only = gate_only;
        opt.retranslate = retranslate;
        opt.discard_cache = discard_cache;
        opt.skip_tsv_on_partial = !build_partial_tsv;

        tr::BatchTranslator translator;
        const tr::TranslateReport report = translator.run(opt);
        return report.exit_code;
    } catch (const std::exception& e) {
        crc::log_info(std::string("[ERROR] ") + e.what());
        return crc::apps::kExitError;
    }
}
