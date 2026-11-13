// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "runtime_hooks.h"
#include "translator_logic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr DWORD kInitialModulePathChars = MAX_PATH;
constexpr DWORD kMaxModulePathChars = 32768U;
constexpr std::size_t kMinExpectedProductionTsvEntries = 400U;

using InstallTestHooksFn = int(__cdecl*)(const wchar_t*, void*, void*);
using CleanupTestHooksFn = void(__cdecl*)();

class ScopedTempFileW {
public:
    explicit ScopedTempFileW(std::wstring path) : path_(std::move(path)) {}
    ~ScopedTempFileW() {
        if (!path_.empty()) {
            ::DeleteFileW(path_.c_str());
        }
    }
    ScopedTempFileW(const ScopedTempFileW&) = delete;
    ScopedTempFileW& operator=(const ScopedTempFileW&) = delete;

    const wchar_t* c_str() const noexcept { return path_.c_str(); }

private:
    std::wstring path_;
};

class ScopedLibrary {
public:
    explicit ScopedLibrary(HMODULE module) noexcept : module_(module) {}
    ~ScopedLibrary() {
        if (module_ != nullptr) {
            ::FreeLibrary(module_);
        }
    }
    ScopedLibrary(const ScopedLibrary&) = delete;
    ScopedLibrary& operator=(const ScopedLibrary&) = delete;

    HMODULE get() const noexcept { return module_; }

private:
    HMODULE module_ = nullptr;
};

class ScopedHookCleanup {
public:
    explicit ScopedHookCleanup(CleanupTestHooksFn fn) noexcept : fn_(fn) {}
    ~ScopedHookCleanup() {
        if (fn_ != nullptr) {
            fn_();
        }
    }
    ScopedHookCleanup(const ScopedHookCleanup&) = delete;
    ScopedHookCleanup& operator=(const ScopedHookCleanup&) = delete;

private:
    CleanupTestHooksFn fn_ = nullptr;
};

std::wstring GetExecutableDirectoryW() {
    std::vector<wchar_t> buf(kInitialModulePathChars, L'\0');
    DWORD len = 0;
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1U && ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        if (buf.size() >= kMaxModulePathChars) {
            return {};
        }
        const std::size_t next_size =
            std::min<std::size_t>(buf.size() * 2U, kMaxModulePathChars);
        buf.assign(next_size, L'\0');
    }
    std::wstring full_path(buf.data(), len);
    const std::size_t sep = full_path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return {};
    }
    full_path.resize(sep + 1U);
    return full_path;
}

struct CapturedMessageState {
    std::string header;
    std::string body;
    std::vector<std::string> choices;
};

CapturedMessageState g_state;

#pragma optimize("", off)
__declspec(noinline) int __fastcall SimulatedParseMessageHeaderAndBody(
    void* this_ptr,
    void* /*edx*/,
    char* raw_str) {
    __asm {
        nop
        nop
        nop
        nop
        nop
        nop
    }
    auto* state = static_cast<CapturedMessageState*>(this_ptr);
    if (!raw_str) {
        return 0;
    }
    if (raw_str[0] == '#') {
        char* first_dollar = std::strchr(raw_str, '$');
        if (first_dollar) {
            *first_dollar = '\0';
            state->header = raw_str + 1;
            char* body_start = first_dollar + 1;
            if (body_start[0] == '&' && body_start[1] == '$') {
                body_start += 2;
            }
            state->body = body_start;
            if (!state->body.empty() && state->body.back() == '$') {
                state->body.pop_back();
            }
            return 1;
        }
    }
    state->header.clear();
    state->body = raw_str;
    if (!state->body.empty() && state->body.back() == '$') {
        state->body.pop_back();
    }
    return 1;
}

__declspec(noinline) int __fastcall SimulatedAppendChoiceString(
    void* this_ptr,
    void* /*edx*/,
    const char* choice_str) {
    __asm {
        nop
        nop
        nop
        nop
        nop
        nop
    }
    auto* state = static_cast<CapturedMessageState*>(this_ptr);
    if (choice_str) {
        state->choices.emplace_back(choice_str);
    }
    return 1;
}
#pragma optimize("", on)

std::uint8_t* UnwrapJmpThunk(void* fn) noexcept {
    auto* p = static_cast<std::uint8_t*>(fn);
    if (p[0] == 0xE9U) {
        std::int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        return p + 5 + rel;
    }
    return p;
}

TEST(SystemHookDriverTest, EndToEndInlineTrampolineAndWinmmForwarding) {
    g_state = CapturedMessageState{};
    const std::wstring exe_dir = GetExecutableDirectoryW();
    ASSERT_FALSE(exe_dir.empty());

    const ScopedTempFileW tsv_guard(exe_dir + L"system_test_table.tsv");
    {
        std::ofstream ofs(tsv_guard.c_str(), std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        ofs << "タカシ\tTakashi\n"
            << "明日香\tAsuka\n"
            << "「ここはグレタガルドなのか？」\t\"Is this really Gretagard?\"\n"
            << "冬の冷たい風が柳原の街を吹き抜けていった。\t"
               "A cold winter wind swept right through the streets of Yanagihara.\n"
            << "声をかける\tCall out to her\n";
    }

    const std::wstring hook_dll_path = exe_dir + L"oretsuba_test_hook.dll";
    const ScopedLibrary dll_guard(::LoadLibraryW(hook_dll_path.c_str()));
    ASSERT_NE(dll_guard.get(), nullptr);

    auto install_fn = reinterpret_cast<InstallTestHooksFn>(
        ::GetProcAddress(dll_guard.get(), "InstallTestHooks"));
    auto cleanup_fn = reinterpret_cast<CleanupTestHooksFn>(
        ::GetProcAddress(dll_guard.get(), "CleanupTestHooks"));
    ASSERT_NE(install_fn, nullptr);
    ASSERT_NE(cleanup_fn, nullptr);

    void* real_parse_msg = UnwrapJmpThunk(
        reinterpret_cast<void*>(&SimulatedParseMessageHeaderAndBody));
    void* real_append_choice = UnwrapJmpThunk(
        reinterpret_cast<void*>(&SimulatedAppendChoiceString));

    const int loaded = install_fn(tsv_guard.c_str(), real_parse_msg, real_append_choice);
    const ScopedHookCleanup hook_guard(cleanup_fn);
    EXPECT_EQ(loaded, 5);

    const std::string immutable_sob_dialogue =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "#タカシ\\00_01_001$&$「ここはグレタガルドなのか？」$");
    std::vector<char> sob_copy(
        immutable_sob_dialogue.begin(), immutable_sob_dialogue.end());
    sob_copy.push_back('\0');

    SimulatedParseMessageHeaderAndBody(&g_state, nullptr, sob_copy.data());
    EXPECT_EQ(g_state.header, "Takashi\\00_01_001");
    EXPECT_EQ(g_state.body, "\"Is this really Gretagard?\"");
    EXPECT_EQ(std::string(sob_copy.data()), immutable_sob_dialogue);

    const std::string immutable_sob_narration =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "　冬の冷たい風が柳原の街を吹き抜けていった。$");
    std::vector<char> narr_copy(
        immutable_sob_narration.begin(), immutable_sob_narration.end());
    narr_copy.push_back('\0');

    SimulatedParseMessageHeaderAndBody(&g_state, nullptr, narr_copy.data());
    EXPECT_TRUE(g_state.header.empty());
    EXPECT_NE(g_state.body.find('\\'), std::string::npos);

    const std::string jp_choice =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932("声をかける");
    SimulatedAppendChoiceString(&g_state, nullptr, jp_choice.c_str());
    ASSERT_EQ(g_state.choices.size(), 1U);
    EXPECT_EQ(g_state.choices[0], "Call out to her");
}

TEST(SystemHookDriverTest, LoadsProductionTsvWhenAvailableAndTranslatesCp932Records) {
    const fs::path tsv_path =
        fs::u8path(ORETSUBA_ROOT_DIR) / "build" / "install" / "game" / "translation_table.tsv";
    if (!fs::is_regular_file(tsv_path)) {
        GTEST_SKIP() << "Production translation_table.tsv not staged at "
                     << tsv_path.u8string();
    }

    oretsuba::proxy::TranslationCatalog catalog;
    const std::size_t count = catalog.LoadFromFile(tsv_path.u8string().c_str());
    EXPECT_GT(count, kMinExpectedProductionTsvEntries);

    std::ifstream ifs(tsv_path, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());

    std::string line;
    std::size_t verified = 0;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string jp_utf8 = line.substr(0, tab);
        const std::string jp_cp932 = oretsuba::proxy::TranslationCatalog::Utf8ToCp932(jp_utf8);
        ASSERT_FALSE(jp_cp932.empty()) << "Failed CP932 conversion for: " << jp_utf8;

        const auto hit = catalog.LookupTranslation(jp_cp932);
        ASSERT_TRUE(hit.has_value()) << "Failed CP932 lookup for: " << jp_utf8;
        EXPECT_FALSE(hit->empty());
        EXPECT_EQ(hit->find('\t'), std::string::npos) << "Corrupted tab in: " << *hit;
        EXPECT_EQ(hit->find("\\ow."), std::string::npos) << "Corrupted \\now in: " << *hit;

        const std::string msg_record =
            oretsuba::proxy::TranslationCatalog::Utf8ToCp932("#タカシ\\01_01_001$&$") +
            jp_cp932 + "$";
        const std::string formatted = catalog.TranslateMessageRecord(msg_record);
        EXPECT_EQ(formatted.rfind("#Takashi\\01_01_001$&$", 0), 0U);
        EXPECT_EQ(formatted.back(), '$');
        EXPECT_EQ(formatted.find('\t'), std::string::npos)
            << "Corrupted tab in formatted: " << formatted;
        EXPECT_EQ(formatted.find("\\ow."), std::string::npos)
            << "Corrupted \\now in formatted: " << formatted;
        ++verified;
    }
    EXPECT_EQ(verified, count);
}

}  // namespace
