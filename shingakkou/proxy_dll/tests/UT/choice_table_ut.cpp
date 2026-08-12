// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — choice-destination table relocation for shingakkou.
//
// Tests translator_logic::RelocateChoiceTables, which rewrites the absolute
// jump offsets embedded in choice menus ('2A 10 FF 00 <count> <count x BE24>')
// so they survive variable-length text replacement. The motivating crash:
// selecting a choice seeked to a stale (un-relocated) offset, landed inside a
// shifted English string, and the engine crashed interpreting string bytes as
// bytecode. Each entry must shift by the cumulative delta of replacements that
// lie before the offset it points at; the table body itself shifts by the
// cumulative delta before its own position.
//
// All inputs are synthetic in-memory buffers; no file I/O.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using translator_logic::CumulativeDelta;
using translator_logic::ReadBE24;
using translator_logic::WriteBE24;
using translator_logic::RelocateChoiceTables;

namespace {

// Lay down a '03 0d xx' statement preamble (a valid choice-destination
// boundary) at `off`.
void PutCodeTarget(std::vector<unsigned char>& b, std::size_t off) {
    b[off + 0] = 0x03; b[off + 1] = 0x0d; b[off + 2] = 0x36;
    b[off + 3] = 0xFF; b[off + 4] = 0x01; b[off + 5] = 0x80;
}

// Write a choice table '2A 10 FF 00 <count> <BE24 entries...>' at `off`.
void PutChoiceTable(std::vector<unsigned char>& b, std::size_t off,
                    const std::vector<std::uint32_t>& dests) {
    b[off + 0] = 0x2A; b[off + 1] = 0x10; b[off + 2] = 0xFF; b[off + 3] = 0x00;
    b[off + 4] = static_cast<unsigned char>(dests.size());
    for (std::size_t k = 0; k < dests.size(); k++)
        WriteBE24(&b[off + 5 + k * 3], dests[k]);
}

}  // namespace

TEST(RelocateChoiceTables, ShiftsEntriesByCumulativeDelta) {
    std::vector<unsigned char> data(0x400, 0x00);
    const std::size_t t0 = 0x200, t1 = 0x300;
    PutCodeTarget(data, t0);
    PutCodeTarget(data, t1);
    const std::size_t tbl = 0x50;            // table sits before any replacement
    PutChoiceTable(data, tbl, {t0, t1});

    std::vector<unsigned char> newData = data;  // verbatim copy (no shift here)
    // One replacement of +0x10 bytes ending before t0/t1 (threshold 0x100).
    std::vector<CumulativeDelta> cum = {{0x100, 0x10}};

    const int n = RelocateChoiceTables(data.data(), data.size(),
                                       newData.data(), newData.size(), cum);
    EXPECT_EQ(n, 2);
    // table position (0x50) is before the threshold -> stays at 0x50
    EXPECT_EQ(ReadBE24(&newData[tbl + 5 + 0]), t0 + 0x10);
    EXPECT_EQ(ReadBE24(&newData[tbl + 5 + 3]), t1 + 0x10);
}

TEST(RelocateChoiceTables, TableBodyShiftsWhenAfterReplacement) {
    std::vector<unsigned char> data(0x800, 0x00);
    const std::size_t t0 = 0x600;
    PutCodeTarget(data, t0);
    const std::size_t tbl = 0x400;           // table itself is past the threshold
    PutChoiceTable(data, tbl, {t0});

    // newData is the shifted buffer; the table body lands at tbl + 0x20.
    std::vector<unsigned char> newData(0x900, 0x00);
    std::vector<CumulativeDelta> cum = {{0x100, 0x20}};

    const int n = RelocateChoiceTables(data.data(), data.size(),
                                       newData.data(), newData.size(), cum);
    EXPECT_EQ(n, 1);
    // entry must be written at the RELOCATED table position (tbl + 0x20),
    // and hold the relocated target (t0 + 0x20).
    EXPECT_EQ(ReadBE24(&newData[tbl + 0x20 + 5]), t0 + 0x20);
}

TEST(RelocateChoiceTables, RejectsNonCodeLikeTargets) {
    std::vector<unsigned char> data(0x400, 0x00);
    const std::size_t tbl = 0x50;
    // Entries point at 0x200/0x300 which are 0x00 0x00 -> not code-like.
    PutChoiceTable(data, tbl, {0x200, 0x300});

    std::vector<unsigned char> newData = data;
    std::vector<CumulativeDelta> cum = {{0x100, 0x10}};

    const int n = RelocateChoiceTables(data.data(), data.size(),
                                       newData.data(), newData.size(), cum);
    EXPECT_EQ(n, 0);
    // buffer untouched: entries still hold the original offsets.
    EXPECT_EQ(ReadBE24(&newData[tbl + 5 + 0]), 0x200u);
    EXPECT_EQ(ReadBE24(&newData[tbl + 5 + 3]), 0x300u);
}

TEST(RelocateChoiceTables, AcceptsExpressionTokenTargets) {
    // '00 3f xx' targets are the other valid boundary shape.
    std::vector<unsigned char> data(0x400, 0x00);
    const std::size_t t0 = 0x200;
    data[t0 + 0] = 0x00; data[t0 + 1] = 0x3f; data[t0 + 2] = 0x03;
    const std::size_t tbl = 0x50;
    PutChoiceTable(data, tbl, {t0});

    std::vector<unsigned char> newData = data;
    std::vector<CumulativeDelta> cum = {{0x100, 0x10}};

    const int n = RelocateChoiceTables(data.data(), data.size(),
                                       newData.data(), newData.size(), cum);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(ReadBE24(&newData[tbl + 5]), t0 + 0x10);
}

TEST(RelocateChoiceTables, NoAnchorNoChange) {
    std::vector<unsigned char> data(0x100, 0x55);
    std::vector<unsigned char> newData = data;
    std::vector<CumulativeDelta> cum;
    EXPECT_EQ(RelocateChoiceTables(data.data(), data.size(),
                                   newData.data(), newData.size(), cum), 0);
    EXPECT_EQ(newData, data);
}
