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
#include "scoped_temp_dir.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
namespace tr = mgi::translate;

namespace {

// Four files that are not a cache: nothing at all, a truncated object, and
// two roots that parse cleanly but are not objects.
constexpr const char* kNotACachePayloads[] = {
    "", "{\"unterminated\": ", "[]", "\"text\""};

constexpr int kSampleCacheEntryCount = 7;
constexpr std::size_t kSeededCacheCount = 6u;
constexpr std::size_t kExpectedPurgedCount = 2u;
constexpr std::size_t kExpectedSurvivorCount = 4u;

std::string file_bytes(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

}  // namespace

// The first run of a fresh checkout has no cache, which is not an error.
TEST(CacheIo, an_absent_cache_loads_as_a_first_run) {
    const mgi::test_util::ScopedTempDir temp_dir("mgi_ut_cache_io_absent");
    const std::string path = temp_dir.join_u8("cache.json");
    EXPECT_EQ(tr::load_cache(path).size(), 0u);
}

// Boost's parse error carries an offset and its own header, never the file,
// and a run reads several JSON files -- so the diagnosis names this one.
TEST(CacheIo, a_cache_that_cannot_be_read_names_the_file) {
    const mgi::test_util::ScopedTempDir temp_dir("mgi_ut_cache_io_unreadable");
    const std::string path = temp_dir.join_u8("cache.json");
    for (const char* content : kNotACachePayloads) {
        mgi::write_file(path, std::string(content));
        try {
            tr::load_cache(path);
            ADD_FAILURE() << "loaded a file that is not a cache: [" << content << "]";
        } catch (const std::exception& e) {
            EXPECT_NE(std::string(e.what()).find(path), std::string::npos)
                << "the diagnosis must name the file: " << e.what();
        }
    }
}

// The discard guard counts the same file before a flag that would throw it
// away, and it runs before the banner: an escaping throw would end the run
// with no output at all.  It fails like the load, naming the file.
TEST(CacheIo, the_discard_guard_refuses_a_cache_it_cannot_read) {
    const mgi::test_util::ScopedTempDir temp_dir("mgi_ut_cache_io_guard_unreadable");
    const std::string path = temp_dir.join_u8("cache.json");
    EXPECT_EQ(tr::cache_entry_count(path), 0u);

    for (const char* content : kNotACachePayloads) {
        mgi::write_file(path, std::string(content));
        try {
            tr::cache_entry_count(path);
            ADD_FAILURE() << "counted a file that is not a cache: [" << content << "]";
        } catch (const std::exception& e) {
            EXPECT_NE(std::string(e.what()).find(path), std::string::npos)
                << "the diagnosis must name the file: " << e.what();
        }
    }
}

// The guard reads the cache file itself, so a missing file is not an error and
// a real one is counted rather than estimated.
TEST(CacheIo, the_discard_guard_counts_the_file_on_disk) {
    const mgi::test_util::ScopedTempDir temp_dir("mgi_ut_cache_io_guard_count");
    const std::string path = temp_dir.join_u8("cache.json");
    tr::Cache cache;
    for (int i = 0; i < kSampleCacheEntryCount; ++i) {
        cache.set("日本語" + std::to_string(i), "English " + std::to_string(i));
    }
    tr::save_cache(cache, path);
    EXPECT_EQ(tr::cache_entry_count(path), static_cast<std::size_t>(kSampleCacheEntryCount));
}

// The purge runs against the cache on disk, so what it keeps has to come back
// off disk unchanged.  A translation costs money and nothing in the pipeline
// can rebuild it, so an entry the purge damaged on the way out would be lost
// without a symptom.  Key order is part of the file, not an accident of it.
TEST(CacheIo, a_purge_writes_back_every_entry_it_keeps) {
    const mgi::test_util::ScopedTempDir temp_dir("mgi_ut_cache_io_purge");
    const std::string path = temp_dir.join_u8("cache_purge.json");
    const std::string expected = temp_dir.join_u8("cache_purge.json.expected");

    tr::Cache seed;
    seed.set("朝が来た。", "Morning came.");
    seed.set("こんにちは", "こんにちは");   // echo, goes
    seed.set("OK", "OK");                   // ASCII identity, stays
    seed.set("Eスbg152n", "Eスbg152n");     // echo, goes
    seed.set("bg152n", "bg152n");           // ASCII identifier, stays
    seed.set("雨が降る。", "It is raining.");
    tr::save_cache(seed, path);

    const tr::Cache before = tr::load_cache(path);
    ASSERT_EQ(before.size(), kSeededCacheCount);

    // What the file must hold afterwards: every entry the predicate spares,
    // in the order it already had.
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
