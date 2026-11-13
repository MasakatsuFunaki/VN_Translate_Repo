// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests verifying corpus JSON and deployed TSV artifacts.
#include "build_tsv/box_budget.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/coverage.h"
#include "common/util.h"

#include <boost/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bt = snu::build_tsv;
namespace tr = snu::translate;
namespace bj = boost::json;

constexpr std::size_t kMinExtractedScriptCount = 50;
constexpr std::size_t kSampledEntriesPerScript = 3;
constexpr std::int64_t kMinTotalExtractedStrings = 30'000;
constexpr std::int64_t kMaxTotalExtractedStrings = 200'000;
constexpr std::size_t kMaxLoggedBadTsvLines = 5;
constexpr std::size_t kLogPreviewMaxBytes = 80;
constexpr std::size_t kShortPreviewMaxBytes = 60;
constexpr std::string_view kBacklogNameOpenBracket = "【";
constexpr std::size_t kMinBracketedNameBytes = 7;
constexpr std::int64_t kExpectedCorpusTotalStrings = 68129;
constexpr std::size_t kExpectedDistinctExemptLines = 9;
constexpr std::int64_t kExpectedExemptEntries = 78;
constexpr std::int64_t kExpectedInternalBreakLines = 6633;
constexpr std::int64_t kExpectedLinesWithBreaks = 8271;
constexpr std::size_t kExpectedSpeakersInData = 143;
constexpr std::size_t kExpectedGlossaryEntries = 143;
constexpr std::size_t kExpectedDialogueTotal = 48758;
constexpr std::size_t kExpectedDialogueSampled = 50;
constexpr unsigned char kAsciiLimit = 0x80;
constexpr unsigned char kTwoByteLeadPrefix = 0x6;
constexpr int kTwoByteLeadShift = 5;
constexpr unsigned char kThreeByteLeadPrefix = 0xE;
constexpr int kThreeByteLeadShift = 4;
constexpr std::size_t kTwoByteUtf8Len = 2;
constexpr std::size_t kThreeByteUtf8Len = 3;
constexpr std::size_t kFourByteUtf8Len = 4;
constexpr std::size_t kYouonCodePoints = 2;
constexpr std::size_t kRubyOpenPrefixLen = 3;
constexpr std::size_t kRubyCloseTagLen = 4;
constexpr std::size_t kQuartileDivisor = 4;
constexpr std::size_t kMidpointDivisor = 2;
constexpr std::size_t kThreeQuarterNumerator = 3;

std::map<std::string, std::vector<std::string>> read_tsv(const std::string& path) {
    std::map<std::string, std::vector<std::string>> rows;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) {
            continue;
        }
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        rows[line.substr(0, tab)].push_back(line.substr(tab + 1));
    }
    return rows;
}

std::vector<std::size_t> boundary_indices(std::size_t n) {
    if (n == 0) {
        return {};
    }
    const std::size_t q1 = n / kQuartileDivisor;
    const std::size_t mid = n / kMidpointDivisor;
    const std::size_t q3 = (kThreeQuarterNumerator * n) / kQuartileDivisor;
    const std::size_t candidates[] = {
        0,
        1,
        q1 > 0 ? q1 - 1 : 0,
        q1,
        q1 + 1,
        mid > 0 ? mid - 1 : 0,
        mid,
        mid + 1,
        q3 > 0 ? q3 - 1 : 0,
        q3,
        q3 + 1,
        n > 1 ? n - 2 : 0,
        n - 1,
    };
    std::set<std::size_t> unique;
    for (std::size_t idx : candidates) {
        if (idx < n) {
            unique.insert(idx);
        }
    }
    return {unique.begin(), unique.end()};
}

std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = kFourByteUtf8Len;
        if (c < kAsciiLimit) {
            n = 1;
        } else if ((c >> kTwoByteLeadShift) == kTwoByteLeadPrefix) {
            n = kTwoByteUtf8Len;
        } else if ((c >> kThreeByteLeadShift) == kThreeByteLeadPrefix) {
            n = kThreeByteUtf8Len;
        }
        if (i + n > s.size()) {
            n = 1;
        }
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

struct Mora {
    const char* kana;
    const char* romaji;
};

const Mora YOUON[] = {
    {"きゃ", "kya"}, {"きゅ", "kyu"}, {"きょ", "kyo"}, {"ぎゃ", "gya"}, {"ぎゅ", "gyu"},
    {"ぎょ", "gyo"}, {"しゃ", "sha"}, {"しゅ", "shu"}, {"しょ", "sho"}, {"じゃ", "ja"},
    {"じゅ", "ju"},  {"じょ", "jo"},  {"ちゃ", "cha"}, {"ちゅ", "chu"}, {"ちょ", "cho"},
    {"にゃ", "nya"}, {"にゅ", "nyu"}, {"にょ", "nyo"}, {"ひゃ", "hya"}, {"ひゅ", "hyu"},
    {"ひょ", "hyo"}, {"びゃ", "bya"}, {"びゅ", "byu"}, {"びょ", "byo"}, {"ぴゃ", "pya"},
    {"ぴゅ", "pyu"}, {"ぴょ", "pyo"}, {"みゃ", "mya"}, {"みゅ", "myu"}, {"みょ", "myo"},
    {"りゃ", "rya"}, {"りゅ", "ryu"}, {"りょ", "ryo"},
};

const Mora KANA[] = {
    {"あ", "a"},  {"い", "i"},   {"う", "u"},   {"え", "e"},  {"お", "o"},
    {"か", "ka"}, {"き", "ki"},  {"く", "ku"},  {"け", "ke"}, {"こ", "ko"},
    {"が", "ga"}, {"ぎ", "gi"},  {"ぐ", "gu"},  {"げ", "ge"}, {"ご", "go"},
    {"さ", "sa"}, {"し", "shi"}, {"す", "su"},  {"せ", "se"}, {"そ", "so"},
    {"ざ", "za"}, {"じ", "ji"},  {"ず", "zu"},  {"ぜ", "ze"}, {"ぞ", "zo"},
    {"た", "ta"}, {"ち", "chi"}, {"つ", "tsu"}, {"て", "te"}, {"と", "to"},
    {"だ", "da"}, {"ぢ", "ji"},  {"づ", "zu"},  {"で", "de"}, {"ど", "do"},
    {"な", "na"}, {"に", "ni"},  {"ぬ", "nu"},  {"ね", "ne"}, {"の", "no"},
    {"は", "ha"}, {"ひ", "hi"},  {"ふ", "fu"},  {"へ", "he"}, {"ほ", "ho"},
    {"ば", "ba"}, {"び", "bi"},  {"ぶ", "bu"},  {"べ", "be"}, {"ぼ", "bo"},
    {"ぱ", "pa"}, {"ぴ", "pi"},  {"ぷ", "pu"},  {"ぺ", "pe"}, {"ぽ", "po"},
    {"ま", "ma"}, {"み", "mi"},  {"む", "mu"},  {"め", "me"}, {"も", "mo"},
    {"や", "ya"}, {"ゆ", "yu"},  {"よ", "yo"},
    {"ら", "ra"}, {"り", "ri"},  {"る", "ru"},  {"れ", "re"}, {"ろ", "ro"},
    {"わ", "wa"}, {"ゐ", "i"},   {"ゑ", "e"},   {"を", "o"},  {"ん", "n"},
    {"ぁ", "a"},  {"ぃ", "i"},   {"ぅ", "u"},   {"ぇ", "e"},  {"ぉ", "o"},
    {"ゃ", "ya"}, {"ゅ", "yu"},  {"ょ", "yo"},
};

bool next_mora(const std::vector<std::string>& ch, std::size_t i, std::string* out,
               std::size_t* used) {
    if (i + 1 < ch.size()) {
        std::string two = ch[i] + ch[i + 1];
        for (const auto& m : YOUON) {
            if (two == m.kana) {
                *out = m.romaji;
                *used = kYouonCodePoints;
                return true;
            }
        }
    }
    for (const auto& m : KANA) {
        if (ch[i] == m.kana) {
            *out = m.romaji;
            *used = 1;
            return true;
        }
    }
    return false;
}

bool romanise_hiragana(const std::string& kana, std::string* out) {
    std::vector<std::string> ch = utf8_chars(kana);
    std::string r;
    for (std::size_t i = 0; i < ch.size();) {
        if (ch[i] == "っ") {
            std::string next;
            std::size_t used = 0;
            if (i + 1 >= ch.size() || !next_mora(ch, i + 1, &next, &used)) {
                return false;
            }
            if (next.empty() || next[0] < 'a' || next[0] > 'z') {
                return false;
            }
            r += next[0];
            r += next;
            i += 1 + used;
            continue;
        }
        if (ch[i] == "ー") {
            if (r.empty()) {
                return false;
            }
            r += r.back();
            ++i;
            continue;
        }
        std::string mora;
        std::size_t used = 0;
        if (!next_mora(ch, i, &mora, &used)) {
            return false;
        }
        r += mora;
        i += used;
    }
    *out = r;
    return true;
}

std::string comparable(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\'' || c == '-' || c == ' ') {
            continue;
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void collect_ruby(const std::string& text,
                  std::vector<std::pair<std::string, std::string>>* out) {
    for (std::size_t pos = 0; (pos = text.find("<r ", pos)) != std::string::npos;) {
        std::size_t gt = text.find('>', pos + kRubyOpenPrefixLen);
        if (gt == std::string::npos) {
            break;
        }
        std::size_t close = text.find("</r>", gt + 1);
        if (close == std::string::npos) {
            pos = gt + 1;
            continue;
        }
        out->emplace_back(
            text.substr(pos + kRubyOpenPrefixLen, gt - pos - kRubyOpenPrefixLen),
            text.substr(gt + 1, close - gt - 1));
        pos = close + kRubyCloseTagLen;
    }
}

}  // namespace

TEST(Extract, extracted_json_has_archives_with_entries) {
    const fs::path p =
        fs::u8path(SNU_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    auto root = snu::json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    EXPECT_FALSE(root.get_object().empty());
    bool has_entries = false;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) {
            continue;
        }
        auto* s = kv.value().get_object().if_contains("strings");
        if (s && s->is_array() && !s->get_array().empty()) {
            has_entries = true;
            break;
        }
    }
    EXPECT_TRUE(has_entries) << "no archives with strings container";

    EXPECT_GT(root.get_object().size(), kMinExtractedScriptCount);
    std::int64_t total = 0;
    bool chapter1 = false;
    for (const auto& kv : root.get_object()) {
        const auto& sc = kv.value().get_object();
        for (const char* field : {"file", "arc", "file_size", "jp_strings"}) {
            EXPECT_TRUE(sc.if_contains(field))
                << std::string(kv.key()) << " missing " << field;
        }
        const auto& strings = sc.at("strings").get_array();
        EXPECT_EQ(sc.at("jp_strings").get_int64(),
                  static_cast<std::int64_t>(strings.size()));
        total += sc.at("jp_strings").get_int64();
        if (std::string(kv.key()).rfind("01_fruhlingsbeginn", 0) == 0) {
            chapter1 = true;
        }
        for (std::size_t i = 0;
             i < strings.size() && i < kSampledEntriesPerScript; ++i) {
            const auto& e = strings[i].get_object();
            for (const char* field : {"offset", "byte_len", "text", "has_jp", "type"}) {
                EXPECT_TRUE(e.if_contains(field));
            }
        }
    }
    EXPECT_GE(total, kMinTotalExtractedStrings);
    EXPECT_LE(total, kMaxTotalExtractedStrings);
    EXPECT_TRUE(chapter1) << "01_fruhlingsbeginn chapter missing";
}

TEST(BuildTsv, deployed_tsv_carries_no_ruby_markup) {
    const fs::path p = fs::u8path(SNU_PROJECT_DIR) / "build" / "install" /
                       "game" / "translations.tsv";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    std::ifstream f(p, std::ios::binary);
    std::string line;
    std::size_t bad = 0;
    while (std::getline(f, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string en = line.substr(tab + 1);
        const auto open = en.find('<');
        if (open == std::string::npos) {
            continue;
        }
        if (en.find('>', open + 1) != std::string::npos) {
            ++bad;
            if (bad < kMaxLoggedBadTsvLines) {
                GTEST_LOG_(INFO) << "markup in: " << en.substr(0, kLogPreviewMaxBytes);
            }
        }
    }
    EXPECT_EQ(bad, 0u) << bad << " English values carry a bracket pair the "
                                 "engine would consume as a tag";
}

TEST(BuildTsv, deployed_tsv_draws_no_two_names_alike) {
    const fs::path p = fs::u8path(SNU_PROJECT_DIR) / "build" / "install" /
                       "game" / "translations.tsv";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    std::vector<std::pair<std::string, std::string>> name_rows;
    std::ifstream f(p, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string jp = line.substr(0, tab);
        if (jp.size() < kMinBracketedNameBytes ||
            jp.compare(0, kBacklogNameOpenBracket.size(), kBacklogNameOpenBracket) != 0) {
            continue;
        }
        name_rows.emplace_back(jp, line.substr(tab + 1));
    }
    ASSERT_FALSE(name_rows.empty()) << "the table should carry backlog names";
    const auto shared = bt::name_plates_sharing_english(name_rows);
    std::string listed;
    for (const std::string& s : shared) {
        listed += "\n  " + s;
    }
    EXPECT_TRUE(shared.empty())
        << shared.size() << " name(s) the backlog draws alike:" << listed;
}

TEST(BuildTsv, deployed_tsv_has_one_tab_per_line) {
    const fs::path p = fs::u8path(SNU_PROJECT_DIR) / "build" / "install" /
                       "game" / "translations.tsv";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    std::ifstream f(p, std::ios::binary);
    std::string line;
    std::size_t ln = 0;
    std::size_t bad = 0;
    while (std::getline(f, line) && bad < kMaxLoggedBadTsvLines) {
        ++ln;
        if (line.empty()) {
            continue;
        }
        if (line.back() == '\r') {
            line.pop_back();
        }
        if (std::count(line.begin(), line.end(), '\t') != 1) {
            ++bad;
            GTEST_LOG_(INFO) << "bad line " << ln << ": "
                             << line.substr(0, kShortPreviewMaxBytes);
        }
    }
    EXPECT_EQ(bad, 0u);
}

TEST(BoxBudget, the_deployed_table_fits_the_boxes_it_is_drawn_in) {
    const fs::path tsv = fs::u8path(SNU_PROJECT_DIR) / "build" / "install" /
                         "game" / "translations.tsv";
    const fs::path doc =
        fs::u8path(SNU_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(tsv)) << tsv.u8string();
    ASSERT_TRUE(fs::exists(doc)) << doc.u8string();

    auto table = bt::load_runtime_table(tsv.u8string());
    ASSERT_FALSE(table.empty());
    bt::BoxBudgetReport r = bt::check_box_budgets_of_document(doc.u8string(), table);

    std::cout << "[ NOTE     ] " << r.boxes << " message boxes, "
              << r.chained_boxes << " filled by more than one string; "
              << r.over_budget_boxes << " over budget ("
              << r.over_budget_chained << " of them chained)\n";
    for (const bt::BoxOverflow& o : r.examples) {
        std::cout << "[ NOTE     ]   " << o.script << " segment "
                  << o.first_segment << " of " << o.segments
                  << ": box held " << o.lines_before << ", this drew "
                  << o.lines_drawn << "\n";
    }

    EXPECT_EQ(r.dropped_segments, 0u);
    EXPECT_EQ(r.over_budget_boxes - r.over_budget_chained, 0u)
        << "single-string boxes whose English does not fit the box";
}

TEST(BuildTsv, deployed_tsv_matches_translated_text_boundary_samples) {
    const fs::path tsv = fs::u8path(SNU_PROJECT_DIR) / "build" / "install" /
                         "game" / "translations.tsv";
    const fs::path doc =
        fs::u8path(SNU_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(tsv)) << tsv.u8string();
    ASSERT_TRUE(fs::exists(doc)) << doc.u8string();

    const auto tsv_rows = read_tsv(tsv.u8string());
    ASSERT_FALSE(tsv_rows.empty());

    const bj::object doc_obj = snu::json_parse_file(doc.u8string()).get_object();
    std::vector<std::pair<std::string, std::string>> order;
    std::unordered_map<std::string, std::size_t> index;
    std::vector<std::vector<std::string>> per_script_keys;
    per_script_keys.reserve(doc_obj.size());

    for (const auto& kv : doc_obj) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto& script = kv.value().get_object();
        auto it = script.find("strings");
        if (it == script.end() || !it->value().is_array()) {
            continue;
        }
        std::vector<std::string> script_keys;
        for (const auto& ev : it->value().get_array()) {
            const auto& entry = ev.get_object();
            std::string jp(entry.at("text").get_string());
            std::string en = jp;
            if (const auto* t = entry.if_contains("translated")) {
                if (t->is_string()) {
                    en = std::string(t->get_string());
                }
            }
            if (en == jp) {
                continue;
            }
            script_keys.push_back(jp);
            auto idx_it = index.find(jp);
            if (idx_it != index.end()) {
                order[idx_it->second].second = en;
                continue;
            }
            index.emplace(jp, order.size());
            order.emplace_back(jp, en);
        }
        if (!script_keys.empty()) {
            per_script_keys.push_back(std::move(script_keys));
        }
    }
    ASSERT_FALSE(order.empty());

    std::set<std::size_t> sample_indices;
    for (std::size_t idx : boundary_indices(order.size())) {
        sample_indices.insert(idx);
    }

    for (const auto& keys : per_script_keys) {
        for (std::size_t k_idx : boundary_indices(keys.size())) {
            sample_indices.insert(index.at(keys[k_idx]));
        }
    }

    std::vector<std::size_t> by_jp_len(order.size());
    std::vector<std::size_t> by_en_len(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        by_jp_len[i] = i;
        by_en_len[i] = i;
    }
    std::sort(by_jp_len.begin(), by_jp_len.end(), [&](std::size_t a, std::size_t b) {
        return order[a].first.size() < order[b].first.size();
    });
    std::sort(by_en_len.begin(), by_en_len.end(), [&](std::size_t a, std::size_t b) {
        return order[a].second.size() < order[b].second.size();
    });
    for (std::size_t idx : boundary_indices(order.size())) {
        sample_indices.insert(by_jp_len[idx]);
        sample_indices.insert(by_en_len[idx]);
    }

    for (std::size_t idx : sample_indices) {
        const auto& [jp, en] = order[idx];
        const std::string escaped_key = bt::esc(jp);
        const std::string expected_val = bt::esc(bt::shipped_english(jp, en));
        auto tsv_it = tsv_rows.find(escaped_key);
        ASSERT_NE(tsv_it, tsv_rows.end())
            << "missing TSV row for boundary sample idx=" << idx << " jp=" << jp;
        EXPECT_EQ(tsv_it->second.front(), expected_val)
            << "stale TSV value at boundary sample idx=" << idx << " jp=" << jp
            << " (run 02_build_tsv.exe --rebuild-table)";
    }
}

TEST(Translate, exempt_set_over_the_current_extraction) {
    const fs::path p =
        fs::u8path(SNU_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    bj::value root = snu::json_parse_file(p.u8string());
    std::set<std::string> distinct;
    std::int64_t entries = 0;
    std::int64_t total = 0;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) {
            continue;
        }
        for (const auto& ev : strings->get_array()) {
            std::string text(ev.get_object().at("text").get_string());
            ++total;
            if (snu::is_exempt_line(text)) {
                ++entries;
                distinct.insert(text);
            }
        }
    }
    EXPECT_EQ(total, kExpectedCorpusTotalStrings)
        << "corpus changed -- re-measure before trusting the rest";
    EXPECT_EQ(distinct.size(), kExpectedDistinctExemptLines);
    EXPECT_EQ(entries, kExpectedExemptEntries);
}

TEST(Translate, ruby_readings_agree_with_the_pinned_names) {
    const fs::path p =
        fs::u8path(SNU_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    std::map<std::string, std::string> people;
    for (const auto* table : {&tr::prompt_name_translations(),
                              &tr::extra_name_translations()}) {
        for (const auto& [jp, en] : *table) {
            people.emplace(jp, en);
        }
    }
    ASSERT_FALSE(people.empty());

    bj::value root = snu::json_parse_file(p.u8string());
    std::set<std::string> checked;
    std::set<std::string> unreadable;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) {
            continue;
        }
        for (const auto& ev : strings->get_array()) {
            std::vector<std::pair<std::string, std::string>> pairs;
            collect_ruby(std::string(ev.get_object().at("text").get_string()), &pairs);
            for (const auto& [reading, base] : pairs) {
                auto it = people.find(base);
                if (it == people.end()) {
                    continue;
                }
                std::string romaji;
                if (!romanise_hiragana(reading, &romaji)) {
                    unreadable.insert(base + " = " + reading);
                    continue;
                }
                checked.insert(base);
                EXPECT_EQ(romaji, comparable(it->second))
                    << "the corpus reads " << base << " as " << reading
                    << ", the name table ships " << it->second;
            }
        }
    }
    for (const auto& u : unreadable) {
        GTEST_LOG_(WARNING) << "reading not in kana, judge by hand: " << u;
    }
    for (const char* jp : {"吹", "寧", "宇綱", "葛", "末竹", "伯奇", "義貞"}) {
        EXPECT_EQ(checked.count(jp), 1u) << "no ruby reading swept for " << jp;
    }
}

TEST(Translate, line_breaks_and_backslashes_over_the_current_extraction) {
    const fs::path p =
        fs::u8path(SNU_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    bj::value root = snu::json_parse_file(p.u8string());
    std::int64_t total = 0;
    std::int64_t with_break = 0;
    std::int64_t internal = 0;
    std::int64_t backslash = 0;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) {
            continue;
        }
        for (const auto& ev : strings->get_array()) {
            std::string text(ev.get_object().at("text").get_string());
            ++total;
            if (text.find('\\') != std::string::npos) {
                ++backslash;
            }
            if (text.find('\n') == std::string::npos) {
                continue;
            }
            ++with_break;
            std::string body = text;
            while (!body.empty() && body.back() == '\n') {
                body.pop_back();
            }
            if (body.find('\n') != std::string::npos) {
                ++internal;
            }
        }
    }
    EXPECT_EQ(total, kExpectedCorpusTotalStrings)
        << "corpus changed -- re-measure before trusting the rest";
    EXPECT_EQ(backslash, 0)
        << "a backslash in the source makes the wire escape ambiguous";
    EXPECT_EQ(internal, kExpectedInternalBreakLines);
    EXPECT_EQ(with_break, kExpectedLinesWithBreaks);
}

TEST(Translate, speaker_gate_passes_over_the_current_extraction) {
    const fs::path p =
        fs::u8path(SNU_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    bj::value root = snu::json_parse_file(p.u8string());
    tr::SpeakerCheckReport report = tr::run_speaker_checks(root.get_object());
    EXPECT_TRUE(report.passed)
        << "check " << report.failed_check << ": " << report.reason;
    EXPECT_EQ(report.failed_check, 0);
    EXPECT_EQ(report.speakers_in_data, kExpectedSpeakersInData);
    EXPECT_EQ(report.glossary_entries, kExpectedGlossaryEntries)
        << "the table and the scripts have to cover each other exactly";
    EXPECT_EQ(report.dialogue_total, kExpectedDialogueTotal);
    EXPECT_EQ(report.dialogue_sampled, kExpectedDialogueSampled);
}
