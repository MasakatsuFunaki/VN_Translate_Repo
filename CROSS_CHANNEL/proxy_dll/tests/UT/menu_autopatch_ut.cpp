// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — menu_autopatch (atomic backup, .applied fast-path, re-patch & recovery)

#include "menu_autopatch.h"
#include "menu_patch_format.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

class ScopedTempGameDir {
public:
    ScopedTempGameDir() {
        static std::atomic<std::uint32_t> counter{0};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint32_t id = ++counter;
        root_ = fs::temp_directory_path() /
                ("cc_menu_autopatch_ut_" + std::to_string(tick) + "_" +
                 std::to_string(id));
        fs::create_directories(root_ / "data");
    }

    ~ScopedTempGameDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    ScopedTempGameDir(const ScopedTempGameDir&) = delete;
    ScopedTempGameDir& operator=(const ScopedTempGameDir&) = delete;

    const fs::path& path() const noexcept { return root_; }

private:
    fs::path root_;
};

void WriteBinaryFile(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out));
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    out.flush();
    ASSERT_TRUE(static_cast<bool>(out));
}

Bytes ReadBinaryFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    Bytes buf(sz);
    if (sz > 0) {
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(sz));
    }
    return buf;
}

void AppendBe16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}
void AppendBe32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}
void AppendBe64(Bytes& out, std::uint64_t v) {
    AppendBe32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
    AppendBe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}
void AppendLe32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

struct UtfCol {
    std::uint8_t flag;
    std::string name;
};

Bytes BuildUtf(const std::string& tname,
               const std::vector<UtfCol>& cols,
               const std::vector<Bytes>& rows,
               const Bytes& blobs = {}) {
    Bytes strs;
    auto add_s = [&](const std::string& s) -> std::uint32_t {
        const auto off = static_cast<std::uint32_t>(strs.size());
        strs.insert(strs.end(), s.begin(), s.end());
        strs.push_back(0);
        return off;
    };
    const std::uint32_t tname_off = add_s(tname);
    Bytes schema;
    for (const auto& c : cols) {
        schema.push_back(c.flag);
        AppendBe32(schema, add_s(c.name));
    }
    const std::uint16_t row_len =
        rows.empty() ? 0u : static_cast<std::uint16_t>(rows[0].size());
    Bytes row_buf;
    for (const auto& r : rows) {
        row_buf.insert(row_buf.end(), r.begin(), r.end());
    }
    const auto rows_off = static_cast<std::uint32_t>(24u + schema.size());
    const auto strs_off = rows_off + static_cast<std::uint32_t>(row_buf.size());
    const auto data_off = strs_off + static_cast<std::uint32_t>(strs.size());
    const auto tsize = data_off + static_cast<std::uint32_t>(blobs.size());

    Bytes out = {'@', 'U', 'T', 'F'};
    AppendBe32(out, tsize);
    AppendBe32(out, rows_off);
    AppendBe32(out, strs_off);
    AppendBe32(out, data_off);
    AppendBe32(out, tname_off);
    AppendBe16(out, static_cast<std::uint16_t>(cols.size()));
    AppendBe16(out, row_len);
    AppendBe32(out, static_cast<std::uint32_t>(rows.size()));
    out.insert(out.end(), schema.begin(), schema.end());
    out.insert(out.end(), row_buf.begin(), row_buf.end());
    out.insert(out.end(), strs.begin(), strs.end());
    out.insert(out.end(), blobs.begin(), blobs.end());
    return out;
}

Bytes BuildFixtureCpk(const Bytes& sub0_payload) {
    Bytes pak;
    AppendLe32(pak, 16u);
    AppendLe32(pak, static_cast<std::uint32_t>(sub0_payload.size()));
    AppendLe32(pak, 0u);
    AppendLe32(pak, 0u);
    pak.insert(pak.end(), sub0_payload.begin(), sub0_payload.end());

    Bytes comp;
    menu_patch::LzssCompressZeroRing(pak, comp);
    const auto sz = static_cast<std::uint32_t>(comp.size());

    std::vector<Bytes> rows_l;
    std::vector<Bytes> rows_h;
    if (sz <= 0xFFFFu) {
        Bytes r;
        AppendBe16(r, 0u);
        AppendBe16(r, static_cast<std::uint16_t>(sz));
        AppendBe16(r, static_cast<std::uint16_t>(sz));
        rows_l.push_back(r);
    } else {
        Bytes r;
        AppendBe16(r, 0u);
        AppendBe32(r, sz);
        AppendBe32(r, sz);
        rows_h.push_back(r);
    }
    const Bytes dl = BuildUtf(
        "CpkItocL", {{0x52, "ID"}, {0x52, "FileSize"}, {0x52, "ExtractSize"}}, rows_l);
    const Bytes dh = BuildUtf(
        "CpkItocH", {{0x52, "ID"}, {0x54, "FileSize"}, {0x54, "ExtractSize"}}, rows_h);
    Bytes blobs = dl;
    const auto dh_off = static_cast<std::uint32_t>(blobs.size());
    blobs.insert(blobs.end(), dh.begin(), dh.end());

    Bytes ir;
    AppendBe32(ir, static_cast<std::uint32_t>(rows_l.size()));
    AppendBe32(ir, static_cast<std::uint32_t>(rows_h.size()));
    AppendBe32(ir, 0u);
    AppendBe32(ir, static_cast<std::uint32_t>(dl.size()));
    AppendBe32(ir, dh_off);
    AppendBe32(ir, static_cast<std::uint32_t>(dh.size()));
    const Bytes itoc_utf = BuildUtf(
        "CpkItocInfo",
        {{0x54, "FilesL"}, {0x54, "FilesH"}, {0x5B, "DataL"}, {0x5B, "DataH"}},
        {ir},
        blobs);

    const std::uint64_t csize = (static_cast<std::uint64_t>(sz) + 0x7FFu) & ~0x7FFull;
    Bytes hr;
    AppendBe64(hr, 0x1000u);
    AppendBe64(hr, csize);
    AppendBe64(hr, 0x800u);
    AppendBe64(hr, 16u + itoc_utf.size());
    AppendBe64(hr, sz);
    AppendBe64(hr, sz);
    AppendBe32(hr, 1u);
    AppendBe16(hr, 0x800u);
    const Bytes cpk_utf = BuildUtf(
        "CpkHeader",
        {{0x56, "ContentOffset"},
         {0x56, "ContentSize"},
         {0x56, "ItocOffset"},
         {0x56, "ItocSize"},
         {0x56, "EnabledPackedSize"},
         {0x56, "EnabledDataSize"},
         {0x54, "Files"},
         {0x52, "Align"}},
        {hr});

    Bytes cpk(0x1000u, 0u);
    std::memcpy(cpk.data(), "CPK ", 4);
    cpk[4] = 0xFFu;
    cpk[8] = static_cast<std::uint8_t>(cpk_utf.size() & 0xFFu);
    cpk[9] = static_cast<std::uint8_t>((cpk_utf.size() >> 8) & 0xFFu);
    std::memcpy(cpk.data() + 16, cpk_utf.data(), cpk_utf.size());

    std::memcpy(cpk.data() + 0x800, "ITOC", 4);
    cpk[0x804] = 0xFFu;
    cpk[0x808] = static_cast<std::uint8_t>(itoc_utf.size() & 0xFFu);
    cpk[0x809] = static_cast<std::uint8_t>((itoc_utf.size() >> 8) & 0xFFu);
    std::memcpy(cpk.data() + 0x810, itoc_utf.data(), itoc_utf.size());

    cpk.insert(cpk.end(), comp.begin(), comp.end());
    return cpk;
}

}  // namespace

TEST(MenuAutoPatch, MissingPatchOrMissingSysCpkIsGracefulSkip) {
    ScopedTempGameDir dir;

    // Neither menu_patch.bin nor data\sys.cpk exists.
    const auto r1 = menu_autopatch::ApplyMenuPatchDetailed(dir.path());
    EXPECT_EQ(r1.status, menu_autopatch::PatchStatus::kSkippedMissingInput);
    EXPECT_FALSE(menu_autopatch::ApplyMenuPatch(dir.path()));

    // data\sys.cpk exists, but menu_patch.bin is absent -> leaves sys.cpk untouched.
    const Bytes orig_sub(2048u, 0x22u);
    const Bytes orig_cpk = BuildFixtureCpk(orig_sub);
    WriteBinaryFile(dir.path() / "data" / "sys.cpk", orig_cpk);

    const auto r2 = menu_autopatch::ApplyMenuPatchDetailed(dir.path());
    EXPECT_EQ(r2.status, menu_autopatch::PatchStatus::kSkippedMissingInput);
    EXPECT_FALSE(fs::exists(dir.path() / "data" / "sys.cpk.bak"));
    EXPECT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk"), orig_cpk);
}

TEST(MenuAutoPatch, FirstLaunchCreatesBackupAndSentinelSecondLaunchHitsFastPath) {
    ScopedTempGameDir dir;

    Bytes orig_sub(4096u);
    for (std::size_t i = 0; i < orig_sub.size(); ++i) {
        orig_sub[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    Bytes en_sub_v1 = orig_sub;
    std::fill_n(en_sub_v1.begin() + 64, 128, static_cast<std::uint8_t>(0xEEu));

    const Bytes orig_cpk = BuildFixtureCpk(orig_sub);
    Bytes patch_v1;
    Bytes expected_cpk_v1;
    ASSERT_TRUE(menu_patch::MenuPatchPackage::BuildPatchBlob(
        orig_cpk, {{0u, 0u, orig_sub, en_sub_v1}}, patch_v1, &expected_cpk_v1));

    WriteBinaryFile(dir.path() / "data" / "sys.cpk", orig_cpk);
    WriteBinaryFile(dir.path() / "menu_patch.bin", patch_v1);

    // Launch 1: creates data\sys.cpk.bak, patches data\sys.cpk, writes .applied.
    const auto launch1 = menu_autopatch::ApplyMenuPatchDetailed(dir.path());
    EXPECT_EQ(launch1.status, menu_autopatch::PatchStatus::kApplied);
    EXPECT_EQ(launch1.patched_size, expected_cpk_v1.size());
    EXPECT_TRUE(fs::exists(dir.path() / "data" / "sys.cpk.bak"));
    EXPECT_TRUE(fs::exists(dir.path() / "data" / "sys.cpk.applied"));
    EXPECT_FALSE(fs::exists(dir.path() / "data" / "sys.cpk.tmp"));
    EXPECT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk.bak"), orig_cpk);
    EXPECT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk"), expected_cpk_v1);

    // Launch 2: hits <1 ms sentinel fast-path without rewriting sys.cpk.
    const auto launch2 = menu_autopatch::ApplyMenuPatchDetailed(dir.path());
    EXPECT_EQ(launch2.status, menu_autopatch::PatchStatus::kAlreadyUpToDate);
    EXPECT_TRUE(menu_autopatch::ApplyMenuPatch(dir.path()));
}

TEST(MenuAutoPatch, UpdatedPatchBlobCleanlyReAppliesFromOriginalBackup) {
    ScopedTempGameDir dir;

    Bytes orig_sub(4096u);
    for (std::size_t i = 0; i < orig_sub.size(); ++i) {
        orig_sub[i] = static_cast<std::uint8_t>((i * 53u + 7u) & 0xFFu);
    }
    Bytes en_sub_v1 = orig_sub;
    std::fill_n(en_sub_v1.begin() + 32, 64, static_cast<std::uint8_t>(0xAAu));

    Bytes en_sub_v2 = orig_sub;
    for (std::size_t i = 100; i < 600; ++i) {
        en_sub_v2[i] = static_cast<std::uint8_t>((i * 97u) & 0xFFu);
    }

    const Bytes orig_cpk = BuildFixtureCpk(orig_sub);
    Bytes patch_v1;
    Bytes expected_v1;
    Bytes patch_v2;
    Bytes expected_v2;
    ASSERT_TRUE(menu_patch::MenuPatchPackage::BuildPatchBlob(
        orig_cpk, {{0u, 0u, orig_sub, en_sub_v1}}, patch_v1, &expected_v1));
    ASSERT_TRUE(menu_patch::MenuPatchPackage::BuildPatchBlob(
        orig_cpk, {{0u, 0u, orig_sub, en_sub_v2}}, patch_v2, &expected_v2));

    WriteBinaryFile(dir.path() / "data" / "sys.cpk", orig_cpk);
    WriteBinaryFile(dir.path() / "menu_patch.bin", patch_v1);
    ASSERT_EQ(menu_autopatch::ApplyMenuPatchDetailed(dir.path()).status,
              menu_autopatch::PatchStatus::kApplied);
    ASSERT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk"), expected_v1);

    // Replace menu_patch.bin with v2 -> must detect stale .applied, preserve
    // original .bak, and rebuild expected_v2 from .bak.
    WriteBinaryFile(dir.path() / "menu_patch.bin", patch_v2);
    const auto upgrade = menu_autopatch::ApplyMenuPatchDetailed(dir.path());
    EXPECT_EQ(upgrade.status, menu_autopatch::PatchStatus::kApplied);
    EXPECT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk.bak"), orig_cpk);
    EXPECT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk"), expected_v2);

    // Subsequent launch after upgrade hits fast-path again.
    EXPECT_EQ(menu_autopatch::ApplyMenuPatchDetailed(dir.path()).status,
              menu_autopatch::PatchStatus::kAlreadyUpToDate);
}

TEST(MenuAutoPatch, RecoversMissingSysCpkFromExistingBackupAndRejectsCorruptPatch) {
    ScopedTempGameDir dir;

    Bytes orig_sub(3072u, 0x44u);
    Bytes en_sub = orig_sub;
    en_sub[0] = 0x99u;
    const Bytes orig_cpk = BuildFixtureCpk(orig_sub);
    Bytes patch_blob;
    Bytes expected_cpk;
    ASSERT_TRUE(menu_patch::MenuPatchPackage::BuildPatchBlob(
        orig_cpk, {{0u, 0u, orig_sub, en_sub}}, patch_blob, &expected_cpk));

    WriteBinaryFile(dir.path() / "data" / "sys.cpk", orig_cpk);
    WriteBinaryFile(dir.path() / "menu_patch.bin", patch_blob);
    ASSERT_TRUE(menu_autopatch::ApplyMenuPatch(dir.path()));

    // Simulate interrupted/deleted data\sys.cpk while data\sys.cpk.bak exists.
    fs::remove(dir.path() / "data" / "sys.cpk");
    const auto recovered = menu_autopatch::ApplyMenuPatchDetailed(dir.path());
    EXPECT_EQ(recovered.status, menu_autopatch::PatchStatus::kApplied);
    EXPECT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk"), expected_cpk);

    // Now corrupt menu_patch.bin and invalidate .applied -> must fail cleanly
    // without corrupting the existing valid data\sys.cpk!
    Bytes bad_patch = patch_blob;
    bad_patch[bad_patch.size() / 2] ^= 0xFFu;
    WriteBinaryFile(dir.path() / "menu_patch.bin", bad_patch);
    fs::remove(dir.path() / "data" / "sys.cpk.applied");

    const auto failed = menu_autopatch::ApplyMenuPatchDetailed(dir.path());
    EXPECT_EQ(failed.status, menu_autopatch::PatchStatus::kFailed);
    EXPECT_EQ(ReadBinaryFile(dir.path() / "data" / "sys.cpk"), expected_cpk);
}
