// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_autopatch.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                std::filesystem::u8path(name)) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

using translator_logic::FindOffsetTable;
using translator_logic::HxbTranslationResult;
using translator_logic::IsSupportedTextMarker;
using translator_logic::OffsetTable;
using translator_logic::PopulateSystemTranslations;
using translator_logic::ReadBE24;
using translator_logic::TranslateHxbScript;
using translator_logic::Utf16LeToUtf8;
using translator_logic::Utf8ToUtf16Le;
using translator_logic::Utf8TranslationMap;
using translator_logic::VerifyBytecodeJumps;
using translator_logic::WriteBE24;

using Bytes = std::vector<std::uint8_t>;

constexpr int kExpectedSystemMapSize          = 30;
constexpr int kDisableDialogueWordWrap        = 0;
constexpr int kSingleExpectedItem             = 1;
constexpr std::size_t kMarkerIndex            = 2;
constexpr std::size_t kSyntheticHeader        = 0x10;
constexpr std::size_t kHeaderSizeOffset       = 0x08;
constexpr std::size_t kHeaderPtrOffset        = 0x16;
constexpr std::size_t kMarkerTailOffsetBias   = 3;
constexpr std::uint32_t kHeaderPtrBias        = 5;
constexpr unsigned int kByteShift8            = 8;
constexpr unsigned int kByteShift16           = 16;
constexpr unsigned int kByteShift24           = 24;
constexpr unsigned int kByteMask              = 0xFF;

constexpr unsigned char kZeroByte             = 0x00;
constexpr unsigned char kExprTermByte         = 0xFF;
constexpr unsigned char kStdRecordByte1       = 0x01;
constexpr unsigned char kTextTokenByte        = 0x80;
constexpr unsigned char kImm16TokenByte       = 0x0E;
constexpr unsigned char kEqualOpByte          = 0x50;
constexpr unsigned char kSampleAsciiByte      = 0x41;
constexpr unsigned char kChoiceContextByte0   = 0x2E;
constexpr unsigned char kChoiceContextByte1   = 0x63;
constexpr unsigned char kLockedContextByte    = 0xBE;

constexpr unsigned char kOpNop                = 0x39;
constexpr unsigned char kOpJumpZero           = 0x26;
constexpr unsigned char kOpJump               = 0x29;
constexpr unsigned char kOpDialogue           = 0x36;
constexpr unsigned char kOpConfirm            = 0x01;
constexpr unsigned char kOpErrorModal         = 0x02;
constexpr unsigned char kOpMenuFirst          = 0xB0;
constexpr unsigned char kOpMenuLast           = 0xBA;
constexpr unsigned char kInlineChoice         = 0x7E;
constexpr unsigned char kInlineLocked         = 0x1F;
constexpr unsigned char kInlineSuffix         = 0x60;
constexpr unsigned char kInlineTrack          = 0xA0;
constexpr unsigned char kUnsupportedPre       = 0x03;

constexpr std::size_t kDdp2HeaderSize         = 32;
constexpr std::size_t kDdp2EntrySize          = 16;
constexpr std::uint8_t kShsLiteral32Opcode    = 0x1FU;
constexpr std::size_t kWordBytes              = 4;
constexpr std::size_t kRemThreeBytes          = 3;
constexpr std::size_t kRemTwoBytes            = 2;
constexpr std::size_t kRemOneByte             = 1;
constexpr std::size_t kByteOffset1            = 1;
constexpr std::size_t kByteOffset2            = 2;
constexpr std::size_t kByteOffset3            = 3;
constexpr std::uint32_t kMurmurMultiplier     = 0x5BD1E995U;
constexpr int kMurmurShiftR                   = 24;
constexpr int kMurmurFinalShift1              = 13;
constexpr int kMurmurFinalShift2              = 15;
constexpr std::uint32_t kPatchFormatVersion   = 1U;
constexpr std::uint32_t kArchiveIdSinSysd     = 0U;
constexpr std::uint32_t kArchiveIdBootup      = 1U;
constexpr std::uint32_t kXorStreamMultiplier  = 0x9EU;
constexpr std::uint32_t kXorStreamAddend      = 0x37U;
constexpr std::uint32_t kHashByteIndexMask    = 3U;
constexpr std::size_t kFirstRecordOrigHashOff = 44U;
constexpr std::uint8_t kTamperFlipMask        = 0xFFU;
constexpr std::uint32_t kTestZeroEntries      = 0U;
constexpr std::uint32_t kTestOneEntry         = 1U;
constexpr std::uint32_t kTestTwoEntries       = 2U;
constexpr std::uint32_t kEntryIndexZero       = 0U;

std::uint32_t ReadU32Le(const Bytes& b, std::size_t off)
{
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + kByteOffset1]) << kByteShift8) |
           (static_cast<std::uint32_t>(b[off + kByteOffset2]) << kByteShift16) |
           (static_cast<std::uint32_t>(b[off + kByteOffset3]) << kByteShift24);
}

void AppendU32Le(Bytes& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift8) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift16) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByteShift24) & kByteMask));
}

Bytes ShsWrap(const Bytes& raw)
{
    Bytes out;
    out.push_back(kShsLiteral32Opcode);
    const auto n = static_cast<std::uint32_t>(raw.size());
    out.push_back(static_cast<std::uint8_t>((n >> kByteShift24) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((n >> kByteShift16) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((n >> kByteShift8) & kByteMask));
    out.push_back(static_cast<std::uint8_t>(n & kByteMask));
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

std::uint32_t MurmurHash2(const Bytes& data)
{
    std::uint32_t h = static_cast<std::uint32_t>(data.size());
    std::size_t i = 0;
    while (i + kWordBytes <= data.size()) {
        std::uint32_t k = ReadU32Le(data, i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }
    const std::size_t rem = data.size() - i;
    if (rem == kRemThreeBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset2]) << kByteShift16;
    }
    if (rem >= kRemTwoBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset1]) << kByteShift8;
    }
    if (rem >= kRemOneByte) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

Bytes BuildTestDdp2(const std::vector<Bytes>& raw_entries)
{
    Bytes out = {'D', 'D', 'P', '2'};
    AppendU32Le(out, static_cast<std::uint32_t>(raw_entries.size()));
    while (out.size() < kDdp2HeaderSize) {
        out.push_back(0);
    }
    std::vector<Bytes> packed_entries;
    packed_entries.reserve(raw_entries.size());
    for (const Bytes& raw : raw_entries) {
        packed_entries.push_back(ShsWrap(raw));
    }
    std::uint32_t cursor = static_cast<std::uint32_t>(
        kDdp2HeaderSize + raw_entries.size() * kDdp2EntrySize);
    for (std::size_t i = 0; i < raw_entries.size(); ++i) {
        AppendU32Le(out, cursor);
        AppendU32Le(out, static_cast<std::uint32_t>(raw_entries[i].size()));
        AppendU32Le(out, static_cast<std::uint32_t>(packed_entries[i].size()));
        AppendU32Le(out, 0);
        cursor += static_cast<std::uint32_t>(packed_entries[i].size());
    }
    for (const Bytes& packed : packed_entries) {
        out.insert(out.end(), packed.begin(), packed.end());
    }
    return out;
}

Bytes BuildTestShinMenuPatch(const Bytes& orig_sin_sysd,
                             std::uint32_t sin_sysd_total_entries,
                             const Bytes& orig_sin_sysd_stored0,
                             const Bytes& repl_sin_sysd0,
                             const Bytes& orig_bootup,
                             std::uint32_t bootup_total_entries,
                             const Bytes& orig_bootup_stored0,
                             const Bytes& repl_bootup0)
{
    Bytes out = {'S', 'H', 'I', 'N', 'M', 'E', 'N', 'U'};
    AppendU32Le(out, kPatchFormatVersion);
    AppendU32Le(out, sin_sysd_total_entries);
    AppendU32Le(out, static_cast<std::uint32_t>(orig_sin_sysd.size()));
    AppendU32Le(out, kTestOneEntry);
    AppendU32Le(out, bootup_total_entries);
    AppendU32Le(out, static_cast<std::uint32_t>(orig_bootup.size()));
    AppendU32Le(out, orig_bootup.empty() ? kTestZeroEntries : kTestOneEntry);

    auto appendRecord = [&](std::uint32_t archive_id,
                            std::uint32_t entry_idx,
                            const Bytes& orig_stored,
                            const Bytes& repl_unpacked) {
        const std::uint32_t orig_hash = MurmurHash2(orig_stored);
        const std::uint32_t new_hash = MurmurHash2(repl_unpacked);
        AppendU32Le(out, archive_id);
        AppendU32Le(out, entry_idx);
        AppendU32Le(out, orig_hash);
        AppendU32Le(out, static_cast<std::uint32_t>(repl_unpacked.size()));
        AppendU32Le(out, new_hash);
        for (std::size_t k = 0; k < repl_unpacked.size(); ++k) {
            const auto k32 = static_cast<std::uint32_t>(k);
            const std::uint32_t shift = (k32 & kHashByteIndexMask) * kByteShift8;
            const auto hash_byte = static_cast<std::uint8_t>((orig_hash >> shift) & kByteMask);
            const auto stream_byte = static_cast<std::uint8_t>(
                (k32 * kXorStreamMultiplier + kXorStreamAddend) & kByteMask);
            const auto mask = static_cast<std::uint8_t>(
                orig_stored[k % orig_stored.size()] ^ hash_byte ^ stream_byte);
            out.push_back(static_cast<std::uint8_t>(repl_unpacked[k] ^ mask));
        }
    };

    appendRecord(kArchiveIdSinSysd, kEntryIndexZero, orig_sin_sysd_stored0, repl_sin_sysd0);
    if (!orig_bootup.empty()) {
        appendRecord(kArchiveIdBootup, kEntryIndexZero, orig_bootup_stored0, repl_bootup0);
    }
    return out;
}

Bytes LoadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize size = in.tellg();
    if (size <= 0) return {};
    in.seekg(0, std::ios::beg);
    Bytes buf(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(buf.data()), size)) return {};
    return buf;
}

void WriteBinaryFile(const std::filesystem::path& path, const Bytes& data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

void AppendUtf16LeWithNull(std::vector<unsigned char>& buf,
                           const std::string& utf8)
{
    const std::u16string u16 = Utf8ToUtf16Le(utf8);
    for (const char16_t ch : u16) {
        const auto v = static_cast<std::uint16_t>(ch);
        buf.push_back(static_cast<unsigned char>(v & kByteMask));
        buf.push_back(
            static_cast<unsigned char>((v >> kByteShift8) & kByteMask));
    }
    buf.push_back(kZeroByte);
    buf.push_back(kZeroByte);
}

}  // namespace

TEST(SystemTranslationsTest, PopulatesAllBootupSystemStringsAndPreservesOverrides)
{
    Utf8TranslationMap map;
    const int inserted = PopulateSystemTranslations(map);
    EXPECT_EQ(inserted, kExpectedSystemMapSize);
    EXPECT_EQ(static_cast<int>(map.size()), kExpectedSystemMapSize);

    EXPECT_EQ(map["セーブ"], "Save");
    EXPECT_EQ(map["ロード"], "Load");
    EXPECT_EQ(map["【選択肢】"], "[Choice]");
    EXPECT_EQ(map["？？？？"], "????");

    Utf8TranslationMap withOverride;
    withOverride["セーブ"] = "Custom Save";
    const int insertedWithOverride = PopulateSystemTranslations(withOverride);
    EXPECT_EQ(insertedWithOverride, kExpectedSystemMapSize - kSingleExpectedItem);
    EXPECT_EQ(withOverride["セーブ"], "Custom Save");
}

TEST(SystemTranslationsTest, Utf8AndUtf16LeRoundTripPreservesJapaneseAndAscii)
{
    const std::string original = "『神学校-Noli me tangere-』";
    const std::u16string wide = Utf8ToUtf16Le(original);
    EXPECT_FALSE(wide.empty());
    const std::string roundTrip = Utf16LeToUtf8(wide);
    EXPECT_EQ(roundTrip, original);
}

TEST(MarkerDetectionTest, RecognizesAllStandardAndBootupInlineMarkers)
{
    auto checkMarker = [](unsigned char b0,
                          unsigned char b1,
                          unsigned char b2,
                          unsigned char b3,
                          bool expectedValid,
                          unsigned char expectedPre) {
        const std::vector<unsigned char> buf = {
            kZeroByte, b0, b1, b2, b3, kSampleAsciiByte,
            kZeroByte, kZeroByte, kZeroByte
        };
        unsigned char pre = kZeroByte;
        const bool ok = IsSupportedTextMarker(
            buf.data(), buf.size(), kMarkerIndex, pre);
        EXPECT_EQ(ok, expectedValid);
        if (expectedValid) {
            EXPECT_EQ(pre, expectedPre);
        }
    };

    checkMarker(
        kOpDialogue, kExprTermByte, kStdRecordByte1, kTextTokenByte,
        true, kOpDialogue);
    checkMarker(
        kOpConfirm, kExprTermByte, kStdRecordByte1, kTextTokenByte,
        true, kOpConfirm);
    checkMarker(
        kOpErrorModal, kExprTermByte, kStdRecordByte1, kTextTokenByte,
        true, kOpErrorModal);

    for (unsigned char menuOp = kOpMenuFirst; menuOp <= kOpMenuLast; menuOp++) {
        checkMarker(
            menuOp, kExprTermByte, kStdRecordByte1, kTextTokenByte,
            true, menuOp);
    }

    checkMarker(
        kZeroByte, kOpErrorModal, kStdRecordByte1, kTextTokenByte,
        true, kOpErrorModal);
    checkMarker(
        kChoiceContextByte0, kChoiceContextByte1, kInlineChoice, kTextTokenByte,
        true, kInlineChoice);
    checkMarker(
        kZeroByte, kLockedContextByte, kInlineLocked, kTextTokenByte,
        true, kInlineLocked);
    checkMarker(
        kZeroByte, kInlineSuffix, kInlineSuffix, kTextTokenByte,
        true, kInlineSuffix);
    checkMarker(
        kZeroByte, kZeroByte, kInlineTrack, kTextTokenByte,
        true, kInlineTrack);

    checkMarker(
        kUnsupportedPre, kExprTermByte, kStdRecordByte1, kTextTokenByte,
        false, kZeroByte);
}

TEST(HxbRelocationTest, TranslatesScriptAndRelocatesJumpsAndTailOffsetTable)
{
    std::vector<unsigned char> script(kSyntheticHeader, kZeroByte);

    const std::vector<unsigned char> headerStmt = {
        kOpJumpZero, kImm16TokenByte, kZeroByte, kZeroByte,
        kEqualOpByte, kExprTermByte, kZeroByte, kZeroByte, kZeroByte
    };
    script.insert(script.end(), headerStmt.begin(), headerStmt.end());

    const std::size_t jmpOperandPos = script.size() + 1;
    script.push_back(kOpJump);
    script.push_back(kZeroByte);
    script.push_back(kZeroByte);
    script.push_back(kZeroByte);

    script.push_back(kOpConfirm);
    script.push_back(kZeroByte);
    script.push_back(kOpDialogue);
    const std::size_t markerPos = script.size();
    const std::size_t textEntryOffset = markerPos - kMarkerTailOffsetBias;
    script.push_back(kExprTermByte);
    script.push_back(kStdRecordByte1);
    script.push_back(kTextTokenByte);
    AppendUtf16LeWithNull(script, "セーブ");
    script.push_back(kExprTermByte);

    const std::size_t targetStmtPos = script.size();
    script.push_back(kOpNop);

    const std::size_t tableStartPos = script.size();
    script.push_back(kZeroByte);
    script.push_back(kZeroByte);
    script.push_back(kZeroByte);
    script.push_back(kExprTermByte);

    WriteBE24(script.data() + kHeaderPtrOffset,
              static_cast<std::uint32_t>(tableStartPos - kHeaderPtrBias));
    WriteBE24(script.data() + jmpOperandPos,
              static_cast<std::uint32_t>(targetStmtPos));
    WriteBE24(script.data() + tableStartPos,
              static_cast<std::uint32_t>(textEntryOffset));
    WriteBE24(script.data() + kHeaderSizeOffset,
              static_cast<std::uint32_t>(script.size()));

    int origJumps = 0;
    ASSERT_TRUE(VerifyBytecodeJumps(script.data(), script.size(), origJumps));

    Utf8TranslationMap map;
    map["セーブ"] = "Save Data Now";

    const HxbTranslationResult res = TranslateHxbScript(
        script.data(), script.size(), map, kDisableDialogueWordWrap);

    EXPECT_EQ(res.strings_replaced, kSingleExpectedItem);
    EXPECT_EQ(res.offset_table_entries_relocated, kSingleExpectedItem);
    EXPECT_EQ(res.bytecode_jumps_relocated, kSingleExpectedItem);

    int newJumps = 0;
    EXPECT_TRUE(VerifyBytecodeJumps(res.data.data(), res.data.size(), newJumps));

    const OffsetTable newTbl = FindOffsetTable(res.data.data(), res.data.size());
    EXPECT_EQ(newTbl.count, kSingleExpectedItem);
    EXPECT_EQ(ReadBE24(res.data.data() + kHeaderPtrOffset) + kHeaderPtrBias,
              newTbl.start);
}

TEST(MenuAutoPatchTest, ReconstructsPatchedDdp2ArchivesInMemory)
{
    const Bytes orig_s0 = {'S', 'Y', 'S', 'D', '_', 'O', 'R', 'I', 'G', '_', '0'};
    const Bytes orig_s1 = {'S', 'Y', 'S', 'D', '_', 'K', 'E', 'E', 'P', '_', '1'};
    const Bytes repl_s0 = {'S', 'Y', 'S', 'D', '_', 'E', 'N', '_', 'P', 'A', 'T', 'C', 'H'};

    const Bytes orig_b0 = {'B', 'O', 'O', 'T', '_', 'O', 'R', 'I', 'G'};
    const Bytes repl_b0 = {'B', 'O', 'O', 'T', '_', 'E', 'N', '_', 'P', 'A', 'T', 'C', 'H'};

    const Bytes orig_sin_sysd = BuildTestDdp2({orig_s0, orig_s1});
    const Bytes orig_bootup = BuildTestDdp2({orig_b0});
    const Bytes expected_sin_sysd = BuildTestDdp2({repl_s0, orig_s1});
    const Bytes expected_bootup = BuildTestDdp2({repl_b0});

    const Bytes patch_blob = BuildTestShinMenuPatch(
        orig_sin_sysd,
        kTestTwoEntries,
        ShsWrap(orig_s0),
        repl_s0,
        orig_bootup,
        kTestOneEntry,
        ShsWrap(orig_b0),
        repl_b0);

    Bytes actual_sin_sysd;
    Bytes actual_bootup;
    EXPECT_TRUE(menu_autopatch::ApplyXorMenuPatchInMemory(
        orig_sin_sysd, orig_bootup, patch_blob, actual_sin_sysd, actual_bootup));
    EXPECT_EQ(actual_sin_sysd, expected_sin_sysd);
    EXPECT_EQ(actual_bootup, expected_bootup);
}

TEST(MenuAutoPatchTest, RejectsTamperedOrigHashOrCorruptedXorPayload)
{
    const Bytes orig_s0 = {'S', 'Y', 'S', 'D', '_', 'O', 'R', 'I', 'G', '_', '0'};
    const Bytes repl_s0 = {'S', 'Y', 'S', 'D', '_', 'E', 'N', '_', 'P', 'A', 'T', 'C', 'H'};
    const Bytes orig_sin_sysd = BuildTestDdp2({orig_s0});
    const Bytes empty_bootup;

    const Bytes valid_patch = BuildTestShinMenuPatch(
        orig_sin_sysd,
        kTestOneEntry,
        ShsWrap(orig_s0),
        repl_s0,
        empty_bootup,
        kTestZeroEntries,
        {},
        {});

    Bytes bad_orig_hash = valid_patch;
    bad_orig_hash[kFirstRecordOrigHashOff] ^= kTamperFlipMask;
    Bytes out_sin_sysd;
    Bytes out_bootup;
    EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(
        orig_sin_sysd, empty_bootup, bad_orig_hash, out_sin_sysd, out_bootup));

    Bytes bad_payload = valid_patch;
    bad_payload.back() ^= kTamperFlipMask;
    EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(
        orig_sin_sysd, empty_bootup, bad_payload, out_sin_sysd, out_bootup));
}

TEST(MenuAutoPatchTest, EnsureMenuArchivePatchedInDirCreatesBackupsPatchesArchivesAndIsIdempotent)
{
    const ScopedTempDir tmp("shin_menu_autopatch_ut");
    const std::filesystem::path& temp_dir = tmp.path();
    std::filesystem::create_directories(temp_dir / "Data");

    const Bytes orig_s0 = {'S', 'Y', 'S', 'D', '_', 'O', 'R', 'I', 'G', '_', '0'};
    const Bytes repl_s0 = {'S', 'Y', 'S', 'D', '_', 'E', 'N', '_', 'P', 'A', 'T', 'C', 'H'};
    const Bytes orig_b0 = {'B', 'O', 'O', 'T', '_', 'O', 'R', 'I', 'G'};
    const Bytes repl_b0 = {'B', 'O', 'O', 'T', '_', 'E', 'N', '_', 'P', 'A', 'T', 'C', 'H'};

    const Bytes orig_sin_sysd = BuildTestDdp2({orig_s0});
    const Bytes orig_bootup = BuildTestDdp2({orig_b0});
    const Bytes expected_sin_sysd = BuildTestDdp2({repl_s0});
    const Bytes expected_bootup = BuildTestDdp2({repl_b0});

    const Bytes patch_blob = BuildTestShinMenuPatch(
        orig_sin_sysd,
        kTestOneEntry,
        ShsWrap(orig_s0),
        repl_s0,
        orig_bootup,
        kTestOneEntry,
        ShsWrap(orig_b0),
        repl_b0);

    const std::filesystem::path sin_sysd_path = temp_dir / "Data" / "sin_sysd.dat";
    const std::filesystem::path sin_sysd_bak = temp_dir / "Data" / "sin_sysd.dat.bak";
    const std::filesystem::path bootup_path = temp_dir / "Bootup.dat";
    const std::filesystem::path bootup_bak = temp_dir / "Bootup.dat.bak";
    const std::filesystem::path patch_path = temp_dir / "menu_patch.bin";

    WriteBinaryFile(sin_sysd_path, orig_sin_sysd);
    WriteBinaryFile(bootup_path, orig_bootup);
    WriteBinaryFile(patch_path, patch_blob);

    EXPECT_TRUE(menu_autopatch::EnsureMenuArchivePatchedInDir(temp_dir.wstring()));
    ASSERT_TRUE(std::filesystem::exists(sin_sysd_bak));
    ASSERT_TRUE(std::filesystem::exists(bootup_bak));
    EXPECT_EQ(LoadBinaryFile(sin_sysd_bak), orig_sin_sysd);
    EXPECT_EQ(LoadBinaryFile(bootup_bak), orig_bootup);
    EXPECT_EQ(LoadBinaryFile(sin_sysd_path), expected_sin_sysd);
    EXPECT_EQ(LoadBinaryFile(bootup_path), expected_bootup);

    EXPECT_TRUE(menu_autopatch::EnsureMenuArchivePatchedInDir(temp_dir.wstring()));
    EXPECT_EQ(LoadBinaryFile(sin_sysd_bak), orig_sin_sysd);
    EXPECT_EQ(LoadBinaryFile(bootup_bak), orig_bootup);
    EXPECT_EQ(LoadBinaryFile(sin_sysd_path), expected_sin_sysd);
    EXPECT_EQ(LoadBinaryFile(bootup_path), expected_bootup);
}
