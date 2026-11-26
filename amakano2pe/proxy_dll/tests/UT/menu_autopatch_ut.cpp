// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — first-launch auto-patcher against synthetic game directories: loose
// override extraction, standalone .bak backup and idempotent re-apply, the
// applied-sentinel fast path, digest and path-traversal rejection, bundle
// updates, and cross-thread serialization.

#include "menu_autopatch.h"

#include "menu_patch_test_encoder.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

using menu_autopatch::EnsureMenuAssetsPatched;
using menu_autopatch::PatchOutcome;
using menu_patch_test::Bytes;
using menu_patch_test::EncodeContainer;
using menu_patch_test::PseudoRandomBytes;
using menu_patch_test::SpecEntry;

constexpr std::size_t kArchiveSize = 8192;
constexpr std::uint64_t kSliceOffset = 1000;
constexpr std::size_t kSliceSize = 512;
constexpr std::size_t kNewRawSize = 3000;
constexpr std::size_t kStandaloneSize = 300;
constexpr std::size_t kStandaloneNewSize = 450;
constexpr std::size_t kManyTargetCount = 200;
constexpr std::size_t kManySliceSize = 64;
constexpr std::size_t kManyNewRawSize = 256;
constexpr std::size_t kConcurrentCallers = 4;
constexpr std::uint32_t kArchiveSeed = 11;
constexpr std::uint32_t kNewRawSeed = 22;
constexpr std::uint32_t kStandaloneSeed = 33;
constexpr std::uint32_t kStandaloneNewSeed = 44;
constexpr std::uint32_t kUpdatedSeed = 55;
constexpr std::uint32_t kManySeedBase = 1000;
constexpr std::uint8_t kFlipMask = 0x01;
constexpr unsigned kCounterIncrement = 1;
constexpr std::uint64_t kSingleBytePastEnd = 1;
constexpr std::size_t kExpectedSingleAppliedRun = 1;
constexpr auto kFastPathBudget = std::chrono::milliseconds(10);

constexpr char kArchiveName[] = "image01.int";
constexpr char kLooseTarget[] = "image\\sys_title.hg3";
constexpr char kStandaloneName[] = "boot.hg3";

class TempGameDir {
public:
    TempGameDir() {
        static std::atomic<unsigned> counter{0};
        const fs::path base = fs::temp_directory_path();
        const std::wstring name =
            L"amk2_autopatch_ut_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(counter.fetch_add(kCounterIncrement));
        path_ = base / name;
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempGameDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempGameDir(const TempGameDir&) = delete;
    TempGameDir& operator=(const TempGameDir&) = delete;

    const fs::path& Path() const { return path_; }
    std::wstring Wide() const { return path_.wstring(); }
    fs::path File(const std::string& rel_utf8) const {
        return path_ / fs::u8path(rel_utf8);
    }

private:
    fs::path path_;
};

void WriteBytes(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

Bytes ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
}

Bytes Slice(const Bytes& bytes, std::uint64_t offset, std::size_t size) {
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    return Bytes(begin, begin + static_cast<std::ptrdiff_t>(size));
}

bool HasLeftoverTempFiles(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().extension() == L".tmp") {
            return true;
        }
    }
    return false;
}

struct LooseFixture {
    Bytes archive;
    SpecEntry entry;
};

LooseFixture MakeLooseFixture(const TempGameDir& dir,
                              std::uint32_t new_raw_seed) {
    LooseFixture f;
    f.archive = PseudoRandomBytes(kArchiveSize, kArchiveSeed);
    WriteBytes(dir.File(kArchiveName), f.archive);
    f.entry.source = kArchiveName;
    f.entry.target = kLooseTarget;
    f.entry.orig_offset = kSliceOffset;
    f.entry.orig_slice = Slice(f.archive, kSliceOffset, kSliceSize);
    f.entry.new_raw = PseudoRandomBytes(kNewRawSize, new_raw_seed);
    return f;
}

SpecEntry MakeStandaloneEntry(const Bytes& original) {
    SpecEntry e;
    e.source = kStandaloneName;
    e.target = kStandaloneName;
    e.flags = menu_patch_test::kSpecFlagBackupStandalone;
    e.orig_offset = 0;
    e.orig_slice = original;
    e.new_raw = PseudoRandomBytes(kStandaloneNewSize, kStandaloneNewSeed);
    return e;
}

void WriteBundle(const TempGameDir& dir,
                 const std::vector<SpecEntry>& entries) {
    WriteBytes(dir.File("menu_patch.bin"), EncodeContainer(entries));
}

TEST(MenuAutoPatch,
     ExtractsLooseOverridesAndPreservesSentinelFastPathWithinBudget) {
    EXPECT_EQ(EnsureMenuAssetsPatched(std::wstring()), PatchOutcome::kFailed);

    const TempGameDir empty_dir;
    EXPECT_EQ(EnsureMenuAssetsPatched(empty_dir.Wide()),
              PatchOutcome::kNoPatchBundle);
    EXPECT_FALSE(fs::exists(empty_dir.File("menu_patch.applied")));

    const TempGameDir dir;
    const Bytes archive = PseudoRandomBytes(kArchiveSize, kArchiveSeed);
    WriteBytes(dir.File(kArchiveName), archive);
    std::vector<SpecEntry> entries;
    for (std::size_t i = 0; i < kManyTargetCount; ++i) {
        SpecEntry e;
        e.source = kArchiveName;
        e.target = "image\\item_" + std::to_string(i) + ".hg3";
        e.orig_offset = i;
        e.orig_slice = Slice(archive, i, kManySliceSize);
        e.new_raw = PseudoRandomBytes(
            kManyNewRawSize, kManySeedBase + static_cast<std::uint32_t>(i));
        entries.push_back(std::move(e));
    }
    WriteBundle(dir, entries);

    ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    EXPECT_EQ(ReadBytes(dir.File("image\\item_0.hg3")), entries[0].new_raw);
    EXPECT_EQ(ReadBytes(dir.File(kArchiveName)), archive);
    EXPECT_FALSE(fs::exists(dir.File(std::string(kArchiveName) + ".bak")));
    EXPECT_TRUE(fs::exists(dir.File("menu_patch.applied")));
    EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));

    // Fast path over 200 targets must complete under budget without touching
    // the source archive (even if the source archive is removed).
    fs::remove(dir.File(kArchiveName));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()),
              PatchOutcome::kAlreadyApplied);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, kFastPathBudget);

    // Restoring the archive and deleting one override triggers a rebuild.
    WriteBytes(dir.File(kArchiveName), archive);
    fs::remove(dir.File("image\\item_0.hg3"));
    EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    EXPECT_EQ(ReadBytes(dir.File("image\\item_0.hg3")), entries[0].new_raw);
}

TEST(MenuAutoPatch,
     StandaloneBackupAndReapplyAreIdempotentAndRejectTamperedOriginals) {
    const TempGameDir dir;
    const Bytes original = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed);
    WriteBytes(dir.File(kStandaloneName), original);
    const SpecEntry entry = MakeStandaloneEntry(original);
    WriteBundle(dir, {entry});

    ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    const fs::path backup = dir.File(std::string(kStandaloneName) + ".bak");
    EXPECT_EQ(ReadBytes(backup), original);
    EXPECT_EQ(ReadBytes(dir.File(kStandaloneName)), entry.new_raw);

    // Deleting the sentinel forces a slow-path re-apply that reads from .bak
    // rather than the already-patched target and preserves .bak untouched.
    fs::remove(dir.File("menu_patch.applied"));
    EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    EXPECT_EQ(ReadBytes(backup), original);
    EXPECT_EQ(ReadBytes(dir.File(kStandaloneName)), entry.new_raw);
    EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));

    // Refuses to back up a non-original standalone file when no .bak exists.
    const TempGameDir tampered_dir;
    Bytes modified = original;
    modified[0] ^= kFlipMask;
    WriteBytes(tampered_dir.File(kStandaloneName), modified);
    WriteBundle(tampered_dir, {entry});
    EXPECT_EQ(EnsureMenuAssetsPatched(tampered_dir.Wide()),
              PatchOutcome::kPartiallyApplied);
    EXPECT_FALSE(
        fs::exists(tampered_dir.File(std::string(kStandaloneName) + ".bak")));
    EXPECT_EQ(ReadBytes(tampered_dir.File(kStandaloneName)), modified);
}

TEST(MenuAutoPatch,
     RejectsSliceDigestMismatchesOutOfRangeOffsetsAndPathTraversal) {
    {
        const TempGameDir dir;
        LooseFixture f = MakeLooseFixture(dir, kNewRawSeed);
        f.archive[kSliceOffset] ^= kFlipMask;
        WriteBytes(dir.File(kArchiveName), f.archive);
        WriteBundle(dir, {f.entry});
        EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()),
                  PatchOutcome::kPartiallyApplied);
        EXPECT_FALSE(fs::exists(dir.File(kLooseTarget)));
    }
    {
        const TempGameDir dir;
        LooseFixture f = MakeLooseFixture(dir, kNewRawSeed);
        f.entry.orig_offset = kArchiveSize - kSliceSize + kSingleBytePastEnd;
        WriteBundle(dir, {f.entry});
        EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()),
                  PatchOutcome::kPartiallyApplied);
        EXPECT_FALSE(fs::exists(dir.File(kLooseTarget)));
    }
    {
        const TempGameDir dir;
        LooseFixture f = MakeLooseFixture(dir, kNewRawSeed);
        f.entry.target = "..\\amk2_escape.hg3";
        WriteBundle(dir, {f.entry});
        EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kFailed);
        EXPECT_FALSE(
            fs::exists(dir.Path().parent_path() / L"amk2_escape.hg3"));
    }
}

TEST(MenuAutoPatch, SerializesConcurrentCallersAndAppliesUpdatedBundles) {
    const TempGameDir dir;
    const LooseFixture first = MakeLooseFixture(dir, kNewRawSeed);
    WriteBundle(dir, {first.entry});

    std::vector<PatchOutcome> outcomes(kConcurrentCallers,
                                       PatchOutcome::kFailed);
    std::vector<std::thread> callers;
    for (std::size_t i = 0; i < kConcurrentCallers; ++i) {
        callers.emplace_back([&dir, &outcomes, i] {
            outcomes[i] = EnsureMenuAssetsPatched(dir.Wide());
        });
    }
    for (std::thread& t : callers) {
        t.join();
    }

    std::size_t applied = 0;
    for (const PatchOutcome outcome : outcomes) {
        EXPECT_TRUE(outcome == PatchOutcome::kApplied ||
                    outcome == PatchOutcome::kAlreadyApplied);
        if (outcome == PatchOutcome::kApplied) {
            applied += kExpectedSingleAppliedRun;
        }
    }
    EXPECT_EQ(applied, kExpectedSingleAppliedRun);
    EXPECT_EQ(ReadBytes(dir.File(kLooseTarget)), first.entry.new_raw);
    EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));

    // Updating the bundle invalidates the sentinel and writes the new override.
    const LooseFixture second = MakeLooseFixture(dir, kUpdatedSeed);
    WriteBundle(dir, {second.entry});
    EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    EXPECT_EQ(ReadBytes(dir.File(kLooseTarget)), second.entry.new_raw);
}

TEST(MenuAutoPatch,
     ShrinkingBundleRestoresDroppedStandalonesDeletesDroppedLooseAndHandlesCaseInsensitiveRetain) {
    const TempGameDir dir;
    const Bytes archive = PseudoRandomBytes(kArchiveSize, kArchiveSeed);
    WriteBytes(dir.File(kArchiveName), archive);

    const Bytes orig_boot = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed);
    const Bytes orig_cfg = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed + 1U);
    WriteBytes(dir.File(kStandaloneName), orig_boot);
    WriteBytes(dir.File("config\\startup.xml"), orig_cfg);

    SpecEntry loose_a;
    loose_a.source = kArchiveName;
    loose_a.target = "image\\keep.hg3";
    loose_a.orig_offset = 0;
    loose_a.orig_slice = Slice(archive, 0, kSliceSize);
    loose_a.new_raw = PseudoRandomBytes(kNewRawSize, kNewRawSeed);

    SpecEntry loose_b;
    loose_b.source = kArchiveName;
    loose_b.target = "image\\drop.hg3";
    loose_b.orig_offset = kSliceOffset;
    loose_b.orig_slice = Slice(archive, kSliceOffset, kSliceSize);
    loose_b.new_raw = PseudoRandomBytes(kNewRawSize, kNewRawSeed + 1U);

    SpecEntry standalone_a = MakeStandaloneEntry(orig_boot);

    SpecEntry standalone_b;
    standalone_b.source = "config\\startup.xml";
    standalone_b.target = "config\\startup.xml";
    standalone_b.flags = menu_patch_test::kSpecFlagBackupStandalone;
    standalone_b.orig_offset = 0;
    standalone_b.orig_slice = orig_cfg;
    standalone_b.new_raw = PseudoRandomBytes(kStandaloneNewSize, kStandaloneNewSeed + 1U);

    WriteBundle(dir, {loose_a, loose_b, standalone_a, standalone_b});
    ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    ASSERT_TRUE(fs::exists(dir.File("image\\drop.hg3")));
    ASSERT_EQ(ReadBytes(dir.File("config\\startup.xml")), standalone_b.new_raw);
    ASSERT_EQ(ReadBytes(dir.File("config\\startup.xml.bak")), orig_cfg);

    // Mark dropped targets read-only to confirm restore/delete clears attributes.
    ASSERT_NE(SetFileAttributesW(dir.File("image\\drop.hg3").c_str(),
                                 FILE_ATTRIBUTE_READONLY),
              0);
    ASSERT_NE(SetFileAttributesW(dir.File("config\\startup.xml").c_str(),
                                 FILE_ATTRIBUTE_READONLY),
              0);

    // Pass 2 shrinks to 2 targets with different ASCII casing on retained paths.
    SpecEntry loose_a_v2 = loose_a;
    loose_a_v2.target = "IMAGE\\KEEP.HG3";
    loose_a_v2.new_raw = PseudoRandomBytes(kNewRawSize, kUpdatedSeed);

    SpecEntry standalone_a_v2 = standalone_a;
    standalone_a_v2.source = "BOOT.HG3";
    standalone_a_v2.target = "BOOT.HG3";
    standalone_a_v2.new_raw = PseudoRandomBytes(kStandaloneNewSize, kUpdatedSeed + 1U);

    WriteBundle(dir, {loose_a_v2, standalone_a_v2});
    ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);

    EXPECT_EQ(ReadBytes(dir.File("image\\keep.hg3")), loose_a_v2.new_raw);
    EXPECT_EQ(ReadBytes(dir.File(kStandaloneName)), standalone_a_v2.new_raw);
    EXPECT_EQ(ReadBytes(dir.File(std::string(kStandaloneName) + ".bak")), orig_boot);
    EXPECT_FALSE(fs::exists(dir.File("image\\drop.hg3")));
    EXPECT_EQ(ReadBytes(dir.File("config\\startup.xml")), orig_cfg);
    EXPECT_EQ(ReadBytes(dir.File("config\\startup.xml.bak")), orig_cfg);
    EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));
    EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kAlreadyApplied);

    // Pass 3 shrinks to an empty bundle ({}): removes remaining loose override
    // and restores remaining standalone from .bak.
    WriteBundle(dir, {});
    ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    EXPECT_FALSE(fs::exists(dir.File("image\\keep.hg3")));
    EXPECT_EQ(ReadBytes(dir.File(kStandaloneName)), orig_boot);
    EXPECT_EQ(ReadBytes(dir.File(std::string(kStandaloneName) + ".bak")), orig_boot);
    EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));
    EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kAlreadyApplied);
}

TEST(MenuAutoPatch,
     DroppingStandaloneWithMissingZeroByteOrTruncatedBakLeavesLiveFileUntouched) {
    const TempGameDir dir;
    const Bytes orig_boot = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed);
    const Bytes orig_startup = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed + 1U);
    const Bytes orig_sound = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed + 2U);
    WriteBytes(dir.File(kStandaloneName), orig_boot);
    WriteBytes(dir.File("config\\startup.xml"), orig_startup);
    WriteBytes(dir.File("config\\soundconf.xml"), orig_sound);

    SpecEntry e_boot = MakeStandaloneEntry(orig_boot);
    SpecEntry e_startup;
    e_startup.source = "config\\startup.xml";
    e_startup.target = "config\\startup.xml";
    e_startup.flags = menu_patch_test::kSpecFlagBackupStandalone;
    e_startup.orig_offset = 0;
    e_startup.orig_slice = orig_startup;
    e_startup.new_raw = PseudoRandomBytes(kStandaloneNewSize, kStandaloneNewSeed + 1U);

    SpecEntry e_sound;
    e_sound.source = "config\\soundconf.xml";
    e_sound.target = "config\\soundconf.xml";
    e_sound.flags = menu_patch_test::kSpecFlagBackupStandalone;
    e_sound.orig_offset = 0;
    e_sound.orig_slice = orig_sound;
    e_sound.new_raw = PseudoRandomBytes(kStandaloneNewSize, kStandaloneNewSeed + 2U);

    WriteBundle(dir, {e_boot, e_startup, e_sound});
    ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);

    // 1. Delete boot.hg3.bak (missing .bak on protected retail standalone).
    ASSERT_TRUE(fs::remove(dir.File("boot.hg3.bak")));
    // 2. Replace config\startup.xml.bak with a 0-byte file.
    WriteBytes(dir.File("config\\startup.xml.bak"), Bytes{});
    // 3. Replace config\soundconf.xml.bak with a truncated 63-byte (< 64) file.
    WriteBytes(dir.File("config\\soundconf.xml.bak"),
               PseudoRandomBytes(menu_patch::kMinOrigSliceSize - 1U, kUpdatedSeed));

    // Shrinking to {} drops all 3 standalones: none may be deleted or
    // overwritten with empty/truncated bytes.
    WriteBundle(dir, {});
    ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
    EXPECT_EQ(ReadBytes(dir.File(kStandaloneName)), e_boot.new_raw);
    EXPECT_EQ(ReadBytes(dir.File("config\\startup.xml")), e_startup.new_raw);
    EXPECT_EQ(ReadBytes(dir.File("config\\soundconf.xml")), e_sound.new_raw);
    EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));
    EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kAlreadyApplied);
}

TEST(MenuAutoPatch,
     TwoPhaseStagingRollsBackAllLiveTargetsBackupsAndDroppedCleanupOnMidBatchFailure) {
    // Subcase A: Fresh install where Record 0 (standalone) and Record 1 (loose)
    // succeed in Phase 1 while Record 2 fails orig_slice_hash. Zero live files
    // may be mutated, zero .bak or .tmp files may remain.
    {
        const TempGameDir dir;
        const Bytes archive = PseudoRandomBytes(kArchiveSize, kArchiveSeed);
        WriteBytes(dir.File(kArchiveName), archive);
        const Bytes orig_boot = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed);
        WriteBytes(dir.File(kStandaloneName), orig_boot);

        SpecEntry valid_standalone = MakeStandaloneEntry(orig_boot);
        SpecEntry valid_loose;
        valid_loose.source = kArchiveName;
        valid_loose.target = "image\\valid_first.hg3";
        valid_loose.orig_offset = 0;
        valid_loose.orig_slice = Slice(archive, 0, kSliceSize);
        valid_loose.new_raw = PseudoRandomBytes(kNewRawSize, kNewRawSeed);

        SpecEntry bad_loose;
        bad_loose.source = kArchiveName;
        bad_loose.target = "image\\bad_last.hg3";
        bad_loose.orig_offset = kSliceOffset;
        bad_loose.orig_slice = Slice(archive, kSliceOffset, kSliceSize);
        bad_loose.orig_slice[0] ^= kFlipMask;
        bad_loose.new_raw = PseudoRandomBytes(kNewRawSize, kUpdatedSeed);

        WriteBundle(dir, {valid_standalone, valid_loose, bad_loose});
        EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()),
                  PatchOutcome::kPartiallyApplied);

        EXPECT_EQ(ReadBytes(dir.File(kStandaloneName)), orig_boot);
        EXPECT_FALSE(fs::exists(dir.File(std::string(kStandaloneName) + ".bak")));
        EXPECT_FALSE(fs::exists(dir.File("image\\valid_first.hg3")));
        EXPECT_FALSE(fs::exists(dir.File("image\\bad_last.hg3")));
        EXPECT_FALSE(fs::exists(dir.File("menu_patch.applied")));
        EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));
    }

    // Subcase B: Shrinking v1 -> broken v2 must not delete dropped loose
    // overrides or revert dropped standalones when v2 fails Phase 1 staging.
    {
        const TempGameDir dir;
        const Bytes archive = PseudoRandomBytes(kArchiveSize, kArchiveSeed);
        WriteBytes(dir.File(kArchiveName), archive);
        const Bytes orig_boot = PseudoRandomBytes(kStandaloneSize, kStandaloneSeed);
        WriteBytes(dir.File(kStandaloneName), orig_boot);

        SpecEntry loose_keep;
        loose_keep.source = kArchiveName;
        loose_keep.target = "image\\keep.hg3";
        loose_keep.orig_offset = 0;
        loose_keep.orig_slice = Slice(archive, 0, kSliceSize);
        loose_keep.new_raw = PseudoRandomBytes(kNewRawSize, kNewRawSeed);

        SpecEntry loose_drop;
        loose_drop.source = kArchiveName;
        loose_drop.target = "image\\drop.hg3";
        loose_drop.orig_offset = kSliceOffset;
        loose_drop.orig_slice = Slice(archive, kSliceOffset, kSliceSize);
        loose_drop.new_raw = PseudoRandomBytes(kNewRawSize, kNewRawSeed + 1U);

        SpecEntry standalone_drop = MakeStandaloneEntry(orig_boot);

        WriteBundle(dir, {loose_keep, loose_drop, standalone_drop});
        ASSERT_EQ(EnsureMenuAssetsPatched(dir.Wide()), PatchOutcome::kApplied);
        const Bytes v1_sentinel = ReadBytes(dir.File("menu_patch.applied"));

        SpecEntry broken_keep = loose_keep;
        broken_keep.orig_slice[0] ^= kFlipMask;
        broken_keep.new_raw = PseudoRandomBytes(kNewRawSize, kUpdatedSeed);
        WriteBundle(dir, {broken_keep});

        EXPECT_EQ(EnsureMenuAssetsPatched(dir.Wide()),
                  PatchOutcome::kPartiallyApplied);
        EXPECT_EQ(ReadBytes(dir.File("image\\keep.hg3")), loose_keep.new_raw);
        EXPECT_EQ(ReadBytes(dir.File("image\\drop.hg3")), loose_drop.new_raw);
        EXPECT_EQ(ReadBytes(dir.File(kStandaloneName)), standalone_drop.new_raw);
        EXPECT_EQ(ReadBytes(dir.File("menu_patch.applied")), v1_sentinel);
        EXPECT_FALSE(HasLeftoverTempFiles(dir.Path()));
    }
}

}  // namespace
