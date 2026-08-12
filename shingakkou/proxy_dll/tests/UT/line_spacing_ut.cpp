// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — message-window line-fit patches for shingakkou.
//
// Tests translator_logic::PatchMessageLineSpacing, which rewrites the
// system scripts' 'var[0x2D1] = 30' assignment (3F 02 D1 0D 1E 40) to a
// smaller imm8 so 4 blitted English lines fit inside the 90px dialogue
// window, translator_logic::PatchTextSurfaceHeight, which enlarges
// the offscreen text surface (layer 130, originally 4 rows x 26px) whose
// height clipped wrapped EN lines 4+ at render time, and
// translator_logic::PatchMessageWindowRefreshTop, which re-anchors the
// per-message window-background stamp at line 1's post-spacing-patch
// position so its top rows get erased between messages. Motivating bugs:
// long EN translations wrapped to a 4th line whose glyphs silently
// vanished ("...a smile blossoming on" with " his face." missing), and
// ghost glyph tops accumulating above the first dialogue line.
//
// All inputs are synthetic in-memory buffers; no file I/O.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <vector>

using translator_logic::PatchMessageLineSpacing;
using translator_logic::PatchMessageWindowRefreshTop;
using translator_logic::PatchTextSurfaceHeight;

namespace {

const unsigned char kAssign30[6] = {0x3F, 0x02, 0xD1, 0x0D, 0x1E, 0x40};

// Write the 6-byte 'var[0x2D1] = 30' assignment at `off`.
void PutAssign30(std::vector<unsigned char>& b, std::size_t off) {
    for (std::size_t k = 0; k < 6; k++) b[off + k] = kAssign30[k];
}

// op15 create surface 130 (Bootup entry 0): layer, x=0, y=0,
// w = 546+4, h = 26*4 — the trailing 4 (byte 17) is the patched
// row-count multiplier.
const unsigned char kCreateSurface[19] = {
    0x15, 0x0E, 0x00, 0x82, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
    0x0E, 0x02, 0x22, 0x04, 0x60, 0xFF, 0x0D, 0x1A, 0x04, 0x68};

// Write the 19-byte create-surface sequence at `off`.
void PutCreateSurface(std::vector<unsigned char>& b, std::size_t off) {
    for (std::size_t k = 0; k < 19; k++) b[off + k] = kCreateSurface[k];
}

// op16 window-background refresh blit (Bootup entry 7 @0x14F): layer 131
// -> screen layer 1, rows var[0x2D3]+36 .. +126. Byte 13 = src y imm8,
// byte 23 = height imm8 (expr adds 4), byte 37 = dst y imm8.
const unsigned char kRefreshBlit[40] = {
    0x16, 0x0E, 0x00, 0x83, 0xFF,
    0x3F, 0x02, 0xD2, 0xFF,
    0x3F, 0x02, 0xD3, 0x0D, 0x24, 0x60, 0xFF,
    0x0E, 0x02, 0x22, 0x04, 0x60, 0xFF,
    0x0D, 0x56, 0x04, 0x60, 0xFF,
    0x01, 0xFF,
    0x3F, 0x02, 0xD2, 0xFF,
    0x3F, 0x02, 0xD3, 0x0D, 0x24, 0x60, 0xFF};

// Write the 40-byte refresh-blit statement at `off`.
void PutRefreshBlit(std::vector<unsigned char>& b, std::size_t off) {
    for (std::size_t k = 0; k < 40; k++) b[off + k] = kRefreshBlit[k];
}

}  // namespace

TEST(PatchMessageLineSpacing, PatchesTheImm8Operand) {
    std::vector<unsigned char> data(0x100, 0x00);
    PutAssign30(data, 0x20);

    EXPECT_EQ(PatchMessageLineSpacing(data.data(), data.size(), 22), 1);

    // Only the imm8 operand changes; the surrounding bytes are intact.
    EXPECT_EQ(data[0x20 + 0], 0x3F);
    EXPECT_EQ(data[0x20 + 1], 0x02);
    EXPECT_EQ(data[0x20 + 2], 0xD1);
    EXPECT_EQ(data[0x20 + 3], 0x0D);
    EXPECT_EQ(data[0x20 + 4], 22);
    EXPECT_EQ(data[0x20 + 5], 0x40);
}

TEST(PatchMessageLineSpacing, PatchesEveryOccurrence) {
    // Bootup entry 7 assigns var 0x2D1 in two branches; entries 0/11/12
    // once each. Any count must be handled.
    std::vector<unsigned char> data(0x100, 0x00);
    PutAssign30(data, 0x10);
    PutAssign30(data, 0x80);

    EXPECT_EQ(PatchMessageLineSpacing(data.data(), data.size(), 22), 2);
    EXPECT_EQ(data[0x10 + 4], 22);
    EXPECT_EQ(data[0x80 + 4], 22);
}

TEST(PatchMessageLineSpacing, LeavesLargeFontAssignmentAlone) {
    // 'var[0x2D1] = 42' (0D 2A) is the large-font branch — intentionally
    // not patched (4 lines of size-32 glyphs cannot fit the box).
    std::vector<unsigned char> data(0x100, 0x00);
    PutAssign30(data, 0x20);
    data[0x20 + 4] = 0x2A;

    EXPECT_EQ(PatchMessageLineSpacing(data.data(), data.size(), 22), 0);
    EXPECT_EQ(data[0x20 + 4], 0x2A);
}

TEST(PatchMessageLineSpacing, LeavesReadReferencesAlone) {
    // A read of var 0x2D1 inside an expression ('3F 02 D1 FF' or
    // '3F 02 D1 68' in the y RPN) lacks the '0D 1E 40' tail and must not
    // be touched.
    std::vector<unsigned char> data(0x100, 0x00);
    data[0x20] = 0x3F; data[0x21] = 0x02; data[0x22] = 0xD1;
    data[0x23] = 0x68; data[0x24] = 0x60; data[0x25] = 0xFF;

    EXPECT_EQ(PatchMessageLineSpacing(data.data(), data.size(), 22), 0);
    EXPECT_EQ(data[0x23], 0x68);
}

TEST(PatchMessageLineSpacing, HandlesBufferEdges) {
    // Too small to contain the pattern at all.
    std::vector<unsigned char> tiny(4, 0x3F);
    EXPECT_EQ(PatchMessageLineSpacing(tiny.data(), tiny.size(), 22), 0);

    // Pattern truncated by end-of-buffer must not match (no overrun).
    std::vector<unsigned char> cut(kAssign30, kAssign30 + 5);
    EXPECT_EQ(PatchMessageLineSpacing(cut.data(), cut.size(), 22), 0);

    // Pattern flush against the end of the buffer still matches.
    std::vector<unsigned char> flush(0x40, 0x00);
    PutAssign30(flush, flush.size() - 6);
    EXPECT_EQ(PatchMessageLineSpacing(flush.data(), flush.size(), 22), 1);
    EXPECT_EQ(flush[flush.size() - 2], 22);
}

TEST(PatchTextSurfaceHeight, PatchesTheRowMultiplier) {
    std::vector<unsigned char> data(0x100, 0x00);
    PutCreateSurface(data, 0x20);

    EXPECT_EQ(PatchTextSurfaceHeight(data.data(), data.size(), 7), 1);

    // Only the multiplier (byte 17) changes; everything else is intact.
    EXPECT_EQ(data[0x20 + 17], 7);
    EXPECT_EQ(data[0x20 + 0], 0x15);   // op15 create
    EXPECT_EQ(data[0x20 + 16], 0x1A);  // imm8 26 (row height) untouched
    EXPECT_EQ(data[0x20 + 18], 0x68);  // mul op untouched
    EXPECT_EQ(data[0x20 + 12], 0x04);  // width's '+4' literal untouched
}

TEST(PatchTextSurfaceHeight, OccursOnceGameWide) {
    // The 19-byte create sequence is unique to Bootup entry 0; a buffer
    // with a single occurrence patches exactly one site.
    std::vector<unsigned char> data(0x200, 0x00);
    PutCreateSurface(data, 0x40);
    EXPECT_EQ(PatchTextSurfaceHeight(data.data(), data.size(), 7), 1);
}

TEST(PatchTextSurfaceHeight, LeavesNonMatchingBuffersAlone) {
    // A buffer that shares the op15 prefix but differs in the width/height
    // body (e.g. a different surface) must not be touched.
    std::vector<unsigned char> data(0x100, 0x00);
    PutCreateSurface(data, 0x20);
    data[0x20 + 11] = 0x99;  // perturb the width imm16 — no longer surface 130 layout

    EXPECT_EQ(PatchTextSurfaceHeight(data.data(), data.size(), 7), 0);
    EXPECT_EQ(data[0x20 + 17], 0x04);  // multiplier unchanged
}

TEST(PatchTextSurfaceHeight, HandlesBufferEdges) {
    // Too small to contain the pattern at all.
    std::vector<unsigned char> tiny(8, 0x15);
    EXPECT_EQ(PatchTextSurfaceHeight(tiny.data(), tiny.size(), 7), 0);

    // Pattern truncated by end-of-buffer must not match (no overrun).
    std::vector<unsigned char> cut(kCreateSurface, kCreateSurface + 18);
    EXPECT_EQ(PatchTextSurfaceHeight(cut.data(), cut.size(), 7), 0);

    // Pattern flush against the end of the buffer still matches.
    std::vector<unsigned char> flush(0x40, 0x00);
    PutCreateSurface(flush, flush.size() - 19);
    EXPECT_EQ(PatchTextSurfaceHeight(flush.data(), flush.size(), 7), 1);
    EXPECT_EQ(flush[flush.size() - 2], 7);
}

TEST(PatchMessageWindowRefreshTop, PatchesBothYsAndHeight) {
    std::vector<unsigned char> data(0x100, 0x00);
    PutRefreshBlit(data, 0x20);

    EXPECT_EQ(PatchMessageWindowRefreshTop(data.data(), data.size(), 29), 1);

    EXPECT_EQ(data[0x20 + 13], 29);    // src y imm8 (was 36)
    EXPECT_EQ(data[0x20 + 23], 93);    // height imm8 (was 86); 29+93+4 = 126
    EXPECT_EQ(data[0x20 + 37], 29);    // dst y imm8 (was 36)

    // Everything else is intact.
    for (std::size_t k = 0; k < 40; k++) {
        if (k == 13 || k == 23 || k == 37) continue;
        EXPECT_EQ(data[0x20 + k], kRefreshBlit[k]) << "byte " << k;
    }
}

TEST(PatchMessageWindowRefreshTop, KeepsBottomEdgeAt126) {
    // Whatever top is requested, top + height-imm8 + 4 must stay 126 so
    // the stamp's bottom edge (and the 4-line fit below it) never moves.
    for (unsigned char top : {20, 29, 36}) {
        std::vector<unsigned char> data(0x100, 0x00);
        PutRefreshBlit(data, 0x20);
        EXPECT_EQ(PatchMessageWindowRefreshTop(data.data(), data.size(), top), 1);
        EXPECT_EQ(data[0x20 + 13] + data[0x20 + 23] + 4, 126);
    }
}

TEST(PatchMessageWindowRefreshTop, LeavesNonMatchingBuffersAlone) {
    // A blit that shares the op16+layer-131 prefix but differs in the
    // body (e.g. entry 6's 31x33 click-cursor stamp) must not be touched.
    std::vector<unsigned char> data(0x100, 0x00);
    PutRefreshBlit(data, 0x20);
    data[0x20 + 17] = 0x99;  // perturb the width imm16 — different blit

    EXPECT_EQ(PatchMessageWindowRefreshTop(data.data(), data.size(), 29), 0);
    EXPECT_EQ(data[0x20 + 13], 0x24);  // y unchanged
    EXPECT_EQ(data[0x20 + 23], 0x56);  // height unchanged
}

TEST(PatchMessageWindowRefreshTop, HandlesBufferEdges) {
    // Too small to contain the pattern at all.
    std::vector<unsigned char> tiny(8, 0x16);
    EXPECT_EQ(PatchMessageWindowRefreshTop(tiny.data(), tiny.size(), 29), 0);

    // Pattern truncated by end-of-buffer must not match (no overrun).
    std::vector<unsigned char> cut(kRefreshBlit, kRefreshBlit + 39);
    EXPECT_EQ(PatchMessageWindowRefreshTop(cut.data(), cut.size(), 29), 0);

    // Pattern flush against the end of the buffer still matches.
    std::vector<unsigned char> flush(0x60, 0x00);
    PutRefreshBlit(flush, flush.size() - 40);
    EXPECT_EQ(PatchMessageWindowRefreshTop(flush.data(), flush.size(), 29), 1);
    EXPECT_EQ(flush[flush.size() - 3], 29);  // dst y is pattern byte 37 of 40
}
