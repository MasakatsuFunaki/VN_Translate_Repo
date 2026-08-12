// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-5 tests.  Only the pure helpers are covered: detect / translate need
// live vision calls, and glyph placement in the patched BMPs is deliberately
// not pinned to the pixel.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "cg/cg_pipeline.h"
#include "cg/image.h"
#include "common/util.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace crc;
namespace cg = crc::cg;

namespace {

cg::Image solid(int w, int h, cg::Rgb c) {
    cg::Image img;
    img.width = w;
    img.height = h;
    img.rgb.resize(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) img.px(x, y)[k] = c[static_cast<std::size_t>(k)];
    return img;
}

}  // namespace

TEST(Cg, SampleBgTakesTheBorderMedianAndAveragesAnEvenSample) {
    // 10x10 solid grey, with a bright block in the middle that the 2-pixel
    // border sampler must ignore entirely.
    cg::Image img = solid(10, 10, {40, 40, 40});
    for (int y = 3; y < 7; ++y)
        for (int x = 3; x < 7; ++x)
            for (int k = 0; k < 3; ++k) img.px(x, y)[k] = 250;
    EXPECT_EQ(cg::sample_bg(img, 0, 0, 10, 10), (cg::Rgb{40, 40, 40}));

    // Even sample count where the two middle values differ: the two are
    // averaged and the result truncated.  Half the border is 10, half is 21 ->
    // 15.5 -> 15.  A "middle element" median would give 10 or 21.
    cg::Image half = solid(6, 6, {10, 10, 10});
    for (int y = 3; y < 6; ++y)
        for (int x = 0; x < 6; ++x)
            for (int k = 0; k < 3; ++k) half.px(x, y)[k] = 21;
    EXPECT_EQ(cg::sample_bg(half, 0, 0, 6, 6), (cg::Rgb{15, 15, 15}));
}

TEST(Cg, SampleBgSkipsRowsOrColsBelowThreshold) {
    // Both dimensions <= 4: no edges collected at all -> white fallback.
    const cg::Image tiny = solid(4, 4, {7, 8, 9});
    EXPECT_EQ(cg::sample_bg(tiny, 0, 0, 4, 4), (cg::Rgb{255, 255, 255}));
    // Only the height passes the > 4 gate.
    const cg::Image narrow = solid(3, 10, {7, 8, 9});
    EXPECT_EQ(cg::sample_bg(narrow, 0, 0, 3, 10), (cg::Rgb{7, 8, 9}));
}

TEST(Cg, ParseJsonArrayStopsAtFirstBalancedArray) {
    bool found = false;
    // Trailing prose containing brackets must not extend the match -- that is
    // exactly what killed the old greedy \[.*\] regex.
    const std::string resp =
        "Here you go:\n[{\"a\": [1, 2]}]\nNote: see [1] and [2] for details.";
    const std::string got = cg::first_json_array(resp, &found);
    EXPECT_TRUE(found);
    EXPECT_EQ(got, "[{\"a\": [1, 2]}]");
    EXPECT_NO_THROW(bj::parse(got));

    // Brackets inside string literals are ignored, and a backslash escape does
    // not end the string.
    bool f2 = false;
    EXPECT_EQ(cg::first_json_array("[\"a]b\", \"c\\\"]d\"]", &f2), "[\"a]b\", \"c\\\"]d\"]");
    EXPECT_TRUE(f2);

    bool f3 = true;
    cg::first_json_array("no array here", &f3);
    EXPECT_FALSE(f3);
}

TEST(Cg, DetectPromptJsonExtractionIsNonGreedy) {
    // The detect reply is a single flat object, so the match must be
    // non-greedy -- it stops at the FIRST '}' and ignores trailing prose --
    // and (?s) is what lets '.' cross the newlines inside it.
    static const boost::regex OBJ_RE("(?s)\\{.*?\\}");
    boost::smatch m;
    const std::string resp = "text\n{\n \"has_narrative_text\": true\n}\ntrailing {junk}";
    ASSERT_TRUE(boost::regex_search(resp, m, OBJ_RE));
    EXPECT_EQ(m[0].str(), "{\n \"has_narrative_text\": true\n}");
}

TEST(Cg, SafeFilenameSanitisation) {
    EXPECT_EQ(cg::safe_filename("00004"), "00004");
    EXPECT_EQ(cg::safe_filename("a\"b<c>d|e:f*g?h\\i/j"), "a_b_c_d_e_f_g_h_i_j");
    for (int b = 0; b < 0x20; ++b) {
        const std::string in(1, static_cast<char>(b));
        EXPECT_EQ(cg::safe_filename("x" + in + "y"), "x_y") << "byte " << b;
    }
    // Non-ASCII passes through untouched.
    EXPECT_EQ(cg::safe_filename("\xE6\x97\xA5"), "\xE6\x97\xA5");
}

TEST(Cg, NarrativeScannedIsCompactJsonWithSpacedSeparators) {
    bj::array scanned;
    scanned.push_back(bj::array{"sys.cpk", "00000"});
    scanned.push_back(bj::array{"sys.cpk", "00002"});
    // The file's separators are ", " and ": "; plain serialize() would emit
    // [["sys.cpk","00000"],...] and change the format on every rewrite.
    EXPECT_EQ(json_dump(bj::value(std::move(scanned))),
              "[[\"sys.cpk\", \"00000\"], [\"sys.cpk\", \"00002\"]]");
}

TEST(Cg, BmpEncodeWritesTheCanonicalUncompressedLayout) {
    const cg::Image img = solid(1280, 720, {1, 2, 3});
    const Bytes bmp = cg::encode_bmp(img);
    // 1280*720*3 + 54; the row stride is already a multiple of 4.
    ASSERT_EQ(bmp.size(), 2764854u);
    EXPECT_EQ(bmp[0], 'B');
    EXPECT_EQ(bmp[1], 'M');
    EXPECT_EQ(bmp[10], 54);  // pixel data offset
    EXPECT_EQ(bmp[14], 40);  // BITMAPINFOHEADER size
    EXPECT_EQ(bmp[28], 24);  // bits per pixel

    // Bottom-up BGR: the first stored row is the image's LAST row.
    EXPECT_EQ(bmp[54], 3);
    EXPECT_EQ(bmp[55], 2);
    EXPECT_EQ(bmp[56], 1);

    const fs::path ref = fs::u8path(std::string(CRC_PROJECT_DIR) +
                                    "\\script_output\\narrative_patched\\00004.bmp");
    if (!fs::exists(ref)) GTEST_SKIP() << "reference patched BMP not present";
    const Bytes r = read_file(ref.u8string());
    ASSERT_GE(r.size(), 54u);
    // Header only.  The pixels are not compared: glyph placement is allowed to
    // move, the container layout is not.
    for (std::size_t i = 0; i < 54; ++i) EXPECT_EQ(bmp[i], r[i]) << "header byte " << i;
}

TEST(Cg, ExtractArchiveUsesCachedIndexWhenPresent) {
    // With the game uninstalled, the cached _index.json is the only way the
    // CG step can be exercised at all -- guard that it is still readable and
    // shaped [w, h, path].
    const fs::path idx = fs::u8path(std::string(CRC_PROJECT_DIR) +
                                    "\\script_output\\narrative_extracted\\sys\\_index.json");
    if (!fs::exists(idx)) GTEST_SKIP() << "cached _index.json not present";
    const bj::value v = json_parse_file(idx.u8string());
    ASSERT_TRUE(v.is_object());
    ASSERT_FALSE(v.get_object().empty());
    for (const auto& kv : v.get_object()) {
        ASSERT_TRUE(kv.value().is_array());
        ASSERT_EQ(kv.value().get_array().size(), 3u);
        EXPECT_TRUE(kv.value().get_array()[2].is_string());
    }
}
