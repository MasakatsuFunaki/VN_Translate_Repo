// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"
#include "menu_autopatch.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

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
constexpr std::size_t kUtf16CodeUnitBytes     = 2;
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

constexpr std::size_t kBootupIndex00          = 0;
constexpr std::size_t kBootupIndex03          = 3;
constexpr std::size_t kBootupIndex09          = 9;
constexpr std::size_t kBootupIndex10          = 10;
constexpr std::size_t kBootupIndex12          = 12;
constexpr std::size_t kBootupIndex13          = 13;
constexpr std::size_t kBootupIndex14          = 14;
constexpr std::size_t kBootupIndex15          = 15;

constexpr int kExpectedStringsBootup00        = 1;
constexpr int kExpectedStringsBootup03        = 1;
constexpr int kExpectedStringsBootup09        = 1;
constexpr int kExpectedStringsBootup10        = 14;
constexpr int kExpectedStringsBootup12        = 5;
constexpr int kExpectedStringsBootup13        = 5;
constexpr int kExpectedStringsBootup14        = 2;
constexpr int kExpectedStringsBootup15        = 2;

constexpr int kExpectedJumpsBootup00          = 28;
constexpr int kExpectedJumpsBootup03          = 62;
constexpr int kExpectedJumpsBootup09          = 58;
constexpr int kExpectedJumpsBootup10          = 59;
constexpr int kExpectedJumpsBootup12          = 226;
constexpr int kExpectedJumpsBootup13          = 145;
constexpr int kExpectedJumpsBootup14          = 45;
constexpr int kExpectedJumpsBootup15          = 168;

constexpr std::size_t kDdp2HeaderSize         = 32;
constexpr std::size_t kDdp2EntrySize          = 16;
constexpr std::size_t kDdp2UnpackedOff        = 4;
constexpr std::size_t kDdp2PackedOff          = 8;
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

constexpr std::size_t kHxbMinEncryptedBytes   = 0x11;
constexpr std::size_t kHxbBodyOffset          = 0x10;
constexpr std::size_t kHxbLenHighIdx          = 8;
constexpr std::size_t kHxbLenMidIdx           = 9;
constexpr std::size_t kHxbLenLowIdx           = 10;
constexpr int kHxbKeyShift                    = 5;
constexpr std::uint32_t kHxbKeyXorA           = 0xA5U;
constexpr std::uint32_t kHxbKeyAddB           = 0x6F349U;
constexpr std::uint32_t kHxbKeyXorC           = 0x34A9B129U;
constexpr std::size_t kKeyByteCount           = 4;

constexpr std::uint8_t kShsLiteralLimit       = 32;
constexpr std::uint8_t kShsLiteral1D          = 0x1DU;
constexpr std::uint8_t kShsLiteral1E          = 0x1EU;
constexpr std::int64_t kShsBias1D             = 0x1E;
constexpr std::int64_t kShsBias1E             = 0x11E;
constexpr std::uint8_t kShsHighBit            = 0x80U;
constexpr std::uint8_t kShsModeMask           = 0x60U;
constexpr std::uint8_t kShsMode20             = 0x20U;
constexpr std::uint8_t kShsMode40             = 0x40U;
constexpr std::uint8_t kShsCountMask3         = 3U;
constexpr std::uint8_t kShsOffsetMask7        = 7U;
constexpr std::uint8_t kShsMask1F             = 0x1FU;
constexpr std::uint8_t kShsExtFE              = 0xFEU;
constexpr std::uint8_t kShsExtFF              = 0xFFU;
constexpr std::int64_t kShsBias102            = 0x102;
constexpr std::int64_t kShsBackrefBias4       = 4;
constexpr std::int64_t kShsBackrefMinCount    = 3;
constexpr std::int64_t kShsBackrefMinOffset   = 1;
constexpr std::int64_t kShsSingleByteStep     = 1;
constexpr std::int64_t kShsTwoByteStep        = 2;
constexpr int kShsShift2                      = 2;
constexpr int kShsShift5                      = 5;

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

Bytes ShsDecompress(const Bytes& src, std::uint32_t decomp_size)
{
    Bytes output(decomp_size, 0);
    const auto out_size = static_cast<std::int64_t>(decomp_size);
    const auto src_size = static_cast<std::int64_t>(src.size());
    std::int64_t dst = 0;
    std::int64_t si = 0;

    while (dst < out_size && si < src_size) {
        const std::uint8_t ctl = src[static_cast<std::size_t>(si)];
        ++si;
        std::int64_t count = 0;

        if (ctl < kShsLiteralLimit) {
            if (ctl == kShsLiteral1D) {
                count = src[static_cast<std::size_t>(si)] + kShsBias1D;
                si += kShsSingleByteStep;
            } else if (ctl == kShsLiteral1E) {
                count = ((src[static_cast<std::size_t>(si)] << kByteShift8) |
                         src[static_cast<std::size_t>(si) + kByteOffset1]) + kShsBias1E;
                si += kShsTwoByteStep;
            } else if (ctl == kShsLiteral32Opcode) {
                count = (static_cast<std::int64_t>(src[static_cast<std::size_t>(si)]) << kByteShift24) |
                        (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset1]) << kByteShift16) |
                        (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset2]) << kByteShift8) |
                        static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset3]);
                si += kWordBytes;
            } else {
                count = ctl + kShsSingleByteStep;
            }

            count = std::min(count, out_size - dst);
            for (std::int64_t i = 0; i < count; ++i) {
                output[static_cast<std::size_t>(dst + i)] =
                    src[static_cast<std::size_t>(si + i)];
            }
            si += count;
            dst += count;
            continue;
        }

        std::int64_t offset = 0;
        if ((ctl & kShsHighBit) == 0) {
            if ((ctl & kShsModeMask) == kShsMode20) {
                offset = (ctl >> kShsShift2) & kShsOffsetMask7;
                count = ctl & kShsCountMask3;
            } else if ((ctl & kShsModeMask) == kShsMode40) {
                offset = src[static_cast<std::size_t>(si)];
                si += kShsSingleByteStep;
                count = (ctl & kShsMask1F) + kShsBackrefBias4;
            } else {
                offset = src[static_cast<std::size_t>(si)];
                si += kShsSingleByteStep;
                offset |= static_cast<std::int64_t>(ctl & kShsMask1F) << kByteShift8;
                const std::uint8_t ctl2 = src[static_cast<std::size_t>(si)];
                si += kShsSingleByteStep;
                if (ctl2 == kShsExtFE) {
                    count = ((src[static_cast<std::size_t>(si)] << kByteShift8) |
                             src[static_cast<std::size_t>(si) + kByteOffset1]) + kShsBias102;
                    si += kShsTwoByteStep;
                } else if (ctl2 == kShsExtFF) {
                    count =
                        (static_cast<std::int64_t>(src[static_cast<std::size_t>(si)]) << kByteShift24) |
                        (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset1]) << kByteShift16) |
                        (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset2]) << kByteShift8) |
                        static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset3]);
                    si += kWordBytes;
                } else {
                    count = ctl2 + kShsBackrefBias4;
                }
            }
        } else {
            count = (ctl >> kShsShift5) & kShsCountMask3;
            offset = (static_cast<std::int64_t>(ctl & kShsMask1F) << kByteShift8) |
                     src[static_cast<std::size_t>(si)];
            si += kShsSingleByteStep;
        }

        count += kShsBackrefMinCount;
        offset += kShsBackrefMinOffset;
        count = std::min(count, out_size - dst);
        const std::int64_t src_pos = dst - offset;
        for (std::int64_t i = 0; i < count; ++i) {
            output[static_cast<std::size_t>(dst + i)] =
                output[static_cast<std::size_t>(src_pos + i)];
        }
        dst += count;
    }

    output.resize(static_cast<std::size_t>(dst));
    return output;
}

Bytes DecryptHxb(const Bytes& data)
{
    if (data.size() < kHxbMinEncryptedBytes) {
        return data;
    }
    const std::uint32_t length =
        (static_cast<std::uint32_t>(data[kHxbLenHighIdx]) << kByteShift16) |
        (static_cast<std::uint32_t>(data[kHxbLenMidIdx]) << kByteShift8) |
        static_cast<std::uint32_t>(data[kHxbLenLowIdx]);
    const std::uint32_t key =
        (((length << kHxbKeyShift) ^ kHxbKeyXorA) * (length + kHxbKeyAddB)) ^
        kHxbKeyXorC;
    const std::uint8_t kb[kKeyByteCount] = {
        static_cast<std::uint8_t>(key & kByteMask),
        static_cast<std::uint8_t>((key >> kByteShift8) & kByteMask),
        static_cast<std::uint8_t>((key >> kByteShift16) & kByteMask),
        static_cast<std::uint8_t>((key >> kByteShift24) & kByteMask)};

    Bytes dec = data;
    for (std::size_t i = kHxbBodyOffset; i < dec.size(); ++i) {
        dec[i] ^= kb[i & (kKeyByteCount - 1)];
    }
    return dec;
}

Bytes ExtractDdp2EntryDecompressed(const Bytes& ddp2, std::size_t entry_index)
{
    const std::size_t meta_off = kDdp2HeaderSize + entry_index * kDdp2EntrySize;
    const std::uint32_t offset = ReadU32Le(ddp2, meta_off);
    const std::uint32_t unpacked = ReadU32Le(ddp2, meta_off + kDdp2UnpackedOff);
    const std::uint32_t packed = ReadU32Le(ddp2, meta_off + kDdp2PackedOff);
    const std::uint32_t stored = (packed != 0) ? packed : unpacked;
    const Bytes slice(ddp2.begin() + static_cast<std::ptrdiff_t>(offset),
                      ddp2.begin() + static_cast<std::ptrdiff_t>(offset + stored));
    if (packed != 0) {
        return ShsDecompress(slice, unpacked);
    }
    return slice;
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

std::vector<unsigned char> LoadBinaryFile(const std::filesystem::path& path)
{
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return {};
    }
    std::vector<unsigned char> buf(static_cast<std::size_t>(size));
    const std::size_t readBytes = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(readBytes);
    return buf;
}

void WriteBinaryFile(const std::filesystem::path& path, const Bytes& data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

bool ContainsUtf16LeSubstring(const std::vector<unsigned char>& haystack,
                              const std::string& utf8Needle)
{
    const std::u16string needle16 = Utf8ToUtf16Le(utf8Needle);
    if (needle16.empty()) return false;
    std::vector<unsigned char> needleBytes;
    needleBytes.reserve(needle16.size() * kUtf16CodeUnitBytes);
    for (const char16_t ch : needle16) {
        const auto v = static_cast<std::uint16_t>(ch);
        needleBytes.push_back(static_cast<unsigned char>(v & kByteMask));
        needleBytes.push_back(
            static_cast<unsigned char>((v >> kByteShift8) & kByteMask));
    }
    if (haystack.size() < needleBytes.size()) return false;
    for (std::size_t i = 0; i + needleBytes.size() <= haystack.size(); i++) {
        bool match = true;
        for (std::size_t k = 0; k < needleBytes.size(); k++) {
            if (haystack[i + k] != needleBytes[k]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
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

    EXPECT_EQ(map["\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96"], "Save");
    EXPECT_EQ(map["\xE3\x83\xAD\xE3\x83\xBC\xE3\x83\x89"], "Load");
    EXPECT_EQ(map["\xE3\x80\x90\xE9\x81\xB8\xE6\x8A\x9E\xE8\x82\xA2\xE3\x80\x91"], "[Choice]");
    EXPECT_EQ(map["\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F\xEF\xBC\x9F"], "????");

    Utf8TranslationMap withOverride;
    withOverride["\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96"] = "Custom Save";
    const int insertedWithOverride = PopulateSystemTranslations(withOverride);
    EXPECT_EQ(insertedWithOverride, kExpectedSystemMapSize - kSingleExpectedItem);
    EXPECT_EQ(withOverride["\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96"], "Custom Save");
}

TEST(SystemTranslationsTest, Utf8AndUtf16LeRoundTripPreservesJapaneseAndAscii)
{
    const std::string original =
        "\xE3\x80\x8E\xE7\xA5\x9E\xE5\xAD\xA6\xE6\xA0\xA1-Noli me tangere-\xE3\x80\x8F";
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
    AppendUtf16LeWithNull(script, "\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96");
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
    map["\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96"] = "Save Data Now";

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

TEST(BootupScriptsTranslationTest, TranslatesAllEightBootupScriptsWithValidRelocatedJumps)
{
    struct BootupExpectation {
        std::size_t entry_index;
        const char* label;
        int expected_strings;
        int expected_jumps;
        const char* expected_english_snippet;
    };

    const BootupExpectation kCases[] = {
        {kBootupIndex00, "bootup_00000.hxb", kExpectedStringsBootup00, kExpectedJumpsBootup00,
         "Please insert [Disc 1] of/Shingakkou -Noli me tangere-."},
        {kBootupIndex03, "bootup_00003.hxb", kExpectedStringsBootup03, kExpectedJumpsBootup03,
         "Quick load the latest save?/Are you sure?"},
        {kBootupIndex09, "bootup_00009.hxb", kExpectedStringsBootup09, kExpectedJumpsBootup09,
         "[Choice]"},
        {kBootupIndex10, "bootup_00010.hxb", kExpectedStringsBootup10, kExpectedJumpsBootup10,
         "Return to Previous Choice"},
        {kBootupIndex12, "bootup_00012.hxb", kExpectedStringsBootup12, kExpectedJumpsBootup12,
         "Is this text display speed\\ncomfortable for reading?"},
        {kBootupIndex13, "bootup_00013.hxb", kExpectedStringsBootup13, kExpectedJumpsBootup13,
         "Save data already exists in this slot./Overwrite it?"},
        {kBootupIndex14, "bootup_00014.hxb", kExpectedStringsBootup14, kExpectedJumpsBootup14,
         " already has a saved voice clip./Overwrite it?"},
        {kBootupIndex15, "bootup_00015.hxb", kExpectedStringsBootup15, kExpectedJumpsBootup15,
         " will be deleted./Are you sure?"}
    };

    const std::filesystem::path liveGameRoot =
        "d:/Kits/my_downloadedGames/Shingakkou + FD/Shingakkou";
    const std::filesystem::path sinSysdBakPath =
        liveGameRoot / "Data" / "sin_sysd.dat.bak";
    const std::filesystem::path bootupBakPath =
        liveGameRoot / "Bootup.dat.bak";
    const std::filesystem::path patchBinPath =
        "D:/Projects/VN_TRANSLATE/shingakkou/menu_patch.bin";

    if (!std::filesystem::exists(sinSysdBakPath) ||
        !std::filesystem::exists(bootupBakPath) ||
        !std::filesystem::exists(patchBinPath)) {
        GTEST_SKIP() << "Live Shingakkou .bak archives not present on this machine";
    }

    const Bytes origSinSysd = LoadBinaryFile(sinSysdBakPath);
    const Bytes origBootup = LoadBinaryFile(bootupBakPath);
    const Bytes patchBlob = LoadBinaryFile(patchBinPath);
    ASSERT_FALSE(origSinSysd.empty());
    ASSERT_FALSE(origBootup.empty());
    ASSERT_FALSE(patchBlob.empty());

    Bytes patchedSinSysd;
    Bytes patchedBootup;
    ASSERT_TRUE(menu_autopatch::ApplyXorMenuPatchInMemory(
        origSinSysd, origBootup, patchBlob, patchedSinSysd, patchedBootup));

    Utf8TranslationMap map;
    PopulateSystemTranslations(map);

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const Bytes origPlainHxb =
            DecryptHxb(ExtractDdp2EntryDecompressed(origBootup, tc.entry_index));
        const Bytes patchedPlainHxb =
            DecryptHxb(ExtractDdp2EntryDecompressed(patchedBootup, tc.entry_index));
        ASSERT_FALSE(origPlainHxb.empty());
        ASSERT_FALSE(patchedPlainHxb.empty());

        int origJumpCount = 0;
        EXPECT_TRUE(VerifyBytecodeJumps(origPlainHxb.data(), origPlainHxb.size(), origJumpCount));
        EXPECT_EQ(origJumpCount, tc.expected_jumps);

        const HxbTranslationResult res = TranslateHxbScript(
            origPlainHxb.data(), origPlainHxb.size(), map, kDisableDialogueWordWrap);

        EXPECT_EQ(res.strings_replaced, tc.expected_strings);
        EXPECT_EQ(res.bytecode_jumps_relocated, tc.expected_jumps);

        int relocatedJumpCount = 0;
        EXPECT_TRUE(VerifyBytecodeJumps(
            res.data.data(), res.data.size(), relocatedJumpCount));
        EXPECT_EQ(relocatedJumpCount, tc.expected_jumps);
        EXPECT_TRUE(ContainsUtf16LeSubstring(res.data, tc.expected_english_snippet));

        for (const auto& kv : map) {
            EXPECT_FALSE(ContainsUtf16LeSubstring(res.data, kv.first));
        }

        EXPECT_EQ(patchedPlainHxb, res.data);
    }
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
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "shin_menu_autopatch_ut";
    std::filesystem::remove_all(temp_dir);
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

    std::filesystem::remove_all(temp_dir);
}
