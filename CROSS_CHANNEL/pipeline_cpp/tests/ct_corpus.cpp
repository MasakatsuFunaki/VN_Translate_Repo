// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests validating pipeline stages against tracked corpus artefacts
// in script_output/ and staged build/install/game/translations.tsv.
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>

namespace bj = boost::json;
namespace bt = crc::build_tsv;
namespace ex = crc::extract;
namespace fs = std::filesystem;
namespace tr = crc::translate;
using namespace crc;

namespace {

constexpr const char* kTaichi = "太一";
constexpr const char* kOpenQuote = "「";
constexpr std::size_t kHexByteBufSize = 3;
constexpr char32_t kControlCharLimit = 0x20;
constexpr std::size_t kMaxSpeakerCodepoints = 20;
constexpr int kBuildSuccessExitCode = 0;
constexpr std::uint8_t kCarriageReturnByte = 0x0D;
constexpr std::size_t kExpectedTsvBytes = 4073036u;
constexpr std::size_t kExpectedTsvRows = 43256u;
constexpr std::size_t kExpectedCacheEntries = 43256u;
constexpr std::size_t kExpectedCacheBytes = 4507820u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name =
            "crc_ct_corpus_" + std::to_string(++seq);
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

std::string sha256_hex(const Bytes& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), digest);
    std::string out;
    char buf[kHexByteBufSize];
    for (const unsigned char b : digest) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

bool is_smart_punctuation(char32_t cp) {
    return cp == U'—' || cp == U'–' || cp == U'…' || cp == U'‘' ||
           cp == U'’' || cp == U'“' || cp == U'”' || cp == U'　';
}

}  // namespace

TEST(Translate, DeployedTranslatedJsonInvariants) {
    const fs::path tp =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(tp)) << tp.u8string();

    const bj::value translated = json_parse_file(tp.u8string());
    ASSERT_TRUE(translated.is_object());
    ASSERT_TRUE(translated.get_object().if_contains("sn.bin"));
    const auto& block = translated.get_object().at("sn.bin").get_object();
    ASSERT_TRUE(block.if_contains("strings"));
    const auto& strings = block.at("strings").get_array();
    ASSERT_FALSE(strings.empty());
    for (const char* k : {"offset", "type", "speaker", "text", "translated"}) {
        EXPECT_TRUE(strings[0].get_object().if_contains(k)) << k;
    }

    std::set<std::int64_t> offsets;
    std::size_t dup = 0;
    std::size_t ctrl = 0;
    std::size_t not_cp932 = 0;
    std::size_t bad_speaker = 0;
    std::size_t smart = 0;

    for (const auto& sv : strings) {
        const auto& s = sv.get_object();
        if (!offsets.insert(s.at("offset").get_int64()).second) {
            ++dup;
        }

        const std::string jp(s.at("text").get_string());
        const std::string speaker(s.at("speaker").get_string());
        const std::string en(s.at("translated").get_string());

        std::size_t k = 0;
        while (k < jp.size()) {
            const char32_t cp = utf8_next(jp, k);
            if (cp < kControlCharLimit && cp != '\r' && cp != '\n' && cp != '\t') {
                ++ctrl;
                break;
            }
        }
        if (!utf8_to_cp932_strict(jp)) {
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

        if (en.empty() || en == jp) {
            continue;
        }
        std::size_t i = 0;
        while (i < en.size()) {
            const char32_t cp = utf8_next(en, i);
            if (is_smart_punctuation(cp)) {
                ++smart;
                break;
            }
        }
    }

    EXPECT_EQ(dup, 0u) << "duplicate offsets in translated_text.json";
    EXPECT_EQ(ctrl, 0u) << "entries with control bytes in source text";
    EXPECT_EQ(not_cp932, 0u) << "keys that do not encode to CP932";
    EXPECT_EQ(bad_speaker, 0u) << "speaker fields holding dialogue, not a bare name";
    EXPECT_EQ(smart, 0u) << "translations still carrying smart punctuation";
    EXPECT_GE(strings[0].get_object().at("offset").get_int64(),
              static_cast<std::int64_t>(ex::GAME_START_OFFSET));
}

TEST(Translate, SaveCacheWritesIndentTwoCrlfInInsertionOrder) {
    const fs::path ref = fs::u8path(std::string(CRC_PROJECT_DIR)) /
                         "script_output" / "translation_cache_anthropic.json";
    ASSERT_TRUE(fs::exists(ref)) << ref.u8string();

    const tr::Cache cache = tr::load_cache(ref.u8string());
    ASSERT_EQ(cache.size(), kExpectedCacheEntries);
    EXPECT_EQ(cache.items()[0].first, kTaichi);
    EXPECT_EQ(cache.items()[0].second, "Taichi");

    ScopedTempDir scratch;
    const fs::path tmp = scratch.path() / "cache_roundtrip.json";
    tr::save_cache(cache, tmp.u8string());
    const Bytes out = read_file(tmp.u8string());

    EXPECT_EQ(out.size(), kExpectedCacheBytes);
    EXPECT_EQ(sha256_hex(out),
              "3b6414a9b601c7cecc9a4f90215b23e85cc7bfc1c84fbd35370cef6bf9222f28");
}

TEST(BuildTsv, RealTranslatedJsonProducesReferenceTsv) {
    const fs::path in =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(in)) << in.u8string();

    ScopedTempDir scratch;
    const std::string out = (scratch.path() / "translations.tsv").u8string();
    ASSERT_EQ(
        bt::run_build(in.u8string(), out, (scratch.path() / "no_such_game" / "t.tsv").u8string()),
        kBuildSuccessExitCode);

    const Bytes raw = read_file(out);
    EXPECT_EQ(raw.size(), kExpectedTsvBytes);
    std::size_t rows = 0;
    for (const std::uint8_t b : raw) {
        EXPECT_NE(b, kCarriageReturnByte);
        if (b == '\n') {
            ++rows;
        }
    }
    EXPECT_EQ(rows, kExpectedTsvRows);

    const fs::path staged =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "build" / "install" / "game" / "translations.tsv";
    ASSERT_TRUE(fs::exists(staged)) << staged.u8string();
    const Bytes staged_raw = read_file(staged.u8string());
    EXPECT_EQ(staged_raw, raw);
}
