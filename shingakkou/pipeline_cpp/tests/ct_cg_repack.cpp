// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room component tests for shingakkou's offline CG repack pipeline
// driven strictly through shin::cg::run_find_narrative_cg(repack_only=true).
#include "cg/cg_pipeline.h"
#include "cg/image.h"
#include "common/util.h"
#include "ddp/ddp_archive.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;
using shin::Bytes;

void ClearReadOnlyTree(const fs::path& root) noexcept {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        const std::wstring w = entry.path().wstring();
        const DWORD attrs = GetFileAttributesW(w.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
            SetFileAttributesW(w.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name_utf8)
        : path_(fs::temp_directory_path() / fs::u8path(name_utf8)) {
        ClearReadOnlyTree(path_);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        ClearReadOnlyTree(path_);
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void PutU32Le(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

Bytes MakeDdp2Archive(const std::vector<Bytes>& raw_entries) {
    Bytes out = {'D', 'D', 'P', '2'};
    PutU32Le(out, static_cast<std::uint32_t>(raw_entries.size()));
    out.resize(0x20u, 0u);
    std::vector<Bytes> wrapped;
    wrapped.reserve(raw_entries.size());
    for (const Bytes& e : raw_entries) wrapped.push_back(shin::ddp::shs_wrap(e));

    std::uint32_t cursor =
        static_cast<std::uint32_t>(0x20u + raw_entries.size() * 16u);
    for (std::size_t i = 0u; i < raw_entries.size(); ++i) {
        PutU32Le(out, cursor);
        PutU32Le(out, static_cast<std::uint32_t>(raw_entries[i].size()));
        PutU32Le(out, static_cast<std::uint32_t>(wrapped[i].size()));
        PutU32Le(out, 0u);
        cursor += static_cast<std::uint32_t>(wrapped[i].size());
    }
    for (const Bytes& w : wrapped) out.insert(out.end(), w.begin(), w.end());
    return out;
}

Bytes MakeDdp3Archive(const std::vector<std::pair<std::string, Bytes>>& named_raw_entries) {
    auto to_utf16le = [](const std::string& s) {
        Bytes out;
        for (char c : s) {
            out.push_back(static_cast<std::uint8_t>(c));
            out.push_back(0u);
        }
        return out;
    };

    std::vector<Bytes> names;
    std::vector<Bytes> wrapped;
    std::uint32_t block = 0u;
    for (const auto& [name, raw] : named_raw_entries) {
        names.push_back(to_utf16le(name));
        wrapped.push_back(shin::ddp::shs_wrap(raw));
        block += static_cast<std::uint32_t>(17u + names.back().size());
    }
    const std::uint32_t sec_off = 0x20u + 8u;
    const std::uint32_t data_start = 0x400u;

    Bytes out = {'D', 'D', 'P', '3'};
    PutU32Le(out, 1u);
    PutU32Le(out, data_start);
    out.resize(0x20u, 0u);
    PutU32Le(out, block);
    PutU32Le(out, sec_off);

    std::uint32_t cursor = data_start;
    for (std::size_t i = 0u; i < named_raw_entries.size(); ++i) {
        out.push_back(static_cast<std::uint8_t>(17u + names[i].size()));
        PutU32Le(out, cursor);
        PutU32Le(out, static_cast<std::uint32_t>(named_raw_entries[i].second.size()));
        PutU32Le(out, static_cast<std::uint32_t>(wrapped[i].size()));
        PutU32Le(out, 0u);
        out.insert(out.end(), names[i].begin(), names[i].end());
        cursor += static_cast<std::uint32_t>(wrapped[i].size());
    }
    out.resize(data_start, 0u);
    for (const Bytes& w : wrapped) out.insert(out.end(), w.begin(), w.end());
    return out;
}

shin::cg::Image MakeSolidImage(int w, int h, int ch,
                               std::uint8_t r, std::uint8_t g,
                               std::uint8_t b, std::uint8_t a) {
    shin::cg::Image img;
    img.width = w;
    img.height = h;
    img.channels = ch;
    img.px.resize(static_cast<std::size_t>(w) * h * ch);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = img.at(x, y);
            p[0] = r;
            p[1] = g;
            p[2] = b;
            if (ch == 4) p[3] = a;
        }
    }
    return img;
}

Bytes ExtractEntryDecompressed(const std::string& archive_path, const std::string& entry_name) {
    const Bytes raw_arc = shin::read_file(archive_path);
    const auto entries = shin::ddp::list_archive(archive_path);
    for (const auto& e : entries) {
        if (e.name != entry_name) continue;
        const std::uint32_t stored = (e.packed != 0u) ? e.packed : e.unpacked;
        const Bytes slice(raw_arc.begin() + static_cast<std::ptrdiff_t>(e.offset),
                          raw_arc.begin() + static_cast<std::ptrdiff_t>(e.offset + stored));
        return (e.packed != 0u) ? shin::ddp::shs_decompress(slice, e.unpacked) : slice;
    }
    return {};
}

bool HasAnyTempFiles(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".tmp") return true;
    }
    return false;
}

}  // namespace

TEST(CgRepackCt, RepacksFromBackupPreserves32BppAlphaRevertsShrinkingAndEmptySets) {
    const ScopedTempDir sandbox("shin_ct_cg_repack");
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path data_dir = game_dir / "data";
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path out_dir = project_dir / "script_output";
    const fs::path patched_dir = out_dir / "narrative_patched";
    fs::create_directories(data_dir);
    fs::create_directories(patched_dir);

    // Build 32-bpp RGBA sprite (with transparent + translucent + opaque pixels) and 24-bpp BG.
    shin::cg::Image ev01_orig_img = MakeSolidImage(16, 12, 4, 0x10u, 0x20u, 0x30u, 0x00u);
    ev01_orig_img.at(0, 0)[3] = 0x80u;
    ev01_orig_img.at(1, 0)[3] = 0xFFu;
    const Bytes ev01_orig_bmp = shin::cg::encode_bmp(ev01_orig_img);

    const Bytes ev02_orig_bmp =
        shin::cg::encode_bmp(MakeSolidImage(16, 12, 3, 0x40u, 0x50u, 0x60u, 0xFFu));
    const Bytes ev03_orig_bmp =
        shin::cg::encode_bmp(MakeSolidImage(16, 12, 3, 0x7Fu, 0x80u, 0xFFu, 0xFFu));

    const Bytes orig_cgev = MakeDdp3Archive({
        {"ev01", ev01_orig_bmp},
        {"ev02", ev02_orig_bmp},
        {"ev03", ev03_orig_bmp},
    });
    const fs::path cgev_path = data_dir / "sin_cgev.dat";
    const fs::path cgev_bak = data_dir / "sin_cgev.dat.backup";
    shin::write_file(cgev_path.u8string(), orig_cgev);

    // Prepare rendered patched BMPs (ev01 remains 32-bpp BGRA with preserved alpha!).
    shin::cg::Image ev01_patched_img = MakeSolidImage(16, 12, 4, 0x00u, 0x00u, 0x00u, 0x00u);
    ev01_patched_img.at(2, 2)[0] = 0xFFu;
    ev01_patched_img.at(2, 2)[1] = 0xFFu;
    ev01_patched_img.at(2, 2)[2] = 0xFFu;
    ev01_patched_img.at(2, 2)[3] = 0x80u;
    const Bytes ev01_patched_bmp = shin::cg::encode_bmp(ev01_patched_img);
    ASSERT_EQ(ev01_patched_bmp[0x1Cu], 32u);

    const Bytes ev02_patched_bmp =
        shin::cg::encode_bmp(MakeSolidImage(16, 12, 3, 0xAAu, 0xBBu, 0xCCu, 0xFFu));

    const fs::path ev01_file = patched_dir / "ev01.bmp";
    const fs::path ev02_file = patched_dir / "ev02.bmp";
    shin::write_file(ev01_file.u8string(), ev01_patched_bmp);
    shin::write_file(ev02_file.u8string(), ev02_patched_bmp);

    auto write_candidates = [&](const bj::array& arr) {
        shin::write_file_text((out_dir / "narrative_candidates.json").u8string(),
                              shin::json_pretty(bj::value(arr), 2));
    };

    shin::cg::CgOptions opt;
    opt.game_dir = game_dir.u8string();
    opt.project_dir = project_dir.u8string();
    opt.repack_only = true;

    // Pass 1: patch {ev01, ev02}; ev03 stays verbatim.
    write_candidates(bj::array{
        bj::object{{"arc", "sin_cgev.dat"}, {"entry", "ev01"}, {"patched_bmp", ev01_file.u8string()}},
        bj::object{{"arc", "sin_cgev.dat"}, {"entry", "ev02"}, {"patched_bmp", ev02_file.u8string()}},
    });
    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    ASSERT_TRUE(fs::exists(cgev_bak));
    EXPECT_EQ(shin::read_file(cgev_bak.u8string()), orig_cgev);
    EXPECT_FALSE(HasAnyTempFiles(data_dir));

    const Bytes pass1_bytes = shin::read_file(cgev_path.u8string());
    const Bytes got_ev01 = ExtractEntryDecompressed(cgev_path.u8string(), "ev01");
    ASSERT_GE(got_ev01.size(), 30u);
    EXPECT_EQ(got_ev01[0x1Cu], 32u) << "32-bpp straight alpha must survive CG repack";
    EXPECT_EQ(got_ev01, ev01_patched_bmp);
    EXPECT_EQ(ExtractEntryDecompressed(cgev_path.u8string(), "ev02"), ev02_patched_bmp);
    EXPECT_EQ(ExtractEntryDecompressed(cgev_path.u8string(), "ev03"), ev03_orig_bmp);

    // Pass 1 replay: strictly idempotent
    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    EXPECT_EQ(shin::read_file(cgev_path.u8string()), pass1_bytes);
    EXPECT_EQ(shin::read_file(cgev_bak.u8string()), orig_cgev);

    // Pass 2: shrinking set {ev01} ONLY -> ev02 MUST revert to pristine original from .backup!
    write_candidates(bj::array{
        bj::object{{"arc", "sin_cgev.dat"}, {"entry", "ev01"}, {"patched_bmp", ev01_file.u8string()}},
    });
    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    EXPECT_EQ(ExtractEntryDecompressed(cgev_path.u8string(), "ev01"), ev01_patched_bmp);
    EXPECT_EQ(ExtractEntryDecompressed(cgev_path.u8string(), "ev02"), ev02_orig_bmp);
    EXPECT_EQ(ExtractEntryDecompressed(cgev_path.u8string(), "ev03"), ev03_orig_bmp);
    EXPECT_EQ(shin::read_file(cgev_bak.u8string()), orig_cgev);

    // Pass 3: empty candidate set -> live sin_cgev.dat MUST restore byte-identical to .backup!
    write_candidates(bj::array{});
    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    EXPECT_EQ(shin::read_file(cgev_path.u8string()), orig_cgev);
    EXPECT_EQ(shin::read_file(cgev_bak.u8string()), orig_cgev);
    EXPECT_FALSE(HasAnyTempFiles(data_dir));
}

TEST(CgRepackCt, NeverMutatesMenuManagedSinSysdDatOrBreaksBakContract) {
    const ScopedTempDir sandbox("shin_ct_cg_sin_sysd_guard");
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path data_dir = game_dir / "data";
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path out_dir = project_dir / "script_output";
    const fs::path patched_dir = out_dir / "narrative_patched";
    fs::create_directories(data_dir);
    fs::create_directories(patched_dir);

    const Bytes orig_tga = {'T', 'G', 'A', '_', 'P', 'R', 'I', 'S', 'T', 'I', 'N', 'E'};
    const Bytes orig_sin_sysd = MakeDdp2Archive({orig_tga});
    const fs::path sin_sysd_path = data_dir / "sin_sysd.dat";
    shin::write_file(sin_sysd_path.u8string(), orig_sin_sysd);

    const Bytes repl_bmp =
        shin::cg::encode_bmp(MakeSolidImage(8, 8, 3, 0xFFu, 0x00u, 0x00u, 0xFFu));
    const fs::path repl_file = patched_dir / "sin_sysd#00000.bmp";
    shin::write_file(repl_file.u8string(), repl_bmp);

    shin::write_file_text(
        (out_dir / "narrative_candidates.json").u8string(),
        shin::json_pretty(
            bj::value(bj::array{
                bj::object{{"arc", "sin_sysd.dat"},
                           {"entry", "sin_sysd#00000"},
                           {"patched_bmp", repl_file.u8string()}},
            }),
            2));

    shin::cg::CgOptions opt;
    opt.game_dir = game_dir.u8string();
    opt.project_dir = project_dir.u8string();
    opt.repack_only = true;

    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    EXPECT_EQ(shin::read_file(sin_sysd_path.u8string()), orig_sin_sysd)
        << "03_find_narrative_cg must never mutate Data\\sin_sysd.dat";
    EXPECT_FALSE(fs::exists(data_dir / "sin_sysd.dat.backup"));
    EXPECT_FALSE(HasAnyTempFiles(data_dir));
}

TEST(CgRepackCt, CorruptArchiveRepackLeavesLiveIntactWithoutBackupOrTempLeak) {
    const ScopedTempDir sandbox("shin_ct_cg_corrupt_rollback");
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path data_dir = game_dir / "data";
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path out_dir = project_dir / "script_output";
    const fs::path patched_dir = out_dir / "narrative_patched";
    fs::create_directories(data_dir);
    fs::create_directories(patched_dir);

    const Bytes corrupt_arc = {'B', 'A', 'D', '!', 0x00u, 0x01u, 0x02u, 0x03u, 0xFFu};
    const fs::path cgev_path = data_dir / "sin_cgev.dat";
    shin::write_file(cgev_path.u8string(), corrupt_arc);

    const Bytes repl_bmp =
        shin::cg::encode_bmp(MakeSolidImage(8, 8, 3, 0x11u, 0x22u, 0x33u, 0xFFu));
    const fs::path repl_file = patched_dir / "ev01.bmp";
    shin::write_file(repl_file.u8string(), repl_bmp);

    shin::write_file_text(
        (out_dir / "narrative_candidates.json").u8string(),
        shin::json_pretty(
            bj::value(bj::array{
                bj::object{{"arc", "sin_cgev.dat"},
                           {"entry", "ev01"},
                           {"patched_bmp", repl_file.u8string()}},
            }),
            2));

    shin::cg::CgOptions opt;
    opt.game_dir = game_dir.u8string();
    opt.project_dir = project_dir.u8string();
    opt.repack_only = true;

    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    EXPECT_EQ(shin::read_file(cgev_path.u8string()), corrupt_arc);
    EXPECT_FALSE(fs::exists(data_dir / "sin_cgev.dat.backup"));
    EXPECT_FALSE(HasAnyTempFiles(data_dir));
}

TEST(CgRepackCt, ReadOnlySourceArchiveRepacksAndRestoresWithoutTempLeak) {
    const ScopedTempDir sandbox("shin_ct_cg_readonly");
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path data_dir = game_dir / "data";
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path out_dir = project_dir / "script_output";
    const fs::path patched_dir = out_dir / "narrative_patched";
    fs::create_directories(data_dir);
    fs::create_directories(patched_dir);

    const Bytes orig_bmp =
        shin::cg::encode_bmp(MakeSolidImage(8, 8, 3, 0x10u, 0x20u, 0x30u, 0xFFu));
    const Bytes patched_bmp =
        shin::cg::encode_bmp(MakeSolidImage(8, 8, 3, 0xEEu, 0xDDu, 0xCCu, 0xFFu));
    const Bytes orig_cgev = MakeDdp3Archive({{"ev01", orig_bmp}});

    const fs::path cgev_path = data_dir / "sin_cgev.dat";
    const fs::path cgev_bak = data_dir / "sin_cgev.dat.backup";
    const fs::path ev01_file = patched_dir / "ev01.bmp";
    shin::write_file(cgev_path.u8string(), orig_cgev);
    shin::write_file(ev01_file.u8string(), patched_bmp);

    // Mark retail source archive FILE_ATTRIBUTE_READONLY before first repack.
    ASSERT_TRUE(SetFileAttributesW(cgev_path.wstring().c_str(), FILE_ATTRIBUTE_READONLY));

    auto write_candidates = [&](const bj::array& arr) {
        shin::write_file_text((out_dir / "narrative_candidates.json").u8string(),
                              shin::json_pretty(bj::value(arr), 2));
    };

    shin::cg::CgOptions opt;
    opt.game_dir = game_dir.u8string();
    opt.project_dir = project_dir.u8string();
    opt.repack_only = true;

    write_candidates(bj::array{
        bj::object{{"arc", "sin_cgev.dat"},
                   {"entry", "ev01"},
                   {"patched_bmp", ev01_file.u8string()}},
    });
    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    ASSERT_TRUE(fs::exists(cgev_bak));
    EXPECT_EQ(shin::read_file(cgev_bak.u8string()), orig_cgev);
    EXPECT_EQ(ExtractEntryDecompressed(cgev_path.u8string(), "ev01"), patched_bmp);
    EXPECT_FALSE(HasAnyTempFiles(data_dir));

    // Mark both live archive and .backup FILE_ATTRIBUTE_READONLY before empty-set restore.
    ASSERT_TRUE(SetFileAttributesW(cgev_path.wstring().c_str(), FILE_ATTRIBUTE_READONLY));
    ASSERT_TRUE(SetFileAttributesW(cgev_bak.wstring().c_str(), FILE_ATTRIBUTE_READONLY));

    write_candidates(bj::array{});
    ASSERT_EQ(shin::cg::run_find_narrative_cg(opt), 0);
    EXPECT_EQ(shin::read_file(cgev_path.u8string()), orig_cgev);
    EXPECT_FALSE(HasAnyTempFiles(data_dir));
}
