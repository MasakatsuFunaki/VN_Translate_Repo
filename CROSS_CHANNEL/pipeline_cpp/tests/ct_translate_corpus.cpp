// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box multi-component pipeline tests driving BatchTranslator,
// translate_all, and build_tsv::run_build end-to-end across a realistic corpus
// slice with zero internal mocks (faking only ITranslationTransport at HTTPS).
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bj = boost::json;
namespace bt = crc::build_tsv;
namespace fs = std::filesystem;
namespace tr = crc::translate;

namespace {

constexpr int kExtractedJsonIndent = 1;
constexpr int kBuildSuccessExitCode = 0;
constexpr std::int64_t kOffsetStride = 128;
constexpr std::int64_t kPromptTokensPerBatch = 320;
constexpr std::int64_t kOutputTokensPerBatch = 90;
constexpr std::int64_t kThoughtTokensPerBatch = 45;
constexpr double kBudgetCeilingEur = 50.0;
constexpr std::size_t kExpectedGlossarySize = 75u;
constexpr std::size_t kSingleBlockedCount = 1u;
constexpr std::uint8_t kCarriageReturnByte = 0x0D;

constexpr const char* kSharedCoverageJp = "「了解。」";
constexpr const char* kSharedCoverageEn = "\"Understood.\"";
constexpr const char* kBlockedHLineJp = "「やめて……そんなに奥までっ！」";
constexpr const char* kRecoveredHLineEn = "\"Stop... not that deep!\"";

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name =
            "crc_ct_translate_日本語_" + std::to_string(++seq);
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

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

bj::object make_entry(std::int64_t offset, std::string_view type,
                      std::string_view speaker, std::string_view text) {
    bj::object o;
    o["offset"] = offset;
    o["type"] = type;
    o["speaker"] = speaker;
    o["text"] = text;
    return o;
}

std::string_view json_sv(const bj::value& v) {
    const bj::string& s = v.as_string();
    return std::string_view(s.data(), s.size());
}

std::int64_t json_i64(const bj::value& v) {
    if (v.is_int64()) {
        return v.as_int64();
    }
    return static_cast<std::int64_t>(v.as_uint64());
}

bool has_smart_punctuation(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const char32_t cp = crc::utf8_next(s, i);
        if (cp == U'—' || cp == U'–' || cp == U'…' || cp == U'‘' ||
            cp == U'’' || cp == U'“' || cp == U'”' || cp == U'　') {
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

std::string unescape_tsv_field(std::string_view escaped) {
    std::string out;
    out.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '\\' && i + 1u < escaped.size()) {
            const char next = escaped[++i];
            if (next == 't') {
                out.push_back('\t');
                continue;
            }
            if (next == 'r') {
                out.push_back('\r');
                continue;
            }
            if (next == 'n') {
                out.push_back('\n');
                continue;
            }
            if (next == '\\') {
                out.push_back('\\');
                continue;
            }
            out.push_back('\\');
            out.push_back(next);
            continue;
        }
        out.push_back(escaped[i]);
    }
    return out;
}

std::vector<std::string> parse_requested_escaped_jps(
    std::string_view user_prompt,
    const std::unordered_map<std::string, std::string>& escaped_jp_to_en) {
    std::string_view section = user_prompt;
    constexpr std::string_view kOpenTag = "<lines_to_translate>";
    constexpr std::string_view kCloseTag = "</lines_to_translate>";
    constexpr std::string_view kAfterTag = "<context_after>";

    const std::size_t open_pos = section.find(kOpenTag);
    if (open_pos != std::string_view::npos) {
        section.remove_prefix(open_pos + kOpenTag.size());
    }
    const std::size_t close_pos = section.find(kCloseTag);
    if (close_pos != std::string_view::npos) {
        section = section.substr(0, close_pos);
    } else {
        const std::size_t after_pos = section.find(kAfterTag);
        if (after_pos != std::string_view::npos) {
            section = section.substr(0, after_pos);
        }
    }

    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos < section.size()) {
        const std::size_t nl = section.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
                                    ? section.substr(pos)
                                    : section.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? section.size() : (nl + 1u);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1u);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1u);
        }
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line.front()))) {
            continue;
        }
        std::size_t i = 0;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= line.size() || (line[i] != '.' && line[i] != ')' && line[i] != ':')) {
            continue;
        }
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        std::string_view payload = line.substr(i);
        if (escaped_jp_to_en.count(std::string(payload)) == 0 &&
            !payload.empty() && payload.front() == '[') {
            const std::size_t tag_end = payload.find("] ");
            if (tag_end != std::string_view::npos) {
                payload.remove_prefix(tag_end + 2u);
            }
        }
        result.emplace_back(payload);
    }
    return result;
}

class FakeHttpsTransport final : public tr::ITranslationTransport {
public:
    void set_mapping(const std::string& jp, const std::string& raw_en) {
        escaped_jp_to_en_[tr::escape_prompt_line(jp)] = tr::escape_prompt_line(raw_en);
    }

    void block_jp(const std::string& jp) {
        blocked_escaped_jps_.insert(tr::escape_prompt_line(jp));
    }

    void unblock_jp(const std::string& jp) {
        blocked_escaped_jps_.erase(tr::escape_prompt_line(jp));
    }

    tr::GeminiResponse generate(const tr::GeminiRequest& request) override {
        ++calls_;
        const std::vector<std::string> jps =
            parse_requested_escaped_jps(request.user_prompt, escaped_jp_to_en_);
        last_requested_count_ = jps.size();

        for (const std::string& jp : jps) {
            if (blocked_escaped_jps_.count(jp) != 0) {
                tr::GeminiResponse blocked;
                blocked.content_blocked = true;
                blocked.finish_reason = "PROHIBITED_CONTENT";
                blocked.prompt_tokens = kPromptTokensPerBatch;
                return blocked;
            }
        }

        std::string text;
        for (std::size_t i = 0; i < jps.size(); ++i) {
            const auto it = escaped_jp_to_en_.find(jps[i]);
            const std::string en =
                (it != escaped_jp_to_en_.end()) ? it->second : "\"Fallback.\"";
            text += std::to_string(i + 1u) + ". " + en + "\n";
        }

        tr::GeminiResponse ok;
        ok.text = std::move(text);
        ok.finish_reason = "STOP";
        ok.prompt_tokens = kPromptTokensPerBatch;
        ok.output_tokens = kOutputTokensPerBatch;
        ok.thought_tokens = kThoughtTokensPerBatch;
        return ok;
    }

    std::size_t calls() const noexcept { return calls_; }
    std::size_t last_requested_count() const noexcept { return last_requested_count_; }

private:
    std::unordered_map<std::string, std::string> escaped_jp_to_en_;
    std::unordered_set<std::string> blocked_escaped_jps_;
    std::size_t calls_ = 0;
    std::size_t last_requested_count_ = 0;
};

const bj::array& extract_blocked_array(const bj::value& root) {
    if (root.is_array()) {
        return root.as_array();
    }
    for (const auto& kv : root.as_object()) {
        if (kv.value().is_array()) {
            return kv.value().as_array();
        }
    }
    static const bj::array kEmpty;
    return kEmpty;
}

bool has_any_tmp_files(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().extension() == ".tmp") {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(TranslatePipelineCt, EndToEndBatchTranslateBisectionRecoveryOfflineBuildAndTsvConsumer) {
    ScopedTempDir sandbox;
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path script_out = project_dir / "script_output";
    const fs::path game_dir = sandbox.path() / "game_install";
    fs::create_directories(script_out);
    fs::create_directories(game_dir);

    const fs::path extracted_path = script_out / "extracted_text.json";
    const fs::path cache_path = script_out / "translation_cache_gemini.json";
    const fs::path blocked_path = script_out / "blocked_lines.json";
    const fs::path raw_log_path = script_out / "gemini_raw_answers.log";
    const fs::path last_batch_path =
        script_out / "last_gemini_translate" / "last_translation.json";
    const fs::path translated_path = script_out / "translated_text.json";
    const fs::path staged_tsv_path =
        project_dir / "build" / "install" / "game" / "translations.tsv";
    const fs::path deployed_tsv_path = game_dir / "translations.tsv";

    bj::array strings;
    std::int64_t next_offset = static_cast<std::int64_t>(crc::extract::GAME_START_OFFSET);
    for (const auto& [jp_speaker, en_speaker] : tr::name_translations_ordered()) {
        (void)en_speaker;
        strings.push_back(make_entry(next_offset, "dialogue", jp_speaker, kSharedCoverageJp));
        next_offset += kOffsetStride;
    }

    const std::int64_t first_dup_offset = next_offset;
    strings.push_back(make_entry(next_offset, "dialogue", "太一", "「今、何時だ？」"));
    next_offset += kOffsetStride;
    strings.push_back(make_entry(next_offset, "narration", "", "校舎の廊下に人影はない。"));
    next_offset += kOffsetStride;
    strings.push_back(make_entry(next_offset, "dialogue", "美希",
                                 "「ここは一階よ。%N屋上に行きましょう%N先輩」"));
    next_offset += kOffsetStride;
    strings.push_back(make_entry(next_offset, "dialogue", "",
                                 "太一\r\n「聞こえるか？\r\n応答しろ」\r\n"));
    next_offset += kOffsetStride;

    const std::int64_t blocked_offset = next_offset;
    strings.push_back(make_entry(next_offset, "dialogue", "冬子", kBlockedHLineJp));
    next_offset += kOffsetStride;
    strings.push_back(make_entry(next_offset, "narration", "", "帰る"));
    next_offset += kOffsetStride;
    strings.push_back(make_entry(next_offset, "narration", "", "追う"));
    next_offset += kOffsetStride;

    const std::int64_t second_dup_offset = next_offset;
    strings.push_back(make_entry(next_offset, "dialogue", "太一", "「今、何時だ？」"));
    next_offset += kOffsetStride;
    strings.push_back(make_entry(next_offset, "narration", "", "……"));
    next_offset += kOffsetStride;
    strings.push_back(make_entry(next_offset, "narration", "", "CASE 01"));

    const std::size_t expected_total_entries = strings.size();
    bj::object extracted_root;
    extracted_root["sn.bin"] = bj::object{{"strings", std::move(strings)}};
    crc::write_file_text(extracted_path.u8string(),
                         crc::json_pretty(bj::value(extracted_root), kExtractedJsonIndent));
    const crc::Bytes extracted_bytes_initial = crc::read_file(extracted_path.u8string());

    FakeHttpsTransport transport;
    transport.set_mapping(kSharedCoverageJp, kSharedCoverageEn);
    transport.set_mapping("「今、何時だ？」", "“What time—is it?…”");
    transport.set_mapping("校舎の廊下に人影はない。", "No one in\\the\thallway.");
    transport.set_mapping("「ここは一階よ。%N屋上に行きましょう%N先輩」",
                          "\"This is the first floor.%NLet's head to the roof,%Nsenpai.\"");
    transport.set_mapping("太一\r\n「聞こえるか？\r\n応答しろ」\r\n",
                          "[Taichi] \"Can you hear me?\\r\\nAnswer me.\"  ");
    transport.set_mapping(kBlockedHLineJp, kRecoveredHLineEn);
    transport.set_mapping("帰る", "Go home");
    transport.set_mapping("追う", "Chase her");
    transport.block_jp(kBlockedHLineJp);

    tr::GeminiTranslateOptions live_opt;
    live_opt.input_file = extracted_path.u8string();
    live_opt.cache_file = cache_path.u8string();
    live_opt.output_file = translated_path.u8string();
    live_opt.tsv_file = staged_tsv_path.u8string();
    live_opt.game_tsv_file = deployed_tsv_path.u8string();
    live_opt.blocked_file = blocked_path.u8string();
    live_opt.raw_log_file = raw_log_path.u8string();
    live_opt.last_batch_file = last_batch_path.u8string();
    live_opt.batch_size = 4;
    live_opt.context_window = 3;
    live_opt.context_after_window = 2;
    live_opt.max_budget_eur = kBudgetCeilingEur;

    // Pass 1: 7 innocent unique strings succeed across batches + bisection;
    // the 1 blocked H-line is isolated in blocked_lines.json and omitted from cache.
    tr::BatchTranslator translator(&transport);
    const tr::TranslateReport pass1 = translator.run(live_opt);
    EXPECT_EQ(pass1.blocked_count, kSingleBlockedCount);
    EXPECT_EQ(pass1.translated_new, 7u);
    EXPECT_EQ(pass1.remaining_uncached, kSingleBlockedCount);

    // Side-effect hygiene: input corpus untouched and zero leftover .tmp files.
    EXPECT_EQ(crc::read_file(extracted_path.u8string()), extracted_bytes_initial);
    EXPECT_FALSE(has_any_tmp_files(sandbox.path()));
    EXPECT_TRUE(fs::exists(last_batch_path));
    EXPECT_TRUE(fs::exists(raw_log_path));
    (void)crc::json_parse_file(last_batch_path.u8string());

    const tr::Cache cache_after_pass1 = tr::load_cache(cache_path.u8string());
    EXPECT_FALSE(cache_after_pass1.contains(kBlockedHLineJp));
    EXPECT_EQ(cache_after_pass1.size(), kExpectedGlossarySize + 7u);

    ASSERT_TRUE(fs::exists(blocked_path));
    const bj::value blocked_json = crc::json_parse_file(blocked_path.u8string());
    const bj::array& blocked_arr = extract_blocked_array(blocked_json);
    ASSERT_EQ(blocked_arr.size(), kSingleBlockedCount);
    EXPECT_EQ(json_i64(blocked_arr[0].as_object().at("offset")), blocked_offset);

    // Offline builder must refuse to emit incomplete output while 1 line is uncached.
    tr::TranslateOptions offline_opt;
    offline_opt.input_file = extracted_path.u8string();
    offline_opt.cache_file = cache_path.u8string();
    offline_opt.output_file = translated_path.u8string();
    offline_opt.tsv_file = staged_tsv_path.u8string();
    offline_opt.game_tsv_file = deployed_tsv_path.u8string();
    EXPECT_THROW((void)tr::translate_all(offline_opt), std::runtime_error);

    // Pass 2 (resume after unblocking): sends ONLY the 1 remaining uncached line.
    transport.unblock_jp(kBlockedHLineJp);
    const std::size_t calls_before_pass2 = transport.calls();
    const tr::TranslateReport pass2 = translator.run(live_opt);
    EXPECT_EQ(pass2.exit_code, tr::kExitOk);
    EXPECT_EQ(pass2.translated_new, kSingleBlockedCount);
    EXPECT_EQ(pass2.remaining_uncached, 0u);
    EXPECT_EQ(pass2.blocked_count, 0u);
    EXPECT_EQ(transport.calls() - calls_before_pass2, 1u);
    EXPECT_EQ(transport.last_requested_count(), 1u);

    // Pass 3 (full-cache replay): 100% idempotent with 0 transport calls.
    const crc::Bytes cache_bytes_after_pass2 = crc::read_file(cache_path.u8string());
    const std::size_t calls_before_pass3 = transport.calls();
    const tr::TranslateReport pass3 = translator.run(live_opt);
    EXPECT_EQ(pass3.exit_code, tr::kExitOk);
    EXPECT_EQ(pass3.translated_new, 0u);
    EXPECT_EQ(pass3.remaining_uncached, 0u);
    EXPECT_EQ(transport.calls(), calls_before_pass3);
    EXPECT_EQ(crc::read_file(cache_path.u8string()), cache_bytes_after_pass2);

    // Producer -> Consumer chain: offline translate_all -> translated_text.json ->
    // build_tsv::run_build -> staged & deployed translations.tsv.
    ASSERT_EQ(tr::translate_all(offline_opt), tr::kExitOk);
    ASSERT_EQ(bt::run_build(translated_path.u8string(), staged_tsv_path.u8string(),
                            deployed_tsv_path.u8string()),
              kBuildSuccessExitCode);

    const bj::array translated_strings = crc::json_parse_file(translated_path.u8string())
                                             .as_object()
                                             .at("sn.bin")
                                             .as_object()
                                             .at("strings")
                                             .as_array();
    ASSERT_EQ(translated_strings.size(), expected_total_entries);

    std::map<std::int64_t, std::string> by_offset;
    for (const bj::value& item : translated_strings) {
        const bj::object& obj = item.as_object();
        const std::string jp(json_sv(obj.at("text")));
        const std::string en(json_sv(obj.at("translated")));
        ASSERT_TRUE(crc::utf8_to_cp932_strict(jp).has_value()) << jp;
        if (en != jp) {
            EXPECT_FALSE(has_smart_punctuation(en)) << en;
            EXPECT_FALSE(tr::has_real_japanese(en)) << en;
        }
        if (jp.find("%N") != std::string::npos) {
            EXPECT_EQ(count_percent_n(en), count_percent_n(jp)) << en;
        }
        by_offset.emplace(json_i64(obj.at("offset")), en);
    }

    EXPECT_EQ(by_offset.at(first_dup_offset), "\"What time--is it?...\"");
    EXPECT_EQ(by_offset.at(second_dup_offset), "\"What time--is it?...\"");
    EXPECT_EQ(by_offset.at(blocked_offset), kRecoveredHLineEn);

    // Consumer format verification on translations.tsv and deployed copy.
    const crc::Bytes raw_tsv = crc::read_file(staged_tsv_path.u8string());
    const crc::Bytes deployed_tsv = crc::read_file(deployed_tsv_path.u8string());
    EXPECT_EQ(raw_tsv, deployed_tsv);
    ASSERT_FALSE(raw_tsv.empty());
    for (const std::uint8_t b : raw_tsv) {
        EXPECT_NE(b, kCarriageReturnByte);
    }

    const std::string tsv_text(reinterpret_cast<const char*>(raw_tsv.data()), raw_tsv.size());
    std::map<std::string, std::string> decoded_tsv;
    std::size_t row_count = 0;
    std::size_t pos = 0;
    while (pos < tsv_text.size()) {
        const std::size_t nl = tsv_text.find('\n', pos);
        ASSERT_NE(nl, std::string::npos);
        const std::string_view line(tsv_text.data() + pos, nl - pos);
        pos = nl + 1u;
        ++row_count;

        const std::size_t tab = line.find('\t');
        ASSERT_NE(tab, std::string_view::npos) << line;
        EXPECT_EQ(line.find('\t', tab + 1u), std::string_view::npos) << line;

        const std::string jp = unescape_tsv_field(line.substr(0, tab));
        const std::string en = unescape_tsv_field(line.substr(tab + 1u));
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(en.empty());
        EXPECT_NE(jp, en);
        EXPECT_TRUE(crc::utf8_to_cp932_strict(jp).has_value()) << jp;
        EXPECT_FALSE(has_smart_punctuation(en)) << en;
        EXPECT_FALSE(tr::has_real_japanese(en)) << en;
        EXPECT_TRUE(decoded_tsv.emplace(jp, en).second) << "Duplicate TSV key: " << jp;
    }

    // 8 unique translated story strings + 75 glossary entries = 83 deduplicated rows.
    EXPECT_EQ(row_count, kExpectedGlossarySize + 8u);
    EXPECT_EQ(decoded_tsv.at("「今、何時だ？」"), "\"What time--is it?...\"");
    EXPECT_EQ(decoded_tsv.at("校舎の廊下に人影はない。"), "No one in\\the\thallway.");
    EXPECT_EQ(decoded_tsv.at("「ここは一階よ。%N屋上に行きましょう%N先輩」"),
              "\"This is the first floor.%NLet's head to the roof,%Nsenpai.\"");
    EXPECT_EQ(decoded_tsv.at("太一\r\n「聞こえるか？\r\n応答しろ」\r\n"),
              "Taichi\r\n\"Can you hear me?\r\nAnswer me.\"\r\n");
    EXPECT_EQ(decoded_tsv.at(kBlockedHLineJp), kRecoveredHLineEn);
    EXPECT_EQ(decoded_tsv.at("帰る"), "Go home");
    EXPECT_EQ(decoded_tsv.at("追う"), "Chase her");
    EXPECT_EQ(decoded_tsv.count("……"), 0u);
    EXPECT_EQ(decoded_tsv.count("CASE 01"), 0u);
    for (const auto& [jp_name, en_name] : tr::name_translations_ordered()) {
        ASSERT_NE(decoded_tsv.find(jp_name), decoded_tsv.end()) << jp_name;
        EXPECT_EQ(decoded_tsv.at(jp_name), en_name);
    }

    EXPECT_FALSE(has_any_tmp_files(sandbox.path()));
}
