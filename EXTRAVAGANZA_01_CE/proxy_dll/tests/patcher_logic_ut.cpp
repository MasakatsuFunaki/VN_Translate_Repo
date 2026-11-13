// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_autopatch.h"
#include "patcher_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr unsigned char kNopByte = 0x90u;
constexpr unsigned char kCallOpcode = 0xE8u;
constexpr unsigned char kJmpOpcode = 0xE9u;
constexpr std::size_t kRelInstructionBytes = 5u;
constexpr std::size_t kNopBufferSize = 8u;
constexpr std::uintptr_t kSampleInstrAddr = 0x00401000u;
constexpr std::uintptr_t kSampleTargetAddr = 0x00402005u;
constexpr std::int32_t kExpectedRel32Disp = 0x00001000;

constexpr std::size_t kPatchMagicSize = 8u;
constexpr char kPatchMagic[kPatchMagicSize] = {'E', 'X', 'C', 'E', 'P', 'A', 'T', '1'};
constexpr char kBadPatchMagic[kPatchMagicSize] = {'B', 'A', 'D', 'M', 'A', 'G', 'I', 'C'};
constexpr std::uint32_t kPatchFormatVersion = 1u;

constexpr std::size_t kU16Bytes = 2u;
constexpr std::size_t kU32Bytes = 4u;
constexpr unsigned int kBitsPerByte = 8u;
constexpr std::uint32_t kCorruptHashXorMask = 0xDEAD0001u;

constexpr std::size_t kSyntheticOrigLen = 29u;
constexpr std::size_t kSyntheticPatchedLen = 47u;
constexpr std::uint8_t kOrigStep = 17u;
constexpr std::uint8_t kOrigBias = 5u;
constexpr std::uint8_t kPatchedStep = 31u;
constexpr std::uint8_t kPatchedBias = 9u;

constexpr std::uint32_t kTestArchiveEntryCount = 2u;
constexpr std::uint32_t kModifiedGpkEntryIndex = 0u;
constexpr std::uint32_t kModifiedGpkEntryCount = 1u;
constexpr std::uint32_t kStandaloneRecordCount = 2u;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::string& suffix)
        : path_(fs::temp_directory_path() / fs::u8path("extravaganza_ut_" + suffix))
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void AppendU16Le(Bytes& out, std::uint16_t val)
{
    for (std::size_t i = 0; i < kU16Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(val >> (i * kBitsPerByte)));
    }
}

void AppendU32Le(Bytes& out, std::uint32_t val)
{
    for (std::size_t i = 0; i < kU32Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(val >> (i * kBitsPerByte)));
    }
}

void WriteBytesToFile(const fs::path& p, const Bytes& data)
{
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.good()) << "Failed to open for write: " << p.u8string();
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}

Bytes ReadBytesFromFile(const fs::path& p)
{
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(ifs.good()) << "Failed to open for read: " << p.u8string();
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        return {};
    }
    Bytes out(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(out.data()), sz);
    return out;
}

Bytes BuildSyntheticGtb(const Bytes& entry0, const Bytes& entry1)
{
    const std::string name0 = "SPRITE0";
    const std::string name1 = "SPRITE1";

    Bytes names_blob;
    names_blob.insert(names_blob.end(), name0.begin(), name0.end());
    names_blob.push_back(0u);
    const auto name1_off = static_cast<std::uint32_t>(names_blob.size());
    names_blob.insert(names_blob.end(), name1.begin(), name1.end());
    names_blob.push_back(0u);

    Bytes gtb;
    AppendU32Le(gtb, kTestArchiveEntryCount);
    AppendU32Le(gtb, 0u);
    AppendU32Le(gtb, name1_off);
    AppendU32Le(gtb, 0u);
    AppendU32Le(gtb, static_cast<std::uint32_t>(entry0.size()));
    gtb.insert(gtb.end(), names_blob.begin(), names_blob.end());
    return gtb;
}

struct SyntheticFixtureData {
    Bytes orig_spt;
    Bytes new_spt;
    Bytes orig_fxf;
    Bytes new_fxf;
    Bytes orig_entry0;
    Bytes new_entry0;
    Bytes untouched_entry1;
    Bytes orig_gtb;
    Bytes new_gtb;
    Bytes orig_gpk;
    Bytes new_gpk;
};

SyntheticFixtureData MakeSyntheticFixtureData()
{
    SyntheticFixtureData d;
    d.orig_spt = {'O', 'R', 'I', 'G', '_', 'S', 'P', 'T', '_', '0', '1'};
    d.new_spt = {'P', 'A', 'T', 'C', 'H', 'E', 'D', '_', 'S', 'P', 'T', '_', '0', '1', '!'};

    // Include a same-size standalone file to exercise the orig_size == new_size hash check.
    d.orig_fxf = {'F', 'X', 'F', '_', 'J', 'P', '_', 'T', 'E', 'X', 'T'};
    d.new_fxf = {'F', 'X', 'F', '_', 'E', 'N', '_', 'T', 'E', 'X', 'T'};

    d.orig_entry0 = {'D', 'W', 'Q', '_', 'O', 'R', 'I', 'G', '_', 'E', 'N', 'T', 'R', 'Y', '0'};
    d.new_entry0 = {
        'D', 'W', 'Q', '_', 'P', 'A', 'T', 'C', 'H', 'E', 'D', '_', 'E', 'N', 'T', 'R', 'Y', '0', '!'};
    d.untouched_entry1 = {'D', 'W', 'Q', '_', 'U', 'N', 'T', 'O', 'U', 'C', 'H', 'E', 'D', '_', '1'};

    d.orig_gtb = BuildSyntheticGtb(d.orig_entry0, d.untouched_entry1);
    d.new_gtb = BuildSyntheticGtb(d.new_entry0, d.untouched_entry1);

    d.orig_gpk = d.orig_entry0;
    d.orig_gpk.insert(
        d.orig_gpk.end(), d.untouched_entry1.begin(), d.untouched_entry1.end());

    d.new_gpk = d.new_entry0;
    d.new_gpk.insert(d.new_gpk.end(), d.untouched_entry1.begin(), d.untouched_entry1.end());
    return d;
}

Bytes BuildSyntheticPatchBin(const SyntheticFixtureData& d,
                             const char magic[kPatchMagicSize],
                             bool corrupt_gpk_entry_hash)
{
    const std::uint32_t orig_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(d.orig_gtb.data(), d.orig_gtb.size());
    const std::uint32_t new_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(d.new_gtb.data(), d.new_gtb.size());

    Bytes out(magic, magic + kPatchMagicSize);
    AppendU32Le(out, kPatchFormatVersion);
    AppendU32Le(out, kStandaloneRecordCount);
    AppendU32Le(out, static_cast<std::uint32_t>(d.orig_gpk.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(d.new_gpk.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(d.orig_gtb.size()));
    AppendU32Le(out, orig_gtb_hash);
    AppendU32Le(out, static_cast<std::uint32_t>(d.new_gtb.size()));
    AppendU32Le(out, new_gtb_hash);
    AppendU32Le(out, kModifiedGpkEntryCount);

    auto append_standalone = [&](const std::string& rel_path,
                                 const Bytes& orig_file,
                                 const Bytes& new_file) {
        const std::uint32_t orig_hash =
            menu_autopatch::ComputeMurmurHash2(orig_file.data(), orig_file.size());
        const std::uint32_t new_hash =
            menu_autopatch::ComputeMurmurHash2(new_file.data(), new_file.size());
        Bytes masked = new_file;
        menu_autopatch::XorMaskPayload(
            masked.data(), masked.size(), orig_file.data(), orig_file.size(), orig_hash);

        AppendU16Le(out, static_cast<std::uint16_t>(rel_path.size()));
        out.insert(out.end(), rel_path.begin(), rel_path.end());
        AppendU32Le(out, static_cast<std::uint32_t>(orig_file.size()));
        AppendU32Le(out, orig_hash);
        AppendU32Le(out, static_cast<std::uint32_t>(new_file.size()));
        AppendU32Le(out, new_hash);
        out.insert(out.end(), masked.begin(), masked.end());
    };

    append_standalone("spt\\01.spt", d.orig_spt, d.new_spt);
    append_standalone("init2\\mainControl.fxf", d.orig_fxf, d.new_fxf);

    Bytes masked_gtb = d.new_gtb;
    menu_autopatch::XorMaskPayload(masked_gtb.data(),
                                   masked_gtb.size(),
                                   d.orig_gtb.data(),
                                   d.orig_gtb.size(),
                                   orig_gtb_hash);
    out.insert(out.end(), masked_gtb.begin(), masked_gtb.end());

    const std::uint32_t e0_orig_hash =
        menu_autopatch::ComputeMurmurHash2(d.orig_entry0.data(), d.orig_entry0.size());
    std::uint32_t e0_new_hash =
        menu_autopatch::ComputeMurmurHash2(d.new_entry0.data(), d.new_entry0.size());
    if (corrupt_gpk_entry_hash) {
        e0_new_hash ^= kCorruptHashXorMask;
    }
    Bytes masked_e0 = d.new_entry0;
    menu_autopatch::XorMaskPayload(masked_e0.data(),
                                   masked_e0.size(),
                                   d.orig_entry0.data(),
                                   d.orig_entry0.size(),
                                   e0_orig_hash);

    AppendU32Le(out, kModifiedGpkEntryIndex);
    AppendU32Le(out, 0u);
    AppendU32Le(out, static_cast<std::uint32_t>(d.orig_entry0.size()));
    AppendU32Le(out, e0_orig_hash);
    AppendU32Le(out, static_cast<std::uint32_t>(d.new_entry0.size()));
    AppendU32Le(out, e0_new_hash);
    out.insert(out.end(), masked_e0.begin(), masked_e0.end());

    return out;
}

void PopulateUnpatchedGameDir(const fs::path& root, const SyntheticFixtureData& d)
{
    WriteBytesToFile(root / "spt" / "01.spt", d.orig_spt);
    WriteBytesToFile(root / "init2" / "mainControl.fxf", d.orig_fxf);
    WriteBytesToFile(root / "dwq" / "sys0.gtb", d.orig_gtb);
    WriteBytesToFile(root / "dwq" / "sys0.gpk", d.orig_gpk);
}

}  // namespace

TEST(PatcherLogic, BytesMatchNopsAndRel32Helpers)
{
    const unsigned char a[] = {0x6Au, 0x10u, 0xFFu};
    const unsigned char b[] = {0x6Au, 0x10u, 0xFFu};
    const unsigned char c[] = {0x6Au, 0x12u, 0xFFu};
    EXPECT_TRUE(patcher_logic::BytesMatch(a, b, sizeof(a)));
    EXPECT_FALSE(patcher_logic::BytesMatch(a, c, sizeof(a)));

    unsigned char nops[kNopBufferSize] = {};
    patcher_logic::FillNops(nops, kNopBufferSize);
    for (unsigned char byte : nops) {
        EXPECT_EQ(byte, kNopByte);
    }

    EXPECT_EQ(patcher_logic::ComputeRel32(kSampleInstrAddr, kSampleTargetAddr),
              kExpectedRel32Disp);

    unsigned char code[kRelInstructionBytes] = {};
    unsigned char* end = patcher_logic::EmitRelCall32(
        code, reinterpret_cast<std::uintptr_t>(code) + kRelInstructionBytes);
    EXPECT_EQ(end, code + kRelInstructionBytes);
    EXPECT_EQ(code[0], kCallOpcode);

    end = patcher_logic::EmitRelJmp32(
        code, reinterpret_cast<std::uintptr_t>(code) + kRelInstructionBytes);
    EXPECT_EQ(end, code + kRelInstructionBytes);
    EXPECT_EQ(code[0], kJmpOpcode);
}

TEST(MenuAutopatch, MurmurHash2AndXorMaskPayloadRoundTrip)
{
    Bytes orig(kSyntheticOrigLen);
    for (std::size_t i = 0; i < orig.size(); ++i) {
        orig[i] = static_cast<std::uint8_t>(i * kOrigStep + kOrigBias);
    }
    Bytes patched(kSyntheticPatchedLen);
    for (std::size_t i = 0; i < patched.size(); ++i) {
        patched[i] = static_cast<std::uint8_t>(i * kPatchedStep + kPatchedBias);
    }

    const std::uint32_t orig_hash =
        menu_autopatch::ComputeMurmurHash2(orig.data(), orig.size());
    const std::uint32_t new_hash =
        menu_autopatch::ComputeMurmurHash2(patched.data(), patched.size());
    EXPECT_NE(orig_hash, 0u);
    EXPECT_NE(new_hash, 0u);
    EXPECT_NE(orig_hash, new_hash);

    Bytes buf = patched;
    menu_autopatch::XorMaskPayload(
        buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_NE(buf, patched);

    menu_autopatch::XorMaskPayload(
        buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_EQ(buf, patched);
    EXPECT_EQ(menu_autopatch::ComputeMurmurHash2(buf.data(), buf.size()), new_hash);
}

TEST(MenuAutopatch, EnsureGameFilesPatchedInDirAppliesCreatesBackupsAndIsIdempotent)
{
    ScopedTempDir tmp("autopatch_ok");
    const SyntheticFixtureData d = MakeSyntheticFixtureData();
    PopulateUnpatchedGameDir(tmp.path(), d);
    WriteBytesToFile(tmp.path() / "extravaganza_patch.bin",
                     BuildSyntheticPatchBin(d, kPatchMagic, false));

    // First launch: creates .bak files and reconstructs all patched files.
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(tmp.path().wstring()));

    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "spt" / "01.spt.bak"), d.orig_spt);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "init2" / "mainControl.fxf.bak"), d.orig_fxf);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gtb.bak"), d.orig_gtb);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gpk.bak"), d.orig_gpk);

    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "spt" / "01.spt"), d.new_spt);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "init2" / "mainControl.fxf"), d.new_fxf);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gtb"), d.new_gtb);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gpk"), d.new_gpk);

    // Second launch: fast-path returns true and preserves both active and .bak files.
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(tmp.path().wstring()));
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "spt" / "01.spt.bak"), d.orig_spt);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "spt" / "01.spt"), d.new_spt);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gpk"), d.new_gpk);

    // Recovery: if a same-size standalone file is reverted to its original bytes while
    // .bak still exists, EnsureGameFilesPatchedInDir detects the hash mismatch and re-patches.
    WriteBytesToFile(tmp.path() / "init2" / "mainControl.fxf", d.orig_fxf);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(tmp.path().wstring()));
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "init2" / "mainControl.fxf"), d.new_fxf);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "init2" / "mainControl.fxf.bak"), d.orig_fxf);
}

TEST(MenuAutopatch, CorruptionOrBadMagicLeavesOriginalFilesUntouched)
{
    ScopedTempDir tmp("autopatch_corrupt");
    const SyntheticFixtureData d = MakeSyntheticFixtureData();
    PopulateUnpatchedGameDir(tmp.path(), d);

    // 1. Missing extravaganza_patch.bin returns false cleanly.
    EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(tmp.path().wstring()));
    EXPECT_FALSE(fs::exists(tmp.path() / "spt" / "01.spt.bak"));

    // 2. Bad magic header returns false and leaves files untouched.
    WriteBytesToFile(tmp.path() / "extravaganza_patch.bin",
                     BuildSyntheticPatchBin(d, kBadPatchMagic, false));
    EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(tmp.path().wstring()));
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "spt" / "01.spt"), d.orig_spt);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gpk"), d.orig_gpk);
    EXPECT_FALSE(fs::exists(tmp.path() / "spt" / "01.spt.bak"));
    EXPECT_FALSE(fs::exists(tmp.path() / "dwq" / "sys0.gpk.bak"));

    // 3. Corrupt GPK entry hash in Section C rolls back all staged .tmp files so even
    //    Section A standalone files remain unpatched and no .bak files are created.
    WriteBytesToFile(tmp.path() / "extravaganza_patch.bin",
                     BuildSyntheticPatchBin(d, kPatchMagic, true));
    EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(tmp.path().wstring()));
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "spt" / "01.spt"), d.orig_spt);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "init2" / "mainControl.fxf"), d.orig_fxf);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gtb"), d.orig_gtb);
    EXPECT_EQ(ReadBytesFromFile(tmp.path() / "dwq" / "sys0.gpk"), d.orig_gpk);
    EXPECT_FALSE(fs::exists(tmp.path() / "spt" / "01.spt.bak"));
    EXPECT_FALSE(fs::exists(tmp.path() / "spt" / "01.spt.tmp"));
    EXPECT_FALSE(fs::exists(tmp.path() / "dwq" / "sys0.gpk.bak"));
    EXPECT_FALSE(fs::exists(tmp.path() / "dwq" / "sys0.gpk.tmp"));
}
