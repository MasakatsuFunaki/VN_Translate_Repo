// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests validating CROSS_CHANNEL pipeline artefacts on disk across
// both pre-full-run smoke state (extracted_text.json + partial Gemini cache)
// and full-corpus completion (43,256+ cache entries, 53,635 translated strings,
// and 43,256-row translations.tsv).
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
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
constexpr char32_t kControlCharLimit = 0x20;
constexpr std::size_t kMaxSpeakerCodepoints = 20u;
constexpr int kBuildSuccessExitCode = 0;
constexpr std::uint8_t kCarriageReturnByte = 0x0D;

constexpr std::size_t kExpectedTotalExtractedEntries = 53635u;
constexpr std::size_t kExpectedDialogueEntries = 28787u;
constexpr std::size_t kExpectedNarrationEntries = 24848u;
constexpr std::size_t kExpectedGlossaryEntries = 75u;
constexpr std::size_t kExpectedFullCacheEntries = 43256u;
constexpr std::size_t kExpectedFullTsvRows = 43256u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name = "crc_ct_corpus_" + std::to_string(++seq);
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

bool is_smart_punctuation(char32_t cp) {
    return cp == U'—' || cp == U'–' || cp == U'…' || cp == U'‘' ||
           cp == U'’' || cp == U'“' || cp == U'”' || cp == U'　';
}

bool contains_smart_punctuation(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (is_smart_punctuation(utf8_next(text, i))) {
            return true;
        }
    }
    return false;
}

std::size_t count_percent_n(std::string_view s) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = s.find("%N", pos)) != std::string_view::npos) {
        ++count;
        pos += 2u;
    }
    return count;
}

}  // namespace

TEST(ExtractCorpus, ExtractedJsonFullCorpusInvariantsAndSpeakerGate) {
    const fs::path ep =
        fs::u8path(std::string(CRC_PROJECT_DIR)) / "script_output" / "extracted_text.json";
    if (!fs::exists(ep)) {
        GTEST_SKIP() << "Optional workspace corpus absent on clean runner: " << ep.u8string();
    }

    const bj::value extracted = json_parse_file(ep.u8string());
    ASSERT_TRUE(extracted.is_object());
    const bj::object& root_obj = extracted.get_object();
    ASSERT_TRUE(root_obj.if_contains("sn.bin"));
    const bj::object& block = root_obj.at("sn.bin").get_object();
    ASSERT_TRUE(block.if_contains("strings"));
    const bj::array& strings = block.at("strings").get_array();
    ASSERT_EQ(strings.size(), kExpectedTotalExtractedEntries);

    for (const char* key : {"offset", "type", "speaker", "text"}) {
        EXPECT_TRUE(strings[0].get_object().if_contains(key)) << key;
    }

    const auto& glossary = tr::name_translations();
    std::set<std::int64_t> offsets;
    std::size_t dialogue_count = 0;
    std::size_t narration_count = 0;
    std::size_t dup = 0;
    std::size_t ctrl = 0;
    std::size_t not_cp932 = 0;
    std::size_t bad_speaker = 0;

    for (const auto& sv : strings) {
        const bj::object& s = sv.get_object();
        if (!offsets.insert(s.at("offset").get_int64()).second) {
            ++dup;
        }

        const std::string type(s.at("type").get_string());
        const std::string speaker(s.at("speaker").get_string());
        const std::string text(s.at("text").get_string());

        if (type == "dialogue") {
            ++dialogue_count;
            if (!speaker.empty() && glossary.find(speaker) == glossary.end()) {
                ++bad_speaker;
            }
        } else if (type == "narration") {
            ++narration_count;
            if (!speaker.empty()) {
                ++bad_speaker;
            }
        } else {
            ++bad_speaker;
        }

        std::size_t k = 0;
        while (k < text.size()) {
            const char32_t cp = utf8_next(text, k);
            if (cp < kControlCharLimit && cp != '\r' && cp != '\n' && cp != '\t') {
                ++ctrl;
                break;
            }
        }
        if (!utf8_to_cp932_strict(text).has_value()) {
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
    EXPECT_EQ(dup, 0u) << "duplicate offsets in extracted_text.json";
    EXPECT_EQ(ctrl, 0u) << "entries with control bytes in extracted_text.json";
    EXPECT_EQ(not_cp932, 0u) << "entries not encodable in CP932";
    EXPECT_EQ(bad_speaker, 0u) << "invalid speaker metadata in extracted_text.json";
    EXPECT_GE(strings[0].get_object().at("offset").get_int64(),
              static_cast<std::int64_t>(ex::GAME_START_OFFSET));

    const tr::SpeakerCheckReport gate = tr::run_speaker_checks(root_obj);
    EXPECT_TRUE(gate.passed) << gate.reason;
    EXPECT_EQ(gate.failed_check, 0);
    EXPECT_EQ(gate.dialogue_total, kExpectedDialogueEntries);
    EXPECT_EQ(gate.speakers_in_data, kExpectedGlossaryEntries);
    EXPECT_EQ(gate.glossary_entries, kExpectedGlossaryEntries);
}

TEST(TranslateCorpus, GeminiCacheInvariantsAndRoundTrip) {
    const fs::path cache_path = fs::u8path(std::string(CRC_PROJECT_DIR)) /
                                "script_output" / "translation_cache_gemini.json";
    if (!fs::exists(cache_path)) {
        GTEST_SKIP() << "translation_cache_gemini.json not yet created at "
                     << cache_path.u8string();
    }

    const tr::Cache cache = tr::load_cache(cache_path.u8string());
    ASSERT_GE(cache.size(), kExpectedGlossaryEntries);
    EXPECT_EQ(cache.items()[0].first, kTaichi);
    EXPECT_EQ(cache.items()[0].second, "Taichi");

    // Verify all 75 pinned glossary entries match verbatim.
    for (const auto& [jp_name, en_name] : tr::name_translations_ordered()) {
        const std::string* got = cache.get(jp_name);
        ASSERT_NE(got, nullptr) << "Missing glossary key in cache: " << jp_name;
        EXPECT_EQ(*got, en_name) << "Glossary mismatch for: " << jp_name;
    }

    // Validate every cached entry (works on both 195-entry smoke cache and full 43,256+ cache).
    std::size_t not_cp932 = 0;
    std::size_t smart_punct = 0;
    std::size_t jp_leak = 0;
    std::size_t failed_identity = 0;
    std::size_t broken_percent_n = 0;

    for (const auto& [jp, en] : cache.items()) {
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(en.empty());
        if (!utf8_to_cp932_strict(jp).has_value()) {
            ++not_cp932;
        }
        if (tr::is_failed_entry(jp, en)) {
            ++failed_identity;
        }
        if (en != jp) {
            if (contains_smart_punctuation(en)) {
                ++smart_punct;
            }
            if (tr::has_real_japanese(en)) {
                ++jp_leak;
            }
            if (jp.find("%N") != std::string::npos &&
                count_percent_n(en) != count_percent_n(jp)) {
                ++broken_percent_n;
            }
        }
    }

    EXPECT_EQ(not_cp932, 0u) << "cache keys not encodable in CP932";
    EXPECT_EQ(failed_identity, 0u) << "untranslated Japanese identity entries in cache";
    EXPECT_EQ(smart_punct, 0u) << "smart punctuation leaked into cache values";
    EXPECT_EQ(jp_leak, 0u) << "Japanese kana/kanji leaked into non-identity cache values";
    EXPECT_EQ(broken_percent_n, 0u) << "%N line-break count mismatch in cache values";

    // Lossless insertion-ordered CRLF JSON round-trip.
    ScopedTempDir scratch;
    const fs::path roundtrip_path = scratch.path() / "cache_roundtrip.json";
    tr::save_cache(cache, roundtrip_path.u8string());

    const tr::Cache reloaded = tr::load_cache(roundtrip_path.u8string());
    EXPECT_EQ(reloaded.items(), cache.items());
    EXPECT_EQ(read_file(roundtrip_path.u8string()), read_file(cache_path.u8string()));
}

TEST(TranslateCorpus, FullCorpusCompletionAndTsvParity) {
    const fs::path project_root = fs::u8path(std::string(CRC_PROJECT_DIR));
    const fs::path cache_path =
        project_root / "script_output" / "translation_cache_gemini.json";
    const fs::path tp = project_root / "script_output" / "translated_text.json";

    if (!fs::exists(cache_path) || !fs::exists(tp)) {
        GTEST_SKIP() << "Full-corpus completion check skipped during smoke/pre-run phase "
                        "(missing "
                     << (!fs::exists(cache_path) ? cache_path.u8string() : tp.u8string())
                     << ")";
    }

    const tr::Cache cache = tr::load_cache(cache_path.u8string());
    if (cache.size() < kExpectedFullCacheEntries) {
        GTEST_SKIP() << "Full-corpus completion check skipped during smoke phase: cache has "
                     << cache.size() << " entries (< " << kExpectedFullCacheEntries << ")";
    }

    const bj::value translated = json_parse_file(tp.u8string());
    ASSERT_TRUE(translated.is_object());
    ASSERT_TRUE(translated.get_object().if_contains("sn.bin"));
    const bj::object& block = translated.get_object().at("sn.bin").get_object();
    ASSERT_TRUE(block.if_contains("strings"));
    const bj::array& strings = block.at("strings").get_array();
    ASSERT_EQ(strings.size(), kExpectedTotalExtractedEntries);

    for (const char* key : {"offset", "type", "speaker", "text", "translated"}) {
        EXPECT_TRUE(strings[0].get_object().if_contains(key)) << key;
    }

    std::set<std::int64_t> offsets;
    std::size_t dup = 0;
    std::size_t ctrl = 0;
    std::size_t not_cp932 = 0;
    std::size_t bad_speaker = 0;
    std::size_t smart = 0;
    std::size_t jp_leak = 0;

    for (const auto& sv : strings) {
        const bj::object& s = sv.get_object();
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
        if (!utf8_to_cp932_strict(jp).has_value()) {
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
        if (contains_smart_punctuation(en)) {
            ++smart;
        }
        if (tr::has_real_japanese(en)) {
            ++jp_leak;
        }
    }

    EXPECT_EQ(dup, 0u) << "duplicate offsets in translated_text.json";
    EXPECT_EQ(ctrl, 0u) << "entries with control bytes in source text";
    EXPECT_EQ(not_cp932, 0u) << "keys that do not encode to CP932";
    EXPECT_EQ(bad_speaker, 0u) << "speaker fields holding dialogue, not a bare name";
    EXPECT_EQ(smart, 0u) << "translations still carrying smart punctuation";
    EXPECT_EQ(jp_leak, 0u) << "translations leaking Japanese kana/kanji";
    EXPECT_GE(strings[0].get_object().at("offset").get_int64(),
              static_cast<std::int64_t>(ex::GAME_START_OFFSET));

    ScopedTempDir scratch;
    const std::string out_tsv = (scratch.path() / "translations.tsv").u8string();
    ASSERT_EQ(
        bt::run_build(tp.u8string(), out_tsv,
                      (scratch.path() / "no_such_game" / "translations.tsv").u8string()),
        kBuildSuccessExitCode);

    const Bytes raw = read_file(out_tsv);
    std::size_t rows = 0;
    for (const std::uint8_t b : raw) {
        EXPECT_NE(b, kCarriageReturnByte);
        if (b == '\n') {
            ++rows;
        }
    }
    EXPECT_EQ(rows, kExpectedFullTsvRows);

    const fs::path staged =
        project_root / "build" / "install" / "game" / "translations.tsv";
    if (fs::exists(staged)) {
        EXPECT_EQ(read_file(staged.u8string()), raw);
    }
}
