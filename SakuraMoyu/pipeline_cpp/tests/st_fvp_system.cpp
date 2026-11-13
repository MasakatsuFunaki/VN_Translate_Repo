// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests against installed FAVORITE game files (Sakura.hcb, graph.bin,
// translation_table.tsv) and optional live Gemini API verification.
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "fvp/fvp_graph.h"
#include "fvp/fvp_hcb.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"
#include "ui/ui_patcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
namespace bj = boost::json;
namespace tr = fav::translate;
using namespace fav;

namespace {

constexpr std::uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;
constexpr std::size_t kHeaderSizeOffset = 4u;
constexpr std::size_t kTocOffsetFieldOffset = 4u;
constexpr std::size_t kTocSizeFieldOffset = 8u;
constexpr std::size_t kMaxBadLinesReported = 5u;
constexpr std::size_t kBadLinePreviewChars = 60u;
constexpr std::ptrdiff_t kExpectedTabsPerRow = 1;
constexpr std::uint32_t kExpectedGraphFileCount = 1146u;
constexpr std::size_t kMinExpectedHcbStrings = 10000u;
constexpr std::size_t kMinExpectedCastNames = 5u;
constexpr std::size_t kMinExpectedMappedSpeakers = 20000u;
constexpr std::uint32_t kKuroEllipsisOffset = 0x0A1E99u;
constexpr std::uint32_t kChiwaOnigiriOffset = 0x0AA64Eu;
constexpr std::uint32_t kTaigaSpokenOffset = 0x0A3A56u;
constexpr std::size_t kEarlyNovelStartIdx = 4900u;
constexpr std::size_t kMidNovelStartIdx = 25000u;
constexpr std::size_t kLateNovelStartIdx = 45000u;
constexpr std::size_t kMovingWindowLen = 8u;
constexpr int kPromptLineNumberOffset = 1;
constexpr unsigned char kAsciiCeiling = 0x80u;
constexpr std::int64_t kJudgeMaxTokens = 4096;
constexpr int kSuccessExitCode = 0;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
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

std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in path");
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        utf8.data(),
        needed,
        nullptr,
        nullptr);
    if (written != needed) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return utf8;
}

std::string resolve_game_dir() {
    std::error_code ec;
    if (const wchar_t* env_dir = _wgetenv(L"VN_GAME_DIR");
        env_dir != nullptr && *env_dir != L'\0') {
        const fs::path candidate(env_dir);
        if (fs::is_directory(candidate, ec)) {
            return wide_to_utf8_strict(candidate.native());
        }
        return {};
    }

    for (const char* fallback :
         {"C:\\FAVORITE\\さくら、もゆ。-as the Night's, Reincarnation-",
          "C:\\Favorite"}) {
        const fs::path candidate = fs::u8path(fallback);
        if (fs::is_directory(candidate, ec)) {
            return wide_to_utf8_strict(candidate.native());
        }
    }
    return {};
}

std::string resolve_installed_hcb() {
    const std::string game_dir = resolve_game_dir();
    if (game_dir.empty()) {
        return {};
    }
    std::error_code ec;
    const fs::path hcb = fs::u8path(game_dir) / "Sakura.hcb";
    if (!fs::exists(hcb, ec)) {
        return {};
    }
    return hcb.u8string();
}

bool live_api_tests_enabled() noexcept {
    const wchar_t* flag = _wgetenv(L"VN_RUN_LIVE_API_TESTS");
    return flag != nullptr && std::wcscmp(flag, L"1") == 0;
}

void write_u32_le(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & kByteMask);
    p[1] = static_cast<std::uint8_t>((v >> kOneByteShift) & kByteMask);
    p[2] = static_cast<std::uint8_t>((v >> kTwoBytesShift) & kByteMask);
    p[3] = static_cast<std::uint8_t>((v >> kThreeBytesShift) & kByteMask);
}

}  // namespace

TEST(FvpSystem, deployed_tsv_has_one_tab_per_line) {
    const std::string game_dir = resolve_game_dir();
    if (game_dir.empty()) {
        GTEST_SKIP() << "Game directory not installed";
    }
    const fs::path tsv = fs::u8path(game_dir) / "translation_table.tsv";
    if (!fs::exists(tsv)) {
        GTEST_SKIP() << "translation_table.tsv not built yet";
    }
    std::ifstream f(tsv, std::ios::binary);
    std::string line;
    std::size_t ln = 0;
    std::size_t bad = 0;
    while (std::getline(f, line) && bad < kMaxBadLinesReported) {
        ++ln;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (std::count(line.begin(), line.end(), '\t') != kExpectedTabsPerRow) {
            ++bad;
            GTEST_LOG_(INFO) << "bad line " << ln << ": "
                             << line.substr(0, kBadLinePreviewChars);
        }
    }
    EXPECT_EQ(bad, 0u);
}

TEST(FvpSystem, ui_patcher_patches_real_graph_bin_entries) {
    const std::string game_dir = resolve_game_dir();
    if (game_dir.empty()) {
        GTEST_SKIP() << "Game directory not installed";
    }
    const fs::path bin_fs = fs::u8path(game_dir) / "graph.bin";
    const fs::path backup_fs = fs::u8path(game_dir) / "graph.bin.backup";
    if (!fs::exists(bin_fs) || !fs::exists(backup_fs)) {
        GTEST_SKIP() << "graph.bin(.backup) not installed in " << game_dir;
    }
    const fvp::GraphArchive deployed(bin_fs.u8string());
    const fvp::GraphArchive backup(backup_fs.u8string());
    EXPECT_EQ(deployed.header().file_count, kExpectedGraphFileCount);

    std::vector<const fvp::ArchiveEntry*> ui_entries;
    for (const auto& e : backup.header().entries) {
        if (e.name.rfind("menu_", 0) == 0 ||
            e.name.rfind("opt_", 0) == 0 ||
            e.name.rfind("fc_", 0) == 0 ||
            e.name.rfind("sys_", 0) == 0) {
            ui_entries.push_back(&e);
        }
    }

    const ScopedTempDir tmp_dir("fav_st_ui_patcher_facade");
    const std::string mini_bin = (tmp_dir.path() / "graph.bin").u8string();

    {
        std::string name_pool;
        std::vector<std::uint32_t> name_offsets;
        std::vector<std::vector<std::uint8_t>> payloads;
        for (const fvp::ArchiveEntry* e : ui_entries) {
            name_offsets.push_back(static_cast<std::uint32_t>(name_pool.size()));
            name_pool.append(e->name);
            name_pool.push_back('\0');
            payloads.push_back(backup.extract_raw(e->name));
        }

        std::ofstream out(fs::u8path(mini_bin), std::ios::binary | std::ios::trunc);
        std::uint8_t hdr[fvp::kArchiveHeaderByteCount]{};
        const std::uint32_t count = static_cast<std::uint32_t>(ui_entries.size());
        write_u32_le(hdr, count);
        write_u32_le(hdr + kHeaderSizeOffset,
                     static_cast<std::uint32_t>(name_pool.size()));
        out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));

        std::uint32_t offset = static_cast<std::uint32_t>(
            fvp::kArchiveHeaderByteCount +
            static_cast<std::size_t>(count) * fvp::kTocEntryByteCount +
            name_pool.size());
        std::vector<std::uint8_t> toc(
            static_cast<std::size_t>(count) * fvp::kTocEntryByteCount, 0);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint8_t* p =
                toc.data() + static_cast<std::size_t>(i) * fvp::kTocEntryByteCount;
            write_u32_le(p, name_offsets[i]);
            write_u32_le(p + kTocOffsetFieldOffset, offset);
            write_u32_le(p + kTocSizeFieldOffset,
                         static_cast<std::uint32_t>(payloads[i].size()));
            offset += static_cast<std::uint32_t>(payloads[i].size());
        }
        out.write(reinterpret_cast<const char*>(toc.data()),
                  static_cast<std::streamsize>(toc.size()));
        out.write(name_pool.data(), static_cast<std::streamsize>(name_pool.size()));
        for (const auto& payload : payloads) {
            out.write(reinterpret_cast<const char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
        }
    }

    ui::UiPatcher patcher;
    ui::UiPatcher moved_patcher = std::move(patcher);
    ui::PatchUiOptions opts;
    opts.game_dir = tmp_dir.path().u8string();
    ASSERT_EQ(moved_patcher.patch_archive(opts), kSuccessExitCode);
    EXPECT_TRUE(fs::exists(tmp_dir.path() / "graph.bin.backup"));

    const fvp::GraphArchive patched_mini(mini_bin);
    for (const char* req :
         {"opt_sheet1", "opt_sheet2", "opt_sheet3", "opt_parts", "menu_bg",
          "menu_exit_mes", "menu_yes_1", "menu_no_1", "opt_s_pts1", "opt_m_pts2",
          "opt_001", "opt_117", "opt_201", "fc_back_help", "sys_config_M"}) {
        ASSERT_TRUE(patched_mini.contains(req)) << "Missing UI entry: " << req;
        const auto actual_img = patched_mini.load_image(req);
        const auto expected_img = deployed.load_image(req);
        EXPECT_EQ(actual_img.width, expected_img.width) << req;
        EXPECT_EQ(actual_img.height, expected_img.height) << req;
        EXPECT_EQ(actual_img.pixels, expected_img.pixels)
            << "Pixel mismatch on patched UI entry: " << req;
    }
}

TEST(FvpSystem, real_sakura_hcb_walks_cleanly) {
    const std::string hcb = resolve_installed_hcb();
    if (hcb.empty()) {
        GTEST_SKIP() << "Sakura.hcb not installed";
    }
    const auto items = fvp::load_all(hcb);
    EXPECT_GT(items.size(), kMinExpectedHcbStrings);
    for (std::size_t i = 1; i < items.size(); ++i) {
        ASSERT_GT(items[i].offset, items[i - 1].offset) << "at index " << i;
    }
}

TEST(FvpSystem, cast_registry_still_finds_names_in_the_bytecode) {
    const std::string hcb = resolve_installed_hcb();
    if (hcb.empty()) {
        GTEST_SKIP() << "Sakura.hcb not installed";
    }
    const auto cast = tr::cast_registry(read_file(hcb));
    EXPECT_GT(cast.size(), kMinExpectedCastNames)
        << "the name-window walk found almost nothing";
    EXPECT_NE(cast.find("クロ"), cast.end()) << "クロ missing";
}

TEST(FvpSystem, speaker_gate_passes_over_the_installed_game) {
    const std::string hcb = resolve_installed_hcb();
    if (hcb.empty()) {
        GTEST_SKIP() << "Sakura.hcb not installed";
    }
    const tr::SpeakerCheckReport r = tr::run_speaker_checks(read_file(hcb));
    EXPECT_TRUE(r.passed) << "check " << r.failed_check << ": " << r.reason;
    EXPECT_EQ(r.failed_check, kSuccessExitCode);
    EXPECT_GT(r.glossary_entries, 0u);
}

TEST(FvpSystem, map_line_speakers_resolves_bytecode_speakers) {
    const std::string hcb = resolve_installed_hcb();
    if (hcb.empty()) {
        GTEST_SKIP() << "Sakura.hcb not installed";
    }
    const auto speakers = tr::map_line_speakers(read_file(hcb));
    EXPECT_GT(speakers.size(), kMinExpectedMappedSpeakers);
    ASSERT_NE(speakers.find(kKuroEllipsisOffset), speakers.end());
    EXPECT_EQ(speakers.at(kKuroEllipsisOffset), "Kuro");
    ASSERT_NE(speakers.find(kChiwaOnigiriOffset), speakers.end());
    EXPECT_EQ(speakers.at(kChiwaOnigiriOffset), "Chiwa");
    ASSERT_NE(speakers.find(kTaigaSpokenOffset), speakers.end());
    EXPECT_EQ(speakers.at(kTaigaSpokenOffset), "Taiga");
}

TEST(FvpSystem, real_api_three_moving_windows_with_argon_judge) {
    if (!live_api_tests_enabled()) {
        GTEST_SKIP() << "Set VN_RUN_LIVE_API_TESTS=1 to run live Gemini API tests";
    }
    const std::string hcb_path = resolve_installed_hcb();
    if (hcb_path.empty()) {
        GTEST_SKIP() << "Sakura.hcb not installed";
    }
    const fs::path corpus_fs =
        fs::u8path(FAV_PROJECT_DIR) / "script_output" / "translated_text.json";
    ASSERT_TRUE(fs::exists(corpus_fs)) << corpus_fs.u8string();
    if (gemini::load_api_key().empty()) {
        GTEST_SKIP() << "GEMINI_API_KEY not configured";
    }

    const Bytes hcb = read_file(hcb_path);
    const auto speakers = tr::map_line_speakers(hcb);

    const bj::value root = json_parse_file(corpus_fs.u8string());
    const bj::array& all_lines =
        root.get_object().at("Sakura.hcb").get_object().at("lines").get_array();

    auto format_line = [&](const bj::object& ln) -> std::string {
        const std::string c(ln.at("content").get_string());
        const std::uint32_t off =
            static_cast<std::uint32_t>(ln.at("offset").to_number<std::uint64_t>());
        const auto it = speakers.find(off);
        if (it != speakers.end() && !it->second.empty() && it->second != "???") {
            return "[" + it->second + "] " + c;
        }
        return c;
    };

    struct WindowSpec {
        const char* label;
        std::size_t start_idx;
    };
    const WindowSpec specs[] = {
        {"early_novel", kEarlyNovelStartIdx},
        {"mid_novel", kMidNovelStartIdx},
        {"late_novel", kLateNovelStartIdx},
    };

    gemini::Client client;
    bj::array tsv_test_lines;
    bj::object cache_obj;
    std::string judge_transcript;

    for (const auto& spec : specs) {
        std::vector<const bj::object*> before_objs;
        std::vector<const bj::object*> target_objs;
        std::vector<const bj::object*> after_objs;

        for (std::size_t idx = 0; idx < all_lines.size(); ++idx) {
            const auto& ln = all_lines[idx].get_object();
            const auto* tp = ln.if_contains("type");
            if (tp == nullptr || !tp->is_string() || tp->get_string() != "STRING") {
                continue;
            }
            const std::string c(ln.at("content").get_string());
            if (!tr::needs_translation(c)) {
                continue;
            }

            if (idx < spec.start_idx) {
                before_objs.push_back(&ln);
                if (before_objs.size() > static_cast<std::size_t>(tr::CONTEXT_WINDOW)) {
                    before_objs.erase(before_objs.begin());
                }
                continue;
            }
            if (target_objs.size() < kMovingWindowLen) {
                target_objs.push_back(&ln);
                continue;
            }
            if (after_objs.size() < static_cast<std::size_t>(tr::CONTEXT_AFTER)) {
                after_objs.push_back(&ln);
                continue;
            }
            break;
        }
        ASSERT_EQ(target_objs.size(), kMovingWindowLen) << spec.label;

        std::vector<std::string> ctx_before;
        ctx_before.reserve(before_objs.size());
        for (const auto* o : before_objs) {
            ctx_before.push_back(format_line(*o));
        }

        std::vector<std::string> prompt_lines;
        prompt_lines.reserve(target_objs.size());
        for (const auto* o : target_objs) {
            prompt_lines.push_back(format_line(*o));
        }

        std::vector<std::string> ctx_after;
        ctx_after.reserve(after_objs.size());
        for (const auto* o : after_objs) {
            ctx_after.push_back(format_line(*o));
        }

        auto translations = tr::call_gemini(client, prompt_lines, ctx_before, ctx_after);
        ASSERT_EQ(translations.size(), kMovingWindowLen)
            << "Incomplete window translation in " << spec.label;

        judge_transcript += std::string("=== Window: ") + spec.label + " ===\n";
        for (std::size_t j = 0; j < kMovingWindowLen; ++j) {
            const std::string jp(target_objs[j]->at("content").get_string());
            const std::string en =
                tr::strip(translations[static_cast<int>(j) + kPromptLineNumberOffset]);
            ASSERT_FALSE(en.empty()) << spec.label << " line "
                                     << (j + static_cast<std::size_t>(kPromptLineNumberOffset));
            EXPECT_NE(en, jp) << spec.label << " line "
                              << (j + static_cast<std::size_t>(kPromptLineNumberOffset));
            EXPECT_NE(en.front(), '[') << "Leaked [Speaker] tag in " << en;
            EXPECT_EQ(en.find('|'), std::string::npos) << "Leaked ruby pipe in " << en;
            for (const unsigned char ch : en) {
                EXPECT_LT(ch, kAsciiCeiling) << "Non-ASCII byte in translation: " << en;
            }

            cache_obj[jp] = en;
            bj::object row;
            row["content"] = jp;
            row["translated"] = en;
            tsv_test_lines.push_back(std::move(row));

            judge_transcript += "JP: " + prompt_lines[j] + "\nEN: " + en + "\n\n";
        }
    }

    {
        const ScopedTempDir tmp_dir("fav_st_moving_windows");
        const std::string tmp_tr = (tmp_dir.path() / "translated.json").u8string();
        const std::string tmp_ca = (tmp_dir.path() / "cache.json").u8string();
        const std::string tmp_tsv = (tmp_dir.path() / "table.tsv").u8string();

        bj::object script_obj;
        script_obj["lines"] = tsv_test_lines;
        bj::object root_obj;
        root_obj["Sakura.hcb"] = std::move(script_obj);
        write_file(tmp_tr, bj::serialize(bj::value(std::move(root_obj))));
        write_file(tmp_ca, bj::serialize(bj::value(cache_obj)));

        ASSERT_EQ(build_tsv::run_build(tmp_tr, tmp_ca, tmp_tsv), kSuccessExitCode);
        const Bytes tsv_bytes = read_file(tmp_tsv);
        const std::string tsv_text(
            reinterpret_cast<const char*>(tsv_bytes.data()), tsv_bytes.size());
        for (const auto& kv : cache_obj) {
            const std::string flat_jp = build_tsv::flatten(std::string(kv.key()));
            EXPECT_NE(tsv_text.find(flat_jp + "\t"), std::string::npos)
                << "Missing JP key in TSV: " << flat_jp;
        }
    }

    bj::object judge_body;
    judge_body["model"] = tr::MODEL;
    judge_body["max_tokens"] = kJudgeMaxTokens;
    judge_body["system"] =
        "You are a senior Japanese-to-English Visual Novel translation auditor. "
        "Verify whether the English translations across the 3 sampled windows "
        "(early_novel, mid_novel, late_novel) accurately translate the Japanese lines, "
        "preserve speaker voice and pronouns, handle [・|...] emphasis and ruby text cleanly, "
        "and contain zero untranslated Japanese or formatting glitches. "
        "End your response with 'VERDICT: PASS' if the translations are publication-ready "
        "with zero critical errors, or 'VERDICT: FAIL' followed by the error details.";
    bj::object judge_msg;
    judge_msg["role"] = "user";
    judge_msg["content"] =
        "Audit these 3 moving windows of translated visual novel dialogue:\n\n" +
        judge_transcript;
    judge_body["messages"] = bj::array{std::move(judge_msg)};

    const bj::value judge_res = client.messages(bj::value(std::move(judge_body)));
    std::string judge_text;
    if (const auto* content = judge_res.get_object().if_contains("content")) {
        for (const auto& block : content->get_array()) {
            if (const auto* txt = block.get_object().if_contains("text");
                txt != nullptr && txt->is_string()) {
                judge_text += std::string(txt->get_string());
            }
        }
    }
    log_info("\n--- ARGON JUDGE AUDIT REPORT ---\n" + judge_text +
             "\n--------------------------------");
    EXPECT_NE(judge_text.find("VERDICT: PASS"), std::string::npos)
        << "Argon judge did not return VERDICT: PASS:\n" << judge_text;
}
