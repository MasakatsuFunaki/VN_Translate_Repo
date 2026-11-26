// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "xtx.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

#include "common/util.h"

namespace exm::xtx {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(sep, pos);
        if (hit == std::string::npos) {
            out.push_back(s.substr(pos));
            return out;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + 1;
    }
}

std::string join(const std::vector<std::string>& v, char sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

}  // namespace

int translate_xtx_file(const std::string& filepath,
                       const std::vector<std::pair<std::string, std::string>>& translations,
                       const std::string& backup_dir, int name_field) {
    const fs::path live_path = fs::u8path(filepath);
    const fs::path bak_path = fs::u8path(filepath + ".bak");
    const std::string fname = live_path.filename().u8string();
    const fs::path backup_dir_path = fs::u8path(backup_dir);
    const fs::path backup_path = backup_dir_path / fs::u8path(fname);
    const fs::path nested_backup_path = backup_dir_path / "spt" / fs::u8path(fname);

    const bool has_backup = fs::exists(backup_path);
    const bool has_nested_backup = fs::exists(nested_backup_path);
    const bool has_bak = fs::exists(bak_path);
    const bool has_live = fs::exists(live_path);
    if (!has_backup && !has_nested_backup && !has_bak && !has_live) {
        log_info("  SKIP: " + filepath + " not found");
        return 0;
    }

    fs::create_directories(backup_dir_path);
    if (!has_backup) {
        const fs::path seed = has_nested_backup ? nested_backup_path
                                                : (has_bak ? bak_path : live_path);
        fs::copy_file(seed, backup_path);
    }

    const Bytes raw = read_file(backup_path.u8string());
    const auto strict = cp932_to_utf8_strict(raw);
    const std::string content =
        strict ? *strict : cp932_to_utf8_replace(raw.data(), raw.size());

    std::unordered_map<std::string, std::string> map;
    for (const auto& [jp, en] : translations) map.emplace(jp, en);

    int changes = 0;
    std::vector<std::string> new_lines;
    for (const auto& line : split(content, '\n')) {
        const bool had_cr = !line.empty() && line.back() == '\r';
        std::string stripped = had_cr ? line.substr(0, line.size() - 1) : line;
        if (stripped.find(',') != std::string::npos) {
            auto fields = split(stripped, ',');
            if (name_field >= 0 && static_cast<int>(fields.size()) > name_field) {
                const auto idx = static_cast<std::size_t>(name_field);
                const auto it = map.find(fields[idx]);
                if (it != map.end()) {
                    fields[idx] = it->second;
                    ++changes;
                }
            }
            stripped = join(fields, ',');
        }
        new_lines.push_back(stripped + (had_cr ? "\r" : ""));
    }

    const std::string new_content = join(new_lines, '\n');
    if (live_path.has_parent_path()) {
        fs::create_directories(live_path.parent_path());
    }
    write_file(filepath, utf8_to_cp932_replace(new_content));
    return changes;
}

int run_translate_xtx(const std::string& game_dir) {
    const std::string backup_dir = game_dir + "\\xtx_backup";
    fs::create_directories(fs::u8path(backup_dir));

    log_info("Translating .xtx config files...\n");

    const int n = translate_xtx_file(game_dir + "\\spt\\charaname.xtx",
                                     charaname_translations(), backup_dir);
    log_info("  charaname.xtx: " + std::to_string(n) + " changes");

    log_info("\nBackups saved to: " + backup_dir);
    return 0;
}

}  // namespace exm::xtx
