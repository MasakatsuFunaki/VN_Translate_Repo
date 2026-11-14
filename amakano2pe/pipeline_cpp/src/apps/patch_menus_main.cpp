// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_patch_menus -- render translated menu/UI .hg3 graphics and non-graphic
// UI text overrides, verify HG-3 frame metadata parity and zero raw asset
// signatures, and pack the keyless "AMK2MENU" v1 XOR-delta bundle
// (menu_patch.bin).
//
// Usage:
//   03_patch_menus --game-dir <install>
//   03_patch_menus --game-dir <install> --out <menu_patch.bin>
//   03_patch_menus --game-dir <install> --contact-sheets <dir>
#include "apps/paths.h"
#include "common/util.h"
#include "hg3/hg3_codec.h"
#include "menu/core/menu_patch_container.h"
#include "menu/domain_renderer.h"
#include "menu/render_config.h"
#include "menu/render_extra_eyecatch.h"
#include "menu/render_hud_saveload.h"
#include "menu/render_map_graphs.h"
#include "menu/render_story_items.h"
#include "menu/render_title_dialogs.h"
#include "menu/ui_text_builders.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;
constexpr std::string_view kDefaultPatchBundleName = "menu_patch.bin";
constexpr std::string_view kHg3Extension = ".hg3";

bool EndsWithIgnoreCase(std::string_view str, std::string_view suffix) noexcept {
    if (str.size() < suffix.size()) {
        return false;
    }
    const std::size_t off = str.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto a = static_cast<unsigned char>(str[off + i]);
        const auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::vector<std::unique_ptr<ama::menu::IMenuDomainRenderer>> CreateAllMenuDomainRenderers() {
    std::vector<std::unique_ptr<ama::menu::IMenuDomainRenderer>> list;
    list.push_back(ama::menu::CreateConfigDomainRenderer());
    list.push_back(ama::menu::CreateHudSaveLoadDomainRenderer());
    list.push_back(ama::menu::CreateTitleDialogsDomainRenderer());
    list.push_back(ama::menu::CreateMapGraphsDomainRenderer());
    list.push_back(ama::menu::CreateExtraEyecatchDomainRenderer());
    list.push_back(ama::menu::CreateStoryItemsDomainRenderer());
    list.push_back(ama::menu::CreateUiTextPayloadsRenderer());
    return list;
}

}  // namespace

int main() {
    ama::setup_console_utf8();
    namespace po = ama::apps::po;

    try {
        std::string project;
        std::string game;
        std::string out_path;
        std::string contact_sheets_dir;

        po::options_description desc(
            "03_patch_menus -- build menu_patch.bin (AMK2MENU v1 keyless XOR-delta)");
        desc.add(ama::apps::common_options(project, game));
        desc.add_options()
            ("out", po::value(&out_path),
             "output path for menu_patch.bin (default <dir>\\menu_patch.bin)")
            ("contact-sheets", po::value(&contact_sheets_dir),
             "optional output directory for rendered PNG contact sheets");

        po::variables_map vm;
        if (const auto rc = ama::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path game_dir = fs::u8path(game);
        const fs::path bundle_path =
            out_path.empty()
                ? (fs::u8path(project) / fs::u8path(kDefaultPatchBundleName))
                : fs::u8path(out_path);

        ama::menu::DomainRenderOptions options;
        if (!contact_sheets_dir.empty()) {
            options.contact_sheet_dir = fs::u8path(contact_sheets_dir);
            options.export_contact_sheets = true;
            fs::create_directories(options.contact_sheet_dir);
        }

        ama::menu::GameAssetSource source(game_dir);
        const auto renderers = CreateAllMenuDomainRenderers();
        ama::menu::MenuPatchBuilder builder;
        std::size_t hg3_count = 0;
        std::size_t non_graphic_count = 0;

        for (const auto& renderer : renderers) {
            ama::log_info("[03_patch_menus] Rendering domain '" +
                          renderer->DomainName() + "'...");
            std::vector<ama::menu::MenuPatchEntrySpec> records =
                renderer->RenderDomain(source, options);
            ama::log_info("[03_patch_menus] Domain '" + renderer->DomainName() +
                          "' produced " + std::to_string(records.size()) +
                          " record(s).");

            for (const auto& rec : records) {
                builder.AddRecord(rec);
                if (!EndsWithIgnoreCase(rec.target_rel_path, kHg3Extension)) {
                    ++non_graphic_count;
                    continue;
                }
                const std::string filename =
                    fs::u8path(rec.target_rel_path).filename().u8string();
                const ama::hg3::Hg3Container orig = source.LoadHg3(filename);
                const ama::hg3::Hg3Container patched =
                    ama::hg3::Hg3Codec::Decode(rec.new_raw);
                if (!ama::hg3::Hg3Codec::VerifyFrameMetadataParity(orig, patched)) {
                    throw std::runtime_error(
                        "HG-3 stdinfo/frame metadata parity check failed for " +
                        rec.target_rel_path);
                }
                if (options.export_contact_sheets) {
                    ama::menu::ExportHg3ContactSheetPages(
                        filename, patched, options.contact_sheet_dir);
                }
                ++hg3_count;
            }
        }

        const std::vector<std::uint8_t> packed = builder.BuildBytes();
        if (!ama::menu::VerifyNoRawAssetSignatures(packed)) {
            throw std::runtime_error(
                "raw asset signature detected in serialized menu_patch.bin");
        }
        builder.WriteToFile(bundle_path);

        ama::log_info("[03_patch_menus] Wrote " + bundle_path.u8string() + " (" +
                      std::to_string(packed.size()) + " bytes, " +
                      std::to_string(builder.record_count()) + " records: " +
                      std::to_string(hg3_count) + " HG-3, " +
                      std::to_string(non_graphic_count) + " UI text/script).");
        return kExitOk;
    } catch (const std::exception& e) {
        ama::log_info(std::string("[ERROR] ") + e.what());
        return ama::apps::kExitError;
    }
}
