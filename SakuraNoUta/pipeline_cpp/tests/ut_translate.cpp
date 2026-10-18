// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation-core tests: glossary, speaker extraction, prompt assembly, and response parsing.
#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "apps/pipeline_steps.h"
#include "common/coverage.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

namespace tr = snu::translate;
namespace bj = boost::json;
namespace fs = std::filesystem;

// Universal gate: needs_translation says YES to real Japanese and NO to
// ASCII / whitespace / pure punctuation.
TEST(Translate, needs_translation_filters_correctly) {
    EXPECT_TRUE(tr::needs_translation("こんにちは"));
    EXPECT_TRUE(tr::needs_translation("カタカナ"));
    EXPECT_TRUE(tr::needs_translation("漢字"));
    EXPECT_FALSE(tr::needs_translation("Hello"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("……"));
    EXPECT_FALSE(tr::needs_translation("ABC123"));
}

// save_cache writes atomically (.tmp -> rename); load_cache reads back.
// Round-tripping CJK exercises the encoding path.
TEST(Translate, translation_cache_round_trips) {
    std::string path = (fs::temp_directory_path() / "snu_ut_cache.json").u8string();
    tr::Cache cache;
    cache.set("こんにちは", "Hello");
    cache.set("太一", "Taichi");
    tr::save_cache(cache, path);
    tr::Cache loaded = tr::load_cache(path);
    ASSERT_EQ(loaded.size(), 2u);
    ASSERT_TRUE(loaded.get("こんにちは"));
    EXPECT_EQ(*loaded.get("こんにちは"), "Hello");
    ASSERT_TRUE(loaded.get("太一"));
    EXPECT_EQ(*loaded.get("太一"), "Taichi");
    fs::remove(fs::u8path(path));
    EXPECT_FALSE(fs::exists(fs::u8path(path + ".tmp")));  // atomic-write cleanup
}

TEST(Translate, extract_speaker_mapping) {
    // Glossary names reach the prompt in English, not raw JP.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"直哉", "Naoya"}, {"稟", "Rin"}, {"真琴", "Makoto"},
        {"里奈", "Rina"}, {"優美", "Yumi"}};
    for (const auto& [jp, en] : cases) {
        bj::object entry;
        entry["speaker"] = jp;
        entry["text"] = "「dummy」";
        entry["type"] = "dialogue";
        EXPECT_EQ(tr::extract_speaker(entry), en);
    }

    bj::object name_entry;
    name_entry["type"] = "name";
    name_entry["text"] = "直哉";
    EXPECT_EQ(tr::extract_speaker(name_entry), "NAME");

    bj::object narration;
    narration["type"] = "narrative";
    narration["speaker"] = nullptr;
    EXPECT_EQ(tr::extract_speaker(narration), "NARRATION");

    bj::object unknown;
    unknown["type"] = "dialogue";
    unknown["speaker"] = "誰か";  // not in glossary -> raw passthrough
    EXPECT_EQ(tr::extract_speaker(unknown), "誰か");
}

TEST(Translate, strip_speaker_tags_removes_leaked_tags) {
    EXPECT_EQ(tr::strip_speaker_tags("[Naoya] Hello there"), "Hello there");
    EXPECT_EQ(tr::strip_speaker_tags("[NARRATION] The night fell."),
              "The night fell.");
    EXPECT_EQ(tr::strip_speaker_tags("No tags here"), "No tags here");
}

// Only a leading [Speaker] tag is stripped; brackets inside the text are content.
TEST(Translate, strip_speaker_tags_leaves_brackets_inside_the_line) {
    EXPECT_EQ(tr::strip_speaker_tags("He read the sign [EXIT] and stopped."),
              "He read the sign [EXIT] and stopped.");
    EXPECT_EQ(tr::strip_speaker_tags("\"Wait,\" he said [NARRATION] later."),
              "\"Wait,\" he said [NARRATION] later.");
    // A leading tag is still removed, and only the leading one.
    EXPECT_EQ(tr::strip_speaker_tags("[Rin] the sign said [EXIT]"),
              "the sign said [EXIT]");
}

// max_tokens is rejected outright above the model's output ceiling, so a batch
// large enough to compute past it must clamp rather than send the raw product.
TEST(Translate, max_tokens_stays_inside_the_model_ceiling) {
    EXPECT_EQ(tr::compute_max_tokens(500), tr::MAX_OUTPUT_TOKENS);
    EXPECT_LE(tr::compute_max_tokens(500), 128000);
    EXPECT_EQ(tr::compute_max_tokens(150), 150 * tr::OUTPUT_TOKENS_PER_LINE);
    EXPECT_EQ(tr::compute_max_tokens(1), tr::MIN_OUTPUT_TOKENS);   // floor
    EXPECT_EQ(tr::compute_max_tokens(0), tr::MIN_OUTPUT_TOKENS);
    EXPECT_EQ(tr::compute_max_tokens(-5), tr::MIN_OUTPUT_TOKENS);  // no overflow
    // The clamp engages somewhere between the two, and never above the ceiling.
    for (int batch : {1, 50, 150, 426, 427, 1000, 100000})
        EXPECT_LE(tr::compute_max_tokens(batch), tr::MAX_OUTPUT_TOKENS) << batch;
}

// The batch cap is a spend limit, so every value that is not a limit has to be
// refused.  A negative one used to mean "unbounded" -- the exact opposite.
TEST(Translate, option_validation_refuses_values_that_would_overspend) {
    EXPECT_FALSE(tr::validate_options(150, 0, 0));   // full run
    EXPECT_FALSE(tr::validate_options(150, 0, 1));   // one bounded request
    EXPECT_FALSE(tr::validate_options(1, 3, 0));     // smoke run

    EXPECT_TRUE(tr::validate_options(150, 0, -1));   // the fail-open case
    EXPECT_TRUE(tr::validate_options(150, 0, -999));
    EXPECT_TRUE(tr::validate_options(150, -1, 0));
    EXPECT_TRUE(tr::validate_options(0, 0, 0));      // batch 0 never advances
    EXPECT_TRUE(tr::validate_options(-5, 0, 0));

    // The message names the offending flag, since it is printed and nothing else.
    EXPECT_NE(tr::validate_options(150, 0, -1)->find("--max-batches"),
              std::string::npos);
    EXPECT_NE(tr::validate_options(0, 0, 0)->find("--batch"), std::string::npos);
}

// The banner must describe the request that is actually built.  Both read the
// same function, so this pins them together.
TEST(Translate, reasoning_banner_matches_the_transmitted_field) {
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_OPUS_47),
              tr::ReasoningControl::AdaptiveThinking);
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_SONNET), tr::ReasoningControl::Effort);
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_OPUS_46), tr::ReasoningControl::Effort);
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_HAIKU), tr::ReasoningControl::None);

    // Whatever the configured model is, the label may only mention effort when
    // effort is what goes into the body.
    std::string label = tr::reasoning_control_label(tr::MODEL);
    bool mentions_effort = label.find("effort") != std::string::npos;
    EXPECT_EQ(mentions_effort, tr::reasoning_control(tr::MODEL) ==
                                   tr::ReasoningControl::Effort);
}

// Lines made only of punctuation translate to themselves.  The purge cannot
// tell them from failures, so the coverage report counts them apart instead.
TEST(Translate, exempt_predicate_fires_on_symbols_only) {
    EXPECT_TRUE(snu::is_exempt_line("　・・・"));
    EXPECT_TRUE(snu::is_exempt_line("　・・・・・・。"));
    EXPECT_TRUE(snu::is_exempt_line("　「・・・」"));
    EXPECT_TRUE(snu::is_exempt_line("？？？"));
    EXPECT_TRUE(snu::is_exempt_line("……"));
    EXPECT_TRUE(snu::is_exempt_line("「……」"));
    EXPECT_TRUE(snu::is_exempt_line("――"));

    EXPECT_FALSE(snu::is_exempt_line(""));            // nothing to exempt
    EXPECT_FALSE(snu::is_exempt_line("こんにちは"));  // kana
    EXPECT_FALSE(snu::is_exempt_line("桜"));          // kanji
    EXPECT_FALSE(snu::is_exempt_line("「あ……」"));   // one kana is enough
    EXPECT_FALSE(snu::is_exempt_line("OK"));          // latin
    EXPECT_FALSE(snu::is_exempt_line("Ａ"));          // fullwidth latin
    EXPECT_FALSE(snu::is_exempt_line("１２３"));      // fullwidth digits
    EXPECT_FALSE(snu::is_exempt_line("ア"));          // katakana letter

    // The purge is deliberately left alone: re-queuing these costs a little,
    // while making the purge exempt-aware would re-admit real failures.
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
}

// Exempt-set size over the real corpus, pinned rather than left to a side
// script.  Skips when the extraction hasn't been produced yet.
TEST(Translate, exempt_set_over_the_current_extraction) {
    fs::path extracted = fs::u8path(std::string(SNU_PROJECT_DIR) +
                                    "\\script_output\\extracted_text.json");
    if (!fs::exists(extracted)) {
        GTEST_SKIP() << "extracted_text.json not produced yet";
    }
    bj::value root = snu::json_parse_file(extracted.u8string());
    std::set<std::string> distinct;
    std::int64_t entries = 0, total = 0;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& ev : strings->get_array()) {
            std::string text(ev.get_object().at("text").get_string());
            ++total;
            if (snu::is_exempt_line(text)) {
                ++entries;
                distinct.insert(text);
            }
        }
    }
    EXPECT_EQ(total, 68129) << "corpus changed -- re-measure before trusting the rest";
    EXPECT_EQ(distinct.size(), 9u);
    EXPECT_EQ(entries, 78);
}

namespace {

// One UTF-8 code point per element.
std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        if (i + n > s.size()) n = 1;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

struct Mora {
    const char* kana;
    const char* romaji;
};

// Two code points, so they are tried before their first half.
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
        for (const auto& m : YOUON)
            if (two == m.kana) { *out = m.romaji; *used = 2; return true; }
    }
    for (const auto& m : KANA)
        if (ch[i] == m.kana) { *out = m.romaji; *used = 1; return true; }
    return false;
}

// Hepburn romanisation of a hiragana reading, in the same ASCII the name
// tables are written in.  False when the reading uses something outside the
// table -- katakana, a kanji, punctuation -- which means the pair cannot be
// judged rather than that it disagrees.
bool romanise_hiragana(const std::string& kana, std::string* out) {
    std::vector<std::string> ch = utf8_chars(kana);
    std::string r;
    for (std::size_t i = 0; i < ch.size();) {
        if (ch[i] == "っ") {  // doubles the consonant of the mora after it
            std::string next;
            std::size_t used = 0;
            if (i + 1 >= ch.size() || !next_mora(ch, i + 1, &next, &used)) return false;
            if (next.empty() || next[0] < 'a' || next[0] > 'z') return false;
            r += next[0];
            r += next;
            i += 1 + used;
            continue;
        }
        if (ch[i] == "ー") {  // holds the vowel before it
            if (r.empty()) return false;
            r += r.back();
            ++i;
            continue;
        }
        std::string mora;
        std::size_t used = 0;
        if (!next_mora(ch, i, &mora, &used)) return false;
        r += mora;
        i += used;
    }
    *out = r;
    return true;
}

// Comparable form of a pinned romanisation: the apostrophe that separates a
// syllabic n and the hyphen that joins a compound carry no sound.
std::string comparable(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\'' || c == '-' || c == ' ') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// The well-formed <r reading>base</r> pairs in one entry.
void collect_ruby(const std::string& text,
                  std::vector<std::pair<std::string, std::string>>* out) {
    for (std::size_t pos = 0; (pos = text.find("<r ", pos)) != std::string::npos;) {
        std::size_t gt = text.find('>', pos + 3);
        if (gt == std::string::npos) break;
        std::size_t close = text.find("</r>", gt + 1);
        if (close == std::string::npos) { pos = gt + 1; continue; }
        out->emplace_back(text.substr(pos + 3, gt - pos - 3),
                          text.substr(gt + 1, close - gt - 1));
        pos = close + 4;
    }
}

}  // namespace

// Pinned romanisations must match the corpus's own furigana readings; scoped
// to name-table people only.  Skips when the extraction hasn't been produced.
TEST(Translate, ruby_readings_agree_with_the_pinned_names) {
    fs::path extracted = fs::u8path(std::string(SNU_PROJECT_DIR) +
                                    "\\script_output\\extracted_text.json");
    if (!fs::exists(extracted)) {
        GTEST_SKIP() << "extracted_text.json not produced yet";
    }
    std::map<std::string, std::string> people;
    for (const auto* table : {&tr::prompt_name_translations(),
                              &tr::extra_name_translations()})
        for (const auto& [jp, en] : *table) people.emplace(jp, en);
    ASSERT_FALSE(people.empty());

    bj::value root = snu::json_parse_file(extracted.u8string());
    std::set<std::string> checked, unreadable;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& ev : strings->get_array()) {
            std::vector<std::pair<std::string, std::string>> pairs;
            collect_ruby(std::string(ev.get_object().at("text").get_string()), &pairs);
            for (const auto& [reading, base] : pairs) {
                auto it = people.find(base);
                if (it == people.end()) continue;
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
    for (const auto& u : unreadable)
        GTEST_LOG_(WARNING) << "reading not in kana, judge by hand: " << u;
    // The names the corpus vouches for today. A drop means the sweep stopped
    // finding them and the check went quiet rather than green.
    for (const char* jp : {"吹", "寧", "宇綱", "葛", "末竹", "伯奇", "義貞"})
        EXPECT_EQ(checked.count(jp), 1u) << "no ruby reading swept for " << jp;
}

// Pinned terminology is data, and the prompt is built from it -- a term added
// to the table cannot be left out of the prompt.
TEST(Translate, system_prompt_carries_every_pinned_term) {
    const auto& terms = tr::terminology_translations();
    ASSERT_FALSE(terms.empty()) << "the terminology table is what stops drift";
    const std::string& prompt = tr::system_prompt();
    for (const auto& [jp, en] : terms) {
        EXPECT_NE(prompt.find(jp), std::string::npos) << "term missing: " << jp;
        EXPECT_NE(prompt.find(en), std::string::npos) << "rendering missing: " << en;
        EXPECT_NE(prompt.find(jp + " = " + en), std::string::npos)
            << "pair not rendered together: " << jp;
    }
    EXPECT_NE(prompt.find("<terminology>"), std::string::npos);
    // The painting the story is built around, which shipped under two names.
    EXPECT_NE(prompt.find("The Reclining Sakura"), std::string::npos);
}

// The prompt must name the tag form (<r ...>...</r>) and also forbid tag
// fragments, since both well-formed pairs and malformed fragments appear.
TEST(Translate, system_prompt_names_the_ruby_form_the_engine_uses) {
    const std::string& prompt = tr::system_prompt();
    EXPECT_NE(prompt.find("<r "), std::string::npos);
    EXPECT_NE(prompt.find("</r>"), std::string::npos);
    EXPECT_NE(prompt.find("</>"), std::string::npos);
    EXPECT_NE(prompt.find("NO tag and NO piece of one"), std::string::npos);
    EXPECT_EQ(prompt.find("[漢字/かな]"), std::string::npos);
}

// Prompt and runtime read the same name table, so the spelling is consistent.
TEST(Translate, system_prompt_carries_the_pinned_personal_names) {
    const auto& names = tr::prompt_name_translations();
    const auto& all = tr::name_translations();
    ASSERT_FALSE(names.empty());
    const std::string& prompt = tr::system_prompt();
    for (const auto& [jp, en] : names) {
        ASSERT_EQ(all.count(jp), 1u) << "not a speaker-table key: " << jp;
        EXPECT_EQ(all.at(jp), en) << "prompt and runtime disagree on " << jp;
        EXPECT_NE(prompt.find(jp + " = " + en), std::string::npos)
            << "name not rendered: " << jp;
    }
    EXPECT_NE(prompt.find("<character_names>"), std::string::npos);

    // These are the readings nobody gets right without the table.
    for (const char* jp : {"吹", "藍", "中村", "葛", "水菜", "寧", "咲崎",
                           "霧乃", "方相氏"})
        EXPECT_NE(prompt.find(std::string(jp) + " = "), std::string::npos)
            << "ungoverned in prose: " << jp;

    // Role and mob labels stay out: inside a sentence they are ordinary nouns,
    // and pinning them would capitalise every girl and customer in the novel.
    for (const char* jp : {"少女", "客Ａ", "看護婦", "老人", "店員", "教師"})
        EXPECT_EQ(prompt.find(std::string(jp) + " = "), std::string::npos)
            << "role label pinned as a name: " << jp;
    EXPECT_LT(names.size(), tr::name_translations_ordered().size());
}

// Prompt size is measured in both code points and bytes, since it's paid on every request.
TEST(Translate, system_prompt_size_stays_within_budget) {
    const std::string& prompt = tr::system_prompt();
    std::size_t bytes = prompt.size();
    std::size_t characters = 0;
    for (unsigned char c : prompt)
        if ((c & 0xC0) != 0x80) ++characters;  // count UTF-8 lead bytes
    GTEST_LOG_(INFO) << "system prompt: " << characters << " characters, "
                     << bytes << " bytes";
    EXPECT_GT(characters, 4000u) << "the prompt lost a section";
    EXPECT_LT(characters, 9000u)
        << "every added character is re-sent on every request of the run";
}

// The line-break rule must be present — it is the only thing preventing the
// model from using a real newline inside a numbered entry, which the parser
// cannot distinguish from the next entry.
TEST(Translate, system_prompt_states_line_break_policy) {
    const std::string& prompt = tr::system_prompt();
    EXPECT_NE(prompt.find("ONE physical line per numbered entry"),
              std::string::npos);
}

TEST(Translate, parse_numbered_response_variants) {
    std::string resp =
        "1. First line\n"
        "2) Second line\n"
        "3: Third line\n"
        "4. Wrapped line\n"
        "continues here\n";
    auto t = tr::parse_numbered_response(resp);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t.at(1), "First line");
    EXPECT_EQ(t.at(2), "Second line");
    EXPECT_EQ(t.at(3), "Third line");
    EXPECT_EQ(t.at(4), "Wrapped line continues here");
}

TEST(Translate, parse_numbered_response_strips_echoed_tags) {
    auto t = tr::parse_numbered_response("1. [Naoya] \"Morning.\"\n");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t.at(1), "\"Morning.\"");
}

// A backward-pointing number on a continuation line would silently replace an
// already-parsed translation; the parser treats it as prose instead.
TEST(Translate, parse_numbered_response_keeps_a_backward_number_as_prose) {
    std::string resp =
        "1. He came back at dawn.\n"
        "2. \"It has been a while,\" she said.\n"
        "3. The war ended without a sound.\n"
        "2 days later, the rain came.\n"
        "4. Nothing was left of the garden.\n";
    auto t = tr::parse_numbered_response(resp, 4);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t.at(1), "He came back at dawn.");
    EXPECT_EQ(t.at(2), "\"It has been a while,\" she said.")
        << "an already-parsed translation was overwritten";
    EXPECT_EQ(t.at(3), "The war ended without a sound. 2 days later, the rain came.");
    EXPECT_EQ(t.at(4), "Nothing was left of the garden.");

    // The ordering rule alone catches this one, so it holds without a count.
    EXPECT_EQ(tr::parse_numbered_response(resp).at(2),
              "\"It has been a while,\" she said.");
}

// A forward-pointing number would file text under a line nobody asked for;
// the batch size bounds it so the parser treats it as prose.
TEST(Translate, parse_numbered_response_keeps_a_forward_number_as_prose) {
    std::string resp =
        "1. He never spoke of the painting again.\n"
        "1985 came and went, and still he said nothing.\n"
        "2. The gallery closed that spring.\n";
    auto t = tr::parse_numbered_response(resp, 2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1),
              "He never spoke of the painting again. 1985 came and went, and still "
              "he said nothing.");
    EXPECT_EQ(t.at(2), "The gallery closed that spring.");
    EXPECT_EQ(t.count(1985), 0u) << "a line number nobody asked for was invented";

    // A digit run too long to be a line number is prose whatever the count is.
    auto huge = tr::parse_numbered_response("1. Priced at\n999999999999 yen.\n", 1);
    ASSERT_EQ(huge.size(), 1u);
    EXPECT_EQ(huge.at(1), "Priced at 999999999999 yen.");
}

// Multi-line entries still join, and a skipped number leaves a gap the caller re-queues.
TEST(Translate, parse_numbered_response_still_joins_and_still_allows_gaps) {
    auto t = tr::parse_numbered_response(
        "1. The first line\n"
        "runs on for a second physical line\n"
        "and a third.\n"
        "2. Second line\n"
        "4. Fourth line -- the third never came back\n",
        4);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(1), "The first line runs on for a second physical line and a third.");
    EXPECT_EQ(t.at(2), "Second line");
    EXPECT_EQ(t.count(3), 0u);
    EXPECT_EQ(t.at(4), "Fourth line -- the third never came back");
}

// Prose that happens to start with a number must not be read as an entry
// opener; doing so would silently replace the real entry.
TEST(Translate, parse_numbered_response_rejects_a_prose_opener) {
    auto with_third_line = [](const char* third) {
        return std::string("1. He came back at dawn.\n") +
               "2. The house was empty.\n" + third + "\n" +
               "3. She never wrote back.\n" +
               "4. Nothing was left of the garden.\n";
    };

    for (const char* prose : {"3 days later, the rain stopped.", "3:00 sharp, he left.",
                              "3.5 cm across."}) {
        auto t = tr::parse_numbered_response(with_third_line(prose), 4);
        ASSERT_EQ(t.size(), 4u) << prose;
        EXPECT_EQ(t.at(1), "He came back at dawn.") << prose;
        EXPECT_EQ(t.at(2), std::string("The house was empty. ") + prose) << prose;
        EXPECT_EQ(t.at(3), "She never wrote back.")
            << "the real entry 3 was swallowed by: " << prose;
        EXPECT_EQ(t.at(4), "Nothing was left of the garden.") << prose;
    }

    // A year, a price and a measurement are prose for the same reason.
    auto t = tr::parse_numbered_response(
        "1. He never spoke of it again.\n"
        "1985 came and went.\n"
        "2. The gallery closed that spring.\n",
        2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "He never spoke of it again. 1985 came and went.");
    EXPECT_EQ(t.at(2), "The gallery closed that spring.");
}

// The three separators the model is asked for still open an entry, and a
// number equal to the count asked for is the last legal one.
TEST(Translate, parse_numbered_response_accepts_every_requested_separator) {
    auto t = tr::parse_numbered_response(
        "1. First line\n"
        "2) Second line\n"
        "3: Third line\n",
        3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(1), "First line");
    EXPECT_EQ(t.at(2), "Second line");
    EXPECT_EQ(t.at(3), "Third line") << "the number equal to the count was refused";
}

// An unusable numbering (out of order, duplicated, past the count) discards
// the whole batch; an empty map re-queues it.
TEST(Translate, parse_numbered_response_discards_a_batch_it_cannot_trust) {
    struct Case {
        const char* what;
        const char* body;
        int expected;
    };
    const Case cases[] = {
        {"out of order", "1. A\n3. C\n2. B\n4. D\n", 4},
        {"duplicate number", "2. B\n2. B-prime\n", 2},
        {"renumbered from 1 midway", "1. A\n2. B\n1. C\n2. D\n", 4},
        {"past the count asked for", "1. A\n2. B\n5. E\n", 2},
        {"model-invented sublist",
         "1. First line\n2. Second line\n   1. an aside\n   2. another\n3. Third line\n", 3},
        {"number alone on its line", "1. First\n2.\nthe text came a line late\n", 2},
    };
    for (const auto& c : cases) {
        int rejected = 0;
        auto t = tr::parse_numbered_response(c.body, c.expected, &rejected);
        EXPECT_TRUE(t.empty()) << c.what << ": kept " << t.size()
                               << " entries instead of re-queuing the batch";
        EXPECT_GT(rejected, 0) << c.what << ": the unusable line was not counted";
    }

    // The ordering rule holds without a count too.
    EXPECT_TRUE(tr::parse_numbered_response("1. A\n3. C\n2. B\n").empty());
}

// A clean answer reports nothing rejected, so the count separates "the model
// answered short" from "the answer was thrown away".
TEST(Translate, parse_numbered_response_reports_the_rejected_line_count) {
    int rejected = -1;
    auto good = tr::parse_numbered_response("1. A\n2. B\n", 2, &rejected);
    EXPECT_EQ(good.size(), 2u);
    EXPECT_EQ(rejected, 0);

    rejected = -1;
    auto bad = tr::parse_numbered_response("1. A\n2. B\n1. C\n1. D\n", 4, &rejected);
    EXPECT_TRUE(bad.empty());
    EXPECT_EQ(rejected, 2);

    // The out-parameter is optional; the two-argument form still works.
    EXPECT_EQ(tr::parse_numbered_response("1. A\n", 1).size(), 1u);
}

// A digit run too long to be a line number is always treated as prose.
TEST(Translate, parse_numbered_response_treats_an_overlong_digit_run_as_prose) {
    int rejected = -1;
    auto t = tr::parse_numbered_response("1. Priced at\n999999999999. yen, he said.\n", 1,
                                         &rejected);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t.at(1), "Priced at 999999999999. yen, he said.");
    EXPECT_EQ(rejected, 0);
}

namespace {

// Written as bytes rather than as characters, so the escape cannot be read as
// part of the text that follows it.
const char* const U3000 = "\xE3\x80\x80";      // IDEOGRAPHIC SPACE
const char* const FULLWIDTH_ONE = "\xEF\xBC\x91";   // U+FF11
const char* const FULLWIDTH_STOP = "\xEF\xBC\x8E";  // U+FF0E

}  // namespace

// Every numbering shape the model writes must be recognised as an opener;
// a missed one silently glues two translations together.
TEST(Translate, parse_numbered_response_reads_every_numbering_the_model_writes) {
    struct Case {
        const char* what;
        std::string body;
    };
    const std::vector<Case> cases = {
        {"one blank", "1. Alpha"},
        {"two blanks", "1.  Alpha"},
        {"no blank at all", "1.Alpha"},
        {"fullwidth space for the blank", std::string("1.") + U3000 + "Alpha"},
        {"blank before the separator", "1 . Alpha"},
        {"paren separator", "1) Alpha"},
        {"colon separator", "1: Alpha"},
        {"fullwidth digit", std::string(FULLWIDTH_ONE) + ". Alpha"},
        {"fullwidth separator", std::string("1") + FULLWIDTH_STOP + " Alpha"},
        {"markdown emphasis", "**1.** Alpha"},
        {"quote marker", "> 1. Alpha"},
        {"ascii indent", "   1. Alpha"},
        {"fullwidth indent", std::string(U3000) + "1. Alpha"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(c.body + "\n", 1, &rejected);
        ASSERT_EQ(t.size(), 1u) << c.what;
        EXPECT_EQ(t.at(1), "Alpha") << c.what;
        EXPECT_EQ(rejected, 0) << c.what;
    }
}

// The two shapes the whitespace rule made likely: the model is asked to copy
// the source's leading fullwidth space, so a number followed straight by one
// is a shape it can produce, and so is a number with no gap at all.
TEST(Translate, parse_numbered_response_does_not_glue_a_spaceless_opener_to_the_line_before) {
    const std::vector<std::string> openers = {"2.Bravo", std::string("2.") + U3000 + "Bravo"};
    for (const auto& opener : openers) {
        auto t = tr::parse_numbered_response("1. Alpha\n" + opener + "\n3. Charlie\n", 3);
        ASSERT_EQ(t.size(), 3u) << opener;
        EXPECT_EQ(t.at(1), "Alpha") << opener;
        EXPECT_EQ(t.at(2), "Bravo") << opener;
        EXPECT_EQ(t.at(3), "Charlie") << opener;
    }
}

// A digit after the separator is what separates a line number from ordinary
// English, so a sub-point reads as prose exactly as a decimal does.
TEST(Translate, parse_numbered_response_keeps_a_sub_point_number_as_prose) {
    auto t = tr::parse_numbered_response(
        "1. The list was short.\n"
        "1.1 sub point, he wrote in the margin.\n"
        "2. Nothing else was on the page.\n",
        2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "The list was short. 1.1 sub point, he wrote in the margin.");
    EXPECT_EQ(t.at(2), "Nothing else was on the page.");
}

// A year with a separator (e.g. "1985.") reads as a line number nobody asked
// for, so the batch is discarded.
TEST(Translate, parse_numbered_response_rejects_a_year_that_reads_as_a_line_number) {
    int rejected = -1;
    auto t = tr::parse_numbered_response(
        "1. He never spoke of the painting again.\n"
        "1985. That was the year, he said.\n"
        "2. The gallery closed that spring.\n",
        2, &rejected);
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(rejected, 1);
}

// Decoration is skipped to FIND the number, never removed from the text: a
// translation that genuinely starts with one of those characters is kept whole.
TEST(Translate, parse_numbered_response_leaves_decoration_inside_a_translation_alone) {
    auto t = tr::parse_numbered_response(
        "1. *softly, from the doorway*\n"
        "> and then nothing at all\n"
        "2. #1 in her year, she said.\n",
        2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "*softly, from the doorway* > and then nothing at all");
    EXPECT_EQ(t.at(2), "#1 in her year, she said.");
}

// A translation that opens with a word the parser also uses as markup is
// ordinary text and must survive untouched.
TEST(Translate, parse_numbered_response_keeps_a_translation_that_reads_like_markup) {
    auto t = tr::parse_numbered_response(
        "1. He came back at dawn.\n"
        "2. The house was empty.\n"
        "3. Tag was the worst.\n",
        3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(3), "Tag was the worst.");
}

TEST(Translate, parse_numbered_response_reads_crlf_line_endings) {
    auto t = tr::parse_numbered_response("1. Alpha\r\n2. Bravo\r\n", 2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "Alpha");
    EXPECT_EQ(t.at(2), "Bravo");
}

// Anything before the first number belongs to no entry, so it is dropped
// rather than filed under one.
TEST(Translate, parse_numbered_response_drops_anything_before_the_first_entry) {
    auto prose = tr::parse_numbered_response(
        "Here are the translations:\n\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(prose.size(), 2u);
    EXPECT_EQ(prose.at(1), "Alpha");
    EXPECT_EQ(prose.at(2), "Bravo");

    // A preamble that starts with a number is prose too -- it carries no
    // separator, so it cannot be read as a line number.
    auto numbered = tr::parse_numbered_response(
        "2 lines follow, translated below\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(numbered.size(), 2u);
    EXPECT_EQ(numbered.at(1), "Alpha");
    EXPECT_EQ(numbered.at(2), "Bravo");

    int rejected = -1;
    EXPECT_TRUE(tr::parse_numbered_response("", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tr::parse_numbered_response("  \n\t\n \r\n", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
}

// A remark after the last entry is reported (not rejected), since it cannot
// be distinguished from a multi-line final translation.
TEST(Translate, parse_numbered_response_reports_text_hanging_off_the_last_entry) {
    std::string tail = "unset";
    auto clean = tr::parse_numbered_response("1. Alpha\n2. Bravo\n", 2, nullptr, &tail);
    ASSERT_EQ(clean.size(), 2u);
    EXPECT_TRUE(tail.empty()) << "a single-line last entry reported a tail";

    auto noted = tr::parse_numbered_response(
        "1. Alpha\n2. Bravo\nNote: I kept the ruby base words only.\n", 2, nullptr, &tail);
    ASSERT_EQ(noted.size(), 2u) << "the count still matches, so nothing else warns";
    EXPECT_EQ(noted.at(2), "Bravo Note: I kept the ruby base words only.");
    EXPECT_EQ(tail, "Note: I kept the ruby base words only.");

    // Only the LAST entry is reported: an earlier one running over two lines
    // is ordinary and says nothing about trailing commentary.
    auto wrapped = tr::parse_numbered_response(
        "1. Alpha\nand the rest of it\n2. Bravo\n", 2, nullptr, &tail);
    ASSERT_EQ(wrapped.size(), 2u);
    EXPECT_EQ(wrapped.at(1), "Alpha and the rest of it");
    EXPECT_TRUE(tail.empty());
}

// The production batch size, so the guards are measured at the scale they run
// at rather than on three-line samples.
TEST(Translate, parse_numbered_response_holds_over_a_production_batch) {
    const int kLines = 300;
    std::string body;
    for (int i = 1; i <= kLines; ++i)
        body += std::to_string(i) + ". Line " + std::to_string(i) + "\n";

    int rejected = -1;
    std::string tail = "unset";
    auto t = tr::parse_numbered_response(body, kLines, &rejected, &tail);
    ASSERT_EQ(t.size(), static_cast<std::size_t>(kLines));
    EXPECT_EQ(t.at(1), "Line 1");
    EXPECT_EQ(t.at(kLines), "Line 300");
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tail.empty());

    auto strayed = tr::parse_numbered_response(body + "That's all of them.\n", kLines,
                                               &rejected, &tail);
    ASSERT_EQ(strayed.size(), static_cast<std::size_t>(kLines));
    EXPECT_EQ(strayed.at(kLines), "Line 300 That's all of them.");
    EXPECT_EQ(rejected, 0) << "a stray last line is not a numbering fault";
    EXPECT_EQ(tail, "That's all of them.");
}

// Pinned known-blind-spot: a skipped number whose continuation starts with
// exactly that number passes every guard.
TEST(Translate, parse_numbered_response_cannot_tell_a_skipped_number_from_a_real_one) {
    auto t = tr::parse_numbered_response(
        "1. He came back at dawn.\n"
        "2. The house was empty, and\n"
        "3. days later it was still empty.\n"
        "4. Nothing was left of the garden.\n",
        4);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t.at(2), "The house was empty, and") << "the entry lost its tail";
    EXPECT_EQ(t.at(3), "days later it was still empty.") << "and the tail became an entry";
}

// Without a count, only the ordering rule applies; kept so the backward-
// collision guard is exercisable on its own.
TEST(Translate, parse_numbered_response_without_a_count_still_holds_the_order_rule) {
    EXPECT_TRUE(tr::parse_numbered_response("1. A\n3. C\n2. B\n").empty());

    auto forward = tr::parse_numbered_response("1. A\n9. I\n");
    ASSERT_EQ(forward.size(), 2u) << "with no count, nothing says how far forward is too far";
    EXPECT_EQ(forward.at(9), "I");
}

// Source breaks travel as \\n escapes in both directions and become real
// breaks again before caching.
TEST(Translate, a_line_break_travels_as_an_escape_in_both_directions) {
    EXPECT_EQ(tr::escape_line_breaks("first\nsecond"), "first\\nsecond");
    EXPECT_EQ(tr::escape_line_breaks("first\r\nsecond"), "first\\nsecond");
    EXPECT_EQ(tr::escape_line_breaks("first\rsecond"), "first\\nsecond");
    EXPECT_EQ(tr::escape_line_breaks("one\ntwo\nthree"), "one\\ntwo\\nthree");
    EXPECT_EQ(tr::escape_line_breaks("no break here"), "no break here");

    EXPECT_EQ(tr::unescape_line_breaks("first\\nsecond"), "first\nsecond");
    EXPECT_EQ(tr::unescape_line_breaks(tr::escape_line_breaks("one\ntwo\nthree")),
              "one\ntwo\nthree");
    EXPECT_EQ(tr::unescape_line_breaks("no break here"), "no break here");

    // A blank written against the escape belongs to neither side of the break.
    EXPECT_EQ(tr::unescape_line_breaks("first \\n second"), "first\nsecond");
    // The fullwidth space is the source's paragraph indent, which the answer is
    // asked to copy -- it is content, not a blank.
    EXPECT_EQ(tr::unescape_line_breaks(std::string("first\\n") + U3000 + "second"),
              std::string("first\n") + U3000 + "second");
    // A backslash in front of anything else is left exactly where it is.
    EXPECT_EQ(tr::unescape_line_breaks("a\\tb"), "a\\tb");
    EXPECT_EQ(tr::unescape_line_breaks("trailing backslash\\"), "trailing backslash\\");
}

// The prompt writes what it asks for: one physical line per entry, in the
// lines to translate and in the context alike.
TEST(Translate, user_prompt_keeps_every_entry_on_one_physical_line) {
    std::vector<std::pair<std::string, std::string>> batch = {
        {"Naoya", "一行目\n二行目"}, {"NARRATION", "朝が来た。"}};
    std::vector<std::pair<std::string, std::string>> context = {
        {"Rin", "Good night.\nSleep well."}};
    std::string prompt = tr::build_user_prompt(batch, context);

    EXPECT_NE(prompt.find("[Rin] Good night.\\nSleep well.\n"), std::string::npos);
    EXPECT_NE(prompt.find("1. [Naoya] 一行目\\n二行目\n"), std::string::npos);
    EXPECT_NE(prompt.find("2. [NARRATION] 朝が来た。\n"), std::string::npos);

    // Two entries between the tags means exactly two physical lines there.
    std::size_t open = prompt.find("<lines_to_translate>\n");
    ASSERT_NE(open, std::string::npos);
    open += std::string("<lines_to_translate>\n").size();
    std::size_t close = prompt.find("</lines_to_translate>", open);
    ASSERT_NE(close, std::string::npos);
    EXPECT_EQ(std::count(prompt.begin() + static_cast<std::ptrdiff_t>(open),
                         prompt.begin() + static_cast<std::ptrdiff_t>(close), '\n'),
              2);
}

// The escape becomes the break itself in the parsed value, so the cache holds
// what the game has to render.
TEST(Translate, parse_numbered_response_restores_a_line_break_inside_an_entry) {
    auto t = tr::parse_numbered_response(
        "1. First half\\nsecond half\n"
        "2. One\\ntwo\\nthree\n"
        "3. No break at all\n",
        3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(1), "First half\nsecond half");
    EXPECT_EQ(t.at(2), "One\ntwo\nthree");
    EXPECT_EQ(t.at(3), "No break at all");

    // A break hung off the end of an entry is trimmed: the engine re-attaches
    // the boundary bytes the original line carried.
    EXPECT_EQ(tr::parse_numbered_response("1. Alpha\\n\n", 1).at(1), "Alpha");

    // The break survives the speaker-tag strip and the leading indent.
    auto tagged = tr::parse_numbered_response(
        "1. [Naoya] \"Morning.\"\\n\"And to you.\"\n", 1);
    ASSERT_EQ(tagged.size(), 1u);
    EXPECT_EQ(tagged.at(1), "\"Morning.\"\n\"And to you.\"");
    auto indented = tr::parse_numbered_response(
        std::string("1. ") + U3000 + "First half\\nsecond half\n", 1);
    ASSERT_EQ(indented.size(), 1u);
    EXPECT_EQ(indented.at(1), std::string(U3000) + "First half\nsecond half");
}

// A cache file has to carry the break too -- it is the resume point of a run
// that is never repeated.
TEST(Translate, cache_file_round_trips_a_line_break) {
    std::string path = (fs::temp_directory_path() / "snu_ut_cache_breaks.json").u8string();
    tr::Cache cache;
    cache.set("一行目\n二行目", "First half\nsecond half");
    tr::save_cache(cache, path);
    tr::Cache back = tr::load_cache(path);
    ASSERT_TRUE(back.get("一行目\n二行目"));
    EXPECT_EQ(*back.get("一行目\n二行目"), "First half\nsecond half");
    fs::remove(fs::u8path(path));
}

// A numbered list inside a single source string used to poison its batch;
// one physical line per entry removes the shape by construction.
TEST(Translate, a_numbered_list_inside_a_source_line_parses_clean) {
    const std::string jp =
        std::string(U3000) + "１．巨乳系エッチ雑誌\n２．お尻系エッチ雑誌\n３．ロリコン系雑誌";
    std::vector<std::pair<std::string, std::string>> batch = {{"NARRATION", jp}};
    std::string prompt = tr::build_user_prompt(batch, {});
    EXPECT_NE(prompt.find("1. [NARRATION] " + std::string(U3000) +
                          "１．巨乳系エッチ雑誌\\n２．お尻系エッチ雑誌\\n３．ロリコン系雑誌\n"),
              std::string::npos);

    int rejected = -1;
    std::string tail = "unset";
    auto t = tr::parse_numbered_response(
        std::string("1. ") + U3000 +
            "1. Big-bust adult magazines\\n2. Rear-end adult magazines\\n"
            "3. Lolicon magazines\n",
        1, &rejected, &tail);
    ASSERT_EQ(t.size(), 1u) << "the list's own numbering was read as line numbers";
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tail.empty());
    EXPECT_EQ(t.at(1), std::string(U3000) +
                           "1. Big-bust adult magazines\n2. Rear-end adult magazines\n"
                           "3. Lolicon magazines");

    // The other list line in the corpus (offset 453982) numbers itself with
    // circled digits, which are not digits to the parser at all.
    const std::string circled = std::string(U3000) + "①エッチな漫画雑誌" + U3000 +
                                "②エッチな漫画単行本\n③エッチな雑誌（写真）" + U3000 +
                                "④エッチな本（写真）";
    auto circled_t = tr::parse_numbered_response(
        "1. " + tr::escape_line_breaks(circled) + "\n", 1, &rejected);
    ASSERT_EQ(circled_t.size(), 1u);
    EXPECT_EQ(rejected, 0);
    EXPECT_EQ(circled_t.at(1), circled);
}

// The escape is unambiguous because the source writes no backslashes; both
// facts are measured over the real extraction.
TEST(Translate, line_breaks_and_backslashes_over_the_current_extraction) {
    fs::path extracted = fs::u8path(std::string(SNU_PROJECT_DIR) +
                                    "\\script_output\\extracted_text.json");
    if (!fs::exists(extracted)) {
        GTEST_SKIP() << "extracted_text.json not produced yet";
    }
    bj::value root = snu::json_parse_file(extracted.u8string());
    std::int64_t total = 0, with_break = 0, internal = 0, backslash = 0;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& ev : strings->get_array()) {
            std::string text(ev.get_object().at("text").get_string());
            ++total;
            if (text.find('\\') != std::string::npos) ++backslash;
            if (text.find('\n') == std::string::npos) continue;
            ++with_break;
            std::string body = text;
            while (!body.empty() && body.back() == '\n') body.pop_back();
            if (body.find('\n') != std::string::npos) ++internal;
        }
    }
    EXPECT_EQ(total, 68129) << "corpus changed -- re-measure before trusting the rest";
    EXPECT_EQ(backslash, 0) << "a backslash in the source makes the wire escape ambiguous";
    EXPECT_EQ(internal, 6633);
    EXPECT_EQ(with_break, 8271);
}

// Japanese period/comma as numbering marks must be recognised, or the line
// silently folds into the entry before it.
TEST(Translate, parse_numbered_response_reads_a_japanese_numbering_mark) {
    const char* const IDEOGRAPHIC_STOP = "\xE3\x80\x82";   // U+3002
    const char* const IDEOGRAPHIC_COMMA = "\xE3\x80\x81";  // U+3001
    for (const char* mark : {IDEOGRAPHIC_STOP, IDEOGRAPHIC_COMMA}) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(
            "1" + std::string(mark) + " Alpha\n2" + mark + " Bravo\n", 2, &rejected);
        ASSERT_EQ(t.size(), 2u) << mark;
        EXPECT_EQ(t.at(1), "Alpha") << mark;
        EXPECT_EQ(t.at(2), "Bravo") << mark;
        EXPECT_EQ(rejected, 0) << mark;
    }
    // Mixed with the marks the prompt asks for, an entry it opens is its own
    // rather than a tail on the one before it.
    auto mixed = tr::parse_numbered_response(
        "1. Alpha\n2" + std::string(IDEOGRAPHIC_STOP) + " Bravo\n3. Charlie\n", 3);
    ASSERT_EQ(mixed.size(), 3u);
    EXPECT_EQ(mixed.at(2), "Bravo") << "the line was glued onto entry 1";
}

// Unreadable numbering is reported and rejected; silently folding it in would
// glue two translations together.
TEST(Translate, parse_numbered_response_refuses_numbering_it_cannot_read) {
    struct Case {
        const char* what;
        const char* body;
    };
    const Case cases[] = {
        {"hyphen for a mark", "1. Alpha\n2- Bravo\n3. Charlie\n"},
        {"colon spaced away from the number", "1. Alpha\n7 : 30 he arrived\n2. Bravo\n"},
        {"paren spaced away from the number", "1. Alpha\n3 ) whatever\n2. Bravo\n"},
        {"the mark alone on its line", "1. Alpha\n2..\n"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(c.body, 3, &rejected);
        EXPECT_TRUE(t.empty()) << c.what << ": folded into the entry before it";
        EXPECT_GT(rejected, 0) << c.what << ": no signal at all";
    }

    // The cost of reading a hyphen as numbering would be an entry stolen in
    // range and in order, so a range and a measurement stay prose: the digit
    // after the mark is what decides, exactly as it does for a decimal.
    int rejected = -1;
    auto prose = tr::parse_numbered_response(
        "1. He waited.\n1985-1990 was the whole of it.\n2. Then he left.\n", 2, &rejected);
    ASSERT_EQ(prose.size(), 2u);
    EXPECT_EQ(rejected, 0);
    EXPECT_EQ(prose.at(1), "He waited. 1985-1990 was the whole of it.");
}

// Emphasis the model wrapped the numbering in is numbering, not translation --
// wherever it closes. Emphasis the translation opens for itself is text.
TEST(Translate, parse_numbered_response_keeps_emphasis_out_of_the_numbering) {
    struct Case {
        const char* body;
        const char* want;
    };
    const Case cases[] = {
        {"**1. Alpha**", "Alpha"},
        {"**1.** Bravo", "Bravo"},
        {"**1.** *Charlie*", "*Charlie*"},
        {"1.*sigh*", "*sigh*"},
        {"1.. Delta", "Delta"},
        {"1.Echo", "Echo"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(std::string(c.body) + "\n", 1, &rejected);
        ASSERT_EQ(t.size(), 1u) << c.body;
        EXPECT_EQ(t.at(1), c.want) << c.body;
        EXPECT_EQ(rejected, 0) << c.body;
    }
}

// A self-stopped answer with fewer entries than asked for may have merged
// lines; the count is the only guard, so it decides.
TEST(Translate, a_short_answer_that_stopped_on_its_own_cannot_be_applied) {
    EXPECT_TRUE(tr::answer_count_is_usable(150, 150, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 149, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 149, ""));  // stop_reason absent
    EXPECT_FALSE(tr::answer_count_is_usable(150, 0, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 151, "end_turn"));

    // A cut answer is short because the budget ran out, and the entry the cut
    // landed in has already been dropped. Gating on the count there would throw
    // away the paid prefix and re-request a batch that truncates again.
    EXPECT_TRUE(tr::answer_count_is_usable(150, 90, "max_tokens"));
    EXPECT_TRUE(tr::answer_count_is_usable(150, 90, "model_context_window_exceeded"));

    // The merge itself: nothing in the numbering is wrong.
    int rejected = -1;
    auto merged = tr::parse_numbered_response("1. A\n2. B and C together\n3. D\n", 4,
                                              &rejected);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(rejected, 0) << "the numbering is in order and in range";
    EXPECT_FALSE(tr::answer_count_is_usable(4, merged.size(), "end_turn"));
}

// An answer cut at max_tokens ends mid-sentence. That fragment is non-empty,
// so it caches as a finished translation and the line never comes back.
TEST(Translate, truncated_answer_loses_only_its_last_entry) {
    std::map<int, std::string> full = {
        {1, "He turned and left."}, {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"}};

    auto t = full;
    EXPECT_TRUE(tr::drop_truncated_entry(t, "max_tokens"));
    EXPECT_EQ(t.size(), 2u);
    EXPECT_EQ(t.count(3), 0u) << "the half-written entry was kept";
    EXPECT_EQ(t.at(1), "He turned and left.");
    EXPECT_EQ(t.at(2), "The door stayed open.");

    // Running out of context window cuts the answer the same way running out
    // of output budget does, and leaves the same fragment behind.
    auto ctx = full;
    EXPECT_TRUE(tr::drop_truncated_entry(ctx, "model_context_window_exceeded"));
    EXPECT_EQ(ctx.size(), 2u);
    EXPECT_EQ(ctx.count(3), 0u);

    // A finished answer is left alone, whatever else the model reports.
    for (const char* reason : {"end_turn", "stop_sequence", ""}) {
        auto keep = full;
        EXPECT_FALSE(tr::drop_truncated_entry(keep, reason)) << reason;
        EXPECT_EQ(keep.size(), 3u) << reason;
    }

    // The cut lands in the highest entry PRESENT, which a gap makes different
    // from the number of lines that were asked for.
    std::map<int, std::string> sparse = {{1, "One."}, {2, "Two."}, {7, "Seven and a ha"}};
    EXPECT_TRUE(tr::drop_truncated_entry(sparse, "max_tokens"));
    EXPECT_EQ(sparse.count(7), 0u);
    EXPECT_EQ(sparse.size(), 2u);

    // Nothing to drop when the batch was already discarded.
    std::map<int, std::string> none;
    EXPECT_FALSE(tr::drop_truncated_entry(none, "max_tokens"));
}

// A refusal discards the whole batch; dropping only the last entry would
// permanently cache its fragmented neighbours.
TEST(Translate, a_refused_answer_is_discarded_whole) {
    const std::map<int, std::string> full = {
        {1, "He turned and left."}, {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"}};

    auto refused = full;
    EXPECT_TRUE(tr::discard_refused_answer(refused, "refusal"));
    EXPECT_TRUE(refused.empty()) << "entries survived a refusal and would be cached";

    // Dropping the last entry is the wrong repair here, so the two must not be
    // confused: this is the pair the caller applies, in this order.
    auto both = full;
    ASSERT_TRUE(tr::discard_refused_answer(both, "refusal"));
    EXPECT_FALSE(tr::drop_truncated_entry(both, "refusal"));
    EXPECT_TRUE(both.empty());

    // Every other stop reason leaves the answer alone.
    for (const char* reason : {"end_turn", "max_tokens", "stop_sequence",
                               "model_context_window_exceeded", ""}) {
        auto keep = full;
        EXPECT_FALSE(tr::discard_refused_answer(keep, reason)) << reason;
        EXPECT_EQ(keep.size(), 3u) << reason;
    }

    // A refusal before any text is still a rejected batch, so the caller can
    // report it rather than reading an empty answer as "the model said nothing".
    std::map<int, std::string> none;
    EXPECT_TRUE(tr::discard_refused_answer(none, "refusal"));
}

// The raw-answer log sits beside the document it belongs to and is off by default.
TEST(Translate, raw_answer_log_sits_beside_the_run_it_belongs_to) {
    std::string sibling =
        (fs::temp_directory_path() / "script_output" / "translated_text.json").u8string();
    std::string log = tr::raw_answer_log_path(sibling);
    EXPECT_EQ(fs::u8path(log).parent_path(), fs::u8path(sibling).parent_path());
    EXPECT_EQ(fs::u8path(log).filename().u8string(), "anthropic_raw_answers.log");

    EXPECT_TRUE(tr::raw_answer_log().empty()) << "a side file is written unasked";
    tr::set_raw_answer_log(log);
    EXPECT_EQ(tr::raw_answer_log(), log);
    tr::set_raw_answer_log("");
}

// The assembled user prompt carries real [Speaker] tags and the context
// block -- if either is dropped the model sees [NARRATION] for everything.
TEST(Translate, user_prompt_contains_speaker_tags_and_context) {
    std::vector<std::pair<std::string, std::string>> batch = {
        {"Naoya", "「おはよう」"}, {"NARRATION", "朝が来た。"}};
    std::vector<std::pair<std::string, std::string>> context = {
        {"Rin", "Good night."}};
    std::string prompt = tr::build_user_prompt(batch, context);
    EXPECT_NE(prompt.find("<previous_context>\n[Rin] Good night.\n"),
              std::string::npos);
    EXPECT_NE(prompt.find("1. [Naoya] 「おはよう」"), std::string::npos);
    EXPECT_NE(prompt.find("2. [NARRATION] 朝が来た。"), std::string::npos);
    EXPECT_NE(prompt.find("Output EXACTLY 2 numbered translations"),
              std::string::npos);
}

// Failed lines stay out of the cache but join the rolling context.
TEST(Translate, failed_line_stays_uncached_but_reaches_context) {
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    tr::apply_line_result(cache, context, "Naoya", "「おはよう」", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), 0u);
    ASSERT_EQ(context.size(), 1u);
    EXPECT_EQ(context[0].first, "Naoya");
    EXPECT_EQ(context[0].second, "「おはよう」");

    tr::apply_line_result(cache, context, "Rin", "朝が来た。", "Morning came.");
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    ASSERT_EQ(context.size(), 2u);
    EXPECT_EQ(context[1].second, "Morning came.");

    // A source break the answer dropped is reported and cached anyway: the
    // value is a plausible translation, so nothing can tell it from a correct
    // one, and refusing it would re-queue the same batch on every run.
    tr::apply_line_result(cache, context, "Naoya", "一行目\n二行目", "One line only.");
    ASSERT_TRUE(cache.get("一行目\n二行目"));
    EXPECT_EQ(*cache.get("一行目\n二行目"), "One line only.");
    // A break hanging off the end is a segment boundary the engine re-attaches,
    // so an English line without one is correct and says nothing about it.
    tr::apply_line_result(cache, context, "Naoya", "一行だけ\n", "One line only.");
    EXPECT_TRUE(cache.contains("一行だけ\n"));
}

TEST(Translate, failed_entry_predicate_only_fires_on_japanese_identity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));  // non-ASCII, still a failure
    // A resource name carrying one stray Japanese byte is an echo like any
    // other: the run re-requests it and pays for it again.
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("直哉", "Naoya"));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));  // ASCII identity is legitimate
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));  // ASCII identifier
    EXPECT_FALSE(tr::is_failed_entry("", ""));
}

// The purge takes out exactly the failure artifacts: seeded name pairs, real
// translations and ASCII identities all survive.
TEST(Translate, purge_removes_failed_entries_and_keeps_the_rest) {
    tr::Cache cache;
    cache.set("直哉", "Naoya");
    cache.set("こんにちは", "こんにちは");
    cache.set("朝が来た。", "Morning came.");
    cache.set("OK", "OK");
    cache.set("桜の詩", "桜の詩");
    cache.set("稟", "Rin");

    EXPECT_EQ(tr::purge_failed_entries(cache), 2u);
    EXPECT_EQ(cache.size(), 4u);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("桜の詩"));
    ASSERT_TRUE(cache.get("直哉"));
    EXPECT_EQ(*cache.get("直哉"), "Naoya");
    EXPECT_TRUE(cache.contains("朝が来た。"));
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("稟"));

    // Insertion order survives -- it decides the cache file's key order.
    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].first, "直哉");
    EXPECT_EQ(items[1].first, "朝が来た。");
    EXPECT_EQ(items[2].first, "OK");
    EXPECT_EQ(items[3].first, "稟");

    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);  // idempotent
    EXPECT_EQ(cache.size(), 4u);
}

namespace {

// A document shaped like the extraction: one script, `n` dialogue entries,
// each attributed to `speaker` unless it is empty.
bj::object dialogue_document(std::size_t n, const std::string& speaker) {
    bj::array strings;
    for (std::size_t i = 0; i < n; ++i) {
        bj::object e;
        e["type"] = "dialogue";
        e["text"] = "「おはよう」";
        if (!speaker.empty()) e["speaker"] = speaker;
        strings.push_back(std::move(e));
    }
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object doc;
    doc["00_test_script"] = std::move(script);
    return doc;
}

}  // namespace

// Speaker gate over the real extraction; a one-shot run that lost its
// speakers would permanently translate in the wrong voice.
TEST(Translate, speaker_gate_passes_over_the_current_extraction) {
    fs::path extracted = fs::u8path(std::string(SNU_PROJECT_DIR) +
                                    "\\script_output\\extracted_text.json");
    if (!fs::exists(extracted)) {
        GTEST_SKIP() << "extracted_text.json not produced yet";
    }
    bj::value root = snu::json_parse_file(extracted.u8string());
    tr::SpeakerCheckReport report = tr::run_speaker_checks(root.get_object());
    EXPECT_TRUE(report.passed) << "check " << report.failed_check << ": " << report.reason;
    EXPECT_EQ(report.failed_check, 0);
    EXPECT_EQ(report.speakers_in_data, 143u);
    EXPECT_EQ(report.glossary_entries, 143u)
        << "the table and the scripts have to cover each other exactly";
    EXPECT_EQ(report.dialogue_total, 48758u);
    EXPECT_EQ(report.dialogue_sampled, 50u);
}

// The regression the gate exists for: the walker stops finding the name
// pointer, every dialogue line reads [NARRATION], and nothing else notices.
TEST(Translate, speaker_gate_refuses_a_document_that_lost_its_speakers) {
    tr::SpeakerCheckReport lost = tr::run_speaker_checks(dialogue_document(50, ""));
    EXPECT_FALSE(lost.passed);
    EXPECT_EQ(lost.failed_check, 1);
    EXPECT_NE(lost.reason.find("NARRATION"), std::string::npos);

    // An extraction with almost nothing in it fails at the same check rather
    // than passing on a sample too small to mean anything.
    tr::SpeakerCheckReport thin = tr::run_speaker_checks(dialogue_document(4, "直哉"));
    EXPECT_FALSE(thin.passed);
    EXPECT_EQ(thin.failed_check, 1);

    // Speakers alone are not enough: the table and the scripts must cover each
    // other, which is check 5 and which no partial document can satisfy.
    tr::SpeakerCheckReport partial = tr::run_speaker_checks(dialogue_document(50, "直哉"));
    EXPECT_FALSE(partial.passed);
    EXPECT_EQ(partial.failed_check, 5);
}

// A failed gate stops the step before reading a key or writing a file.
TEST(Translate, a_failed_gate_stops_the_translation_step_before_it_spends) {
    fs::path dir = fs::temp_directory_path() / "snu_ut_gate";
    fs::remove_all(dir);
    fs::create_directories(dir);

    tr::TranslateOptions opt;
    opt.input_file = (dir / "extracted_text.json").u8string();
    opt.cache_file = (dir / "translation_cache_anthropic.json").u8string();
    opt.output_file = (dir / "translated_text.json").u8string();
    opt.tsv_file = (dir / "translations.tsv").u8string();
    // A cache to lose, so "nothing was written" is a claim about this run
    // rather than about an empty folder.
    tr::Cache seeded;
    seeded.set("朝が来た。", "Morning came.");
    tr::save_cache(seeded, opt.cache_file);
    opt.fresh_run = true;

    snu::write_file(opt.input_file, bj::serialize(bj::value(dialogue_document(50, ""))));

    EXPECT_EQ(tr::translate_all(opt), 2) << "a refused gate has to fail the step";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
    EXPECT_TRUE(fs::exists(fs::u8path(opt.cache_file)))
        << "the run stopped before it could drop work already paid for";
    EXPECT_EQ(tr::load_cache(opt.cache_file).size(), 1u);

    fs::remove_all(dir);
}

// Three pipeline steps; the speaker gate and table build are folded into the
// translation step so they cannot be skipped.
TEST(Translate, the_pipeline_runs_three_steps_in_order) {
    const auto& steps = snu::apps::pipeline_steps();
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_STREQ(steps[0].name, "01_extract");
    EXPECT_STREQ(steps[1].name, "02_translate");
    EXPECT_STREQ(steps[2].name, "deploy");
    for (const auto& step : steps) {
        EXPECT_EQ(std::string(step.name).find("speaker_gate"), std::string::npos);
        EXPECT_EQ(std::string(step.name).find("build_translations"), std::string::npos);
        EXPECT_FALSE(std::string(step.what).empty());
    }

    // The caps the orchestrator forwards are validated by the same rule the
    // translation step applies, before either of them does any work.
    EXPECT_TRUE(tr::validate_options(0, 0, 0).has_value()) << "--batch 0";
    EXPECT_TRUE(tr::validate_options(150, 0, -1).has_value()) << "--max-batches -1";
    EXPECT_TRUE(tr::validate_options(150, -1, 0).has_value()) << "--test -1";
    EXPECT_FALSE(tr::validate_options(150, 0, 0).has_value());
}

// Glossary sanity: the pinned names are present and both views agree.
TEST(Translate, glossary_contains_core_names_and_is_consistent) {
    const auto& map = tr::name_translations();
    EXPECT_EQ(map.count("直哉"), 1u);
    EXPECT_EQ(map.at("直哉"), "Naoya");
    // Ordered table and lookup map stay in sync.
    EXPECT_EQ(tr::name_translations_ordered().size(), map.size());
}

// The guard refuses flags that would delete a paid cache until --discard-cache
// confirms the intent.
TEST(Translate, a_flag_that_deletes_a_paid_cache_is_refused_until_it_is_meant) {
    // An empty or nearly empty cache is what --test was written for.
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD,
                                          "--test", false).has_value());

    // One entry past the threshold, every flag is refused.
    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1,
                                            flag, false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    // The count is in the message, because "some lines" is not a reason to
    // stop and "56103 lines" is.
    auto why = tr::refuse_cache_discard(56103, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    // --discard-cache is the caller saying they meant it.
    EXPECT_FALSE(tr::refuse_cache_discard(56103, "--test", true).has_value());
}

// The guard reads the cache file itself, so a missing file is not an error and
// a real one is counted rather than estimated.
TEST(Translate, the_cache_guard_counts_the_file_on_disk) {
    const std::string missing =
        (fs::temp_directory_path() / "snu_ut_no_such_cache.json").u8string();
    fs::remove(fs::u8path(missing));
    EXPECT_EQ(tr::cache_entry_count(missing), 0u);

    const std::string path =
        (fs::temp_directory_path() / "snu_ut_guard_cache.json").u8string();
    tr::Cache cache;
    for (int i = 0; i < 7; ++i)
        cache.set("日本語" + std::to_string(i), "English " + std::to_string(i));
    tr::save_cache(cache, path);
    EXPECT_EQ(tr::cache_entry_count(path), 7u);
    fs::remove(fs::u8path(path));
}
