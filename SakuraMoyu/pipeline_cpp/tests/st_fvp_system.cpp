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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

namespace {

constexpr std::size_t kScriptHeaderBytes = 4u;
constexpr std::size_t kNulTerminatorBytes = 1u;
constexpr std::uint8_t kNulTerminatorByte = 0x00u;
constexpr std::uint8_t kFuncHeaderOp = 0x01u;
constexpr std::uint8_t kCallSubOp = 0x02u;
constexpr std::uint8_t kReturnOp = 0x04u;
constexpr std::uint8_t kPushStringOp = 0x0Eu;
constexpr std::uint8_t kFaceGraphicOp = 0x10u;
constexpr std::uint8_t kFaceGraphicSlot = 0x01u;
constexpr std::uint8_t kZeroOperand = 0x00u;
constexpr std::size_t kMaxShortPushPayloadBytes = 254u;
constexpr std::size_t kPipeChunkBytes = 4096u;
constexpr DWORD kBufferScaleFactor = 2u;
constexpr std::size_t kEscapedBackslashScale = 2u;
constexpr std::size_t kEscapedQuoteBackslashes = 1u;
constexpr std::int64_t kExpectedBatchLines = 8;
constexpr std::int64_t kZeroThoughtTokens = 0;
constexpr double kMaxBatchLatencySeconds =
    static_cast<double>(vn::llm::kDefaultReadTimeoutSeconds);
constexpr std::size_t kNarrationRubySceneIdx = 0u;
constexpr std::size_t kKuroToTaigaSceneIdx = 1u;
constexpr std::size_t kTaigaEmphasisSceneIdx = 2u;
constexpr std::size_t kHaruOniichanSceneIdx = 3u;
constexpr std::size_t kChiwaCastSceneIdx = 4u;
constexpr std::size_t kSolNachtSceneIdx = 6u;
constexpr std::size_t kSakuraHaruSceneIdx = 7u;
constexpr std::size_t kSpeakerPlateCount = 4u;
constexpr std::size_t kDisplayLineOffset = 1u;
constexpr std::size_t kNewlineCharBytes = 1u;

void append_hcb_u32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kOneByteShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kTwoBytesShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kThreeBytesShift) & kByteMask));
}

void append_hcb_push_string(Bytes& out, const std::string& utf8) {
    const Bytes cp932 = utf8_to_cp932_replace(utf8);
    if (cp932.size() > kMaxShortPushPayloadBytes) {
        throw std::runtime_error("Synthetic FVP string exceeds 1-byte push length");
    }
    const auto roundtrip = cp932_to_utf8_strict(cp932);
    if (!roundtrip.has_value() || *roundtrip != utf8) {
        throw std::runtime_error("Synthetic FVP string is not strictly CP932-clean: " + utf8);
    }
    out.push_back(kPushStringOp);
    out.push_back(static_cast<std::uint8_t>(cp932.size() + kNulTerminatorBytes));
    out.insert(out.end(), cp932.begin(), cp932.end());
    out.push_back(kNulTerminatorByte);
}

std::uint32_t append_speaker_subroutine(Bytes& body, const std::string& padded_jp_name) {
    const std::uint32_t sub_offset =
        static_cast<std::uint32_t>(kScriptHeaderBytes + body.size());
    body.push_back(kFuncHeaderOp);
    body.push_back(kFaceGraphicSlot);
    body.push_back(kFaceGraphicSlot);
    body.push_back(kFaceGraphicOp);
    body.push_back(kFaceGraphicSlot);
    append_hcb_push_string(body, padded_jp_name);
    body.push_back(kReturnOp);
    return sub_offset;
}

void append_spoken_line(Bytes& body, std::uint32_t speaker_sub, const std::string& jp_line) {
    body.push_back(kCallSubOp);
    append_hcb_u32(body, speaker_sub);
    append_hcb_push_string(body, jp_line);
}

fs::path current_test_binary_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * kBufferScaleFactor);
    }
}

fs::path resolve_pipeline_exe(const char* exe_name) {
    std::error_code ec;
    const fs::path bin_dir = current_test_binary_dir();
    const fs::path candidates[] = {
        bin_dir / exe_name,
        bin_dir.parent_path().parent_path() / bin_dir.filename() / exe_name,
        fs::u8path(FAV_PROJECT_DIR) / "build" / "pipeline" / "Release" / exe_name,
        fs::u8path(FAV_PROJECT_DIR) / "build" / "install" / "bin" / exe_name,
    };
    for (const fs::path& candidate : candidates) {
        if (!candidate.empty() && fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in command argument");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        wide.data(),
        needed);
    if (written != needed) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    return wide;
}

std::wstring quote_wide_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(
                backslashes * kEscapedBackslashScale + kEscapedQuoteBackslashes, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        out.push_back(ch);
        backslashes = 0;
    }
    out.append(backslashes * kEscapedBackslashScale, L'\\');
    out.push_back(L'"');
    return out;
}

struct CapturedProcessResult {
    int exit_code = -1;
    double elapsed_seconds = 0.0;
    std::string output;
};

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE h = nullptr) noexcept : h_(h) {}
    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;
    ~ScopedWinHandle() { reset(); }

    void reset(HANDLE next = nullptr) noexcept {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
        h_ = next;
    }

    HANDLE get() const noexcept { return h_; }

private:
    HANDLE h_ = nullptr;
};

CapturedProcessResult run_process_captured(
    const fs::path& exe_path, const std::vector<std::string>& args) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &sa, 0)) {
        throw std::runtime_error("CreatePipe failed");
    }
    ScopedWinHandle read_pipe(raw_read);
    ScopedWinHandle write_pipe(raw_write);
    SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);

    const std::wstring app_wide = exe_path.native();
    std::wstring cmd_wide = L"\"" + app_wide + L"\"";
    for (const std::string& a : args) {
        cmd_wide.push_back(L' ');
        cmd_wide += quote_wide_arg(utf8_to_wide_strict(a));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe.get();
    si.hStdError = write_pipe.get();

    const auto start = std::chrono::steady_clock::now();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        app_wide.c_str(),
        cmd_wide.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi);
    write_pipe.reset();
    if (!ok) {
        throw std::runtime_error("CreateProcessW failed for " + exe_path.u8string());
    }
    ScopedWinHandle proc_handle(pi.hProcess);
    ScopedWinHandle thread_handle(pi.hThread);

    CapturedProcessResult res;
    char chunk[kPipeChunkBytes];
    for (;;) {
        DWORD n_read = 0;
        const BOOL read_ok =
            ReadFile(read_pipe.get(), chunk, sizeof(chunk), &n_read, nullptr);
        if (!read_ok || n_read == 0) {
            break;
        }
        res.output.append(chunk, n_read);
    }

    WaitForSingleObject(proc_handle.get(), INFINITE);
    res.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    DWORD exit_code = 1;
    GetExitCodeProcess(proc_handle.get(), &exit_code);
    res.exit_code = static_cast<int>(exit_code);
    return res;
}

std::string to_ascii_lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

bool contains_any_substr(
    std::string_view lower_hay,
    std::initializer_list<std::string_view> needles) noexcept {
    for (const std::string_view needle : needles) {
        if (lower_hay.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool is_github_actions_ci() noexcept {
    const wchar_t* gha = _wgetenv(L"GITHUB_ACTIONS");
    return gha != nullptr && std::wcscmp(gha, L"true") == 0;
}

}  // namespace

TEST(FvpSystem, live_argon_sakuramoyu_pipeline_and_resume_idempotency) {
    if (gemini::load_api_key().empty()) {
        if (is_github_actions_ci()) {
            GTEST_SKIP() << "Skipping live Gemini SYSTEM test on GitHub Actions without GEMINI_API_KEY";
        }
        FAIL() << "GEMINI_API_KEY is missing locally (neither environment variable nor ~/.gemini/.env provided a key)";
    }

    const fs::path extract_exe = resolve_pipeline_exe("01_extract.exe");
    const fs::path translate_exe = resolve_pipeline_exe("02_translate.exe");
    ASSERT_FALSE(extract_exe.empty()) << "Built 01_extract.exe binary not found";
    ASSERT_FALSE(translate_exe.empty()) << "Built 02_translate.exe binary not found";

    const ScopedTempDir temp("fav_st_live_sakuramoyu_e2e");
    const fs::path project_dir = temp.path() / "project";
    const fs::path script_out_dir = project_dir / "script_output";
    const fs::path game_dir = temp.path() / "game";
    fs::create_directories(script_out_dir);
    fs::create_directories(game_dir);

    constexpr const char* kLine1NarrationRubyJp =
        "　夜風が頬を撫で、<桜|さくら>の花びらが静かに舞い落ちた。";
    constexpr const char* kLine2KuroToTaigaJp =
        "「おかえり、大雅。長い夜の始まりだよ」";
    constexpr const char* kLine3TaigaEmphasisJp =
        "「ああ……[・|ぼ][・|く]はずっと、クロの声を待っていたんだ」";
    constexpr const char* kLine4HaruOniichanJp =
        "「お兄ちゃん、早く早く！　一緒に<満開|まんかい>のさくらを見に行こうよ！」";
    constexpr const char* kLine5ChiwaCastJp =
        "「千和も姫織も十夜も、みんな大雅のことを心配してたんだからね！」";
    constexpr const char* kLine6CreatureRubyJp =
        "　<輪廻|りんね>する季節の中で、[よる|夜]の向こうから夜のイキモノが目を覚ました。";
    constexpr const char* kLine7SolNachtJp =
        "「ソルとナハトが巡るみたいに、ぼくたちの願いもきっと繋がっているよ」";
    constexpr const char* kLine8SakuraHaruJp =
        "　ひらひらと舞うさくらの下で、ぼくはハルの小さな手を握り返した。";

    // Build a real FVP Sakura.hcb bytecode image with cast-window subroutines
    // (< 0x2000) and speaker-attributed scene pushes.
    {
        Bytes body;
        const std::uint32_t kuro_sub = append_speaker_subroutine(body, "　クロ　");
        const std::uint32_t haru_sub = append_speaker_subroutine(body, "　ハル　");
        const std::uint32_t chiwa_sub = append_speaker_subroutine(body, "　千和　");
        const std::uint32_t taiga_sub = append_speaker_subroutine(body, "　大雅　");

        body.push_back(kFuncHeaderOp);
        body.push_back(kZeroOperand);
        body.push_back(kZeroOperand);
        append_hcb_push_string(body, kLine1NarrationRubyJp);
        append_spoken_line(body, kuro_sub, kLine2KuroToTaigaJp);
        append_spoken_line(body, taiga_sub, kLine3TaigaEmphasisJp);
        append_spoken_line(body, haru_sub, kLine4HaruOniichanJp);
        append_spoken_line(body, chiwa_sub, kLine5ChiwaCastJp);
        append_hcb_push_string(body, kLine6CreatureRubyJp);
        append_spoken_line(body, kuro_sub, kLine7SolNachtJp);
        append_hcb_push_string(body, kLine8SakuraHaruJp);
        body.push_back(kReturnOp);

        Bytes hcb;
        append_hcb_u32(
            hcb, static_cast<std::uint32_t>(kScriptHeaderBytes + body.size()));
        hcb.insert(hcb.end(), body.begin(), body.end());
        write_file((game_dir / "Sakura.hcb").u8string(), hcb);
    }

    // Step 1: Run real 01_extract.exe over the synthetic Sakura.hcb.
    const CapturedProcessResult extract_proc = run_process_captured(
        extract_exe,
        {"--dir", project_dir.u8string(), "--game-dir", game_dir.u8string()});
    ASSERT_EQ(extract_proc.exit_code, kSuccessExitCode)
        << "01_extract.exe failed:\n" << extract_proc.output;
    ASSERT_TRUE(fs::exists(script_out_dir / "extracted_text.json"));

    // Step 2 (Pass 1): Run real 02_translate.exe against live gemini-4-carbon.
    const CapturedProcessResult pass1 = run_process_captured(
        translate_exe,
        {
            "--dir", project_dir.u8string(),
            "--game-dir", game_dir.u8string(),
            "--batch", "8",
            "--max-batches", "1",
        });

    ASSERT_EQ(pass1.exit_code, kSuccessExitCode)
        << "02_translate.exe Pass 1 failed:\n" << pass1.output;
    EXPECT_EQ(pass1.output.find("[ERROR]"), std::string::npos)
        << "Unexpected [ERROR] in Pass 1 output:\n" << pass1.output;
    EXPECT_LT(pass1.elapsed_seconds, kMaxBatchLatencySeconds)
        << "Batch latency approached/exceeded read timeout";

    const fs::path telemetry_file =
        script_out_dir / "last_gemini_translate" / tr::MODEL /
        "last_batch_telemetry.json";
    ASSERT_TRUE(fs::exists(telemetry_file))
        << "Missing batch telemetry file: " << telemetry_file.u8string();

    const bj::object telemetry = json_parse_file(telemetry_file.u8string()).as_object();
    const std::int64_t thought_tokens = telemetry.at("thought_tokens").as_int64();
    EXPECT_EQ(std::string(telemetry.at("model").as_string()), tr::MODEL);
    EXPECT_EQ(std::string(telemetry.at("thinking_level").as_string()),
              std::string(gemini::kDefaultThinkingLevel));
    EXPECT_EQ(telemetry.at("batch_lines").as_int64(), kExpectedBatchLines);
    EXPECT_GT(thought_tokens, kZeroThoughtTokens)
        << "gemini-4-carbon with thinkingLevel=MEDIUM must produce thought_tokens > 0";
    EXPECT_NE(pass1.output.find("thoughts=" + std::to_string(thought_tokens)),
              std::string::npos)
        << "Expected thoughts=<N> telemetry in Pass 1 log:\n" << pass1.output;

    const fs::path cache_path = script_out_dir / "translation_cache_gemini.json";
    const fs::path tsv_path = game_dir / "translation_table.tsv";
    ASSERT_TRUE(fs::exists(cache_path));
    ASSERT_TRUE(fs::exists(tsv_path));

    const tr::Cache cache = tr::load_cache(cache_path.u8string());
    const char* const kSceneLines[] = {
        kLine1NarrationRubyJp,
        kLine2KuroToTaigaJp,
        kLine3TaigaEmphasisJp,
        kLine4HaruOniichanJp,
        kLine5ChiwaCastJp,
        kLine6CreatureRubyJp,
        kLine7SolNachtJp,
        kLine8SakuraHaruJp,
    };

    log_info("[LIVE SAKURAMOYU ST] thought_tokens=" + std::to_string(thought_tokens) +
             " elapsed=" + std::to_string(pass1.elapsed_seconds) + "s");

    std::vector<std::string> scene_en;
    scene_en.reserve(std::size(kSceneLines));
    for (std::size_t i = 0; i < std::size(kSceneLines); ++i) {
        const std::string* ptr = cache.get(kSceneLines[i]);
        ASSERT_NE(ptr, nullptr)
            << "Missing cached translation for line " << (i + kDisplayLineOffset);
        const std::string& en = *ptr;
        ASSERT_FALSE(en.empty())
            << "Empty translation for line " << (i + kDisplayLineOffset);
        log_info("  [" + std::to_string(i + kDisplayLineOffset) + "] " + en);

        // Zero leaked kana/kanji, speaker tags, [reading|base], or <ruby|reading>.
        EXPECT_EQ(jp_char_count(en), 0) << "Leaked Japanese characters in: " << en;
        EXPECT_EQ(en.find('['), std::string::npos) << "Leaked '[' markup in: " << en;
        EXPECT_EQ(en.find(']'), std::string::npos) << "Leaked ']' markup in: " << en;
        EXPECT_EQ(en.find('<'), std::string::npos) << "Leaked '<' ruby tag in: " << en;
        EXPECT_EQ(en.find('>'), std::string::npos) << "Leaked '>' ruby tag in: " << en;
        EXPECT_EQ(en.find('|'), std::string::npos) << "Leaked '|' ruby pipe in: " << en;
        for (const unsigned char ch : en) {
            EXPECT_LT(ch, kAsciiCeiling) << "Non-ASCII byte in English line: " << en;
        }
        scene_en.push_back(en);
    }

    // Verify SakuraMoyu cast glossary fidelity across translated dialogue/narration.
    EXPECT_NE(to_ascii_lower(scene_en[kKuroToTaigaSceneIdx]).find("taiga"),
              std::string::npos)
        << scene_en[kKuroToTaigaSceneIdx];
    EXPECT_NE(to_ascii_lower(scene_en[kTaigaEmphasisSceneIdx]).find("kuro"),
              std::string::npos)
        << scene_en[kTaigaEmphasisSceneIdx];
    EXPECT_TRUE(contains_any_substr(
        to_ascii_lower(scene_en[kHaruOniichanSceneIdx]),
        {"cherry", "sakura", "blossom"}))
        << scene_en[kHaruOniichanSceneIdx];
    const std::string low5 = to_ascii_lower(scene_en[kChiwaCastSceneIdx]);
    EXPECT_NE(low5.find("himeori"), std::string::npos)
        << scene_en[kChiwaCastSceneIdx];
    EXPECT_NE(low5.find("toya"), std::string::npos)
        << scene_en[kChiwaCastSceneIdx];
    EXPECT_NE(low5.find("taiga"), std::string::npos)
        << scene_en[kChiwaCastSceneIdx];
    const std::string low7 = to_ascii_lower(scene_en[kSolNachtSceneIdx]);
    EXPECT_NE(low7.find("sol"), std::string::npos)
        << scene_en[kSolNachtSceneIdx];
    EXPECT_NE(low7.find("nacht"), std::string::npos)
        << scene_en[kSolNachtSceneIdx];
    EXPECT_NE(to_ascii_lower(scene_en[kSakuraHaruSceneIdx]).find("haru"),
              std::string::npos)
        << scene_en[kSakuraHaruSceneIdx];

    // Verify CP932 cleanliness, 1-tab structure, nameplate keys, and trimmed
    // U+3000 narration keys in translation_table.tsv.
    const Bytes pass1_cache_bytes = read_file(cache_path.u8string());
    const Bytes pass1_tsv_bytes = read_file(tsv_path.u8string());
    ASSERT_FALSE(pass1_tsv_bytes.empty());
    // Note: translation_table.tsv is UTF-8 on disk; verify every row encodes
    // cleanly into CP932 without '?' replacement on its Japanese & English sides.
    const std::string tsv_text(
        reinterpret_cast<const char*>(pass1_tsv_bytes.data()), pass1_tsv_bytes.size());

    std::size_t non_empty_rows = 0;
    std::size_t pos = 0;
    while (pos < tsv_text.size()) {
        const std::size_t nl = tsv_text.find('\n', pos);
        std::string row = tsv_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? tsv_text.size() : nl + kNewlineCharBytes;
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (row.empty()) {
            continue;
        }
        ++non_empty_rows;
        ASSERT_EQ(std::count(row.begin(), row.end(), '\t'), kExpectedTabsPerRow) << row;
        const Bytes row_cp932 = utf8_to_cp932_replace(row);
        const auto row_roundtrip = cp932_to_utf8_strict(row_cp932);
        ASSERT_TRUE(row_roundtrip.has_value()) << "Row failed CP932 decode: " << row;
        EXPECT_EQ(*row_roundtrip, row) << "Row lost characters in CP932 round-trip: " << row;
    }
    EXPECT_GE(non_empty_rows, std::size(kSceneLines) + kSpeakerPlateCount);
    EXPECT_NE(tsv_text.find("クロ\tKuro\n"), std::string::npos);
    EXPECT_NE(tsv_text.find("ハル\tHaru\n"), std::string::npos);
    EXPECT_NE(tsv_text.find("千和\tChiwa\n"), std::string::npos);
    EXPECT_NE(tsv_text.find("大雅\tTaiga\n"), std::string::npos);
    EXPECT_NE(
        tsv_text.find(build_tsv::flatten(kLine1NarrationRubyJp) + "\t" +
                      scene_en[kNarrationRubySceneIdx]),
        std::string::npos);

    // Step 3 (Pass 2): Immediate resume run must make 0 API calls and preserve
    // cache + TSV idempotently.
    const CapturedProcessResult pass2 = run_process_captured(
        translate_exe,
        {
            "--dir", project_dir.u8string(),
            "--game-dir", game_dir.u8string(),
            "--batch", "8",
            "--max-batches", "1",
        });

    ASSERT_EQ(pass2.exit_code, kSuccessExitCode)
        << "02_translate.exe Pass 2 resume failed:\n" << pass2.output;
    EXPECT_EQ(pass2.output.find("[ERROR]"), std::string::npos)
        << "Unexpected [ERROR] in Pass 2 output:\n" << pass2.output;
    EXPECT_NE(pass2.output.find("uncached: 0"), std::string::npos) << pass2.output;
    EXPECT_NE(pass2.output.find("Everything is cached -- skipping API calls."),
              std::string::npos) << pass2.output;
    EXPECT_EQ(pass2.output.find("Tokens: in="), std::string::npos)
        << "Pass 2 resume unexpectedly invoked Gemini API:\n" << pass2.output;
    EXPECT_FALSE(fs::exists(telemetry_file))
        << "Pass 2 resume must make 0 API calls (no batch telemetry file written)";
    EXPECT_EQ(read_file(cache_path.u8string()), pass1_cache_bytes);
    EXPECT_EQ(read_file(tsv_path.u8string()), pass1_tsv_bytes);
}
