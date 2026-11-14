// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pure protocol interface (`IMenuDomainRenderer`), PIMPL-insulated build
// context (`PatchBuildContext`), pipeline orchestrator (`MenuPatchPipeline`),
// and developer UI dump utility (`DumpUiAssets`) for 04_patch_menus.
//
// Thread-compatible.
#pragma once

#include "g00/g00_codec.h"
#include "g00/gameexe_codec.h"
#include "menu/core/menu_patch_container.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace adi::menu {

struct MenuPatchBuildOptions {
    std::filesystem::path game_dir;
    std::filesystem::path output_patch_path;
    std::filesystem::path contact_sheet_dir;
};

struct MenuPatchBuildSummary {
    std::size_t renderer_count = 0u;
    std::size_t record_count = 0u;
    std::size_t patch_bytes = 0u;
    std::uint32_t manifest_hash = 0u;
    std::vector<std::filesystem::path> contact_sheet_pages;
};

struct UiDumpOptions {
    std::filesystem::path game_dir;
    std::filesystem::path dump_dir;
    std::string only_filename;
    bool include_non_ui_g00 = false;
};

struct UiDumpSummary {
    std::filesystem::path gameexe_txt_path;
    std::size_t g00_files_dumped = 0u;
    std::size_t png_images_written = 0u;
};

// Resolves `input_game_dir` to the folder holding `Gameexe.dat`,
// `SiglusEngine.exe`, and `g00\` (checking `StartData\GameData`, `GameData`,
// and `.`). Throws std::runtime_error if not found.
std::filesystem::path ResolveRetailGameDataDir(
    const std::filesystem::path& input_game_dir);

// PIMPL-insulated asset loader, parity validator, contact-sheet collector, and
// XOR-delta record sink passed to each `IMenuDomainRenderer`.
class PatchBuildContext {
public:
    PatchBuildContext(std::filesystem::path resolved_game_dir,
                      std::filesystem::path contact_sheet_dir = {});
    PatchBuildContext(const PatchBuildContext&) = delete;
    PatchBuildContext& operator=(const PatchBuildContext&) = delete;
    PatchBuildContext(PatchBuildContext&& other) noexcept;
    PatchBuildContext& operator=(PatchBuildContext&& other) noexcept;
    ~PatchBuildContext();

    // Validates `patched_image` against the original `g00\<g00_filename>` via
    // `G00Codec::AssertMetadataParity`, appends comparison rows when contact
    // sheets are enabled, and queues the `"ADIEUMEN"` v1 XOR-delta record.
    void SubmitPatchedG00(std::string_view g00_filename,
                          const g00::G00Image& patched_image,
                          const std::set<std::size_t>& modified_cuts = {});

    // Validates `patched_doc` via `VerifyCriticalDirectivesUnchanged`, encodes
    // `Gameexe.dat`, and queues the `"ADIEUMEN"` v1 XOR-delta record.
    void SubmitPatchedGameexe(const g00::GameexeDocument& patched_doc);

    void SubmitRawRecord(MenuPatchRecordInput record);

    const std::filesystem::path& game_dir() const noexcept;
    bool HasAssetFile(std::string_view rel_path) const;

    // Reads pristine retail bytes for `rel_path` (preferring `<rel_path>.bak`
    // when present so re-baking against a patched install stays idempotent).
    std::vector<std::uint8_t> ReadPristineAssetBytes(
        std::string_view rel_path) const;

    const g00::GameexeKey256& GameexeKey() const;
    g00::G00Image LoadG00(std::string_view g00_filename) const;
    g00::GameexeDocument LoadGameexeDocument() const;

    std::size_t submitted_record_count() const noexcept;

    MenuPatchBuildSummary FinalizeAndWrite(
        const std::filesystem::path& output_patch_path,
        std::size_t renderer_count) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pure protocol class implemented by each screen-domain renderer.
class IMenuDomainRenderer {
public:
    virtual ~IMenuDomainRenderer();

    virtual std::string DomainName() const = 0;
    virtual void Apply(PatchBuildContext& ctx) = 0;
};

class MenuPatchPipeline {
public:
    MenuPatchPipeline();
    explicit MenuPatchPipeline(
        std::vector<std::unique_ptr<IMenuDomainRenderer>> renderers);
    MenuPatchPipeline(const MenuPatchPipeline&) = delete;
    MenuPatchPipeline& operator=(const MenuPatchPipeline&) = delete;
    MenuPatchPipeline(MenuPatchPipeline&& other) noexcept;
    MenuPatchPipeline& operator=(MenuPatchPipeline&& other) noexcept;
    ~MenuPatchPipeline();

    void RegisterRenderer(std::unique_ptr<IMenuDomainRenderer> renderer);
    std::size_t renderer_count() const noexcept;

    MenuPatchBuildSummary Run(const MenuPatchBuildOptions& options) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Unpacks candidate UI `.g00` files (or `--only <Filename.g00>`) as `.png` +
// `.json` metadata plus decrypted UTF-8 `Gameexe.txt` into `options.dump_dir`.
UiDumpSummary DumpUiAssets(const UiDumpOptions& options);

}  // namespace adi::menu
