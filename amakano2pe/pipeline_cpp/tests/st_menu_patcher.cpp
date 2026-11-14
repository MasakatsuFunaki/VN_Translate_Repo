// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests for `amakano2pe/menu_patch.bin` against the local retail
// installation (`C:\あざらしそふと\アマカノ2～Perfect Edition～`).
// Verifies that every record in `menu_patch.bin` matches the exact retail
// slice `MurmurHash2`, unmasks cleanly to `new_raw_hash`, every `.hg3` target
// decodes via `Hg3Codec` with 100% matching frame metadata parity, and real
// `menu_autopatch::EnsureMenuAssetsPatched` extraction in an isolated RAII
// sandbox succeeds on Run 1 (`kApplied`), writes a valid `"AMK2APLD"` sentinel,
// and fast-paths on Run 2 (`kAlreadyApplied`) without mutating retail `.int`
// archives or `%APPDATA%` saves.
#include "common/util.h"
#include "cs2/cs2_archive.h"
#include "hg3/hg3_codec.h"
#include "menu/core/fes_script_codec.h"
#include "menu/core/menu_patch_container.h"
#include "menu/domain_renderer.h"
#include "menu_autopatch.h"
#include "menu_patch_format.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kExpectedTotalRecordCount = 154;
constexpr std::size_t kExpectedHg3RecordCount = 146;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

fs::path ResolveGameDir() {
    if (const char* env = std::getenv("AMAKANO2PE_GAME_DIR");
        env != nullptr && env[0] != '\0') {
        return fs::u8path(env);
    }
    return fs::u8path("C:\\あざらしそふと\\アマカノ2～Perfect Edition～");
}

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto a = static_cast<unsigned char>(text[offset + i]);
        const auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto end_pos = in.tellg();
    if (end_pos <= 0) {
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end_pos));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void WriteBinaryFile(const fs::path& path,
                     const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "Failed to open for write: " << path.u8string();
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(out.good()) << "Failed writing: " << path.u8string();
}

fs::path ResolveRetailSliceSourcePath(
    const fs::path& game_dir, const ama::menu::MenuPatchRecord& rec) {
    const fs::path direct = game_dir / fs::u8path(rec.source_rel_path);
    if (rec.flags == ama::menu::kFlagBackupStandalone) {
        const fs::path bak = direct.wstring() + L".bak";
        if (fs::exists(bak)) {
            return bak;
        }
    }
    return direct;
}

void WriteSparseSliceAtOffset(const fs::path& sparse_file_path,
                              std::uint64_t offset,
                              const std::vector<std::uint8_t>& slice) {
    std::error_code ec;
    fs::create_directories(sparse_file_path.parent_path(), ec);
    HANDLE h = CreateFileW(sparse_file_path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE) << sparse_file_path.u8string();
    DWORD bytes_returned = 0;
    std::ignore = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                  &bytes_returned, nullptr);
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    ASSERT_NE(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN), 0);
    DWORD written = 0;
    ASSERT_NE(WriteFile(h, slice.data(), static_cast<DWORD>(slice.size()),
                        &written, nullptr),
              0);
    ASSERT_EQ(written, static_cast<DWORD>(slice.size()));
    CloseHandle(h);
}

}  // namespace

TEST(MenuPatcherSt, AllCommittedRecordsUnmaskAgainstRetailSlicesAndPreserveHg3AndScriptInvariants) {
    const fs::path game_dir = ResolveGameDir();
    if (!fs::exists(game_dir / "cs2.exe") ||
        !fs::exists(game_dir / "image00.int") ||
        !fs::exists(game_dir / "image01.int")) {
        GTEST_SKIP() << "Retail game installation not present at "
                     << game_dir.u8string();
    }

    const fs::path patch_path = fs::u8path(AMA_PROJECT_DIR) / "menu_patch.bin";
    ASSERT_TRUE(fs::exists(patch_path)) << patch_path.u8string();

    const std::vector<std::uint8_t> bundle_bytes = ReadBinaryFile(patch_path);
    ASSERT_FALSE(bundle_bytes.empty());
    ASSERT_TRUE(ama::menu::VerifyNoRawAssetSignatures(bundle_bytes));

    const ama::menu::MenuPatchContainerData manifest =
        ama::menu::ParseMenuPatchContainer(bundle_bytes);
    ASSERT_EQ(manifest.records.size(), kExpectedTotalRecordCount);

    menu_patch::MenuPatchManifest consumer_manifest;
    ASSERT_EQ(menu_patch::ParseMenuPatchContainer(
                  menu_patch::ByteView{bundle_bytes.data(), bundle_bytes.size()},
                  consumer_manifest),
              menu_patch::ParseStatus::kOk);
    ASSERT_EQ(consumer_manifest.records.size(), kExpectedTotalRecordCount);

    ama::menu::GameAssetSource asset_source(game_dir);
    std::size_t verified_hg3 = 0;
    bool checked_startup_xml = false;
    bool checked_inputtxt_fes = false;

    for (std::size_t i = 0; i < manifest.records.size(); ++i) {
        const auto& rec = manifest.records[i];
        const auto& consumer_rec = consumer_manifest.records[i];
        const fs::path src_path = ResolveRetailSliceSourcePath(game_dir, rec);
        ASSERT_TRUE(fs::exists(src_path))
            << "Missing retail source file: " << src_path.u8string();

        const std::vector<std::uint8_t> orig_slice =
            ama::cs2::read_kif_raw_slice(src_path.u8string(), rec.orig_offset,
                                         rec.orig_size);
        ASSERT_EQ(orig_slice.size(), rec.orig_size)
            << "Slice read failed for " << rec.source_rel_path;
        EXPECT_EQ(ama::menu::MurmurHash2(orig_slice), rec.orig_slice_hash)
            << "Retail slice hash mismatch for " << rec.target_rel_path;

        const std::vector<std::uint8_t> unmasked =
            ama::menu::UnmaskMenuPatchRecord(rec, orig_slice);
        ASSERT_EQ(unmasked.size(), rec.new_raw_size);
        EXPECT_EQ(ama::menu::MurmurHash2(unmasked), rec.new_raw_hash);

        std::vector<std::uint8_t> consumer_unmasked;
        ASSERT_EQ(menu_patch::UnmaskRecordPayload(
                      menu_patch::ByteView{bundle_bytes.data(),
                                           bundle_bytes.size()},
                      consumer_rec,
                      menu_patch::ByteView{orig_slice.data(), orig_slice.size()},
                      consumer_unmasked),
                  menu_patch::UnmaskStatus::kOk)
            << rec.target_rel_path;
        EXPECT_EQ(consumer_unmasked, unmasked) << rec.target_rel_path;

        if (EndsWithIgnoreCase(rec.target_rel_path, ".hg3")) {
            const std::string filename =
                fs::u8path(rec.target_rel_path).filename().u8string();
            const ama::hg3::Hg3Container orig_hg3 = asset_source.LoadHg3(filename);
            const ama::hg3::Hg3Container patched_hg3 =
                ama::hg3::Hg3Codec::Decode(unmasked);
            EXPECT_TRUE(
                ama::hg3::Hg3Codec::VerifyFrameMetadataParity(orig_hg3, patched_hg3))
                << "HG-3 metadata parity mismatch on " << rec.target_rel_path;
            if (rec.flags == ama::menu::kFlagBackupStandalone) {
                ASSERT_FALSE(patched_hg3.frames.empty());
                EXPECT_TRUE(patched_hg3.frames[0].top_down_img0000);
            }
            ++verified_hg3;
            continue;
        }

        if (EndsWithIgnoreCase(rec.target_rel_path, ".fes")) {
            const ama::menu::FesPayload fes_payload =
                ama::menu::FesScriptCodec::UnpackFes(unmasked);
            const std::string text = fes_payload.AsText();
            EXPECT_FALSE(text.empty()) << rec.target_rel_path;
            if (EndsWithIgnoreCase(rec.target_rel_path, "inputtxt.fes")) {
                EXPECT_NE(text.find("和倉"), std::string::npos);
                EXPECT_NE(text.find("賢一"), std::string::npos);
                checked_inputtxt_fes = true;
            }
            continue;
        }

        if (EndsWithIgnoreCase(rec.target_rel_path, "startup.xml")) {
            const std::string xml(unmasked.begin(), unmasked.end());
            EXPECT_NE(xml.find("<title>アマカノ2～Perfect Edition～</title>"),
                      std::string::npos);
            checked_startup_xml = true;
        }
    }

    EXPECT_EQ(verified_hg3, kExpectedHg3RecordCount);
    EXPECT_TRUE(checked_startup_xml);
    EXPECT_TRUE(checked_inputtxt_fes);
}

TEST(MenuPatcherSt,
     RealMenuAutoPatchExtractsAllOverridesWritesSentinelAndFastPathsOnSecondRunWithoutMutatingRetailFiles) {
    const fs::path game_dir = ResolveGameDir();
    if (!fs::exists(game_dir / "cs2.exe") ||
        !fs::exists(game_dir / "boot.hg3") ||
        !fs::exists(game_dir / "fes.int") ||
        !fs::exists(game_dir / "config.int") ||
        !fs::exists(game_dir / "image00.int") ||
        !fs::exists(game_dir / "image01.int")) {
        GTEST_SKIP() << "Retail game installation not present at "
                     << game_dir.u8string();
    }

    const fs::path patch_path = fs::u8path(AMA_PROJECT_DIR) / "menu_patch.bin";
    ASSERT_TRUE(fs::exists(patch_path));
    const std::vector<std::uint8_t> bundle_bytes = ReadBinaryFile(patch_path);
    const ama::menu::MenuPatchContainerData manifest =
        ama::menu::ParseMenuPatchContainer(bundle_bytes);
    ASSERT_EQ(manifest.records.size(), kExpectedTotalRecordCount);

    const ScopedTempDir sandbox("ama_st_menu_autopatch_sandbox");
    WriteBinaryFile(sandbox.path() / "menu_patch.bin", bundle_bytes);

    std::map<std::string, std::vector<std::uint8_t>> retail_standalone_originals;
    for (const auto& rec : manifest.records) {
        const fs::path retail_src = ResolveRetailSliceSourcePath(game_dir, rec);
        if (rec.flags == ama::menu::kFlagBackupStandalone) {
            const std::vector<std::uint8_t> standalone_bytes =
                ReadBinaryFile(retail_src);
            ASSERT_FALSE(standalone_bytes.empty()) << rec.target_rel_path;
            WriteBinaryFile(sandbox.path() / fs::u8path(rec.target_rel_path),
                            standalone_bytes);
            retail_standalone_originals[rec.target_rel_path] = standalone_bytes;
        } else {
            const std::vector<std::uint8_t> slice =
                ama::cs2::read_kif_raw_slice(retail_src.u8string(),
                                             rec.orig_offset, rec.orig_size);
            ASSERT_EQ(slice.size(), rec.orig_size) << rec.source_rel_path;
            WriteSparseSliceAtOffset(
                sandbox.path() / fs::u8path(rec.source_rel_path),
                rec.orig_offset, slice);
        }
    }

    // Run 1: execute real `menu_autopatch::EnsureMenuAssetsPatched` across all 154 records.
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.path().wstring()),
              menu_autopatch::PatchOutcome::kApplied);

    // Verify binary "AMK2APLD" sentinel was written and matches the manifest.
    const fs::path sentinel_path = sandbox.path() / "menu_patch.applied";
    ASSERT_TRUE(fs::exists(sentinel_path));
    const std::vector<std::uint8_t> sentinel_bytes = ReadBinaryFile(sentinel_path);
    menu_patch::AppliedSentinel sentinel;
    ASSERT_TRUE(menu_patch::ParseAppliedSentinel(
        menu_patch::ByteView{sentinel_bytes.data(), sentinel_bytes.size()},
        sentinel));
    EXPECT_EQ(sentinel.manifest_hash, manifest.manifest_hash);
    EXPECT_EQ(sentinel.patch_file_size, bundle_bytes.size());
    EXPECT_EQ(sentinel.targets.size(), kExpectedTotalRecordCount);

    // Verify all 154 targets exist with exact expected size and hash, and all
    // standalone .bak files match retail originals.
    for (const auto& rec : manifest.records) {
        const fs::path target_file = sandbox.path() / fs::u8path(rec.target_rel_path);
        ASSERT_TRUE(fs::exists(target_file)) << rec.target_rel_path;
        const std::vector<std::uint8_t> extracted_bytes =
            ReadBinaryFile(target_file);
        EXPECT_EQ(extracted_bytes.size(), rec.new_raw_size) << rec.target_rel_path;
        EXPECT_EQ(ama::menu::MurmurHash2(extracted_bytes), rec.new_raw_hash)
            << rec.target_rel_path;

        if (rec.flags == ama::menu::kFlagBackupStandalone) {
            const fs::path bak_file = target_file.wstring() + L".bak";
            ASSERT_TRUE(fs::exists(bak_file)) << rec.target_rel_path;
            EXPECT_EQ(ReadBinaryFile(bak_file),
                      retail_standalone_originals.at(rec.target_rel_path))
                << rec.target_rel_path;
        }
    }

    // Run 2: fast-path via `menu_patch.applied` sentinel.
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.path().wstring()),
              menu_autopatch::PatchOutcome::kAlreadyApplied);

    // Run 3: removing `menu_patch.applied` replays extraction using `<file>.bak`
    // for standalone records and keeps `<file>.bak` byte-for-byte identical to
    // the retail original.
    std::error_code ec;
    fs::remove(sentinel_path, ec);
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.path().wstring()),
              menu_autopatch::PatchOutcome::kApplied);
    for (const auto& [rel_path, orig_bytes] : retail_standalone_originals) {
        const fs::path target_file = sandbox.path() / fs::u8path(rel_path);
        const fs::path bak_file = target_file.wstring() + L".bak";
        EXPECT_EQ(ReadBinaryFile(bak_file), orig_bytes) << rel_path;
    }
}
