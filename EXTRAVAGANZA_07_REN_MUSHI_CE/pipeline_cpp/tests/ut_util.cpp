// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/common/util.{h,cpp}: file IO, Unicode trimming,
// Japanese character counting, comma formatting, and JSON serialization.
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;

namespace {

constexpr int kExpectedThreeJpChars = 3;
constexpr int kExpectedFiveJpChars = 5;
constexpr long long kSubThousandNumber = 999;
constexpr long long kOneThousandNumber = 1000;
constexpr long long kSampleLargeNumber = 323299;
constexpr long long kSampleNegativeNumber = -1234567;
constexpr int kTwoSpaceIndent = 2;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST(Util, trim_removes_ascii_and_ideographic_spaces) {
    EXPECT_EQ(trim("  hello  "), "hello");
    EXPECT_EQ(trim("　　アゲハ　　"), "アゲハ");
    EXPECT_EQ(trim_left("　\t アゲハ "), "アゲハ ");
    EXPECT_EQ(trim_right(" アゲハ \r\n　"), " アゲハ");
    EXPECT_EQ(trim("   　\t\r\n"), "");
}

TEST(Util, jp_char_count_counts_kana_and_cjk_only) {
    EXPECT_EQ(jp_char_count("アゲハ"), kExpectedThreeJpChars);
    EXPECT_EQ(jp_char_count("九条アゲハ"), kExpectedFiveJpChars);
    EXPECT_EQ(jp_char_count("Ageha 123!"), 0);
    EXPECT_TRUE(is_jp("シュミ"));
    EXPECT_FALSE(is_jp("Shumi"));
}

TEST(Util, comma_formats_thousands_separators) {
    EXPECT_EQ(comma(0), "0");
    EXPECT_EQ(comma(kSubThousandNumber), "999");
    EXPECT_EQ(comma(kOneThousandNumber), "1,000");
    EXPECT_EQ(comma(kSampleLargeNumber), "323,299");
    EXPECT_EQ(comma(kSampleNegativeNumber), "-1,234,567");
}

TEST(Util, scoped_file_manages_file_lifetime_and_move) {
    const ScopedTempDir tmp("agh_ut_scoped_file");
    const std::string p = (tmp.path() / "scoped.bin").u8string();
    {
        ScopedFile out(p, "wb");
        ASSERT_TRUE(static_cast<bool>(out));
        ASSERT_NE(out.get(), nullptr);
        const char payload[] = "ageha";
        EXPECT_EQ(std::fwrite(payload, 1, sizeof(payload) - 1, out.get()), sizeof(payload) - 1);

        ScopedFile moved(std::move(out));
        EXPECT_FALSE(static_cast<bool>(out));
        EXPECT_TRUE(static_cast<bool>(moved));
    }
    const Bytes raw = read_file(p);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()), "ageha");

    const ScopedFile missing((tmp.path() / "nonexistent_dir" / "missing.bin").u8string(), "rb");
    EXPECT_FALSE(static_cast<bool>(missing));
}

TEST(Util, write_and_read_file_round_trips_bytes_and_text) {
    const ScopedTempDir tmp("agh_ut_util_io");
    const std::string p = (tmp.path() / "io.txt").u8string();
    write_file_atomic_text(p, "line1\nline2\r\nline3");
    const Bytes raw = read_file(p);
    const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    EXPECT_EQ(text, "line1\r\nline2\r\nline3");
}

TEST(Util, json_pretty_and_dump_preserve_raw_utf8) {
    bj::object obj;
    obj["speaker"] = "アゲハ";
    obj["lines"] = bj::array{"シュミ", "史郎"};

    const std::string dumped = json_dump(bj::value(obj));
    EXPECT_NE(dumped.find("\"speaker\": \"アゲハ\""), std::string::npos);
    EXPECT_NE(dumped.find("[\"シュミ\", \"史郎\"]"), std::string::npos);

    const std::string pretty = json_pretty(bj::value(obj), kTwoSpaceIndent);
    EXPECT_NE(pretty.find("  \"speaker\": \"アゲハ\""), std::string::npos);
}
