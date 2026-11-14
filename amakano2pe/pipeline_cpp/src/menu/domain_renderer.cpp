// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/domain_renderer.h"

#include "cs2/cs2_archive.h"
#include "imaging/contact_sheet.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace ama::menu {

namespace {

constexpr std::uint32_t kDefaultSliceCapBytes = 4096;
constexpr int kContactSheetColumns = 4;
constexpr int kContactSheetMaxFramesPerPage = 32;
constexpr int kContactSheetMaxCellPx = 320;

std::string NormalizeLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string StripExtension(const std::string& filename) {
    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) {
        return filename;
    }
    return filename.substr(0, dot);
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open file: " + path.u8string());
    }
    const std::streamsize sz = in.tellg();
    if (sz < 0) {
        throw std::runtime_error("cannot stat file: " + path.u8string());
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    in.seekg(0, std::ios::beg);
    if (sz > 0) {
        in.read(reinterpret_cast<char*>(buf.data()), sz);
    }
    return buf;
}

}  // namespace

struct GameAssetSource::Impl {
    std::filesystem::path game_dir;
    ama::cs2::KifNameCipher cipher;
    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, std::unique_ptr<ama::cs2::KifArchive>> archives;

    explicit Impl(std::filesystem::path dir)
        : game_dir(std::move(dir)),
          cipher(ama::cs2::read_vcode2_from_exe((game_dir / "cs2.exe").u8string())) {}

    Impl(std::filesystem::path dir, const std::string& vcode2)
        : game_dir(std::move(dir)),
          cipher(vcode2) {}

    const ama::cs2::KifArchive& GetArchive(const std::string& archive_rel_path) const {
        const std::string key = NormalizeLower(archive_rel_path);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = archives.find(key);
        if (it != archives.end()) {
            return *it->second;
        }
        const std::filesystem::path full_path = game_dir / std::filesystem::u8path(archive_rel_path);
        const bool is_v2_scene = (key == "scene.int");
        auto arc = is_v2_scene
                       ? std::make_unique<ama::cs2::KifArchive>(full_path.u8string())
                       : std::make_unique<ama::cs2::KifArchive>(full_path.u8string(), cipher);
        const ama::cs2::KifArchive& ref = *arc;
        archives.emplace(key, std::move(arc));
        return ref;
    }
};

GameAssetSource::GameAssetSource(std::filesystem::path game_dir)
    : impl_(std::make_unique<Impl>(std::move(game_dir))) {}

GameAssetSource::GameAssetSource(std::filesystem::path game_dir,
                                 const std::string& vcode2)
    : impl_(std::make_unique<Impl>(std::move(game_dir), vcode2)) {}

GameAssetSource::~GameAssetSource() = default;
GameAssetSource::GameAssetSource(GameAssetSource&&) noexcept = default;
GameAssetSource& GameAssetSource::operator=(GameAssetSource&&) noexcept = default;

const std::filesystem::path& GameAssetSource::game_dir() const noexcept {
    return impl_->game_dir;
}

bool GameAssetSource::HasArchiveEntry(const std::string& archive_rel_path,
                                      const std::string& entry_name) const {
    const ama::cs2::KifArchive& arc = impl_->GetArchive(archive_rel_path);
    return arc.find(entry_name) != nullptr;
}

std::vector<std::string> GameAssetSource::ListArchiveEntries(
    const std::string& archive_rel_path) const {
    const ama::cs2::KifArchive& arc = impl_->GetArchive(archive_rel_path);
    std::vector<std::string> names;
    names.reserve(arc.entries().size());
    for (const auto& e : arc.entries()) {
        names.push_back(e.name);
    }
    return names;
}

ExtractedAssetEntry GameAssetSource::ReadArchiveEntry(const std::string& archive_rel_path,
                                                      const std::string& entry_name) const {
    const ama::cs2::KifArchive& arc = impl_->GetArchive(archive_rel_path);
    const ama::cs2::KifEntry* entry = arc.find(entry_name);
    if (entry == nullptr) {
        throw std::runtime_error("entry '" + entry_name + "' not found in " + archive_rel_path);
    }

    const std::filesystem::path archive_path =
        impl_->game_dir / std::filesystem::u8path(archive_rel_path);
    const std::uint32_t raw_slice_len = std::clamp<std::uint32_t>(
        entry->length, static_cast<std::uint32_t>(kMinOrigSliceBytes), kDefaultSliceCapBytes);

    ExtractedAssetEntry result;
    result.archive_rel_path = archive_rel_path;
    result.entry_name = entry->name;
    result.orig_offset = entry->offset;
    result.orig_raw_slice =
        ama::cs2::read_kif_raw_slice(archive_path.u8string(), entry->offset, raw_slice_len);
    result.plain_bytes = arc.extract(*entry);
    return result;
}

ExtractedAssetEntry GameAssetSource::ReadStandaloneFile(const std::string& rel_path) const {
    const std::filesystem::path full_path = impl_->game_dir / std::filesystem::u8path(rel_path);
    std::vector<std::uint8_t> bytes = ReadFileBytes(full_path);
    if (bytes.size() < kMinOrigSliceBytes) {
        throw std::runtime_error("standalone file smaller than minimum slice size: " + rel_path);
    }

    const std::size_t slice_len =
        std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(kDefaultSliceCapBytes));
    ExtractedAssetEntry result;
    result.archive_rel_path = rel_path;
    result.entry_name = std::filesystem::u8path(rel_path).filename().u8string();
    result.orig_offset = 0;
    result.orig_raw_slice.assign(bytes.begin(),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(slice_len));
    result.plain_bytes = std::move(bytes);
    return result;
}

ExtractedAssetEntry GameAssetSource::ReadExecutableSlice(std::uint64_t offset,
                                                         std::uint32_t length) const {
    const std::uint32_t safe_len =
        std::max<std::uint32_t>(length, static_cast<std::uint32_t>(kMinOrigSliceBytes));
    const std::filesystem::path exe_path = impl_->game_dir / "cs2.exe";
    ExtractedAssetEntry result;
    result.archive_rel_path = "cs2.exe";
    result.entry_name = "cs2.exe";
    result.orig_offset = offset;
    result.orig_raw_slice = ama::cs2::read_kif_raw_slice(exe_path.u8string(), offset, safe_len);
    result.plain_bytes = result.orig_raw_slice;
    return result;
}

ExtractedAssetEntry GameAssetSource::FindAndReadHg3(const std::string& hg3_filename) const {
    if (NormalizeLower(hg3_filename) == "boot.hg3") {
        return ReadStandaloneFile("boot.hg3");
    }
    if (HasArchiveEntry("image01.int", hg3_filename)) {
        return ReadArchiveEntry("image01.int", hg3_filename);
    }
    if (HasArchiveEntry("image00.int", hg3_filename)) {
        return ReadArchiveEntry("image00.int", hg3_filename);
    }
    throw std::runtime_error("HG-3 container not found in retail archives: " + hg3_filename);
}

ama::hg3::Hg3Container GameAssetSource::LoadHg3(const std::string& hg3_filename,
                                                ExtractedAssetEntry* out_source) const {
    ExtractedAssetEntry entry = FindAndReadHg3(hg3_filename);
    ama::hg3::Hg3Container decoded = ama::hg3::Hg3Codec::Decode(entry.plain_bytes);
    if (out_source != nullptr) {
        *out_source = std::move(entry);
    }
    return decoded;
}

MenuPatchEntrySpec GameAssetSource::BuildHg3PatchEntry(
    const ExtractedAssetEntry& source_entry, const ama::hg3::Hg3Container& patched_hg3) {
    std::vector<std::uint8_t> encoded = ama::hg3::Hg3Codec::Encode(patched_hg3);
    const bool is_standalone_boot = (NormalizeLower(source_entry.archive_rel_path) == "boot.hg3");
    const std::string target_rel =
        is_standalone_boot ? "boot.hg3" : ("image\\" + source_entry.entry_name);
    const std::uint8_t flags =
        is_standalone_boot ? kFlagBackupStandalone : kFlagLooseOverride;
    return BuildRawPatchEntry(source_entry, target_rel, std::move(encoded), flags);
}

MenuPatchEntrySpec GameAssetSource::BuildRawPatchEntry(
    const ExtractedAssetEntry& source_entry, std::string target_rel_path,
    std::vector<std::uint8_t> new_raw_bytes, std::uint8_t flags) {
    MenuPatchEntrySpec spec;
    spec.source_rel_path = source_entry.archive_rel_path;
    spec.target_rel_path = std::move(target_rel_path);
    spec.flags = flags;
    spec.orig_offset = source_entry.orig_offset;
    spec.orig_slice = source_entry.orig_raw_slice;
    spec.new_raw = std::move(new_raw_bytes);
    return spec;
}

void ExportHg3ContactSheetPages(const std::string& hg3_filename,
                                const ama::hg3::Hg3Container& container,
                                const std::filesystem::path& output_dir) {
    std::vector<ama::imaging::ContactSheetCell> cells;
    cells.reserve(container.frames.size());
    for (std::size_t idx = 0; idx < container.frames.size(); ++idx) {
        const auto& f = container.frames[idx];
        if (f.pixels.pixels.empty()) {
            continue;
        }
        ama::imaging::ContactSheetCell cell;
        cell.image = f.pixels;
        cell.label = "#" + std::to_string(idx) + " (id=" + std::to_string(f.frame_id) + ") " +
                     std::to_string(f.stdinfo.width) + "x" + std::to_string(f.stdinfo.height) +
                     " " + std::to_string(f.stdinfo.bpp) + "bpp";
        cells.push_back(std::move(cell));
    }
    if (cells.empty()) {
        return;
    }

    ama::imaging::ContactSheetLayout layout;
    layout.columns = kContactSheetColumns;
    layout.cells_per_page = kContactSheetMaxFramesPerPage;
    layout.max_cell_px = kContactSheetMaxCellPx;
    ama::imaging::SaveContactSheetPages(cells, output_dir, StripExtension(hg3_filename), layout);
}

IMenuDomainRenderer::~IMenuDomainRenderer() = default;

}  // namespace ama::menu
