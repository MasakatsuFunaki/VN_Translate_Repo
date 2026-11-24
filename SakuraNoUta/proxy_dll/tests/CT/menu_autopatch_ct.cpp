// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box Component Tests for menu_autopatch::EnsureMenuAssetsPatched over
// multi-record "SNUMENU1" v1 manifests combining kFlagArchiveEntry (BURIKO
// ARC20 repacking) and kFlagBackupStandalone (loose asset writing with
// offset-shifted .arc / .arc.backup fallback, Launch-2 sentinel fast-path,
// .bak deletion recovery, and mid-batch corrupt-hash atomic rollback).

#include "menu_autopatch.h"
#include "menu_patch_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using menu_patch_format::AppliedSentinel;
using menu_patch_format::ArcStatus;
using menu_patch_format::ByteSpan;
using menu_patch_format::FindArc20EntrySlice;
using menu_patch_format::kArc20ExtraFieldBytes;
using menu_patch_format::kArc20ExtraFieldOffset;
using menu_patch_format::kArc20HeaderBytes;
using menu_patch_format::kArc20IndexEntryBytes;
using menu_patch_format::kArc20Magic;
using menu_patch_format::kArc20MagicBytes;
using menu_patch_format::kArc20NameFieldBytes;
using menu_patch_format::kFlagArchiveEntry;
using menu_patch_format::kFlagBackupStandalone;
using menu_patch_format::ManifestStatus;
using menu_patch_format::MenuPatchRecordInput;
using menu_patch_format::ParseAppliedSentinel;
using menu_patch_format::SentinelStatus;
using menu_patch_format::SerializeMenuPatchManifest;

constexpr std::size_t kRelOffsetPos = 96u;
constexpr std::size_t kSizePos = 100u;
constexpr std::size_t kOrigSliceLenA = 96u;
constexpr std::size_t kOrigSliceLenB = 112u;
constexpr std::size_t kOrigSliceLenC = 80u;
constexpr std::size_t kShiftedPadBytes = 320u;
constexpr unsigned int kBitsPerByte = 8u;
constexpr std::size_t kU32Bytes = 4u;
constexpr std::uint32_t kByteMask = 0xFFu;

class ScopedSandboxDir {
public:
    ScopedSandboxDir() {
        static std::atomic<std::uint64_t> counter{0u};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t seq = counter.fetch_add(1u) + 1u;
        path_ = fs::temp_directory_path() /
                ("snu_autopatch_ct_" + std::to_string(tick) + "_" +
                 std::to_string(seq));
        fs::create_directories(path_);
        wpath_ = path_.wstring();
    }

    ~ScopedSandboxDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedSandboxDir(const ScopedSandboxDir&) = delete;
    ScopedSandboxDir& operator=(const ScopedSandboxDir&) = delete;

    const fs::path& path() const noexcept { return path_; }
    const wchar_t* wpath() const noexcept { return wpath_.c_str(); }

private:
    fs::path path_;
    std::wstring wpath_;
};

void AppendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned int i = 0u; i < kU32Bytes; ++i) {
        out.push_back(
            static_cast<std::uint8_t>((value >> (i * kBitsPerByte)) & kByteMask));
    }
}

std::vector<std::uint8_t> MakePatternBlob(std::size_t size,
                                          std::uint8_t seed_a,
                                          std::uint8_t seed_b) {
    constexpr std::uint8_t kSeamBytes[4] = {0x00u, 0x7Fu, 0x80u, 0xFFu};
    std::vector<std::uint8_t> out(size, 0u);
    for (std::size_t i = 0u; i < size; ++i) {
        if (i < std::size(kSeamBytes)) {
            out[i] = static_cast<std::uint8_t>(kSeamBytes[i] ^ seed_a);
            continue;
        }
        out[i] = static_cast<std::uint8_t>(
            (seed_a + static_cast<std::uint8_t>(i * 13u)) ^ seed_b);
    }
    return out;
}

struct SyntheticArcEntry {
    std::string name;
    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra{};
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> BuildSyntheticArc20(
    const std::vector<SyntheticArcEntry>& entries) {
    std::vector<std::uint8_t> arc;
    arc.insert(arc.end(), kArc20Magic, kArc20Magic + kArc20MagicBytes);
    AppendLe32(arc, static_cast<std::uint32_t>(entries.size()));

    std::uint32_t rel_offset = 0u;
    for (const auto& e : entries) {
        std::array<std::uint8_t, kArc20NameFieldBytes> name_slot{};
        const std::size_t copy_len =
            std::min(e.name.size(), kArc20NameFieldBytes - 1u);
        std::memcpy(name_slot.data(), e.name.data(), copy_len);
        arc.insert(arc.end(), name_slot.begin(), name_slot.end());
        AppendLe32(arc, rel_offset);
        AppendLe32(arc, static_cast<std::uint32_t>(e.payload.size()));
        arc.insert(arc.end(), e.extra.begin(), e.extra.end());
        rel_offset += static_cast<std::uint32_t>(e.payload.size());
    }

    for (const auto& e : entries) {
        arc.insert(arc.end(), e.payload.begin(), e.payload.end());
    }
    return arc;
}

void WriteBinaryFile(const fs::path& path, ByteSpan data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed opening for write: " << path.string();
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    out.flush();
    ASSERT_TRUE(out.good()) << "Failed flushing: " << path.string();
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.good()) {
        return {};
    }
    const auto sz = static_cast<std::size_t>(in.tellg());
    std::vector<std::uint8_t> buf(sz, 0u);
    in.seekg(0);
    if (sz > 0u) {
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(sz));
    }
    return buf;
}

std::size_t CountTempFilesInSandbox(const fs::path& root) {
    std::size_t count = 0u;
    for (const auto& dir_entry : fs::recursive_directory_iterator(root)) {
        const std::string filename = dir_entry.path().filename().u8string();
        if (filename.size() >= 4u &&
            filename.substr(filename.size() - 4u) == ".tmp") {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST(MenuAutopatchComponentTest,
     AppliesCombinedArchiveAndStandaloneManifestWithFastPathAndIdempotentReapply) {
    ScopedSandboxDir sandbox;

    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra_title{};
    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra_untouched{};
    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra_dialog{};
    extra_title.fill(0x11u);
    extra_untouched.fill(0xA5u);
    extra_dialog.fill(0x22u);

    const std::vector<std::uint8_t> jp_title =
        MakePatternBlob(kOrigSliceLenA, 0x31u, 0x5Au);
    const std::vector<std::uint8_t> jp_untouched =
        MakePatternBlob(kOrigSliceLenC, 0x80u, 0xFFu);
    const std::vector<std::uint8_t> jp_dialog =
        MakePatternBlob(kOrigSliceLenB, 0x47u, 0xC3u);

    const std::vector<std::uint8_t> orig_sysgrp = BuildSyntheticArc20({
        {"SGTitle990000", extra_title, jp_title},
        {"SGIconUntouched", extra_untouched, jp_untouched},
        {"SGDialog990000", extra_dialog, jp_dialog},
    });

    const std::vector<std::uint8_t> jp_att01 =
        MakePatternBlob(kOrigSliceLenA, 0x62u, 0x99u);
    const std::vector<std::uint8_t> jp_att02 =
        MakePatternBlob(kOrigSliceLenB, 0x73u, 0xE1u);
    const std::vector<std::uint8_t> orig_data02501 = BuildSyntheticArc20({
        {"att01", extra_title, jp_att01},
        {"att02", extra_dialog, jp_att02},
    });

    const std::vector<std::uint8_t> en_title =
        MakePatternBlob(72u, 0x19u, 0xEEu);
    const std::vector<std::uint8_t> en_dialog =
        MakePatternBlob(148u, 0xD4u, 0x2Bu);
    const std::vector<std::uint8_t> en_att01 =
        MakePatternBlob(128u, 0x42u, 0x81u);
    const std::vector<std::uint8_t> en_att02 =
        MakePatternBlob(136u, 0x55u, 0xFAu);
    const std::vector<std::uint8_t> preexisting_custom_att02 =
        MakePatternBlob(88u, 0xCCu, 0x0Fu);

    std::uint64_t off_title = 0u;
    std::uint64_t sz_title = 0u;
    std::uint64_t off_dialog = 0u;
    std::uint64_t sz_dialog = 0u;
    std::uint64_t off_att01 = 0u;
    std::uint64_t sz_att01 = 0u;
    std::uint64_t off_att02 = 0u;
    std::uint64_t sz_att02 = 0u;

    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_sysgrp), "SGTitle990000",
                                  off_title, sz_title),
              ArcStatus::kOk);
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_sysgrp), "SGDialog990000",
                                  off_dialog, sz_dialog),
              ArcStatus::kOk);
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_data02501), "att01",
                                  off_att01, sz_att01),
              ArcStatus::kOk);
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_data02501), "att02",
                                  off_att02, sz_att02),
              ArcStatus::kOk);

    const std::vector<MenuPatchRecordInput> inputs = {
        {"sysgrp.arc", "sysgrp.arc", "SGTitle990000", off_title, jp_title,
         en_title, kFlagArchiveEntry},
        {"data02501.arc", "att01", "att01", off_att01, jp_att01, en_att01,
         kFlagBackupStandalone},
        {"sysgrp.arc", "sysgrp.arc", "SGDialog990000", off_dialog, jp_dialog,
         en_dialog, kFlagArchiveEntry},
        {"data02501.arc", "att02", "att02", off_att02, jp_att02, en_att02,
         kFlagBackupStandalone},
    };

    std::vector<std::uint8_t> patch_blob;
    ASSERT_EQ(SerializeMenuPatchManifest(inputs, patch_blob),
              ManifestStatus::kOk);

    // No menu_patch.bin yet -> kNoPatchFile.
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kNoPatchFile);

    WriteBinaryFile(sandbox.path() / "sysgrp.arc", ByteSpan(orig_sysgrp));
    WriteBinaryFile(sandbox.path() / "data02501.arc", ByteSpan(orig_data02501));
    WriteBinaryFile(sandbox.path() / "att02",
                    ByteSpan(preexisting_custom_att02));
    WriteBinaryFile(sandbox.path() / "menu_patch.bin", ByteSpan(patch_blob));

    // Launch 1: applies archive + standalone records.
    ASSERT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kApplied);
    EXPECT_EQ(CountTempFilesInSandbox(sandbox.path()), 0u);

    // Verify expected repacked sysgrp.arc byte-for-byte.
    const std::vector<std::uint8_t> expected_sysgrp = BuildSyntheticArc20({
        {"SGTitle990000", extra_title, en_title},
        {"SGIconUntouched", extra_untouched, jp_untouched},
        {"SGDialog990000", extra_dialog, en_dialog},
    });
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "sysgrp.arc"), expected_sysgrp);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "sysgrp.arc.bak"), orig_sysgrp);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "data02501.arc"), orig_data02501);
    EXPECT_FALSE(fs::exists(sandbox.path() / "data02501.arc.bak"));

    // Verify loose standalone outputs and backup hygiene.
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att01"), en_att01);
    EXPECT_FALSE(fs::exists(sandbox.path() / "att01.bak"));
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att02"), en_att02);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att02.bak"),
              preexisting_custom_att02);

    // Verify sentinel has exactly 3 unique target entries ("sysgrp.arc",
    // "att01", "att02") for 4 manifest records.
    const std::vector<std::uint8_t> sentinel_raw =
        ReadBinaryFile(sandbox.path() / "menu_patch.applied");
    AppliedSentinel parsed_sentinel{};
    ASSERT_EQ(ParseAppliedSentinel(ByteSpan(sentinel_raw), parsed_sentinel),
              SentinelStatus::kOk);
    ASSERT_EQ(parsed_sentinel.entries.size(), 3u);
    EXPECT_EQ(parsed_sentinel.entries[0].rel_path, "sysgrp.arc");
    EXPECT_EQ(parsed_sentinel.entries[0].expected_new_size,
              expected_sysgrp.size());
    EXPECT_EQ(parsed_sentinel.entries[1].rel_path, "att01");
    EXPECT_EQ(parsed_sentinel.entries[1].expected_new_size, en_att01.size());
    EXPECT_EQ(parsed_sentinel.entries[2].rel_path, "att02");
    EXPECT_EQ(parsed_sentinel.entries[2].expected_new_size, en_att02.size());

    // Launch 2: fast-path kAlreadyApplied without mutating files.
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kAlreadyApplied);

    // Delete sentinel and re-run: idempotent re-application from .bak without
    // polluting att01.bak or overwriting sysgrp.arc.bak / att02.bak.
    fs::remove(sandbox.path() / "menu_patch.applied");
    ASSERT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "sysgrp.arc"), expected_sysgrp);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "sysgrp.arc.bak"), orig_sysgrp);
    EXPECT_FALSE(fs::exists(sandbox.path() / "att01.bak"));
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att02.bak"),
              preexisting_custom_att02);
}

TEST(MenuAutopatchComponentTest,
     ResolvesStandaloneOrigSlicesAcrossOffsetShiftedArcAndDotBackupFallback) {
    ScopedSandboxDir sandbox;

    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra{};
    extra.fill(0x5Cu);
    const std::vector<std::uint8_t> jp_lead =
        MakePatternBlob(kOrigSliceLenC, 0x10u, 0x20u);
    const std::vector<std::uint8_t> jp_att01 =
        MakePatternBlob(kOrigSliceLenA, 0x33u, 0x77u);
    const std::vector<std::uint8_t> jp_att02 =
        MakePatternBlob(kOrigSliceLenB, 0x44u, 0x88u);

    const std::vector<std::uint8_t> retail_arc = BuildSyntheticArc20({
        {"cg_intro", extra, jp_lead},
        {"att01", extra, jp_att01},
        {"att02", extra, jp_att02},
    });

    std::uint64_t retail_off_att01 = 0u;
    std::uint64_t retail_sz_att01 = 0u;
    std::uint64_t retail_off_att02 = 0u;
    std::uint64_t retail_sz_att02 = 0u;
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(retail_arc), "att01",
                                  retail_off_att01, retail_sz_att01),
              ArcStatus::kOk);
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(retail_arc), "att02",
                                  retail_off_att02, retail_sz_att02),
              ArcStatus::kOk);

    const std::vector<std::uint8_t> en_att01 =
        MakePatternBlob(140u, 0x91u, 0x3Eu);
    const std::vector<std::uint8_t> en_att02 =
        MakePatternBlob(152u, 0xA2u, 0x4Fu);

    std::vector<std::uint8_t> patch_blob;
    ASSERT_EQ(
        SerializeMenuPatchManifest(
            {
                {"data02501.arc", "att01", "att01", retail_off_att01, jp_att01,
                 en_att01, kFlagBackupStandalone},
                {"data02501.arc", "att02", "att02", retail_off_att02, jp_att02,
                 en_att02, kFlagBackupStandalone},
            },
            patch_blob),
        ManifestStatus::kOk);

    // Case A: Live data02501.arc was repacked so cg_intro grew by 320 bytes,
    // shifting att01 and att02 away from retail_off_att01 / retail_off_att02.
    const std::vector<std::uint8_t> expanded_lead =
        MakePatternBlob(kOrigSliceLenC + kShiftedPadBytes, 0x99u, 0x11u);
    const std::vector<std::uint8_t> shifted_arc = BuildSyntheticArc20({
        {"cg_intro", extra, expanded_lead},
        {"att01", extra, jp_att01},
        {"att02", extra, jp_att02},
    });

    WriteBinaryFile(sandbox.path() / "data02501.arc", ByteSpan(shifted_arc));
    WriteBinaryFile(sandbox.path() / "menu_patch.bin", ByteSpan(patch_blob));

    ASSERT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att01"), en_att01);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att02"), en_att02);

    // Case B: Live data02501.arc has modified att01 bytes while
    // data02501.arc.backup holds the shifted unpatched archive.
    fs::remove(sandbox.path() / "menu_patch.applied");
    fs::remove(sandbox.path() / "att01");
    fs::remove(sandbox.path() / "att02");

    const std::vector<std::uint8_t> mutated_live_arc = BuildSyntheticArc20({
        {"cg_intro", extra, expanded_lead},
        {"att01", extra, MakePatternBlob(kOrigSliceLenA, 0xFFu, 0x01u)},
        {"att02", extra, MakePatternBlob(kOrigSliceLenB, 0xFEu, 0x02u)},
    });
    WriteBinaryFile(sandbox.path() / "data02501.arc",
                    ByteSpan(mutated_live_arc));
    WriteBinaryFile(sandbox.path() / "data02501.arc.backup",
                    ByteSpan(shifted_arc));

    ASSERT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att01"), en_att01);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att02"), en_att02);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "data02501.arc"),
              mutated_live_arc);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "data02501.arc.backup"),
              shifted_arc);
}

TEST(MenuAutopatchComponentTest,
     RecoversCleanlyWhenBothSentinelAndBackupsAreDeletedAfterPatch) {
    ScopedSandboxDir sandbox;

    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra{};
    extra.fill(0x3Au);
    const std::vector<std::uint8_t> jp_e1 =
        MakePatternBlob(kOrigSliceLenA, 0x21u, 0x43u);
    const std::vector<std::uint8_t> jp_e2 =
        MakePatternBlob(kOrigSliceLenB, 0x65u, 0x87u);
    const std::vector<std::uint8_t> orig_sysgrp = BuildSyntheticArc20({
        {"SGCnfgWnd010000", extra, jp_e1},
        {"SGCnfgWnd010100", extra, jp_e2},
    });
    const std::vector<std::uint8_t> jp_att01 =
        MakePatternBlob(kOrigSliceLenA, 0x51u, 0x62u);
    const std::vector<std::uint8_t> orig_data = BuildSyntheticArc20({
        {"att01", extra, jp_att01},
    });

    std::uint64_t off_e1 = 0u;
    std::uint64_t sz_e1 = 0u;
    std::uint64_t off_e2 = 0u;
    std::uint64_t sz_e2 = 0u;
    std::uint64_t off_att = 0u;
    std::uint64_t sz_att = 0u;
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_sysgrp), "SGCnfgWnd010000",
                                  off_e1, sz_e1),
              ArcStatus::kOk);
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_sysgrp), "SGCnfgWnd010100",
                                  off_e2, sz_e2),
              ArcStatus::kOk);
    ASSERT_EQ(
        FindArc20EntrySlice(ByteSpan(orig_data), "att01", off_att, sz_att),
        ArcStatus::kOk);

    const std::vector<std::uint8_t> en_e1 = MakePatternBlob(84u, 0xB1u, 0x12u);
    const std::vector<std::uint8_t> en_e2 = MakePatternBlob(92u, 0xC2u, 0x23u);
    const std::vector<std::uint8_t> en_att01 =
        MakePatternBlob(120u, 0xD3u, 0x34u);

    std::vector<std::uint8_t> patch_blob;
    ASSERT_EQ(
        SerializeMenuPatchManifest(
            {
                {"sysgrp.arc", "sysgrp.arc", "SGCnfgWnd010000", off_e1, jp_e1,
                 en_e1, kFlagArchiveEntry},
                {"sysgrp.arc", "sysgrp.arc", "SGCnfgWnd010100", off_e2, jp_e2,
                 en_e2, kFlagArchiveEntry},
                {"data02501.arc", "att01", "att01", off_att, jp_att01, en_att01,
                 kFlagBackupStandalone},
            },
            patch_blob),
        ManifestStatus::kOk);

    WriteBinaryFile(sandbox.path() / "sysgrp.arc", ByteSpan(orig_sysgrp));
    WriteBinaryFile(sandbox.path() / "data02501.arc", ByteSpan(orig_data));
    WriteBinaryFile(sandbox.path() / "menu_patch.bin", ByteSpan(patch_blob));

    ASSERT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kApplied);
    const std::vector<std::uint8_t> patched_sysgrp =
        ReadBinaryFile(sandbox.path() / "sysgrp.arc");

    // Delete sentinel, sysgrp.arc.bak, AND source data02501.arc while keeping
    // the already-patched live sysgrp.arc and att01 on disk.
    fs::remove(sandbox.path() / "menu_patch.applied");
    fs::remove(sandbox.path() / "sysgrp.arc.bak");
    fs::remove(sandbox.path() / "data02501.arc");

    ASSERT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "sysgrp.arc"), patched_sysgrp);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "att01"), en_att01);
    EXPECT_FALSE(fs::exists(sandbox.path() / "sysgrp.arc.bak"));
    EXPECT_TRUE(fs::exists(sandbox.path() / "menu_patch.applied"));

    // Next call hits fast-path immediately.
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kAlreadyApplied);
}

TEST(MenuAutopatchComponentTest,
     RollsBackAtomicallyWithZeroTempLeftoversWhenMidBatchRecordFailsHashCheck) {
    ScopedSandboxDir sandbox;

    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra{};
    extra.fill(0x7Eu);
    const std::vector<std::uint8_t> jp_e1 =
        MakePatternBlob(kOrigSliceLenA, 0x15u, 0x26u);
    const std::vector<std::uint8_t> jp_e2 =
        MakePatternBlob(kOrigSliceLenB, 0x37u, 0x48u);
    const std::vector<std::uint8_t> orig_sysgrp = BuildSyntheticArc20({
        {"SGMsgWnd800000", extra, jp_e1},
        {"SGMsgWnd800001", extra, jp_e2},
    });

    const std::vector<std::uint8_t> jp_att01 =
        MakePatternBlob(kOrigSliceLenA, 0x59u, 0x6Au);
    const std::vector<std::uint8_t> jp_att02 =
        MakePatternBlob(kOrigSliceLenB, 0x7Bu, 0x8Cu);
    const std::vector<std::uint8_t> orig_data = BuildSyntheticArc20({
        {"att01", extra, jp_att01},
        {"att02", extra, jp_att02},
    });

    std::uint64_t off_e1 = 0u;
    std::uint64_t sz_e1 = 0u;
    std::uint64_t off_att01 = 0u;
    std::uint64_t sz_att01 = 0u;
    std::uint64_t off_att02 = 0u;
    std::uint64_t sz_att02 = 0u;
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_sysgrp), "SGMsgWnd800000",
                                  off_e1, sz_e1),
              ArcStatus::kOk);
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_data), "att01", off_att01,
                                  sz_att01),
              ArcStatus::kOk);
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(orig_data), "att02", off_att02,
                                  sz_att02),
              ArcStatus::kOk);

    const std::vector<std::uint8_t> en_e1 = MakePatternBlob(76u, 0x9Du, 0xAEu);
    const std::vector<std::uint8_t> en_att01 =
        MakePatternBlob(104u, 0xBFu, 0xC0u);
    const std::vector<std::uint8_t> en_att02 =
        MakePatternBlob(108u, 0xD1u, 0xE2u);

    std::vector<std::uint8_t> patch_blob;
    ASSERT_EQ(
        SerializeMenuPatchManifest(
            {
                {"sysgrp.arc", "sysgrp.arc", "SGMsgWnd800000", off_e1, jp_e1,
                 en_e1, kFlagArchiveEntry},
                {"data02501.arc", "att01", "att01", off_att01, jp_att01,
                 en_att01, kFlagBackupStandalone},
                {"data02501.arc", "att02", "att02", off_att02, jp_att02,
                 en_att02, kFlagBackupStandalone},
            },
            patch_blob),
        ManifestStatus::kOk);

    // Corrupt the final byte of Record #3's xor_payload and recompute the outer
    // header manifest_hash so ParseMenuPatchManifest succeeds, forcing Phase 1
    // to stage sysgrp.arc + att01 before failing UnmaskRecordPayload on att02.
    patch_blob.back() ^= 0xFFu;
    const std::uint32_t updated_manifest_hash = menu_patch_format::MurmurHash2(
        ByteSpan(patch_blob).subspan(menu_patch_format::kPatchHeaderBytes));
    for (unsigned int i = 0u; i < kU32Bytes; ++i) {
        patch_blob[0x10u + i] = static_cast<std::uint8_t>(
            (updated_manifest_hash >> (i * kBitsPerByte)) & kByteMask);
    }

    WriteBinaryFile(sandbox.path() / "sysgrp.arc", ByteSpan(orig_sysgrp));
    WriteBinaryFile(sandbox.path() / "data02501.arc", ByteSpan(orig_data));
    WriteBinaryFile(sandbox.path() / "menu_patch.bin", ByteSpan(patch_blob));

    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.wpath()),
              menu_autopatch::MenuPatchOutcome::kFailed);

    // Every live file is untouched and zero .bak / .applied / .tmp files exist.
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "sysgrp.arc"), orig_sysgrp);
    EXPECT_FALSE(fs::exists(sandbox.path() / "sysgrp.arc.bak"));
    EXPECT_FALSE(fs::exists(sandbox.path() / "att01"));
    EXPECT_FALSE(fs::exists(sandbox.path() / "att01.bak"));
    EXPECT_FALSE(fs::exists(sandbox.path() / "att02"));
    EXPECT_FALSE(fs::exists(sandbox.path() / "menu_patch.applied"));
    EXPECT_EQ(CountTempFilesInSandbox(sandbox.path()), 0u);
}
