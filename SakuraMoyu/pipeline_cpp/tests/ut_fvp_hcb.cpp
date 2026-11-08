// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// FVP .hcb bytecode-walker tests.  The walker is validated end-to-end against
// c:\Favorite\Sakura.hcb (61,163 strings, byte-identical to the reference
// fvp-tools decompiler output); these are the synthetic round-trips that pin
// the format down without needing the game installed, plus a real-file gate
// that skips when Sakura.hcb isn't present.
#include <gtest/gtest.h>

#include <filesystem>

#include "common/util.h"
#include "fvp/fvp_hcb.h"

namespace fs = std::filesystem;
using namespace fav;

namespace {

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

// 0x0E <LEN> <BYTES> 0x00, where LEN counts the NUL.
void put_string_push(Bytes& b, const Bytes& payload) {
    b.push_back(0x0E);
    b.push_back(static_cast<std::uint8_t>(payload.size() + 1));
    b.insert(b.end(), payload.begin(), payload.end());
    b.push_back(0x00);
}

// Wraps a bytecode body in the 4-byte script_len header.
Bytes make_hcb(const Bytes& body) {
    Bytes out;
    put_u32(out, static_cast<std::uint32_t>(4 + body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

TEST(FvpHcb, opcode_table_operand_widths_are_pinned) {
    // Spot-check the operand widths that make the walk self-synchronising.
    EXPECT_EQ(fvp::opcode_args(0x01)->size(), 2u);
    EXPECT_EQ(fvp::opcode_args(0x02)->at(0), fvp::Arg::D);
    EXPECT_EQ(fvp::opcode_args(0x03)->at(0), fvp::Arg::W);
    EXPECT_TRUE(fvp::opcode_args(0x04)->empty());
    EXPECT_EQ(fvp::opcode_args(0x0e)->at(0), fvp::Arg::S);
    // 0x0d and 0x13 are holes in the opcode space, as is everything from 0x28
    // up -- they must stay unknown so a walk that reaches one stops rather
    // than guessing an operand width and desynchronising.
    EXPECT_FALSE(fvp::is_known_opcode(0x0d));
    EXPECT_FALSE(fvp::is_known_opcode(0x13));
    EXPECT_FALSE(fvp::is_known_opcode(0x28));
}

TEST(FvpHcb, extracts_string_push_with_offset) {
    Bytes body;
    body.push_back(0x04);                              // no-arg filler
    const std::size_t expect_off = 4 + body.size();    // 0x0E lands here
    put_string_push(body, Bytes{0x82, 0xA0});          // CP932 'あ'
    Bytes data = make_hcb(body);

    auto items = fvp::extract_strings(data);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].offset, expect_off);
    EXPECT_EQ(items[0].decoded, "\xE3\x81\x82");  // U+3042
    EXPECT_EQ(items[0].raw.size(), 2u);
}

// The whole point of walking (rather than scanning for 0x0E) is that a 0x0E
// byte sitting inside another opcode's operand must NOT be read as a string.
TEST(FvpHcb, does_not_misread_0x0e_inside_operands) {
    Bytes body;
    body.push_back(0x02);                       // 'd' operand: 4 bytes
    body.insert(body.end(), {0x0E, 0x0E, 0x0E, 0x0E});
    put_string_push(body, Bytes{'o', 'k'});
    Bytes data = make_hcb(body);

    auto items = fvp::extract_strings(data);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].decoded, "ok");
}

TEST(FvpHcb, empty_string_push_yields_empty_entry) {
    Bytes body;
    put_string_push(body, Bytes{});  // LEN == 1, payload empty
    auto items = fvp::extract_strings(make_hcb(body));
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].decoded, "");
}

// Invalid CP932 must fall back to U+FFFD replacement rather than be dropped:
// the runtime hook keys on exactly what the extractor recorded.
TEST(FvpHcb, invalid_cp932_falls_back_to_replacement) {
    Bytes body;
    put_string_push(body, Bytes{0x86, 0x3A});  // 0x3A is not a valid trail
    auto items = fvp::extract_strings(make_hcb(body));
    ASSERT_EQ(items.size(), 1u);
    EXPECT_NE(items[0].decoded.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(FvpHcb, unknown_opcode_throws) {
    Bytes body{0xFF};
    EXPECT_THROW(fvp::extract_strings(make_hcb(body)), std::runtime_error);
}

TEST(FvpHcb, too_small_throws) {
    Bytes data{0x01, 0x02};
    EXPECT_THROW(fvp::extract_strings(data), std::runtime_error);
}

// walk_events stops at the first unknown opcode instead of throwing, and
// records the 0x10 -> 0x0E adjacency the cast-roster gate keys on.
TEST(FvpHcb, walk_events_stops_at_unknown_opcode) {
    Bytes body;
    body.push_back(0x10);
    body.push_back(0x01);  // 'b' operand
    put_string_push(body, Bytes{'n', 'a', 'm', 'e'});
    body.push_back(0xFF);  // unknown -> stop
    body.push_back(0x04);

    auto events = fvp::walk_events(make_hcb(body));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].op, 0x10);
    EXPECT_FALSE(events[0].payload.has_value());
    EXPECT_EQ(events[1].op, 0x0E);
    ASSERT_TRUE(events[1].payload.has_value());
    EXPECT_EQ(*events[1].payload, "name");
}

// Real-file gate: skips cleanly when the game isn't installed.
TEST(FvpHcb, real_sakura_hcb_walks_cleanly) {
    fs::path hcb = fs::u8path("C:\\Favorite\\Sakura.hcb");
    if (!fs::exists(hcb)) GTEST_SKIP() << "C:\\Favorite\\Sakura.hcb not installed";
    auto items = fvp::load_all(hcb.u8string());
    EXPECT_GT(items.size(), 10000u);
    // Offsets are strictly increasing (stream order, no rewinds).
    for (std::size_t i = 1; i < items.size(); ++i)
        ASSERT_GT(items[i].offset, items[i - 1].offset) << "at index " << i;
}
