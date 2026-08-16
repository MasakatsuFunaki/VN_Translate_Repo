// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Integrity gate over the DEPLOYED extracted_text.json.
//
// Unlike ut_extract.cpp (which pins the writer's schema on synthetic input),
// this asserts against the real 443k-entry artifact: that the extraction
// captured the whole archive, in archive order, with the expected volume and
// density of Japanese. It is the check that would catch a decoder regression
// silently dropping strings -- exactly what the CP932 special-single bug did
// to the sibling BLACKCyc games.
//
// Skips when extracted_text.json has not been generated.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace ama;

namespace {

const bj::value& extracted() {
    static bj::value cached = [] {
        fs::path p = fs::u8path(std::string(AMA_PROJECT_DIR) +
                                "\\script_output\\extracted_text.json");
        if (!fs::exists(p)) return bj::value();
        return json_parse_file(p.u8string());
    }();
    return cached;
}

bool have_data() { return extracted().is_object() && !extracted().get_object().empty(); }

#define REQUIRE_DATA()                                                    \
    if (!have_data()) GTEST_SKIP() << "extracted_text.json not built yet"

// Hiragana / katakana / CJK unified -- what this gate counts as a Japanese
// character.
bool has_jp_char(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF))
            return true;
    }
    return false;
}

// Every codepoint is printable ASCII or fullwidth Latin.
bool all_latinish(const std::string& s) {
    std::size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        any = true;
        if (!((cp >= 0x20 && cp <= 0x7E) || (cp >= 0xFF01 && cp <= 0xFF5E))) return false;
    }
    return any;
}

struct Line {
    std::string type;
    std::string content;
    std::int64_t idx = 0;
};

// Walk every line once; the artifact is ~450 MB of JSON in memory, so callers
// share this rather than re-iterating per assertion.
template <typename F>
void for_each_line(F&& f) {
    for (const auto& kv : extracted().get_object()) {
        const auto& script = kv.value().get_object();
        const auto* lines = script.if_contains("lines");
        if (!lines || !lines->is_array()) continue;
        for (const auto& lv : lines->get_array()) {
            const auto& l = lv.get_object();
            Line line;
            if (auto* t = l.if_contains("type"))
                if (t->is_string()) line.type = std::string(t->get_string());
            if (auto* c = l.if_contains("content"))
                if (c->is_string()) line.content = std::string(c->get_string());
            if (auto* i = l.if_contains("idx"))
                if (i->is_int64()) line.idx = i->get_int64();
            f(std::string(kv.key()), line);
        }
    }
}

}  // namespace

// ── Structure ───────────────────────────────────────────────────────

TEST(ExtractedIntegrity, has_the_whole_archive) {
    REQUIRE_DATA();
    EXPECT_GE(extracted().get_object().size(), 3000u)
        << "far fewer scripts than Amakano 2 PE's archive holds";
}

TEST(ExtractedIntegrity, script_records_are_well_formed) {
    REQUIRE_DATA();
    for (const auto& kv : extracted().get_object()) {
        const std::string key(kv.key());
        ASSERT_FALSE(key.empty());
        ASSERT_NE(key.find('.'), std::string::npos) << "script key has no extension: " << key;
        const auto& s = kv.value().get_object();
        for (const char* f : {"arc_idx", "name", "offset", "size", "lines"})
            ASSERT_TRUE(s.contains(f)) << key << " missing " << f;
        EXPECT_EQ(std::string(s.at("name").get_string()), key) << "name field != key";
        EXPECT_GE(s.at("offset").get_int64(), 0);
        EXPECT_GT(s.at("size").get_int64(), 0);
        ASSERT_TRUE(s.at("lines").is_array());
    }
}

// arc_idx must be a dense 0..N-1 sequence in iteration order: a gap means a
// script failed to parse and was silently skipped.
TEST(ExtractedIntegrity, scripts_are_in_dense_archive_order) {
    REQUIRE_DATA();
    std::vector<std::int64_t> idxs;
    for (const auto& kv : extracted().get_object())
        idxs.push_back(kv.value().get_object().at("arc_idx").get_int64());
    ASSERT_FALSE(idxs.empty());

    std::set<std::int64_t> uniq(idxs.begin(), idxs.end());
    EXPECT_EQ(uniq.size(), idxs.size()) << "duplicate arc_idx";
    EXPECT_TRUE(std::is_sorted(idxs.begin(), idxs.end())) << "scripts not in archive order";
    EXPECT_EQ(idxs.front(), 0);
    EXPECT_EQ(idxs.back(), static_cast<std::int64_t>(idxs.size()) - 1);
}

TEST(ExtractedIntegrity, offsets_are_non_decreasing) {
    REQUIRE_DATA();
    std::int64_t prev = -1;
    for (const auto& kv : extracted().get_object()) {
        const std::int64_t off = kv.value().get_object().at("offset").get_int64();
        EXPECT_GE(off, prev) << "offset went backwards at " << std::string(kv.key());
        prev = off;
    }
}

TEST(ExtractedIntegrity, line_types_are_known) {
    REQUIRE_DATA();
    static const std::set<std::string> KNOWN = {"NONE",    "INPUT", "PAGE",
                                                "MESSAGE", "NAME",  "COMMAND"};
    std::set<std::string> unknown;
    for_each_line([&](const std::string&, const Line& l) {
        // Unrecognised kinds legitimately render as 0xNNNN.
        if (!KNOWN.count(l.type) && l.type.rfind("0x", 0) != 0) unknown.insert(l.type);
    });
    EXPECT_TRUE(unknown.empty())
        << "unknown line type(s), first: " << (unknown.empty() ? "" : *unknown.begin());
}

// ── Volume and density ──────────────────────────────────────────────
//
// These thresholds are the real regression barrier: a decoder that drops
// strings shows up here as a count shortfall.

TEST(ExtractedIntegrity, volume_and_japanese_density) {
    REQUIRE_DATA();
    std::size_t msg = 0, name = 0, msg_nonempty = 0, msg_jp = 0, msg_empty = 0;
    for_each_line([&](const std::string&, const Line& l) {
        if (l.type == "MESSAGE") {
            ++msg;
            if (trim(l.content).empty()) ++msg_empty;
            if (!l.content.empty()) {
                ++msg_nonempty;
                if (has_jp_char(l.content)) ++msg_jp;
            }
        } else if (l.type == "NAME") {
            ++name;
        }
    });

    EXPECT_GE(msg, 50000u) << "only " << msg << " MESSAGE lines";
    EXPECT_GE(name, 20000u) << "only " << name << " NAME lines";
    ASSERT_GT(msg_nonempty, 0u);
    const double jp_ratio = static_cast<double>(msg_jp) / msg_nonempty;
    EXPECT_GT(jp_ratio, 0.90) << "only " << jp_ratio * 100 << "% of MESSAGE lines are Japanese";
    ASSERT_GT(msg, 0u);
    const double empty_ratio = static_cast<double>(msg_empty) / msg;
    EXPECT_LT(empty_ratio, 0.05) << empty_ratio * 100 << "% of MESSAGE lines are blank";
}

TEST(ExtractedIntegrity, name_lines_are_recognisable_text) {
    REQUIRE_DATA();
    std::size_t bad = 0;
    for_each_line([&](const std::string&, const Line& l) {
        if (l.type != "NAME" || l.content.empty()) return;
        const bool jp = has_jp_char(l.content);
        const bool var = l.content[0] == '$';
        const bool unknown_marker =
            l.content == u8"？？？" || l.content == "???";
        if (!(jp || var || unknown_marker || all_latinish(l.content))) ++bad;
    });
    EXPECT_LT(bad, 10u) << bad << " NAME lines have no recognisable text";
}

TEST(ExtractedIntegrity, core_character_names_present) {
    REQUIRE_DATA();
    std::set<std::string> names;
    for_each_line([&](const std::string&, const Line& l) {
        if (l.type == "NAME") names.insert(l.content);
    });
    for (const char* n : {u8"ちとせ", u8"ゆうひ", u8"玲", u8"結灯"})
        EXPECT_TRUE(names.count(n)) << "character name missing from every NAME line: " << n;
}

// A corrupted decode can collapse many distinct strings into one value.
TEST(ExtractedIntegrity, message_content_is_varied) {
    REQUIRE_DATA();
    std::set<std::string> sample;
    for_each_line([&](const std::string&, const Line& l) {
        if (sample.size() >= 100) return;
        if (l.type == "MESSAGE" && !l.content.empty()) sample.insert(l.content);
    });
    EXPECT_GT(sample.size(), 50u) << "only " << sample.size()
                                  << " distinct MESSAGE strings in sample -- possible corruption";
}
