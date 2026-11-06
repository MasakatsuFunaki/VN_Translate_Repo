// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Double formatting for the JSON writers.
//
// boost::json serialises every double through ryu, which ALWAYS emits
// scientific notation with a capital 'E' ("1E0", "9.7E-1").  The artifacts
// must carry "1.0" and "0.97" -- narrative_candidates.json contains 91 of
// them, and confidences written as "9.7E-1" are unreadable when triaging a
// scan by hand.
#include <gtest/gtest.h>

#include <boost/json.hpp>

#include "common/util.h"

using namespace mgi;
namespace bj = boost::json;

TEST(JsonFloat, doubles_print_in_shortest_round_trip_form) {
    EXPECT_EQ(format_float(1.0), "1.0");
    EXPECT_EQ(format_float(0.97), "0.97");
    EXPECT_EQ(format_float(0.85), "0.85");
    EXPECT_EQ(format_float(0.0), "0.0");
    EXPECT_EQ(format_float(-0.0), "-0.0");
    EXPECT_EQ(format_float(0.0001), "0.0001");
    EXPECT_EQ(format_float(1e-5), "1e-05");
    EXPECT_EQ(format_float(1e15), "1000000000000000.0");
    EXPECT_EQ(format_float(1e16), "1e+16");
    EXPECT_EQ(format_float(123.456), "123.456");
    EXPECT_EQ(format_float(0.1 + 0.2), "0.30000000000000004");
    EXPECT_EQ(format_float(-2.5), "-2.5");
    EXPECT_EQ(format_float(1e-4), "0.0001");
    EXPECT_EQ(format_float(1e100), "1e+100");
    EXPECT_EQ(format_float(2.5e-10), "2.5e-10");
}

TEST(JsonFloat, json_pretty_uses_format_float) {
    const bj::value v = bj::parse(R"([{"confidence": 1.0}])");
    const std::string s = json_pretty(v, 2);
    EXPECT_NE(s.find("\"confidence\": 1.0"), std::string::npos) << s;
    EXPECT_EQ(s.find('E'), std::string::npos) << s;
}

TEST(JsonFloat, json_dump_uses_format_float) {
    const bj::value v = bj::parse(R"({"c": 0.97})");
    EXPECT_EQ(json_dump(v), "{\"c\": 0.97}");
}

// A blanket double conversion in the kind::double_ branch would turn every
// bbox coordinate into "130.0"; they must stay bare integers.
TEST(JsonFloat, integers_stay_integers) {
    const bj::value v = bj::parse(R"({"bbox": [130, 30, 1230, 660]})");
    EXPECT_EQ(json_dump(v), "{\"bbox\": [130, 30, 1230, 660]}");
}
