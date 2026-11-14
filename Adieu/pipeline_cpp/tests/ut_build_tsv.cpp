// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for Adieu TSV builder: escaping order, first-wins conflict
// resolution, exclusion of `other`/`empty` and `#NAMAE` macro keys (`＊Ａ`/`＊Ｂ`),
// UTF-8 LF-only formatting, and `02_build_tsv` CLI flag semantics.
#include "build_tsv/build_tsv.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace adi;
using namespace adi::build_tsv;

namespace {

constexpr int kJsonIndent = 1;
constexpr int kBuildSuccessExitCode = 0;
constexpr std::size_t kUtf8BomLen = 3;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name =
            "adi_ut_tsv_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return dir_; }

private:
    fs::path dir_;
};

bj::object entry(const std::string& jp, const std::string& en, const std::string& type) {
    bj::object o;
    o["text"] = jp;
    o["type"] = type;
    o["translated"] = en;
    return o;
}

void write_translated_doc(const fs::path& path, std::initializer_list<bj::object> entries) {
    bj::array arr;
    for (const auto& e : entries) {
        arr.push_back(e);
    }
    bj::object scene;
    scene["strings"] = std::move(arr);
    bj::object root;
    root["s"] = std::move(scene);
    fs::create_directories(path.parent_path());
    write_file_text(path.u8string(), json_pretty(bj::value(std::move(root)), kJsonIndent));
}

std::string read_text_bytes(const fs::path& p) {
    const Bytes raw = read_file(p.u8string());
    return std::string(raw.begin(), raw.end());
}

std::string run(std::initializer_list<bj::object> entries, const char* tag) {
    ScopedTempDir scratch;
    const fs::path in = scratch.path() / (std::string("in_") + tag + ".json");
    const fs::path out = scratch.path() / (std::string("out_") + tag + ".tsv");
    write_translated_doc(in, entries);
    EXPECT_EQ(run_build(in.u8string(), out.u8string()), kBuildSuccessExitCode);
    return read_text_bytes(out);
}

}  // namespace

TEST(BuildTsv, EscapeOrderBackslashFirst) {
    EXPECT_EQ(escape_for_tsv("a\\nb"), "a\\\\nb");
    EXPECT_EQ(escape_for_tsv("a\tb"), "a\\tb");
    EXPECT_EQ(escape_for_tsv("a\r\nb"), "a\\r\\nb");
    EXPECT_EQ(escape_for_tsv("plain"), "plain");
}

TEST(BuildTsv, FirstWinsAndConflictCount) {
    const std::string out = run({entry("jp", "first", "dialogue"),
                                 entry("jp", "second", "dialogue"),
                                 entry("jp", "first", "narrative")},
                                "dedupe");
    EXPECT_EQ(out, "jp\tfirst\n");
}

TEST(BuildTsv, SkipsNonTranslatableTypesIdentityRowsAndNamaeMacros) {
    const std::string out = run({entry("a", "A", "other"),
                                 entry("b", "B", "empty"),
                                 entry("＊Ａ", "Hitomi", "name"),
                                 entry("＊Ｂ", "Kousuke", "dialogue"),
                                 entry("", "C", "dialogue"),
                                 entry("d", "", "dialogue"),
                                 entry("e", "e", "dialogue"),
                                 entry("人見", "Hitomi", "name"),
                                 entry("教室", "Classroom", "menu")},
                                "skips");
    EXPECT_EQ(out, "人見\tHitomi\n教室\tClassroom\n");
}

TEST(BuildTsv, OutputIsUtf8LfNoBom) {
    const std::string jp = "睦　月";
    const std::string out = run({entry(jp, "Mutsuki", "name")}, "utf8");
    EXPECT_EQ(out, jp + "\tMutsuki\n");
    EXPECT_EQ(out.find('\r'), std::string::npos);
    EXPECT_NE(out.compare(0, kUtf8BomLen, "\xEF\xBB\xBF"), 0);
}

TEST(BuildTsv, CliFlagsOutIsolatesStagedInstallAndGameDirSyncsBoth) {
    ScopedTempDir scratch;
    const fs::path proj = scratch.path() / "Adieu";
    const fs::path install_game = proj / "build" / "install" / "game";
    const fs::path game_dir = scratch.path() / "GameData";
    const fs::path custom_out = scratch.path() / "custom.tsv";
    fs::create_directories(install_game);
    fs::create_directories(game_dir);

    const fs::path staged_tsv = install_game / "translation_table.tsv";
    write_file(staged_tsv.u8string(), std::string("UNTOUCHED_STAGE_SENTINEL\n"));

    write_translated_doc(proj / "script_output" / "translated_text.json",
                         {entry("人見", "Hitomi", "name"),
                          entry("　夢だ。", "It's a dream.", "narrative")});

    // 1. `--rebuild-table --dir <proj> --out <custom_out>` must write ONLY
    // `<custom_out>` and leave `build/install/game/translation_table.tsv` untouched.
    ASSERT_EQ(run_build_tsv_args(
                  {"--rebuild-table", "--dir", proj.u8string(), "--out", custom_out.u8string()}),
              kBuildSuccessExitCode);
    EXPECT_EQ(read_text_bytes(custom_out), "人見\tHitomi\n　夢だ。\tIt's a dream.\n");
    EXPECT_EQ(read_text_bytes(staged_tsv), "UNTOUCHED_STAGE_SENTINEL\n");
    EXPECT_FALSE(fs::exists(proj / "translation_table.tsv"));

    // 2. `--rebuild-table --dir <proj> --game-dir <game_dir>` writes
    // `<proj>/translation_table.tsv`, syncs `build/install/game/translation_table.tsv`,
    // and copies `<game_dir>/translation_table.tsv`.
    ASSERT_EQ(run_build_tsv_args(
                  {"--rebuild-table", "--dir", proj.u8string(), "--game-dir", game_dir.u8string()}),
              kBuildSuccessExitCode);
    const std::string expected = "人見\tHitomi\n　夢だ。\tIt's a dream.\n";
    EXPECT_EQ(read_text_bytes(proj / "translation_table.tsv"), expected);
    EXPECT_EQ(read_text_bytes(staged_tsv), expected);
    EXPECT_EQ(read_text_bytes(game_dir / "translation_table.tsv"), expected);

    // 3. Unknown CLI flag or missing `translated_text.json` fails cleanly with non-zero code.
    EXPECT_NE(run_build_tsv_args({"--unknown-flag", "--dir", proj.u8string()}),
              kBuildSuccessExitCode);
    EXPECT_NE(run_build_tsv_args({"--dir", (scratch.path() / "missing_proj").u8string()}),
              kBuildSuccessExitCode);
}
