// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests validating local extraction and narrative-CG outputs when
// present on disk in a developer workspace.
#include "cg/cg_pipeline.h"
#include "cg/image.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace bj = boost::json;
namespace cg = crc::cg;
namespace ex = crc::extract;
namespace fs = std::filesystem;
using namespace crc;

namespace {

constexpr const char* kOpenQuote = "「";
constexpr char32_t kControlCharLimit = 0x20;
constexpr std::size_t kMaxSpeakerCodepoints = 20;
constexpr std::size_t kExpectedTotalExtractedEntries = 53635u;
constexpr std::size_t kExpectedDialogueEntries = 28787u;
constexpr std::size_t kExpectedNarrationEntries = 24848u;
constexpr int kExtractedJsonIndent = 1;
constexpr int kBuildSuccessExitCode = 0;
constexpr int kRgbChannels = 3;
constexpr int kCanvasWidth = 1280;
constexpr int kCanvasHeight = 720;
constexpr std::uint8_t kSampleRed = 1;
constexpr std::uint8_t kSampleGreen = 2;
constexpr std::uint8_t kSampleBlue = 3;
constexpr std::size_t kBmpHeaderBytes = 54u;
constexpr int kCachedIndexIndent = 2;
constexpr std::size_t kExpectedIndexTupleSize = 3u;
constexpr std::size_t kIndexPathSlot = 2u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name =
            "crc_st_system_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return dir_; }

private:
    fs::path dir_;
};

cg::Image solid(int w, int h, cg::Rgb c) {
    cg::Image img;
    img.width = w;
    img.height = h;
    img.rgb.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * kRgbChannels);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int k = 0; k < kRgbChannels; ++k) {
                img.px(x, y)[k] = c[static_cast<std::size_t>(k)];
            }
        }
    }
    return img;
}

}  // namespace

TEST(Extract, DeployedExtractedJsonInvariants) {
    const fs::path p =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "extracted_text.json";
    if (!fs::exists(p)) {
        GTEST_SKIP() << "extracted_text.json not present at " << p.u8string();
    }

    const bj::value root = json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    ASSERT_TRUE(root.get_object().if_contains("sn.bin"));
    const auto& block = root.get_object().at("sn.bin").get_object();
    ASSERT_TRUE(block.if_contains("strings"));
    const auto& strings = block.at("strings").get_array();
    ASSERT_EQ(strings.size(), kExpectedTotalExtractedEntries);
    for (const char* k : {"offset", "type", "speaker", "text"}) {
        EXPECT_TRUE(strings[0].get_object().if_contains(k)) << k;
    }

    std::set<std::int64_t> offsets;
    std::size_t dialogue_count = 0;
    std::size_t narration_count = 0;
    std::size_t ctrl = 0;
    std::size_t dup = 0;
    std::size_t not_cp932 = 0;
    std::size_t bad_speaker = 0;
    for (const auto& sv : strings) {
        const auto& s = sv.get_object();
        const std::string type(s.at("type").get_string());
        const std::string text(s.at("text").get_string());
        const std::string speaker(s.at("speaker").get_string());

        if (type == "dialogue") {
            ++dialogue_count;
        } else if (type == "narration") {
            ++narration_count;
        }

        if (!offsets.insert(s.at("offset").get_int64()).second) {
            ++dup;
        }

        std::size_t i = 0;
        while (i < text.size()) {
            const char32_t cp = utf8_next(text, i);
            if (cp < kControlCharLimit && cp != '\r' && cp != '\n' && cp != '\t') {
                ++ctrl;
                break;
            }
        }
        if (!utf8_to_cp932_strict(text)) {
            ++not_cp932;
        }

        std::size_t sp_len = 0;
        std::size_t j = 0;
        while (j < speaker.size()) {
            utf8_next(speaker, j);
            ++sp_len;
        }
        if (!speaker.empty() &&
            (speaker.find(kOpenQuote) != std::string::npos || sp_len > kMaxSpeakerCodepoints)) {
            ++bad_speaker;
        }
    }
    EXPECT_EQ(dialogue_count, kExpectedDialogueEntries);
    EXPECT_EQ(narration_count, kExpectedNarrationEntries);
    EXPECT_EQ(ctrl, 0u) << "entries with control bytes in text";
    EXPECT_EQ(dup, 0u) << "duplicate offsets";
    EXPECT_EQ(not_cp932, 0u) << "keys that do not encode to CP932";
    EXPECT_EQ(bad_speaker, 0u) << "speaker fields holding dialogue, not a bare name";

    EXPECT_GE(strings[0].get_object().at("offset").get_int64(),
              static_cast<std::int64_t>(ex::GAME_START_OFFSET));
}

TEST(Extract, ExtractedJsonSerialisesWithIndentOneAndCrlf) {
    const fs::path p =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "extracted_text.json";
    if (!fs::exists(p)) {
        GTEST_SKIP() << "extracted_text.json not present at " << p.u8string();
    }

    ScopedTempDir scratch;
    const bj::value root = json_parse_file(p.u8string());
    const fs::path tmp = scratch.path() / "extracted_roundtrip.json";
    write_file_text(tmp.u8string(), json_pretty(root, kExtractedJsonIndent));

    const Bytes a = read_file(p.u8string());
    const Bytes b = read_file(tmp.u8string());
    EXPECT_EQ(a.size(), b.size());
    EXPECT_TRUE(a == b) << "json_pretty(v,1) + write_file_text does not reproduce the reference";
}

TEST(Translate, TranslatedOffsetsAllExistInExtractedCorpus) {
    const fs::path tp =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "translated_text.json";
    const fs::path ep =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "extracted_text.json";
    if (!fs::exists(tp) || !fs::exists(ep)) {
        GTEST_SKIP() << "Full translated_text.json check skipped (missing "
                     << (!fs::exists(tp) ? tp.u8string() : ep.u8string()) << ")";
    }

    const bj::value extracted = json_parse_file(ep.u8string());
    const bj::value translated = json_parse_file(tp.u8string());

    const auto& extracted_strings =
        extracted.get_object().at("sn.bin").get_object().at("strings").get_array();
    const auto& translated_strings =
        translated.get_object().at("sn.bin").get_object().at("strings").get_array();
    ASSERT_EQ(translated_strings.size(), extracted_strings.size());

    std::size_t mismatch = 0;
    for (std::size_t i = 0; i < extracted_strings.size(); ++i) {
        if (translated_strings[i].get_object().at("offset").get_int64() !=
            extracted_strings[i].get_object().at("offset").get_int64()) {
            ++mismatch;
        }
    }
    EXPECT_EQ(mismatch, 0u) << "translated_text.json offset sequence diverged from extracted_text.json";
}

TEST(Cg, PatchedBmpMatchesCanonicalHeader) {
    const fs::path ref =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "narrative_patched" / "00004.bmp";
    if (!fs::exists(ref)) {
        GTEST_SKIP() << "reference patched BMP not present at " << ref.u8string();
    }

    const cg::Image img = solid(kCanvasWidth, kCanvasHeight,
                                {kSampleRed, kSampleGreen, kSampleBlue});
    const Bytes bmp = cg::encode_bmp(img);
    const Bytes r = read_file(ref.u8string());
    ASSERT_GE(r.size(), kBmpHeaderBytes);
    for (std::size_t i = 0; i < kBmpHeaderBytes; ++i) {
        EXPECT_EQ(bmp[i], r[i]) << "header byte " << i;
    }
}

TEST(Extract, LiveGameDirSnBinReproducesReferenceCorpus) {
    wchar_t* raw_env = nullptr;
    std::size_t env_len = 0;
    if (::_wdupenv_s(&raw_env, &env_len, L"VN_GAME_DIR") != 0 || raw_env == nullptr) {
        GTEST_SKIP() << "Optional external game directory not set (VN_GAME_DIR)";
    }
    const std::wstring game_dir_w(raw_env);
    std::free(raw_env);
    if (game_dir_w.empty()) {
        GTEST_SKIP() << "Optional external game directory is empty (VN_GAME_DIR)";
    }

    const fs::path sn_bin_path = fs::path(game_dir_w) / "data" / "sn.bin";
    ASSERT_TRUE(fs::exists(sn_bin_path))
        << "VN_GAME_DIR is set, so data\\sn.bin must exist at " << sn_bin_path.u8string();

    ScopedTempDir scratch;
    const fs::path scratch_extracted = scratch.path() / "extracted_text.json";
    ASSERT_EQ(ex::run_extract(sn_bin_path.u8string(), scratch_extracted.u8string()),
              kBuildSuccessExitCode);

    const bj::value extracted = json_parse_file(scratch_extracted.u8string());
    const auto& strings =
        extracted.get_object().at("sn.bin").get_object().at("strings").get_array();
    EXPECT_EQ(strings.size(), kExpectedTotalExtractedEntries);

    const fs::path ref_extracted =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "extracted_text.json";
    if (fs::exists(ref_extracted)) {
        EXPECT_EQ(read_file(scratch_extracted.u8string()),
                  read_file(ref_extracted.u8string()));
    }
}

TEST(Cg, ExtractArchiveUsesCachedIndexWhenPresent) {
    const fs::path src_out =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output";
    const fs::path src_sys = src_out / "narrative_extracted" / "sys";
    const fs::path src_patched = src_out / "narrative_patched";
    const fs::path src_scanned = src_out / "narrative_scanned.json";
    const fs::path src_candidates = src_out / "narrative_candidates.json";
    if (!fs::is_directory(src_sys) || !fs::is_directory(src_patched) ||
        !fs::exists(src_scanned) || !fs::exists(src_candidates)) {
        GTEST_SKIP() << "narrative CG corpus artefacts not present under " << src_out.u8string();
    }

    ScopedTempDir scratch;
    const fs::path dst_out = scratch.path() / "script_output";
    const fs::path dst_sys = dst_out / "narrative_extracted" / "sys";
    const fs::path dst_patched = dst_out / "narrative_patched";
    fs::create_directories(dst_sys);
    fs::create_directories(dst_patched);
    fs::copy_file(src_scanned, dst_out / "narrative_scanned.json");
    fs::copy_file(src_candidates, dst_out / "narrative_candidates.json");
    for (const auto& entry : fs::directory_iterator(src_patched)) {
        if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), dst_patched / entry.path().filename());
        }
    }

    std::vector<fs::path> bmps;
    for (const auto& entry : fs::directory_iterator(src_sys)) {
        if (entry.is_regular_file() && entry.path().extension() == ".bmp") {
            bmps.push_back(entry.path());
        }
    }
    std::sort(bmps.begin(), bmps.end());
    ASSERT_FALSE(bmps.empty());

    bj::object index;
    for (const auto& bmp : bmps) {
        const auto decoded = cg::decode_image(read_file(bmp.u8string()));
        ASSERT_TRUE(decoded.has_value());
        index[bmp.stem().u8string()] =
            bj::array{decoded->width, decoded->height, bmp.u8string()};
    }

    const fs::path idx = dst_sys / "_index.json";
    write_file_text(idx.u8string(), json_pretty(bj::value(index), kCachedIndexIndent));

    const bj::value v = json_parse_file(idx.u8string());
    ASSERT_TRUE(v.is_object());
    ASSERT_EQ(v.get_object().size(), bmps.size());
    for (const auto& kv : v.get_object()) {
        ASSERT_TRUE(kv.value().is_array());
        ASSERT_EQ(kv.value().get_array().size(), kExpectedIndexTupleSize);
        EXPECT_TRUE(kv.value().get_array()[kIndexPathSlot].is_string());
    }

    cg::CgOptions opt;
    opt.game_dir = (scratch.path() / "uninstalled_game").u8string();
    opt.project_dir = scratch.path().u8string();
    opt.assume_yes = true;
    EXPECT_EQ(cg::run_find_narrative_cg(opt), kBuildSuccessExitCode);
}
