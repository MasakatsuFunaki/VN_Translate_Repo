// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Producer-to-consumer component test: synthetic .spt extraction ->
// translatable selection -> build_tsv::run_build CP932 TSV generation ->
// proxy_dll translator_logic::ParseTsvBuffer + LookupTranslation (including
// CP932 0x5C trail bytes, host % / #color prefixes, input hygiene, idempotence,
// and malformed-input fault isolation).
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "spt/spt_script.h"
#include "translator_logic.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;

namespace {

constexpr std::size_t kHeaderSize = 0x80;
constexpr std::uint8_t kSpaceByte = 0x20;
constexpr std::size_t kMagicOffset = 0x30;
constexpr const char* kMagicText = "SPTHEADER0";
constexpr std::size_t kMagicLen = 10;
constexpr std::size_t kCountOffset = 0x18;
constexpr unsigned int kShift8 = 8;
constexpr unsigned int kShift16 = 16;
constexpr unsigned int kShift24 = 24;
constexpr std::size_t kAlignment = 4;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

Bytes BuildEncryptedSpt(const std::vector<std::string>& utf8_strings) {
    Bytes dec(kHeaderSize, kSpaceByte);
    std::memcpy(dec.data() + kMagicOffset, kMagicText, kMagicLen);
    const auto count = static_cast<std::uint32_t>(utf8_strings.size());
    dec[kCountOffset + 0] = static_cast<std::uint8_t>(count);
    dec[kCountOffset + 1] = static_cast<std::uint8_t>(count >> kShift8);
    dec[kCountOffset + 2] = static_cast<std::uint8_t>(count >> kShift16);
    dec[kCountOffset + 3] = static_cast<std::uint8_t>(count >> kShift24);

    for (const std::string& text : utf8_strings) {
        const auto enc = utf8_to_cp932_strict(text);
        if (!enc) {
            ADD_FAILURE() << "Test fixture string is not representable in CP932: " << text;
            return {};
        }
        dec.insert(dec.end(), enc->begin(), enc->end());
        dec.push_back(0);
        while (dec.size() % kAlignment != 0) {
            dec.push_back(0);
        }
    }
    return spt::decrypt(dec);
}

std::string EncodeCp932String(const std::string& utf8) {
    const auto enc = utf8_to_cp932_strict(utf8);
    if (!enc) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(enc->data()), enc->size());
}

}  // namespace

TEST(TsvRoundTripComponent, ExtractBuildTsvAndProxyLookupPreserveAllEntriesAndHostPrefixes) {
    const ScopedTempDir tmp("anzu_ce_ct_tsv_roundtrip");
    const fs::path spt_dir = tmp.path() / "spt";
    fs::create_directories(spt_dir);

    struct TranslatableFixture {
        std::string jp;
        std::string en_raw;
        std::string expected_runtime_en;
    };
    const std::vector<TranslatableFixture> translatable = {
        {"俺は、限界までスピードを上げて夢美を突き続け、\r\nそして――――\r\n",
         "I pushed my speed to the limit, thrusting into Yumemi, and\r\nthen--\r\n",
         "I pushed my speed to the limit, thrusting into Yumemi, and\r\nthen--\r\n"},
        {"だが――――\r\n",
         "However--\r\n",
         "However--\r\n"},
        {"――と、\r\n",
         "--Wait,\r\n",
         "--Wait,\r\n"},
        {"高まる官能。\r\n",
         "Rising ecstasy.\r\n",
         "Rising ecstasy.\r\n"},
        {"夢美を呼ぶ",
         "Call Yumemi",
         "Call Yumemi"},
        {"アゲハを呼ぶ",
         "Call Ageha",
         "Call Ageha"},
        {"その表rと能tとソ\r\n",
         "Check table r, skill t, and so.\r\n",
         "Check table r, skill t, and so.\r\n"},
        {"十\\の印ソ\r\n",
         "Ten\\ mark.\r\n",
         "Ten\\ mark.\r\n"},
        {"夢美\r\n「いらっしゃいませ」",
         "Yumemi\r\n\"Welcome.\"",
         "Yumemi: \"Welcome.\""},
    };
    const std::vector<std::string> non_translatable = {
        "夢美",
        "アゲハ",
        "タイトル",
        "bgm_01",
    };

    std::vector<std::string> all_spt_strings;
    for (const auto& item : translatable) {
        all_spt_strings.push_back(item.jp);
    }
    for (const auto& skip : non_translatable) {
        all_spt_strings.push_back(skip);
    }

    const Bytes spt_bytes = BuildEncryptedSpt(all_spt_strings);
    ASSERT_FALSE(spt_bytes.empty());
    const fs::path spt_file = spt_dir / "tokuten.spt";
    write_file(spt_file.u8string(), spt_bytes);

    // Stage 1: Real SPT extraction facade (read-only input must remain untouched).
    const fs::path extracted_json = tmp.path() / "extracted_text.json";
    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), extracted_json.u8string()), 0);
    EXPECT_EQ(read_file(spt_file.u8string()), spt_bytes);

    bj::value extracted_val = json_parse_file(extracted_json.u8string());
    ASSERT_TRUE(extracted_val.is_object());
    bj::object& tokuten_obj = extracted_val.as_object().at("tokuten.spt").as_object();

    // Stage 2: Translatable selection must include every fixture and exclude bare tags.
    const std::vector<std::size_t> indices = translate::translatable_indices(tokuten_obj);
    ASSERT_EQ(indices.size(), translatable.size());

    std::map<std::string, std::string> jp_to_en;
    for (const auto& item : translatable) {
        jp_to_en[item.jp] = item.en_raw;
    }

    bj::array& strings_arr = tokuten_obj.at("strings").as_array();
    std::set<std::string> selected_jp;
    for (std::size_t idx : indices) {
        bj::object& entry = strings_arr.at(idx).as_object();
        const std::string jp(entry.at("text").as_string());
        selected_jp.insert(jp);
        const auto it = jp_to_en.find(jp);
        ASSERT_NE(it, jp_to_en.end()) << "Unexpected translatable string: " << jp;
        entry["translated"] = it->second;
    }
    for (const std::string& skip : non_translatable) {
        EXPECT_EQ(selected_jp.count(skip), 0u) << "Non-translatable tag leaked: " << skip;
    }

    const fs::path translated_json = tmp.path() / "translated_text.json";
    write_file_text(translated_json.u8string(), json_pretty(extracted_val));

    // Stage 3: Real CP932 TSV builder facade (including idempotence verification).
    const fs::path tsv_path = tmp.path() / "translation_table.tsv";
    ASSERT_EQ(build_tsv::run_build(translated_json.u8string(), tsv_path.u8string()), 0);
    const Bytes first_tsv_bytes = read_file(tsv_path.u8string());
    ASSERT_FALSE(first_tsv_bytes.empty());

    ASSERT_EQ(build_tsv::run_build(translated_json.u8string(), tsv_path.u8string()), 0);
    EXPECT_EQ(read_file(tsv_path.u8string()), first_tsv_bytes);

    // Stage 4: Real proxy_dll TSV consumer & runtime lookup.
    translator_logic::TranslationMap runtime_map;
    const int loaded = translator_logic::ParseTsvBuffer(
        reinterpret_cast<const char*>(first_tsv_bytes.data()),
        first_tsv_bytes.size(),
        runtime_map);
    EXPECT_EQ(loaded, static_cast<int>(translatable.size()));

    for (const auto& item : translatable) {
        const std::string cp932_key = EncodeCp932String(item.jp);
        ASSERT_FALSE(cp932_key.empty()) << "CP932 encode failed for: " << item.jp;

        std::string resolved;
        EXPECT_TRUE(translator_logic::LookupTranslation(
            runtime_map, cp932_key.c_str(), false, resolved))
            << "Runtime lookup missed for: " << item.jp;
        EXPECT_EQ(resolved, item.expected_runtime_en) << "Mismatch for: " << item.jp;
    }

    // Verify BLACKCyc host wire transformations: single-line flattening and #color prefixing.
    std::string flattened;
    const std::string multiline_key = EncodeCp932String(translatable[0].jp);
    ASSERT_TRUE(translator_logic::LookupTranslation(
        runtime_map, multiline_key.c_str(), true, flattened));
    EXPECT_EQ(flattened, "I pushed my speed to the limit, thrusting into Yumemi, and then--");

    std::string colored_choice;
    const std::string host_colored_key = EncodeCp932String("#黄夢美を呼ぶ");
    ASSERT_TRUE(translator_logic::LookupTranslation(
        runtime_map, host_colored_key.c_str(), true, colored_choice));
    EXPECT_EQ(colored_choice, EncodeCp932String("#黄") + "Call Yumemi");

    // Excluded bare tags must not resolve in the runtime map.
    for (const std::string& skip : non_translatable) {
        const std::string cp932_key = EncodeCp932String(skip);
        std::string resolved;
        EXPECT_FALSE(translator_logic::LookupTranslation(
            runtime_map, cp932_key.c_str(), false, resolved))
            << "Non-translatable tag unexpectedly present in runtime TSV: " << skip;
    }
}

TEST(TsvRoundTripComponent, MalformedTranslatedJsonFailsCleanlyWithoutHalfWrittenTsv) {
    const ScopedTempDir tmp("anzu_ce_ct_tsv_fault");
    const fs::path bad_json = tmp.path() / "corrupt_translated.json";
    const fs::path out_tsv = tmp.path() / "must_not_exist.tsv";

    write_file_text(bad_json.u8string(), "{ \"tokuten.spt\": [ truncated");
    EXPECT_THROW(build_tsv::run_build(bad_json.u8string(), out_tsv.u8string()), std::exception);
    EXPECT_FALSE(fs::exists(out_tsv));
}
