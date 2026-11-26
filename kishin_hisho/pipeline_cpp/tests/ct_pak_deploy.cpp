// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box component tests for kishin_hisho's multi-script NpsRebuilder ->
// PakArchiveReader producer-to-consumer pipeline and patch::deploy_script_artifacts
// atomic overwrite, staging hygiene (zero .bak/.tmp), first-overwrite immutable
// .bak backups, and mid-batch fault rollback.
#include "common/util.h"
#include "pak/nps_rebuilder.h"
#include "pak/pak_archive.h"
#include "patch/deploy.h"

#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace ksh;
using namespace ksh::pak;

namespace {

class ScopedSandboxDir {
public:
    explicit ScopedSandboxDir(const char* prefix) {
        static std::atomic<unsigned> seq{0u};
        path_ = fs::temp_directory_path() /
                fs::u8path(std::string(prefix) + "_" + std::to_string(++seq));
        ClearReadOnlyRecursively(path_);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedSandboxDir() {
        ClearReadOnlyRecursively(path_);
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedSandboxDir(const ScopedSandboxDir&) = delete;
    ScopedSandboxDir& operator=(const ScopedSandboxDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

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

    fs::path path_;
};

std::vector<std::string> FindFilesWithExtension(const fs::path& root,
                                                std::wstring_view ext) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ext) {
            found.push_back(fs::relative(entry.path(), root, ec).generic_u8string());
        }
    }
    return found;
}

void SeedSyntheticProject(const fs::path& project_dir,
                          std::string_view line_a,
                          std::string_view line_b) {
    const fs::path script_out = project_dir / "script_output";
    fs::create_directories(script_out);

    const std::string translated_json =
        "{\n"
        "  \"da0100.nps\": {\n"
        "    \"strings\": [\n"
        "      {\"translated\": \"" + std::string(line_a) + "\"}\n"
        "    ]\n"
        "  },\n"
        "  \"da0101.nps\": {\n"
        "    \"strings\": [\n"
        "      {\"translated\": \"" + std::string(line_b) + "\"}\n"
        "    ]\n"
        "  }\n"
        "}\n";

    const std::string recon_map_json =
        "{\n"
        "  \"da0100.nps\": {\n"
        "    \"chunks\": [\"<BOX type=\\\"0\\\">\\r\", \"\\r</BOX>\"],\n"
        "    \"blocks\": [\n"
        "      {\n"
        "        \"box_type\": 0,\n"
        "        \"first_indent\": \"　\",\n"
        "        \"trailing_k\": true,\n"
        "        \"k_clause_count\": 1,\n"
        "        \"orig_k_clauses_clean\": [\"Old line A.\"],\n"
        "        \"ruby_list\": [],\n"
        "        \"font_list\": [],\n"
        "        \"wait_list\": [],\n"
        "        \"is_typewriter\": false,\n"
        "        \"has_complex_fx\": false,\n"
        "        \"orig_clean_en\": \"Old line A.\",\n"
        "        \"orig_raw_en\": \"　Old line A.<k>\"\n"
        "      }\n"
        "    ]\n"
        "  },\n"
        "  \"da0101.nps\": {\n"
        "    \"chunks\": [\"<BOX type=\\\"1\\\">\\r\", \"\\r</BOX>\"],\n"
        "    \"blocks\": [\n"
        "      {\n"
        "        \"box_type\": 1,\n"
        "        \"first_indent\": \"　\",\n"
        "        \"trailing_k\": true,\n"
        "        \"k_clause_count\": 1,\n"
        "        \"orig_k_clauses_clean\": [\"Old line B.\"],\n"
        "        \"ruby_list\": [],\n"
        "        \"font_list\": [],\n"
        "        \"wait_list\": [],\n"
        "        \"is_typewriter\": false,\n"
        "        \"has_complex_fx\": false,\n"
        "        \"orig_clean_en\": \"Old line B.\",\n"
        "        \"orig_raw_en\": \"　Old line B.<k>\"\n"
        "      }\n"
        "    ]\n"
        "  },\n"
        "  \"__non_nps_files__\": {\n"
        "    \"product.ini\": \"[製品情報]\\rname=KishinHisho\"\n"
        "  }\n"
        "}\n";

    write_file((script_out / "translated_text.json").u8string(), translated_json);
    write_file((script_out / "nps_reconstruction_map.json").u8string(), recon_map_json);
}

}  // namespace

TEST(PakDeployCt, StagingModeRebuildsAtomicallyAcrossRepeatRunsWithZeroBakOrTmpLeaks) {
    const ScopedSandboxDir sandbox("ksh_ct_staging");
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path staging_game = sandbox.path() / "install_game";
    fs::create_directories(staging_game);

    SeedSyntheticProject(project_dir, "Demonbane awakens under Arkham skies.",
                         "Al Azif opens the grimoire.");

    const NpsRebuilder rebuilder;
    const NpsRebuildStats stats1 = rebuilder.RebuildAndDeploy(
        project_dir.u8string(), {staging_game.u8string()}, /*verify_archive=*/true,
        /*backup_existing=*/false);
    EXPECT_EQ(stats1.total_nps_scripts, 2u);
    EXPECT_EQ(stats1.total_pak_files, 3u);

    // Repeat run against the same staging directory (as in repeated --install)
    // must overwrite cleanly with zero .bak and zero .tmp files.
    SeedSyntheticProject(project_dir, "Demonbane rises again.",
                         "Al Azif chants the spell.");
    const NpsRebuildStats stats2 = rebuilder.RebuildAndDeploy(
        project_dir.u8string(), {staging_game.u8string()}, /*verify_archive=*/true,
        /*backup_existing=*/false);
    EXPECT_EQ(stats2.total_pak_files, 3u);

    EXPECT_TRUE(FindFilesWithExtension(staging_game, L".bak").empty());
    EXPECT_TRUE(FindFilesWithExtension(staging_game, L".tmp").empty());

    // Producer -> Consumer verification: unpack staged 0script.pak and verify
    // concrete rebuilt CP932 script payloads as well as loose file parity.
    const Bytes pak_bytes = read_file((staging_game / "0script.pak").u8string());
    const PakArchiveReader reader(pak_bytes);
    ASSERT_EQ(reader.Entries().size(), 3u);

    const Bytes expected_da0100 =
        Utf8ToCp932("<BOX type=\"0\">\r　Demonbane rises again.<k>\r</BOX>");
    const Bytes expected_da0101 =
        Utf8ToCp932("<BOX type=\"1\">\r　Al Azif chants the spell.<k>\r</BOX>");
    const Bytes expected_ini = Utf8ToCp932("[製品情報]\rname=KishinHisho");

    const PakEntry* e_da0100 = reader.FindEntry(R"(script\da0100.nps)");
    const PakEntry* e_da0101 = reader.FindEntry(R"(script\da0101.nps)");
    const PakEntry* e_ini = reader.FindEntry(R"(script\product.ini)");
    ASSERT_NE(e_da0100, nullptr);
    ASSERT_NE(e_da0101, nullptr);
    ASSERT_NE(e_ini, nullptr);
    EXPECT_EQ(e_da0100->payload, expected_da0100);
    EXPECT_EQ(e_da0101->payload, expected_da0101);
    EXPECT_EQ(e_ini->payload, expected_ini);

    for (const PakEntry& entry : reader.Entries()) {
        const fs::path loose = staging_game / "script" / patch::base_name(entry.relative_path);
        ASSERT_TRUE(fs::is_regular_file(loose)) << loose.u8string();
        EXPECT_EQ(read_file(loose.u8string()), entry.payload) << entry.relative_path;
    }
}

TEST(PakDeployCt, GameDeployModeCreatesImmutableBackupsOnFirstOverwriteAndPreservesRetailPak) {
    const ScopedSandboxDir sandbox("ksh_ct_game_deploy");
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path game_dir = sandbox.path() / "game";
    fs::create_directories(game_dir / "script");

    const Bytes retail_script_pak{0x04u, 0x00u, 0x7Fu, 0x80u, 0xFFu};
    const Bytes preexisting_0script{0x10u, 0x20u, 0x30u, 0x40u};
    const Bytes preexisting_da0100{0x55u, 0x66u, 0x77u};
    write_file((game_dir / "script.pak").u8string(), retail_script_pak);
    write_file((game_dir / "0script.pak").u8string(), preexisting_0script);
    write_file((game_dir / "script" / "da0100.nps").u8string(), preexisting_da0100);

    // Pass 1: deploy v1 via patch::deploy_script_artifacts (backup_existing = true).
    SeedSyntheticProject(project_dir, "First pass translation A.",
                         "First pass translation B.");
    patch::deploy_script_artifacts(project_dir.u8string(), game_dir.u8string());

    const Bytes expected_v1_da0100 =
        Utf8ToCp932("<BOX type=\"0\">\r　First pass translation A.<k>\r</BOX>");
    const Bytes expected_v1_da0101 =
        Utf8ToCp932("<BOX type=\"1\">\r　First pass translation B.<k>\r</BOX>");
    const Bytes v1_pak = read_file((game_dir / "0script.pak").u8string());
    const PakArchiveReader v1_reader(v1_pak);
    ASSERT_NE(v1_reader.FindEntry(R"(script\da0100.nps)"), nullptr);
    ASSERT_NE(v1_reader.FindEntry(R"(script\da0101.nps)"), nullptr);
    EXPECT_EQ(v1_reader.FindEntry(R"(script\da0100.nps)")->payload, expected_v1_da0100);
    EXPECT_EQ(v1_reader.FindEntry(R"(script\da0101.nps)")->payload, expected_v1_da0101);
    EXPECT_EQ(read_file((game_dir / "script" / "da0100.nps").u8string()), expected_v1_da0100);
    EXPECT_EQ(read_file((game_dir / "script.pak").u8string()), retail_script_pak);
    EXPECT_EQ(read_file((game_dir / "0script.pak.bak").u8string()), preexisting_0script);
    EXPECT_EQ(read_file((game_dir / "script" / "da0100.nps.bak").u8string()),
              preexisting_da0100);
    // da0101.nps did not exist prior to Pass 1, so no .bak was created for it yet.
    EXPECT_FALSE(fs::exists(game_dir / "script" / "da0101.nps.bak"));
    EXPECT_TRUE(FindFilesWithExtension(game_dir, L".tmp").empty());

    // Pass 2: deploy v2 -> targets update while existing .bak files stay immutable.
    SeedSyntheticProject(project_dir, "Second pass translation A.",
                         "Second pass translation B.");
    patch::deploy_script_artifacts(project_dir.u8string(), game_dir.u8string());

    const Bytes expected_v2_da0100 =
        Utf8ToCp932("<BOX type=\"0\">\r　Second pass translation A.<k>\r</BOX>");
    const Bytes expected_v2_da0101 =
        Utf8ToCp932("<BOX type=\"1\">\r　Second pass translation B.<k>\r</BOX>");
    const PakArchiveReader v2_reader(read_file((game_dir / "0script.pak").u8string()));
    ASSERT_NE(v2_reader.FindEntry(R"(script\da0100.nps)"), nullptr);
    ASSERT_NE(v2_reader.FindEntry(R"(script\da0101.nps)"), nullptr);
    EXPECT_EQ(v2_reader.FindEntry(R"(script\da0100.nps)")->payload, expected_v2_da0100);
    EXPECT_EQ(v2_reader.FindEntry(R"(script\da0101.nps)")->payload, expected_v2_da0101);
    EXPECT_EQ(read_file((game_dir / "script" / "da0100.nps").u8string()), expected_v2_da0100);
    EXPECT_EQ(read_file((game_dir / "0script.pak.bak").u8string()), preexisting_0script);
    EXPECT_EQ(read_file((game_dir / "script" / "da0100.nps.bak").u8string()),
              preexisting_da0100);
    EXPECT_EQ(read_file((game_dir / "script.pak").u8string()), retail_script_pak);
    EXPECT_TRUE(FindFilesWithExtension(game_dir, L".tmp").empty());
}

TEST(PakDeployCt, StagedCopyDeploySkipsStrayBakTmpHandlesReadOnlyAndRollsBackOnFault) {
    const ScopedSandboxDir sandbox("ksh_ct_staged_copy");
    const fs::path dist_game = sandbox.path() / "dist_game";
    const fs::path target_game = sandbox.path() / "target_game";
    fs::create_directories(dist_game / "script");
    fs::create_directories(target_game / "script");

    const Bytes dist_pak{0x00u, 0x7Fu, 0x80u, 0xFFu, 0xAAu};
    const Bytes dist_nps{0x11u, 0x22u, 0x33u, 0x44u};
    write_file((dist_game / "0script.pak").u8string(), dist_pak);
    write_file((dist_game / "script" / "da0100.nps").u8string(), dist_nps);
    // Stray .bak and .tmp files in dist_game/script/ must never be copied.
    write_file((dist_game / "script" / "stray.nps.bak").u8string(), Bytes{0x99u});
    write_file((dist_game / "script" / "stray.nps.tmp").u8string(), Bytes{0x88u});

    const Bytes orig_target_pak{0x01u, 0x02u, 0x03u};
    write_file((target_game / "0script.pak").u8string(), orig_target_pak);
    ASSERT_NE(::SetFileAttributesW((target_game / "0script.pak").c_str(),
                                   FILE_ATTRIBUTE_READONLY),
              0);

    patch::deploy_script_artifacts(/*project_dir=*/"", target_game.u8string(),
                                   dist_game.u8string(), /*backup_existing=*/true);

    EXPECT_EQ(read_file((target_game / "0script.pak").u8string()), dist_pak);
    EXPECT_EQ(read_file((target_game / "0script.pak.bak").u8string()), orig_target_pak);
    EXPECT_EQ(read_file((target_game / "script" / "da0100.nps").u8string()), dist_nps);
    EXPECT_FALSE(fs::exists(target_game / "script" / "stray.nps.bak"));
    EXPECT_FALSE(fs::exists(target_game / "script" / "stray.nps.tmp"));
    EXPECT_TRUE(FindFilesWithExtension(target_game, L".tmp").empty());

    // Mid-batch fault rollback: create a directory at `script\da0101.nps.tmp`
    // so staging item #2 fails in Phase 1. Pre-existing `0script.pak` and
    // `da0100.nps` must stay untouched with zero leftover `.tmp` regular files.
    const ScopedSandboxDir fault_sandbox("ksh_ct_fault_rollback");
    const fs::path project_dir = fault_sandbox.path() / "project";
    const fs::path fault_game = fault_sandbox.path() / "game";
    fs::create_directories(fault_game / "script" / "da0101.nps.tmp");
    const Bytes pre_pak{0xFEu, 0xEDu, 0xFAu, 0xCEu};
    const Bytes pre_nps{0xCAu, 0xFEu, 0xBAu, 0xBEu};
    write_file((fault_game / "0script.pak").u8string(), pre_pak);
    write_file((fault_game / "script" / "da0100.nps").u8string(), pre_nps);

    SeedSyntheticProject(project_dir, "Fault rollback line A.",
                         "Fault rollback line B.");
    const NpsRebuilder rebuilder;
    EXPECT_THROW((void)rebuilder.RebuildAndDeploy(project_dir.u8string(),
                                                  {fault_game.u8string()}, true,
                                                  /*backup_existing=*/true),
                 std::runtime_error);

    EXPECT_EQ(read_file((fault_game / "0script.pak").u8string()), pre_pak);
    EXPECT_EQ(read_file((fault_game / "script" / "da0100.nps").u8string()), pre_nps);
    EXPECT_FALSE(fs::exists(fault_game / "0script.pak.bak"));
    EXPECT_FALSE(fs::exists(fault_game / "script" / "da0100.nps.bak"));
    EXPECT_TRUE(FindFilesWithExtension(fault_game, L".tmp").empty());
}
