// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "storage/script_repository.h"

#include "common/utf8_text.h"
#include "ports/crypt_sync.h"
#include "ports/file_store.h"
#include "storage/json_format_style.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

std::string normalize_key(const std::filesystem::path& p) {
    std::string s = wide_to_utf8(p.lexically_normal().wstring());
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1U && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

class InMemoryFileStore final : public IFileStore {
public:
    void put_file(const std::filesystem::path& path, std::string content) {
        std::filesystem::path cur = path.lexically_normal().parent_path();
        while (!cur.empty() && normalize_key(cur) != ".") {
            const std::string dir_key = normalize_key(cur);
            if (!dirs_.insert(dir_key).second) {
                break;
            }
            const std::filesystem::path parent = cur.parent_path();
            if (parent == cur) {
                break;
            }
            cur = parent;
        }
        files_[normalize_key(path)] = std::move(content);
    }

    void add_dir(const std::filesystem::path& dir) {
        dirs_.insert(normalize_key(dir));
    }

    void fail_on_write_number(std::size_t call_index) noexcept {
        fail_write_call_ = call_index;
        write_calls_ = 0U;
    }

    void clear_faults() noexcept {
        fail_write_call_ = 0U;
        write_calls_ = 0U;
    }

    std::string raw_file(const std::filesystem::path& path) const {
        const auto it = files_.find(normalize_key(path));
        if (it == files_.end()) {
            return {};
        }
        return it->second;
    }

    bool exists(const std::filesystem::path& path) const override {
        const std::string key = normalize_key(path);
        return files_.find(key) != files_.end() || dirs_.find(key) != dirs_.end();
    }

    bool is_directory(const std::filesystem::path& path) const override {
        return dirs_.find(normalize_key(path)) != dirs_.end();
    }

    std::vector<std::filesystem::path> list_regular_files(
        const std::filesystem::path& dir) const override {
        const std::string dir_key = normalize_key(dir);
        std::vector<std::pair<std::string, std::filesystem::path>> matched;
        for (const auto& [file_key, unused_body] : files_) {
            (void)unused_body;
            const std::filesystem::path p(utf8_to_wide(file_key));
            if (normalize_key(p.parent_path()) == dir_key) {
                matched.emplace_back(wide_to_utf8(p.filename().wstring()), p);
            }
        }
        std::sort(matched.begin(), matched.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.first < rhs.first;
                  });
        std::vector<std::filesystem::path> out;
        out.reserve(matched.size());
        for (auto& item : matched) {
            out.push_back(std::move(item.second));
        }
        return out;
    }

    Result<std::string> read_text(
        const std::filesystem::path& path) const override {
        const auto it = files_.find(normalize_key(path));
        if (it == files_.end()) {
            return Result<std::string>::failure(ErrorCode::IoError,
                                                "file not found");
        }
        return Result<std::string>::success(it->second);
    }

    Status write_text_atomic(const std::filesystem::path& path,
                             std::string_view content) override {
        ++write_calls_;
        if (fail_write_call_ != 0U && write_calls_ == fail_write_call_) {
            return Status::error(ErrorCode::IoError, "injected atomic write fault");
        }
        put_file(path, std::string(content));
        return Status::ok();
    }

private:
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
    std::size_t write_calls_ = 0U;
    std::size_t fail_write_call_ = 0U;
};

class ProgrammableCryptSync final : public ICryptSync {
public:
    std::function<Status(const std::filesystem::path&)> on_decrypt;
    bool fail_encrypt = false;
    std::size_t decrypt_calls = 0U;
    std::size_t encrypt_calls = 0U;

    Status decrypt_if_needed(const std::filesystem::path& game_root) override {
        ++decrypt_calls;
        if (on_decrypt) {
            return on_decrypt(game_root);
        }
        return Status::ok();
    }

    Status encrypt_if_tracked(const std::filesystem::path& game_root) override {
        (void)game_root;
        ++encrypt_calls;
        if (fail_encrypt) {
            return Status::error(ErrorCode::CryptSyncFailed,
                                 "injected VNE2 encryption failure");
        }
        return Status::ok();
    }
};

const ScriptEntry* find_entry_by_jp(const std::vector<ScriptEntry>& entries,
                                    std::string_view jp_text) {
    const auto it = std::find_if(
        entries.begin(), entries.end(),
        [jp_text](const ScriptEntry& e) { return e.jp_text == jp_text; });
    if (it == entries.end()) {
        return nullptr;
    }
    return &(*it);
}

TEST(ScriptRepository, ParsesShapesABAndCWithWrappedAndDirectArrays) {
    constexpr std::string_view kShapeAandB = R"({
  "01_shape_a.ks": {
    "strings": [
      {
        "type": "DIALOGUE",
        "speaker": "美咲",
        "speaker_en": "Misaki",
        "text": "「おはよう」",
        "translated": "\"Good morning.\""
      }
    ]
  },
  "02_shape_b.scn": {
    "lines": [
      {
        "content": "風が吹いた。",
        "translation": "The wind blew."
      }
    ]
  },
  "03_shape_c_direct": [
    {
      "jp": "夜が明けた。",
      "en": "Dawn broke."
    }
  ]
}
)";

    const Result<LoadedCorpus> ab_res =
        ScriptRepository::parse_corpus_memory(kShapeAandB);
    ASSERT_TRUE(ab_res.is_ok()) << ab_res.error_message();
    ASSERT_EQ(ab_res.value().entries.size(), 3U);

    const ScriptEntry& e0 = ab_res.value().entries[0];
    EXPECT_EQ(e0.global_index, 0U);
    EXPECT_EQ(e0.script_name, "01_shape_a.ks");
    EXPECT_EQ(e0.array_key, "strings");
    EXPECT_EQ(e0.entry_index, 0U);
    EXPECT_EQ(e0.text_key, "text");
    EXPECT_EQ(e0.speaker, "Misaki");
    EXPECT_EQ(e0.jp_text, "「おはよう」");
    EXPECT_EQ(e0.current_en, "\"Good morning.\"");

    const ScriptEntry& e1 = ab_res.value().entries[1];
    EXPECT_EQ(e1.global_index, 1U);
    EXPECT_EQ(e1.script_name, "02_shape_b.scn");
    EXPECT_EQ(e1.array_key, "lines");
    EXPECT_EQ(e1.text_key, "content");
    EXPECT_EQ(e1.speaker, "NARRATION");
    EXPECT_EQ(e1.jp_text, "風が吹いた。");
    EXPECT_EQ(e1.current_en, "The wind blew.");

    const ScriptEntry& e2 = ab_res.value().entries[2];
    EXPECT_EQ(e2.global_index, 2U);
    EXPECT_EQ(e2.script_name, "03_shape_c_direct");
    EXPECT_EQ(e2.array_key, "");
    EXPECT_EQ(e2.text_key, "jp");
    EXPECT_EQ(e2.speaker, "NARRATION");
    EXPECT_EQ(e2.jp_text, "夜が明けた。");
    EXPECT_EQ(e2.current_en, "Dawn broke.");

    constexpr std::string_view kShapeCWrapped = R"({
  "scripts": {
    "scene_wrapped": [
      {
        "jp": "「行くぞ」",
        "translated": "\"Let's go.\"",
        "translation": "\"Let's go.\""
      }
    ]
  }
})";
    const Result<LoadedCorpus> wrapped_res =
        ScriptRepository::parse_corpus_memory(kShapeCWrapped);
    ASSERT_TRUE(wrapped_res.is_ok()) << wrapped_res.error_message();
    ASSERT_EQ(wrapped_res.value().entries.size(), 1U);
    EXPECT_EQ(wrapped_res.value().entries[0].script_name, "scene_wrapped");
    EXPECT_EQ(wrapped_res.value().entries[0].jp_text, "「行くぞ」");
    EXPECT_EQ(wrapped_res.value().entries[0].current_en, "\"Let's go.\"");

    const Result<LoadedCorpus> bad_json =
        ScriptRepository::parse_corpus_memory("[1, 2, 3]");
    EXPECT_FALSE(bad_json.is_ok());
    EXPECT_EQ(bad_json.error_code(), ErrorCode::CorruptJson);
}

TEST(ScriptRepository, TracksSpeakerStateMachineAndHarvestsSupplementalGlossary) {
    constexpr std::string_view kCorpusJson = R"({
  "01_scene": {
    "strings": [
      {"type": "NAME", "text": "和倉 賢一", "translated": "Kenichi Wakura"},
      {"type": "DIALOGUE", "text": "「おはよう、みんな」", "translated": "\"Good morning, everyone.\""},
      {"type": "COMMAND", "text": "fw 0", "translated": "fw 0"},
      {"type": "DIALOGUE", "text": "静かな教室だった。", "translated": "It was a quiet classroom."},
      {"type": "NAME", "text": "美咲", "translated": "Misaki"},
      {"type": "DIALOGUE", "text": "「今日からよろしく\\@", "translated": "\"Nice to meet you\\@"},
      {"type": "DIALOGUE", "text": "お願いね」", "translated": "starting today.\""},
      {"type": "NAME", "text": "美咲", "translated": "Misaki"},
      {"type": "DIALOGUE", "text": "＊", "translated": "*"},
      {"type": "DIALOGUE", "text": "翌朝になった。", "translated": "The next morning arrived."},
      {"type": "NAME", "text": "美咲", "translated": "Misaki"},
      {"type": "COMMAND", "text": "menu", "translated": "menu"},
      {"type": "DIALOGUE", "text": "選択肢が表示された。", "translated": "Choices appeared."},
      {"type": "DIALOGUE", "text": "／美咲「ここよ！」", "translated": "\"Over here!\""},
      {"type": "DIALOGUE", "text": "和倉 賢一\r\n「待ってくれ」", "translated": "Kenichi Wakura\r\n\"Wait for me.\""},
      {"type": "DIALOGUE", "speaker": "高瀬 瑞希", "speaker_en": "Mizuki Takase", "text": "「行きましょう」", "translated": "\"Let's go.\""},
      {"type": "DIALOGUE", "speaker": null, "text": "扉が閉まった。", "translated": "The door closed."},
      {"type": "DIALOGUE", "speaker": "", "text": "誰もいなかった。", "translated": "No one was there."},
      {"type": "NAME", "text": "美咲", "translated": "Misaki"}
    ]
  },
  "02_next_scene": {
    "strings": [
      {"type": "DIALOGUE", "text": "新しい章が始まった。", "translated": "A new chapter began."}
    ]
  }
})";

    const Result<LoadedCorpus> res =
        ScriptRepository::parse_corpus_memory(kCorpusJson);
    ASSERT_TRUE(res.is_ok()) << res.error_message();
    const LoadedCorpus& corpus = res.value();

    EXPECT_EQ(corpus.glossary.resolve_speaker("和倉 賢一"), "Kenichi Wakura");
    EXPECT_EQ(corpus.glossary.resolve_speaker("美咲"), "Misaki");
    EXPECT_EQ(corpus.glossary.resolve_speaker("高瀬 瑞希"), "Mizuki Takase");

    struct SpeakerProbe {
        std::string_view jp_text;
        std::string_view expected_speaker;
    };
    constexpr SpeakerProbe kProbes[] = {
        {"「おはよう、みんな」", "Kenichi Wakura"},
        {"静かな教室だった。", "NARRATION"},
        {"「今日からよろしく\\@", "Misaki"},
        {"お願いね」", "Misaki"},
        {"翌朝になった。", "NARRATION"},
        {"選択肢が表示された。", "NARRATION"},
        {"／美咲「ここよ！」", "Misaki"},
        {"和倉 賢一\r\n「待ってくれ」", "Kenichi Wakura"},
        {"「行きましょう」", "Mizuki Takase"},
        {"扉が閉まった。", "NARRATION"},
        {"誰もいなかった。", "NARRATION"},
        {"新しい章が始まった。", "NARRATION"},
    };

    for (const auto& probe : kProbes) {
        SCOPED_TRACE(probe.jp_text);
        const ScriptEntry* found = find_entry_by_jp(corpus.entries, probe.jp_text);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->speaker, probe.expected_speaker);
    }
}

TEST(ScriptRepository, PropagatesDuplicateJpUpdatesSyncsDualKeysAndPreservesStyle) {
    const std::string raw_crlf_4space =
        "{\r\n"
        "    \"scripts\": {\r\n"
        "        \"s1\": [\r\n"
        "            {\r\n"
        "                \"jp\": \"「待って！」\",\r\n"
        "                \"translated\": \"\\\"Wait up!\\\"\",\r\n"
        "                \"translation\": \"\\\"Wait up!\\\"\"\r\n"
        "            }\r\n"
        "        ],\r\n"
        "        \"s2\": [\r\n"
        "            {\r\n"
        "                \"jp\": \"「待って！」\",\r\n"
        "                \"translated\": \"\\\"Wait up!\\\"\",\r\n"
        "                \"translation\": \"\\\"Wait up!\\\"\"\r\n"
        "            },\r\n"
        "            {\r\n"
        "                \"jp\": \"「待って！」\",\r\n"
        "                \"translated\": \"「待って！」\"\r\n"
        "            },\r\n"
        "            {\r\n"
        "                \"jp\": \"「待って！」\",\r\n"
        "                \"translated\": \"\\\"Stop right there!\\\"\"\r\n"
        "            }\r\n"
        "        ]\r\n"
        "    }\r\n"
        "}";

    Improvement imp;
    imp.script_name = "s1";
    imp.entry_index = 0U;
    imp.jp_text = "「待って！」";
    imp.old_en = "\"Wait up!\"";
    imp.improved_en = "\"Hold on!\"";

    std::size_t updated_count = 0U;
    const Result<std::string> updated =
        ScriptRepository::apply_improvements_memory(
            raw_crlf_4space, {imp}, &updated_count);
    ASSERT_TRUE(updated.is_ok()) << updated.error_message();
    EXPECT_EQ(updated_count, 3U);

    const JsonFormatStyle out_style = detect_json_format_style(updated.value());
    EXPECT_EQ(out_style.indent_spaces, 4);
    EXPECT_TRUE(out_style.use_crlf);
    EXPECT_FALSE(out_style.trailing_newline);
    EXPECT_NE(updated.value().find("\"translation\": \"\\\"Hold on!\\\"\""),
              std::string::npos);

    const Result<LoadedCorpus> reparsed =
        ScriptRepository::parse_corpus_memory(updated.value());
    ASSERT_TRUE(reparsed.is_ok()) << reparsed.error_message();
    ASSERT_EQ(reparsed.value().entries.size(), 4U);
    EXPECT_EQ(reparsed.value().entries[0].current_en, "\"Hold on!\"");
    EXPECT_EQ(reparsed.value().entries[1].current_en, "\"Hold on!\"");
    EXPECT_EQ(reparsed.value().entries[2].current_en, "\"Hold on!\"");
    EXPECT_EQ(reparsed.value().entries[3].current_en, "\"Stop right there!\"");

    Improvement bad_index = imp;
    bad_index.entry_index = 99U;
    const Result<std::string> out_of_range =
        ScriptRepository::apply_improvements_memory(raw_crlf_4space, {bad_index});
    EXPECT_FALSE(out_of_range.is_ok());
    EXPECT_EQ(out_of_range.error_code(), ErrorCode::InvalidArgument);
}

TEST(ScriptRepository, DiscoversMultipleCachesAndRollsBackPreImagesOnWriteOrCryptFailure) {
    InMemoryFileStore store;
    ProgrammableCryptSync crypt;
    const std::filesystem::path game_root("/virtual/SampleVN");
    const std::filesystem::path script_out = game_root / "script_output";
    const std::filesystem::path translated_path = script_out / "translated_text.json";
    const std::filesystem::path cache_a_path =
        script_out / "translation_cache_dialogue.json";
    const std::filesystem::path cache_b_path =
        script_out / "translation_cache_extra.json";
    const std::filesystem::path unrelated_path = script_out / "notes.json";

    const std::string initial_translated =
        "{\n"
        "  \"01.ks\": {\n"
        "    \"strings\": [\n"
        "      {\n"
        "        \"type\": \"DIALOGUE\",\n"
        "        \"text\": \"<12>\\\\「待って！」\",\n"
        "        \"translated\": \"\\\"Wait up!\\\"\"\n"
        "      }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    const std::string initial_cache_a =
        "{\r\n"
        "    \"ん\": \"N\",\r\n"
        "    \"<12>\\\\「待って！」\": \"\\\"Wait up!\\\"\",\r\n"
        "    \"あ\": \"A\"\r\n"
        "}\r\n";
    const std::string initial_cache_b =
        "{\n"
        "  \"「待って！」\": \"\\\"Wait up!\\\"\",\n"
        "  \"別キー\": \"\\\"Wait up!\\\"\",\n"
        "  \"既存別訳\": \"\\\"Different\\\"\"\n"
        "}\n";
    const std::string initial_unrelated = "{\"keep\": \"untouched\"}\n";

    store.put_file(game_root / "TONE_OF_NOVEL.md",
                   "<name_reference>\n| 美咲 | Misaki |\n</name_reference>\n");
    store.put_file(cache_a_path, initial_cache_a);
    store.put_file(cache_b_path, initial_cache_b);
    store.put_file(unrelated_path, initial_unrelated);

    crypt.on_decrypt = [&](const std::filesystem::path& root) -> Status {
        store.put_file(root / "script_output" / "translated_text.json",
                       initial_translated);
        return Status::ok();
    };

    ScriptRepository repo(store, crypt);
    const Result<LoadedCorpus> loaded = repo.load_from_game_dir(game_root);
    ASSERT_TRUE(loaded.is_ok()) << loaded.error_message();
    EXPECT_EQ(crypt.decrypt_calls, 1U);
    EXPECT_EQ(loaded.value().glossary.resolve_speaker("美咲"), "Misaki");
    ASSERT_EQ(loaded.value().entries.size(), 1U);

    Improvement imp;
    imp.script_name = "01.ks";
    imp.entry_index = 0U;
    imp.jp_text = "<12>\\「待って！」";
    imp.old_en = "\"Wait up!\"";
    imp.improved_en = "\"Hold on!\"";

    // 1. Mid-batch IFileStore write failure on the 3rd atomic write rolls back
    // the already-overwritten earlier files to their exact byte-for-byte pre-image.
    store.fail_on_write_number(3U);
    const Result<SyncCounts> write_fail =
        repo.apply_improvements_to_artifacts(game_root, {imp});
    EXPECT_FALSE(write_fail.is_ok());
    EXPECT_EQ(write_fail.error_code(), ErrorCode::IoError);
    EXPECT_EQ(crypt.encrypt_calls, 0U);
    EXPECT_EQ(store.raw_file(translated_path), initial_translated);
    EXPECT_EQ(store.raw_file(cache_a_path), initial_cache_a);
    EXPECT_EQ(store.raw_file(cache_b_path), initial_cache_b);
    EXPECT_EQ(store.raw_file(unrelated_path), initial_unrelated);

    // 2. Encryption sync failure after all JSON writes also rolls back all plain
    // JSON artifacts so plain files never drift ahead of tracked .json.enc files.
    store.clear_faults();
    crypt.fail_encrypt = true;
    const Result<SyncCounts> crypt_fail =
        repo.apply_improvements_to_artifacts(game_root, {imp});
    EXPECT_FALSE(crypt_fail.is_ok());
    EXPECT_EQ(crypt_fail.error_code(), ErrorCode::CryptSyncFailed);
    EXPECT_EQ(crypt.encrypt_calls, 1U);
    EXPECT_EQ(store.raw_file(translated_path), initial_translated);
    EXPECT_EQ(store.raw_file(cache_a_path), initial_cache_a);
    EXPECT_EQ(store.raw_file(cache_b_path), initial_cache_b);

    // 3. Subsequent clean invocation succeeds and preserves per-file key order & style.
    crypt.fail_encrypt = false;
    const Result<SyncCounts> ok_sync =
        repo.apply_improvements_to_artifacts(game_root, {imp});
    ASSERT_TRUE(ok_sync.is_ok()) << ok_sync.error_message();
    EXPECT_EQ(ok_sync.value().script_entries_updated, 1U);
    EXPECT_EQ(ok_sync.value().cache_entries_updated, 2U);
    EXPECT_EQ(crypt.encrypt_calls, 2U);
    EXPECT_EQ(store.raw_file(unrelated_path), initial_unrelated);

    const std::string updated_cache_a = store.raw_file(cache_a_path);
    const JsonFormatStyle style_a = detect_json_format_style(updated_cache_a);
    EXPECT_EQ(style_a.indent_spaces, 4);
    EXPECT_TRUE(style_a.use_crlf);
    EXPECT_TRUE(style_a.trailing_newline);

    const Result<OrderedStringMap> parsed_a =
        parse_ordered_json_string_map(updated_cache_a, "cache_a");
    ASSERT_TRUE(parsed_a.is_ok()) << parsed_a.error_message();
    const std::vector<std::string> expected_a_keys{
        "ん", "<12>\\「待って！」", "あ"};
    ASSERT_EQ(parsed_a.value().keys().size(), expected_a_keys.size());
    EXPECT_EQ(parsed_a.value().keys(), expected_a_keys);
    ASSERT_NE(parsed_a.value().find("<12>\\「待って！」"), nullptr);
    EXPECT_EQ(*parsed_a.value().find("<12>\\「待って！」"), "\"Hold on!\"");

    const Result<OrderedStringMap> parsed_b =
        parse_ordered_json_string_map(store.raw_file(cache_b_path), "cache_b");
    ASSERT_TRUE(parsed_b.is_ok()) << parsed_b.error_message();
    ASSERT_NE(parsed_b.value().find("「待って！」"), nullptr);
    EXPECT_EQ(*parsed_b.value().find("「待って！」"), "\"Hold on!\"");
    ASSERT_NE(parsed_b.value().find("別キー"), nullptr);
    EXPECT_EQ(*parsed_b.value().find("別キー"), "\"Wait up!\"");
}

}  // namespace
}  // namespace spot_checker
