// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-2 tests: speaker resolution off the JSON `speaker` field, postprocess's
// name-plate re-attachment, prompt shape, response parsing, cache ordering and
// the two generated tables.
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include <boost/json.hpp>
#include <openssl/sha.h>

#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = crc::translate;
using namespace crc;

namespace {
constexpr const char* TAICHI = "\xE5\xA4\xAA\xE4\xB8\x80";  // 太一
constexpr const char* OPEN = "\xE3\x80\x8C";                // 「
constexpr const char* CLOSE = "\xE3\x80\x8D";               // 」

std::string sha256_hex(const Bytes& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), digest);
    std::string out;
    char buf[3];
    for (unsigned char b : digest) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

bj::object entry(const std::string& speaker, const std::string& text,
                 const std::string& type = "dialogue") {
    bj::object o;
    o["speaker"] = speaker;
    o["text"] = text;
    o["type"] = type;
    return o;
}
}  // namespace

TEST(Translate, NeedsTranslationTable) {
    EXPECT_TRUE(tr::needs_translation("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"));  // こんにちは
    EXPECT_TRUE(tr::needs_translation("\xE3\x82\xAB\xE3\x82\xBF\xE3\x82\xAB\xE3\x83\x8A"));              // カタカナ
    EXPECT_TRUE(tr::needs_translation("\xE6\xBC\xA2\xE5\xAD\x97"));                                      // 漢字
    EXPECT_FALSE(tr::needs_translation("Hello"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("\xE2\x80\xA6\xE2\x80\xA6"));  // ……
    EXPECT_FALSE(tr::needs_translation("ABC123"));
}

TEST(Translate, ExtractSpeakerReadsSpeakerFieldNotText) {
    // The regression the gate exists for: the speaker comes from the FIELD.
    EXPECT_EQ(tr::extract_speaker(entry(TAICHI, std::string(OPEN) + "\xE2\x80\xA6" + CLOSE)),
              "Taichi");
    // Legacy fallback: name embedded in the text before a \r\n.
    EXPECT_EQ(tr::extract_speaker(
                  entry("", std::string(TAICHI) + "\r\n" + OPEN + "\xE2\x80\xA6" + CLOSE)),
              "Taichi");
    // Unknown JP name passes through verbatim.
    EXPECT_EQ(tr::extract_speaker(entry("\xE6\x9C\xAA\xE7\x9F\xA5", "x")),
              "\xE6\x9C\xAA\xE7\x9F\xA5");
    // >20 CODEPOINTS -> NARRATION.  21 kana is 63 bytes, so a byte-length
    // check would reject far shorter names.
    std::string long_name;
    for (int i = 0; i < 21; ++i) long_name += "\xE3\x81\x82";  // あ x21
    EXPECT_EQ(tr::extract_speaker(entry(long_name, "x")), "NARRATION");
    // Exactly 20 is still accepted.
    std::string name20;
    for (int i = 0; i < 20; ++i) name20 += "\xE3\x81\x82";
    EXPECT_EQ(tr::extract_speaker(entry(name20, "x")), name20);
    EXPECT_EQ(tr::extract_speaker(entry("", "no crlf here")), "NARRATION");
}

TEST(Translate, PostprocessReattachesSpeakerAndSanitises) {
    const std::string original =
        std::string(TAICHI) + "\r\n" + OPEN + "\xE3\x81\xB5\xE3\x82\x93" + CLOSE;  // 太一\r\n「ふん」
    const std::string got = tr::postprocess(original, "Hmph.");
    EXPECT_EQ(got, "Taichi\r\nHmph.");

    // The LLM kept the Japanese name -- it must be replaced, not stacked.
    EXPECT_EQ(tr::postprocess(original, std::string(TAICHI) + "\r\nHmph."), "Taichi\r\nHmph.");
    // Already correct -> left alone.
    EXPECT_EQ(tr::postprocess(original, "Taichi\r\nHmph."), "Taichi\r\nHmph.");

    // sanitize_ascii runs on every non-empty translation.
    EXPECT_EQ(tr::postprocess("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                              "\xE2\x80\x9C" "Hello\xE2\x80\x9D \xE2\x80\x94 nope\xE2\x80\xA6"),
              "\"Hello\" -- nope...");

    // A trailing CRLF in the source is restored on the translation.
    EXPECT_EQ(tr::postprocess("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\r\n", "Japanese  "),
              "Japanese\r\n");
    // Unchanged translation short-circuits before sanitising.
    EXPECT_EQ(tr::postprocess("same", "same"), "same");
    EXPECT_EQ(tr::postprocess("x", ""), "");
}

TEST(Translate, SanitizeAsciiReplacesSmartPunctuation) {
    EXPECT_EQ(tr::sanitize_ascii("a\xE2\x80\x94" "b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("a\xE2\x80\x93" "b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("\xE2\x80\x98x\xE2\x80\x99"), "'x'");
    EXPECT_EQ(tr::sanitize_ascii("a\xE3\x80\x80" "b"), "a b");
}

TEST(Translate, ParseNumberedResponseStripsSpeakerTagsAndUnescapes) {
    // The tag regex is ANCHORED at the start of the line, so a tag that
    // appears AFTER the line number is part of the translation and survives.
    auto t = tr::parse_numbered_response("1. [Taichi] Hello\\r\\nWorld\n\n[NARRATION] 2) Foo\n");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1], "[Taichi] Hello\r\nWorld");
    EXPECT_EQ(t[2], "Foo");

    // \r\n must be unescaped BEFORE the bare \n, or "\\r\\n" becomes "\\r\n".
    auto t2 = tr::parse_numbered_response("1. a\\r\\nb\\nc");
    EXPECT_EQ(t2[1], "a\r\nb\nc");

    // Blank lines and non-numbered prose are skipped.
    auto t3 = tr::parse_numbered_response("\n  \nSure, here you go:\n3: Bar");
    ASSERT_EQ(t3.size(), 1u);
    EXPECT_EQ(t3[3], "Bar");
}

TEST(Translate, BuildUserPromptUsesFiftyLineContextWindow) {
    EXPECT_EQ(tr::CONTEXT_WINDOW, 50);
    EXPECT_EQ(tr::BATCH_SIZE, 150);

    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < 60; ++i) ctx.emplace_back("S" + std::to_string(i), "line " + std::to_string(i));
    const std::vector<std::pair<std::string, std::string>> batch = {{"Taichi", "a\r\nb"}};
    const std::string p = tr::build_user_prompt(batch, ctx);

    // Only the last 50 context lines are sent.
    EXPECT_EQ(p.find("[S9] "), std::string::npos);
    EXPECT_NE(p.find("[S10] line 10\n"), std::string::npos);
    EXPECT_NE(p.find("[S59] line 59\n"), std::string::npos);
    // \r\n is flattened to '|' in context, escaped in the batch lines.
    EXPECT_NE(p.find("1. [Taichi] a\\r\\nb\n"), std::string::npos);
    EXPECT_NE(p.find("Translate the 1 lines inside <lines_to_translate> to English. "
                     "Use the <previous_context> (if present) for scene continuity but do NOT "
                     "translate it. Output EXACTLY 1 numbered translations, one per line. "
                     "Do NOT include [SPEAKER] tags in your output."),
              std::string::npos);

    // Context truncates at 100 CODEPOINTS, not bytes: 120 kana would be 360
    // bytes, so a byte cut would send a third of the line.
    std::string kana;
    for (int i = 0; i < 120; ++i) kana += "\xE3\x81\x82";
    std::string expected_head = "[C] ";
    for (int i = 0; i < 100; ++i) expected_head += "\xE3\x81\x82";
    const std::string p2 = tr::build_user_prompt(batch, {{"C", kana}});
    EXPECT_NE(p2.find(expected_head + "\n"), std::string::npos);
}

TEST(Translate, CacheIsInsertionOrderedAndUpdateKeepsPosition) {
    tr::Cache c;
    c.set("a", "1");
    c.set("b", "2");
    c.set("a", "3");
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c.items()[0].first, "a");
    EXPECT_EQ(c.items()[0].second, "3");
    EXPECT_EQ(c.items()[1].first, "b");
}

TEST(Translate, GlossaryHasSeventyFivePairsInSourceOrder) {
    const auto& g = tr::name_translations_ordered();
    ASSERT_EQ(g.size(), 75u);
    EXPECT_EQ(g.front().first, TAICHI);
    EXPECT_EQ(g.front().second, "Taichi");

    const auto& m = tr::name_translations();
    EXPECT_EQ(m.size(), 75u);
    // 桜庭・友貴
    EXPECT_EQ(m.at("\xE6\xA1\x9C\xE5\xBA\xAD\xE3\x83\xBB\xE5\x8F\x8B\xE8\xB2\xB4"),
              "Sakuraba & Tomoki");
    // 太一（妄想）
    EXPECT_EQ(m.at("\xE5\xA4\xAA\xE4\xB8\x80\xEF\xBC\x88\xE5\xA6\x84\xE6\x83\xB3\xEF\xBC\x89"),
              "Taichi (Delusion)");
    // ＊＊
    EXPECT_EQ(m.at("\xEF\xBC\x8A\xEF\xBC\x8A"), "**");

    ASSERT_EQ(tr::story_order().size(), 1u);
    EXPECT_EQ(tr::story_order()[0], "sn.bin");
}

TEST(Translate, SystemPromptIsExactly3822Chars) {
    const std::string p = tr::SYSTEM_PROMPT;
    std::size_t cps = 0, i = 0;
    while (i < p.size()) {
        utf8_next(p, i);
        ++cps;
    }
    EXPECT_EQ(cps, 3822u);
    EXPECT_EQ(p.rfind("You are a professional Japanese-to-English translator", 0), 0u);
    EXPECT_NE(p.find("   Replace em dashes with --, ellipsis with ..., and curly quotes with "
                     "straight quotes.\n</formatting_rules>"),
              std::string::npos);
    EXPECT_EQ(p.size() - p.rfind("</formatting_rules>"), std::string("</formatting_rules>").size());
}

// Real-data invariants over the committed step-2 artefact.
TEST(Translate, DeployedTranslatedJsonInvariants) {
    const fs::path tp =
        fs::u8path(std::string(CRC_PROJECT_DIR) + "\\script_output\\translated_text.json");
    const fs::path ep =
        fs::u8path(std::string(CRC_PROJECT_DIR) + "\\script_output\\extracted_text.json");
    if (!fs::exists(tp) || !fs::exists(ep)) GTEST_SKIP() << "pipeline artefacts not present";

    // Both parses are bound to named values: the range-for below iterates a
    // reference into the document, which a temporary would have destroyed.
    const bj::value extracted = json_parse_file(ep.u8string());
    const bj::value translated = json_parse_file(tp.u8string());

    std::set<std::int64_t> src;
    for (const auto& sv :
         extracted.get_object().at("sn.bin").get_object().at("strings").get_array())
        src.insert(sv.get_object().at("offset").get_int64());

    std::size_t smart = 0, orphan = 0;
    for (const auto& sv :
         translated.get_object().at("sn.bin").get_object().at("strings").get_array()) {
        const auto& s = sv.get_object();
        if (!src.count(s.at("offset").get_int64())) ++orphan;

        const std::string jp(s.at("text").get_string());
        const std::string en(s.at("translated").get_string());
        if (en.empty() || en == jp) continue;  // untranslated / passthrough
        // formatting_rule #8: sanitize_ascii must have run on this path.
        std::size_t i = 0;
        while (i < en.size()) {
            const char32_t cp = utf8_next(en, i);
            if (cp == 0x2014 || cp == 0x2013 || cp == 0x2026 || cp == 0x2018 || cp == 0x2019 ||
                cp == 0x201C || cp == 0x201D || cp == 0x3000) {
                ++smart;
                break;
            }
        }
    }
    EXPECT_EQ(smart, 0u) << "translations still carrying smart punctuation";
    // Only translated-subset-of-extracted is checked: the other direction is
    // 02's own post-condition, and asserting it would block the very run that
    // refreshes translated_text.json after extraction grows.
    EXPECT_EQ(orphan, 0u) << "translated offsets with no extracted entry (stale run)";
}

// Cache load/save round trip over the real 43,256-entry artefact: insertion
// order, indent=2, CRLF.
//
// The committed translation_cache_anthropic.json is NOT itself the byte
// reference -- it was written with indent=1 by an older revision, while
// save_cache writes indent=2.  The digest below is the indent=2, CRLF form of
// that same content, i.e. what save_cache must produce for this cache.
TEST(Translate, SaveCacheWritesIndentTwoCrlfInInsertionOrder) {
    const fs::path ref = fs::u8path(std::string(CRC_PROJECT_DIR) +
                                    "\\script_output\\translation_cache_anthropic.json");
    if (!fs::exists(ref)) GTEST_SKIP() << "translation_cache_anthropic.json not present";

    const tr::Cache cache = tr::load_cache(ref.u8string());
    ASSERT_EQ(cache.size(), 43256u);
    // Insertion order survives the load: the glossary seeds the file's head.
    EXPECT_EQ(cache.items()[0].first, TAICHI);
    EXPECT_EQ(cache.items()[0].second, "Taichi");

    const fs::path tmp = fs::temp_directory_path() / "crc_ut_cache_roundtrip.json";
    tr::save_cache(cache, tmp.u8string());
    const Bytes out = read_file(tmp.u8string());
    fs::remove(tmp);

    EXPECT_EQ(out.size(), 4500800u);
    EXPECT_EQ(sha256_hex(out),
              "e9eaf6ff96a169fb5a5f8a392700c2470370cb1060da7d542da2cd94296d09b6");
}

// The guard refuses flags that would throw away a paid cache until
// --discard-cache confirms the intent.
TEST(Translate, AFlagThatDeletesAPaidCacheIsRefusedUntilItIsMeant) {
    // An empty or nearly empty cache is what a from-scratch run was written for.
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                      false).has_value());

    // One entry past the threshold, every flag is refused.
    for (const char* flag : {"--test", "--clean"}) {
        auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag,
                                        false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
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

// An echo -- the model handing back the line it was given -- is a failure only
// when the line is Japanese.  An English string that survives translation
// unchanged is a correct answer, so pure ASCII is never a failure.
TEST(Translate, FailedEntryPredicateOnlyFiresOnJapaneseIdentity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));  // non-ASCII, still an echo
    // A resource name carrying one stray Japanese byte is an echo like any
    // other: the run re-requests it and pays for it again.
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));          // ASCII identity is right
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));  // ASCII identifier
    EXPECT_FALSE(tr::is_failed_entry("", ""));
}

// The purge takes out exactly the echoes and leaves everything else where it
// was, key order included -- that order is the cache file's key order.
TEST(Translate, PurgeRemovesFailedEntriesAndKeepsTheRest) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 2u);
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    // Nothing left to remove on a second pass.
    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 3u);
}
