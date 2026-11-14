// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv/build_tsv.h"

#include "common/util.h"

#include <boost/json.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace adi::build_tsv {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitRefused = 2;

constexpr std::string_view kNamaeSurnameMacro = "＊Ａ";
constexpr std::string_view kNamaeGivenMacro = "＊Ｂ";

const char* const kTranslatableTypes[] = {"dialogue", "narrative", "name", "menu"};

bool is_translatable_type(const std::string& t) {
    for (const char* k : kTranslatableTypes) {
        if (t == k) {
            return true;
        }
    }
    return false;
}

bool is_namae_macro_key(std::string_view jp) {
    return jp == kNamaeSurnameMacro || jp == kNamaeGivenMacro;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

std::string obj_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key)) {
        if (v->is_string()) {
            return std::string(v->get_string());
        }
    }
    return {};
}

bool consume_value_option(const std::vector<std::string>& args,
                          std::size_t& idx,
                          std::string_view flag,
                          std::string& dst,
                          std::string& err) {
    const std::string& token = args[idx];
    if (token == flag) {
        if (idx + 1 >= args.size()) {
            err = "option '" + std::string(flag) + "' requires an argument";
            return false;
        }
        ++idx;
        dst = args[idx];
        return true;
    }
    const std::string prefix = std::string(flag) + "=";
    if (token.rfind(prefix, 0) == 0) {
        dst = token.substr(prefix.size());
        return true;
    }
    return false;
}

void print_usage() {
    print_line("02_build_tsv -- rebuild Adieu translation_table.tsv from translated_text.json");
    print_line("");
    print_line("Options:");
    print_line("  -h, --help            Show this help message and exit");
    print_line("  --rebuild-table       Rebuild translation_table.tsv (default action)");
    print_line("  --dir <path>          Adieu project folder");
    print_line("  --out <path>          Explicit output TSV path (skips staged install sync)");
    print_line("  --game-dir <path>     Copy rebuilt translation_table.tsv into GameData");
    print_line("  --sync-install        Refresh build\\install\\game\\translation_table.tsv if present");
}

}  // namespace

std::string escape_for_tsv(const std::string& text) {
    std::string s = replace_all(text, "\\", "\\\\");
    s = replace_all(s, "\t", "\\t");
    s = replace_all(s, "\r", "\\r");
    s = replace_all(s, "\n", "\\n");
    return s;
}

int run_build(const std::string& translated_file, const std::string& out_tsv) {
    log_info("Loading " + translated_file + "...");
    bj::value root = json_parse_file(translated_file);

    std::vector<std::pair<std::string, std::string>> seen;
    std::map<std::string, std::size_t> index;
    long long conflicts = 0;

    for (const auto& skv : root.get_object()) {
        if (!skv.value().is_object()) {
            continue;
        }
        const auto* strings = skv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) {
            continue;
        }
        for (const auto& sv : strings->get_array()) {
            const auto& s = sv.get_object();
            if (!is_translatable_type(obj_str(s, "type"))) {
                continue;
            }
            const std::string jp = obj_str(s, "text");
            const std::string en = obj_str(s, "translated");
            if (jp.empty() || en.empty() || jp == en || is_namae_macro_key(jp)) {
                continue;
            }
            auto it = index.find(jp);
            if (it == index.end()) {
                index.emplace(jp, seen.size());
                seen.emplace_back(jp, en);
                continue;
            }
            if (seen[it->second].second != en) {
                ++conflicts;
            }
        }
    }

    std::string out;
    for (const auto& [jp, en] : seen) {
        out += escape_for_tsv(jp) + "\t" + escape_for_tsv(en) + "\n";
    }

    const fs::path parent = fs::u8path(out_tsv).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent);
    }
    // Binary write preserves LF-only line endings required by the runtime TSV parser.
    write_file(out_tsv, out);

    log_info("Generated " + out_tsv);
    log_info("  Entries:    " + comma(static_cast<long long>(seen.size())));
    log_info("  Conflicts:  " + comma(conflicts) + "  (same JP, different EN -- first kept)");
    log_info("  File size:  " +
             comma(static_cast<long long>(fs::file_size(fs::u8path(out_tsv)))) + " bytes");
    return kExitOk;
}

int run_rebuild_table(const RebuildTableOptions& opt) {
    if (opt.project_dir.empty()) {
        log_info("[ERROR] project directory is empty; pass --dir <AdieuDir>");
        return kExitError;
    }

    const fs::path proj = fs::u8path(opt.project_dir);
    const fs::path translated = proj / "script_output" / "translated_text.json";
    std::error_code ec;
    if (!fs::is_regular_file(translated, ec)) {
        log_info("[ERROR] no translated document at " + translated.u8string());
        return kExitRefused;
    }

    const fs::path primary_tsv =
        opt.out_tsv.empty() ? (proj / "translation_table.tsv") : fs::u8path(opt.out_tsv);
    const int rc = run_build(translated.u8string(), primary_tsv.u8string());
    if (rc != kExitOk) {
        return rc;
    }

    if (opt.out_tsv.empty() && (opt.sync_install || !opt.game_dir.empty())) {
        const fs::path install_game = proj / "build" / "install" / "game";
        if (fs::is_directory(install_game, ec)) {
            const fs::path staged_tsv = install_game / "translation_table.tsv";
            fs::copy_file(primary_tsv, staged_tsv, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                log_info("[ERROR] failed syncing staged table " + staged_tsv.u8string() +
                         ": " + ec.message());
                return kExitError;
            }
            log_info("  Synced staged table -> " + staged_tsv.u8string());
        }
    }

    if (!opt.game_dir.empty()) {
        const fs::path game_path = fs::u8path(opt.game_dir);
        if (!fs::is_directory(game_path, ec)) {
            log_info("[ERROR] game directory not found at " + opt.game_dir);
            return kExitError;
        }
        const fs::path deployed_tsv = game_path / "translation_table.tsv";
        fs::copy_file(primary_tsv, deployed_tsv, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            log_info("[ERROR] failed copying table to " + deployed_tsv.u8string() +
                     ": " + ec.message());
            return kExitError;
        }
        log_info("  Deployed table -> " + deployed_tsv.u8string());
    }

    return kExitOk;
}

int run_build_tsv_args(const std::vector<std::string>& utf8_args,
                       const std::string& default_project_dir) {
    RebuildTableOptions opt;
    opt.project_dir = default_project_dir;

    for (std::size_t i = 0; i < utf8_args.size(); ++i) {
        const std::string& arg = utf8_args[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return kExitOk;
        }
        if (arg == "--rebuild-table") {
            continue;
        }
        if (arg == "--sync-install") {
            opt.sync_install = true;
            continue;
        }

        std::string err;
        if (consume_value_option(utf8_args, i, "--dir", opt.project_dir, err) ||
            consume_value_option(utf8_args, i, "--out", opt.out_tsv, err) ||
            consume_value_option(utf8_args, i, "--game-dir", opt.game_dir, err)) {
            continue;
        }
        if (!err.empty()) {
            log_info("[ERROR] " + err);
            return kExitError;
        }

        log_info("[ERROR] unrecognized argument '" + arg + "'");
        return kExitError;
    }

    if (opt.project_dir.empty()) {
        log_info("[ERROR] --dir is required when project directory cannot be inferred");
        return kExitError;
    }
    return run_rebuild_table(opt);
}

}  // namespace adi::build_tsv
