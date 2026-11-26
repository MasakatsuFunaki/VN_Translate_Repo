// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 32-bit Win32 component tests for menu_autopatch::EnsureGameFilesPatchedInDir:
// clean-install patching under a Japanese directory, .applied sentinel fast-path
// idempotence, same-size v2 upgrade (same standalone new_size with orig!=new AND
// same GPK entry size leaving GTB byte-identical), hash-verified standalone
// tamper repair, and mid-batch all-or-nothing rollback atomicity.
#include "menu_autopatch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kMagicBytes = 8u;
constexpr char kPatchMagic[kMagicBytes] = {'A', 'G', 'H', 'M', 'E', 'N', 'U', '1'};
constexpr char kSentinelMagic[kMagicBytes] = {'A', 'G', 'H', 'M', 'S', 'N', 'T', '1'};
constexpr std::size_t kExpectedSentinelSize = 20u;

constexpr std::uint32_t kPatchVersion = 1u;
constexpr std::uint32_t kStandaloneRecordCount = 3u;
constexpr std::uint32_t kArchiveEntryCount = 3u;
constexpr std::uint32_t kModifiedGpkCount = 1u;
constexpr std::uint32_t kModifiedGpkEntryIndex = 1u;

constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr unsigned int kShift8 = 8u;
constexpr unsigned int kShift16 = 16u;
constexpr unsigned int kShift24 = 24u;
constexpr std::uint8_t kFaultFlipByte = 0xA5u;

void ClearReadOnlyTree(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        const DWORD attrs = GetFileAttributesW(entry.path().c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
            SetFileAttributesW(entry.path().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }
}

void SetReadOnly(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    ASSERT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    ASSERT_TRUE(SetFileAttributesW(path.c_str(), attrs | FILE_ATTRIBUTE_READONLY) != FALSE);
}

void AssertNoStrayTempFiles(const fs::path& root) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        EXPECT_NE(entry.path().extension().wstring(), L".tmp")
            << "Stray temp file leaked: " << entry.path().u8string();
    }
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::wstring_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        const std::wstring dir_name =
            std::wstring(prefix) + L"_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(id);
        path_ = fs::temp_directory_path() / fs::path(dir_name);
        ClearReadOnlyTree(path_);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        ClearReadOnlyTree(path_);
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void AppendU16Le(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kLowByteMask));
}

void AppendU32Le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift16) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift24) & kLowByteMask));
}

void WriteBytes(const fs::path& path, const Bytes& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.good());
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    ASSERT_TRUE(ofs.good());
}

Bytes ReadBytes(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return {};
    }
    const std::streamsize sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    Bytes buf(static_cast<std::size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    }
    return buf;
}

struct MultiRecordFixture {
    std::array<std::string, kStandaloneRecordCount> rel_paths = {{
        "spt\\charaname.xtx",
        "init2\\mainControl.fxf",
        "init2\\backlog.fxf",
    }};
    std::array<Bytes, kStandaloneRecordCount> orig_standalone;
    std::array<Bytes, kStandaloneRecordCount> new_standalone_v1;
    std::array<Bytes, kStandaloneRecordCount> new_standalone_v2;
    Bytes orig_entry0;
    Bytes orig_entry1;
    Bytes orig_entry2;
    Bytes new_entry1_v1;
    Bytes new_entry1_v2;
    Bytes orig_gtb;
    Bytes orig_gpk;
    Bytes new_gtb_v1;
    Bytes new_gpk_v1;
    Bytes new_gtb_v2;
    Bytes new_gpk_v2;
};

void BuildGtbGpkPair(const std::array<const Bytes*, kArchiveEntryCount>& entries,
                     Bytes& out_gtb,
                     Bytes& out_gpk) {
    out_gtb.clear();
    out_gpk.clear();
    constexpr std::array<std::string_view, kArchiveEntryCount> kNames = {{
        "frame_top", "btn_save", "frame_bottom"}};

    Bytes names_blob;
    std::array<std::uint32_t, kArchiveEntryCount> name_offs{};
    std::array<std::uint32_t, kArchiveEntryCount> data_offs{};
    for (std::size_t i = 0; i < kArchiveEntryCount; ++i) {
        name_offs[i] = static_cast<std::uint32_t>(names_blob.size());
        names_blob.insert(names_blob.end(), kNames[i].begin(), kNames[i].end());
        names_blob.push_back(0u);

        data_offs[i] = static_cast<std::uint32_t>(out_gpk.size());
        out_gpk.insert(out_gpk.end(), entries[i]->begin(), entries[i]->end());
    }

    AppendU32Le(out_gtb, kArchiveEntryCount);
    for (std::uint32_t off : name_offs) {
        AppendU32Le(out_gtb, off);
    }
    for (std::uint32_t off : data_offs) {
        AppendU32Le(out_gtb, off);
    }
    out_gtb.insert(out_gtb.end(), names_blob.begin(), names_blob.end());
}

MultiRecordFixture MakeSameSizeV2Fixture() {
    MultiRecordFixture f;

    const std::string orig_xtx = "0,アゲハ\r\n";
    const std::string v1_xtx = "0,Ageha_v1_nameplate_en\r\n";
    const std::string v2_xtx = "0,Ageha_v2_nameplate_en\r\n";
    f.orig_standalone[0].assign(orig_xtx.begin(), orig_xtx.end());
    f.orig_standalone[0].insert(f.orig_standalone[0].end(), {0x00u, 0x7Fu, 0x80u, 0xFFu});
    f.new_standalone_v1[0].assign(v1_xtx.begin(), v1_xtx.end());
    f.new_standalone_v2[0].assign(v2_xtx.begin(), v2_xtx.end());

    const std::string orig_main = "windowTitle アゲハ0000\r\n";
    const std::string v1_main = "windowTitle AgehaCE_V1\r\n";
    const std::string v2_main = "windowTitle AgehaCE_V2\r\n";
    f.orig_standalone[1].assign(orig_main.begin(), orig_main.end());
    f.new_standalone_v1[1].assign(v1_main.begin(), v1_main.end());
    f.new_standalone_v2[1].assign(v2_main.begin(), v2_main.end());

    const std::string orig_log = "firstMessage1 ログ\r\n";
    const std::string v1_log = "firstMessage1 Backlog_Expanded\r\n";
    f.orig_standalone[2].assign(orig_log.begin(), orig_log.end());
    f.new_standalone_v1[2].assign(v1_log.begin(), v1_log.end());
    f.new_standalone_v2[2] = f.new_standalone_v1[2];

    f.orig_entry0 = {0x42u, 0x4Du, 0x00u, 0x7Fu, 0x80u, 0xFFu, 0x11u, 0x22u};
    f.orig_entry1 = {0x42u, 0x4Du, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u, 0x99u};
    f.orig_entry2 = {0x42u, 0x4Du, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu};
    f.new_entry1_v1 = {0x42u, 0x4Du, 0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u, 0x90u};
    f.new_entry1_v2 = {0x42u, 0x4Du, 0xF1u, 0xE2u, 0xD3u, 0xC4u, 0xB5u, 0xA6u, 0x97u, 0x88u, 0x79u};

    BuildGtbGpkPair({{&f.orig_entry0, &f.orig_entry1, &f.orig_entry2}}, f.orig_gtb, f.orig_gpk);
    BuildGtbGpkPair({{&f.orig_entry0, &f.new_entry1_v1, &f.orig_entry2}}, f.new_gtb_v1, f.new_gpk_v1);
    BuildGtbGpkPair({{&f.orig_entry0, &f.new_entry1_v2, &f.orig_entry2}}, f.new_gtb_v2, f.new_gpk_v2);
    return f;
}

Bytes SerializePatchBin(
    const MultiRecordFixture& f,
    const std::array<Bytes, kStandaloneRecordCount>& target_standalone,
    const Bytes& target_entry1,
    const Bytes& target_gtb,
    const Bytes& target_gpk) {
    const std::uint32_t orig_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(f.orig_gtb.data(), f.orig_gtb.size());
    const std::uint32_t new_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(target_gtb.data(), target_gtb.size());

    Bytes out;
    out.insert(out.end(), kPatchMagic, kPatchMagic + kMagicBytes);
    AppendU32Le(out, kPatchVersion);
    AppendU32Le(out, kStandaloneRecordCount);
    AppendU32Le(out, static_cast<std::uint32_t>(f.orig_gpk.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(target_gpk.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(f.orig_gtb.size()));
    AppendU32Le(out, orig_gtb_hash);
    AppendU32Le(out, static_cast<std::uint32_t>(target_gtb.size()));
    AppendU32Le(out, new_gtb_hash);
    AppendU32Le(out, kModifiedGpkCount);

    for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
        const std::string& rel = f.rel_paths[i];
        const Bytes& orig = f.orig_standalone[i];
        const Bytes& patched = target_standalone[i];
        const std::uint32_t orig_hash =
            menu_autopatch::ComputeMurmurHash2(orig.data(), orig.size());
        const std::uint32_t new_hash =
            menu_autopatch::ComputeMurmurHash2(patched.data(), patched.size());

        Bytes masked = patched;
        menu_autopatch::XorMaskPayload(
            masked.data(), masked.size(), orig.data(), orig.size(), orig_hash);

        AppendU16Le(out, static_cast<std::uint16_t>(rel.size()));
        out.insert(out.end(), rel.begin(), rel.end());
        AppendU32Le(out, static_cast<std::uint32_t>(orig.size()));
        AppendU32Le(out, orig_hash);
        AppendU32Le(out, static_cast<std::uint32_t>(patched.size()));
        AppendU32Le(out, new_hash);
        out.insert(out.end(), masked.begin(), masked.end());
    }

    Bytes masked_gtb = target_gtb;
    menu_autopatch::XorMaskPayload(masked_gtb.data(), masked_gtb.size(),
                                   f.orig_gtb.data(), f.orig_gtb.size(), orig_gtb_hash);
    out.insert(out.end(), masked_gtb.begin(), masked_gtb.end());

    const std::uint32_t entry1_orig_off = static_cast<std::uint32_t>(f.orig_entry0.size());
    const std::uint32_t entry1_orig_hash =
        menu_autopatch::ComputeMurmurHash2(f.orig_entry1.data(), f.orig_entry1.size());
    const std::uint32_t entry1_new_hash =
        menu_autopatch::ComputeMurmurHash2(target_entry1.data(), target_entry1.size());
    Bytes masked_e1 = target_entry1;
    menu_autopatch::XorMaskPayload(masked_e1.data(), masked_e1.size(),
                                   f.orig_entry1.data(), f.orig_entry1.size(), entry1_orig_hash);

    AppendU32Le(out, kModifiedGpkEntryIndex);
    AppendU32Le(out, entry1_orig_off);
    AppendU32Le(out, static_cast<std::uint32_t>(f.orig_entry1.size()));
    AppendU32Le(out, entry1_orig_hash);
    AppendU32Le(out, static_cast<std::uint32_t>(target_entry1.size()));
    AppendU32Le(out, entry1_new_hash);
    out.insert(out.end(), masked_e1.begin(), masked_e1.end());

    return out;
}

void PopulateCleanInstall(const fs::path& root, const MultiRecordFixture& f) {
    for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
        WriteBytes(root / fs::u8path(f.rel_paths[i]), f.orig_standalone[i]);
    }
    WriteBytes(root / "dwq" / "sys0.gtb", f.orig_gtb);
    WriteBytes(root / "dwq" / "sys0.gpk", f.orig_gpk);
}

}  // namespace

TEST(MenuAutopatchCt, CleanInstallFastPathSameSizeV2UpgradeAndStandaloneHashRepair) {
    ScopedTempDir sandbox(L"agh_32bit_アゲハ_ct");
    const MultiRecordFixture f = MakeSameSizeV2Fixture();
    PopulateCleanInstall(sandbox.path(), f);

    const Bytes patch_v1 =
        SerializePatchBin(f, f.new_standalone_v1, f.new_entry1_v1, f.new_gtb_v1, f.new_gpk_v1);
    const Bytes patch_v2 =
        SerializePatchBin(f, f.new_standalone_v2, f.new_entry1_v2, f.new_gtb_v2, f.new_gpk_v2);
    ASSERT_EQ(patch_v1.size(), patch_v2.size());
    ASSERT_EQ(f.new_gtb_v1, f.new_gtb_v2);
    ASSERT_EQ(f.new_gpk_v1.size(), f.new_gpk_v2.size());
    ASSERT_NE(f.orig_standalone[0].size(), f.new_standalone_v1[0].size());
    ASSERT_EQ(f.new_standalone_v1[0].size(), f.new_standalone_v2[0].size());

    WriteBytes(sandbox.path() / "menu_patch.bin", patch_v1);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    AssertNoStrayTempFiles(sandbox.path());

    const fs::path sentinel_path = sandbox.path() / "menu_patch.bin.applied";
    ASSERT_TRUE(fs::is_regular_file(sentinel_path));
    const Bytes sentinel_v1 = ReadBytes(sentinel_path);
    ASSERT_EQ(sentinel_v1.size(), kExpectedSentinelSize);
    EXPECT_EQ(std::memcmp(sentinel_v1.data(), kSentinelMagic, kMagicBytes), 0);

    for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i])), f.new_standalone_v1[i]);
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i] + ".bak")),
                  f.orig_standalone[i]);
    }
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb"), f.new_gtb_v1);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb.bak"), f.orig_gtb);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.new_gpk_v1);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), f.orig_gpk);

    const auto bak_mtime = fs::last_write_time(sandbox.path() / "dwq" / "sys0.gpk.bak");
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    EXPECT_EQ(fs::last_write_time(sandbox.path() / "dwq" / "sys0.gpk.bak"), bak_mtime);
    AssertNoStrayTempFiles(sandbox.path());

    WriteBytes(sandbox.path() / "menu_patch.bin", patch_v2);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    AssertNoStrayTempFiles(sandbox.path());

    const Bytes sentinel_v2 = ReadBytes(sentinel_path);
    ASSERT_EQ(sentinel_v2.size(), kExpectedSentinelSize);
    EXPECT_EQ(std::memcmp(sentinel_v2.data(), kSentinelMagic, kMagicBytes), 0);
    EXPECT_NE(sentinel_v2, sentinel_v1);
    for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i])), f.new_standalone_v2[i]);
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i] + ".bak")),
                  f.orig_standalone[i]);
    }
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb"), f.new_gtb_v2);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb.bak"), f.orig_gtb);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.new_gpk_v2);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), f.orig_gpk);

    // Legacy pre-sentinel upgrade (missing .applied) re-applies cleanly from .bak and writes sentinel.
    ASSERT_TRUE(fs::remove(sentinel_path));
    WriteBytes(sandbox.path() / "menu_patch.bin", patch_v1);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    AssertNoStrayTempFiles(sandbox.path());
    EXPECT_EQ(ReadBytes(sentinel_path), sentinel_v1);
    for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i])), f.new_standalone_v1[i]);
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i] + ".bak")),
                  f.orig_standalone[i]);
    }
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb"), f.new_gtb_v1);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb.bak"), f.orig_gtb);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.new_gpk_v1);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), f.orig_gpk);

    // FILE_ATTRIBUTE_READONLY on live target files and stale .applied sentinel still upgrades cleanly.
    SetReadOnly(sandbox.path() / "init2" / "mainControl.fxf");
    SetReadOnly(sandbox.path() / "dwq" / "sys0.gtb");
    SetReadOnly(sandbox.path() / "dwq" / "sys0.gpk");
    SetReadOnly(sentinel_path);
    WriteBytes(sandbox.path() / "menu_patch.bin", patch_v2);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    AssertNoStrayTempFiles(sandbox.path());
    EXPECT_EQ(ReadBytes(sentinel_path), sentinel_v2);
    for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i])), f.new_standalone_v2[i]);
        EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i] + ".bak")),
                  f.orig_standalone[i]);
    }
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb"), f.new_gtb_v2);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb.bak"), f.orig_gtb);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.new_gpk_v2);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), f.orig_gpk);

    Bytes tampered_xtx = f.new_standalone_v2[0];
    tampered_xtx[0] ^= kFaultFlipByte;
    WriteBytes(sandbox.path() / fs::u8path(f.rel_paths[0]), tampered_xtx);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    AssertNoStrayTempFiles(sandbox.path());
    EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[0])), f.new_standalone_v2[0]);
}

TEST(MenuAutopatchCt, MidBatchOriginalMismatchRollsBackWithoutPartialMutation) {
    const MultiRecordFixture f = MakeSameSizeV2Fixture();
    const Bytes patch_v1 =
        SerializePatchBin(f, f.new_standalone_v1, f.new_entry1_v1, f.new_gtb_v1, f.new_gpk_v1);

    {
        ScopedTempDir sandbox(L"agh_32bit_rollback_standalone_ct");
        PopulateCleanInstall(sandbox.path(), f);

        Bytes bad_log = f.orig_standalone[2];
        bad_log[0] ^= kFaultFlipByte;
        WriteBytes(sandbox.path() / "init2" / "backlog.fxf", bad_log);
        WriteBytes(sandbox.path() / "menu_patch.bin", patch_v1);

        EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
        AssertNoStrayTempFiles(sandbox.path());
        EXPECT_FALSE(fs::exists(sandbox.path() / "menu_patch.bin.applied"));
        for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
            EXPECT_FALSE(fs::exists(sandbox.path() / fs::u8path(f.rel_paths[i] + ".bak")));
        }
        EXPECT_FALSE(fs::exists(sandbox.path() / "dwq" / "sys0.gpk.bak"));
        EXPECT_FALSE(fs::exists(sandbox.path() / "dwq" / "sys0.gtb.bak"));
        EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx"), f.orig_standalone[0]);
        EXPECT_EQ(ReadBytes(sandbox.path() / "init2" / "mainControl.fxf"), f.orig_standalone[1]);
        EXPECT_EQ(ReadBytes(sandbox.path() / "init2" / "backlog.fxf"), bad_log);
        EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.orig_gpk);
    }

    {
        ScopedTempDir sandbox(L"agh_32bit_rollback_archive_ct");
        PopulateCleanInstall(sandbox.path(), f);

        Bytes bad_gpk = f.orig_gpk;
        bad_gpk[f.orig_entry0.size()] ^= kFaultFlipByte;
        WriteBytes(sandbox.path() / "dwq" / "sys0.gpk", bad_gpk);
        WriteBytes(sandbox.path() / "menu_patch.bin", patch_v1);

        EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
        AssertNoStrayTempFiles(sandbox.path());
        EXPECT_FALSE(fs::exists(sandbox.path() / "menu_patch.bin.applied"));
        for (std::size_t i = 0; i < kStandaloneRecordCount; ++i) {
            EXPECT_FALSE(fs::exists(sandbox.path() / fs::u8path(f.rel_paths[i] + ".bak")));
            EXPECT_EQ(ReadBytes(sandbox.path() / fs::u8path(f.rel_paths[i])), f.orig_standalone[i]);
        }
        EXPECT_FALSE(fs::exists(sandbox.path() / "dwq" / "sys0.gpk.bak"));
        EXPECT_FALSE(fs::exists(sandbox.path() / "dwq" / "sys0.gtb.bak"));
        EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb"), f.orig_gtb);
        EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), bad_gpk);
    }
}
