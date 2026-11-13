// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace translator_logic {

namespace {

constexpr std::size_t kTabEscapeLen      = 5;
constexpr std::size_t kTabEscapeAdvance  = 4;
constexpr std::size_t kCrLfEscapeLen     = 4;
constexpr std::size_t kCrLfEscapeAdvance = 3;
constexpr std::size_t kUtf8BomBytes      = 3;
constexpr unsigned char kUtf8BomByte0    = 0xEFU;
constexpr unsigned char kUtf8BomByte1    = 0xBBU;
constexpr unsigned char kUtf8BomByte2    = 0xBFU;
constexpr unsigned int  kByteShift8      = 8U;
constexpr unsigned int  kByteShift16     = 16U;
constexpr unsigned int  kByteMask        = 0xFFU;

constexpr std::size_t kMinMarkerOffset            = 1;
constexpr std::size_t kMarkerSpanBytes            = 3;
constexpr std::size_t kUtf16CodeUnitBytes         = 2;
constexpr std::size_t kBe24ByteCount              = 3;
constexpr std::size_t kHxbHeaderSizeFieldOffset   = 0x08;
constexpr std::size_t kHxbLineNumberFlagOffset    = 0x0B;
constexpr std::size_t kHxbBodyStartOffset         = 0x10;
constexpr std::size_t kHxbHeaderTableFlagOffset   = 0x15;
constexpr std::size_t kHxbHeaderTablePtrOffset    = 0x16;
constexpr std::size_t kHxbMinHeaderTablePatchSize = 0x2A;
constexpr std::uint32_t kHeaderTablePtrBias       = 5;

constexpr unsigned char kExprTerminatorByte   = 0xFF;
constexpr unsigned char kStandardRecordByte1  = 0x01;
constexpr unsigned char kTextTokenByte        = 0x80;
constexpr unsigned char kHighNibbleMask       = 0xF0;
constexpr unsigned char kLowNibbleMask        = 0x0F;
constexpr unsigned char kSmallIntTokenLimit   = 0x40;
constexpr unsigned char kOperatorTokenLimit   = 0x80;
constexpr unsigned char kLowImm8Nibble        = 0x0D;
constexpr unsigned char kLowImm16Nibble       = 0x0E;
constexpr unsigned char kLowImm32Nibble       = 0x0F;
constexpr std::size_t   kImm8Bytes            = 1;
constexpr std::size_t   kImm16Bytes           = 2;
constexpr std::size_t   kImm32Bytes           = 4;
constexpr unsigned char kVarargEndByte        = 0x00;

constexpr unsigned char kPreByteDialogue      = 0x36;
constexpr unsigned char kPreByteSpeakerMisc   = 0x07;
constexpr unsigned char kPreByteConfirmChoice = 0x01;
constexpr unsigned char kPreByteErrorModal    = 0x02;
constexpr unsigned char kPreByteMenuFirst     = 0xB0;
constexpr unsigned char kPreByteMenuLast      = 0xBA;

constexpr unsigned char kInlineChoiceHeaderTag = 0x7E;
constexpr unsigned char kInlineLockedSlotTag   = 0x1F;
constexpr unsigned char kInlineSuffixPromptTag = 0x60;
constexpr unsigned char kInlineLockedTrackTag  = 0xA0;

constexpr unsigned char kOpGosub         = 0x02;
constexpr unsigned char kOpCallScript    = 0x03;
constexpr unsigned char kOpVararg06      = 0x06;
constexpr unsigned char kOpJumpZero      = 0x26;
constexpr unsigned char kOpJumpNonZero   = 0x27;
constexpr unsigned char kOpTwoWayBranch  = 0x28;
constexpr unsigned char kOpJump          = 0x29;
constexpr unsigned char kOpChoiceTable   = 0x2A;
constexpr unsigned char kOpTextWrap      = 0x6A;
constexpr unsigned char kMaxDdwOpcode    = 0x71;

constexpr int kOpcodeFixedExprCount[kMaxDdwOpcode + 1] = {
    1, 2, 0, 1, 2, 1, 3, 1, 1, 1, 0, 1, 2, 3, 11, 4,
    9, 5, 5, 2, 3, 8, 12, 8, 3, 1, 1, 1, 2, 1, 0, 2,
    0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 4, 4, 3, 3,
    0, 4, 6, 1, 1, 2, 2, 1, 2, 0, 3, 3, 1, 1, 3, 0,
    1, 2, 2, 2, 3, 3, 2, 2, 4, 2, 2, 3, 3, 3, 2, 1,
    3, 0, 2, 2, 2, 4, 2, 1, 2, 4, 2, 6, 1, 0, 2, 3,
    5, 2, 1, 0, 2, 2, 4, 0, 0, 0, 0, 0, 2, 7, 2, 0,
    0, 5
};

constexpr std::uint32_t kUtf8OneByteMax      = 0x7F;
constexpr std::uint32_t kUtf8TwoByteMax      = 0x7FF;
constexpr std::uint32_t kUtf8ThreeByteMax    = 0xFFFF;
constexpr unsigned char kUtf8TwoBytePrefix   = 0xC0;
constexpr unsigned char kUtf8ThreeBytePrefix = 0xE0;
constexpr unsigned char kUtf8FourBytePrefix  = 0xF0;
constexpr unsigned char kUtf8TrailPrefix     = 0x80;
constexpr unsigned char kUtf8TwoByteMask     = 0x1F;
constexpr unsigned char kUtf8ThreeByteMask   = 0x0F;
constexpr unsigned char kUtf8FourByteMask    = 0x07;
constexpr unsigned char kUtf8TrailMask       = 0x3F;
constexpr std::uint32_t kSurrogateBase       = 0x10000;
constexpr std::uint16_t kHighSurrogateStart  = 0xD800;
constexpr std::uint16_t kHighSurrogateEnd    = 0xDBFF;
constexpr std::uint16_t kLowSurrogateStart   = 0xDC00;
constexpr std::uint16_t kLowSurrogateEnd     = 0xDFFF;
constexpr std::uint32_t kSurrogateMask       = 0x3FF;
constexpr unsigned int  kSurrogateShift      = 10;
constexpr unsigned int  kUtf8Shift6          = 6;
constexpr unsigned int  kUtf8Shift12         = 12;
constexpr unsigned int  kUtf8Shift18         = 18;

struct SystemTranslationPair {
    const char* jp;
    const char* en;
};

constexpr SystemTranslationPair kSystemTranslations[] = {
    // Bootup#00: disc insertion prompt
    {"『神学校-Noli me tangere-』の/[Disc1]を挿入してください",
     "Please insert [Disc 1] of/Shingakkou -Noli me tangere-."},
    // Bootup#03: quick load confirmation
    {"クイックロードします。/よろしいですか？",
     "Quick load the latest save?/Are you sure?"},
    // Bootup#09: backlog choice label
    {"【選択肢】",
     "[Choice]"},
    // Bootup#10: right-click context menu items & modal confirmations
    {"ひとつ前の選択肢に戻る",
     "Return to Previous Choice"},
    {"セーブ",
     "Save"},
    {"ロード",
     "Load"},
    {"各種設定",
     "Configuration"},
    {"ボイスコレクション",
     "Voice Collection"},
    {"画面サイズ変更",
     "Window Size"},
    {"フルスクリーン設定",
     "Fullscreen Settings"},
    {"タイトル画面",
     "Return to Title"},
    {"ゲームを終了",
     "Exit Game"},
    {"回想一覧に戻る",
     "Return to Scene Replay"},
    {"閉じる",
     "Close Menu"},
    {"タイトル画面に戻ります。/よろしいですか？",
     "Return to the title screen?/Unsaved progress will be lost."},
    {"『神学校』を終了しますか？",
     "Exit Shingakkou -Noli me tangere-?"},
    {"回想メニューに戻ります。/よろしいですか？",
     "Return to the Scene Replay menu?/Are you sure?"},
    // Bootup#12: config menu preview texts (both literal \n and control LF) & reset confirm
    {"オートプレイの待ち時間は、\\nこのくらいになります。",
     "Auto-play will advance at\\nthis pace between lines."},
    {"オートプレイの待ち時間は、\nこのくらいになります。",
     "Auto-play will advance at\nthis pace between lines."},
    {"メッセージの表示速度は、\\nこのくらいで良いですか？",
     "Is this text display speed\\ncomfortable for reading?"},
    {"メッセージの表示速度は、\nこのくらいで良いですか？",
     "Is this text display speed\ncomfortable for reading?"},
    {"設定を初期値に戻します/よろしいですか？",
     "Reset this page to default settings?/Are you sure?"},
    // Bootup#13: save/load error, confirmations & locked title placeholder
    {"ファイルの保存が出来ませんでした",
     "Failed to save the file."},
    {"既にデータが記録されています。/上書きしてもよろしいですか？",
     "Save data already exists in this slot./Overwrite it?"},
    {"ロードしますがよろしいですか？",
     "Load this save data?/Unsaved progress will be lost."},
    {"？？？？",
     "????"},
    // Bootup#14: voice collection registration confirmations
    {"は既に記録されています。/上書きしてもよろしいですか？",
     " already has a saved voice clip./Overwrite it?"},
    {"に保存します。/よろしいですか？",
     " will store this voice clip./Are you sure?"},
    // Bootup#15: voice collection deletion & locked music track placeholder
    {"を削除します。/よろしいですか？",
     " will be deleted./Are you sure?"},
    {"？？？？？？？？？？？？？",
     "?????????????"}
};

std::uint16_t ReadLE16(const unsigned char* p)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << kByteShift8));
}

void WriteLE16(unsigned char* p, std::uint16_t v)
{
    p[0] = static_cast<unsigned char>(v & kByteMask);
    p[1] = static_cast<unsigned char>((v >> kByteShift8) & kByteMask);
}

bool SkipDdwExpression(const unsigned char* data,
                       std::size_t limit,
                       std::size_t& pos)
{
    while (pos < limit) {
        const unsigned char token = data[pos];
        if (token == kExprTerminatorByte) {
            pos += 1;
            return true;
        }
        const unsigned char high = token & kHighNibbleMask;
        const unsigned char low  = token & kLowNibbleMask;
        if (token < kSmallIntTokenLimit) {
            if (high == 0) {
                std::size_t extra = 0;
                if (low == kLowImm8Nibble)  extra = kImm8Bytes;
                if (low == kLowImm16Nibble) extra = kImm16Bytes;
                if (low == kLowImm32Nibble) extra = kImm32Bytes;
                pos += 1 + extra;
                continue;
            }
            std::size_t extra = 0;
            if (low == kLowImm16Nibble) extra = kImm8Bytes;
            if (low == kLowImm32Nibble) extra = kImm16Bytes;
            pos += 1 + extra;
            continue;
        }
        if (high < kOperatorTokenLimit) {
            pos += 1;
            continue;
        }
        if (high == kTextTokenByte) {
            pos += 1;
            bool foundNull = false;
            while (pos + 1 < limit) {
                const std::uint16_t ch = ReadLE16(data + pos);
                pos += kUtf16CodeUnitBytes;
                if (ch == 0) {
                    foundNull = true;
                    break;
                }
            }
            if (!foundNull) return false;
            continue;
        }
        std::size_t extra = 0;
        if (low == kLowImm16Nibble) extra = kImm8Bytes;
        if (low == kLowImm32Nibble) extra = kImm16Bytes;
        pos += 1 + extra;
    }
    return false;
}

bool SkipDdwVarargs(const unsigned char* data,
                    std::size_t limit,
                    std::size_t& pos)
{
    while (pos < limit) {
        const unsigned char tag = data[pos];
        pos += 1;
        if (tag == kVarargEndByte) return true;
        if (!SkipDdwExpression(data, limit, pos)) return false;
    }
    return false;
}

bool ParseDdwStatements(const unsigned char* data,
                        std::size_t dataSize,
                        std::unordered_set<std::uint32_t>& outStatementStarts,
                        std::vector<std::size_t>& outJumpTargetOffsets)
{
    outStatementStarts.clear();
    outJumpTargetOffsets.clear();
    if (dataSize < kHxbBodyStartOffset) return false;

    const OffsetTable tbl = FindOffsetTable(data, dataSize);
    const std::size_t codeEnd = (tbl.count > 0) ? tbl.start : dataSize;
    const bool hasLineNumbers = (data[kHxbLineNumberFlagOffset] != 0);
    std::size_t pos = kHxbBodyStartOffset;

    if (hasLineNumbers) {
        for (int s = 0; s < 2; s++) {
            while (pos + 1 < codeEnd) {
                const std::uint16_t w = ReadLE16(data + pos);
                pos += kUtf16CodeUnitBytes;
                if (w == 0) break;
            }
        }
    }

    while (pos < codeEnd) {
        outStatementStarts.insert(static_cast<std::uint32_t>(pos));
        if (hasLineNumbers) {
            if (pos + kImm16Bytes > codeEnd) return false;
            pos += kImm16Bytes;
        }
        if (pos >= codeEnd) break;
        const unsigned char op = data[pos];
        pos += 1;
        if (op == kExprTerminatorByte) break;
        if (op > kMaxDdwOpcode) return false;

        const int exprCount = kOpcodeFixedExprCount[op];
        for (int e = 0; e < exprCount; e++) {
            if (!SkipDdwExpression(data, codeEnd, pos)) return false;
        }

        if (op == kOpGosub || op == kOpCallScript ||
            op == kOpVararg06 || op == kOpTextWrap) {
            if (!SkipDdwVarargs(data, codeEnd, pos)) return false;
        }

        if (op == kOpGosub || op == kOpJumpZero ||
            op == kOpJumpNonZero || op == kOpJump) {
            if (pos + kBe24ByteCount > codeEnd) return false;
            outJumpTargetOffsets.push_back(pos);
            pos += kBe24ByteCount;
            continue;
        }

        if (op == kOpTwoWayBranch) {
            if (pos + (kBe24ByteCount * 2) > codeEnd) return false;
            outJumpTargetOffsets.push_back(pos);
            outJumpTargetOffsets.push_back(pos + kBe24ByteCount);
            pos += kBe24ByteCount * 2;
            continue;
        }

        if (op == kOpChoiceTable) {
            if (pos + kImm16Bytes > codeEnd) return false;
            const std::size_t branchCount = ReadBE16(data + pos);
            const std::size_t tableSpan =
                kImm16Bytes + (branchCount * kBe24ByteCount);
            if (pos + tableSpan > codeEnd) return false;
            pos += kImm16Bytes;
            for (std::size_t b = 0; b < branchCount; b++) {
                outJumpTargetOffsets.push_back(pos);
                pos += kBe24ByteCount;
            }
            continue;
        }
    }

    return pos == codeEnd;
}

}  // namespace

void UnescapeInPlace(std::string& s)
{
    // Behavior must match the pre-extraction loop byte-for-byte:
    //   {TAB} -> \t      5 chars consumed
    //   {CR}  -> \r      4 chars consumed
    //   {LF}  -> \n      4 chars consumed
    //   anything else    copied verbatim (so "{NOPE}" stays literal,
    //                    which is important -- a handful of TSV
    //                    entries contain curly braces on purpose).
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{') {
            if (s.compare(i, kTabEscapeLen, "{TAB}") == 0) {
                out += '\t';
                i += kTabEscapeAdvance;
                continue;
            }
            if (s.compare(i, kCrLfEscapeLen, "{CR}") == 0) {
                out += '\r';
                i += kCrLfEscapeAdvance;
                continue;
            }
            if (s.compare(i, kCrLfEscapeLen, "{LF}") == 0) {
                out += '\n';
                i += kCrLfEscapeAdvance;
                continue;
            }
        }
        out += s[i];
    }
    s = std::move(out);
}

std::size_t Utf8BomLen(const char* data, std::size_t len)
{
    if (len >= kUtf8BomBytes &&
        static_cast<unsigned char>(data[0]) == kUtf8BomByte0 &&
        static_cast<unsigned char>(data[1]) == kUtf8BomByte1 &&
        static_cast<unsigned char>(data[2]) == kUtf8BomByte2) {
        return kUtf8BomBytes;
    }
    return 0;
}

int ParseTsvBuffer(const char* data, std::size_t len, Utf8TranslationMap& out)
{
    int count = 0;
    std::size_t i = 0;
    while (i < len) {
        const std::size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        const std::size_t line_end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) i++;

        std::size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') tab++;
        if (tab == line_start || tab >= line_end) continue;

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + 1, line_end - (tab + 1));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        if (jp.empty() || en.empty()) continue;
        out[jp] = en;   // last duplicate wins (matches prior behavior)
        count++;
    }
    return count;
}

// Big-endian pack/unpack

std::uint16_t ReadBE16(const unsigned char* p)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << kByteShift8) |
         static_cast<std::uint16_t>(p[1]));
}

void WriteBE16(unsigned char* p, std::uint16_t v)
{
    p[0] = static_cast<unsigned char>((v >> kByteShift8) & kByteMask);
    p[1] = static_cast<unsigned char>(v                  & kByteMask);
}

std::uint32_t ReadBE24(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0]) << kByteShift16) |
           (static_cast<std::uint32_t>(p[1]) << kByteShift8) |
            static_cast<std::uint32_t>(p[2]);
}

void WriteBE24(unsigned char* p, std::uint32_t v)
{
    p[0] = static_cast<unsigned char>((v >> kByteShift16) & kByteMask);
    p[1] = static_cast<unsigned char>((v >> kByteShift8)  & kByteMask);
    p[2] = static_cast<unsigned char>( v                  & kByteMask);
}

// Cumulative-delta offset fixup

int ApplyCumulativeDelta(std::uint32_t offset,
                         const std::vector<CumulativeDelta>& table)
{
    // Iterate front-to-back, tracking the last entry whose threshold
    // is <= offset. Same loop body as the original code in
    // PatchDecryptedData so behavior is unchanged: offsets before the
    // first threshold get delta 0 (because the loop breaks before
    // assigning adj), offsets at/past a threshold pick up that
    // cum_delta.
    int adj = 0;
    for (const auto& c : table) {
        if (offset < c.threshold) break;
        adj = c.cum_delta;
    }
    return adj;
}

// Tail offset-table location

OffsetTable FindOffsetTable(const unsigned char* data, std::size_t dataSize)
{
    if (dataSize <= 4 || data[dataSize - 1] != 0xFF)
        return {0, 0};

    // The final byte is the 0xFF terminator, so the last 3-byte entry
    // begins at dataSize - 4. Walk backwards while each entry stays a
    // valid, ascending offset that points at an 'FF 01 80' text-record
    // marker. `next` is the value of the entry we just accepted; earlier
    // entries must not exceed it (the table ascends towards the tail).
    std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(dataSize) - 4;
    std::uint32_t next = static_cast<std::uint32_t>(dataSize);
    std::ptrdiff_t start = -1;

    while (pos >= 0) {
        const std::uint32_t v = ReadBE24(data + pos);
        if (v == 0 || v >= dataSize) break;          // out of range
        if (v > next) break;                          // not ascending
        if (v + 5 >= dataSize ||                      // marker must fit
            data[v + 3] != 0xFF ||
            data[v + 4] != 0x01 ||
            data[v + 5] != 0x80) break;               // not a text record
        next = v;
        start = pos;
        pos -= 3;
    }

    if (start < 0) return {0, 0};

    const int count = static_cast<int>(
        (static_cast<std::ptrdiff_t>(dataSize) - 1 - start) / 3);
    return {static_cast<std::size_t>(start), count};
}

// Choice-destination table relocation

int RelocateChoiceTables(const unsigned char* data, std::size_t dataSize,
                         unsigned char* newData, std::size_t newDataSize,
                         const std::vector<CumulativeDelta>& cumulative)
{
    // A choice destination must land on a code-like boundary: either an
    // '03 0d xx' statement preamble or a '00 3f xx' expression token. These
    // are the only two target shapes the real tables use; requiring it of
    // every entry rejects coincidental 2A 10 FF 00 byte runs.
    auto codeLike = [&](std::uint32_t v) -> bool {
        if (v < 0x10 || v + 6 > dataSize) return false;
        return (data[v] == 0x03 && data[v + 1] == 0x0d) ||
               (data[v] == 0x00 && data[v + 1] == 0x3f);
    };

    int relocated = 0;
    if (dataSize < 5) return 0;

    for (std::size_t i = 0; i + 5 <= dataSize; i++) {
        if (data[i]     != 0x2A || data[i + 1] != 0x10 ||
            data[i + 2] != 0xFF || data[i + 3] != 0x00)
            continue;

        const unsigned count = data[i + 4];
        if (count == 0 || count > 16) continue;
        if (i + 5 + static_cast<std::size_t>(count) * 3 > dataSize) continue;

        // Validate every entry before touching the buffer (all-or-nothing).
        bool ok = true;
        for (unsigned e = 0; e < count; e++) {
            if (!codeLike(ReadBE24(data + i + 5 + e * 3))) { ok = false; break; }
        }
        if (!ok) continue;

        // The table body is copied verbatim, so it sits at i shifted by the
        // cumulative delta of all replacements before it; each entry value is
        // shifted by the cumulative delta at the value it points to.
        const std::size_t newTablePos = static_cast<std::size_t>(
            static_cast<int>(i) + ApplyCumulativeDelta(
                static_cast<std::uint32_t>(i), cumulative));

        for (unsigned e = 0; e < count; e++) {
            const std::size_t newEntryPos = newTablePos + 5 + e * 3;
            if (newEntryPos + 3 > newDataSize) break;
            const std::uint32_t val = ReadBE24(data + i + 5 + e * 3);
            const std::uint32_t nv = static_cast<std::uint32_t>(
                static_cast<int>(val) + ApplyCumulativeDelta(val, cumulative));
            WriteBE24(newData + newEntryPos, nv);
            relocated++;
        }
    }
    return relocated;
}

// Message-window line-spacing patch

int PatchMessageLineSpacing(unsigned char* data, std::size_t dataSize,
                            unsigned char newSpacing)
{
    // var[0x2D1] = 30 :  var-ref token (3F 02 D1), imm8 push (0D 1E),
    // store op (40). See translator_logic.h for why this byte sequence
    // is unique to the system scripts' spacing assignment.
    static const unsigned char kPattern[6] = {0x3F, 0x02, 0xD1, 0x0D, 0x1E, 0x40};

    int patched = 0;
    if (dataSize < sizeof(kPattern)) return 0;

    for (std::size_t i = 0; i + sizeof(kPattern) <= dataSize; i++) {
        bool match = true;
        for (std::size_t k = 0; k < sizeof(kPattern); k++) {
            if (data[i + k] != kPattern[k]) { match = false; break; }
        }
        if (match) {
            data[i + 4] = newSpacing;   // the imm8 operand (was 0x1E = 30)
            patched++;
        }
    }
    return patched;
}

// Message text-surface height patch

int PatchTextSurfaceHeight(unsigned char* data, std::size_t dataSize,
                           unsigned char newLineCapacity)
{
    // op15 create surface 130: 15 | imm16 0x82 | x=0 | y=0 |
    // w = imm16 546 + 4 | h = imm8 26 * 4 (each arg FF-terminated).
    // See translator_logic.h; occurs exactly once game-wide (Bootup
    // entry 0).
    static const unsigned char kPattern[19] = {
        0x15, 0x0E, 0x00, 0x82, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
        0x0E, 0x02, 0x22, 0x04, 0x60, 0xFF, 0x0D, 0x1A, 0x04, 0x68};

    int patched = 0;
    if (dataSize < sizeof(kPattern)) return 0;

    for (std::size_t i = 0; i + sizeof(kPattern) <= dataSize; i++) {
        bool match = true;
        for (std::size_t k = 0; k < sizeof(kPattern); k++) {
            if (data[i + k] != kPattern[k]) { match = false; break; }
        }
        if (match) {
            data[i + 17] = newLineCapacity;  // the multiplier (was 4 rows)
            patched++;
        }
    }
    return patched;
}

// English word wrap for the message window

int EngineCharWidthPx(wchar_t c)
{
    // Mirror of the engine's per-char measure (FUN_00402860, fixed-pitch
    // path) with the proportionalizer patches applied: halfwidth chars
    // measure 8px (patch #2), everything else the full cell 21px.
    // Halfwidth = code < 0xFF excluding 0xB1/0xD7/0xF7 (the +-/x/÷ signs),
    // plus the halfwidth-katakana block FF61..FF9F.
    const unsigned int o = static_cast<unsigned int>(c);
    if ((o < 0xFF && o != 0xB1 && o != 0xD7 && o != 0xF7) ||
        (o >= 0xFF61 && o <= 0xFF9F))
        return 8;
    return 21;
}

int WordWrapMessage(std::wstring& text, int budgetPx)
{
    // Greedy word wrap: walk the string with the engine's own width
    // metric; when a char would overflow the budget, replace the LAST
    // space of the current line with '\r' (the engine's hard-break char,
    // consumed by the 0x6A mode-1 wrap). One-for-one char swap: the
    // string length never changes, so replacement byte sizes and all
    // offset relocation downstream are unaffected.
    int breaks = 0;
    int lineWidth = 0;            // width of the current line so far
    std::ptrdiff_t lastSpace = -1;// index of the latest space in this line
    int widthAfterSpace = 0;      // width accumulated after that space

    for (std::size_t i = 0; i < text.size(); i++) {
        const wchar_t c = text[i];
        if (c == L'\r') {         // pre-existing hard break
            lineWidth = 0; lastSpace = -1; widthAfterSpace = 0;
            continue;
        }
        const int w = EngineCharWidthPx(c);
        if (c == L' ') {
            lineWidth += w;
            lastSpace = static_cast<std::ptrdiff_t>(i);
            widthAfterSpace = 0;
            continue;
        }
        if (lineWidth + w > budgetPx) {
            if (lastSpace >= 0) {
                text[static_cast<std::size_t>(lastSpace)] = L'\r';
                breaks++;
                lineWidth = widthAfterSpace + w;
                lastSpace = -1; widthAfterSpace = 0;
            } else {
                // Word longer than a whole line: leave it to the engine's
                // char-granularity break (it resets the line to this char).
                lineWidth = w;
            }
        } else {
            lineWidth += w;
        }
        if (lastSpace >= 0) widthAfterSpace += w;
    }
    return breaks;
}

// Message-window background-refresh band patch

int PatchMessageWindowRefreshTop(unsigned char* data, std::size_t dataSize,
                                 unsigned char newTopOffset)
{
    // op16 blit: window background (layer 131) re-stamped over the text
    // area at the start of every message. Args, each FF-terminated:
    //   src layer 131 | src x = var[0x2D2] | src y = var[0x2D3] + imm8 36 |
    //   w = 546+4 | h = imm8 86 + 4 | dst layer 1 | dst x = var[0x2D2] |
    //   dst y = var[0x2D3] + imm8 36 | ...
    // See translator_logic.h for why the 36 must follow the line spacing.
    static const unsigned char kPattern[40] = {
        0x16, 0x0E, 0x00, 0x83, 0xFF,                    // op16, src layer 131
        0x3F, 0x02, 0xD2, 0xFF,                          // src x = var[0x2D2]
        0x3F, 0x02, 0xD3, 0x0D, 0x24, 0x60, 0xFF,        // src y = var[0x2D3]+36
        0x0E, 0x02, 0x22, 0x04, 0x60, 0xFF,              // w = 546+4
        0x0D, 0x56, 0x04, 0x60, 0xFF,                    // h = 86+4
        0x01, 0xFF,                                      // dst layer 1
        0x3F, 0x02, 0xD2, 0xFF,                          // dst x = var[0x2D2]
        0x3F, 0x02, 0xD3, 0x0D, 0x24, 0x60, 0xFF};       // dst y = var[0x2D3]+36

    int patched = 0;
    if (dataSize < sizeof(kPattern)) return 0;

    // Keep the bottom edge at +126 (= 36 + 86 + 4): height imm8 grows by
    // exactly what the top moved up.
    const unsigned char newHeight =
        static_cast<unsigned char>(122 - newTopOffset);

    for (std::size_t i = 0; i + sizeof(kPattern) <= dataSize; i++) {
        bool match = true;
        for (std::size_t k = 0; k < sizeof(kPattern); k++) {
            if (data[i + k] != kPattern[k]) { match = false; break; }
        }
        if (match) {
            data[i + 13] = newTopOffset;  // src y imm8 (was 36)
            data[i + 23] = newHeight;     // height imm8 (was 86; expr adds 4)
            data[i + 37] = newTopOffset;  // dst y imm8 (was 36)
            patched++;
        }
    }
    return patched;
}

// System-script translation table and HXB bytecode relocation

int PopulateSystemTranslations(Utf8TranslationMap& out)
{
    int inserted = 0;
    for (const auto& entry : kSystemTranslations) {
        const auto res = out.emplace(entry.jp, entry.en);
        if (res.second) inserted++;
    }
    return inserted;
}

std::u16string Utf8ToUtf16Le(const std::string& utf8)
{
    std::u16string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char b0 = static_cast<unsigned char>(utf8[i]);
        if (b0 <= kUtf8OneByteMax) {
            out.push_back(static_cast<char16_t>(b0));
            i += 1;
            continue;
        }
        if ((b0 & kUtf8ThreeBytePrefix) == kUtf8TwoBytePrefix &&
            i + 1 < utf8.size()) {
            const std::uint32_t b1 =
                static_cast<unsigned char>(utf8[i + 1]) & kUtf8TrailMask;
            const std::uint32_t cp =
                ((static_cast<std::uint32_t>(b0) & kUtf8TwoByteMask) << kUtf8Shift6) | b1;
            out.push_back(static_cast<char16_t>(cp));
            i += 2;
            continue;
        }
        if ((b0 & kUtf8FourBytePrefix) == kUtf8ThreeBytePrefix &&
            i + 2 < utf8.size()) {
            const std::uint32_t b1 =
                static_cast<unsigned char>(utf8[i + 1]) & kUtf8TrailMask;
            const std::uint32_t b2 =
                static_cast<unsigned char>(utf8[i + 2]) & kUtf8TrailMask;
            const std::uint32_t cp =
                ((static_cast<std::uint32_t>(b0) & kUtf8ThreeByteMask) << kUtf8Shift12) |
                (b1 << kUtf8Shift6) | b2;
            out.push_back(static_cast<char16_t>(cp));
            i += 3;
            continue;
        }
        if (i + 3 < utf8.size()) {
            const std::uint32_t b1 =
                static_cast<unsigned char>(utf8[i + 1]) & kUtf8TrailMask;
            const std::uint32_t b2 =
                static_cast<unsigned char>(utf8[i + 2]) & kUtf8TrailMask;
            const std::uint32_t b3 =
                static_cast<unsigned char>(utf8[i + 3]) & kUtf8TrailMask;
            const std::uint32_t cp =
                ((static_cast<std::uint32_t>(b0) & kUtf8FourByteMask) << kUtf8Shift18) |
                (b1 << kUtf8Shift12) | (b2 << kUtf8Shift6) | b3;
            const std::uint32_t adj = cp - kSurrogateBase;
            out.push_back(static_cast<char16_t>(
                kHighSurrogateStart + ((adj >> kSurrogateShift) & kSurrogateMask)));
            out.push_back(static_cast<char16_t>(
                kLowSurrogateStart + (adj & kSurrogateMask)));
            i += 4;
            continue;
        }
        i += 1;
    }
    return out;
}

std::string Utf16LeToUtf8(const std::u16string& utf16)
{
    std::string out;
    out.reserve(utf16.size() * 3);
    std::size_t i = 0;
    while (i < utf16.size()) {
        std::uint32_t cp = static_cast<std::uint16_t>(utf16[i]);
        i += 1;
        if (cp >= kHighSurrogateStart && cp <= kHighSurrogateEnd &&
            i < utf16.size()) {
            const std::uint32_t low = static_cast<std::uint16_t>(utf16[i]);
            if (low >= kLowSurrogateStart && low <= kLowSurrogateEnd) {
                cp = kSurrogateBase +
                     (((cp - kHighSurrogateStart) << kSurrogateShift) |
                      (low - kLowSurrogateStart));
                i += 1;
            }
        }
        if (cp <= kUtf8OneByteMax) {
            out.push_back(static_cast<char>(cp));
            continue;
        }
        if (cp <= kUtf8TwoByteMax) {
            out.push_back(static_cast<char>(
                kUtf8TwoBytePrefix | ((cp >> kUtf8Shift6) & kUtf8TwoByteMask)));
            out.push_back(static_cast<char>(
                kUtf8TrailPrefix | (cp & kUtf8TrailMask)));
            continue;
        }
        if (cp <= kUtf8ThreeByteMax) {
            out.push_back(static_cast<char>(
                kUtf8ThreeBytePrefix | ((cp >> kUtf8Shift12) & kUtf8ThreeByteMask)));
            out.push_back(static_cast<char>(
                kUtf8TrailPrefix | ((cp >> kUtf8Shift6) & kUtf8TrailMask)));
            out.push_back(static_cast<char>(
                kUtf8TrailPrefix | (cp & kUtf8TrailMask)));
            continue;
        }
        out.push_back(static_cast<char>(
            kUtf8FourBytePrefix | ((cp >> kUtf8Shift18) & kUtf8FourByteMask)));
        out.push_back(static_cast<char>(
            kUtf8TrailPrefix | ((cp >> kUtf8Shift12) & kUtf8TrailMask)));
        out.push_back(static_cast<char>(
            kUtf8TrailPrefix | ((cp >> kUtf8Shift6) & kUtf8TrailMask)));
        out.push_back(static_cast<char>(
            kUtf8TrailPrefix | (cp & kUtf8TrailMask)));
    }
    return out;
}

bool IsSupportedTextMarker(const unsigned char* data,
                           std::size_t dataSize,
                           std::size_t markerPos,
                           unsigned char& outPreByte)
{
    if (!data || markerPos < kMinMarkerOffset) return false;
    if (markerPos + kMarkerSpanBytes + kUtf16CodeUnitBytes > dataSize) return false;
    if (data[markerPos + 2] != kTextTokenByte) return false;

    const unsigned char b0 = data[markerPos];
    const unsigned char b1 = data[markerPos + 1];

    if (b0 == kExprTerminatorByte && b1 == kStandardRecordByte1) {
        const unsigned char preByte = data[markerPos - 1];
        const bool isMenuPreByte =
            (preByte >= kPreByteMenuFirst && preByte <= kPreByteMenuLast);
        if (preByte == kPreByteDialogue ||
            preByte == kPreByteSpeakerMisc ||
            preByte == kPreByteConfirmChoice ||
            preByte == kPreByteErrorModal ||
            isMenuPreByte) {
            outPreByte = preByte;
            return true;
        }
        return false;
    }

    if (b0 == kPreByteErrorModal && b1 == kStandardRecordByte1) {
        outPreByte = kPreByteErrorModal;
        return true;
    }

    if (b1 == kInlineChoiceHeaderTag ||
        b1 == kInlineLockedSlotTag ||
        b1 == kInlineSuffixPromptTag ||
        b1 == kInlineLockedTrackTag) {
        outPreByte = b1;
        return true;
    }

    return false;
}

int RelocateBytecodeJumps(const unsigned char* data,
                          std::size_t dataSize,
                          unsigned char* newData,
                          std::size_t newDataSize,
                          const std::vector<CumulativeDelta>& cumulative)
{
    if (!data || !newData) return -1;

    std::unordered_set<std::uint32_t> statementStarts;
    std::vector<std::size_t> jumpOffsets;
    if (!ParseDdwStatements(data, dataSize, statementStarts, jumpOffsets)) {
        return -1;
    }

    const OffsetTable tbl = FindOffsetTable(data, dataSize);
    int relocated = 0;

    for (const std::size_t origOff : jumpOffsets) {
        if (tbl.count > 0 && origOff == kHxbHeaderTablePtrOffset) continue;

        const std::uint32_t origTarget = ReadBE24(data + origOff);
        if (statementStarts.find(origTarget) == statementStarts.end()) continue;

        const int posDelta = ApplyCumulativeDelta(
            static_cast<std::uint32_t>(origOff), cumulative);
        const std::size_t newOff = static_cast<std::size_t>(
            static_cast<int>(origOff) + posDelta);
        if (newOff + kBe24ByteCount > newDataSize) return -1;

        const int targetDelta = ApplyCumulativeDelta(origTarget, cumulative);
        const std::uint32_t newTarget = static_cast<std::uint32_t>(
            static_cast<int>(origTarget) + targetDelta);
        WriteBE24(newData + newOff, newTarget);
        relocated++;
    }

    return relocated;
}

bool VerifyBytecodeJumps(const unsigned char* data,
                         std::size_t dataSize,
                         int& outJumpCount)
{
    outJumpCount = 0;
    if (!data || dataSize < kHxbBodyStartOffset) return false;
    if (ReadBE24(data + kHxbHeaderSizeFieldOffset) != dataSize) return false;

    std::unordered_set<std::uint32_t> statementStarts;
    std::vector<std::size_t> jumpOffsets;
    if (!ParseDdwStatements(data, dataSize, statementStarts, jumpOffsets)) {
        return false;
    }

    const OffsetTable tbl = FindOffsetTable(data, dataSize);
    for (const std::size_t off : jumpOffsets) {
        const std::uint32_t target = ReadBE24(data + off);
        if (tbl.count > 0 && off == kHxbHeaderTablePtrOffset) {
            if (target + kHeaderTablePtrBias != tbl.start) return false;
            outJumpCount++;
            continue;
        }
        if (statementStarts.find(target) == statementStarts.end()) return false;
        outJumpCount++;
    }

    return true;
}

HxbTranslationResult TranslateHxbScript(const unsigned char* data,
                                        std::size_t dataSize,
                                        const Utf8TranslationMap& translations,
                                        int wrapBudgetPx)
{
    HxbTranslationResult res;
    if (!data || dataSize == 0) return res;
    if (dataSize < kHxbBodyStartOffset + kMarkerSpanBytes) {
        res.data.assign(data, data + dataSize);
        return res;
    }

    struct PendingReplacement {
        std::size_t str_start;
        std::size_t str_end;
        std::vector<unsigned char> new_bytes;
        int delta;
    };

    std::vector<PendingReplacement> replacements;
    for (std::size_t i = kHxbBodyStartOffset + 1;
         i + kMarkerSpanBytes + 1 < dataSize;
         i++) {
        unsigned char preByte = 0;
        if (!IsSupportedTextMarker(data, dataSize, i, preByte)) continue;

        const std::size_t strStart = i + kMarkerSpanBytes;
        std::size_t pos = strStart;
        bool foundNull = false;
        std::u16string jp16;
        while (pos + 1 < dataSize) {
            const std::uint16_t ch = ReadLE16(data + pos);
            if (ch == 0) {
                foundNull = true;
                break;
            }
            jp16.push_back(static_cast<char16_t>(ch));
            pos += kUtf16CodeUnitBytes;
        }
        if (!foundNull || jp16.empty()) continue;

        const std::size_t strEnd = pos;
        const std::string jpUtf8 = Utf16LeToUtf8(jp16);
        const auto it = translations.find(jpUtf8);
        if (it == translations.end()) continue;

        std::u16string en16 = Utf8ToUtf16Le(it->second);
        if (preByte == kPreByteDialogue && wrapBudgetPx > 0) {
            std::wstring wideEn(en16.begin(), en16.end());
            WordWrapMessage(wideEn, wrapBudgetPx);
            en16.assign(wideEn.begin(), wideEn.end());
        }

        std::vector<unsigned char> newBytes(en16.size() * kUtf16CodeUnitBytes);
        for (std::size_t c = 0; c < en16.size(); c++) {
            WriteLE16(newBytes.data() + (c * kUtf16CodeUnitBytes),
                      static_cast<std::uint16_t>(en16[c]));
        }

        const int delta = static_cast<int>(newBytes.size()) -
                          static_cast<int>(strEnd - strStart);
        replacements.push_back({strStart, strEnd, std::move(newBytes), delta});
        i = strEnd + 1;
    }

    if (replacements.empty()) {
        res.data.assign(data, data + dataSize);
        return res;
    }

    int totalDelta = 0;
    for (const auto& r : replacements) totalDelta += r.delta;
    const std::size_t newDataSize = static_cast<std::size_t>(
        static_cast<int>(dataSize) + totalDelta);

    res.data.resize(newDataSize);
    std::size_t srcPos = 0;
    std::size_t dstPos = 0;
    for (const auto& r : replacements) {
        const std::size_t copyLen = r.str_start - srcPos;
        std::memcpy(res.data.data() + dstPos, data + srcPos, copyLen);
        dstPos += copyLen;

        std::memcpy(res.data.data() + dstPos,
                    r.new_bytes.data(),
                    r.new_bytes.size());
        dstPos += r.new_bytes.size();
        srcPos = r.str_end;
    }
    const std::size_t remainLen = dataSize - srcPos;
    std::memcpy(res.data.data() + dstPos, data + srcPos, remainLen);

    int cum = 0;
    for (const auto& r : replacements) {
        const std::uint32_t afterNull =
            static_cast<std::uint32_t>(r.str_end + kUtf16CodeUnitBytes);
        cum += r.delta;
        res.cumulative.push_back({afterNull, cum});
    }

    res.strings_replaced = static_cast<int>(replacements.size());

    const OffsetTable tbl = FindOffsetTable(data, dataSize);
    if (tbl.count > 0) {
        const std::size_t origTableStart = tbl.start;
        const std::size_t newTableStart = static_cast<std::size_t>(
            static_cast<int>(origTableStart) + totalDelta);
        for (int e = 0; e < tbl.count; e++) {
            const std::size_t origEpos =
                origTableStart + static_cast<std::size_t>(e) * kBe24ByteCount;
            const std::size_t newEpos =
                newTableStart + static_cast<std::size_t>(e) * kBe24ByteCount;
            if (newEpos + kBe24ByteCount > newDataSize) break;

            const std::uint32_t val = ReadBE24(data + origEpos);
            const int adj = ApplyCumulativeDelta(val, res.cumulative);
            const std::uint32_t newVal =
                static_cast<std::uint32_t>(static_cast<int>(val) + adj);
            WriteBE24(res.data.data() + newEpos, newVal);
            res.offset_table_entries_relocated++;
        }

        if (newDataSize >= kHxbMinHeaderTablePatchSize &&
            res.data[kHxbBodyStartOffset] == kOpJumpZero &&
            res.data[kHxbHeaderTableFlagOffset] == kExprTerminatorByte) {
            const std::uint32_t oldPtr =
                ReadBE24(res.data.data() + kHxbHeaderTablePtrOffset);
            const std::uint32_t newPtr =
                static_cast<std::uint32_t>(static_cast<int>(oldPtr) + totalDelta);
            WriteBE24(res.data.data() + kHxbHeaderTablePtrOffset, newPtr);
        }
    }

    res.choice_entries_relocated = RelocateChoiceTables(
        data, dataSize, res.data.data(), newDataSize, res.cumulative);

    res.bytecode_jumps_relocated = RelocateBytecodeJumps(
        data, dataSize, res.data.data(), newDataSize, res.cumulative);

    WriteBE24(res.data.data() + kHxbHeaderSizeFieldOffset,
              static_cast<std::uint32_t>(newDataSize));
    return res;
}

}  // namespace translator_logic
