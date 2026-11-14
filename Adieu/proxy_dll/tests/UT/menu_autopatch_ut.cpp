// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Specification-driven unit tests for Adieu's loader-lock-safe menu_autopatch
// entry point: missing-bundle no-op, invalid directory handling, corrupt bundle
// rejection with zero side-effect leftovers, and failed-to-valid state recovery.

#include "menu_autopatch.h"
#include "menu_patch_format.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || \
    defined(__wincodec_h__)
#error "menu_autopatch.h and menu_patch_format.h must remain free of Windows headers"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using menu_autopatch::EnsureMenuAssetsPatched;
using menu_autopatch::kMenuPatchFileName;
using menu_autopatch::kMenuPatchSentinelFileName;
using menu_autopatch::MenuPatchOutcome;
using menu_patch_format::kFlagBackupStandalone;
using menu_patch_format::ManifestStatus;
using menu_patch_format::MenuPatchRecordInput;
using menu_patch_format::SerializeMenuPatchManifest;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        const std::string name =
            "adi_autopatch_ut_日本語_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

void WriteBinaryFile(const fs::path& path,
                     const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out));
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(static_cast<bool>(out));
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto sz = in.tellg();
    if (sz <= 0) {
        return {};
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

std::vector<std::uint8_t> MakePatternBytes(std::size_t len,
                                           std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0u; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 37u + seed) & 0xFFu);
    }
    return out;
}

std::size_t CountFilesWithExtension(const fs::path& root,
                                    std::string_view ext) {
    std::size_t count = 0u;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file() &&
            entry.path().extension().u8string() == ext) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST(MenuAutoPatch, RejectsNullEmptyAndNonExistentGameDirectories) {
    EXPECT_EQ(EnsureMenuAssetsPatched(nullptr), MenuPatchOutcome::kFailed);
    EXPECT_EQ(EnsureMenuAssetsPatched(L""), MenuPatchOutcome::kFailed);

    ScopedTempDir sandbox;
    const fs::path missing_dir = sandbox.path() / "does_not_exist";
    EXPECT_EQ(EnsureMenuAssetsPatched(missing_dir.c_str()),
              MenuPatchOutcome::kFailed);
}

TEST(MenuAutoPatch, ReturnsNoPatchFileWhenBundleAbsentWithZeroSideEffects) {
    ScopedTempDir sandbox;
    const std::vector<std::uint8_t> retail_gameexe = MakePatternBytes(128u, 0x15u);
    const fs::path gameexe_path = sandbox.path() / "Gameexe.dat";
    WriteBinaryFile(gameexe_path, retail_gameexe);

    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kNoPatchFile);

    EXPECT_EQ(ReadBinaryFile(gameexe_path), retail_gameexe);
    EXPECT_FALSE(fs::exists(sandbox.path() / kMenuPatchSentinelFileName));
    EXPECT_EQ(CountFilesWithExtension(sandbox.path(), ".bak"), 0u);
    EXPECT_EQ(CountFilesWithExtension(sandbox.path(), ".tmp"), 0u);
}

TEST(MenuAutoPatch, CorruptOrTruncatedPatchFileFailsCleanlyWithoutLeftovers) {
    ScopedTempDir sandbox;
    const std::vector<std::uint8_t> retail_g00 = MakePatternBytes(96u, 0x5Au);
    const fs::path target_path = sandbox.path() / "g00" / "ME_02.g00";
    WriteBinaryFile(target_path, retail_g00);

    const fs::path patch_path = sandbox.path() / kMenuPatchFileName;

    // 1. Truncated 12-byte menu_patch.bin
    WriteBinaryFile(patch_path, {'A', 'D', 'I', 'E', 'U', 'M', 'E', 'N', 1, 0, 0, 0});
    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kFailed);
    EXPECT_EQ(ReadBinaryFile(target_path), retail_g00);
    EXPECT_FALSE(fs::exists(sandbox.path() / kMenuPatchSentinelFileName));
    EXPECT_EQ(CountFilesWithExtension(sandbox.path(), ".bak"), 0u);
    EXPECT_EQ(CountFilesWithExtension(sandbox.path(), ".tmp"), 0u);

    // 2. Valid header size with bad manifest_hash
    MenuPatchRecordInput rec;
    rec.source_rel_path = "g00\\ME_02.g00";
    rec.target_rel_path = "g00\\ME_02.g00";
    rec.orig_slice = retail_g00;
    rec.new_raw = MakePatternBytes(112u, 0xC3u);
    rec.flags = kFlagBackupStandalone;

    std::vector<std::uint8_t> valid_blob;
    ASSERT_EQ(SerializeMenuPatchManifest({rec}, valid_blob),
              ManifestStatus::kOk);
    auto tampered_blob = valid_blob;
    tampered_blob.back() ^= 0xFFu;
    WriteBinaryFile(patch_path, tampered_blob);

    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kFailed);
    EXPECT_EQ(ReadBinaryFile(target_path), retail_g00);
    EXPECT_FALSE(fs::exists(sandbox.path() / kMenuPatchSentinelFileName));
    EXPECT_EQ(CountFilesWithExtension(sandbox.path(), ".bak"), 0u);
    EXPECT_EQ(CountFilesWithExtension(sandbox.path(), ".tmp"), 0u);

    // 3. Valid -> Failing -> Valid seam: replacing tampered menu_patch.bin with
    //    the valid bundle succeeds cleanly with no leaked mutex or temp state.
    WriteBinaryFile(patch_path, valid_blob);
    EXPECT_EQ(EnsureMenuAssetsPatched(sandbox.path().c_str()),
              MenuPatchOutcome::kApplied);
    EXPECT_EQ(ReadBinaryFile(target_path), rec.new_raw);
    EXPECT_EQ(ReadBinaryFile( fs::path(target_path.wstring() + L".bak") ),
              retail_g00);
    EXPECT_TRUE(fs::exists(sandbox.path() / kMenuPatchSentinelFileName));
    EXPECT_EQ(CountFilesWithExtension(sandbox.path(), ".tmp"), 0u);
}
