// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for Adieu string classification, speaker-plate rules, contextual
// `idx - 1` speaker attribution (`＊Ａ` -> `人見`, internal U+3000 preservation),
// and end-to-end extraction over synthetic Scene.pck and `.ss.txt` inputs.
#include "extract/extractor.h"
#include "siglus/scene_pck.h"
#include "siglus/siglus_decoder.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace adi;
using namespace adi::extract;

namespace {

constexpr std::uint8_t kByteMask = 0xFFU;
constexpr unsigned kShift8 = 8U;
constexpr unsigned kShift16 = 16U;
constexpr unsigned kShift24 = 24U;
constexpr std::size_t kByte1 = 1;
constexpr std::size_t kByte2 = 2;
constexpr std::size_t kByte3 = 3;
constexpr char kBomByte0 = static_cast<char>(0xFFU);
constexpr char kBomByte1 = static_cast<char>(0xFEU);

constexpr std::size_t kCaptureEntryCount = 7;
constexpr std::size_t kIdx0 = 0;
constexpr std::size_t kIdx1 = 1;
constexpr std::size_t kIdx2 = 2;
constexpr std::size_t kIdx3 = 3;
constexpr std::size_t kIdx4 = 4;
constexpr std::size_t kIdx5 = 5;
constexpr std::size_t kIdx6 = 6;
constexpr std::size_t kUniversalNewlineCount = 3;
constexpr std::size_t kDumpSceneCount = 2;
constexpr std::int64_t kBetaSiglusIdx = 10;
constexpr std::size_t kBetaStringCount = 2;
constexpr std::int64_t kFirstPoolIdx = 0;
constexpr std::int64_t kThirdPoolIdx = 2;
constexpr std::int64_t kFourthPoolIdx = 3;

constexpr std::uint32_t kSceneHeaderV2Size = 0x1CU;
constexpr std::size_t kStrIdxOffsetField = 0x0C;
constexpr std::size_t kStrCountField = 0x10;
constexpr std::size_t kStrDataOffsetField = 0x14;
constexpr std::uint32_t kIndexPairBytes = 8U;
constexpr std::size_t kPairSecondFieldOffset = 4;
constexpr std::size_t kLzssFlagGroupSize = 8;
constexpr std::uint8_t kAllLiteralsFlag = 0xFFU;
constexpr std::size_t kChunkHeaderBytes = 8;
constexpr std::size_t kSiglusKeyMask = 0xFF;
constexpr std::size_t kPrivateKeyMask = 0x0F;
constexpr std::uint32_t kPackHeaderSize = 0x5CU;
constexpr std::size_t kSectionTableOffset = 4;
constexpr std::uint32_t kOneEntry = 1U;
constexpr std::uint32_t kKeyFlagOn = 1U;
constexpr std::uint32_t kUtf16UnitBytes = 2U;
constexpr int kExtractOk = 0;
constexpr std::size_t kSingleSceneCount = 1;
constexpr std::size_t kExpectedPckStringCount = 3;
constexpr std::size_t kExpectedContextEntryCount = 11;
constexpr std::size_t kExpectedStartSeedCount = 3;
constexpr std::size_t kZeroCount = 0;

// Hand-computed SS_V2 XOR keys `(idx * 0x7087) & 0xFFFF` for synthetic Scene.pck:
//   idx 2 -> 0xE10E
//   idx 3 -> 0x5195
constexpr std::uint16_t kSsV2KeyIdx2 = 0xE10EU;
constexpr std::uint16_t kSsV2KeyIdx3 = 0x5195U;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name =
            "adi_ut_extract_" + std::to_string(++seq);
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

    std::string write_child(const std::string& name, const std::string& content) const {
        const fs::path file = dir_ / fs::u8path(name);
        write_file(file.u8string(), content);
        return file.u8string();
    }

private:
    fs::path dir_;
};

void put32(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v & kByteMask);
    b[off + kByte1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    b[off + kByte2] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    b[off + kByte3] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

std::string utf16le(const std::vector<std::uint16_t>& u, bool bom) {
    std::string s;
    if (bom) {
        s += kBomByte0;
        s += kBomByte1;
    }
    for (std::uint16_t w : u) {
        s += static_cast<char>(w & kByteMask);
        s += static_cast<char>(w >> kShift8);
    }
    return s;
}

std::string obj_str(const bj::object& obj, const char* key) {
    return std::string(obj.at(key).get_string());
}

Bytes build_synthetic_scene_pck(const std::string& name) {
    const std::vector<std::vector<std::uint16_t>> entries = {
        {'a', 'b'},
        {},
        {static_cast<std::uint16_t>(u'ま' ^ kSsV2KeyIdx2),
         static_cast<std::uint16_t>(u'ひ' ^ kSsV2KeyIdx2),
         static_cast<std::uint16_t>(u'る' ^ kSsV2KeyIdx2)},
        {static_cast<std::uint16_t>(u'「' ^ kSsV2KeyIdx3),
         static_cast<std::uint16_t>(u'ね' ^ kSsV2KeyIdx3),
         static_cast<std::uint16_t>(u'」' ^ kSsV2KeyIdx3)},
    };
    const std::uint32_t idx_off = kSceneHeaderV2Size;
    const std::uint32_t data_off =
        idx_off + static_cast<std::uint32_t>(entries.size()) * kIndexPairBytes;
    Bytes scene(data_off, 0);
    put32(scene, kIdx0, kSceneHeaderV2Size);
    put32(scene, kStrIdxOffsetField, idx_off);
    put32(scene, kStrCountField, static_cast<std::uint32_t>(entries.size()));
    put32(scene, kStrDataOffsetField, data_off);
    std::uint32_t woff = 0;
    Bytes blob;
    for (std::size_t j = 0; j < entries.size(); ++j) {
        put32(scene, idx_off + j * kIndexPairBytes, woff);
        put32(scene, idx_off + j * kIndexPairBytes + kPairSecondFieldOffset,
              static_cast<std::uint32_t>(entries[j].size()));
        for (std::uint16_t u : entries[j]) {
            blob.push_back(static_cast<std::uint8_t>(u & kByteMask));
            blob.push_back(static_cast<std::uint8_t>(u >> kShift8));
        }
        woff += static_cast<std::uint32_t>(entries[j].size());
    }
    scene.insert(scene.end(), blob.begin(), blob.end());

    Bytes comp;
    for (std::size_t i = 0; i < scene.size(); i += kLzssFlagGroupSize) {
        comp.push_back(kAllLiteralsFlag);
        for (std::size_t k = i; k < i + kLzssFlagGroupSize && k < scene.size(); ++k) {
            comp.push_back(scene[k]);
        }
    }
    Bytes chunk(kChunkHeaderBytes, 0);
    put32(chunk, kIdx0, static_cast<std::uint32_t>(comp.size()));
    put32(chunk, kPairSecondFieldOffset, static_cast<std::uint32_t>(scene.size()));
    chunk.insert(chunk.end(), comp.begin(), comp.end());
    for (std::size_t i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<std::uint8_t>(
            chunk[i] ^ siglus::SIGLUS_KEY_256[i & kSiglusKeyMask] ^
            siglus::ADIEU_PRIVATE_KEY[i & kPrivateKeyMask]);
    }

    const std::uint32_t name_idx_off = kPackHeaderSize;
    const std::uint32_t name_blob_off = name_idx_off + kIndexPairBytes;
    const std::uint32_t data_idx_off =
        name_blob_off + static_cast<std::uint32_t>(name.size()) * kUtf16UnitBytes;
    const std::uint32_t chunk_base = data_idx_off + kIndexPairBytes;
    Bytes pck(chunk_base, 0);
    put32(pck, kIdx0, kPackHeaderSize);
    for (int i = 0; i < siglus::SECTION_COUNT; ++i) {
        put32(pck, kSectionTableOffset + static_cast<std::size_t>(i) * kIndexPairBytes,
              kPackHeaderSize);
    }
    put32(pck, kSectionTableOffset + siglus::SEC_SCRIPT_NAME_IDX * kIndexPairBytes,
          name_idx_off);
    put32(pck,
          kSectionTableOffset + siglus::SEC_SCRIPT_NAME_IDX * kIndexPairBytes +
              kPairSecondFieldOffset,
          kOneEntry);
    put32(pck, kSectionTableOffset + siglus::SEC_SCRIPT_NAME_BLOB * kIndexPairBytes,
          name_blob_off);
    put32(pck,
          kSectionTableOffset + siglus::SEC_SCRIPT_NAME_BLOB * kIndexPairBytes +
              kPairSecondFieldOffset,
          kOneEntry);
    put32(pck, kSectionTableOffset + siglus::SEC_SCRIPT_DATA_IDX * kIndexPairBytes,
          data_idx_off);
    put32(pck,
          kSectionTableOffset + siglus::SEC_SCRIPT_DATA_IDX * kIndexPairBytes +
              kPairSecondFieldOffset,
          kOneEntry);
    put32(pck, kSectionTableOffset + siglus::SEC_SCRIPT_KEY * kIndexPairBytes, chunk_base);
    put32(pck,
          kSectionTableOffset + siglus::SEC_SCRIPT_KEY * kIndexPairBytes +
              kPairSecondFieldOffset,
          kKeyFlagOn);
    put32(pck, name_idx_off, 0);
    put32(pck, name_idx_off + kPairSecondFieldOffset,
          static_cast<std::uint32_t>(name.size()));
    for (std::size_t i = 0; i < name.size(); ++i) {
        pck[name_blob_off + i * kUtf16UnitBytes] = static_cast<std::uint8_t>(name[i]);
        pck[name_blob_off + i * kUtf16UnitBytes + kByte1] = 0;
    }
    put32(pck, data_idx_off, 0);
    put32(pck, data_idx_off + kPairSecondFieldOffset,
          static_cast<std::uint32_t>(chunk.size()));
    pck.insert(pck.end(), chunk.begin(), chunk.end());
    return pck;
}

}  // namespace

TEST(Extract, ClassifyTextAndSpeakerPlateBoundaries) {
    struct Case {
        const char* input;
        const char* expected_type;
        bool expected_has_jp;
        bool expected_plate;
    };
    const Case kCases[] = {
        {"", "empty", false, false},
        {"   ", "empty", false, false},
        {"　", "empty", false, false},
        {"white", "other", false, false},
        {"＊Ａ", "other", false, false},
        {"＊Ｂ", "other", false, false},
        {"？？？", "other", false, false},
        // Genuine speaker plates, including internal U+3000 ideographic space
        // and the exact 12-codepoint upper boundary vs 13-codepoint overflow.
        {"睦　月", "name", true, true},
        {"まひる", "name", true, true},
        {"ああああああああああああ", "name", true, true},
        {"あああああああああああああ", "narrative", true, false},
        // Short prose lines starting/ending with U+3000, punctuation, parens,
        // or newlines must NEVER classify as speaker plates (`name`).
        {"　夢だ。", "narrative", true, false},
        {"　全裸で！", "narrative", true, false},
        {"ああ……。", "narrative", true, false},
        {"‥‥ぱたん", "narrative", true, false},
        {"（誰だ？）", "narrative", true, false},
        {"あ\nあ", "narrative", true, false},
        {"あ\rあ", "narrative", true, false},
        {"「ああああああ", "narrative", true, false},
        // Context-free single-string quotes classify as `dialogue` (before the
        // scene-level pass checks `idx - 1` speaker attribution).
        {"「おはよう」", "dialogue", true, false},
        {"『おはよう\nございます』", "dialogue", true, false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.input);
        EXPECT_EQ(has_japanese(tc.input), tc.expected_has_jp);
        EXPECT_EQ(is_speaker_plate_candidate(tc.input), tc.expected_plate);
        EXPECT_EQ(classify_text(tc.input), tc.expected_type);
    }
}

TEST(Extract, ContextualSceneSpeakerAttributionAndUnattributedQuotes) {
    const std::vector<std::string> raw_pool = {
        "「おはよう」",             // idx 0: unpreceded quote -> narrative, speaker ""
        "睦　月",                   // idx 1: precedes quote -> name (internal U+3000 kept)
        "「おはよう、先生」",       // idx 2: attributed dialogue -> speaker "睦　月"
        "＊Ａ",                     // idx 3: protagonist macro -> other, has_jp false
        "「……ああ」",               // idx 4: attributed dialogue -> speaker "人見"
        "＊Ｂ",                     // idx 5: given-name macro -> other, has_jp false
        "『……広介』",               // idx 6: attributed recall -> speaker "広介"
        "　夢だ。",                 // idx 7: short narration -> narrative
        "『さよならを教えて』",     // idx 8: quote after non-plate -> narrative, speaker ""
        "こより",                   // idx 9: followed by empty gap -> standalone narrative
        "",                         // idx 10: empty slot skipped, breaks adjacency
        "「えへへ」",               // idx 11: preceded by empty slot -> narrative, speaker ""
    };

    const bj::array entries = extract_scene_strings(raw_pool, "seen020");
    ASSERT_EQ(entries.size(), kExpectedContextEntryCount);

    // idx 0: unattributed quote becomes narrative
    EXPECT_EQ(entries[0].get_object().at("idx").get_int64(), 0);
    EXPECT_EQ(obj_str(entries[0].get_object(), "type"), "narrative");
    EXPECT_EQ(obj_str(entries[0].get_object(), "speaker"), "");

    // idx 1 & 2: "睦　月" plate + dialogue preserving internal U+3000
    EXPECT_EQ(entries[1].get_object().at("idx").get_int64(), 1);
    EXPECT_EQ(obj_str(entries[1].get_object(), "text"), "睦　月");
    EXPECT_EQ(obj_str(entries[1].get_object(), "type"), "name");
    EXPECT_EQ(obj_str(entries[1].get_object(), "speaker"), "");

    EXPECT_EQ(entries[2].get_object().at("idx").get_int64(), 2);
    EXPECT_EQ(obj_str(entries[2].get_object(), "type"), "dialogue");
    EXPECT_EQ(obj_str(entries[2].get_object(), "speaker"), "睦　月");

    // idx 3 & 4: "＊Ａ" stays `other` (`has_jp == false`), dialogue gets "人見"
    EXPECT_EQ(entries[3].get_object().at("idx").get_int64(), 3);
    EXPECT_EQ(obj_str(entries[3].get_object(), "text"), "＊Ａ");
    EXPECT_EQ(obj_str(entries[3].get_object(), "type"), "other");
    EXPECT_FALSE(entries[3].get_object().at("has_jp").get_bool());

    EXPECT_EQ(entries[4].get_object().at("idx").get_int64(), 4);
    EXPECT_EQ(obj_str(entries[4].get_object(), "type"), "dialogue");
    EXPECT_EQ(obj_str(entries[4].get_object(), "speaker"), "人見");

    // idx 5 & 6: "＊Ｂ" stays `other` (`has_jp == false`), quote gets "広介"
    EXPECT_EQ(entries[5].get_object().at("idx").get_int64(), 5);
    EXPECT_EQ(obj_str(entries[5].get_object(), "type"), "other");
    EXPECT_FALSE(entries[5].get_object().at("has_jp").get_bool());

    EXPECT_EQ(entries[6].get_object().at("idx").get_int64(), 6);
    EXPECT_EQ(obj_str(entries[6].get_object(), "type"), "dialogue");
    EXPECT_EQ(obj_str(entries[6].get_object(), "speaker"), "広介");

    // idx 7 & 8: short prose "　夢だ。" does not attribute following quote
    EXPECT_EQ(obj_str(entries[7].get_object(), "type"), "narrative");
    EXPECT_EQ(obj_str(entries[8].get_object(), "type"), "narrative");
    EXPECT_EQ(obj_str(entries[8].get_object(), "speaker"), "");

    // idx 9 & 11 (idx 10 empty skipped): standalone short string -> narrative,
    // quote after empty slot -> narrative
    EXPECT_EQ(entries[9].get_object().at("idx").get_int64(), 9);
    EXPECT_EQ(obj_str(entries[9].get_object(), "type"), "narrative");
    EXPECT_EQ(entries[10].get_object().at("idx").get_int64(), 11);
    EXPECT_EQ(obj_str(entries[10].get_object(), "type"), "narrative");
    EXPECT_EQ(obj_str(entries[10].get_object(), "speaker"), "");

    // Default #NAMAE seeds "人見" and "広介" in `_start` stay `name`, whereas
    // unrelated standalone words stay `narrative`.
    const bj::array start_entries =
        extract_scene_strings({"人見", "広介", "教室"}, "_start");
    ASSERT_EQ(start_entries.size(), kExpectedStartSeedCount);
    EXPECT_EQ(obj_str(start_entries[0].get_object(), "type"), "name");
    EXPECT_EQ(obj_str(start_entries[1].get_object(), "type"), "name");
    EXPECT_EQ(obj_str(start_entries[2].get_object(), "type"), "narrative");
}

TEST(Extract, CaptureLogUnescape) {
    ScopedTempDir scratch;
    const std::string log =
        "a\\nb\n"
        "tab\\there\n"
        "back\\\\slash\n"
        "keep\\q\n"
        "trail\\\n"
        "dup\n"
        "dup\n"
        "  spaced  \n";
    const auto out = ingest_capture_log(scratch.write_child("capture.txt", log));
    ASSERT_EQ(out.size(), kCaptureEntryCount);
    EXPECT_EQ(out[kIdx0], "a\nb");
    EXPECT_EQ(out[kIdx1], "tab\there");
    EXPECT_EQ(out[kIdx2], "back\\slash");
    EXPECT_EQ(out[kIdx3], "keep\\q");
    EXPECT_EQ(out[kIdx4], "trail\\");
    EXPECT_EQ(out[kIdx5], "dup");
    EXPECT_EQ(out[kIdx6], "  spaced  ");
}

TEST(Extract, CaptureLogUniversalNewlines) {
    ScopedTempDir scratch;
    const auto out =
        ingest_capture_log(scratch.write_child("cr.txt", "a\rb\r\nc\n"));
    ASSERT_EQ(out.size(), kUniversalNewlineCount);
    EXPECT_EQ(out[kIdx0], "a");
    EXPECT_EQ(out[kIdx1], "b");
    EXPECT_EQ(out[kIdx2], "c");
}

TEST(Extract, SiglusExtractDumpFilenameRegexBomAndSpeakerAttribution) {
    ScopedTempDir scratch;
    const std::string dir = scratch.path().u8string();
    write_file((scratch.path() / "10.beta.ss.txt").u8string(),
               utf16le({u'睦', u'　', u'月', '\r', '\n',
                        u'「', u'あ', u'」'}, true));
    write_file((scratch.path() / "2.alpha.ss.txt").u8string(),
               utf16le({'x', 'y', '\r', '\n', '\r', '\n', 'c', 'd'}, true));
    write_file((scratch.path() / "notmatching.txt").u8string(), std::string("ignored"));

    const auto out = ingest_siglus_extract_dump(dir);
    ASSERT_EQ(out.size(), kDumpSceneCount);
    EXPECT_EQ(std::string(out.begin()->key()), "beta");

    const auto& beta = out.at("beta").get_object();
    EXPECT_EQ(beta.at("siglus_idx").get_int64(), kBetaSiglusIdx);
    EXPECT_EQ(std::string(beta.at("source").get_string()), "10.beta.ss.txt");
    const auto& beta_strings = beta.at("strings").get_array();
    ASSERT_EQ(beta_strings.size(), kBetaStringCount);
    EXPECT_EQ(obj_str(beta_strings[kIdx0].get_object(), "text"), "睦　月");
    EXPECT_EQ(obj_str(beta_strings[kIdx0].get_object(), "type"), "name");
    EXPECT_EQ(obj_str(beta_strings[kIdx1].get_object(), "text"), "「あ」");
    EXPECT_EQ(obj_str(beta_strings[kIdx1].get_object(), "type"), "dialogue");
    EXPECT_EQ(obj_str(beta_strings[kIdx1].get_object(), "speaker"), "睦　月");

    const auto& alpha_strings = out.at("alpha").get_object().at("strings").get_array();
    ASSERT_EQ(alpha_strings.size(), kBetaStringCount);
    EXPECT_EQ(alpha_strings[kIdx0].get_object().at("idx").get_int64(), kFirstPoolIdx);
    EXPECT_EQ(alpha_strings[kIdx1].get_object().at("idx").get_int64(), kThirdPoolIdx);
}

// End-to-end over a synthetic Scene.pck: pins unconditional SS_V2 decoding,
// idx-gap preservation across empty pool entries, speaker attribution, and CRLF output.
TEST(Extract, RunExtractDecodesSsV2PreservesIdxAndAttributesSpeaker) {
    const std::string name = "seenTEST";
    const Bytes pck = build_synthetic_scene_pck(name);

    ScopedTempDir scratch;
    const std::string dir = scratch.path().u8string();
    write_file((scratch.path() / "Scene.pck").u8string(), pck);
    const std::string out_json = (scratch.path() / "extracted_text.json").u8string();
    ASSERT_EQ(run_extract(dir, out_json), kExtractOk);

    bj::value v = json_parse_file(out_json);
    const auto& root = v.get_object();
    ASSERT_EQ(root.size(), kSingleSceneCount);
    const auto& scene_obj = root.at(name).get_object();
    std::vector<std::string> keys;
    for (const auto& kv : scene_obj) {
        keys.emplace_back(kv.key());
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"name", "rel_offset", "size", "strings"}));

    const auto& arr = scene_obj.at("strings").get_array();
    ASSERT_EQ(arr.size(), kExpectedPckStringCount);
    EXPECT_EQ(arr[kIdx0].get_object().at("idx").get_int64(), kFirstPoolIdx);
    EXPECT_EQ(obj_str(arr[kIdx0].get_object(), "text"), "ab");
    EXPECT_EQ(obj_str(arr[kIdx0].get_object(), "type"), "other");

    EXPECT_EQ(arr[kIdx1].get_object().at("idx").get_int64(), kThirdPoolIdx);
    EXPECT_EQ(obj_str(arr[kIdx1].get_object(), "text"), "まひる");
    EXPECT_EQ(obj_str(arr[kIdx1].get_object(), "type"), "name");
    EXPECT_TRUE(arr[kIdx1].get_object().at("has_jp").get_bool());

    EXPECT_EQ(arr[kIdx2].get_object().at("idx").get_int64(), kFourthPoolIdx);
    EXPECT_EQ(obj_str(arr[kIdx2].get_object(), "text"), "「ね」");
    EXPECT_EQ(obj_str(arr[kIdx2].get_object(), "type"), "dialogue");
    EXPECT_EQ(obj_str(arr[kIdx2].get_object(), "speaker"), "まひる");

    std::vector<std::string> skeys;
    for (const auto& kv : arr[kIdx0].get_object()) {
        skeys.emplace_back(kv.key());
    }
    EXPECT_EQ(skeys,
              (std::vector<std::string>{"idx", "text", "type", "has_jp", "speaker"}));

    const Bytes raw = read_file(out_json);
    std::size_t lf = 0;
    std::size_t crlf = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n') {
            ++lf;
            if (i > kZeroCount && raw[i - kByte1] == '\r') {
                ++crlf;
            }
        }
    }
    EXPECT_GT(lf, kZeroCount);
    EXPECT_EQ(lf, crlf);
}

// Primary-source precedence contract: when a valid Scene.pck is present in
// game_dir alongside a non-empty runtime capture log (`adieu_jp_strings.txt`)
// or `__Unpack__/Scene` dump, `run_extract` MUST decode `Scene.pck` as the
// authoritative source; `__Unpack__/Scene` and `adieu_jp_strings.txt` act only
// as ordered fallbacks when `Scene.pck` is absent.
TEST(Extract, RunExtractPrefersScenePckOverFallbackDumpAndCaptureLog) {
    ScopedTempDir scratch;
    const std::string dir = scratch.path().u8string();
    const fs::path pck_path = scratch.path() / "Scene.pck";
    const fs::path unpack_dir = scratch.path() / "__Unpack__" / "Scene";
    const fs::path capture_path = scratch.path() / "adieu_jp_strings.txt";
    const std::string out_json = (scratch.path() / "extracted_text.json").u8string();

    fs::create_directories(unpack_dir);
    write_file(pck_path.u8string(), build_synthetic_scene_pck("seenPRIMARY"));
    write_file((unpack_dir / "1.dump_fallback.ss.txt").u8string(),
               utf16le({u'睦', u'　', u'月', '\r', '\n', u'「', u'あ', u'」'}, true));
    write_file(capture_path.u8string(), std::string("　ランタイムキャプチャ。\n"));

    // 1. All three present -> authoritative Scene.pck wins.
    ASSERT_EQ(run_extract(dir, out_json), kExtractOk);
    {
        const bj::object root = json_parse_file(out_json).get_object();
        ASSERT_EQ(root.size(), kSingleSceneCount);
        EXPECT_NE(root.if_contains("seenPRIMARY"), nullptr);
        EXPECT_EQ(root.if_contains("dump_fallback"), nullptr);
        EXPECT_EQ(root.if_contains("_runtime_capture"), nullptr);
    }

    // 2. Scene.pck absent, unpack dump + capture log present -> unpack dump fallback wins.
    fs::remove(pck_path);
    ASSERT_EQ(run_extract(dir, out_json), kExtractOk);
    {
        const bj::object root = json_parse_file(out_json).get_object();
        ASSERT_EQ(root.size(), kSingleSceneCount);
        EXPECT_NE(root.if_contains("dump_fallback"), nullptr);
        EXPECT_EQ(root.if_contains("_runtime_capture"), nullptr);
    }

    // 3. Both Scene.pck and unpack dump absent -> runtime capture log fallback wins.
    fs::remove_all(scratch.path() / "__Unpack__");
    ASSERT_EQ(run_extract(dir, out_json), kExtractOk);
    {
        const bj::object root = json_parse_file(out_json).get_object();
        ASSERT_EQ(root.size(), kSingleSceneCount);
        EXPECT_NE(root.if_contains("_runtime_capture"), nullptr);
    }
}
