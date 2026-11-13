// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// System tests for WelcomeToAnzU!_CE requiring a live game installation via
// VN_GAME_DIR. All write/patch tests copy input files into a ScopedTempDir
// under temp_directory_path() and never mutate VN_GAME_DIR.
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "menu/menu_patcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;
using exm::menu::ComputeMurmurHash2;
using exm::menu::MenuArchivePatcher;
using exm::menu::StandalonePatchRecord;
using exm::menu::encode_dwq_from_rgba;
using exm::menu::kAnzUSys0EntryCount;
using exm::menu::kExpectedModifiedGpkEntryCount;
using exm::menu::kExpectedStandaloneFileCount;
using exm::menu::parse_patch_bin;

constexpr int kMaxWalkLevels = 6;
constexpr std::size_t kExpectedAnzUSptFiles = 3;
constexpr std::int64_t kExpectedTokutenStrings = 2558;
constexpr std::int64_t kExpectedDistinctSpeakers = 19;

constexpr std::size_t kIdxBgMinigame601 = 0;
constexpr std::size_t kIdxChartBack11 = 4;
constexpr std::size_t kIdxChartBack41 = 10;
constexpr std::size_t kIdxTaChartmapBt71 = 85;
constexpr std::size_t kIdxTaXtgCgcur02 = 109;
constexpr std::size_t kIdxTaXtgMeswin02 = 159;
constexpr std::size_t kIdxTaXtgTitbt04 = 177;

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

fs::path ResolveLiveGameDir() {
    const wchar_t* env_dir = _wgetenv(L"VN_GAME_DIR");
    if (env_dir == nullptr || *env_dir == L'\0') {
        return {};
    }
    const std::wstring_view wide(env_dir);
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return fs::u8path(utf8);
}

fs::path FindCommittedPatchBin() {
    fs::path cur = fs::current_path();
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / "menu_patch.bin")) {
            return cur / "menu_patch.bin";
        }
        if (fs::exists(cur / "EXTRAVAGANZA_05_ANZU_CE" / "menu_patch.bin")) {
            return cur / "EXTRAVAGANZA_05_ANZU_CE" / "menu_patch.bin";
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

fs::path ResolveMatchingUnpatchedFile(const fs::path& game_dir,
                                      const std::vector<fs::path>& rel_candidates,
                                      std::uint32_t expected_size,
                                      std::uint32_t expected_hash) {
    for (const fs::path& rel : rel_candidates) {
        const fs::path full = game_dir / rel;
        if (!fs::is_regular_file(full)) {
            continue;
        }
        const auto bytes = exm::read_file(full.u8string());
        if (bytes.size() == expected_size &&
            ComputeMurmurHash2(bytes.data(), bytes.size()) == expected_hash) {
            return full;
        }
    }
    return {};
}

fs::path ResolveStandaloneUnpatchedSource(const fs::path& game_dir,
                                          const StandalonePatchRecord& rec) {
    const fs::path rel = fs::u8path(rec.rel_path);
    const fs::path filename = rel.filename();
    const std::vector<fs::path> candidates = {
        fs::u8path(rec.rel_path + ".bak"),
        fs::path("xtx_backup") / filename,
        fs::path("xtx_backup") / rel,
        fs::path("init2_backup") / filename,
        fs::path("init2_backup") / rel,
        rel,
    };
    return ResolveMatchingUnpatchedFile(
        game_dir, candidates, rec.orig_file_size, rec.orig_data_hash);
}

}  // namespace

TEST(LiveGameSystem, SpeakerGatePassesOnLiveExtractedCorpus) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live SPT extraction check.";
    }
    const fs::path spt_dir = fs::u8path(extract::resolve_spt_dir(game_dir.u8string()));
    if (!fs::is_directory(spt_dir)) {
        GTEST_SKIP() << "Live WelcomeToAnzU!_CE directory not present: " << spt_dir.u8string();
    }

    const ScopedTempDir tmp("anzu_ce_st_speaker_gate");
    const fs::path tmp_extracted = tmp.path() / "extracted_text.json";

    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), tmp_extracted.u8string()), 0);
    const bj::value extracted_v = json_parse_file(tmp_extracted.u8string());
    const auto report = translate::run_speaker_checks(extracted_v.as_object());
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.failed_check, 0);
}

TEST(LiveGameSystem, LiveGameExtractionProducesCompleteCorpusAndSpeakers) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live SPT extraction check.";
    }
    const fs::path spt_dir = fs::u8path(extract::resolve_spt_dir(game_dir.u8string()));
    if (!fs::is_directory(spt_dir)) {
        GTEST_SKIP() << "Live WelcomeToAnzU!_CE directory not present: " << spt_dir.u8string();
    }

    const ScopedTempDir tmp("anzu_ce_st_extract_corpus");
    const fs::path tmp_out = tmp.path() / "extracted_text.json";

    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), tmp_out.u8string()), 0);
    ASSERT_TRUE(fs::exists(tmp_out));

    const bj::value root_v = json_parse_file(tmp_out.u8string());
    ASSERT_TRUE(root_v.is_object());
    const bj::object& root = root_v.get_object();
    EXPECT_EQ(root.size(), kExpectedAnzUSptFiles);
    ASSERT_TRUE(root.contains("tokuten.spt"));
    EXPECT_EQ(root.at("tokuten.spt").as_object().at("strings_found").as_int64(),
              kExpectedTokutenStrings);

    std::set<std::string> speakers;
    for (const auto& kv : root) {
        for (const auto& sv : kv.value().as_object().at("strings").as_array()) {
            const auto& so = sv.as_object();
            if (const auto* sp = so.if_contains("speaker")) {
                if (sp->is_string() && !sp->as_string().empty()) {
                    speakers.emplace(sp->as_string());
                }
            }
        }
    }
    EXPECT_EQ(static_cast<std::int64_t>(speakers.size()), kExpectedDistinctSpeakers);
    EXPECT_TRUE(speakers.count("夢美") != 0);
    EXPECT_TRUE(speakers.count("アゲハ") != 0);
}

TEST(LiveGameSystem, LiveSys0GpkDecodesAndReencodes24BitAnd2ASprites) {
    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP() << "Live WelcomeToAnzU!_CE dwq\\sys0.{gpk,gtb} not present.";
    }

    MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kAnzUSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kAnzUSys0EntryCount),
              static_cast<std::uint32_t>(fs::file_size(gpk_path)));
    EXPECT_EQ(patcher.entry_name(kIdxBgMinigame601), "bg_minigame6_01");
    EXPECT_EQ(patcher.entry_name(kIdxChartBack11), "chart_back1_1");
    EXPECT_EQ(patcher.entry_name(kIdxChartBack41), "chart_back4_1");
    EXPECT_EQ(patcher.entry_name(kIdxTaChartmapBt71), "ta_chartmap_bt71");

    for (std::size_t idx = 0; idx < patcher.entry_count(); ++idx) {
        const auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u) << "entry " << idx;
        EXPECT_GT(img.height, 0u) << "entry " << idx;
        if (idx == kIdxBgMinigame601 || idx == kIdxChartBack41 ||
            idx == kIdxTaChartmapBt71 || idx == kIdxTaXtgCgcur02 ||
            idx == kIdxTaXtgMeswin02 || idx == kIdxTaXtgTitbt04) {
            const auto& raw = patcher.entry_raw_bytes(idx);
            const auto reenc = encode_dwq_from_rgba(raw.data(), raw.size(), img, true);
            const auto redec = exm::menu::decode_dwq_to_rgba(reenc.data(), reenc.size());
            EXPECT_EQ(redec.pixels, img.pixels) << "entry " << idx;
        }
    }
}

TEST(LiveGameSystem, CommittedAnzMenu1ReconstructsAndAppliesInTempDir) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live ANZMENU1 reconstruction check.";
    }

    const fs::path patch_bin_path = FindCommittedPatchBin();
    ASSERT_FALSE(patch_bin_path.empty())
        << "Committed EXTRAVAGANZA_05_ANZU_CE/menu_patch.bin not found";

    const auto patch_bytes = exm::read_file(patch_bin_path.u8string());
    const auto parsed = parse_patch_bin(patch_bytes);

    const fs::path orig_gtb_path = ResolveMatchingUnpatchedFile(
        game_dir,
        {fs::path("dwq_backup") / "sys0.gtb",
         fs::path("dwq") / "sys0.gtb.bak",
         fs::path("dwq") / "sys0.gtb"},
        parsed.orig_gtb_size,
        parsed.orig_gtb_hash);
    if (orig_gtb_path.empty()) {
        GTEST_SKIP() << "Unpatched sys0.gtb not present in live WelcomeToAnzU!_CE directory.";
    }

    fs::path orig_gpk_path;
    for (const fs::path& rel : {fs::path("dwq_backup") / "sys0.gpk",
                                fs::path("dwq") / "sys0.gpk.bak",
                                fs::path("dwq") / "sys0.gpk"}) {
        const fs::path full = game_dir / rel;
        if (fs::is_regular_file(full) && fs::file_size(full) == parsed.orig_gpk_size) {
            orig_gpk_path = full;
            break;
        }
    }
    if (orig_gpk_path.empty()) {
        GTEST_SKIP() << "Unpatched sys0.gpk not present in live WelcomeToAnzU!_CE directory.";
    }

    const auto orig_gtb = exm::read_file(orig_gtb_path.u8string());
    const auto orig_gpk = exm::read_file(orig_gpk_path.u8string());
    const auto [rebuilt_gtb, rebuilt_gpk] =
        MenuArchivePatcher::reconstruct_archive_from_patch_bin(
            patch_bytes, orig_gtb, orig_gpk);
    EXPECT_EQ(rebuilt_gtb.size(), parsed.new_gtb_size);
    EXPECT_EQ(rebuilt_gpk.size(), parsed.new_gpk_size);

    const ScopedTempDir staged_game("anzu_ce_st_staged_game");
    fs::create_directories(staged_game.path() / "dwq");
    fs::copy_file(orig_gtb_path, staged_game.path() / "dwq" / "sys0.gtb",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(orig_gpk_path, staged_game.path() / "dwq" / "sys0.gpk",
                  fs::copy_options::overwrite_existing);

    for (const StandalonePatchRecord& rec : parsed.standalone_records) {
        const fs::path src = ResolveStandaloneUnpatchedSource(game_dir, rec);
        if (src.empty()) {
            GTEST_SKIP() << "Unpatched standalone file not present for " << rec.rel_path;
        }
        const fs::path dst = staged_game.path() / fs::u8path(rec.rel_path);
        fs::create_directories(dst.parent_path());
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    }

    const auto summary = MenuArchivePatcher::apply_patch_bin_to_game_dir(
        staged_game.path(), patch_bin_path, false);
    EXPECT_EQ(summary.total_archive_entries, kAnzUSys0EntryCount);
    EXPECT_EQ(summary.patched_sprite_entries, kExpectedModifiedGpkEntryCount);
    EXPECT_EQ(summary.untouched_sprite_entries,
              kAnzUSys0EntryCount - kExpectedModifiedGpkEntryCount);
    EXPECT_EQ(summary.patched_standalone_files, kExpectedStandaloneFileCount);
    EXPECT_TRUE(fs::exists(staged_game.path() / "dwq" / "sys0.gpk.bak"));
    EXPECT_EQ(fs::file_size(staged_game.path() / "dwq" / "sys0.gpk"), parsed.new_gpk_size);
}
