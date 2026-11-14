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

}  // namespace
