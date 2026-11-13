// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests against real Sakura no Uta archives (skipped when the game or
// external CBG reference decoder is absent).
#include "apps/paths.h"
#include "bgi/bgi_archive.h"
#include "bgi/bgi_cbg.h"
#include "cg/image.h"
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using snu::Bytes;
namespace bgi = snu::bgi;
namespace fs = std::filesystem;

constexpr std::size_t kBurikoMagicBytes = 27;
constexpr std::size_t kMinDecompressedScriptBytes = 10'000;
constexpr std::size_t kCbgMinHeaderSize = 0x30;
constexpr std::size_t kCbgVersionLoOffset = 0x2E;
constexpr std::size_t kCbgVersionHiOffset = 0x2F;
constexpr std::uint8_t kCbgVersionOne = 1;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
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

std::string resolve_game_dir() {
    if (const wchar_t* env = _wgetenv(L"VN_GAME_DIR");
        env != nullptr && *env != L'\0') {
        return snu::apps::wide_to_utf8(env);
    }
    return "C:\\SakuraNoUta";
}

}  // namespace

TEST(BgiArchive, real_arc_dsc_decompresses_to_buriko_script) {
    const std::string arc = resolve_game_dir() + "\\data01100.arc";
    if (!fs::exists(fs::u8path(arc))) {
        GTEST_SKIP() << "game arc not present at " << arc;
    }
    auto entries = bgi::list_arc(arc);
    bool found = false;
    for (const auto& e : entries) {
        EXPECT_GT(e.size, 0u);
        if (e.name == "00_op_01") {
            found = true;
        }
    }
    ASSERT_TRUE(found);
    Bytes data = bgi::read_entry_by_name(arc, "00_op_01");
    ASSERT_GE(data.size(), kBurikoMagicBytes);
    EXPECT_EQ(std::memcmp(data.data(), "BurikoCompiledScriptVer1.00", kBurikoMagicBytes), 0);
    EXPECT_GT(data.size(), kMinDecompressedScriptBytes);
}

TEST(Cbg, real_cbg_matches_garbro_reference) {
    const std::string arc = resolve_game_dir() + "\\data02503.arc";
    const std::string ref_exe =
        std::string(SNU_PROJECT_DIR) + "\\..\\TOOLS\\garbro\\extract_cbg.exe";
    if (!fs::exists(fs::u8path(arc)) || !fs::exists(fs::u8path(ref_exe))) {
        GTEST_SKIP() << "game data or extract_cbg.exe not available";
    }

    auto entries = snu::bgi::list_arc(arc);
    Bytes raw;
    std::string picked;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Bytes r = snu::bgi::read_entry(arc, i, /*decompress=*/false);
        if (snu::bgi::is_cbg(r) && r.size() > kCbgMinHeaderSize &&
            r[kCbgVersionLoOffset] == kCbgVersionOne &&
            r[kCbgVersionHiOffset] == 0) {
            raw = std::move(r);
            picked = entries[i].name;
            break;
        }
    }
    if (raw.empty()) {
        GTEST_SKIP() << "no v1 CBG entry found in " << arc;
    }
    GTEST_LOG_(INFO) << "cross-validating entry: " << picked;

    bgi::CbgImage ours = bgi::decode_cbg(raw);

    const ScopedTempDir scratch("snu_st_bgi_cbg_ref");
    const std::string tmp_cbg =
        (scratch.path() / fs::u8path("ref.cbg")).u8string();
    const std::string tmp_bmp =
        (scratch.path() / fs::u8path("ref.bmp")).u8string();
    snu::write_file(tmp_cbg, raw);
    ASSERT_EQ(snu::apps::run_process(ref_exe, {tmp_cbg, tmp_bmp}),
              snu::apps::kExitOk);

    Bytes ref_bmp = snu::read_file(tmp_bmp);
    auto ref = snu::cg::decode_image(ref_bmp);
    ASSERT_TRUE(ref.has_value());
    snu::cg::Image mine =
        snu::cg::from_cbg_pixels(ours.pixels, ours.width, ours.height, ours.bpp);

    ASSERT_EQ(mine.width, ref->width);
    ASSERT_EQ(mine.height, ref->height);
    EXPECT_EQ(mine.rgb, ref->rgb);
}
