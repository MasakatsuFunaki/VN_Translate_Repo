// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Narrative-CG helper unit tests: background median sampling, JSON response
// array extraction, filename sanitization, and uncompressed BMP layout.
#include "cg/cg_pipeline.h"
#include "cg/image.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <boost/regex.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace crc;
namespace cg = crc::cg;

namespace {

constexpr int kRgbChannels = 3;
constexpr int kSampleGridSize = 10;
constexpr int kInnerBlockMin = 3;
constexpr int kInnerBlockMax = 7;
constexpr std::uint8_t kGreyShade = 40;
constexpr std::uint8_t kBrightHighlight = 250;
constexpr int kEvenGridSize = 6;
constexpr int kEvenHalfRow = 3;
constexpr std::uint8_t kEvenLowShade = 10;
constexpr std::uint8_t kEvenHighShade = 21;
constexpr std::uint8_t kEvenAverageShade = 15;
constexpr int kTinyEdgeThreshold = 4;
constexpr int kNarrowWidth = 3;
constexpr std::uint8_t kSampleR = 7;
constexpr std::uint8_t kSampleG = 8;
constexpr std::uint8_t kSampleB = 9;
constexpr std::uint8_t kWhiteChannel = 255;
constexpr int kControlByteLimit = 0x20;
constexpr int kCanvasWidth = 1280;
constexpr int kCanvasHeight = 720;
constexpr std::size_t kExpectedBmpSize = 2764854u;
constexpr std::size_t kBmpDataOffsetPos = 10;
constexpr std::size_t kBmpInfoHeaderSizePos = 14;
constexpr std::size_t kBmpBitsPerPixelPos = 28;
constexpr std::uint8_t kBmpHeaderBytes = 54;
constexpr std::uint8_t kBmpInfoHeaderBytes = 40;
constexpr std::uint8_t kBmpBitsPerPixel24 = 24;
constexpr std::size_t kFirstPixelBlueOffset = 54;
constexpr std::size_t kFirstPixelGreenOffset = 55;
constexpr std::size_t kFirstPixelRedOffset = 56;
constexpr std::uint8_t kPixelRed = 1;
constexpr std::uint8_t kPixelGreen = 2;
constexpr std::uint8_t kPixelBlue = 3;

cg::Image solid(int w, int h, cg::Rgb c) {
    cg::Image img;
    img.width = w;
    img.height = h;
    img.rgb.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * kRgbChannels);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int k = 0; k < kRgbChannels; ++k) {
                img.px(x, y)[k] = c[static_cast<std::size_t>(k)];
            }
        }
    }
    return img;
}

}  // namespace

TEST(Cg, SampleBgTakesTheBorderMedianAndAveragesAnEvenSample) {
    cg::Image img = solid(kSampleGridSize, kSampleGridSize,
                          {kGreyShade, kGreyShade, kGreyShade});
    for (int y = kInnerBlockMin; y < kInnerBlockMax; ++y) {
        for (int x = kInnerBlockMin; x < kInnerBlockMax; ++x) {
            for (int k = 0; k < kRgbChannels; ++k) {
                img.px(x, y)[k] = kBrightHighlight;
            }
        }
    }
    EXPECT_EQ(cg::sample_bg(img, 0, 0, kSampleGridSize, kSampleGridSize),
              (cg::Rgb{kGreyShade, kGreyShade, kGreyShade}));

    cg::Image half = solid(kEvenGridSize, kEvenGridSize,
                           {kEvenLowShade, kEvenLowShade, kEvenLowShade});
    for (int y = kEvenHalfRow; y < kEvenGridSize; ++y) {
        for (int x = 0; x < kEvenGridSize; ++x) {
            for (int k = 0; k < kRgbChannels; ++k) {
                half.px(x, y)[k] = kEvenHighShade;
            }
        }
    }
    EXPECT_EQ(cg::sample_bg(half, 0, 0, kEvenGridSize, kEvenGridSize),
              (cg::Rgb{kEvenAverageShade, kEvenAverageShade, kEvenAverageShade}));
}

TEST(Cg, SampleBgSkipsRowsOrColsBelowThreshold) {
    const cg::Image tiny = solid(kTinyEdgeThreshold, kTinyEdgeThreshold,
                                 {kSampleR, kSampleG, kSampleB});
    EXPECT_EQ(cg::sample_bg(tiny, 0, 0, kTinyEdgeThreshold, kTinyEdgeThreshold),
              (cg::Rgb{kWhiteChannel, kWhiteChannel, kWhiteChannel}));

    const cg::Image narrow = solid(kNarrowWidth, kSampleGridSize,
                                   {kSampleR, kSampleG, kSampleB});
    EXPECT_EQ(cg::sample_bg(narrow, 0, 0, kNarrowWidth, kSampleGridSize),
              (cg::Rgb{kSampleR, kSampleG, kSampleB}));
}

TEST(Cg, ParseJsonArrayStopsAtFirstBalancedArray) {
    bool found = false;
    const std::string resp =
        "Here you go:\n[{\"a\": [1, 2]}]\nNote: see [1] and [2] for details.";
    const std::string got = cg::first_json_array(resp, &found);
    EXPECT_TRUE(found);
    EXPECT_EQ(got, "[{\"a\": [1, 2]}]");
    EXPECT_NO_THROW(bj::parse(got));

    bool f2 = false;
    EXPECT_EQ(cg::first_json_array("[\"a]b\", \"c\\\"]d\"]", &f2), "[\"a]b\", \"c\\\"]d\"]");
    EXPECT_TRUE(f2);

    bool f3 = true;
    cg::first_json_array("no array here", &f3);
    EXPECT_FALSE(f3);
}

TEST(Cg, DetectPromptJsonExtractionIsNonGreedy) {
    static const boost::regex kObjRegex("(?s)\\{.*?\\}");
    boost::smatch m;
    const std::string resp = "text\n{\n \"has_narrative_text\": true\n}\ntrailing {junk}";
    ASSERT_TRUE(boost::regex_search(resp, m, kObjRegex));
    EXPECT_EQ(m[0].str(), "{\n \"has_narrative_text\": true\n}");
}

TEST(Cg, SafeFilenameSanitisation) {
    EXPECT_EQ(cg::safe_filename("00004"), "00004");
    EXPECT_EQ(cg::safe_filename("a\"b<c>d|e:f*g?h\\i/j"), "a_b_c_d_e_f_g_h_i_j");
    for (int b = 0; b < kControlByteLimit; ++b) {
        const std::string in(1, static_cast<char>(b));
        EXPECT_EQ(cg::safe_filename("x" + in + "y"), "x_y") << "byte " << b;
    }
    EXPECT_EQ(cg::safe_filename("日"), "日");
}

TEST(Cg, NarrativeScannedIsCompactJsonWithSpacedSeparators) {
    bj::array scanned;
    scanned.push_back(bj::array{"sys.cpk", "00000"});
    scanned.push_back(bj::array{"sys.cpk", "00002"});
    EXPECT_EQ(json_dump(bj::value(std::move(scanned))),
              "[[\"sys.cpk\", \"00000\"], [\"sys.cpk\", \"00002\"]]");
}

TEST(Cg, BmpEncodeWritesTheCanonicalUncompressedLayout) {
    const cg::Image img = solid(kCanvasWidth, kCanvasHeight,
                                {kPixelRed, kPixelGreen, kPixelBlue});
    const Bytes bmp = cg::encode_bmp(img);
    ASSERT_EQ(bmp.size(), kExpectedBmpSize);
    EXPECT_EQ(bmp[0], 'B');
    EXPECT_EQ(bmp[1], 'M');
    EXPECT_EQ(bmp[kBmpDataOffsetPos], kBmpHeaderBytes);
    EXPECT_EQ(bmp[kBmpInfoHeaderSizePos], kBmpInfoHeaderBytes);
    EXPECT_EQ(bmp[kBmpBitsPerPixelPos], kBmpBitsPerPixel24);

    // Bottom-up BGR: the first stored row is the image's last row.
    EXPECT_EQ(bmp[kFirstPixelBlueOffset], kPixelBlue);
    EXPECT_EQ(bmp[kFirstPixelGreenOffset], kPixelGreen);
    EXPECT_EQ(bmp[kFirstPixelRedOffset], kPixelRed);
}

TEST(Cg, RunFindNarrativeCgCompletesOfflineWithSyntheticCachedIndex) {
    struct ScopedTempDir {
        fs::path dir;
        ScopedTempDir() {
            static std::atomic<unsigned int> seq{0};
            dir = fs::temp_directory_path() /
                  fs::u8path("crc_ut_cg_offline_" + std::to_string(++seq));
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir);
        }
        ~ScopedTempDir() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    };

    constexpr int kSyntheticWidth = 200;
    constexpr int kSyntheticHeight = 100;
    constexpr int kCachedIndexIndent = 2;
    constexpr int kSuccessExitCode = 0;

    ScopedTempDir scratch;
    const fs::path game_data = scratch.dir / "game" / "data";
    const fs::path script_out = scratch.dir / "project" / "script_output";
    const fs::path sys_extract = script_out / "narrative_extracted" / "sys";
    const fs::path patched_dir = script_out / "narrative_patched";
    fs::create_directories(game_data);
    fs::create_directories(sys_extract);
    fs::create_directories(patched_dir);

    write_file((game_data / "sys.cpk").u8string(), std::string("CPK"));

    const Bytes synthetic_bmp = cg::encode_bmp(
        solid(kSyntheticWidth, kSyntheticHeight, {kPixelRed, kPixelGreen, kPixelBlue}));
    const fs::path src_bmp = sys_extract / "00004.bmp";
    const fs::path dst_bmp = patched_dir / "00004.bmp";
    write_file(src_bmp.u8string(), synthetic_bmp);
    write_file(dst_bmp.u8string(), synthetic_bmp);

    bj::object index;
    index["00004"] = bj::array{kSyntheticWidth, kSyntheticHeight, src_bmp.u8string()};
    write_file_text((sys_extract / "_index.json").u8string(),
                    json_pretty(bj::value(index), kCachedIndexIndent));

    bj::array scanned;
    scanned.push_back(bj::array{"sys.cpk", "00004"});
    write_file_text((script_out / "narrative_scanned.json").u8string(),
                    json_dump(bj::value(scanned)));

    bj::object cand;
    cand["arc"] = "sys.cpk";
    cand["entry"] = "00004";
    cand["regions"] = bj::array{};
    cand["bmp_path"] = src_bmp.u8string();
    cand["patched_bmp"] = dst_bmp.u8string();
    write_file_text((script_out / "narrative_candidates.json").u8string(),
                    json_pretty(bj::value(bj::array{cand}), kCachedIndexIndent));

    cg::CgOptions opt;
    opt.game_dir = (scratch.dir / "game").u8string();
    opt.project_dir = (scratch.dir / "project").u8string();
    opt.assume_yes = true;
    EXPECT_EQ(cg::run_find_narrative_cg(opt), kSuccessExitCode);
}
