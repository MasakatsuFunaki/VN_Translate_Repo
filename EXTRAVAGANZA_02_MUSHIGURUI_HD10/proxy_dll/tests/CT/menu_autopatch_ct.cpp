// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 32-bit Win32 component tests for Mushigurui's menu_autopatch ("MSGMENU1" v1)
// covering clean multi-archive/multi-variant installation under Unicode paths,
// "MSGMSNT1" .applied sentinel fast-path idempotence, same-size GPK entry v2
// upgrades from pristine .bak, strict .bak hash rejection, and mid-batch
// all-or-nothing rollback atomicity.
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
constexpr char kPatchMagic[kMagicBytes] = {'M', 'S', 'G', 'M', 'E', 'N', 'U', '1'};
constexpr char kSentinelMagic[kMagicBytes] = {'M', 'S', 'G', 'M', 'S', 'N', 'T', '1'};
constexpr char kOver2GTrailer[kMagicBytes] = {'o', 'v', 'e', 'r', '2', 'G', '!', '\0'};
constexpr std::size_t kExpectedSentinelSize = 20u;
constexpr std::uint32_t kPatchVersion = 1u;

constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::uint64_t kLowU32Mask = 0xFFFFFFFFull;
constexpr unsigned int kShift8 = 8u;
constexpr unsigned int kShift16 = 16u;
constexpr unsigned int kShift24 = 24u;
constexpr unsigned int kBitsPerU64 = 64u;
constexpr std::uint8_t kFaultFlipMask = 0x5Au;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::wstring_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        const std::wstring dir_name =
            std::wstring(prefix) + L"_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(id);
        path_ = fs::temp_directory_path() / fs::path(dir_name);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
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

void AppendU64Le(Bytes& out, std::uint64_t v) {
    for (unsigned int s = 0; s < kBitsPerU64; s += kShift8) {
        out.push_back(static_cast<std::uint8_t>((v >> s) & kLowByteMask));
    }
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

void BuildOver2GGtbGpk(const std::vector<std::string>& names,
                       const std::vector<Bytes>& entries,
                       Bytes& out_gtb,
                       Bytes& out_gpk) {
    out_gtb.clear();
    out_gpk.clear();
    const auto count = static_cast<std::uint32_t>(entries.size());

    Bytes name_blob;
    std::vector<std::uint32_t> name_offs;
    std::vector<std::uint64_t> data_offs;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        name_offs.push_back(static_cast<std::uint32_t>(name_blob.size()));
        name_blob.insert(name_blob.end(), names[i].begin(), names[i].end());
        name_blob.push_back(0u);
        data_offs.push_back(static_cast<std::uint64_t>(out_gpk.size()));
        out_gpk.insert(out_gpk.end(), entries[i].begin(), entries[i].end());
    }

    AppendU32Le(out_gtb, count);
    for (std::uint32_t noff : name_offs) {
        AppendU32Le(out_gtb, noff);
    }
    for (std::uint64_t doff : data_offs) {
        AppendU32Le(out_gtb, static_cast<std::uint32_t>(doff & kLowU32Mask));
    }
    out_gtb.insert(out_gtb.end(), name_blob.begin(), name_blob.end());
    while (out_gtb.size() % 8u != 0u) {
        out_gtb.push_back(0u);
    }
    for (std::uint64_t doff : data_offs) {
        AppendU64Le(out_gtb, doff);
    }
    AppendU64Le(out_gtb, 0ull);
    out_gtb.insert(out_gtb.end(), kOver2GTrailer, kOver2GTrailer + kMagicBytes);
}

struct VariantSpec {
    Bytes orig;
    Bytes patched;
};

struct StandaloneSpec {
    std::string rel_path;
    std::vector<VariantSpec> variants;
};

struct ArchiveSpec {
    std::string stem;
    Bytes orig_gtb;
    Bytes orig_gpk;
    Bytes new_gtb;
    Bytes new_gpk;
    std::uint32_t entry_index = 0u;
    std::uint64_t orig_offset = 0ull;
    Bytes orig_entry;
    Bytes new_entry;
};

Bytes SerializeMsgMenu1(const std::vector<StandaloneSpec>& standalones,
                        const std::vector<ArchiveSpec>& archives) {
    Bytes out;
    out.insert(out.end(), kPatchMagic, kPatchMagic + kMagicBytes);
    AppendU32Le(out, kPatchVersion);
    AppendU32Le(out, static_cast<std::uint32_t>(standalones.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(archives.size()));
    AppendU32Le(out, 0u);     // flags
    AppendU64Le(out, 0ull);   // reserved

    for (const auto& s : standalones) {
        AppendU16Le(out, static_cast<std::uint16_t>(s.rel_path.size()));
        out.insert(out.end(), s.rel_path.begin(), s.rel_path.end());
        AppendU16Le(out, static_cast<std::uint16_t>(s.variants.size()));
        for (const auto& v : s.variants) {
            const std::uint32_t orig_h =
                menu_autopatch::ComputeMurmurHash2(v.orig.data(), v.orig.size());
            const std::uint32_t new_h =
                menu_autopatch::ComputeMurmurHash2(v.patched.data(), v.patched.size());
            Bytes masked = v.patched;
            menu_autopatch::XorMaskPayload(
                masked.data(), masked.size(), v.orig.data(), v.orig.size(), orig_h);
            AppendU32Le(out, static_cast<std::uint32_t>(v.orig.size()));
            AppendU32Le(out, orig_h);
            AppendU32Le(out, static_cast<std::uint32_t>(v.patched.size()));
            AppendU32Le(out, new_h);
            out.insert(out.end(), masked.begin(), masked.end());
        }
    }

    for (const auto& a : archives) {
        const std::uint32_t orig_gtb_h =
            menu_autopatch::ComputeMurmurHash2(a.orig_gtb.data(), a.orig_gtb.size());
        const std::uint32_t new_gtb_h =
            menu_autopatch::ComputeMurmurHash2(a.new_gtb.data(), a.new_gtb.size());
        Bytes masked_gtb = a.new_gtb;
        menu_autopatch::XorMaskPayload(
            masked_gtb.data(), masked_gtb.size(), a.orig_gtb.data(), a.orig_gtb.size(), orig_gtb_h);

        AppendU16Le(out, static_cast<std::uint16_t>(a.stem.size()));
        out.insert(out.end(), a.stem.begin(), a.stem.end());
        AppendU64Le(out, static_cast<std::uint64_t>(a.orig_gpk.size()));
        AppendU64Le(out, static_cast<std::uint64_t>(a.new_gpk.size()));
        AppendU32Le(out, static_cast<std::uint32_t>(a.orig_gtb.size()));
        AppendU32Le(out, orig_gtb_h);
        AppendU32Le(out, static_cast<std::uint32_t>(a.new_gtb.size()));
        AppendU32Le(out, new_gtb_h);
        AppendU32Le(out, 1u);
        out.insert(out.end(), masked_gtb.begin(), masked_gtb.end());

        const std::uint32_t orig_e_h =
            menu_autopatch::ComputeMurmurHash2(a.orig_entry.data(), a.orig_entry.size());
        const std::uint32_t new_e_h =
            menu_autopatch::ComputeMurmurHash2(a.new_entry.data(), a.new_entry.size());
        Bytes masked_entry = a.new_entry;
        menu_autopatch::XorMaskPayload(
            masked_entry.data(), masked_entry.size(), a.orig_entry.data(), a.orig_entry.size(), orig_e_h);

        AppendU32Le(out, a.entry_index);
        AppendU64Le(out, a.orig_offset);
        AppendU32Le(out, static_cast<std::uint32_t>(a.orig_entry.size()));
        AppendU32Le(out, orig_e_h);
        AppendU32Le(out, static_cast<std::uint32_t>(a.new_entry.size()));
        AppendU32Le(out, new_e_h);
        out.insert(out.end(), masked_entry.begin(), masked_entry.end());
    }
    return out;
}

struct MushiguruiTestFixture {
    Bytes orig_xtx;
    Bytes v1_xtx;
    Bytes v2_xtx;
    Bytes orig_main_hd10;
    Bytes orig_main_ce;
    Bytes v1_main_hd10;
    Bytes v1_main_ce;
    Bytes sys0_e0;
    Bytes sys0_e1_orig;
    Bytes sys0_e1_v1;
    Bytes sys0_e1_v2_same_size;
    Bytes sys0_e2;
    Bytes ta0_e0;
    Bytes ta0_e1_orig;
    Bytes ta0_e1_v1;
    Bytes sys0_orig_gtb;
    Bytes sys0_orig_gpk;
    Bytes sys0_v1_gtb;
    Bytes sys0_v1_gpk;
    Bytes sys0_v2_gtb;
    Bytes sys0_v2_gpk;
    Bytes ta0_orig_gtb;
    Bytes ta0_orig_gpk;
    Bytes ta0_v1_gtb;
    Bytes ta0_v1_gpk;
    Bytes patch_v1;
    Bytes patch_v2_same_size;
};

MushiguruiTestFixture BuildFixture() {
    MushiguruiTestFixture f;
    const std::string s_orig_xtx = "aa,美弥香\r\nbb,レン\r\n";
    const std::string s_v1_xtx = "aa,Miyaka_v1\r\nbb,Ren_v1\r\n";
    const std::string s_v2_xtx = "aa,Miyaka_v2\r\nbb,Ren_v2\r\n";  // identical byte length to v1!
    f.orig_xtx.assign(s_orig_xtx.begin(), s_orig_xtx.end());
    f.v1_xtx.assign(s_v1_xtx.begin(), s_v1_xtx.end());
    f.v2_xtx.assign(s_v2_xtx.begin(), s_v2_xtx.end());

    const std::string hd10_o = "windowTitle HD10_JP\r\n";
    const std::string ce_o = "windowTitle CE_JP\r\n";
    const std::string hd10_n = "windowTitle HD10_EN\r\n";
    const std::string ce_n = "windowTitle CE_EN\r\n";
    f.orig_main_hd10.assign(hd10_o.begin(), hd10_o.end());
    f.orig_main_ce.assign(ce_o.begin(), ce_o.end());
    f.v1_main_hd10.assign(hd10_n.begin(), hd10_n.end());
    f.v1_main_ce.assign(ce_n.begin(), ce_n.end());

    // Exact same byte size for orig, v1, and v2 on sys0 entry #1 so GTB is
    // 100% identical between orig, v1, and v2 (stressing the manifest sentinel!).
    f.sys0_e0 = {0x89u, 'P', 'N', 'G', 0x00u, 0x7Fu, 0x80u, 0xFFu};
    f.sys0_e1_orig = {0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u, 0x90u, 0xA0u};
    f.sys0_e1_v1 = {0x11u, 0x21u, 0x31u, 0x41u, 0x51u, 0x61u, 0x71u, 0x81u, 0x91u, 0xA1u};
    f.sys0_e1_v2_same_size = {0x99u, 0x88u, 0x77u, 0x66u, 0x55u, 0x44u, 0x33u, 0x22u, 0x11u, 0x00u};
    f.sys0_e2 = {0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu};

    f.ta0_e0 = {0x01u, 0x02u, 0x03u, 0x04u, 0x05u};
    f.ta0_e1_orig = {0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u};
    f.ta0_e1_v1 = {0x41u, 0x42u, 0x43u, 0x44u, 0x45u, 0x46u, 0x47u, 0x48u};

    BuildOver2GGtbGpk({"s_e0", "s_e1", "s_e2"}, {f.sys0_e0, f.sys0_e1_orig, f.sys0_e2},
                      f.sys0_orig_gtb, f.sys0_orig_gpk);
    BuildOver2GGtbGpk({"s_e0", "s_e1", "s_e2"}, {f.sys0_e0, f.sys0_e1_v1, f.sys0_e2},
                      f.sys0_v1_gtb, f.sys0_v1_gpk);
    BuildOver2GGtbGpk({"s_e0", "s_e1", "s_e2"}, {f.sys0_e0, f.sys0_e1_v2_same_size, f.sys0_e2},
                      f.sys0_v2_gtb, f.sys0_v2_gpk);

    BuildOver2GGtbGpk({"t_e0", "t_e1"}, {f.ta0_e0, f.ta0_e1_orig}, f.ta0_orig_gtb, f.ta0_orig_gpk);
    BuildOver2GGtbGpk({"t_e0", "t_e1"}, {f.ta0_e0, f.ta0_e1_v1}, f.ta0_v1_gtb, f.ta0_v1_gpk);

    const std::vector<StandaloneSpec> st_v1 = {
        {"spt\\charaname.xtx", {{f.orig_xtx, f.v1_xtx}}},
        {"init2\\mainControl.fxf", {{f.orig_main_hd10, f.v1_main_hd10}, {f.orig_main_ce, f.v1_main_ce}}},
    };
    const std::vector<StandaloneSpec> st_v2 = {
        {"spt\\charaname.xtx", {{f.orig_xtx, f.v2_xtx}}},
        {"init2\\mainControl.fxf", {{f.orig_main_hd10, f.v1_main_hd10}, {f.orig_main_ce, f.v1_main_ce}}},
    };
    const std::vector<ArchiveSpec> arc_v1 = {
        {"dwq\\sys0", f.sys0_orig_gtb, f.sys0_orig_gpk, f.sys0_v1_gtb, f.sys0_v1_gpk,
         1u, static_cast<std::uint64_t>(f.sys0_e0.size()), f.sys0_e1_orig, f.sys0_e1_v1},
        {"dwq\\ta0", f.ta0_orig_gtb, f.ta0_orig_gpk, f.ta0_v1_gtb, f.ta0_v1_gpk,
         1u, static_cast<std::uint64_t>(f.ta0_e0.size()), f.ta0_e1_orig, f.ta0_e1_v1},
    };
    const std::vector<ArchiveSpec> arc_v2 = {
        {"dwq\\sys0", f.sys0_orig_gtb, f.sys0_orig_gpk, f.sys0_v2_gtb, f.sys0_v2_gpk,
         1u, static_cast<std::uint64_t>(f.sys0_e0.size()), f.sys0_e1_orig, f.sys0_e1_v2_same_size},
        {"dwq\\ta0", f.ta0_orig_gtb, f.ta0_orig_gpk, f.ta0_v1_gtb, f.ta0_v1_gpk,
         1u, static_cast<std::uint64_t>(f.ta0_e0.size()), f.ta0_e1_orig, f.ta0_e1_v1},
    };

    f.patch_v1 = SerializeMsgMenu1(st_v1, arc_v1);
    f.patch_v2_same_size = SerializeMsgMenu1(st_v2, arc_v2);
    return f;
}

void PopulateCleanHd10Install(const fs::path& root, const MushiguruiTestFixture& f) {
    WriteBytes(root / "spt" / "charaname.xtx", f.orig_xtx);
    WriteBytes(root / "init2" / "mainControl.fxf", f.orig_main_hd10);
    WriteBytes(root / "dwq" / "sys0.gtb", f.sys0_orig_gtb);
    WriteBytes(root / "dwq" / "sys0.gpk", f.sys0_orig_gpk);
    WriteBytes(root / "dwq" / "ta0.gtb", f.ta0_orig_gtb);
    WriteBytes(root / "dwq" / "ta0.gpk", f.ta0_orig_gpk);
}

void ExpectZeroTempFilesRecursively(const fs::path& root) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        const bool is_tmp =
            name.size() >= 4u && name.compare(name.size() - 4u, 4u, L".tmp") == 0;
        EXPECT_FALSE(is_tmp) << "stray temp file: " << entry.path().u8string();
    }
}

void SetReadOnlyAttr(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    ASSERT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    ASSERT_TRUE(SetFileAttributesW(path.c_str(), attrs | FILE_ATTRIBUTE_READONLY));
}

}  // namespace

TEST(MushiguruiMenuAutopatchCt, CleanInstallFastPathAndSameSizeGpkV2Upgrade) {
    ScopedTempDir sandbox(L"mgi_32bit_蟲狂編_upgrade_ct");
    const MushiguruiTestFixture f = BuildFixture();
    ASSERT_EQ(f.patch_v1.size(), f.patch_v2_same_size.size());
    ASSERT_EQ(f.sys0_v1_gtb, f.sys0_v2_gtb);
    ASSERT_EQ(f.sys0_v1_gpk.size(), f.sys0_v2_gpk.size());

    PopulateCleanHd10Install(sandbox.path(), f);
    WriteBytes(sandbox.path() / "menu_patch.bin", f.patch_v1);
    // Mark live retail archive read-only and plant a pre-existing read-only .tmp
    // file to verify read-only attribute clearing before overwrite & swap.
    SetReadOnlyAttr(sandbox.path() / "dwq" / "sys0.gpk");
    WriteBytes(sandbox.path() / "dwq" / "sys0.gpk.tmp", Bytes{0xDEu, 0xADu});
    SetReadOnlyAttr(sandbox.path() / "dwq" / "sys0.gpk.tmp");

    // Run 1: clean install patches all targets, creates .bak, writes MSGMSNT1 sentinel.
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    ExpectZeroTempFilesRecursively(sandbox.path());

    const fs::path sentinel_path = sandbox.path() / "menu_patch.bin.applied";
    ASSERT_TRUE(fs::is_regular_file(sentinel_path));
    const Bytes sentinel_v1 = ReadBytes(sentinel_path);
    ASSERT_EQ(sentinel_v1.size(), kExpectedSentinelSize);
    EXPECT_EQ(std::memcmp(sentinel_v1.data(), kSentinelMagic, kMagicBytes), 0);

    EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx"), f.v1_xtx);
    EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx.bak"), f.orig_xtx);
    EXPECT_EQ(ReadBytes(sandbox.path() / "init2" / "mainControl.fxf"), f.v1_main_hd10);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.sys0_v1_gpk);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), f.sys0_orig_gpk);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "ta0.gpk"), f.ta0_v1_gpk);

    // Run 2: fast-path leaves .bak files untouched.
    const auto gpk_bak_mtime = fs::last_write_time(sandbox.path() / "dwq" / "sys0.gpk.bak");
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    EXPECT_EQ(fs::last_write_time(sandbox.path() / "dwq" / "sys0.gpk.bak"), gpk_bak_mtime);
    ExpectZeroTempFilesRecursively(sandbox.path());

    // Run 3: drop same-size v2 menu_patch.bin (identical file size, identical
    // sys0.gtb, identical sys0.gpk size — ONLY entry #1 payload & xtx changed).
    // Manifest hash in .applied must trigger clean re-patch from pristine .bak.
    WriteBytes(sandbox.path() / "menu_patch.bin", f.patch_v2_same_size);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    ExpectZeroTempFilesRecursively(sandbox.path());

    EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx"), f.v2_xtx);
    EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx.bak"), f.orig_xtx);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.sys0_v2_gpk);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), f.sys0_orig_gpk);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gtb.bak"), f.sys0_orig_gtb);
    const Bytes sentinel_v2 = ReadBytes(sentinel_path);
    EXPECT_NE(sentinel_v2, sentinel_v1);

    // Run 4: legacy pre-sentinel install (patched v2 on disk with valid .bak
    // but NO .applied file) recovers cleanly from .bak back to v1 and writes
    // the fresh MSGMSNT1 sentinel.
    std::error_code ec;
    fs::remove(sentinel_path, ec);
    ASSERT_FALSE(fs::exists(sentinel_path));
    WriteBytes(sandbox.path() / "menu_patch.bin", f.patch_v1);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
    ExpectZeroTempFilesRecursively(sandbox.path());
    EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx"), f.v1_xtx);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.sys0_v1_gpk);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), f.sys0_orig_gpk);
    EXPECT_EQ(ReadBytes(sentinel_path), sentinel_v1);
}

TEST(MushiguruiMenuAutopatchCt, RejectsCorruptSameSizeBakAndMidBatchFaultsAtomically) {
    const MushiguruiTestFixture f = BuildFixture();

    // 1. Corrupt standalone .bak with exact orig_file_size must be rejected
    // cleanly without mutating live files or creating .applied.
    {
        ScopedTempDir sandbox(L"mgi_32bit_corrupt_st_bak_ct");
        PopulateCleanHd10Install(sandbox.path(), f);
        Bytes corrupt_bak = f.orig_xtx;
        corrupt_bak[0] ^= kFaultFlipMask;
        WriteBytes(sandbox.path() / "spt" / "charaname.xtx.bak", corrupt_bak);
        WriteBytes(sandbox.path() / "menu_patch.bin", f.patch_v1);

        EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
        EXPECT_FALSE(fs::exists(sandbox.path() / "menu_patch.bin.applied"));
        ExpectZeroTempFilesRecursively(sandbox.path());
        EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx"), f.orig_xtx);
        EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.sys0_orig_gpk);
    }

    // 2. Corrupt archive .gtb.bak with exact orig_gtb_size must be rejected
    // cleanly without mutating any live file or leaving .tmp artifacts.
    {
        ScopedTempDir sandbox(L"mgi_32bit_corrupt_gtb_bak_ct");
        PopulateCleanHd10Install(sandbox.path(), f);
        Bytes corrupt_gtb = f.sys0_orig_gtb;
        corrupt_gtb[0] ^= kFaultFlipMask;
        WriteBytes(sandbox.path() / "dwq" / "sys0.gtb.bak", corrupt_gtb);
        WriteBytes(sandbox.path() / "menu_patch.bin", f.patch_v1);

        EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
        EXPECT_FALSE(fs::exists(sandbox.path() / "menu_patch.bin.applied"));
        ExpectZeroTempFilesRecursively(sandbox.path());
        EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx"), f.orig_xtx);
        EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.sys0_orig_gpk);
    }

    // 3. Mid-batch fault on archive #2 rolls back standalone + archive #1 temps.
    {
        ScopedTempDir sandbox(L"mgi_32bit_rollback_ct");
        PopulateCleanHd10Install(sandbox.path(), f);
        Bytes corrupt_ta0 = f.ta0_orig_gpk;
        corrupt_ta0.back() ^= kFaultFlipMask;
        WriteBytes(sandbox.path() / "dwq" / "ta0.gpk", corrupt_ta0);
        WriteBytes(sandbox.path() / "menu_patch.bin", f.patch_v1);

        EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));
        EXPECT_FALSE(fs::exists(sandbox.path() / "menu_patch.bin.applied"));
        EXPECT_FALSE(fs::exists(sandbox.path() / "spt" / "charaname.xtx.bak"));
        EXPECT_FALSE(fs::exists(sandbox.path() / "dwq" / "sys0.gpk.bak"));
        ExpectZeroTempFilesRecursively(sandbox.path());
        EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "charaname.xtx"), f.orig_xtx);
        EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), f.sys0_orig_gpk);
    }
}
