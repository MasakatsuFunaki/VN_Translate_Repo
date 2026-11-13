// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// TSV builder unit tests: RAW key/value reconstruction (the runtime lookup
// keys) and the utf-8-sig + CRLF output contract.
#include "build_tsv/build_tsv.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace bj = boost::json;
namespace bt = shin::build_tsv;
using namespace shin;

namespace {

constexpr int kExitOk = 0;
constexpr int kDefaultOffset = 0;
constexpr std::size_t kUtf8BomBytes = 3;
constexpr std::size_t kCrlfBytes = 2;
constexpr std::uint8_t kUtf8BomByte0 = 0xEFu;
constexpr std::uint8_t kUtf8BomByte1 = 0xBBu;
constexpr std::uint8_t kUtf8BomByte2 = 0xBFu;
constexpr std::size_t kExpectedTypeRowCount = 5u;
constexpr std::size_t kExpectedDedupRowCount = 1u;
constexpr std::ptrdiff_t kExpectedSingleTabPerRow = 1;
constexpr std::size_t kRowIdx0 = 0u;
constexpr std::size_t kRowIdx1 = 1u;
constexpr std::size_t kRowIdx2 = 2u;
constexpr std::size_t kRowIdx3 = 3u;
constexpr std::size_t kRowIdx4 = 4u;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name_utf8)
        : path_(fs::temp_directory_path() / fs::u8path(name_utf8)) {
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

    std::string file_u8(std::string_view file_name_utf8) const {
        return (path_ / fs::u8path(file_name_utf8)).u8string();
    }

private:
    fs::path path_;
};

bj::object mk(const char* type, const char* speaker, const char* text, const char* en) {
    bj::object o;
    o["offset"] = kDefaultOffset;
    o["type"] = type;
    o["speaker"] = speaker;
    o["text"] = text;
    if (en != nullptr) {
        o["translated"] = en;
    }
    return o;
}

std::string write_fixture(const ScopedTempDir& dir,
                          std::string_view name,
                          bj::array strings) {
    bj::object fd;
    fd["strings"] = std::move(strings);
    bj::object root;
    root["main05"] = std::move(fd);
    const std::string path = dir.file_u8(name);
    write_file(path, bj::serialize(bj::value(std::move(root))));
    return path;
}

std::vector<std::string> tsv_rows(const std::string& path) {
    const Bytes raw = read_file(path);
    EXPECT_GE(raw.size(), kUtf8BomBytes) << "missing utf-8-sig BOM";
    if (raw.size() < kUtf8BomBytes) {
        return {};
    }
    EXPECT_EQ(raw[kRowIdx0], kUtf8BomByte0) << "missing utf-8-sig BOM";
    EXPECT_EQ(raw[kRowIdx1], kUtf8BomByte1) << "missing utf-8-sig BOM";
    EXPECT_EQ(raw[kRowIdx2], kUtf8BomByte2) << "missing utf-8-sig BOM";
    std::string s(
        reinterpret_cast<const char*>(raw.data() + kUtf8BomBytes),
        raw.size() - kUtf8BomBytes);
    std::vector<std::string> rows;
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t nl = s.find("\r\n", pos);
        if (nl == std::string::npos) {
            break;
        }
        rows.push_back(s.substr(pos, nl - pos));
        pos = nl + kCrlfBytes;
    }
    EXPECT_EQ(pos, s.size()) << "trailing bytes after the last CRLF";
    return rows;
}

}  // namespace

TEST(BuildTsv, escape_covers_tab_cr_lf_only) {
    EXPECT_EQ(bt::escape_for_tsv("a\tb"), "a{TAB}b");
    EXPECT_EQ(bt::escape_for_tsv("a\rb"), "a{CR}b");
    EXPECT_EQ(bt::escape_for_tsv("a\nb"), "a{LF}b");
    // A literal backslash-n is two ordinary characters and must survive: it is
    // the separator every dialogue and narration key is built around.
    EXPECT_EQ(bt::escape_for_tsv("Michael\\nHello"), "Michael\\nHello");
}

TEST(BuildTsv, raw_form_reconstruction_per_type) {
    const ScopedTempDir scratch("shin_ut_bt1");
    const std::string in = write_fixture(scratch, "in.json", bj::array{
        mk("choice", "", "屋上", "Go up to the roof"),
        mk("dialogue", "マイケル", "「あ」", "\"Ah\""),
        mk("narration", "", "そこには", "There was"),
        // A dialogue record whose speaker never resolved falls into the
        // narration branch, prefix and all.
        mk("dialogue", "", "誰か", "Someone"),
        // An unknown speaker passes through untranslated on the EN side.
        mk("dialogue", "知らない人", "「い」", "\"Ee\""),
    });
    const std::string out = scratch.file_u8("out.tsv");
    ASSERT_EQ(bt::run_build(in, out), kExitOk);

    const auto rows = tsv_rows(out);
    ASSERT_EQ(rows.size(), kExpectedTypeRowCount);
    EXPECT_EQ(rows[kRowIdx0], "屋上\tGo up to the roof");
    EXPECT_EQ(rows[kRowIdx1], "マイケル\\n「あ」\tMichael\\n\"Ah\"");
    EXPECT_EQ(rows[kRowIdx2], "\\nそこには\t\\nThere was");
    EXPECT_EQ(rows[kRowIdx3], "\\n誰か\t\\nSomeone");
    EXPECT_EQ(rows[kRowIdx4], "知らない人\\n「い」\t知らない人\\n\"Ee\"");
}

TEST(BuildTsv, identical_rows_skipped_and_duplicates_deduped_first_wins) {
    const ScopedTempDir scratch("shin_ut_bt2");
    const std::string in = write_fixture(scratch, "in.json", bj::array{
        mk("choice", "", "帰る", "Go home"),
        mk("choice", "", "帰る", "Return"),          // duplicate key -> first wins
        mk("choice", "", "ABC", "ABC"),              // jp_raw == en_raw -> skipped
        mk("narration", "", "XYZ", "XYZ"),           // "\nXYZ" both sides -> skipped
        mk("choice", "", "untranslated", nullptr),   // no EN -> skipped silently
    });
    const std::string out = scratch.file_u8("out.tsv");
    ASSERT_EQ(bt::run_build(in, out), kExitOk);

    const auto rows = tsv_rows(out);
    ASSERT_EQ(rows.size(), kExpectedDedupRowCount);
    EXPECT_EQ(rows[kRowIdx0], "帰る\tGo home");
}

// The single most likely silent break in this port: it is the only utf-8-sig
// writer in the repo, and losing either half means every runtime lookup misses.
TEST(BuildTsv, output_has_bom_and_crlf_and_one_tab_per_row) {
    const ScopedTempDir scratch("shin_ut_bt3");
    const std::string in = write_fixture(scratch, "in.json", bj::array{
        mk("choice", "", "あ", "one"),
        mk("choice", "", "い", "two"),
    });
    const std::string out = scratch.file_u8("out.tsv");
    ASSERT_EQ(bt::run_build(in, out), kExitOk);

    const Bytes raw = read_file(out);
    ASSERT_GE(raw.size(), kUtf8BomBytes);
    EXPECT_EQ(raw[kRowIdx0], kUtf8BomByte0);
    EXPECT_EQ(raw[kRowIdx1], kUtf8BomByte1);
    EXPECT_EQ(raw[kRowIdx2], kUtf8BomByte2);
    // No bare LF anywhere.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n') {
            EXPECT_TRUE(i > 0 && raw[i - 1] == '\r') << "bare LF at " << i;
        }
    }
    for (const auto& row : tsv_rows(out)) {
        EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), kExpectedSingleTabPerRow)
            << row;
    }
}
