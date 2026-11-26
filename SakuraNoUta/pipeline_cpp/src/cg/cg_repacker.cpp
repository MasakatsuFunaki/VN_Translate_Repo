// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cg/cg_repacker.h"

#include "bgi/bgi_archive.h"
#include "bgi/bgi_cbg.h"
#include "menu/bgra_image.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace snu::cg {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kRgbChannels = 3;
constexpr std::size_t kRgbRedOffset = 0;
constexpr std::size_t kRgbGreenOffset = 1;
constexpr std::size_t kRgbBlueOffset = 2;

constexpr std::size_t kCbgMinProbeBytes = 0x16;
constexpr std::size_t kCbgWidthOffset = 0x10;
constexpr std::size_t kCbgHeightOffset = 0x12;
constexpr std::size_t kCbgBppOffset = 0x14;

constexpr std::size_t kBmpFileHeaderBytes = 14;
constexpr std::size_t kBmpInfoHeaderBytes = 40;
constexpr std::size_t kBmpTotalHeaderBytes =
    kBmpFileHeaderBytes + kBmpInfoHeaderBytes;
constexpr std::size_t kBmpDibSizeOffset = 14;
constexpr std::size_t kBmpWidthOffset = 18;
constexpr std::size_t kBmpHeightOffset = 22;
constexpr std::size_t kBmpBppOffset = 28;
constexpr std::uint8_t kBmpSigB = 'B';
constexpr std::uint8_t kBmpSigM = 'M';

constexpr std::array<std::string_view, 3> kDefaultSkipEntryNames = {
    "kt_ib",
    "kt_mh",
    "kt_ny",
};

class ScopedTempFile {
public:
    explicit ScopedTempFile(fs::path path) noexcept : path_(std::move(path)) {}

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    ~ScopedTempFile() {
        if (!released_) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }

    void release() noexcept { released_ = true; }

private:
    fs::path path_;
    bool released_ = false;
};

std::uint16_t read_le16(const Bytes& raw, std::size_t off) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, raw.data() + off, sizeof(v));
    return v;
}

std::uint32_t read_le32(const Bytes& raw, std::size_t off) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, raw.data() + off, sizeof(v));
    return v;
}

bool is_supported_bg_bpp(int bpp) noexcept {
    return bpp == snu::menu::kBmpBpp24 || bpp == snu::menu::kBmpBpp32;
}

}  // namespace

struct ArchiveRepacker::Impl {
    std::set<std::string, std::less<>> skip_entries;
};

const std::set<std::string>& default_skip_entries() {
    static const std::set<std::string> kSkipEntries = [] {
        std::set<std::string> s;
        for (const std::string_view name : kDefaultSkipEntryNames) {
            s.emplace(name);
        }
        return s;
    }();
    return kSkipEntries;
}

bool is_skipped_cg_entry(std::string_view entry_name) noexcept {
    for (const std::string_view skipped : kDefaultSkipEntryNames) {
        if (entry_name == skipped) {
            return true;
        }
    }
    return false;
}

std::optional<ArchiveBgSpec> probe_archive_bg_spec(const Bytes& raw_entry) {
    if (bgi::is_cbg(raw_entry)) {
        if (raw_entry.size() < kCbgMinProbeBytes) {
            return std::nullopt;
        }
        const int width = static_cast<int>(read_le16(raw_entry, kCbgWidthOffset));
        const int height = static_cast<int>(read_le16(raw_entry, kCbgHeightOffset));
        const int bpp = static_cast<int>(read_le16(raw_entry, kCbgBppOffset));
        if (width <= 0 || height <= 0 || !is_supported_bg_bpp(bpp)) {
            return std::nullopt;
        }
        return ArchiveBgSpec{width, height, bpp, {}};
    }

    if (bgi::is_dsc(raw_entry)) {
        try {
            const Bytes decoded = bgi::dsc_decompress(raw_entry);
            const bgi::SysGrpImage sys = bgi::decode_sysgrp(decoded);
            if (!is_supported_bg_bpp(sys.orig_bpp)) {
                return std::nullopt;
            }
            return ArchiveBgSpec{sys.image.width(), sys.image.height(),
                                 sys.orig_bpp, sys.reserved};
        } catch (...) {
            return std::nullopt;
        }
    }

    if (raw_entry.size() >= kBmpTotalHeaderBytes && raw_entry[0] == kBmpSigB &&
        raw_entry[1] == kBmpSigM) {
        const std::uint32_t dib_size = read_le32(raw_entry, kBmpDibSizeOffset);
        if (dib_size < kBmpInfoHeaderBytes) {
            return std::nullopt;
        }
        const auto bi_width =
            static_cast<std::int32_t>(read_le32(raw_entry, kBmpWidthOffset));
        const auto bi_height =
            static_cast<std::int32_t>(read_le32(raw_entry, kBmpHeightOffset));
        const int bpp = static_cast<int>(read_le16(raw_entry, kBmpBppOffset));
        if (bi_width <= 0 || bi_height == 0 ||
            bi_height == std::numeric_limits<std::int32_t>::min() ||
            !is_supported_bg_bpp(bpp)) {
            return std::nullopt;
        }
        const int height = (bi_height < 0) ? -bi_height : bi_height;
        return ArchiveBgSpec{static_cast<int>(bi_width), height, bpp, {}};
    }

    if (raw_entry.size() >= bgi::kSysGrpHeaderBytes) {
        try {
            const bgi::SysGrpImage sys = bgi::decode_sysgrp(raw_entry);
            if (!is_supported_bg_bpp(sys.orig_bpp)) {
                return std::nullopt;
            }
            return ArchiveBgSpec{sys.image.width(), sys.image.height(),
                                 sys.orig_bpp, sys.reserved};
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

Bytes encode_bgi_archive_bg(
    const Image& patched, int target_bpp, const Image* alpha_donor,
    const std::array<std::uint8_t, bgi::kSysGrpReservedBytes>& reserved) {
    if (patched.empty()) {
        throw std::invalid_argument("encode_bgi_archive_bg: empty image");
    }
    if (!is_supported_bg_bpp(target_bpp)) {
        throw std::invalid_argument("encode_bgi_archive_bg: unsupported bpp");
    }

    const std::size_t pixel_count = static_cast<std::size_t>(patched.width) *
                                    static_cast<std::size_t>(patched.height);
    if (patched.rgb.size() != pixel_count * kRgbChannels) {
        throw std::invalid_argument(
            "encode_bgi_archive_bg: rgb buffer size does not match dimensions");
    }
    if (!patched.alpha.empty() && !patched.has_alpha()) {
        throw std::invalid_argument(
            "encode_bgi_archive_bg: alpha buffer size does not match dimensions");
    }
    if (alpha_donor != nullptr) {
        if (alpha_donor->width != patched.width ||
            alpha_donor->height != patched.height) {
            throw std::invalid_argument(
                "encode_bgi_archive_bg: alpha donor dimensions do not match image");
        }
        if (!alpha_donor->alpha.empty() && !alpha_donor->has_alpha()) {
            throw std::invalid_argument(
                "encode_bgi_archive_bg: alpha donor buffer size does not match dimensions");
        }
    }

    const bool use_patched_alpha =
        (target_bpp == snu::menu::kBmpBpp32) && patched.has_alpha();
    const bool use_donor_alpha =
        (target_bpp == snu::menu::kBmpBpp32) && !use_patched_alpha &&
        (alpha_donor != nullptr) && alpha_donor->has_alpha();

    Bytes bgra_bytes(
        pixel_count * static_cast<std::size_t>(snu::menu::kBgraBytesPerPixel));
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::size_t s_off = i * kRgbChannels;
        const std::size_t d_off =
            i * static_cast<std::size_t>(snu::menu::kBgraBytesPerPixel);
        bgra_bytes[d_off + snu::menu::kChannelBlue] =
            patched.rgb[s_off + kRgbBlueOffset];
        bgra_bytes[d_off + snu::menu::kChannelGreen] =
            patched.rgb[s_off + kRgbGreenOffset];
        bgra_bytes[d_off + snu::menu::kChannelRed] =
            patched.rgb[s_off + kRgbRedOffset];
        if (use_patched_alpha) {
            bgra_bytes[d_off + snu::menu::kChannelAlpha] = patched.alpha[i];
        } else if (use_donor_alpha) {
            bgra_bytes[d_off + snu::menu::kChannelAlpha] = alpha_donor->alpha[i];
        } else {
            bgra_bytes[d_off + snu::menu::kChannelAlpha] = snu::menu::kOpaqueAlpha;
        }
    }

    const snu::menu::BgraImage bgra(patched.width, patched.height,
                                    std::move(bgra_bytes));
    const Bytes raw_bg = bgi::encode_sysgrp(bgra, target_bpp, reserved);
    Bytes compressed = bgi::dsc_compress(raw_bg);
    if (bgi::dsc_decompress(compressed) != raw_bg) {
        throw std::runtime_error(
            "encode_bgi_archive_bg: DSC round-trip self-check failed");
    }
    return compressed;
}

ArchiveRepacker::ArchiveRepacker() : impl_(std::make_unique<Impl>()) {
    for (const std::string_view name : kDefaultSkipEntryNames) {
        impl_->skip_entries.emplace(name);
    }
}

ArchiveRepacker::ArchiveRepacker(std::set<std::string> skip_entries)
    : impl_(std::make_unique<Impl>()) {
    for (const std::string& name : skip_entries) {
        impl_->skip_entries.emplace(name);
    }
}

ArchiveRepacker::ArchiveRepacker(ArchiveRepacker&& other) noexcept = default;
ArchiveRepacker& ArchiveRepacker::operator=(ArchiveRepacker&& other) noexcept = default;
ArchiveRepacker::~ArchiveRepacker() = default;

bool ArchiveRepacker::is_skipped(std::string_view entry_name) const noexcept {
    if (!impl_) {
        return false;
    }
    return impl_->skip_entries.find(entry_name) != impl_->skip_entries.end();
}

RepackResult ArchiveRepacker::repack_from_backup(
    const std::string& arc_path,
    const std::map<std::string, Bytes>& replacements) const {
    std::map<std::string, Bytes> filtered;
    std::size_t skipped_count = 0;
    for (const auto& [entry_name, payload] : replacements) {
        if (is_skipped(entry_name)) {
            ++skipped_count;
            continue;
        }
        filtered.emplace(entry_name, payload);
    }

    const std::string bak = arc_path + ".backup";
    const fs::path arc_fs = fs::u8path(arc_path);
    const fs::path bak_fs = fs::u8path(bak);
    const bool has_backup = fs::exists(bak_fs);

    if (filtered.empty() && !has_backup) {
        return RepackResult{0u, skipped_count, {}};
    }

    const std::string source_arc = has_backup ? bak : arc_path;
    const std::string tmp = arc_path + ".tmp";
    const fs::path tmp_fs = fs::u8path(tmp);
    ScopedTempFile tmp_guard(tmp_fs);

    bgi::repack_arc(source_arc, tmp, filtered);

    if (!has_backup) {
        const fs::path bak_tmp_fs = fs::u8path(bak + ".tmp");
        ScopedTempFile bak_tmp_guard(bak_tmp_fs);
        fs::copy_file(arc_fs, bak_tmp_fs, fs::copy_options::overwrite_existing);
        fs::rename(bak_tmp_fs, bak_fs);
        bak_tmp_guard.release();
    }

    fs::rename(tmp_fs, arc_fs);
    tmp_guard.release();
    return RepackResult{filtered.size(), skipped_count, bak};
}

}  // namespace snu::cg
