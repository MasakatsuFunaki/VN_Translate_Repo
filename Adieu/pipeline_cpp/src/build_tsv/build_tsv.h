// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder for Adieu.
//
// Reads translated_text.json and writes translation_table.tsv:
//     JP<TAB>EN\n     (UTF-8 without BOM, LF only, \\ \t \r \n escaped)
// Excludes non-translatable `other`/`empty` rows and Siglus #NAMAE macro keys
// ("＊Ａ", "＊Ｂ").
#pragma once

#include <string>
#include <vector>

namespace adi::build_tsv {

struct RebuildTableOptions {
    std::string project_dir;
    std::string out_tsv;       // Empty => <project_dir>\translation_table.tsv
    std::string game_dir;      // Optional retail GameData dir to copy TSV into
    bool sync_install = false; // Refresh <project_dir>\build\install\game\translation_table.tsv if dir exists
};

// Backslash FIRST, then tab / CR / LF -- escaping the others first would
// double-escape the backslashes they introduce.
std::string escape_for_tsv(const std::string& text);

int run_build(const std::string& translated_file, const std::string& out_tsv);

// Rebuilds the primary TSV (defaulting to `<project_dir>\translation_table.tsv`
// when `opt.out_tsv` is empty), refreshes `<project_dir>\build\install\game\translation_table.tsv`
// only when `(opt.sync_install || !opt.game_dir.empty()) && opt.out_tsv.empty()`
// and that staging folder exists, and copies to `<opt.game_dir>\translation_table.tsv`
// when `opt.game_dir` is provided and exists on disk.
int run_rebuild_table(const RebuildTableOptions& opt);

// Parses UTF-8 CLI arguments (`--help`/`-h`, `--rebuild-table`, `--sync-install`,
// `--dir`, `--out`, `--game-dir`) and executes `run_rebuild_table`.
int run_build_tsv_args(const std::vector<std::string>& utf8_args,
                       const std::string& default_project_dir = {});

}  // namespace adi::build_tsv
