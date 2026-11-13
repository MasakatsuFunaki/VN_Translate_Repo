// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BGI bytecode walker unit tests on synthetic scripts and archives.
#include "arc_fixture.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using snu::Bytes;
namespace ext = snu::extract;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBurikoMagicChars = 27;
constexpr int kDwordBytes = 4;
constexpr int kBitsPerByte = 8;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr std::int64_t kScriptHeaderSize = 0x20;
constexpr char kAsciiSohByte = 0x01;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
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

struct ScriptBuilder {
    std::vector<std::uint32_t> code;
    Bytes text;
    std::vector<std::size_t> addrs;

    // Append a CP932 string to the text section; returns its address.
    std::size_t add_string(const std::string& utf8) {
        Bytes cp = snu::utf8_to_cp932_replace(utf8);
        std::size_t addr = text.size();
        text.insert(text.end(), cp.begin(), cp.end());
        text.push_back(0);
        addrs.push_back(addr);
        return addr;
    }

    Bytes build() const {
        // Header: 27-byte magic + NUL (0x1C bytes), then hdr_extra dword = 4
        // so hdr_size = 0x20.
        Bytes data;
        const char magic[] = "BurikoCompiledScriptVer1.00";
        data.insert(data.end(), magic, magic + kBurikoMagicChars);
        data.push_back(0);
        for (std::uint8_t b : {4, 0, 0, 0}) {
            data.push_back(b);
        }
        // Code section (dwords), ending with the 1B 00 00 00 boundary marker.
        for (std::uint32_t d : code) {
            for (int i = 0; i < kDwordBytes; ++i) {
                data.push_back(
                    static_cast<std::uint8_t>((d >> (kBitsPerByte * i)) & kByteMask));
            }
        }
        data.insert(data.end(), text.begin(), text.end());
        return data;
    }
};

// A one-line narrative script whose only text is `line`.
Bytes one_line_script(const std::string& line) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 8 * kDwordBytes;
    std::size_t a_nar = sb.add_string(line);
    sb.code = {
        0,
        3, code_size + static_cast<std::uint32_t>(a_nar),
        0x140,
        0, 0, 0,
        0x1B,
    };
    return sb.build();
}

}  // namespace

TEST(Extract, non_bgi_returns_nullopt) {
    Bytes junk(64, 0x42);
    EXPECT_FALSE(ext::extract_one(junk).has_value());
}

TEST(Extract, walker_classifies_name_dialogue_narrative) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 26 * kDwordBytes;
    std::size_t a_name = sb.add_string("直哉");
    std::size_t a_dlg = sb.add_string("「テスト」");
    std::size_t a_nar = sb.add_string("ある夜");
    std::size_t a_ruby = sb.add_string("ルビ");
    std::size_t a_lbl = sb.add_string("label");
    auto ref = [&](std::size_t addr) {
        return code_size + static_cast<std::uint32_t>(addr);
    };

    sb.code = {
        0,
        3, ref(a_name),
        0, 0, 0x140,
        ref(a_name),
        3, ref(a_dlg),
        0x140,
        0,
        3, ref(a_nar),
        0x140,
        3, ref(a_ruby),
        0x14B,
        3, ref(a_lbl),
        0,
        3, ref(a_name),
        0, 0, 0x140,
        0x1B,
    };
    ASSERT_EQ(sb.code.size(), 26u);

    auto result = ext::extract_one(sb.build());
    ASSERT_TRUE(result.has_value());
    const auto& entries = *result;
    ASSERT_EQ(entries.size(), 3u);

    const std::int64_t text_base = kScriptHeaderSize + code_size;

    EXPECT_EQ(entries[0].type, "name");
    EXPECT_EQ(entries[0].text, "直哉");
    EXPECT_EQ(entries[0].offset, text_base + static_cast<std::int64_t>(a_name));
    EXPECT_EQ(entries[0].byte_len, 4);
    EXPECT_TRUE(entries[0].has_jp);
    EXPECT_FALSE(entries[0].speaker.has_value());

    EXPECT_EQ(entries[1].type, "dialogue");
    EXPECT_EQ(entries[1].text, "「テスト」");
    EXPECT_EQ(entries[1].offset, text_base + static_cast<std::int64_t>(a_dlg));
    EXPECT_EQ(entries[1].byte_len, 10);
    ASSERT_TRUE(entries[1].speaker.has_value());
    EXPECT_EQ(*entries[1].speaker, "直哉");

    EXPECT_EQ(entries[2].type, "narrative");
    EXPECT_EQ(entries[2].text, "ある夜");
    EXPECT_EQ(entries[2].offset, text_base + static_cast<std::int64_t>(a_nar));
    EXPECT_FALSE(entries[2].speaker.has_value());
}

TEST(Extract, walker_classifies_yuzu_2g_positioned_message_helpers) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 24 * kDwordBytes;
    std::size_t a_empty = sb.add_string("");
    std::size_t a_fn1 = sb.add_string("iMessageLeftIg");
    std::size_t a_fn2 = sb.add_string("MessageCenter");
    std::size_t a_line1 =
        sb.add_string("ガゼルのその愛くるしい瞳も、その美しい毛並みも、");
    std::size_t a_spk = sb.add_string("直哉");
    std::size_t a_line2 =
        sb.add_string("外見など、彼にとってはただ肉と血の詰まった袋でしかない。");
    auto ref = [&](std::size_t addr) {
        return code_size + static_cast<std::uint32_t>(addr);
    };

    sb.code = {
        0,
        3, ref(a_line1),
        3, ref(a_empty),
        0, 0,
        0, 0,
        3, ref(a_fn1),
        0x1C,
        3, ref(a_line2),
        3, ref(a_spk),
        0, 0,
        0, 2,
        3, ref(a_fn2),
        0x1C,
        0x1B,
    };
    ASSERT_EQ(sb.code.size(), 24u);

    auto result = ext::extract_one(sb.build());
    ASSERT_TRUE(result.has_value());
    const auto& entries = *result;
    ASSERT_EQ(entries.size(), 3u);

    EXPECT_EQ(entries[0].type, "narrative");
    EXPECT_EQ(entries[0].text, "ガゼルのその愛くるしい瞳も、その美しい毛並みも、");
    EXPECT_FALSE(entries[0].speaker.has_value());

    EXPECT_EQ(entries[1].type, "dialogue");
    EXPECT_EQ(entries[1].text,
              "外見など、彼にとってはただ肉と血の詰まった袋でしかない。");
    ASSERT_TRUE(entries[1].speaker.has_value());
    EXPECT_EQ(*entries[1].speaker, "直哉");

    EXPECT_EQ(entries[2].type, "name");
    EXPECT_EQ(entries[2].text, "直哉");
}

TEST(Extract, walker_keeps_backlog_only_strings_and_drops_echoes) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 20 * kDwordBytes;
    std::size_t a_only = sb.add_string("『春と修羅』序");
    std::size_t a_shown = sb.add_string("ある夜");
    auto ref = [&](std::size_t addr) {
        return code_size + static_cast<std::uint32_t>(addr);
    };

    sb.code = {
        0,
        3, ref(a_only),
        0, 0, 0x143,
        0,
        3, ref(a_shown),
        0, 0, 0x143,
        0,
        3, ref(a_shown),
        0x140,
        0, 0, 0,
        0x1B,
    };
    ASSERT_EQ(sb.code.size(), 20u);

    auto result = ext::extract_one(sb.build());
    ASSERT_TRUE(result.has_value());
    const auto& entries = *result;
    ASSERT_EQ(entries.size(), 2u);

    const std::int64_t text_base = kScriptHeaderSize + code_size;
    EXPECT_EQ(entries[0].text, "『春と修羅』序");
    EXPECT_EQ(entries[0].type, "narrative");
    EXPECT_EQ(entries[0].offset, text_base + static_cast<std::int64_t>(a_only));
    EXPECT_FALSE(entries[0].speaker.has_value());

    EXPECT_EQ(entries[1].text, "ある夜");
    EXPECT_EQ(entries[1].type, "narrative");
    EXPECT_EQ(entries[1].offset, text_base + static_cast<std::int64_t>(a_shown));
}

TEST(Extract, script_to_json_speaker_field_rules) {
    ScriptBuilder sb;
    const std::uint32_t code_size = 26 * kDwordBytes;
    std::size_t a_name = sb.add_string("直哉");
    std::size_t a_dlg = sb.add_string("「テスト」");
    std::size_t a_nar = sb.add_string("ある夜");
    sb.add_string("ルビ");
    sb.add_string("label");
    auto ref = [&](std::size_t addr) {
        return code_size + static_cast<std::uint32_t>(addr);
    };
    sb.code = {0, 3, ref(a_name), 0, 0, 0x140,
               ref(a_name), 3, ref(a_dlg), 0x140,
               0, 3, ref(a_nar), 0x140,
               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
               0x1B};
    ASSERT_EQ(sb.code.size(), 26u);
    auto entries = ext::extract_one(sb.build());
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 3u);

    auto obj = ext::script_to_json("sc01", "data01099.arc", 123, *entries);
    EXPECT_EQ(obj.at("file").get_string(), "sc01");
    EXPECT_EQ(obj.at("jp_strings").get_int64(), 3);
    const auto& strings = obj.at("strings").get_array();
    EXPECT_FALSE(strings[0].get_object().if_contains("speaker"));
    EXPECT_EQ(strings[1].get_object().at("speaker").get_string(), "直哉");
    EXPECT_TRUE(strings[2].get_object().at("speaker").is_null());
}

TEST(Extract, jp_and_clean_text_helpers) {
    EXPECT_EQ(snu::jp_char_count("直哉とテスト"), 6);
    EXPECT_TRUE(snu::is_jp("こんにちは"));
    EXPECT_FALSE(snu::is_jp("Hello 123"));
    EXPECT_TRUE(snu::is_clean_text("line\nwith\ttabs\r"));
    const char bad_bytes[] = {'b', 'a', 'd', kAsciiSohByte, 'b', 'y', 't', 'e'};
    EXPECT_FALSE(snu::is_clean_text(std::string(bad_bytes, sizeof(bad_bytes))));
}

TEST(Extract, first_arc_in_search_order_wins_a_duplicated_script) {
    using snu::test_fixture::make_arc;
    const ScopedTempDir scratch("snu_ut_extract_game");
    const fs::path& game = scratch.path();
    snu::write_file((game / fs::u8path("data01099.arc")).u8string(),
                    make_arc({{"dup", one_line_script("パッチの行")}}));
    snu::write_file((game / fs::u8path("data01103.arc")).u8string(),
                    make_arc({{"dup", one_line_script("元の行")}}));
    const std::string out = (game / fs::u8path("extracted.json")).u8string();

    ASSERT_EQ(ext::run_extract(game.u8string(), out), 0);

    const auto root = snu::json_parse_file(out);
    ASSERT_TRUE(root.is_object());
    const auto& sc = root.get_object().at("dup").get_object();
    EXPECT_EQ(sc.at("arc").as_string(), "data01099.arc");
    const auto& strings = sc.at("strings").get_array();
    ASSERT_EQ(strings.size(), 1u);
    EXPECT_EQ(strings[0].get_object().at("text").as_string(), "パッチの行");
}
