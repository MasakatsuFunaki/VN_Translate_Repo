// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// FVP .hcb bytecode-walker unit tests using synthetic bytecode buffers.
#include "common/util.h"
#include "fvp/fvp_hcb.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

using namespace fav;

namespace {

constexpr std::uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;
constexpr std::size_t kScriptHeaderByteCount = 4u;
constexpr std::size_t kNulTerminatorByteCount = 1u;
constexpr std::uint8_t kNulByte = 0x00u;
constexpr std::uint8_t kFuncHeaderOpcode = 0x01u;
constexpr std::uint8_t kCallOpcode = 0x02u;
constexpr std::uint8_t kShortArgOpcode = 0x03u;
constexpr std::uint8_t kNoArgOpcode = 0x04u;
constexpr std::uint8_t kUnknownHole0D = 0x0Du;
constexpr std::uint8_t kPushStringOpcode = 0x0Eu;
constexpr std::uint8_t kFaceGraphicOpcode = 0x10u;
constexpr std::uint8_t kUnknownHole13 = 0x13u;
constexpr std::uint8_t kUnknownAboveTableOpcode = 0x28u;
constexpr std::uint8_t kUnknownFFOpcode = 0xFFu;
constexpr std::uint8_t kInvalidCp932Lead = 0x86u;
constexpr std::uint8_t kInvalidCp932Trail = 0x3Au;
constexpr std::uint8_t kFaceArgOne = 0x01u;
constexpr std::size_t kFuncHeaderArgCount = 2u;
constexpr std::size_t kSingleItemCount = 1u;
constexpr std::size_t kTwoByteRawSize = 2u;
constexpr std::size_t kExpectedWalkEventCount = 2u;

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kOneByteShift) & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kTwoBytesShift) & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kThreeBytesShift) & kByteMask));
}

// 0x0E <LEN> <BYTES> 0x00, where LEN counts the NUL.
void put_string_push(Bytes& b, const Bytes& payload) {
    b.push_back(kPushStringOpcode);
    b.push_back(static_cast<std::uint8_t>(payload.size() + kNulTerminatorByteCount));
    b.insert(b.end(), payload.begin(), payload.end());
    b.push_back(kNulByte);
}

// Wraps a bytecode body in the 4-byte script_len header.
Bytes make_hcb(const Bytes& body) {
    Bytes out;
    put_u32(out, static_cast<std::uint32_t>(kScriptHeaderByteCount + body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

TEST(FvpHcb, opcode_table_operand_widths_are_pinned) {
    // Spot-check the operand widths that make the walk self-synchronising.
    EXPECT_EQ(fvp::opcode_args(kFuncHeaderOpcode)->size(), kFuncHeaderArgCount);
    EXPECT_EQ(fvp::opcode_args(kCallOpcode)->at(0), fvp::Arg::D);
    EXPECT_EQ(fvp::opcode_args(kShortArgOpcode)->at(0), fvp::Arg::W);
    EXPECT_TRUE(fvp::opcode_args(kNoArgOpcode)->empty());
    EXPECT_EQ(fvp::opcode_args(kPushStringOpcode)->at(0), fvp::Arg::S);
    // 0x0d and 0x13 are holes in the opcode space, as is everything from 0x28
    // up -- they must stay unknown so a walk that reaches one stops rather
    // than guessing an operand width and desynchronising.
    EXPECT_FALSE(fvp::is_known_opcode(kUnknownHole0D));
    EXPECT_FALSE(fvp::is_known_opcode(kUnknownHole13));
    EXPECT_FALSE(fvp::is_known_opcode(kUnknownAboveTableOpcode));
}

TEST(FvpHcb, extracts_string_push_with_offset) {
    Bytes body;
    body.push_back(kNoArgOpcode);
    const std::size_t expect_off = kScriptHeaderByteCount + body.size();
    put_string_push(body, utf8_to_cp932_replace("あ"));
    const Bytes data = make_hcb(body);

    const auto items = fvp::extract_strings(data);
    ASSERT_EQ(items.size(), kSingleItemCount);
    EXPECT_EQ(items[0].offset, expect_off);
    EXPECT_EQ(items[0].decoded, "あ");
    EXPECT_EQ(items[0].raw.size(), kTwoByteRawSize);
}

// The whole point of walking (rather than scanning for 0x0E) is that a 0x0E
// byte sitting inside another opcode's operand must NOT be read as a string.
TEST(FvpHcb, does_not_misread_0x0e_inside_operands) {
    Bytes body;
    body.push_back(kCallOpcode);
    body.insert(body.end(),
                {kPushStringOpcode, kPushStringOpcode,
                 kPushStringOpcode, kPushStringOpcode});
    put_string_push(body, Bytes{'o', 'k'});
    const Bytes data = make_hcb(body);

    const auto items = fvp::extract_strings(data);
    ASSERT_EQ(items.size(), kSingleItemCount);
    EXPECT_EQ(items[0].decoded, "ok");
}

TEST(FvpHcb, empty_string_push_yields_empty_entry) {
    Bytes body;
    put_string_push(body, Bytes{});
    const auto items = fvp::extract_strings(make_hcb(body));
    ASSERT_EQ(items.size(), kSingleItemCount);
    EXPECT_EQ(items[0].decoded, "");
}

// Invalid CP932 must fall back to U+FFFD replacement rather than be dropped:
// the runtime hook keys on exactly what the extractor recorded.
TEST(FvpHcb, invalid_cp932_falls_back_to_replacement) {
    Bytes body;
    put_string_push(body, Bytes{kInvalidCp932Lead, kInvalidCp932Trail});
    const auto items = fvp::extract_strings(make_hcb(body));
    ASSERT_EQ(items.size(), kSingleItemCount);
    EXPECT_NE(items[0].decoded.find("\uFFFD"), std::string::npos);
}

TEST(FvpHcb, unknown_opcode_throws) {
    const Bytes body{kUnknownFFOpcode};
    EXPECT_THROW(fvp::extract_strings(make_hcb(body)), std::runtime_error);
}

TEST(FvpHcb, too_small_throws) {
    const Bytes data{kFuncHeaderOpcode, kCallOpcode};
    EXPECT_THROW(fvp::extract_strings(data), std::runtime_error);
}

// walk_events stops at the first unknown opcode instead of throwing, and
// records the 0x10 -> 0x0E adjacency the cast-roster gate keys on.
TEST(FvpHcb, walk_events_stops_at_unknown_opcode) {
    Bytes body;
    body.push_back(kFaceGraphicOpcode);
    body.push_back(kFaceArgOne);
    put_string_push(body, Bytes{'n', 'a', 'm', 'e'});
    body.push_back(kUnknownFFOpcode);
    body.push_back(kNoArgOpcode);

    const auto events = fvp::walk_events(make_hcb(body));
    ASSERT_EQ(events.size(), kExpectedWalkEventCount);
    EXPECT_EQ(events[0].op, kFaceGraphicOpcode);
    EXPECT_FALSE(events[0].payload.has_value());
    EXPECT_EQ(events[1].op, kPushStringOpcode);
    ASSERT_TRUE(events[1].payload.has_value());
    EXPECT_EQ(*events[1].payload, "name");
}
