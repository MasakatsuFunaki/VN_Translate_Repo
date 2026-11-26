// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for output-formatting helpers and synthetic deployment resolution.
#include "common/util.h"
#include "patch/deploy.h"

#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
using namespace ksh;

namespace {

constexpr int kHexWidth0 = 0;
constexpr int kHexWidth4 = 4;
constexpr int kHexWidth8 = 8;
constexpr std::uint64_t kSampleHexShort = 0x1AFull;
constexpr std::uint64_t kSampleHexWide = 0x123456ull;
constexpr std::uint64_t kSampleHexLower = 0x42896Bull;
constexpr std::int64_t kSampleSignedPos = 0x3C35LL;
constexpr std::int64_t kSampleSignedNegSmall = -5LL;
constexpr std::int64_t kSampleSignedNegWide = -0x12345678LL;
constexpr std::size_t kPadWidth4 = 4;
constexpr std::size_t kEmbeddedNulSpanLen = 3;

class ScopedDeployTempDir {
public:
    explicit ScopedDeployTempDir(const char* leaf)
        : path_(fs::temp_directory_path() / leaf) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedDeployTempDir(const ScopedDeployTempDir&) = delete;
    ScopedDeployTempDir& operator=(const ScopedDeployTempDir&) = delete;

    ~ScopedDeployTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void WriteBinaryPayload(const fs::path& file_path, const std::string& payload) {
    fs::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

void WriteHelperPair(const fs::path& dir) {
    WriteBinaryPayload(dir / "alpharom_helper.dll", "DLL");
    WriteBinaryPayload(dir / "launch.exe", "EXE");
}

}  // namespace

TEST(Format, hexu_pads_uppercase_to_the_field_width) {
    EXPECT_EQ(hexu(kSampleHexShort, kHexWidth8), "000001AF");
    EXPECT_EQ(hexu(kSampleHexShort, kHexWidth0), "1AF");
    EXPECT_EQ(hexu(0, kHexWidth4), "0000");
    EXPECT_EQ(hexu(kSampleHexWide, kHexWidth4), "123456");
}

TEST(Format, hexl_is_lowercase_with_an_0x_prefix) {
    EXPECT_EQ(hexl(kSampleHexLower), "0x42896b");
    EXPECT_EQ(hexl(0), "0x0");
}

TEST(Format, hexu_signed_pads_the_sign_into_the_field) {
    EXPECT_EQ(hexu_signed(kSampleSignedPos, kHexWidth8), "00003C35");
    EXPECT_EQ(hexu_signed(kSampleSignedNegSmall, kHexWidth8), "-0000005");
    EXPECT_EQ(hexu_signed(kSampleSignedNegWide, kHexWidth8), "-12345678");
}

TEST(Format, hex_bytes_and_hex_lower) {
    const Bytes b{0x00u, 0x1Fu, 0xABu, 0xFFu};
    EXPECT_EQ(hex_bytes(b), "00 1F AB FF");
    EXPECT_EQ(hex_lower(b.data(), b.size()), "001fabff");
    EXPECT_EQ(hex_bytes(Bytes{}), "");
}

TEST(Format, ljust_pads_but_never_truncates) {
    EXPECT_EQ(ljust("ab", kPadWidth4), "ab  ");
    EXPECT_EQ(ljust("abcdef", kPadWidth4), "abcdef");
}

TEST(Format, bytes_repr_escaping_and_quote_choice_are_pinned) {
    auto repr = [](const std::string& s) {
        return bytes_repr(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    };
    EXPECT_EQ(repr("abc"), "b'abc'");
    EXPECT_EQ(repr(std::string("\\\\.\\SICE")), "b'\\\\\\\\.\\\\SICE'");
    EXPECT_EQ(repr(std::string("a\0b", kEmbeddedNulSpanLen)), "b'a\\x00b'");
    EXPECT_EQ(repr("\t\n\r"), "b'\\t\\n\\r'");
    EXPECT_EQ(repr("\x7F\x80\xFF"), "b'\\x7f\\x80\\xff'");
    EXPECT_EQ(repr("it's"), "b\"it's\"");
    EXPECT_EQ(repr("it's \"x\""), "b'it\\'s \"x\"'");
}

TEST(PathJoin, does_not_double_or_invent_separators) {
    using patch::path_join;
    EXPECT_EQ(path_join(R"(X:\Synthetic\KH)", "launch.exe"), R"(X:\Synthetic\KH\launch.exe)");
    EXPECT_EQ(path_join(R"(X:\Synthetic\KH\)", "launch.exe"), R"(X:\Synthetic\KH\launch.exe)");
    EXPECT_EQ(path_join("X:/Synthetic/KH/", "launch.exe"), "X:/Synthetic/KH/launch.exe");
    EXPECT_EQ(path_join("X:", "launch.exe"), "X:launch.exe");
    EXPECT_EQ(path_join("", "launch.exe"), "launch.exe");
}

TEST(PathJoin, base_name_takes_either_separator) {
    EXPECT_EQ(patch::base_name(R"(X:\a\b\launch.exe)"), "launch.exe");
    EXPECT_EQ(patch::base_name("a/b/launch.exe"), "launch.exe");
    EXPECT_EQ(patch::base_name("launch.exe"), "launch.exe");
}

TEST(CanWrite, probes_the_directory_rather_than_trusting_acls) {
    const ScopedDeployTempDir temp("vn_ut_can_write_kishin_hisho");
    EXPECT_TRUE(patch::can_write(temp.path().u8string()));
    EXPECT_FALSE(patch::can_write((temp.path() / "no_such_dir_here").u8string()));

    const fs::path regular_file = temp.path() / "README.md";
    WriteBinaryPayload(regular_file, "synthetic regular file");
    EXPECT_FALSE(patch::can_write(regular_file.u8string()));
}

TEST(DeployResolution, probes_release_zip_before_install_and_dev_trees) {
    const ScopedDeployTempDir temp("vn_ut_deploy_kishin_hisho");
    const fs::path release_root = temp.path() / "release_zip";
    const fs::path release_bin = release_root / "bin";
    const fs::path release_game = release_root / "game";
    fs::create_directories(release_bin);
    WriteHelperPair(release_game);
    WriteBinaryPayload(release_game / "0script.pak", "PAK_DATA");
    WriteBinaryPayload(release_game / "script" / "da0100.nps", "NPS_DATA");

    EXPECT_EQ(fs::u8path(patch::find_helper_artifact_dir(
                  release_root.u8string(), release_bin.u8string())),
              release_game);
    EXPECT_EQ(fs::u8path(patch::find_staged_game_dir(
                  release_root.u8string(), release_bin.u8string())),
              release_game);

    const fs::path target_game = temp.path() / "installed_game";
    fs::create_directories(target_game);
    patch::deploy_script_artifacts(
        release_root.u8string(), target_game.u8string(), release_game.u8string());

    EXPECT_TRUE(fs::is_regular_file(target_game / "0script.pak"));
    EXPECT_TRUE(fs::is_regular_file(target_game / "script" / "da0100.nps"));
}

TEST(DeployResolution, probes_sibling_game_and_dev_helper_release_fallbacks) {
    const ScopedDeployTempDir temp("vn_ut_deploy_kishin_hisho_dev");
    const fs::path checkout = temp.path() / "checkout";
    const fs::path install_bin = checkout / "build" / "install" / "bin";
    const fs::path install_game = checkout / "build" / "install" / "game";
    const fs::path helper_release = checkout / "build" / "helper" / "Release";
    const fs::path pipeline_release = checkout / "build" / "pipeline" / "Release";
    fs::create_directories(install_bin);
    fs::create_directories(pipeline_release);

    WriteHelperPair(helper_release);
    EXPECT_EQ(fs::u8path(patch::find_helper_artifact_dir(
                  checkout.u8string(), pipeline_release.u8string())),
              helper_release);

    WriteHelperPair(install_game);
    WriteBinaryPayload(install_game / "0script.pak", "STAGED_PAK");
    EXPECT_EQ(fs::u8path(patch::find_helper_artifact_dir(
                  checkout.u8string(), install_bin.u8string())),
              install_game);
    EXPECT_EQ(fs::u8path(patch::find_staged_game_dir(
                  checkout.u8string(), install_bin.u8string())),
              install_game);
}

TEST(AtomicFileIo, write_and_copy_atomic_handle_empty_seam_bytes_readonly_and_errors) {
    const ScopedDeployTempDir temp("vn_ut_atomic_io_kishin_hisho");
    const fs::path dst = temp.path() / "payload.bin";

    // Empty 0-byte write leaves zero .tmp files.
    write_file(dst.u8string(), Bytes{});
    EXPECT_TRUE(read_file(dst.u8string()).empty());
    EXPECT_FALSE(fs::exists(temp.path() / "payload.bin.tmp"));

    // Binary seam bytes (0x00, 0x7F, 0x80, 0xFF) round-trip cleanly.
    const Bytes seam_bytes{0x00u, 0x7Fu, 0x80u, 0xFFu, 0x42u};
    write_file(dst.u8string(), seam_bytes);
    EXPECT_EQ(read_file(dst.u8string()), seam_bytes);
    EXPECT_FALSE(fs::exists(temp.path() / "payload.bin.tmp"));

    // Overwriting a read-only file succeeds atomically.
    ASSERT_NE(::SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_READONLY), 0);
    const Bytes updated{0xDEu, 0xADu, 0xBEu, 0xEFu};
    write_file(dst.u8string(), updated);
    EXPECT_EQ(read_file(dst.u8string()), updated);
    EXPECT_FALSE(fs::exists(temp.path() / "payload.bin.tmp"));

    // Writing to a missing parent throws and leaves zero .tmp files.
    const fs::path missing_parent = temp.path() / "no_such_dir" / "out.bin";
    EXPECT_THROW(write_file(missing_parent.u8string(), updated), std::runtime_error);
    EXPECT_FALSE(fs::exists(missing_parent));

    // copy_file_atomic copies exact bytes and clears read-only destination.
    const fs::path copied = temp.path() / "copied.bin";
    write_file(copied.u8string(), seam_bytes);
    ASSERT_NE(::SetFileAttributesW(copied.c_str(), FILE_ATTRIBUTE_READONLY), 0);
    copy_file_atomic(dst.u8string(), copied.u8string());
    EXPECT_EQ(read_file(copied.u8string()), updated);
    EXPECT_FALSE(fs::exists(temp.path() / "copied.bin.tmp"));

    // backup_file_if_missing creates immutable .bak once and never overwrites it.
    EXPECT_TRUE(backup_file_if_missing(copied.u8string()));
    EXPECT_EQ(read_file((temp.path() / "copied.bin.bak").u8string()), updated);
    write_file(copied.u8string(), seam_bytes);
    EXPECT_FALSE(backup_file_if_missing(copied.u8string()));
    EXPECT_EQ(read_file((temp.path() / "copied.bin.bak").u8string()), updated);
    EXPECT_FALSE(fs::exists(temp.path() / "copied.bin.bak.tmp"));

    // Missing file returns false without creating .bak.
    EXPECT_FALSE(backup_file_if_missing((temp.path() / "absent.bin").u8string()));
}

TEST(AtomicFileIo, atomic_file_batch_rolls_back_all_staged_files_on_failure) {
    const ScopedDeployTempDir temp("vn_ut_atomic_batch_kishin_hisho");
    const fs::path file_a = temp.path() / "a.bin";
    const fs::path file_b = temp.path() / "b.bin";
    const Bytes orig_a{0x11u, 0x22u, 0x33u};
    const Bytes new_a{0xAAu, 0xBBu, 0xCCu};
    write_file(file_a.u8string(), orig_a);

    // Trigger failure on item #2 via missing source file inside a scoped batch.
    {
        AtomicFileBatch batch;
        batch.StageWrite(file_a.u8string(), new_a, /*backup_existing=*/true);
        EXPECT_THROW(batch.StageCopy((temp.path() / "does_not_exist.bin").u8string(),
                                     file_b.u8string(), /*backup_existing=*/true),
                     std::runtime_error);
    }

    // Item #1 must remain untouched, no .bak created, and zero .tmp left behind.
    EXPECT_EQ(read_file(file_a.u8string()), orig_a);
    EXPECT_FALSE(fs::exists(temp.path() / "a.bin.bak"));
    EXPECT_FALSE(fs::exists(file_b));
    for (const auto& entry : fs::recursive_directory_iterator(temp.path())) {
        EXPECT_NE(entry.path().extension(), L".tmp") << entry.path().u8string();
    }
}

TEST(AtomicFileIo, atomic_file_batch_commits_backups_moves_and_rejects_case_duplicates) {
    const ScopedDeployTempDir temp("vn_ut_atomic_batch_commit_kishin_hisho");
    const fs::path src_copy = temp.path() / "src.bin";
    const fs::path dst_write = temp.path() / "target_w.bin";
    const fs::path dst_copy = temp.path() / "target_c.bin";
    const fs::path dst_nobak = temp.path() / "target_nobak.bin";

    const Bytes orig_w{0x01u, 0x02u, 0x03u};
    const Bytes orig_c{0x10u, 0x20u, 0x30u};
    const Bytes orig_nobak{0x44u, 0x55u};
    const Bytes v1_w{0xA1u, 0xA2u, 0xA3u, 0xA4u};
    const Bytes v1_c{0xB1u, 0xB2u, 0xB3u};
    const Bytes v1_nobak{0xC1u, 0xC2u};

    write_file(src_copy.u8string(), v1_c);
    write_file(dst_write.u8string(), orig_w);
    write_file(dst_copy.u8string(), orig_c);
    write_file(dst_nobak.u8string(), orig_nobak);

    // Move construction transfers staged entries; moved-from batch rejects calls.
    AtomicFileBatch batch1;
    batch1.StageWrite(dst_write.u8string(), v1_w, /*backup_existing=*/true);
    batch1.StageCopy(src_copy.u8string(), dst_copy.u8string(), /*backup_existing=*/true);
    batch1.StageWrite(dst_nobak.u8string(), v1_nobak, /*backup_existing=*/false);

    // Case-insensitive duplicate destination is rejected before clobbering .tmp.
    const fs::path dup_upper = temp.path() / "TARGET_W.BIN";
    EXPECT_THROW(batch1.StageWrite(dup_upper.u8string(), Bytes{0xFFu}, false),
                 std::invalid_argument);
    EXPECT_THROW(batch1.StageCopy(src_copy.u8string(), (temp.path() / "TARGET_C.BIN").u8string(),
                                  false),
                 std::invalid_argument);

    AtomicFileBatch moved_batch(std::move(batch1));
    EXPECT_THROW(batch1.StageWrite((temp.path() / "x.bin").u8string(), Bytes{1u}, false),
                 std::logic_error);
    EXPECT_THROW(batch1.Commit(), std::logic_error);

    moved_batch.Commit();
    EXPECT_EQ(read_file(dst_write.u8string()), v1_w);
    EXPECT_EQ(read_file((temp.path() / "target_w.bin.bak").u8string()), orig_w);
    EXPECT_EQ(read_file(dst_copy.u8string()), v1_c);
    EXPECT_EQ(read_file((temp.path() / "target_c.bin.bak").u8string()), orig_c);
    EXPECT_EQ(read_file(dst_nobak.u8string()), v1_nobak);
    EXPECT_FALSE(fs::exists(temp.path() / "target_nobak.bin.bak"));

    // Second overwrite via a new batch updates targets without mutating immutable .bak.
    const Bytes v2_w{0xE1u, 0xE2u};
    AtomicFileBatch batch2;
    batch2.StageWrite(dst_write.u8string(), v2_w, /*backup_existing=*/true);
    batch2.Commit();
    EXPECT_EQ(read_file(dst_write.u8string()), v2_w);
    EXPECT_EQ(read_file((temp.path() / "target_w.bin.bak").u8string()), orig_w);

    for (const auto& entry : fs::recursive_directory_iterator(temp.path())) {
        EXPECT_NE(entry.path().extension(), L".tmp") << entry.path().u8string();
    }
}
