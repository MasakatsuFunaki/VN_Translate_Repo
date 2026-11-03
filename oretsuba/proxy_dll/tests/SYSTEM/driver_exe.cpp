// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "runtime_hooks.h"
#include "translator_logic.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <gtest/gtest.h>

namespace {

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
            // Replicate 0x00455b49: mov byte ptr [ebx], 0
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

using InstallTestHooksFn = int(__cdecl*)(const wchar_t*, void*, void*);
using CleanupTestHooksFn = void(__cdecl*)();

TEST(SystemHookDriverTest, EndToEndInlineTrampolineAndWinmmForwarding) {
    wchar_t exe_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    ASSERT_NE(slash, nullptr);
    *(slash + 1) = L'\0';

    // Write temporary translation_table.tsv
    wchar_t tsv_path[MAX_PATH] = {};
    wcscpy_s(tsv_path, exe_dir);
    wcscat_s(tsv_path, L"system_test_table.tsv");
    {
        std::ofstream ofs(tsv_path, std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        ofs << "タカシ\tTakashi\n"
            << "明日香\tAsuka\n"
            << "「ここはグレタガルドなのか？」\t\"Is this really Gretagard?\"\n"
            << "冬の冷たい風が柳原の街を吹き抜けていった。\t"
               "A cold winter wind swept right through the streets of Yanagihara.\n"
            << "声をかける\tCall out to her\n";
    }

    // Load oretsuba_test_hook.dll and install inline hooks on our simulated __thiscall functions
    wchar_t hook_dll_path[MAX_PATH] = {};
    wcscpy_s(hook_dll_path, exe_dir);
    wcscat_s(hook_dll_path, L"oretsuba_test_hook.dll");
    HMODULE hook_dll = LoadLibraryW(hook_dll_path);
    ASSERT_NE(hook_dll, nullptr);

    auto install_fn = reinterpret_cast<InstallTestHooksFn>(
        GetProcAddress(hook_dll, "InstallTestHooks"));
    auto cleanup_fn = reinterpret_cast<CleanupTestHooksFn>(
        GetProcAddress(hook_dll, "CleanupTestHooks"));
    ASSERT_NE(install_fn, nullptr);
    ASSERT_NE(cleanup_fn, nullptr);

    void* real_parse_msg = UnwrapJmpThunk(
        reinterpret_cast<void*>(&SimulatedParseMessageHeaderAndBody));
    void* real_append_choice = UnwrapJmpThunk(
        reinterpret_cast<void*>(&SimulatedAppendChoiceString));

    const int loaded = install_fn(tsv_path, real_parse_msg, real_append_choice);
    EXPECT_EQ(loaded, 5);

    // 1. Test speaker + voice tag + dialogue body with in-place '$' -> '\0' mutation
    const std::string immutable_sob_dialogue =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "#タカシ\\00_01_001$&$「ここはグレタガルドなのか？」$");
    std::vector<char> sob_copy(
        immutable_sob_dialogue.begin(), immutable_sob_dialogue.end());
    sob_copy.push_back('\0');

    SimulatedParseMessageHeaderAndBody(&g_state, nullptr, sob_copy.data());
    EXPECT_EQ(g_state.header, "Takashi\\00_01_001");
    EXPECT_EQ(g_state.body, "\"Is this really Gretagard?\"");
    // Original SOB buffer must remain 100% untouched by 0x00455b49's '$' -> '\0' write!
    EXPECT_EQ(std::string(sob_copy.data()), immutable_sob_dialogue);

    // 2. Test narration wrapping with '\' at 56 columns
    const std::string immutable_sob_narration =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "　冬の冷たい風が柳原の街を吹き抜けていった。$");
    std::vector<char> narr_copy(
        immutable_sob_narration.begin(), immutable_sob_narration.end());
    narr_copy.push_back('\0');

    SimulatedParseMessageHeaderAndBody(&g_state, nullptr, narr_copy.data());
    EXPECT_TRUE(g_state.header.empty());
    EXPECT_NE(g_state.body.find('\\'), std::string::npos);

    // 3. Test choice string hook
    const std::string jp_choice =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932("声をかける");
    SimulatedAppendChoiceString(&g_state, nullptr, jp_choice.c_str());
    ASSERT_EQ(g_state.choices.size(), 1U);
    EXPECT_EQ(g_state.choices[0], "Call out to her");

    cleanup_fn();
    FreeLibrary(hook_dll);
    DeleteFileW(tsv_path);
}

}  // namespace
