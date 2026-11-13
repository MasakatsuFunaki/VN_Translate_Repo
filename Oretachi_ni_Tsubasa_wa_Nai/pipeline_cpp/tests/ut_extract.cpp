// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "common/util.h"
#include "extract/extractor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_leaf)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_leaf)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST(ExtractorTest, RunThrowsWhenScriptLpkMissingInJapaneseDirectory) {
    const ScopedTempDir jp_dir("俺つば_テスト");
    const fs::path missing_game_dir = jp_dir.path() / fs::u8path("ゲーム本体");
    fs::create_directories(missing_game_dir);
    const fs::path out_json = jp_dir.path() / "extracted_text.json";

    const oretsuba::extract::Extractor extractor;
    EXPECT_THROW(
        extractor.Run(missing_game_dir.u8string(), out_json.u8string()),
        std::runtime_error);
    EXPECT_FALSE(fs::exists(out_json));
}

TEST(ExtractorTest, ExtractArchiveThrowsOnInvalidMagicLpkFile) {
    const ScopedTempDir temp_dir("oretsuba_ut_bad_lpk");
    const fs::path bad_lpk = temp_dir.path() / "SCRIPT.LPK";
    const fs::path out_json = temp_dir.path() / "extracted_text.json";

    const oretsuba::Bytes invalid_magic_bytes = {'B', 'A', 'D', '!', 0U, 0U, 0U, 0U};
    oretsuba::WriteFileBytes(bad_lpk.u8string(), invalid_magic_bytes);

    const oretsuba::extract::Extractor extractor;
    EXPECT_THROW(
        extractor.ExtractArchive(bad_lpk.u8string(), out_json.u8string()),
        std::runtime_error);
    EXPECT_FALSE(fs::exists(out_json));
}
