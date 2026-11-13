// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component integrity tests over script_output/translated_text.json:
// verifies schema invariants, dense archive ordering, and expected volume and
// Japanese character density across the extracted and translated corpus.
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace ama;

namespace {

constexpr char32_t kHiraganaFirst = 0x3040;
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;

constexpr char32_t kPrintableAsciiFirst = 0x20;
constexpr char32_t kPrintableAsciiLast = 0x7E;
constexpr char32_t kFullwidthLatinFirst = 0xFF01;
constexpr char32_t kFullwidthLatinLast = 0xFF5E;

constexpr std::size_t kSchemaSampleLimit = 50;
constexpr std::size_t kMinArchiveScripts = 3000;
constexpr std::size_t kMinMessageLines = 50000;
constexpr std::size_t kMinNameLines = 20000;
constexpr double kMinJapaneseMessageRatio = 0.90;
constexpr double kMaxBlankMessageRatio = 0.05;
constexpr double kPercentScale = 100.0;
constexpr std::size_t kMaxUnrecognisedNameLines = 10;
constexpr std::size_t kVariedMessageSampleCap = 100;
constexpr std::size_t kMinDistinctSampledMessages = 50;
constexpr std::int64_t kInitialPrevOffset = -1;

fs::path corpus_path() {
    return fs::u8path(AMA_PROJECT_DIR) / "script_output" / "translated_text.json";
}

const bj::value& extracted() {
    static const bj::value cached = [] {
        const fs::path p = corpus_path();
        if (!fs::exists(p)) {
            return bj::value();
        }
        return json_parse_file(p.u8string());
    }();
    return cached;
}

bool have_data() {
    return extracted().is_object() && !extracted().get_object().empty();
}

bool has_jp_char(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        if ((cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
            (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
            (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast)) {
            return true;
        }
    }
    return false;
}

bool all_latinish(const std::string& s) {
    std::size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        any = true;
        if (!((cp >= kPrintableAsciiFirst && cp <= kPrintableAsciiLast) ||
              (cp >= kFullwidthLatinFirst && cp <= kFullwidthLatinLast))) {
            return false;
        }
    }
    return any;
}

struct Line {
    std::string type;
    std::string content;
    std::int64_t idx = 0;
};

template <typename F>
void for_each_line(F&& f) {
    for (const auto& kv : extracted().get_object()) {
        const auto& script = kv.value().get_object();
        const auto* lines = script.if_contains("lines");
        if (!lines || !lines->is_array()) {
            continue;
        }
        for (const auto& lv : lines->get_array()) {
            const auto& l = lv.get_object();
            Line line;
            if (const auto* t = l.if_contains("type")) {
                if (t->is_string()) {
                    line.type = std::string(t->get_string());
                }
            }
            if (const auto* c = l.if_contains("content")) {
                if (c->is_string()) {
                    line.content = std::string(c->get_string());
                }
            }
            if (const auto* i = l.if_contains("idx")) {
                if (i->is_int64()) {
                    line.idx = i->get_int64();
                }
            }
            f(std::string(kv.key()), line);
        }
    }
}

}  // namespace

TEST(Extract, deployed_extracted_json_has_entries) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    bool has_entries = false;
    for (const auto& kv : extracted().get_object()) {
        if (!kv.value().is_object()) {
            continue;
        }
        if (const auto* lines = kv.value().get_object().if_contains("lines")) {
            if (lines->is_array() && !lines->get_array().empty()) {
                has_entries = true;
                break;
            }
        }
    }
    EXPECT_TRUE(has_entries) << "no archives with a non-empty lines container";
}

TEST(Extract, deployed_line_entries_have_expected_schema) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    std::size_t checked = 0;
    for (const auto& kv : extracted().get_object()) {
        const auto& script = kv.value().get_object();
        for (const char* f : {"arc_idx", "name", "offset", "size", "lines"}) {
            ASSERT_TRUE(script.contains(f)) << "script missing " << f;
        }
        for (const auto& lv : script.at("lines").get_array()) {
            std::vector<std::string> keys;
            for (const auto& e : lv.get_object()) {
                keys.emplace_back(e.key());
            }
            ASSERT_EQ(keys,
                      (std::vector<std::string>{
                          "idx", "type", "type_val", "content", "translated"}));
            if (++checked >= kSchemaSampleLimit) {
                return;
            }
        }
    }
}

TEST(ExtractedIntegrity, has_the_whole_archive) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    EXPECT_GE(extracted().get_object().size(), kMinArchiveScripts)
        << "far fewer scripts than Amakano 2 PE's archive holds";
}

TEST(ExtractedIntegrity, script_records_are_well_formed) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    for (const auto& kv : extracted().get_object()) {
        const std::string key(kv.key());
        ASSERT_FALSE(key.empty());
        ASSERT_NE(key.find('.'), std::string::npos) << "script key has no extension: " << key;
        const auto& s = kv.value().get_object();
        for (const char* f : {"arc_idx", "name", "offset", "size", "lines"}) {
            ASSERT_TRUE(s.contains(f)) << key << " missing " << f;
        }
        EXPECT_EQ(std::string(s.at("name").get_string()), key) << "name field != key";
        EXPECT_GE(s.at("offset").get_int64(), 0);
        EXPECT_GT(s.at("size").get_int64(), 0);
        ASSERT_TRUE(s.at("lines").is_array());
    }
}

// arc_idx must be a dense 0..N-1 sequence in iteration order: a gap means a
// script failed to parse and was silently skipped.
TEST(ExtractedIntegrity, scripts_are_in_dense_archive_order) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    std::vector<std::int64_t> idxs;
    for (const auto& kv : extracted().get_object()) {
        idxs.push_back(kv.value().get_object().at("arc_idx").get_int64());
    }
    ASSERT_FALSE(idxs.empty());

    const std::set<std::int64_t> uniq(idxs.begin(), idxs.end());
    EXPECT_EQ(uniq.size(), idxs.size()) << "duplicate arc_idx";
    EXPECT_TRUE(std::is_sorted(idxs.begin(), idxs.end())) << "scripts not in archive order";
    EXPECT_EQ(idxs.front(), 0);
    EXPECT_EQ(idxs.back(), static_cast<std::int64_t>(idxs.size()) - 1);
}

TEST(ExtractedIntegrity, offsets_are_non_decreasing) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    std::int64_t prev = kInitialPrevOffset;
    for (const auto& kv : extracted().get_object()) {
        const std::int64_t off = kv.value().get_object().at("offset").get_int64();
        EXPECT_GE(off, prev) << "offset went backwards at " << std::string(kv.key());
        prev = off;
    }
}

TEST(ExtractedIntegrity, line_types_are_known) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    static const std::set<std::string> known = {"NONE",    "INPUT", "PAGE",
                                                "MESSAGE", "NAME",  "COMMAND"};
    std::set<std::string> unknown;
    for_each_line([&](const std::string&, const Line& l) {
        if (!known.count(l.type) && l.type.rfind("0x", 0) != 0) {
            unknown.insert(l.type);
        }
    });
    EXPECT_TRUE(unknown.empty())
        << "unknown line type(s), first: " << (unknown.empty() ? "" : *unknown.begin());
}

TEST(ExtractedIntegrity, volume_and_japanese_density) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    std::size_t msg = 0;
    std::size_t name = 0;
    std::size_t msg_nonempty = 0;
    std::size_t msg_jp = 0;
    std::size_t msg_empty = 0;
    for_each_line([&](const std::string&, const Line& l) {
        if (l.type == "MESSAGE") {
            ++msg;
            if (trim(l.content).empty()) {
                ++msg_empty;
            }
            if (!l.content.empty()) {
                ++msg_nonempty;
                if (has_jp_char(l.content)) {
                    ++msg_jp;
                }
            }
            return;
        }
        if (l.type == "NAME") {
            ++name;
        }
    });

    EXPECT_GE(msg, kMinMessageLines) << "only " << msg << " MESSAGE lines";
    EXPECT_GE(name, kMinNameLines) << "only " << name << " NAME lines";
    ASSERT_GT(msg_nonempty, 0u);
    const double jp_ratio = static_cast<double>(msg_jp) / static_cast<double>(msg_nonempty);
    EXPECT_GT(jp_ratio, kMinJapaneseMessageRatio)
        << "only " << jp_ratio * kPercentScale << "% of MESSAGE lines are Japanese";
    ASSERT_GT(msg, 0u);
    const double empty_ratio = static_cast<double>(msg_empty) / static_cast<double>(msg);
    EXPECT_LT(empty_ratio, kMaxBlankMessageRatio)
        << empty_ratio * kPercentScale << "% of MESSAGE lines are blank";
}

TEST(ExtractedIntegrity, name_lines_are_recognisable_text) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    std::size_t bad = 0;
    for_each_line([&](const std::string&, const Line& l) {
        if (l.type != "NAME" || l.content.empty()) {
            return;
        }
        const bool jp = has_jp_char(l.content);
        const bool var = l.content[0] == '$';
        const bool unknown_marker = (l.content == "？？？" || l.content == "???");
        if (!(jp || var || unknown_marker || all_latinish(l.content))) {
            ++bad;
        }
    });
    EXPECT_LT(bad, kMaxUnrecognisedNameLines)
        << bad << " NAME lines have no recognisable text";
}

TEST(ExtractedIntegrity, core_character_names_present) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    std::set<std::string> names;
    for_each_line([&](const std::string&, const Line& l) {
        if (l.type == "NAME") {
            names.insert(l.content);
        }
    });
    for (const char* n : {"ちとせ", "ゆうひ", "玲", "結灯"}) {
        EXPECT_TRUE(names.count(n)) << "character name missing from every NAME line: " << n;
    }
}

TEST(ExtractedIntegrity, message_content_is_varied) {
    ASSERT_TRUE(fs::exists(corpus_path())) << corpus_path().u8string();
    ASSERT_TRUE(have_data());
    std::set<std::string> sample;
    for_each_line([&](const std::string&, const Line& l) {
        if (sample.size() >= kVariedMessageSampleCap) {
            return;
        }
        if (l.type == "MESSAGE" && !l.content.empty()) {
            sample.insert(l.content);
        }
    });
    EXPECT_GT(sample.size(), kMinDistinctSampledMessages)
        << "only " << sample.size()
        << " distinct MESSAGE strings in sample -- possible corruption";
}
