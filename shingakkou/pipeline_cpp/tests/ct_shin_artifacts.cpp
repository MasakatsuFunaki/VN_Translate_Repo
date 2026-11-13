// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests validating committed repository artifacts (`menu_patch.bin`
// and synced `script_output/translated_text.json` tables).
#include "build_tsv/build_tsv.h"
#include "cache/glossary.h"
#include "common/util.h"
#include "menu/menu_archive_patcher.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace bt = shin::build_tsv;
using shin::Bytes;
using shin::json_parse_file;
using shin::read_file;

constexpr int kExitOk = 0;
constexpr int kMemcmpEqual = 0;
constexpr std::uintmax_t kExpectedGoldenTsvBytes = 6864235U;
constexpr const char* kExpectedGoldenTsvSha256 =
    "a016754c834fe37580d0258cbfdaca9236f1e4c86e35215a0b7dee7880861549";

constexpr std::size_t kByteOffset1 = 1;
constexpr std::size_t kByteOffset2 = 2;
constexpr std::size_t kByteOffset3 = 3;
constexpr std::size_t kByteShift8 = 8;
constexpr std::size_t kByteShift16 = 16;
constexpr std::size_t kByteShift24 = 24;
constexpr unsigned int kNibbleShift = 4;
constexpr unsigned int kLowNibbleMask = 0x0FU;

constexpr std::size_t kPatchVersionOffset = 8;
constexpr std::size_t kPatchSinSysdTotalOffset = 12;
constexpr std::size_t kPatchSinSysdOrigSizeOffset = 16;
constexpr std::size_t kPatchSinSysdCountOffset = 20;
constexpr std::size_t kPatchBootupTotalOffset = 24;
constexpr std::size_t kPatchBootupOrigSizeOffset = 28;
constexpr std::size_t kPatchBootupCountOffset = 32;

constexpr std::uint32_t kExpectedLiveSinSysdTotal = 229;
constexpr std::uint32_t kExpectedLiveSinSysdOrigSize = 13486629;
constexpr std::uint32_t kExpectedLiveSinSysdPatchCount = 37;
constexpr std::uint32_t kExpectedLiveBootupTotal = 18;
constexpr std::uint32_t kExpectedLiveBootupOrigSize = 48447;
constexpr std::uint32_t kExpectedLiveBootupPatchCount = 9;

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

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string Sha256File(const std::string& path) {
    const Bytes raw = read_file(path);
    unsigned char md[EVP_MAX_MD_SIZE] = {};
    unsigned int md_len = 0;
    EVP_Digest(raw.data(), raw.size(), md, &md_len, EVP_sha256(), nullptr);
    static constexpr const char* kHexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(md_len) * kByteOffset2);
    for (unsigned int i = 0; i < md_len; ++i) {
        out += kHexDigits[md[i] >> kNibbleShift];
        out += kHexDigits[md[i] & kLowNibbleMask];
    }
    return out;
}

std::uint32_t ReadU32LeAt(const Bytes& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + kByteOffset1]) << kByteShift8) |
           (static_cast<std::uint32_t>(data[offset + kByteOffset2]) << kByteShift16) |
           (static_cast<std::uint32_t>(data[offset + kByteOffset3]) << kByteShift24);
}

bool ContainsAsciiSubstr(const Bytes& haystack, const std::string& needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
           haystack.end();
}

}  // namespace

TEST(BuildTsv, golden_tsv_matches_reference_sha256) {
    const fs::path translated_path =
        fs::u8path(SHIN_PROJECT_DIR) / fs::u8path("script_output") /
        fs::u8path("translated_text.json");
    ASSERT_TRUE(fs::exists(translated_path))
        << "Missing translated corpus: " << translated_path.u8string();

    const ScopedTempDir scratch("shin_ct_golden");
    const fs::path out_path = scratch.path() / fs::u8path("shin_ct_golden.tsv");
    const std::string out = out_path.u8string();
    ASSERT_EQ(bt::run_build(translated_path.u8string(), out), kExitOk);
    EXPECT_EQ(fs::file_size(out_path), kExpectedGoldenTsvBytes);
    EXPECT_EQ(Sha256File(out), kExpectedGoldenTsvSha256);
}

TEST(Extract, story_order_matches_the_archive_scripts) {
    const fs::path translated_path =
        fs::u8path(SHIN_PROJECT_DIR) / fs::u8path("script_output") /
        fs::u8path("translated_text.json");
    ASSERT_TRUE(fs::exists(translated_path))
        << "Missing translated corpus: " << translated_path.u8string();

    const boost::json::value v = json_parse_file(translated_path.u8string());
    const auto& o = v.get_object();
    EXPECT_EQ(o.size(), shin::translate::story_order().size());
    for (const auto& f : shin::translate::story_order()) {
        EXPECT_TRUE(o.if_contains(f)) << "STORY_ORDER names a missing script: " << f;
    }
}

TEST(XorMenuPatchTest, CommittedMenuPatchBinHasValidHeaderAndZeroPlainHeaders) {
    const fs::path project_root = fs::u8path(SHIN_PROJECT_DIR);
    const fs::path tracked_patch_path = project_root / fs::u8path("menu_patch.bin");
    ASSERT_TRUE(fs::exists(tracked_patch_path));
    EXPECT_FALSE(fs::exists(project_root / fs::u8path("menu_assets")));

    const Bytes patch_blob = read_file(tracked_patch_path.u8string());
    ASSERT_GE(patch_blob.size(), shin::menu::kXorMenuPatchHeaderSize);
    EXPECT_EQ(
        std::memcmp(patch_blob.data(), "SHINMENU", shin::menu::kXorMenuPatchMagicSize),
        kMemcmpEqual);
    EXPECT_EQ(
        ReadU32LeAt(patch_blob, kPatchVersionOffset),
        shin::menu::kXorMenuPatchFormatVersion);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchSinSysdTotalOffset), kExpectedLiveSinSysdTotal);
    EXPECT_EQ(
        ReadU32LeAt(patch_blob, kPatchSinSysdOrigSizeOffset),
        kExpectedLiveSinSysdOrigSize);
    EXPECT_EQ(
        ReadU32LeAt(patch_blob, kPatchSinSysdCountOffset),
        kExpectedLiveSinSysdPatchCount);
    EXPECT_EQ(ReadU32LeAt(patch_blob, kPatchBootupTotalOffset), kExpectedLiveBootupTotal);
    EXPECT_EQ(
        ReadU32LeAt(patch_blob, kPatchBootupOrigSizeOffset),
        kExpectedLiveBootupOrigSize);
    EXPECT_EQ(
        ReadU32LeAt(patch_blob, kPatchBootupCountOffset),
        kExpectedLiveBootupPatchCount);

    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "DDWuHXB"));
    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "T:Shingakkou"));
    EXPECT_FALSE(ContainsAsciiSubstr(patch_blob, "rep_01_01"));
}
