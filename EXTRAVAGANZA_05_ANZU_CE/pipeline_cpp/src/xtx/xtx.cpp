// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "xtx.h"

#include <algorithm>
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

const std::vector<std::pair<std::string, std::string>>& charaname_translations() {
    static const std::vector<std::pair<std::string, std::string>> NAME_TRANSLATIONS = {
        {"名前なし", ""},
        {"夢美", "Ｙｕｍｅｍｉ"},
        {"アゲハ", "Ａｇｅｈａ"},
        {"客Ａ", "Ｃｕｓｔｏｍｅｒ　Ａ"},
        {"客Ｂ", "Ｃｕｓｔｏｍｅｒ　Ｂ"},
        {"客Ｃ", "Ｃｕｓｔｏｍｅｒ　Ｃ"},
        {"客Ｄ", "Ｃｕｓｔｏｍｅｒ　Ｄ"},
        {"客Ｅ", "Ｃｕｓｔｏｍｅｒ　Ｅ"},
        {"客Ｆ", "Ｃｕｓｔｏｍｅｒ　Ｆ"},
        {"客Ｇ", "Ｃｕｓｔｏｍｅｒ　Ｇ"},
        {"客Ｈ", "Ｃｕｓｔｏｍｅｒ　Ｈ"},
        {"客Ｉ", "Ｃｕｓｔｏｍｅｒ　Ｉ"},
        {"客達", "Ｃｕｓｔｏｍｅｒｓ"},
        {"老婆Ａ", "Ｏｌｄ　Ｗｏｍａｎ　Ａ"},
        {"老婆Ｂ", "Ｏｌｄ　Ｗｏｍａｎ　Ｂ"},
        {"老人", "Ｏｌｄ　Ｍａｎ"},
        {"通行人Ａ", "Ｐａｓｓｅｒｂｙ　Ａ"},
        {"通行人Ｂ", "Ｐａｓｓｅｒｂｙ　Ｂ"},
        {"通行人Ｃ", "Ｐａｓｓｅｒｂｙ　Ｃ"},
        {"通行人Ｄ", "Ｐａｓｓｅｒｂｙ　Ｄ"},
        {"？？？", "？？？"},
        {"サユリ", "Ｓａｙｕｒｉ"},
        {"綾佳", "Ａｙａｋａ"},
        {"遥", "Ｈａｒｕｋａ"},
        {"杏子", "Ａｎｚｕ"},
        {"ユーリア", "Ｙｕｌｉａ"},
        {"美弥香", "Ｍｉｙａｋａ"},
        {"唯", "Ｙｕｉ"},
        {"煉悟", "Ｒｅｎｇｏ"},
        {"西", "Ｎｉｓｈｉ"},
        {"レン", "Ｒｅｎ"},
        {"優斗", "Ｙｕｕｔｏ"},
        {"史郎", "Ｓｈｉｒｏｕ"},
        {"煉獄", "Ｒｅｎｇｏｋｕ"},
        {"合蟲人間", "Ｉｎｓｅｃｔ　Ｈｙｂｒｉｄ"},
        {"客", "Ｃｕｓｔｏｍｅｒ"},
        {"男の声", "Ｍａｎ’ｓ　Ｖｏｉｃｅ"},
        {"夢美・アゲハ", "Ｙｕｍｅｍｉ　＆　Ａｇｅｈａ"},
        {"列席者達", "Ａｔｔｅｎｄｅｅｓ"},
        {"男Ａ", "Ｍａｎ　Ａ"},
        {"男Ｂ", "Ｍａｎ　Ｂ"},
        {"男Ｃ", "Ｍａｎ　Ｃ"},
        {"男Ｄ", "Ｍａｎ　Ｄ"},
        {"男Ｅ", "Ｍａｎ　Ｅ"},
        {"男Ｆ", "Ｍａｎ　Ｆ"},
        {"男Ｇ", "Ｍａｎ　Ｇ"},
        {"男Ｈ", "Ｍａｎ　Ｈ"},
        {"男Ｉ", "Ｍａｎ　Ｉ"},
        {"男Ｊ", "Ｍａｎ　Ｊ"},
        {"列席者Ａ", "Ａｔｔｅｎｄｅｅ　Ａ"},
        {"列席者Ｂ", "Ａｔｔｅｎｄｅｅ　Ｂ"},
        {"列席者Ｃ", "Ａｔｔｅｎｄｅｅ　Ｃ"},
        {"列席者Ｄ", "Ａｔｔｅｎｄｅｅ　Ｄ"},
        {"列席者Ｅ", "Ａｔｔｅｎｄｅｅ　Ｅ"},
        {"列席者Ｆ", "Ａｔｔｅｎｄｅｅ　Ｆ"},
        {"列席者Ｇ", "Ａｔｔｅｎｄｅｅ　Ｇ"},
        {"列席者Ｈ", "Ａｔｔｅｎｄｅｅ　Ｈ"},
        {"列席者Ｉ", "Ａｔｔｅｎｄｅｅ　Ｉ"},
        {"列席者Ｊ", "Ａｔｔｅｎｄｅｅ　Ｊ"},
        {"列席者Ｋ", "Ａｔｔｅｎｄｅｅ　Ｋ"},
        {"列席者Ｌ", "Ａｔｔｅｎｄｅｅ　Ｌ"},
        {"列席者Ｍ", "Ａｔｔｅｎｄｅｅ　Ｍ"},
        {"列席者Ｎ", "Ａｔｔｅｎｄｅｅ　Ｎ"},
        {"列席者Ｏ", "Ａｔｔｅｎｄｅｅ　Ｏ"},
        {"列席者Ｐ", "Ａｔｔｅｎｄｅｅ　Ｐ"},
    };
    return NAME_TRANSLATIONS;
}

std::vector<std::uint8_t> decode_xtx(const std::vector<std::uint8_t>& raw) {
    const auto& dec = xtx_decode_table();
    std::vector<std::uint8_t> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = dec[raw[i]];
    }
    return out;
}

std::vector<std::uint8_t> encode_xtx(const std::vector<std::uint8_t>& plain_cp932) {
    const auto& enc = xtx_encode_table();
    std::vector<std::uint8_t> out(plain_cp932.size());
    for (std::size_t i = 0; i < plain_cp932.size(); ++i) {
        out[i] = enc[plain_cp932[i]];
    }
    return out;
}

bool is_plain_cp932_xtx(const std::vector<std::uint8_t>& raw) {
    if (raw.empty()) return true;
    const bool has_comma = std::find(raw.begin(), raw.end(), static_cast<std::uint8_t>(',')) != raw.end();
    const bool has_lf = std::find(raw.begin(), raw.end(), static_cast<std::uint8_t>('\n')) != raw.end();
    if (!has_comma || !has_lf) return false;
    return cp932_to_utf8_strict(raw).has_value();
}

int translate_xtx_file(const std::string& filepath,
                       const std::vector<std::pair<std::string, std::string>>& translations,
                       const std::string& backup_dir, int name_field) {
    if (!fs::exists(fs::u8path(filepath))) {
        log_info("  SKIP: " + filepath + " not found");
        return 0;
    }

    fs::create_directories(fs::u8path(backup_dir));
    const std::string fname = fs::u8path(filepath).filename().u8string();
    const std::string backup_path = backup_dir + "\\" + fname;
    const std::string nested_backup_path = backup_dir + "\\spt\\" + fname;
    if (!fs::exists(fs::u8path(backup_path))) {
        if (fs::exists(fs::u8path(nested_backup_path))) {
            fs::copy_file(fs::u8path(nested_backup_path), fs::u8path(backup_path));
        } else {
            fs::copy_file(fs::u8path(filepath), fs::u8path(backup_path));
        }
    }
    if (!fs::exists(fs::u8path(nested_backup_path))) {
        fs::create_directories(fs::u8path(backup_dir + "\\spt"));
        fs::copy_file(fs::u8path(backup_path), fs::u8path(nested_backup_path));
    }

    const Bytes raw = read_file(backup_path);
    const bool plain_format = is_plain_cp932_xtx(raw);
    const Bytes plain_cp932 = plain_format ? raw : decode_xtx(raw);
    const auto strict = cp932_to_utf8_strict(plain_cp932);
    const std::string content =
        strict ? *strict : cp932_to_utf8_replace(plain_cp932.data(), plain_cp932.size());

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
    const Bytes encoded_cp932 = utf8_to_cp932_replace(new_content);
    write_file(filepath, plain_format ? encoded_cp932 : encode_xtx(encoded_cp932));
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
