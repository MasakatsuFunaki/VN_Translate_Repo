// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for FVP bytecode extraction JSON schema and field ordering using
// synthetic temporary bytecode buffers.
#include "common/util.h"
#include "extract/extractor.h"
#include "fvp/fvp_hcb.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace fav;

namespace {

constexpr std::uint8_t kPushStringOpcode = 0x0Eu;
constexpr std::uint8_t kNulByte = 0x00u;
constexpr std::size_t kNulTerminatorByteCount = 1u;
constexpr std::uint32_t kScriptHeaderByteCount = 4u;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;
constexpr int kSuccessExitCode = 0;
constexpr std::size_t kExpectedLineCount = 2u;
constexpr std::int64_t kFirstLineIndex = 0;
constexpr std::int64_t kFirstInstructionOffset = 4;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
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

Bytes make_hcb_with_strings(const std::vector<Bytes>& payloads) {
    Bytes body;
    for (const auto& p : payloads) {
        body.push_back(kPushStringOpcode);
        body.push_back(static_cast<std::uint8_t>(p.size() + kNulTerminatorByteCount));
        body.insert(body.end(), p.begin(), p.end());
        body.push_back(kNulByte);
    }
    Bytes out;
    const std::uint32_t len =
        kScriptHeaderByteCount + static_cast<std::uint32_t>(body.size());
    out.push_back(static_cast<std::uint8_t>(len & kByteMask));
    out.push_back(static_cast<std::uint8_t>((len >> kOneByteShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((len >> kTwoBytesShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((len >> kThreeBytesShift) & kByteMask));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

TEST(Extract, writes_schema_valid_json) {
    const ScopedTempDir temp("fav_ut_extract");
    const std::string hcb = (temp.path() / "fav_ut.hcb").u8string();
    const std::string out = (temp.path() / "fav_ut_extracted.json").u8string();
    // 'あ' (JP) then 'ok' (ASCII) -- both kept; the translator stage decides
    // what requires API translation.
    write_file(hcb, make_hcb_with_strings({utf8_to_cp932_replace("あ"), Bytes{'o', 'k'}}));

    ASSERT_EQ(extract::run_extract(hcb, out), kSuccessExitCode);

    const bj::value root = json_parse_file(out);
    ASSERT_TRUE(root.is_object());
    const auto& o = root.get_object();
    ASSERT_TRUE(o.contains("Sakura.hcb"));
    const auto& script = o.at("Sakura.hcb").get_object();
    EXPECT_EQ(script.at("name").get_string(), "Sakura.hcb");
    const auto& lines = script.at("lines").get_array();
    ASSERT_EQ(lines.size(), kExpectedLineCount);

    const auto& l0 = lines[0].get_object();
    EXPECT_EQ(l0.at("idx").get_int64(), kFirstLineIndex);
    EXPECT_EQ(l0.at("type").get_string(), "STRING");
    EXPECT_EQ(l0.at("offset").get_int64(), kFirstInstructionOffset);
    EXPECT_EQ(l0.at("content").get_string(), "あ");
    EXPECT_EQ(lines[1].get_object().at("content").get_string(), "ok");

    // Key order is part of the artefact's contract: the DLL-side tooling and
    // the run-to-run diffs of extracted_text.json both read it positionally.
    std::vector<std::string> keys;
    for (const auto& kv : l0) {
        keys.emplace_back(kv.key());
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"idx", "type", "offset", "content"}));
}
