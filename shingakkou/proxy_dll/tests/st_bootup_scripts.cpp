// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_autopatch.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using translator_logic::HxbTranslationResult;
using translator_logic::PopulateSystemTranslations;
using translator_logic::TranslateHxbScript;
using translator_logic::Utf8ToUtf16Le;
using translator_logic::Utf8TranslationMap;
using translator_logic::VerifyBytecodeJumps;

using Bytes = std::vector<std::uint8_t>;

constexpr int kDisableDialogueWordWrap        = 0;
constexpr std::size_t kUtf16CodeUnitBytes     = 2;
constexpr unsigned int kByteShift8            = 8;
constexpr unsigned int kByteShift16           = 16;
constexpr unsigned int kByteShift24           = 24;
constexpr unsigned int kByteMask              = 0xFF;

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
constexpr std::size_t kByteOffset1            = 1;
constexpr std::size_t kByteOffset2            = 2;
constexpr std::size_t kByteOffset3            = 3;

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
constexpr std::int64_t kShsOneByteAdvance     = 1;
constexpr std::int64_t kShsTwoByteAdvance     = 2;
constexpr int kShsShift2                      = 2;
constexpr int kShsShift5                      = 5;

std::uint32_t ReadU32Le(const Bytes& b, std::size_t off)
{
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + kByteOffset1]) << kByteShift8) |
           (static_cast<std::uint32_t>(b[off + kByteOffset2]) << kByteShift16) |
           (static_cast<std::uint32_t>(b[off + kByteOffset3]) << kByteShift24);
}

std::int64_t DecodeShsLiteralCount(const Bytes& src, std::uint8_t ctl, std::int64_t& si)
{
    if (ctl == kShsLiteral1D) {
        const std::int64_t count = src[static_cast<std::size_t>(si)] + kShsBias1D;
        si += kShsOneByteAdvance;
        return count;
    }
    if (ctl == kShsLiteral1E) {
        const std::int64_t count =
            ((src[static_cast<std::size_t>(si)] << kByteShift8) |
             src[static_cast<std::size_t>(si) + kByteOffset1]) +
            kShsBias1E;
        si += kShsTwoByteAdvance;
        return count;
    }
    if (ctl == kShsLiteral32Opcode) {
        const std::int64_t count =
            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si)]) << kByteShift24) |
            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset1]) << kByteShift16) |
            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset2]) << kByteShift8) |
            static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset3]);
        si += static_cast<std::int64_t>(kWordBytes);
        return count;
    }
    return ctl + kShsOneByteAdvance;
}

void DecodeShsBackref(const Bytes& src,
                      std::uint8_t ctl,
                      std::int64_t& si,
                      std::int64_t& offset,
                      std::int64_t& count)
{
    if ((ctl & kShsHighBit) != 0) {
        count = (ctl >> kShsShift5) & kShsCountMask3;
        offset = (static_cast<std::int64_t>(ctl & kShsMask1F) << kByteShift8) |
                 src[static_cast<std::size_t>(si)];
        si += kShsOneByteAdvance;
        return;
    }
    if ((ctl & kShsModeMask) == kShsMode20) {
        offset = (ctl >> kShsShift2) & kShsOffsetMask7;
        count = ctl & kShsCountMask3;
        return;
    }
    if ((ctl & kShsModeMask) == kShsMode40) {
        offset = src[static_cast<std::size_t>(si)];
        si += kShsOneByteAdvance;
        count = (ctl & kShsMask1F) + kShsBackrefBias4;
        return;
    }

    offset = src[static_cast<std::size_t>(si)];
    si += kShsOneByteAdvance;
    offset |= static_cast<std::int64_t>(ctl & kShsMask1F) << kByteShift8;
    const std::uint8_t ctl2 = src[static_cast<std::size_t>(si)];
    si += kShsOneByteAdvance;
    if (ctl2 == kShsExtFE) {
        count = ((src[static_cast<std::size_t>(si)] << kByteShift8) |
                 src[static_cast<std::size_t>(si) + kByteOffset1]) +
                kShsBias102;
        si += kShsTwoByteAdvance;
        return;
    }
    if (ctl2 == kShsExtFF) {
        count =
            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si)]) << kByteShift24) |
            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset1]) << kByteShift16) |
            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset2]) << kByteShift8) |
            static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + kByteOffset3]);
        si += static_cast<std::int64_t>(kWordBytes);
        return;
    }
    count = ctl2 + kShsBackrefBias4;
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

        if (ctl < kShsLiteralLimit) {
            const std::int64_t count =
                std::min(DecodeShsLiteralCount(src, ctl, si), out_size - dst);
            for (std::int64_t i = 0; i < count; ++i) {
                output[static_cast<std::size_t>(dst + i)] =
                    src[static_cast<std::size_t>(si + i)];
            }
            si += count;
            dst += count;
            continue;
        }

        std::int64_t offset = 0;
        std::int64_t count = 0;
        DecodeShsBackref(src, ctl, si, offset, count);

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

}  // namespace

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

    const wchar_t* envGameDir = _wgetenv(L"VN_GAME_DIR");
    if (envGameDir == nullptr || envGameDir[0] == L'\0') {
        GTEST_SKIP() << "VN_GAME_DIR not set";
    }

    const std::filesystem::path liveGameRoot(envGameDir);
    const std::filesystem::path sinSysdBakPath =
        liveGameRoot / "Data" / "sin_sysd.dat.bak";
    const std::filesystem::path bootupBakPath =
        liveGameRoot / "Bootup.dat.bak";
    const std::filesystem::path patchBinPath =
        std::filesystem::u8path(SHIN_MENU_PATCH_BIN_PATH);

    if (!std::filesystem::exists(sinSysdBakPath) ||
        !std::filesystem::exists(bootupBakPath) ||
        !std::filesystem::exists(patchBinPath)) {
        GTEST_SKIP() << "Live Shingakkou .bak archives or menu_patch.bin not present";
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
