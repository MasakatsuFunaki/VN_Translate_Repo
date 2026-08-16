// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-2 helpers: the inline-speaker matcher, the STRICT response parser,
// postprocess, and the cache/prompt byte formats.
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/util.h"
#include "test_support.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

using namespace frat;
using namespace frat::translate;

TEST(Translate, NeedsTranslation_Threshold2NotThree) {
    EXPECT_FALSE(needs_translation(""));
    EXPECT_FALSE(needs_translation("   "));
    EXPECT_FALSE(needs_translation("\xE3\x80\x80"));  // U+3000 only
    EXPECT_FALSE(needs_translation("ab"));            // <= 2 codepoints, no JP
    EXPECT_FALSE(needs_translation("abc"));
    EXPECT_FALSE(needs_translation("……"));            // punctuation is not JP
    EXPECT_TRUE(needs_translation("漢字"));
    EXPECT_TRUE(needs_translation("こんにちは"));
    EXPECT_TRUE(needs_translation("カタカナ"));
}

TEST(Translate, TranslatableIndices_IncludesNameType) {
    boost::json::object fdata;
    boost::json::array strings;
    const char* types[] = {"dialogue", "narrative", "name", "menu", "other"};
    for (const char* t : types) {
        boost::json::object s;
        s["text"] = "漢字";
        s["type"] = t;
        strings.push_back(s);
    }
    fdata["strings"] = strings;
    // "name" IS translatable in this game; anything else is not.
    EXPECT_EQ(translatable_indices(fdata), (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST(Translate, InlineSpeakerMatch_LengthAndCharClass) {
    EXPECT_EQ(inline_speaker_match("大智「x」").value_or(""), "大智");
    // U+FF0F ／ is allowed; U+FF08 （ is not.
    EXPECT_EQ(inline_speaker_match("／美桜「x」").value_or(""), "／美桜");
    EXPECT_FALSE(inline_speaker_match("（あ「x」").has_value());
    // The greedy {1,12} can only succeed at the maximal allowed run, so 13
    // leading allowed codepoints never match.
    EXPECT_TRUE(inline_speaker_match("あいうえおかきくけこさし「x」").has_value());
    EXPECT_FALSE(inline_speaker_match("あいうえおかきくけこさしす「x」").has_value());
    // Excluded characters in the name -- but only the ASCII ones: fullwidth Ａ
    // is outside the negated class, which is why 男Ａ is a valid speaker key.
    EXPECT_TRUE(inline_speaker_match("男Ａ「x」").has_value());
    EXPECT_FALSE(inline_speaker_match("A大智「x」").has_value());
    EXPECT_FALSE(inline_speaker_match("a「x」").has_value());
    EXPECT_FALSE(inline_speaker_match("1「x」").has_value());
    EXPECT_FALSE(inline_speaker_match(" 「x」").has_value());
    EXPECT_FALSE(inline_speaker_match("\xE3\x80\x80" "「x」").has_value());
    EXPECT_FALSE(inline_speaker_match("(あ「x」").has_value());
    // The matcher is happy with prose fragments; only extract_speaker's
    // glossary gate rejects them.
    EXPECT_TRUE(inline_speaker_match("転校が決まった当初は「x」").has_value());
    EXPECT_EQ(extract_speaker("転校が決まった当初は「x」"), "NARRATION");
}

TEST(Translate, ExtractSpeaker_CrlfBranchFallsBackToRawJapanese) {
    // Unlike the sibling games' pipelines, an unknown CRLF name is returned
    // RAW rather than replaced by NARRATION.
    EXPECT_EQ(extract_speaker("未知\r\nText"), "未知");
    EXPECT_EQ(extract_speaker("大智\r\nText"), "Daichi");
    EXPECT_EQ(extract_speaker("大智「よし」"), "Daichi");
    EXPECT_EQ(extract_speaker("／美桜「あ」"), "Mio");
    EXPECT_EQ(extract_speaker("ふつうの地の文です。"), "NARRATION");
    // Empty first segment -> falls through to the inline probe on the WHOLE
    // text, not to NARRATION directly.
    EXPECT_EQ(extract_speaker("\r\n大智「よし」"), "NARRATION");
}

TEST(Translate, ParseTranslations_StrictAcceptsAndRejects) {
    using M = std::map<int, std::string>;
    EXPECT_EQ(parse_translations("1. First\n2. Second\n3. Third", 3),
              (M{{1, "First"}, {2, "Second"}, {3, "Third"}}));
    // 0-indexed responses are shifted, with a WARNING line.
    EXPECT_EQ(parse_translations("0. First\n1. Second\n2. Third", 3),
              (M{{1, "First"}, {2, "Second"}, {3, "Third"}}));
    // The regression that poisoned the cache around idx 720-726: N-1 outputs
    // for N inputs used to shift every remaining line by one.
    EXPECT_TRUE(parse_translations("1. a\n2. b", 3).empty());
    EXPECT_TRUE(parse_translations("1. a\n2. b\n3. c\n4. d", 3).empty());
    EXPECT_TRUE(parse_translations("1. a\n2. b\n4. d", 4).empty());
    EXPECT_TRUE(parse_translations("1. a\n2. b\n2. b_alt\n3. c", 3).empty());
    EXPECT_TRUE(parse_translations("", 5).empty());
    EXPECT_TRUE(parse_translations("Just chatter, no numbers", 3).empty());
    // Unnumbered chatter around a clean sequence is ignored.
    EXPECT_EQ(parse_translations(
                  "Here are the translations:\n1. one\n\n2. two\n3. three\nNote: done", 3),
              (M{{1, "one"}, {2, "two"}, {3, "three"}}));
    // Literal backslash-r-backslash-n decodes to a real CRLF.
    EXPECT_EQ(parse_translations("1. line one\\r\\nline two", 1),
              (M{{1, "line one\r\nline two"}}));
}

TEST(Translate, ParseTranslations_StripsSpeakerTagIncludingAmpersand) {
    using M = std::map<int, std::string>;
    EXPECT_EQ(parse_translations("[NARRATION] 1. text", 1), (M{{1, "text"}}));
    EXPECT_EQ(parse_translations("[Mother (Shizuko)] 1. text", 1), (M{{1, "text"}}));
    // This game's tag class accepts '&'; the sibling games' does not.
    EXPECT_EQ(parse_translations("[R&B] 1. text", 1), (M{{1, "text"}}));
    EXPECT_EQ(parse_translations("[Yuka's Mother] 1. text", 1), (M{{1, "text"}}));
    // A tag containing a digit is not a tag at all -- the line keeps it and
    // then fails the number match.
    EXPECT_TRUE(parse_translations("[Man 1] 1. text", 1).empty());
}

TEST(Translate, Postprocess_NamePrefixSubstitutionIsLongestFirst) {
    // Longest-first + break: 園田Ｈ must not be seen as 園田 followed by Ｈ.
    EXPECT_EQ(postprocess("日本語", "園田Ｈ「x」"), "Sonoda (H)「x」");
    EXPECT_EQ(postprocess("日本語", "園田「x」"), "Sonoda「x」");
}

TEST(Translate, Postprocess_ReattachesBracketsWhenClaudeDroppedThem) {
    EXPECT_EQ(postprocess("大智「よし」", "Alright"), "Daichi「Alright」");
    // Already bracketed -> left alone.
    EXPECT_EQ(postprocess("大智「よし」", "Daichi「Alright」"), "Daichi「Alright」");
    // Trailing CRLF on the original is restored on the translation.
    EXPECT_EQ(postprocess("大智「よし」\r\n", "Alright"), "Daichi「Alright」\r\n");
    // Unchanged / empty translations short-circuit before sanitising.
    EXPECT_EQ(postprocess("日本語", ""), "");
    EXPECT_EQ(postprocess("日本語", "日本語"), "日本語");
}

TEST(Translate, SanitizeAscii_FlattensSmartPunctuation) {
    const std::string out = postprocess("日本語", "\xE2\x80\x9C" "Hello" "\xE2\x80\x9D"
                                                  " \xE2\x80\x94 nope\xE2\x80\xA6");
    EXPECT_NE(out.find("\"Hello\""), std::string::npos);
    EXPECT_NE(out.find("--"), std::string::npos);
    EXPECT_NE(out.find("..."), std::string::npos);
}

TEST(Translate, EnHasJapaneseContent_ExcludesSoundMarks) {
    EXPECT_FALSE(en_has_japanese_content("ah\xE3\x82\x9B" "hh"));  // U+309B
    EXPECT_FALSE(en_has_japanese_content("Ahh \xE3\x83\xBC"));     // U+30FC
    EXPECT_FALSE(en_has_japanese_content("plain english"));
    EXPECT_TRUE(en_has_japanese_content("ああ"));
    EXPECT_TRUE(en_has_japanese_content("ア"));
    EXPECT_TRUE(en_has_japanese_content("漢"));
}

TEST(Translate, SaveCache_Indent2_CRLF_InsertionOrder) {
    frat_test::ScratchDir dir("cache");
    const std::string path = dir / "cache.json";
    Cache cache;
    cache.set("こんにちは", "Hello");
    cache.set("太一", "Taichi");
    cache.set("a\"b", "c\\d");
    save_cache(cache, path);

    const Bytes written = read_file(path);
    // The cache file's exact bytes: indent 2, raw (non-escaped) UTF-8, CRLF
    // line endings, keys in insertion order.
    EXPECT_EQ(written.size(), 76u);
    EXPECT_EQ(frat_test::sha256_hex(written.data(), written.size()),
              "a794f960f52d07efe55ca6a801a035784d17b4d0581c5c85d35bd8ce468c2fca");

    const Cache back = load_cache(path);
    ASSERT_EQ(back.size(), 3u);
    EXPECT_EQ(back.items()[0].first, "こんにちは");
    EXPECT_EQ(back.items()[2].second, "c\\d");
}

TEST(Translate, BuildUserPrompt_ByteFormatIsPinned) {
    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < 60; ++i) {
        std::string t = "ctx" + std::to_string(i) + " ";
        for (int k = 0; k < 120; ++k) t += "あ";
        ctx.emplace_back("S" + std::to_string(i), t);
    }
    const std::vector<std::pair<std::string, std::string>> batch = {
        {"Daichi", "大智「よし」"}, {"NARRATION", "行\r\n次"}, {"Mio", "／美桜「あ」"}};
    const std::string prompt = build_user_prompt(batch, ctx);

    // Only the last CONTEXT_WINDOW(50) entries, each truncated to 100
    // CODEPOINTS; the batch text \r\n-escaped.
    EXPECT_EQ(prompt.rfind("<previous_context>\n[S10] ctx10 ", 0), 0u);
    EXPECT_EQ(char_len(prompt), 5950u);
    EXPECT_EQ(frat_test::sha256_hex(prompt),
              "c631cc6cbc540a8a19f7d4363893896d26cf1d3de20f6cbb7f7c3afa9e099f91");
}

// The guard refuses flags that would delete a paid cache until --discard-cache
// confirms the intent.
TEST(Translate, AFlagThatDeletesAPaidCacheIsRefusedUntilItIsMeant) {
    // An empty or nearly empty cache is what --test was written for.
    EXPECT_FALSE(refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(refuse_cache_discard(CACHE_DISCARD_THRESHOLD, "--test", false)
                     .has_value());

    // One entry past the threshold, every flag is refused.
    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = refuse_cache_discard(CACHE_DISCARD_THRESHOLD + 1, flag, false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    // The count is in the message, because "some lines" is not a reason to
    // stop and "56103 lines" is.
    auto why = refuse_cache_discard(56103, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    // --discard-cache is the caller saying they meant it.
    EXPECT_FALSE(refuse_cache_discard(56103, "--test", true).has_value());
}

// An echo -- the model handing back the line it was given -- is a failure only
// when the line is Japanese.  An English string that survives translation
// unchanged is a correct answer, so pure ASCII is never a failure.
TEST(Translate, FailedEntryPredicateOnlyFiresOnJapaneseIdentity) {
    EXPECT_TRUE(is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(is_failed_entry("……", "……"));  // non-ASCII, still an echo
    // A resource name carrying one stray Japanese byte is an echo like any
    // other: the run re-requests it and pays for it again.
    EXPECT_TRUE(is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(is_failed_entry("OK", "OK"));          // ASCII identity is right
    EXPECT_FALSE(is_failed_entry("bg152n", "bg152n"));  // ASCII identifier
    EXPECT_FALSE(is_failed_entry("", ""));
}

// The purge takes out exactly the echoes and leaves everything else where it
// was, key order included -- that order is the cache file's key order.
TEST(Translate, PurgeRemovesFailedEntriesAndKeepsTheRest) {
    Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(purge_failed_entries(cache), 2u);
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
    EXPECT_EQ(purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 3u);
}

TEST(Translate, Glossary_HasSeventyEightOrderedEntries) {
    EXPECT_EQ(name_translations_ordered().size(), 78u);
    EXPECT_EQ(name_translations_ordered().front().first, "大智");
    EXPECT_EQ(name_translations().at("園田Ｈ"), "Sonoda (H)");
    EXPECT_EQ(story_order(), (std::vector<std::string>{"bn.ypf"}));
    EXPECT_EQ(CONTEXT_WINDOW, 50);
    // Longest JP key first, ties broken by declaration order.
    EXPECT_GE(char_len(names_by_len_desc().front().first),
              char_len(names_by_len_desc().back().first));
}
