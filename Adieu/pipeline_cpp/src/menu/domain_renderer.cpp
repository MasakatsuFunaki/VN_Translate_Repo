// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/domain_renderer.h"

#include "common/util.h"
#include "imaging/contact_sheet.h"
#include "imaging/wic_image_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <boost/json.hpp>

namespace adi::menu {
namespace {

namespace fs = std::filesystem;

constexpr int kCutCropContextPadPx = 6;
constexpr int kCutIndexDigits = 3;

std::string ToUpperAscii(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0u; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool IsUiCandidateStem(std::string_view stem) {
    if (stem.empty()) {
        return false;
    }
    if (stem.front() == '_') {
        return true;
    }
    static constexpr std::array<std::string_view, 28> kUiPrefixes = {
        "ME_",   "MENU",  "MESS", "SYS",   "SL_",  "SAVE", "LOAD",
        "CFG",   "CONF",  "LOG",  "BACK",  "TIT",  "EXTR", "CG",
        "BGM",   "MOV",   "END",  "ASK",   "DLG",  "WIN",  "BTN",
        "CHK",   "BAR",   "NUM",  "FONT",  "BLAC", "WHIT", "OPT",
    };
    const std::string upper = ToUpperAscii(stem);
    for (const std::string_view prefix : kUiPrefixes) {
        if (upper.rfind(prefix, 0u) == 0u) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& file_path) {
    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("Failed to open asset file: " + file_path.u8string());
    }
    const std::streamsize size = ifs.tellg();
    if (size < 0) {
        throw std::runtime_error("Failed to query size of asset file: " + file_path.u8string());
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    ifs.seekg(0, std::ios::beg);
    if (size > 0 && !ifs.read(reinterpret_cast<char*>(buf.data()), size)) {
        throw std::runtime_error("Failed reading asset file: " + file_path.u8string());
    }
    return buf;
}

fs::path RelativeAssetToFsPath(const fs::path& game_dir, std::string_view rel_path) {
    if (!IsSafeRelativeAssetPath(rel_path)) {
        throw std::invalid_argument("Unsafe relative asset path: " + std::string(rel_path));
    }
    std::string normalized(rel_path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return game_dir / fs::u8path(normalized);
}

std::string NormalizeG00RelPath(std::string_view g00_filename) {
    std::string rel(g00_filename);
    if (rel.rfind("g00\\", 0u) != 0u && rel.rfind("G00\\", 0u) != 0u) {
        rel = "g00\\" + rel;
    }
    if (rel.size() < 4u || !EqualsIgnoreCaseAscii(std::string_view(rel).substr(rel.size() - 4u), ".g00")) {
        rel += ".g00";
    }
    if (!IsSafeRelativeAssetPath(rel)) {
        throw std::invalid_argument("Invalid .g00 relative path: " + rel);
    }
    return rel;
}

bool LooksLikeGameDataDir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    const bool has_exe = fs::is_regular_file(dir / "SiglusEngine.exe", ec);
    const bool has_dat = fs::is_regular_file(dir / "Gameexe.dat", ec) ||
                         fs::is_regular_file(dir / "Gameexe.dat.bak", ec);
    const bool has_g00 = fs::is_directory(dir / "g00", ec);
    return has_exe && has_dat && has_g00;
}

imaging::BgraImage ExtractCutPreview(const g00::G00Image& img, const g00::G00Cut& cut) {
    if (cut.is_empty || cut.canvas.empty()) {
        return imaging::BgraImage(1, 1, imaging::BgraColor{0, 0, 0, 0});
    }
    const imaging::RectI alpha_box = cut.canvas.NonZeroAlphaBounds();
    if (!alpha_box.empty()) {
        const imaging::RectI padded = imaging::ClampRect(
            imaging::RectI{
                alpha_box.x - kCutCropContextPadPx,
                alpha_box.y - kCutCropContextPadPx,
                alpha_box.w + 2 * kCutCropContextPadPx,
                alpha_box.h + 2 * kCutCropContextPadPx,
            },
            img.width, img.height);
        if (!padded.empty()) {
            return cut.canvas.Crop(padded);
        }
    }
    const int desc_w = cut.descriptor.x2 - cut.descriptor.x1 + 1;
    const int desc_h = cut.descriptor.y2 - cut.descriptor.y1 + 1;
    if (desc_w > 0 && desc_h > 0) {
        const imaging::RectI desc_rect = imaging::ClampRect(
            imaging::RectI{cut.descriptor.x1, cut.descriptor.y1, desc_w, desc_h}, img.width,
            img.height);
        if (!desc_rect.empty()) {
            return cut.canvas.Crop(desc_rect);
        }
    }
    return cut.canvas;
}

}  // namespace

fs::path ResolveRetailGameDataDir(const fs::path& input_game_dir) {
    if (input_game_dir.empty()) {
        throw std::runtime_error("Game directory path is empty");
    }
    const std::array<fs::path, 3> candidates = {
        input_game_dir,
        input_game_dir / "StartData" / "GameData",
        input_game_dir / "GameData",
    };
    for (const fs::path& cand : candidates) {
        if (LooksLikeGameDataDir(cand)) {
            return cand;
        }
    }
    throw std::runtime_error(
        "Could not locate SiglusEngine.exe, Gameexe.dat, and g00\\ under: " +
        input_game_dir.u8string());
}

struct PatchBuildContext::Impl {
    fs::path game_dir;
    fs::path contact_sheet_dir;
    mutable std::optional<g00::GameexeKey256> cached_key;
    MenuPatchBuilder builder;
    imaging::ContactSheetBuilder sheet_builder;

    Impl(fs::path resolved_dir, fs::path sheet_dir)
        : game_dir(std::move(resolved_dir)), contact_sheet_dir(std::move(sheet_dir)) {}
};

PatchBuildContext::PatchBuildContext(fs::path resolved_game_dir, fs::path contact_sheet_dir)
    : impl_(std::make_unique<Impl>(ResolveRetailGameDataDir(resolved_game_dir),
                                   std::move(contact_sheet_dir))) {}

PatchBuildContext::PatchBuildContext(PatchBuildContext&& other) noexcept = default;
PatchBuildContext& PatchBuildContext::operator=(PatchBuildContext&& other) noexcept = default;
PatchBuildContext::~PatchBuildContext() = default;

const fs::path& PatchBuildContext::game_dir() const noexcept { return impl_->game_dir; }

bool PatchBuildContext::HasAssetFile(std::string_view rel_path) const {
    if (!IsSafeRelativeAssetPath(rel_path)) {
        return false;
    }
    const fs::path live_path = RelativeAssetToFsPath(impl_->game_dir, rel_path);
    const fs::path bak_path = live_path.wstring() + L".bak";
    std::error_code ec;
    return fs::is_regular_file(bak_path, ec) || fs::is_regular_file(live_path, ec);
}

std::vector<std::uint8_t> PatchBuildContext::ReadPristineAssetBytes(
    std::string_view rel_path) const {
    const fs::path live_path = RelativeAssetToFsPath(impl_->game_dir, rel_path);
    const fs::path bak_path = live_path.wstring() + L".bak";
    std::error_code ec;
    if (fs::is_regular_file(bak_path, ec) && fs::file_size(bak_path, ec) >= kMinOrigSliceBytes) {
        return ReadBinaryFile(bak_path);
    }
    return ReadBinaryFile(live_path);
}

const g00::GameexeKey256& PatchBuildContext::GameexeKey() const {
    if (!impl_->cached_key.has_value()) {
        impl_->cached_key = g00::LoadGameexeKeyFromExe(impl_->game_dir / "SiglusEngine.exe");
    }
    return *impl_->cached_key;
}

g00::G00Image PatchBuildContext::LoadG00(std::string_view g00_filename) const {
    const std::string rel = NormalizeG00RelPath(g00_filename);
    const std::vector<std::uint8_t> raw = ReadPristineAssetBytes(rel);
    return g00::G00Codec::Decode(raw);
}

g00::GameexeDocument PatchBuildContext::LoadGameexeDocument() const {
    const std::vector<std::uint8_t> raw = ReadPristineAssetBytes("Gameexe.dat");
    const std::u16string utf16 = g00::DecodeGameexe(raw, GameexeKey());
    return g00::GameexeDocument::FromUtf16(utf16);
}

void PatchBuildContext::SubmitPatchedG00(std::string_view g00_filename,
                                         const g00::G00Image& patched_image,
                                         const std::set<std::size_t>& modified_cuts) {
    const std::string rel = NormalizeG00RelPath(g00_filename);
    std::vector<std::uint8_t> orig_raw = ReadPristineAssetBytes(rel);

    std::set<std::size_t> effective_cuts = modified_cuts;
    if (effective_cuts.empty() && patched_image.type == g00::G00Type::kType2MultiCutBgra32) {
        for (std::size_t i = 0u; i < patched_image.cuts.size(); ++i) {
            if (patched_image.cuts[i].modified) {
                effective_cuts.insert(i);
            }
        }
    }

    std::vector<std::uint8_t> patched_raw = g00::G00Codec::Encode(patched_image);
    g00::G00Codec::AssertMetadataParity(orig_raw, patched_raw, effective_cuts);

    if (!impl_->contact_sheet_dir.empty()) {
        const g00::G00Image orig_img = g00::G00Codec::Decode(orig_raw);
        const std::string stem = fs::u8path(rel).stem().u8string();
        if (patched_image.type == g00::G00Type::kType2MultiCutBgra32) {
            for (const std::size_t cut_idx : effective_cuts) {
                if (cut_idx >= orig_img.cuts.size() || cut_idx >= patched_image.cuts.size()) {
                    continue;
                }
                const imaging::RectI orig_box =
                    orig_img.cuts[cut_idx].canvas.NonZeroAlphaBounds();
                const imaging::RectI patch_box =
                    patched_image.cuts[cut_idx].canvas.NonZeroAlphaBounds();
                const imaging::RectI union_box = imaging::UnionRect(orig_box, patch_box);
                const imaging::RectI crop_box = imaging::ClampRect(
                    imaging::RectI{
                        union_box.x - kCutCropContextPadPx,
                        union_box.y - kCutCropContextPadPx,
                        union_box.w + 2 * kCutCropContextPadPx,
                        union_box.h + 2 * kCutCropContextPadPx,
                    },
                    patched_image.width, patched_image.height);

                const imaging::BgraImage orig_crop =
                    crop_box.empty() ? ExtractCutPreview(orig_img, orig_img.cuts[cut_idx])
                                     : orig_img.cuts[cut_idx].canvas.Crop(crop_box);
                const imaging::BgraImage patch_crop =
                    crop_box.empty()
                        ? ExtractCutPreview(patched_image, patched_image.cuts[cut_idx])
                        : patched_image.cuts[cut_idx].canvas.Crop(crop_box);

                std::ostringstream label;
                label << stem << " cut[" << std::setw(kCutIndexDigits) << std::setfill('0')
                      << cut_idx << "]";
                impl_->sheet_builder.AddComparison(label.str(), orig_crop, patch_crop);
            }
        } else {
            impl_->sheet_builder.AddComparison(stem, orig_img.canvas, patched_image.canvas);
        }
    }

    impl_->builder.AddRecord(MenuPatchRecordInput{
        rel,
        rel,
        0u,
        std::move(orig_raw),
        std::move(patched_raw),
        kFlagBackupStandalone,
    });
}

void PatchBuildContext::SubmitPatchedGameexe(const g00::GameexeDocument& patched_doc) {
    std::vector<std::uint8_t> orig_raw = ReadPristineAssetBytes("Gameexe.dat");
    const std::u16string orig_utf16 = g00::DecodeGameexe(orig_raw, GameexeKey());
    const g00::GameexeDocument orig_doc = g00::GameexeDocument::FromUtf16(orig_utf16);

    g00::VerifyCriticalDirectivesUnchanged(orig_doc.ToUtf8(false), patched_doc.ToUtf8(false));

    const std::u16string patched_utf16 = patched_doc.ToUtf16();
    std::vector<std::uint8_t> patched_raw = g00::EncodeGameexe(patched_utf16, GameexeKey());
    if (g00::DecodeGameexe(patched_raw, GameexeKey()) != patched_utf16) {
        throw std::runtime_error("SubmitPatchedGameexe round-trip verification failed");
    }

    impl_->builder.AddRecord(MenuPatchRecordInput{
        "Gameexe.dat",
        "Gameexe.dat",
        0u,
        std::move(orig_raw),
        std::move(patched_raw),
        kFlagBackupStandalone,
    });
}

void PatchBuildContext::SubmitRawRecord(MenuPatchRecordInput record) {
    impl_->builder.AddRecord(std::move(record));
}

std::size_t PatchBuildContext::submitted_record_count() const noexcept {
    return impl_->builder.record_count();
}

MenuPatchBuildSummary PatchBuildContext::FinalizeAndWrite(const fs::path& output_patch_path,
                                                          std::size_t renderer_count) const {
    if (impl_->builder.record_count() == 0u) {
        throw std::runtime_error(
            "MenuPatchPipeline produced 0 patch records (no domain renderers submitted assets)");
    }

    const std::vector<std::uint8_t> blob = impl_->builder.BuildBytes();
    const MenuPatchManifest manifest = ParseMenuPatchContainer(blob);
    impl_->builder.WriteToFile(output_patch_path);

    MenuPatchBuildSummary summary;
    summary.renderer_count = renderer_count;
    summary.record_count = manifest.records.size();
    summary.patch_bytes = blob.size();
    summary.manifest_hash = manifest.manifest_hash;

    if (!impl_->contact_sheet_dir.empty() && !impl_->sheet_builder.empty()) {
        summary.contact_sheet_pages =
            impl_->sheet_builder.SavePages(impl_->contact_sheet_dir, "adieu_menu_qa");
    }
    return summary;
}

IMenuDomainRenderer::~IMenuDomainRenderer() = default;

struct MenuPatchPipeline::Impl {
    std::vector<std::unique_ptr<IMenuDomainRenderer>> renderers;
};

MenuPatchPipeline::MenuPatchPipeline() : impl_(std::make_unique<Impl>()) {}

MenuPatchPipeline::MenuPatchPipeline(std::vector<std::unique_ptr<IMenuDomainRenderer>> renderers)
    : impl_(std::make_unique<Impl>()) {
    for (auto& renderer : renderers) {
        RegisterRenderer(std::move(renderer));
    }
}

MenuPatchPipeline::MenuPatchPipeline(MenuPatchPipeline&& other) noexcept = default;
MenuPatchPipeline& MenuPatchPipeline::operator=(MenuPatchPipeline&& other) noexcept = default;
MenuPatchPipeline::~MenuPatchPipeline() = default;

void MenuPatchPipeline::RegisterRenderer(std::unique_ptr<IMenuDomainRenderer> renderer) {
    if (!renderer) {
        throw std::invalid_argument("MenuPatchPipeline::RegisterRenderer received null renderer");
    }
    impl_->renderers.push_back(std::move(renderer));
}

std::size_t MenuPatchPipeline::renderer_count() const noexcept { return impl_->renderers.size(); }

MenuPatchBuildSummary MenuPatchPipeline::Run(const MenuPatchBuildOptions& options) const {
    if (options.output_patch_path.empty()) {
        throw std::invalid_argument("MenuPatchBuildOptions::output_patch_path must be non-empty");
    }
    PatchBuildContext ctx(options.game_dir, options.contact_sheet_dir);
    for (const auto& renderer : impl_->renderers) {
        log_info("Applying menu renderer domain: " + renderer->DomainName());
        renderer->Apply(ctx);
    }
    return ctx.FinalizeAndWrite(options.output_patch_path, impl_->renderers.size());
}

UiDumpSummary DumpUiAssets(const UiDumpOptions& options) {
    if (options.dump_dir.empty()) {
        throw std::invalid_argument("UiDumpOptions::dump_dir must be non-empty");
    }
    const fs::path game_dir = ResolveRetailGameDataDir(options.game_dir);
    fs::create_directories(options.dump_dir);

    PatchBuildContext ctx(game_dir);
    const g00::GameexeDocument gameexe_doc = ctx.LoadGameexeDocument();

    UiDumpSummary summary;
    summary.gameexe_txt_path = options.dump_dir / "Gameexe.txt";
    write_file_text(summary.gameexe_txt_path.u8string(), gameexe_doc.ToUtf8(false));

    std::string wanted_file = options.only_filename;
    if (!wanted_file.empty() &&
        (wanted_file.size() < 4u ||
         !EqualsIgnoreCaseAscii(std::string_view(wanted_file).substr(wanted_file.size() - 4u),
                                ".g00"))) {
        wanted_file += ".g00";
    }

    std::vector<fs::path> g00_files;
    for (const fs::directory_entry& entry : fs::directory_iterator(game_dir / "g00")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path p = entry.path();
        if (!EqualsIgnoreCaseAscii(p.extension().u8string(), ".g00")) {
            continue;
        }
        const std::string filename = p.filename().u8string();
        if (!wanted_file.empty()) {
            if (EqualsIgnoreCaseAscii(filename, wanted_file)) {
                g00_files.push_back(p);
            }
            continue;
        }
        if (options.include_non_ui_g00 || IsUiCandidateStem(p.stem().u8string())) {
            g00_files.push_back(p);
        }
    }

    if (!wanted_file.empty() && g00_files.empty()) {
        throw std::runtime_error("Requested --only .g00 asset not found: " + wanted_file);
    }

    std::sort(g00_files.begin(), g00_files.end());
    const imaging::WicImageIo wic;

    for (const fs::path& g00_path : g00_files) {
        const std::string filename = g00_path.filename().u8string();
        const std::string stem = g00_path.stem().u8string();
        const g00::G00Image decoded = ctx.LoadG00(filename);

        boost::json::object meta;
        meta["filename"] = filename;
        meta["type"] = static_cast<int>(decoded.type);
        meta["width"] = decoded.width;
        meta["height"] = decoded.height;
        meta["cut_count"] = decoded.cuts.size();

        if (decoded.type == g00::G00Type::kType2MultiCutBgra32) {
            boost::json::array cuts_json;
            std::vector<imaging::ContactSheetCell> grid_cells;

            for (std::size_t i = 0u; i < decoded.cuts.size(); ++i) {
                const g00::G00Cut& cut = decoded.cuts[i];
                boost::json::object cj;
                cj["index"] = i;
                cj["is_empty"] = cut.is_empty;
                cj["x1"] = cut.descriptor.x1;
                cj["y1"] = cut.descriptor.y1;
                cj["x2"] = cut.descriptor.x2;
                cj["y2"] = cut.descriptor.y2;
                cj["origin_x"] = cut.descriptor.origin_x;
                cj["origin_y"] = cut.descriptor.origin_y;
                cj["bbox_x"] = cut.cut_header.bbox_x;
                cj["bbox_y"] = cut.cut_header.bbox_y;
                cj["bbox_w"] = cut.cut_header.bbox_w;
                cj["bbox_h"] = cut.cut_header.bbox_h;
                cj["center_x"] = cut.cut_header.center_x;
                cj["center_y"] = cut.cut_header.center_y;
                cj["cut_canvas_w"] = cut.cut_header.cut_canvas_w;
                cj["cut_canvas_h"] = cut.cut_header.cut_canvas_h;
                cj["part_count"] = cut.part_headers.size();
                cuts_json.push_back(std::move(cj));

                if (cut.is_empty || cut.canvas.empty()) {
                    continue;
                }

                std::ostringstream cut_name;
                cut_name << stem << "_cut" << std::setw(kCutIndexDigits) << std::setfill('0') << i
                         << ".png";
                wic.SavePngFile(options.dump_dir / cut_name.str(), cut.canvas);
                ++summary.png_images_written;

                std::ostringstream cell_label;
                cell_label << "cut[" << std::setw(kCutIndexDigits) << std::setfill('0') << i
                           << "] (" << cut.descriptor.x1 << "," << cut.descriptor.y1 << ".."
                           << cut.descriptor.x2 << "," << cut.descriptor.y2 << ")";
                grid_cells.push_back(imaging::ContactSheetCell{
                    cell_label.str(),
                    ExtractCutPreview(decoded, cut),
                });
            }
            meta["cuts"] = std::move(cuts_json);

            if (!grid_cells.empty()) {
                const auto pages = imaging::SaveContactSheetGridPages(
                    grid_cells, options.dump_dir, stem + "_sheet");
                summary.png_images_written += pages.size();
            }
        } else {
            wic.SavePngFile(options.dump_dir / (stem + ".png"), decoded.canvas);
            ++summary.png_images_written;
        }

        const fs::path json_path = options.dump_dir / (stem + ".json");
        write_file_text(json_path.u8string(), json_pretty(meta));
        ++summary.g00_files_dumped;
    }

    return summary;
}

}  // namespace adi::menu
