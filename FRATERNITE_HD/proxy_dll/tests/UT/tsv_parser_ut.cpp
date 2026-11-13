// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for TSV parsing, English line reconstruction, and menu autopatch
// in FRATERNITE_HD.

#include "menu_autopatch.h"
#include "translator_logic.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using translator_logic::EnglishLine;
using translator_logic::ParseTsvBuffer;
using translator_logic::ReconstructEnglish;
using translator_logic::TranslationMap;
using translator_logic::UnescapeInPlace;

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

constexpr unsigned int kCp932CodePage = 932u;
constexpr int kExpectedTwoParsedRows = 2;
constexpr int kExpectedOneParsedRow = 1;
constexpr int kExpectedTwoSpeakerSlots = 2;

std::string WideToCp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        kCp932CodePage, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("WideCharToMultiByte(932) failed in test");
    }
    std::string cp932(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        kCp932CodePage, 0, wide.data(), static_cast<int>(wide.size()),
        cp932.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("WideCharToMultiByte(932) length mismatch in test");
    }
    return cp932;
}

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kYpfVersion490 = 490u;
constexpr std::uint8_t kPngEntryType = 2u;
constexpr std::uint8_t kUncompressedFlag = 0u;
constexpr std::uint8_t kYpfNameXorKey = 0xFFu;
constexpr std::size_t kYpfHeaderSize = 32u;
constexpr std::size_t kYpfEntryPrefixSize = 5u;
constexpr std::size_t kYpfEntryFooterSize = 22u;
constexpr std::size_t kHeaderReservedZeros = 16u;
constexpr std::size_t kPayloadTrailerZeros = 4u;
constexpr std::size_t kPngSigSize = 8u;
constexpr std::uint32_t kIhdrChunkSize = 13u;
constexpr std::uint8_t kBitDepth8 = 8u;
constexpr std::uint8_t kColorTypeRgb = 2u;
constexpr std::uint8_t kColorTypeRgba = 6u;
constexpr std::uint32_t kWidthHd = 1280u;
constexpr std::uint32_t kHeightHd = 720u;
constexpr std::uint32_t kWidthBtn = 240u;
constexpr std::uint32_t kHeightBtn = 48u;
constexpr std::size_t kExtraBytesShort = 16u;
constexpr std::size_t kExtraBytesLong = 48u;
constexpr std::uint8_t kFillByteOrig0 = 0x10u;
constexpr std::uint8_t kFillByteOrig1 = 0x20u;
constexpr std::uint8_t kFillByteRepl0 = 0x99u;
constexpr std::uint32_t kPatchFormatVersion = 1u;
constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kXorStreamAddend = 0x37u;
constexpr std::uint32_t kHashByteIndexMask = 3u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::size_t kFirstRecordOrigHashOffset = 28u;
constexpr std::uint8_t kTamperXorByte = 0xFFu;
constexpr std::size_t kSingleEntryTotal = 1u;
constexpr std::size_t kThreeEntryTotal = 3u;
constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::size_t kWordBytes = 4u;
constexpr std::size_t kU64Bytes = 8u;
constexpr std::size_t kRemainderThree = 3u;
constexpr std::size_t kRemainderTwo = 2u;
constexpr std::size_t kRemainderOne = 1u;
constexpr int kBitsPerByte = 8;
constexpr int kShiftByte1 = 8;
constexpr int kShiftByte2 = 16;
constexpr int kShiftByte3 = 24;

void append_le_u32(Bytes& out, std::uint32_t v) {
    for (std::size_t i = 0; i < kWordBytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

void append_le_u64(Bytes& out, std::uint64_t v) {
    for (std::size_t i = 0; i < kU64Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * kBitsPerByte)));
    }
}

void append_be_u32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> kShiftByte3));
    out.push_back(static_cast<std::uint8_t>(v >> kShiftByte2));
    out.push_back(static_cast<std::uint8_t>(v >> kShiftByte1));
    out.push_back(static_cast<std::uint8_t>(v));
}

std::uint32_t murmur_hash2(const Bytes& data) {
    std::uint32_t h = static_cast<std::uint32_t>(data.size());
    std::size_t i = 0;
    while (i + kWordBytes <= data.size()) {
        std::uint32_t k =
            static_cast<std::uint32_t>(data[i]) |
            (static_cast<std::uint32_t>(data[i + 1u]) << kShiftByte1) |
            (static_cast<std::uint32_t>(data[i + 2u]) << kShiftByte2) |
            (static_cast<std::uint32_t>(data[i + 3u]) << kShiftByte3);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }
    const std::size_t rem = data.size() - i;
    if (rem == kRemainderThree) {
        h ^= static_cast<std::uint32_t>(data[i + 2u]) << kShiftByte2;
    }
    if (rem >= kRemainderTwo) {
        h ^= static_cast<std::uint32_t>(data[i + 1u]) << kShiftByte1;
    }
    if (rem >= kRemainderOne) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

Bytes make_test_png(std::uint32_t width, std::uint32_t height,
                    std::uint8_t color_type, std::size_t extra_bytes,
                    std::uint8_t fill_byte) {
    constexpr std::uint8_t kSig[kPngSigSize] = {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    Bytes out(kSig, kSig + kPngSigSize);
    append_be_u32(out, kIhdrChunkSize);
    for (char c : std::string("IHDR")) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    append_be_u32(out, width);
    append_be_u32(out, height);
    out.push_back(kBitDepth8);
    out.push_back(color_type);
    out.push_back(0u);
    out.push_back(0u);
    out.push_back(0u);
    append_be_u32(out, 0u);
    out.insert(out.end(), extra_bytes, fill_byte);
    return out;
}

struct TestYpfSpec {
    std::string ascii_name;
    Bytes payload;
    bool share_previous = false;
};

Bytes build_test_ypf(const std::vector<TestYpfSpec>& specs) {
    std::size_t index_bytes = 0;
    for (const auto& s : specs) {
        index_bytes += kYpfEntryPrefixSize + s.ascii_name.size() + kYpfEntryFooterSize;
    }
    const std::uint64_t data_start = kYpfHeaderSize + index_bytes;
    Bytes index;
    Bytes payloads;
    std::uint64_t last_off = data_start;

    for (std::size_t i = 0; i < specs.size(); ++i) {
        const Bytes name_bytes(specs[i].ascii_name.begin(), specs[i].ascii_name.end());
        const Bytes& blob = specs[i].payload;
        std::uint64_t off = data_start + payloads.size();
        if (specs[i].share_previous && i > 0) {
            off = last_off;
        } else {
            last_off = off;
            payloads.insert(payloads.end(), blob.begin(), blob.end());
            payloads.insert(payloads.end(), kPayloadTrailerZeros, 0u);
        }

        append_le_u32(index, murmur_hash2(name_bytes));
        index.push_back(static_cast<std::uint8_t>(name_bytes.size() ^ kYpfNameXorKey));
        for (const std::uint8_t b : name_bytes) {
            index.push_back(static_cast<std::uint8_t>(b ^ kYpfNameXorKey));
        }
        index.push_back(kPngEntryType);
        index.push_back(kUncompressedFlag);
        append_le_u32(index, static_cast<std::uint32_t>(blob.size()));
        append_le_u32(index, static_cast<std::uint32_t>(blob.size()));
        append_le_u64(index, off);
        append_le_u32(index, murmur_hash2(blob));
    }

    Bytes out = {'Y', 'P', 'F', '\0'};
    append_le_u32(out, kYpfVersion490);
    append_le_u32(out, static_cast<std::uint32_t>(specs.size()));
    append_le_u32(out, static_cast<std::uint32_t>(data_start));
    out.insert(out.end(), kHeaderReservedZeros, 0u);
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), payloads.begin(), payloads.end());
    return out;
}

Bytes build_single_entry_xor_patch(std::size_t total_entries,
                                   std::size_t orig_file_size,
                                   std::uint32_t entry_index,
                                   const Bytes& orig_png,
                                   const Bytes& new_png) {
    const std::uint32_t orig_hash = murmur_hash2(orig_png);
    const std::uint32_t new_hash = murmur_hash2(new_png);
    Bytes out = {'F', 'R', 'A', 'T', 'M', 'E', 'N', 'U'};
    append_le_u32(out, kPatchFormatVersion);
    append_le_u32(out, static_cast<std::uint32_t>(total_entries));
    append_le_u32(out, static_cast<std::uint32_t>(orig_file_size));
    append_le_u32(out, static_cast<std::uint32_t>(kSingleEntryTotal));

    append_le_u32(out, entry_index);
    append_le_u32(out, orig_hash);
    append_le_u32(out, static_cast<std::uint32_t>(new_png.size()));
    append_le_u32(out, new_hash);

    for (std::size_t k = 0; k < new_png.size(); ++k) {
        const std::uint32_t k32 = static_cast<std::uint32_t>(k);
        const std::uint32_t shift =
            (k32 & kHashByteIndexMask) * static_cast<std::uint32_t>(kBitsPerByte);
        const std::uint8_t hash_byte =
            static_cast<std::uint8_t>((orig_hash >> shift) & kLowByteMask);
        const std::uint8_t stream_byte =
            static_cast<std::uint8_t>((k32 * kXorStreamMultiplier + kXorStreamAddend) & kLowByteMask);
        const std::uint8_t mask =
            static_cast<std::uint8_t>(orig_png[k % orig_png.size()] ^ hash_byte ^ stream_byte);
        out.push_back(static_cast<std::uint8_t>(new_png[k] ^ mask));
    }
    return out;
}

void write_binary(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

Bytes read_binary(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    Bytes buf(sz);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

}  // namespace

TEST(Unescape, BasicEscapes) {
    std::string s = "a\\nb\\tc\\\\d\\re";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "a\nb\tc\\d\re");
}

TEST(Unescape, NoEscapes) {
    std::string s = "plain ascii";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "plain ascii");
}

TEST(Unescape, UnknownEscapeKeepsBackslash) {
    std::string s = "a\\qb";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "a\\qb");
}

TEST(Unescape, TrailingBackslashIsPreserved) {
    std::string s = "trailing\\";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "trailing\\");
}

TEST(ParseTsv, EmptyBuffer) {
    TranslationMap m;
    EXPECT_EQ(ParseTsvBuffer("", 0, m), 0);
    EXPECT_TRUE(m.empty());
}

TEST(ParseTsv, AsciiOnly) {
    TranslationMap m;
    const char tsv[] = "hello\tHello\nworld\tWorld\n";
    const int n = ParseTsvBuffer(tsv, sizeof(tsv) - 1, m);
    EXPECT_EQ(n, kExpectedTwoParsedRows);
    EXPECT_EQ(m["hello"], "Hello");
    EXPECT_EQ(m["world"], "World");
}

TEST(ParseTsv, EscapedNewline) {
    TranslationMap m;
    const char tsv[] = "a\\nb\tA\\nB\n";
    EXPECT_EQ(ParseTsvBuffer(tsv, sizeof(tsv) - 1, m), kExpectedOneParsedRow);
    EXPECT_EQ(m["a\nb"], "A\nB");
}

TEST(ParseTsv, CrlfLineEndings) {
    TranslationMap m;
    const char tsv[] = "x\tX\r\ny\tY\r\n";
    EXPECT_EQ(ParseTsvBuffer(tsv, sizeof(tsv) - 1, m), kExpectedTwoParsedRows);
    EXPECT_EQ(m["x"], "X");
    EXPECT_EQ(m["y"], "Y");
}

TEST(ParseTsv, EmptyOrMalformedLinesIgnored) {
    TranslationMap m;
    const char tsv[] = "good\tValue\nno-tab\nempty-jp\t\n\tempty-en\n";
    const int n = ParseTsvBuffer(tsv, sizeof(tsv) - 1, m);
    EXPECT_EQ(n, kExpectedOneParsedRow);
    EXPECT_EQ(m["good"], "Value");
}

TEST(ReconstructEnglish, SpeakerAndQuoteSplitsSpeakerFromBody) {
    TranslationMap m;
    const std::string spk = WideToCp932(L"大智");
    const std::string quote = WideToCp932(L"「あ」");
    m[spk] = "Daichi";
    m[quote] = "\"Ah\"";
    const EnglishLine el = ReconstructEnglish(spk + quote, m);
    EXPECT_TRUE(el.valid);
    EXPECT_EQ(el.speaker, "Daichi");
    EXPECT_EQ(el.speaker_slots, kExpectedTwoSpeakerSlots);
    EXPECT_EQ(el.body, "\"Ah\"");
}

TEST(ReconstructEnglish, NarrationHasNoSpeaker) {
    TranslationMap m;
    const std::string jp = WideToCp932(L"あい");
    m[jp] = "Hello there.";
    const EnglishLine el = ReconstructEnglish(jp, m);
    EXPECT_TRUE(el.valid);
    EXPECT_TRUE(el.speaker.empty());
    EXPECT_EQ(el.speaker_slots, 0);
    EXPECT_EQ(el.body, "Hello there.");
}

TEST(ReconstructEnglish, NoMatchIsInvalid) {
    TranslationMap m;
    m[WideToCp932(L"あい")] = "x";
    const EnglishLine el = ReconstructEnglish(WideToCp932(L"漾漓"), m);
    EXPECT_FALSE(el.valid);
    EXPECT_TRUE(el.body.empty());
}

TEST(MenuAutoPatch, ApplyXorMenuPatchInMemory_ReconstructsPatchedArchiveAndPreservesSharedOffsets) {
    const Bytes orig_png0 =
        make_test_png(kWidthHd, kHeightHd, kColorTypeRgb, kExtraBytesShort, kFillByteOrig0);
    const Bytes orig_png1 =
        make_test_png(kWidthBtn, kHeightBtn, kColorTypeRgba, kExtraBytesShort, kFillByteOrig1);
    const Bytes repl_png0 =
        make_test_png(kWidthHd, kHeightHd, kColorTypeRgb, kExtraBytesLong, kFillByteRepl0);

    const Bytes orig_ypf = build_test_ypf({
        {"cgsys\\config\\back_easy.png", orig_png0, false},
        {"cgsys\\config\\btn_tab_easy_bt3.png", orig_png1, false},
        {"cgsys\\config\\btn_tab_easy_dup.png", orig_png1, true},
    });
    const Bytes expected_patched = build_test_ypf({
        {"cgsys\\config\\back_easy.png", repl_png0, false},
        {"cgsys\\config\\btn_tab_easy_bt3.png", orig_png1, false},
        {"cgsys\\config\\btn_tab_easy_dup.png", orig_png1, true},
    });

    const Bytes patch_blob =
        build_single_entry_xor_patch(kThreeEntryTotal, orig_ypf.size(), 0u, orig_png0, repl_png0);

    Bytes actual_patched;
    EXPECT_TRUE(menu_autopatch::ApplyXorMenuPatchInMemory(
        orig_ypf, patch_blob, actual_patched));
    EXPECT_EQ(actual_patched, expected_patched);
}

TEST(MenuAutoPatch, ApplyXorMenuPatchInMemory_RejectsTamperedOrigHashOrCorruptedPayload) {
    const Bytes orig_png0 =
        make_test_png(kWidthHd, kHeightHd, kColorTypeRgb, kExtraBytesShort, kFillByteOrig0);
    const Bytes repl_png0 =
        make_test_png(kWidthHd, kHeightHd, kColorTypeRgb, kExtraBytesLong, kFillByteRepl0);

    const Bytes orig_ypf = build_test_ypf({
        {"cgsys\\config\\back_easy.png", orig_png0, false},
    });
    const Bytes valid_patch =
        build_single_entry_xor_patch(kSingleEntryTotal, orig_ypf.size(), 0u, orig_png0, repl_png0);

    Bytes bad_orig_hash = valid_patch;
    bad_orig_hash[kFirstRecordOrigHashOffset] ^= kTamperXorByte;
    Bytes out;
    EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(
        orig_ypf, bad_orig_hash, out));

    Bytes bad_payload = valid_patch;
    bad_payload.back() ^= kTamperXorByte;
    EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(
        orig_ypf, bad_payload, out));
}

TEST(MenuAutoPatch, EnsureMenuArchivePatchedInDir_CreatesBackupPatchesArchiveAndIsIdempotent) {
    const ScopedTempDir tmp("frat_menu_autopatch_ut");
    const std::filesystem::path& temp_dir = tmp.path();
    std::filesystem::create_directories(temp_dir / "pac");

    const Bytes orig_png0 =
        make_test_png(kWidthHd, kHeightHd, kColorTypeRgb, kExtraBytesShort, kFillByteOrig0);
    const Bytes repl_png0 =
        make_test_png(kWidthHd, kHeightHd, kColorTypeRgb, kExtraBytesLong, kFillByteRepl0);

    const Bytes orig_ypf = build_test_ypf({
        {"cgsys\\config\\back_easy.png", orig_png0, false},
    });
    const Bytes expected_patched = build_test_ypf({
        {"cgsys\\config\\back_easy.png", repl_png0, false},
    });
    const Bytes patch_blob =
        build_single_entry_xor_patch(kSingleEntryTotal, orig_ypf.size(), 0u, orig_png0, repl_png0);

    const std::filesystem::path ypf_path = temp_dir / "pac" / "cgsys.ypf";
    const std::filesystem::path bak_path = temp_dir / "pac" / "cgsys.ypf.bak";
    const std::filesystem::path patch_path = temp_dir / "cgsys_patch.bin";
    write_binary(ypf_path, orig_ypf);
    write_binary(patch_path, patch_blob);

    EXPECT_TRUE(menu_autopatch::EnsureMenuArchivePatchedInDir(temp_dir.wstring()));
    ASSERT_TRUE(std::filesystem::exists(bak_path));
    EXPECT_EQ(read_binary(bak_path), orig_ypf);
    EXPECT_EQ(read_binary(ypf_path), expected_patched);

    EXPECT_TRUE(menu_autopatch::EnsureMenuArchivePatchedInDir(temp_dir.wstring()));
    EXPECT_EQ(read_binary(bak_path), orig_ypf);
    EXPECT_EQ(read_binary(ypf_path), expected_patched);
}

// FRATERNITE_HD writes translation_table.tsv in CP932 directly ("<jp_cp932>\t<en_cp932>\n")
// and stores keys verbatim without UTF-8-to-CP932 conversion at load time.
