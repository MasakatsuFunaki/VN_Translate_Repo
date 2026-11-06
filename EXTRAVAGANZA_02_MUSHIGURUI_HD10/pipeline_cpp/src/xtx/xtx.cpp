// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "xtx.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

#include "common/util.h"

namespace mgi::xtx {

namespace fs = std::filesystem;

namespace {

// Split on a literal separator, keeping empty fields -- an empty XTX field is
// a real field and dropping it would renumber every field after it.
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
    if (!fs::exists(fs::u8path(filepath))) {
        log_info("  SKIP: " + filepath + " not found");
        return 0;
    }

    const Bytes raw = read_file(filepath);
    auto strict = cp932_to_utf8_strict(raw);
    const std::string content =
        strict ? *strict : cp932_to_utf8_replace(raw.data(), raw.size());

    const std::string fname = fs::u8path(filepath).filename().u8string();
    const std::string backup_path = backup_dir + "\\" + fname;
    if (!fs::exists(fs::u8path(backup_path)))
        fs::copy_file(fs::u8path(filepath), fs::u8path(backup_path));

    std::unordered_map<std::string, std::string> map;
    for (const auto& [jp, en] : translations) map.emplace(jp, en);

    int changes = 0;
    std::vector<std::string> new_lines;
    for (const auto& line : split(content, '\n')) {
        // Keep the CR so CRLF files round-trip byte-for-byte.
        const bool had_cr = !line.empty() && line.back() == '\r';
        std::string stripped = had_cr ? line.substr(0, line.size() - 1) : line;
        if (stripped.find(',') != std::string::npos) {
            auto fields = split(stripped, ',');
            if (static_cast<int>(fields.size()) > name_field) {
                auto it = map.find(fields[static_cast<std::size_t>(name_field)]);
                if (it != map.end()) {
                    fields[static_cast<std::size_t>(name_field)] = it->second;
                    ++changes;
                }
            }
            stripped = join(fields, ',');
        }
        new_lines.push_back(stripped + (had_cr ? "\r" : ""));
    }

    const std::string new_content = join(new_lines, '\n');
    write_file(filepath, utf8_to_cp932_replace(new_content));
    return changes;
}

int run_translate_xtx(const std::string& game_dir) {
    const std::string backup_dir = game_dir + "\\xtx_backup";
    fs::create_directories(fs::u8path(backup_dir));

    log_info("Translating .xtx config files...\n");

    // Character names -- controls the name plate above dialogue.
    const int n = translate_xtx_file(game_dir + "\\spt\\charaname.xtx",
                                     charaname_translations(), backup_dir);
    log_info("  charaname.xtx: " + std::to_string(n) + " changes");

    // BGM/SE lists are cosmetic (gallery/settings only) and stay disabled:
    // the engine crashes on ASCII text in those fields.  See xtx.h.

    log_info("\nBackups saved to: " + backup_dir);
    return 0;
}

}  // namespace mgi::xtx
