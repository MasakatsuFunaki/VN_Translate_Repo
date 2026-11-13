// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Reading the translation cache off disk.  An absent cache is a first run; a
// cache that is there and cannot be read stops the run with a message naming
// it, because the caller is usually about to delete or overwrite that file.
#include "cache/cache_core.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = crc::translate;

namespace {

constexpr int kSampleEntryCount = 7;
constexpr std::size_t kExpectedSurvivorCount = 4u;
constexpr std::size_t kExpectedPurgedCount = 2u;
constexpr std::size_t kExpectedSeedCount = 6u;
constexpr int kIndentOne = 1;

const char* const kNotACache[] = {"", "{\"unterminated\": ", "[]", "\"text\""};

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name =
            "crc_ut_cache_io_" + std::to_string(++seq);
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

std::string file_bytes(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST(CacheIo, AnAbsentCacheLoadsAsAFirstRun) {
    ScopedTempDir scratch;
    const std::string path = (scratch.path() / "absent_cache.json").u8string();
    EXPECT_EQ(tr::load_cache(path).size(), 0u);
}

TEST(CacheIo, ACacheThatCannotBeReadNamesTheFile) {
    ScopedTempDir scratch;
    const std::string path = (scratch.path() / "corrupt_cache.json").u8string();
    for (const char* content : kNotACache) {
        crc::write_file(path, std::string(content));
        try {
            tr::load_cache(path);
            ADD_FAILURE() << "loaded a file that is not a cache: [" << content << "]";
        } catch (const std::exception& e) {
            EXPECT_NE(std::string(e.what()).find(path), std::string::npos)
                << "the diagnosis must name the file: " << e.what();
        }
    }
}

TEST(CacheIo, TheDiscardGuardRefusesACacheItCannotRead) {
    ScopedTempDir scratch;
    const std::string path = (scratch.path() / "discard_guard.json").u8string();
    EXPECT_EQ(tr::cache_entry_count(path), 0u);

    for (const char* content : kNotACache) {
        crc::write_file(path, std::string(content));
        try {
            tr::cache_entry_count(path);
            ADD_FAILURE() << "counted a file that is not a cache: [" << content << "]";
        } catch (const std::exception& e) {
            EXPECT_NE(std::string(e.what()).find(path), std::string::npos)
                << "the diagnosis must name the file: " << e.what();
        }
    }
}

TEST(CacheIo, TheDiscardGuardCountsTheFileOnDisk) {
    ScopedTempDir scratch;
    const std::string path = (scratch.path() / "counted_cache.json").u8string();
    tr::Cache cache;
    for (int i = 0; i < kSampleEntryCount; ++i) {
        cache.set("日本語" + std::to_string(i), "English " + std::to_string(i));
    }
    tr::save_cache(cache, path);
    EXPECT_EQ(tr::cache_entry_count(path), static_cast<std::size_t>(kSampleEntryCount));
}

TEST(CacheIo, APurgeWritesBackEveryEntryItKeeps) {
    ScopedTempDir scratch;
    const std::string path = (scratch.path() / "purge_cache.json").u8string();
    const std::string expected = (scratch.path() / "purge_cache.expected.json").u8string();

    tr::Cache seed;
    seed.set("朝が来た。", "Morning came.");
    seed.set("こんにちは", "こんにちは");
    seed.set("OK", "OK");
    seed.set("Eスbg152n", "Eスbg152n");
    seed.set("bg152n", "bg152n");
    seed.set("雨が降る。", "It is raining.");
    tr::save_cache(seed, path);

    const tr::Cache before = tr::load_cache(path);
    ASSERT_EQ(before.size(), kExpectedSeedCount);

    tr::Cache survivors;
    for (const auto& [jp, en] : before.items()) {
        if (!tr::is_failed_entry(jp, en)) {
            survivors.set(jp, en);
        }
    }
    tr::save_cache(survivors, expected);

    tr::Cache purged = tr::load_cache(path);
    EXPECT_EQ(tr::purge_failed_entries(purged), kExpectedPurgedCount);
    tr::save_cache(purged, path);

    EXPECT_EQ(file_bytes(path), file_bytes(expected));

    const tr::Cache reloaded = tr::load_cache(path);
    ASSERT_EQ(reloaded.size(), kExpectedSurvivorCount);
    EXPECT_EQ(reloaded.items(), survivors.items());
    ASSERT_TRUE(reloaded.get("朝が来た。"));
    EXPECT_EQ(*reloaded.get("朝が来た。"), "Morning came.");
    EXPECT_FALSE(reloaded.contains("こんにちは"));
    EXPECT_FALSE(reloaded.contains("Eスbg152n"));
}

TEST(CacheIo, JsonPrettyIndentOneWithWriteFileTextRoundTripsWithCrlf) {
    ScopedTempDir scratch;
    bj::object entry;
    entry["offset"] = 12345;
    entry["type"] = "dialogue";
    entry["speaker"] = "太一";
    entry["text"] = "「おはよう」";

    bj::object sn_block;
    sn_block["strings"] = bj::array{entry};
    bj::object root;
    root["sn.bin"] = sn_block;

    const std::string out_path = (scratch.path() / "synthetic_extracted.json").u8string();
    crc::write_file_text(out_path, crc::json_pretty(bj::value(root), kIndentOne));

    const std::string raw = file_bytes(out_path);
    EXPECT_EQ(raw.rfind("{\r\n \"sn.bin\": {\r\n  \"strings\": [\r\n   {\r\n", 0), 0u);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n') {
            ASSERT_GT(i, 0u);
            EXPECT_EQ(raw[i - 1], '\r');
        }
    }
    const bj::value parsed = crc::json_parse_file(out_path);
    EXPECT_EQ(parsed, bj::value(root));
}
