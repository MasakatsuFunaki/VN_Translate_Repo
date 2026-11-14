// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Hermetic component tests for Adieu proxy_dll's public TranslationRuntimeFacade:
// UTF-8 BOM TSV file loading from Unicode paths, multi-step SiglusEngine scene
// string post-hook processing, #NAMAE and ASCII asset bypasses, 640x480 NVL
// word-wrapping vs. single-line choice mode, SSO/heap output buffer mutation,
// lookup idempotence, and reload/rejection state transitions.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using translator_logic::kDefaultDialogueCols;
using translator_logic::kDefaultNarrationCols;
using translator_logic::RuntimeStats;
using translator_logic::TranslationRuntimeFacade;

constexpr std::size_t kExpectedLoadedEntries = 6u;
constexpr uint32_t kSsoCapacity = 7u;
constexpr uint32_t kHeapThreshold = 8u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        const std::string folder =
            "adi_ct_日本語_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(folder);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
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

// Simulates SiglusEngine.exe's 24-byte release std::wstring layout across SSO
// (res == 7) and heap (res >= 8) storage transitions.
class SimulatedSiglusWstring {
public:
    SimulatedSiglusWstring() { ResetToSso(L""); }

    explicit SimulatedSiglusWstring(std::wstring_view initial) {
        Assign(initial);
    }

    void Assign(std::wstring_view value) {
        if (value.size() <= kSsoCapacity) {
            ResetToSso(value);
            return;
        }
        heap_.assign(value.begin(), value.end());
        heap_.push_back(L'\0');
        raw_.storage.ptr = heap_.data();
        raw_.size = static_cast<uint32_t>(value.size());
        raw_.res = static_cast<uint32_t>(heap_.size() - 1u);
    }

    std::wstring_view View() const noexcept {
        const wchar_t* data =
            (raw_.res >= kHeapThreshold) ? raw_.storage.ptr : raw_.storage.buf;
        return std::wstring_view(data, raw_.size);
    }

    bool UsesHeap() const noexcept { return raw_.res >= kHeapThreshold; }

private:
    struct RawLayout {
        union Storage {
            wchar_t buf[8];
            wchar_t* ptr;
        } storage;
        uint32_t size;
        uint32_t res;
    };
    static_assert(sizeof(RawLayout) == 24u,
                  "Simulated layout must match 24-byte SiglusEngine ABI");

    void ResetToSso(std::wstring_view value) {
        heap_.clear();
        std::memset(&raw_, 0, sizeof(raw_));
        if (!value.empty()) {
            std::memcpy(raw_.storage.buf, value.data(),
                        value.size() * sizeof(wchar_t));
        }
        raw_.storage.buf[value.size()] = L'\0';
        raw_.size = static_cast<uint32_t>(value.size());
        raw_.res = kSsoCapacity;
    }

    RawLayout raw_{};
    std::vector<wchar_t> heap_;
};

void ExpectEveryLineAtMost(std::wstring_view text, std::size_t max_cols) {
    std::size_t start = 0u;
    while (start <= text.size()) {
        const std::size_t nl = text.find(L'\n', start);
        const std::size_t end =
            (nl == std::wstring_view::npos) ? text.size() : nl;
        EXPECT_LE(end - start, max_cols);
        if (nl == std::wstring_view::npos) {
            break;
        }
        start = nl + 1u;
    }
}

}  // namespace

TEST(TranslationPipeline,
     TranslatesRealisticSiglusSceneSequenceThroughPostHookBuffer) {
    ScopedTempDir sandbox;
    const fs::path tsv_path = sandbox.path() / "translation_table.tsv";
    {
        std::ofstream out(tsv_path, std::ios::binary);
        out << "\xEF\xBB\xBF";
        out << "人見\tHitomi\r\n";
        out << "人見\tWrongDuplicateHitomi\r\n";
        out << "広介\tKousuke\r\n";
        out << "睦　月\tMutsuki\r\n";
        out << "　保健室の白い天井を見つめていた。セミの声が遠くで鳴り続けている。\t"
               "I stared up at the white infirmary ceiling while cicadas kept "
               "crying in the distance.\r\n";
        out << "「先生、こんなところで寝ていたら夏風邪をひいてしまいますよ」\t"
               "\"Sensei, if you fall asleep in a place like this, you'll end "
               "up catching a summer cold.\"\r\n";
        out << "保健室へ行く\tHead straight back to the school infirmary before "
               "homeroom starts\r\n";
        out << "malformed_row_without_tab\r\n";
    }

    TranslationRuntimeFacade runtime;
    ASSERT_TRUE(runtime.LoadTsvFile(tsv_path));
    ASSERT_EQ(runtime.EntryCount(), kExpectedLoadedEntries);

    SimulatedSiglusWstring engine_slot;
    auto bind_slot = [&engine_slot](std::wstring_view replacement) {
        engine_slot.Assign(replacement);
    };

    // 1. Protagonist surname #NAMAE macro key must bypass untouched.
    engine_slot.Assign(L"＊Ａ");
    EXPECT_FALSE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), L"＊Ａ");

    // 2. Default surname value translates in SSO storage (first-wins).
    engine_slot.Assign(L"人見");
    EXPECT_TRUE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), L"Hitomi");
    EXPECT_FALSE(engine_slot.UsesHeap());

    // 3. Protagonist given-name #NAMAE macro key must bypass untouched.
    engine_slot.Assign(L"＊Ｂ");
    EXPECT_FALSE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), L"＊Ｂ");

    // 4. Default given-name value translates cleanly.
    engine_slot.Assign(L"広介");
    EXPECT_TRUE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), L"Kousuke");

    // 5. ASCII .g00 asset identifier must bypass untouched.
    engine_slot.Assign(L"BG_INF01");
    EXPECT_FALSE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), L"BG_INF01");

    // 6. Long NVL narration wraps at <= 56 cols with real U+000A newlines and
    // transitions the output buffer into heap capacity.
    engine_slot.Assign(
        L"　保健室の白い天井を見つめていた。セミの声が遠くで鳴り続けている。");
    EXPECT_TRUE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_TRUE(engine_slot.UsesHeap());
    EXPECT_NE(engine_slot.View().find(L'\n'), std::wstring_view::npos);
    EXPECT_EQ(engine_slot.View().find(L"\\n"), std::wstring_view::npos);
    ExpectEveryLineAtMost(engine_slot.View(), kDefaultNarrationCols);
    const std::wstring first_narration_pass(engine_slot.View());

    // 7. Spaced speaker plate translates and transitions buffer back to SSO.
    engine_slot.Assign(L"睦　月");
    EXPECT_TRUE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), L"Mutsuki");
    EXPECT_FALSE(engine_slot.UsesHeap());

    // 8. Spoken dialogue wraps at <= 50 cols (narrower #MWND.002 TALK_MARGIN).
    engine_slot.Assign(
        L"「先生、こんなところで寝ていたら夏風邪をひいてしまいますよ」");
    EXPECT_TRUE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_NE(engine_slot.View().find(L'\n'), std::wstring_view::npos);
    ExpectEveryLineAtMost(engine_slot.View(), kDefaultDialogueCols);

    // 9. Choice mode (`is_choice = true`) keeps single-line button text intact.
    engine_slot.Assign(L"保健室へ行く");
    EXPECT_TRUE(runtime.ApplyPostHook(engine_slot.View(), bind_slot, true));
    EXPECT_EQ(engine_slot.View(),
              L"Head straight back to the school infirmary before homeroom "
              L"starts");
    EXPECT_EQ(engine_slot.View().find(L'\n'), std::wstring_view::npos);

    // 10. Unmapped Japanese line leaves original engine buffer intact.
    engine_slot.Assign(L"未翻訳の台詞");
    EXPECT_FALSE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), L"未翻訳の台詞");

    const RuntimeStats stats = runtime.Stats();
    EXPECT_EQ(stats.total_calls, 10u);
    EXPECT_EQ(stats.bypassed_calls, 3u);
    EXPECT_EQ(stats.translated_hits, 6u);
    EXPECT_EQ(stats.lookup_misses, 1u);

    // Idempotence check: repeating the narration lookup produces identical output.
    engine_slot.Assign(
        L"　保健室の白い天井を見つめていた。セミの声が遠くで鳴り続けている。");
    ASSERT_TRUE(runtime.ApplyPostHook(engine_slot.View(), bind_slot));
    EXPECT_EQ(engine_slot.View(), first_narration_pass);
}

TEST(TranslationPipeline,
     ReloadReplacesPreviousEntriesAndRejectsMissingOrCorruptPaths) {
    ScopedTempDir sandbox;
    const fs::path first_tsv = sandbox.path() / "first.tsv";
    const fs::path second_tsv = sandbox.path() / "second.tsv";
    {
        std::ofstream out(first_tsv, std::ios::binary);
        out << "屋上\tRooftop\n";
    }
    {
        std::ofstream out(second_tsv, std::ios::binary);
        out << "図書室\tLibrary\n";
    }

    TranslationRuntimeFacade runtime;
    ASSERT_TRUE(runtime.LoadTsvUtf8Path(first_tsv.u8string()));
    std::wstring captured;
    EXPECT_TRUE(runtime.ApplyPostHook(
        L"屋上", [&captured](std::wstring_view s) { captured.assign(s); }));
    EXPECT_EQ(captured, L"Rooftop");

    // Reloading from `second_tsv` must clear stale entries from `first_tsv`.
    ASSERT_TRUE(runtime.LoadTsvUtf8Path(second_tsv.u8string()));
    EXPECT_EQ(runtime.EntryCount(), 1u);
    EXPECT_FALSE(runtime.ApplyPostHook(
        L"屋上", [&captured](std::wstring_view s) { captured.assign(s); }));
    EXPECT_TRUE(runtime.ApplyPostHook(
        L"図書室", [&captured](std::wstring_view s) { captured.assign(s); }));
    EXPECT_EQ(captured, L"Library");

    // Missing or ill-formed UTF-8 paths return false and leave the table empty.
    EXPECT_FALSE(runtime.LoadTsvUtf8Path(""));
    EXPECT_EQ(runtime.EntryCount(), 0u);
    EXPECT_FALSE(runtime.LoadTsvUtf8Path("\xFF\xFEinvalid_utf8.tsv"));
    EXPECT_EQ(runtime.EntryCount(), 0u);
    EXPECT_FALSE(runtime.LoadTsvFile(sandbox.path() / "does_not_exist.tsv"));
    EXPECT_EQ(runtime.EntryCount(), 0u);
}
