// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests running against a live Shingakkou installation pointed to by
// `VN_GAME_DIR`.  Skipped cleanly when `VN_GAME_DIR` is unset or missing.
#include "common/util.h"
#include "extract/extractor.h"
#include "menu/menu_archive_patcher.h"

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace ex = shin::extract;
using shin::Bytes;
using shin::read_file;

constexpr int kExitOk = 0;
constexpr std::size_t kHexCharsPerByte = 2;
constexpr unsigned int kNibbleShift = 4;
constexpr unsigned int kLowNibbleMask = 0x0FU;

constexpr std::uintmax_t kExpectedGoldenExtractBytes = 9474419U;
constexpr const char* kExpectedGoldenExtractSha256 =
    "423a35d7079104e10af1332b92f34e29281dc2d8fc184d3fd06e5dec2d933d5f";

constexpr std::size_t kExpectedLiveSinSysdPatchedSize = 20928356;
constexpr std::size_t kExpectedLiveBootupPatchedSize = 58961;
constexpr const char* kExpectedLiveSinSysdSha256 =
    "ba84988290db3c5341dcfb2fc70e1087b4947c75b807ef3a629d1ea4e4587417";
constexpr const char* kExpectedLiveBootupSha256 =
    "3810178413ab31c8d1355d2b9d84b6e3ecaf69a81924bcda1dd65a671e2a4977";

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

std::string ComputeSha256Hex(const Bytes& raw) {
    unsigned char md[EVP_MAX_MD_SIZE] = {};
    unsigned int md_len = 0;
    EVP_Digest(raw.data(), raw.size(), md, &md_len, EVP_sha256(), nullptr);
    static constexpr const char* kHexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(md_len) * kHexCharsPerByte);
    for (unsigned int i = 0; i < md_len; ++i) {
        out += kHexDigits[md[i] >> kNibbleShift];
        out += kHexDigits[md[i] & kLowNibbleMask];
    }
    return out;
}

}  // namespace

TEST(Extract, golden_extract_is_pinned_by_sha256) {
    const wchar_t* env_game_dir = _wgetenv(L"VN_GAME_DIR");
    if (env_game_dir == nullptr || env_game_dir[0] == L'\0') {
        GTEST_SKIP() << "VN_GAME_DIR not set";
    }
    const std::string game = fs::path(env_game_dir).u8string();
    if (!fs::exists(fs::u8path(ex::resolve_source_file(game)))) {
        GTEST_SKIP() << "game not installed";
    }

    const ScopedTempDir scratch("shin_st_extract");
    const fs::path out_path = scratch.path() / fs::u8path("shin_st_extracted.json");
    const std::string out = out_path.u8string();
    ASSERT_EQ(ex::run_extract(game, out), kExitOk);
    EXPECT_EQ(fs::file_size(out_path), kExpectedGoldenExtractBytes);
    EXPECT_EQ(ComputeSha256Hex(read_file(out)), kExpectedGoldenExtractSha256);
}

TEST(XorMenuPatchTest, LiveMenuPatchBinReconstructsBitExactArchives) {
    const wchar_t* env_game_dir = _wgetenv(L"VN_GAME_DIR");
    if (env_game_dir == nullptr || env_game_dir[0] == L'\0') {
        GTEST_SKIP() << "VN_GAME_DIR not set";
    }
    const fs::path live_game_root(env_game_dir);
    const fs::path live_sin_sysd_bak =
        live_game_root / fs::u8path("Data") / fs::u8path("sin_sysd.dat.bak");
    const fs::path live_bootup_bak = live_game_root / fs::u8path("Bootup.dat.bak");
    if (!fs::exists(live_sin_sysd_bak) || !fs::exists(live_bootup_bak)) {
        GTEST_SKIP() << "live backup archives not found";
    }

    const fs::path tracked_patch_path =
        fs::u8path(SHIN_PROJECT_DIR) / fs::u8path("menu_patch.bin");
    ASSERT_TRUE(fs::exists(tracked_patch_path));
    const Bytes patch_blob = read_file(tracked_patch_path.u8string());

    const Bytes orig_sin_sysd = read_file(live_sin_sysd_bak.u8string());
    const Bytes orig_bootup = read_file(live_bootup_bak.u8string());
    Bytes patched_sin_sysd;
    Bytes patched_bootup;
    shin::menu::ApplyXorMenuPatchBlobToDdp2(
        orig_sin_sysd, orig_bootup, patch_blob, patched_sin_sysd, patched_bootup);

    EXPECT_EQ(patched_sin_sysd.size(), kExpectedLiveSinSysdPatchedSize);
    EXPECT_EQ(ComputeSha256Hex(patched_sin_sysd), kExpectedLiveSinSysdSha256);
    EXPECT_EQ(patched_bootup.size(), kExpectedLiveBootupPatchedSize);
    EXPECT_EQ(ComputeSha256Hex(patched_bootup), kExpectedLiveBootupSha256);
}
