// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box component tests for Adieu's first-launch menu auto-patcher
// (`EnsureMenuAssetsPatched`) across multi-record stateful lifecycles:
//   1. First launch (`kApplied`): atomic `.bak` creation, XOR unmasking across
//      `Gameexe.dat` and `g00\*.g00`, `"ADIEUAPL"` v1 sentinel creation, and
//      zero leftover `.tmp` files.
//   2. Subsequent launch (`kAlreadyApplied`): `< 5 ms` sentinel fast path that
//      avoids reading `menu_patch.bin` record payloads, plus automatic healing
//      when a live target's size drifts.
//   3. Deleted-sentinel and missing-backup recovery without corrupting
//      immutable factory `.bak` files.
//   4. `v1 -> v2` `menu_patch.bin` upgrades (both size-changing and same-size
//      bundles) reading pristine slices from `<target>.bak`.
//   5. Mid-batch record validation failure rolling back all Phase-1 `.stage.tmp`
//      and `.bak.tmp` files so pre-existing live targets remain byte-identical.

#include "menu_autopatch.h"
#include "menu_patch_format.h"

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using menu_autopatch::EnsureMenuAssetsPatched;
using menu_autopatch::kMenuPatchFileName;
using menu_autopatch::kMenuPatchSentinelFileName;
using menu_autopatch::MenuPatchOutcome;
using menu_patch_format::AppliedSentinel;
using menu_patch_format::ByteSpan;
using menu_patch_format::kFlagBackupStandalone;
using menu_patch_format::kPatchHeaderBytes;
using menu_patch_format::ManifestStatus;
using menu_patch_format::MenuPatchManifest;
using menu_patch_format::MenuPatchRecordInput;
using menu_patch_format::MurmurHash2;
using menu_patch_format::ParseAppliedSentinel;
using menu_patch_format::ParseMenuPatchManifest;
using menu_patch_format::SentinelStatus;
using menu_patch_format::SerializeMenuPatchManifest;

constexpr std::size_t kFactoryGameexeBytes = 512u;
constexpr std::size_t kFactoryMe02Bytes = 1024u;
constexpr std::size_t kFactoryMess99Bytes = 768u;

constexpr std::size_t kPatchedV1GameexeBytes = 548u;
constexpr std::size_t kPatchedV1Me02Bytes = 1408u;
constexpr std::size_t kPatchedV1Mess99Bytes = 612u;

constexpr std::size_t kPatchedV2GameexeBytes = 560u;
constexpr std::size_t kPatchedV2Me02Bytes = 1520u;
constexpr std::size_t kPatchedV2Mess99Bytes = 640u;

constexpr double kFastPathMaxElapsedMs = 5.0;
constexpr int kFastPathSampleCount = 3;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        const std::string folder =
            "adi_autopatch_ct_さよなら_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(folder);
        ClearReadOnlyRecursively(dir_);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        ClearReadOnlyRecursively(dir_);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    static void ClearReadOnlyRecursively(const fs::path& root) noexcept {
        std::error_code ec;
        if (!fs::exists(root, ec)) {
            return;
        }
        for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
            const DWORD attrs = ::GetFileAttributesW(entry.path().c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
                ::SetFileAttributesW(entry.path().c_str(),
                                     attrs & ~FILE_ATTRIBUTE_READONLY);
            }
        }
    }

    fs::path dir_;
};

std::vector<std::uint8_t> MakeBinaryAsset(std::size_t size,
                                          std::uint8_t leading_tag,
                                          std::uint8_t stride_seed) {
    std::vector<std::uint8_t> data(size, 0u);
    if (size >= 4u) {
        // Exercise binary seam bytes (0x00, 0x7F, 0x80, 0xFF) right up front.
        data[0] = leading_tag;
        data[1] = 0x00u;
        data[2] = 0x7Fu;
        data[3] = 0x80u;
    }
    if (size >= 5u) {
        data[4] = 0xFFu;
    }
    for (std::size_t i = 5u; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>(
            ((i * 43u) ^ (static_cast<std::size_t>(stride_seed) * 17u)) &
            0xFFu);
    }
    return data;
}

void WriteFileBytes(const fs::path& path,
                    const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
        ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out)) << "Failed opening " << path.u8string();
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(static_cast<bool>(out)) << "Failed writing " << path.u8string();
}

std::vector<std::uint8_t> ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto sz = in.tellg();
    if (sz <= 0) {
        return {};
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), sz);
    return out;
}

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    const std::size_t start = text.size() - suffix.size();
    for (std::size_t i = 0u; i < suffix.size(); ++i) {
        char a = text[start + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> CollectRelativeFilesWithSuffix(
    const fs::path& root,
    std::string_view suffix) {
    std::vector<std::string> matches;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string rel =
            fs::relative(entry.path(), root, ec).generic_u8string();
        if (EndsWithIgnoreCase(rel, suffix)) {
            matches.push_back(rel);
        }
    }
    return matches;
}

struct SimulatedGameAssets {
    std::vector<std::uint8_t> factory_gameexe =
        MakeBinaryAsset(kFactoryGameexeBytes, 0x00u, 0x11u);
    std::vector<std::uint8_t> factory_me02 =
        MakeBinaryAsset(kFactoryMe02Bytes, 0x02u, 0x22u);
    std::vector<std::uint8_t> factory_mess99 =
        MakeBinaryAsset(kFactoryMess99Bytes, 0x02u, 0x33u);

    std::vector<std::uint8_t> en_v1_gameexe =
        MakeBinaryAsset(kPatchedV1GameexeBytes, 0x00u, 0xA1u);
    std::vector<std::uint8_t> en_v1_me02 =
        MakeBinaryAsset(kPatchedV1Me02Bytes, 0x02u, 0xB2u);
    std::vector<std::uint8_t> en_v1_mess99 =
        MakeBinaryAsset(kPatchedV1Mess99Bytes, 0x02u, 0xC3u);

    std::vector<std::uint8_t> en_v2_gameexe =
        MakeBinaryAsset(kPatchedV2GameexeBytes, 0x00u, 0xD4u);
    std::vector<std::uint8_t> en_v2_me02 =
        MakeBinaryAsset(kPatchedV2Me02Bytes, 0x02u, 0xE5u);
    std::vector<std::uint8_t> en_v2_mess99 =
        MakeBinaryAsset(kPatchedV2Mess99Bytes, 0x02u, 0xF6u);

    void SeedFactoryTree(const fs::path& root) const {
        WriteFileBytes(root / "Gameexe.dat", factory_gameexe);
        WriteFileBytes(root / "g00" / "ME_02.g00", factory_me02);
        WriteFileBytes(root / "g00" / "MESS99.g00", factory_mess99);
    }

    std::vector<MenuPatchRecordInput> BuildV1Inputs() const {
        return {
            {"Gameexe.dat", "Gameexe.dat", 0u, factory_gameexe, en_v1_gameexe,
             kFlagBackupStandalone},
            {"g00\\ME_02.g00", "g00\\ME_02.g00", 0u, factory_me02, en_v1_me02,
             kFlagBackupStandalone},
            {"g00\\MESS99.g00", "g00\\MESS99.g00", 0u, factory_mess99,
             en_v1_mess99, kFlagBackupStandalone},
        };
    }

    std::vector<MenuPatchRecordInput> BuildV2Inputs() const {
        return {
            {"Gameexe.dat", "Gameexe.dat", 0u, factory_gameexe, en_v2_gameexe,
             kFlagBackupStandalone},
            {"g00\\ME_02.g00", "g00\\ME_02.g00", 0u, factory_me02, en_v2_me02,
             kFlagBackupStandalone},
            {"g00\\MESS99.g00", "g00\\MESS99.g00", 0u, factory_mess99,
             en_v2_mess99, kFlagBackupStandalone},
        };
    }
};

void WriteLe32InPlace(std::vector<std::uint8_t>& bytes,
                      std::size_t offset,
                      std::uint32_t value) {
    for (unsigned int i = 0u; i < 4u; ++i) {
        bytes[offset + i] =
            static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
    }
}

}  // namespace

TEST(MenuAutoPatchCt,
     FirstLaunchAppliesMultiRecordBundleBacksUpOriginalsAndWritesSentinel) {
    ScopedTempDir sandbox;
    const SimulatedGameAssets assets;
    assets.SeedFactoryTree(sandbox.path());

    // Mark Gameexe.dat read-only (as happens when copied from optical media) to
    // verify the auto-patcher clears read-only attributes cleanly during swap.
    const fs::path gameexe_path = sandbox.path() / "Gameexe.dat";
    const DWORD orig_attrs = ::GetFileAttributesW(gameexe_path.c_str());
    ASSERT_NE(orig_attrs, INVALID_FILE_ATTRIBUTES);
    ASSERT_NE(::SetFileAttributesW(gameexe_path.c_str(),
                                   orig_attrs | FILE_ATTRIBUTE_READONLY),
              0);

    std::vector<std::uint8_t> patch_v1;
    ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV1Inputs(), patch_v1),
              ManifestStatus::kOk);
    WriteFileBytes(sandbox.path() / kMenuPatchFileName, patch_v1);

    MenuPatchManifest expected_manifest{};
    ASSERT_EQ(ParseMenuPatchManifest(ByteSpan(patch_v1), expected_manifest),
              ManifestStatus::kOk);

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);

    // Live files hold unmasked English v1 assets.
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat"),
              assets.en_v1_gameexe);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
              assets.en_v1_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00"),
              assets.en_v1_mess99);

    // Immutable .bak files match factory retail originals byte-for-byte.
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat.bak"),
              assets.factory_gameexe);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00.bak"),
              assets.factory_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00.bak"),
              assets.factory_mess99);

    // "ADIEUAPL" v1 sentinel records manifest_hash, patch size, and all 3 sizes.
    const std::vector<std::uint8_t> sentinel_raw =
        ReadFileBytes(sandbox.path() / kMenuPatchSentinelFileName);
    AppliedSentinel sentinel{};
    ASSERT_EQ(ParseAppliedSentinel(ByteSpan(sentinel_raw), sentinel),
              SentinelStatus::kOk);
    EXPECT_EQ(sentinel.manifest_hash, expected_manifest.header.manifest_hash);
    EXPECT_EQ(sentinel.patch_file_size, patch_v1.size());
    ASSERT_EQ(sentinel.entries.size(), 3u);
    EXPECT_EQ(sentinel.entries[0].rel_path, "Gameexe.dat");
    EXPECT_EQ(sentinel.entries[0].expected_new_size, kPatchedV1GameexeBytes);
    EXPECT_EQ(sentinel.entries[1].rel_path, "g00\\ME_02.g00");
    EXPECT_EQ(sentinel.entries[1].expected_new_size, kPatchedV1Me02Bytes);
    EXPECT_EQ(sentinel.entries[2].rel_path, "g00\\MESS99.g00");
    EXPECT_EQ(sentinel.entries[2].expected_new_size, kPatchedV1Mess99Bytes);

    // Zero .tmp staging files remain anywhere in the tree.
    EXPECT_TRUE(CollectRelativeFilesWithSuffix(sandbox.path(), ".tmp").empty());
}

TEST(MenuAutoPatchCt,
     SecondLaunchTakesSentinelFastPathWithoutReadingPatchPayloads) {
    ScopedTempDir sandbox;
    const SimulatedGameAssets assets;
    assets.SeedFactoryTree(sandbox.path());

    std::vector<std::uint8_t> patch_v1;
    ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV1Inputs(), patch_v1),
              ManifestStatus::kOk);
    const fs::path patch_path = sandbox.path() / kMenuPatchFileName;
    WriteFileBytes(patch_path, patch_v1);

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kAlreadyApplied);

    // Corrupt an interior record byte (>= 0x20) of menu_patch.bin while keeping
    // the 32-byte header and total file size identical. If Launch 2 read and
    // verified record payloads, ParseMenuPatchManifest would reject the
    // corrupted body with kManifestHashMismatch -> kFailed.
    std::vector<std::uint8_t> interior_corrupted = patch_v1;
    interior_corrupted.back() ^= 0xFFu;
    WriteFileBytes(patch_path, interior_corrupted);

    // Sample 3 consecutive Launch-2+ calls (the first open after writing to
    // %TEMP% can incur a one-time Windows Defender minifilter scan; every call
    // must take the sentinel fast path without reading corrupted payload bytes).
    double min_elapsed_ms = std::numeric_limits<double>::infinity();
    for (int attempt = 0; attempt < kFastPathSampleCount; ++attempt) {
        const auto t0 = std::chrono::steady_clock::now();
        const MenuPatchOutcome fast_outcome =
            EnsureMenuAssetsPatched(sandbox.path().c_str());
        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        EXPECT_EQ(fast_outcome, MenuPatchOutcome::kAlreadyApplied);
        min_elapsed_ms = std::min(min_elapsed_ms, elapsed_ms);
    }
    EXPECT_LT(min_elapsed_ms, kFastPathMaxElapsedMs);

    // Live files and factory backups remain untouched.
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
              assets.en_v1_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00.bak"),
              assets.factory_me02);

    // Restore valid menu_patch.bin and truncate one live target by 1 byte:
    // fast-path size check must invalidate and heal the target from .bak.
    WriteFileBytes(patch_path, patch_v1);
    std::vector<std::uint8_t> truncated_me02 = assets.en_v1_me02;
    truncated_me02.pop_back();
    WriteFileBytes(sandbox.path() / "g00" / "ME_02.g00", truncated_me02);

    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
              assets.en_v1_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00.bak"),
              assets.factory_me02);
}

TEST(MenuAutoPatchCt,
     DeletedSentinelAndMissingBackupRecoveryPreservesFactoryBackups) {
    ScopedTempDir sandbox;
    const SimulatedGameAssets assets;
    assets.SeedFactoryTree(sandbox.path());

    std::vector<std::uint8_t> patch_v1;
    ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV1Inputs(), patch_v1),
              ManifestStatus::kOk);
    WriteFileBytes(sandbox.path() / kMenuPatchFileName, patch_v1);

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);

    // Case A: Delete menu_patch.applied while live targets hold v1 English
    // bytes. Recovery must read from <target>.bak without overwriting .bak with
    // patched bytes.
    const fs::path sentinel_path = sandbox.path() / kMenuPatchSentinelFileName;
    ASSERT_TRUE(fs::remove(sentinel_path));

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_TRUE(fs::exists(sentinel_path));
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat"),
              assets.en_v1_gameexe);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
              assets.en_v1_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00"),
              assets.en_v1_mess99);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat.bak"),
              assets.factory_gameexe);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00.bak"),
              assets.factory_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00.bak"),
              assets.factory_mess99);
    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kAlreadyApplied);

    // Case B: Delete BOTH menu_patch.applied AND g00\ME_02.g00.bak while live
    // g00\ME_02.g00 already hashes to new_raw_hash. Recovery must recognize the
    // already-patched target without creating a bogus .bak from English bytes.
    const fs::path me02_bak = sandbox.path() / "g00" / "ME_02.g00.bak";
    ASSERT_TRUE(fs::remove(sentinel_path));
    ASSERT_TRUE(fs::remove(me02_bak));

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_TRUE(fs::exists(sentinel_path));
    EXPECT_FALSE(fs::exists(me02_bak));
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
              assets.en_v1_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat.bak"),
              assets.factory_gameexe);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00.bak"),
              assets.factory_mess99);
}

TEST(MenuAutoPatchCt,
     UpgradingMenuPatchFromV1ToV2ReappliesFromImmutableFactoryBackups) {
    ScopedTempDir sandbox;
    const SimulatedGameAssets assets;
    assets.SeedFactoryTree(sandbox.path());

    const fs::path patch_path = sandbox.path() / kMenuPatchFileName;
    std::vector<std::uint8_t> patch_v1;
    ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV1Inputs(), patch_v1),
              ManifestStatus::kOk);
    WriteFileBytes(patch_path, patch_v1);

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kAlreadyApplied);

    // Upgrade 1: Replace menu_patch.bin with v2 (different payload sizes and
    // new manifest_hash). Auto-patcher must unmask against factory .bak files.
    std::vector<std::uint8_t> patch_v2;
    ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV2Inputs(), patch_v2),
              ManifestStatus::kOk);
    WriteFileBytes(patch_path, patch_v2);

    MenuPatchManifest v2_manifest{};
    ASSERT_EQ(ParseMenuPatchManifest(ByteSpan(patch_v2), v2_manifest),
              ManifestStatus::kOk);

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);

    EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat"),
              assets.en_v2_gameexe);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
              assets.en_v2_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00"),
              assets.en_v2_mess99);

    // Factory .bak files must remain 100% identical to initial retail bytes.
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat.bak"),
              assets.factory_gameexe);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00.bak"),
              assets.factory_me02);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00.bak"),
              assets.factory_mess99);

    AppliedSentinel v2_sentinel{};
    ASSERT_EQ(ParseAppliedSentinel(
                  ByteSpan(ReadFileBytes(sandbox.path() /
                                         kMenuPatchSentinelFileName)),
                  v2_sentinel),
              SentinelStatus::kOk);
    EXPECT_EQ(v2_sentinel.manifest_hash, v2_manifest.header.manifest_hash);
    EXPECT_EQ(v2_sentinel.patch_file_size, patch_v2.size());

    // Upgrade 2 (same-byte-size edge seam): Build v2b with identical byte sizes
    // to v2 so patch_file_size matches, changing only 1 byte of English output.
    // Header manifest_hash check must still invalidate the fast path!
    auto v2b_inputs = assets.BuildV2Inputs();
    v2b_inputs[1].new_raw.back() ^= 0x55u;
    std::vector<std::uint8_t> patch_v2b;
    ASSERT_EQ(SerializeMenuPatchManifest(v2b_inputs, patch_v2b),
              ManifestStatus::kOk);
    ASSERT_EQ(patch_v2b.size(), patch_v2.size());
    WriteFileBytes(patch_path, patch_v2b);

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
              v2b_inputs[1].new_raw);
    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kAlreadyApplied);
}

TEST(MenuAutoPatchCt,
     MidBatchThirdRecordFailureRollsBackStagedFilesLeavingLiveTargetsUntouched) {
    // Subcase A: Fresh factory install where Records 1 & 2 succeed in Phase 1
    // and Record 3 has a mismatched orig_slice_hash. Zero live files may change
    // and zero .bak/.tmp/.applied artifacts may survive.
    {
        ScopedTempDir sandbox;
        const SimulatedGameAssets assets;
        assets.SeedFactoryTree(sandbox.path());

        auto bad_third_inputs = assets.BuildV1Inputs();
        // Build Record 3 against a wrong factory slice so container header and
        // manifest_hash are 100% valid, forcing failure mid-batch on Record 3.
        bad_third_inputs[2].orig_slice[0] ^= 0xFFu;

        std::vector<std::uint8_t> bad_batch_blob;
        ASSERT_EQ(SerializeMenuPatchManifest(bad_third_inputs, bad_batch_blob),
                  ManifestStatus::kOk);
        WriteFileBytes(sandbox.path() / kMenuPatchFileName, bad_batch_blob);

        EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
                  MenuPatchOutcome::kFailed);

        // Records 1, 2, and 3 on disk are 100% byte-identical to pre-call state.
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat"),
                  assets.factory_gameexe);
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
                  assets.factory_me02);
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00"),
                  assets.factory_mess99);

        EXPECT_FALSE(fs::exists(sandbox.path() / kMenuPatchSentinelFileName));
        EXPECT_TRUE(
            CollectRelativeFilesWithSuffix(sandbox.path(), ".bak").empty());
        EXPECT_TRUE(
            CollectRelativeFilesWithSuffix(sandbox.path(), ".tmp").empty());
    }

    // Subcase B: Upgrade over an already-applied v1 install where Record 3's
    // xor_payload has a flipped bit (re-signed manifest_hash -> fails
    // new_raw_hash verification on Record 3). Live v1 targets and factory .bak
    // files must remain completely untouched.
    {
        ScopedTempDir sandbox;
        const SimulatedGameAssets assets;
        assets.SeedFactoryTree(sandbox.path());

        const fs::path patch_path = sandbox.path() / kMenuPatchFileName;
        std::vector<std::uint8_t> patch_v1;
        ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV1Inputs(), patch_v1),
                  ManifestStatus::kOk);
        WriteFileBytes(patch_path, patch_v1);
        ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
                  MenuPatchOutcome::kApplied);

        std::vector<std::uint8_t> broken_v2;
        ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV2Inputs(), broken_v2),
                  ManifestStatus::kOk);
        // Flip the final payload byte of Record 3 and re-sign manifest_hash so
        // ParseMenuPatchManifest succeeds and Phase 1 fails on Record 3 unmask.
        broken_v2.back() ^= 0x01u;
        const std::uint32_t resigned_hash =
            MurmurHash2(ByteSpan(broken_v2).subspan(kPatchHeaderBytes));
        WriteLe32InPlace(broken_v2, 0x10u, resigned_hash);
        WriteFileBytes(patch_path, broken_v2);

        EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
                  MenuPatchOutcome::kFailed);

        // Live targets stay at v1 English bytes; .bak files stay at factory.
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat"),
                  assets.en_v1_gameexe);
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00"),
                  assets.en_v1_me02);
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00"),
                  assets.en_v1_mess99);
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "Gameexe.dat.bak"),
                  assets.factory_gameexe);
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "ME_02.g00.bak"),
                  assets.factory_me02);
        EXPECT_EQ(ReadFileBytes(sandbox.path() / "g00" / "MESS99.g00.bak"),
                  assets.factory_mess99);
        EXPECT_TRUE(
            CollectRelativeFilesWithSuffix(sandbox.path(), ".tmp").empty());
    }
}

TEST(MenuAutoPatchCt, CommittedAdieuMenuPatchBinParsesInWin32Proxy) {
    const fs::path committed_patch =
        fs::u8path(ADIEU_SOURCE_DIR) / kMenuPatchFileName;
    ASSERT_TRUE(fs::is_regular_file(committed_patch))
        << "Missing committed Adieu/menu_patch.bin at "
        << committed_patch.u8string();

    const std::vector<std::uint8_t> blob = ReadFileBytes(committed_patch);
    ASSERT_GT(blob.size(), kPatchHeaderBytes);

    MenuPatchManifest manifest{};
    ASSERT_EQ(ParseMenuPatchManifest(ByteSpan(blob), manifest),
              ManifestStatus::kOk);
    EXPECT_EQ(manifest.header.version, 1u);
    EXPECT_GE(manifest.records.size(), 10u);

    const bool has_gameexe = std::any_of(
        manifest.records.begin(), manifest.records.end(),
        [](const auto& rec) { return rec.target_rel_path == "Gameexe.dat"; });
    EXPECT_TRUE(has_gameexe);

    // When VN_GAME_DIR is set, also exercise real 32-bit EnsureMenuAssetsPatched
    // unmasking + sentinel fast-path against a sandboxed copy of retail files.
    const wchar_t* raw_env = ::_wgetenv(L"VN_GAME_DIR");
    if (raw_env == nullptr || *raw_env == L'\0') {
        return;
    }

    fs::path retail_dir;
    const fs::path env_path(raw_env);
    for (const fs::path& cand :
         {env_path / "StartData" / "GameData", env_path / "GameData", env_path}) {
        std::error_code ec;
        if (fs::is_regular_file(cand / "Gameexe.dat", ec) ||
            fs::is_regular_file(cand / "Gameexe.dat.bak", ec)) {
            retail_dir = cand;
            break;
        }
    }
    ASSERT_FALSE(retail_dir.empty())
        << "VN_GAME_DIR set, but retail Gameexe.dat not found";

    ScopedTempDir sandbox;
    WriteFileBytes(sandbox.path() / kMenuPatchFileName, blob);

    for (const auto& rec : manifest.records) {
        fs::path src = retail_dir / fs::u8path(rec.source_rel_path);
        fs::path bak = src;
        bak += ".bak";
        std::error_code ec;
        if (fs::is_regular_file(bak, ec)) {
            src = bak;
        }
        ASSERT_TRUE(fs::is_regular_file(src, ec))
            << "Missing retail source asset: " << src.u8string();

        const fs::path dst = sandbox.path() / fs::u8path(rec.source_rel_path);
        if (dst.has_parent_path()) {
            fs::create_directories(dst.parent_path(), ec);
        }
        if (!fs::exists(dst, ec)) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(static_cast<bool>(ec));
        }
    }

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_TRUE(fs::is_regular_file(sandbox.path() / "Gameexe.dat.bak"));
    EXPECT_TRUE(fs::is_regular_file(sandbox.path() / kMenuPatchSentinelFileName));
    EXPECT_TRUE(CollectRelativeFilesWithSuffix(sandbox.path(), ".tmp").empty());

    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kAlreadyApplied);
}

TEST(MenuAutoPatchCt,
     ShrinkingV2UpgradeRestoresDroppedAssetsAtomicallyAndSupportsReAddInV3) {
    ScopedTempDir sandbox;
    const SimulatedGameAssets assets;
    assets.SeedFactoryTree(sandbox.path());

    const fs::path patch_path = sandbox.path() / kMenuPatchFileName;
    const fs::path gameexe_path = sandbox.path() / "Gameexe.dat";
    const fs::path me02_path = sandbox.path() / "g00" / "ME_02.g00";
    const fs::path mess99_path = sandbox.path() / "g00" / "MESS99.g00";
    const fs::path mess99_bak = sandbox.path() / "g00" / "MESS99.g00.bak";

    // Pass 1 (v1): apply all 3 records.
    std::vector<std::uint8_t> patch_v1;
    ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV1Inputs(), patch_v1),
              ManifestStatus::kOk);
    WriteFileBytes(patch_path, patch_v1);
    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    ASSERT_EQ(ReadFileBytes(mess99_path), assets.en_v1_mess99);
    const std::vector<std::uint8_t> v1_sentinel_bytes =
        ReadFileBytes(sandbox.path() / kMenuPatchSentinelFileName);

    // Failed shrinking v2: drops MESS99.g00 while Record 2 (ME_02.g00) has a
    // corrupted payload (manifest_hash re-signed). Two-phase rollback must keep
    // MESS99.g00 at v1 (not half-reverted), keep sentinel at v1, and leave zero
    // .tmp files.
    const std::vector<MenuPatchRecordInput> shrinking_v2_inputs = {
        {"Gameexe.dat", "Gameexe.dat", 0u, assets.factory_gameexe,
         assets.en_v2_gameexe, kFlagBackupStandalone},
        {"g00\\ME_02.g00", "g00\\ME_02.g00", 0u, assets.factory_me02,
         assets.en_v2_me02, kFlagBackupStandalone},
    };
    std::vector<std::uint8_t> broken_v2;
    ASSERT_EQ(SerializeMenuPatchManifest(shrinking_v2_inputs, broken_v2),
              ManifestStatus::kOk);
    broken_v2.back() ^= 0x01u;
    WriteLe32InPlace(
        broken_v2, 0x10u,
        MurmurHash2(ByteSpan(broken_v2).subspan(kPatchHeaderBytes)));
    WriteFileBytes(patch_path, broken_v2);

    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kFailed);
    EXPECT_EQ(ReadFileBytes(gameexe_path), assets.en_v1_gameexe);
    EXPECT_EQ(ReadFileBytes(me02_path), assets.en_v1_me02);
    EXPECT_EQ(ReadFileBytes(mess99_path), assets.en_v1_mess99);
    EXPECT_EQ(ReadFileBytes(sandbox.path() / kMenuPatchSentinelFileName),
              v1_sentinel_bytes);
    EXPECT_TRUE(CollectRelativeFilesWithSuffix(sandbox.path(), ".tmp").empty());

    // Valid shrinking v2: mark dropped MESS99.g00 read-only to confirm restore
    // clears attributes cleanly, restores MESS99.g00 from .bak, updates the
    // retained 2 targets, and keeps .bak immutable.
    ASSERT_NE(::SetFileAttributesW(mess99_path.c_str(), FILE_ATTRIBUTE_READONLY),
              0);
    std::vector<std::uint8_t> valid_v2;
    ASSERT_EQ(SerializeMenuPatchManifest(shrinking_v2_inputs, valid_v2),
              ManifestStatus::kOk);
    WriteFileBytes(patch_path, valid_v2);

    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadFileBytes(gameexe_path), assets.en_v2_gameexe);
    EXPECT_EQ(ReadFileBytes(me02_path), assets.en_v2_me02);
    EXPECT_EQ(ReadFileBytes(mess99_path), assets.factory_mess99);
    EXPECT_EQ(ReadFileBytes(mess99_bak), assets.factory_mess99);
    EXPECT_TRUE(CollectRelativeFilesWithSuffix(sandbox.path(), ".tmp").empty());

    AppliedSentinel v2_sentinel{};
    ASSERT_EQ(ParseAppliedSentinel(
                  ByteSpan(ReadFileBytes(sandbox.path() /
                                         kMenuPatchSentinelFileName)),
                  v2_sentinel),
              SentinelStatus::kOk);
    ASSERT_EQ(v2_sentinel.entries.size(), 2u);
    EXPECT_EQ(v2_sentinel.entries[0].rel_path, "Gameexe.dat");
    EXPECT_EQ(v2_sentinel.entries[1].rel_path, "g00\\ME_02.g00");
    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kAlreadyApplied);

    // Pass 3 (v3): re-adding MESS99.g00 reapplies cleanly from immutable .bak.
    std::vector<std::uint8_t> patch_v3;
    ASSERT_EQ(SerializeMenuPatchManifest(assets.BuildV2Inputs(), patch_v3),
              ManifestStatus::kOk);
    WriteFileBytes(patch_path, patch_v3);
    ASSERT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadFileBytes(mess99_path), assets.en_v2_mess99);
    EXPECT_EQ(ReadFileBytes(mess99_bak), assets.factory_mess99);
    EXPECT_TRUE(CollectRelativeFilesWithSuffix(sandbox.path(), ".tmp").empty());
    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kAlreadyApplied);
}
