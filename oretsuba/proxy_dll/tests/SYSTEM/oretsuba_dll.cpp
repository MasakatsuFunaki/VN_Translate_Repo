// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "runtime_hooks.h"
#include "translator_logic.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

constexpr std::uint8_t kJmpRel32Opcode = 0xE9;
constexpr std::uint8_t kNopOpcode = 0x90;
constexpr std::size_t kJmpPatchBytes = 5;
constexpr std::size_t kStolenPrologueBytes = 6;
constexpr std::size_t kTrampolineCapacity = 32;

using ParseMsgFn = int(__thiscall*)(void* this_ptr, char* raw_str);
using AppendChoiceFn = int(__thiscall*)(void* this_ptr, const char* choice_str);

std::unique_ptr<oretsuba::proxy::TranslationCatalog> g_test_catalog;
std::unique_ptr<oretsuba::proxy::RuntimeHookManager> g_test_hook_mgr;

std::uint8_t* g_parse_msg_tramp = nullptr;
std::uint8_t* g_append_choice_tramp = nullptr;
ParseMsgFn g_orig_parse_msg = nullptr;
AppendChoiceFn g_orig_append_choice = nullptr;

void WriteRel32(std::uint8_t* instr, std::uintptr_t dest) noexcept {
    const auto next_ip = reinterpret_cast<std::uintptr_t>(instr) + kJmpPatchBytes;
    const auto rel = static_cast<std::int32_t>(dest - next_ip);
    instr[0] = kJmpRel32Opcode;
    std::memcpy(instr + 1, &rel, sizeof(rel));
}

std::uint8_t* InstallTrampoline(void* target_fn, void* detour_fn, std::size_t stolen_bytes) {
    auto* tramp = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, kTrampolineCapacity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) {
        return nullptr;
    }
    auto* target = static_cast<std::uint8_t*>(target_fn);
    std::memcpy(tramp, target, stolen_bytes);
    WriteRel32(tramp + stolen_bytes, reinterpret_cast<std::uintptr_t>(target) + stolen_bytes);

    DWORD old_protect = 0;
    VirtualProtect(target, stolen_bytes, PAGE_EXECUTE_READWRITE, &old_protect);
    WriteRel32(target, reinterpret_cast<std::uintptr_t>(detour_fn));
    for (std::size_t i = kJmpPatchBytes; i < stolen_bytes; ++i) {
        target[i] = kNopOpcode;
    }
    VirtualProtect(target, stolen_bytes, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, stolen_bytes);
    return tramp;
}

int __fastcall DetourParseMsg(void* this_ptr, void* /*edx*/, char* raw_str) {
    if (g_test_hook_mgr && raw_str && raw_str[0] != '\0') {
        char* rewritten = g_test_hook_mgr->FormatIntoNextScratchBuffer(raw_str);
        return g_orig_parse_msg(this_ptr, rewritten);
    }
    return g_orig_parse_msg(this_ptr, raw_str);
}

int __fastcall DetourAppendChoice(void* this_ptr, void* /*edx*/, const char* choice_str) {
    if (g_test_catalog && choice_str) {
        if (const char* tr = g_test_catalog->LookupPersistentPointer(choice_str)) {
            return g_orig_append_choice(this_ptr, tr);
        }
    }
    return g_orig_append_choice(this_ptr, choice_str);
}

}  // namespace

extern "C" {

__declspec(dllexport) int __cdecl InstallTestHooks(
    const wchar_t* tsv_path,
    void* target_parse_msg,
    void* target_append_choice) {
    g_test_catalog = std::make_unique<oretsuba::proxy::TranslationCatalog>();
    const std::size_t loaded = g_test_catalog->LoadFromFile(tsv_path);
    if (loaded == 0U) {
        return -1;
    }
    g_test_hook_mgr =
        std::make_unique<oretsuba::proxy::RuntimeHookManager>(g_test_catalog.get());

    if (target_parse_msg) {
        g_parse_msg_tramp = InstallTrampoline(
            target_parse_msg,
            reinterpret_cast<void*>(&DetourParseMsg),
            kStolenPrologueBytes);
        g_orig_parse_msg = reinterpret_cast<ParseMsgFn>(g_parse_msg_tramp);
    }
    if (target_append_choice) {
        g_append_choice_tramp = InstallTrampoline(
            target_append_choice,
            reinterpret_cast<void*>(&DetourAppendChoice),
            kStolenPrologueBytes);
        g_orig_append_choice = reinterpret_cast<AppendChoiceFn>(g_append_choice_tramp);
    }
    return static_cast<int>(loaded);
}

__declspec(dllexport) void __cdecl CleanupTestHooks() {
    if (g_parse_msg_tramp) {
        VirtualFree(g_parse_msg_tramp, 0, MEM_RELEASE);
        g_parse_msg_tramp = nullptr;
    }
    if (g_append_choice_tramp) {
        VirtualFree(g_append_choice_tramp, 0, MEM_RELEASE);
        g_append_choice_tramp = nullptr;
    }
    g_test_hook_mgr.reset();
    g_test_catalog.reset();
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
