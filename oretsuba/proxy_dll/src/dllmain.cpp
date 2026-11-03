// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "log.h"
#include "proxy.h"
#include "runtime_hooks.h"
#include "translator_logic.h"

#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

std::unique_ptr<oretsuba::proxy::TranslationCatalog> g_catalog;
std::unique_ptr<oretsuba::proxy::RuntimeHookManager> g_hook_mgr;

void InitializeOretsubaTranslation(HMODULE hModule) {
    wchar_t dll_dir[MAX_PATH] = {};
    GetModuleFileNameW(hModule, dll_dir, MAX_PATH);
    wchar_t* last_slash = wcsrchr(dll_dir, L'\\');
    if (last_slash) {
        *(last_slash + 1) = L'\0';
    }

    wchar_t log_path[MAX_PATH] = {};
    wcscpy_s(log_path, dll_dir);
    wcscat_s(log_path, L"vn_translate_log.txt");
    oretsuba::proxy::LogInit(log_path);

    oretsuba::proxy::ProxyInit();

    wchar_t tsv_path[MAX_PATH] = {};
    wcscpy_s(tsv_path, dll_dir);
    wcscat_s(tsv_path, L"translation_table.tsv");

    g_catalog = std::make_unique<oretsuba::proxy::TranslationCatalog>();
    const std::size_t loaded_count = g_catalog->LoadFromFile(tsv_path);
    oretsuba::proxy::Log(
        "Loaded %zu entries from translation_table.tsv", loaded_count);

    g_hook_mgr = std::make_unique<oretsuba::proxy::RuntimeHookManager>(g_catalog.get());
    const auto exe_base =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_hook_mgr->VerifyEngineSignatures(exe_base)) {
        oretsuba::proxy::Log(
            "Host process at 0x%08X is not ORE_TUBA.exe (skipping inline patch)",
            static_cast<unsigned int>(exe_base));
        return;
    }
    g_hook_mgr->InstallHooks(exe_base);
}

void ShutdownOretsubaTranslation() {
    g_hook_mgr.reset();
    g_catalog.reset();
    oretsuba::proxy::ProxyShutdown();
    oretsuba::proxy::LogClose();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitializeOretsubaTranslation(hModule);
        return TRUE;
    }
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        ShutdownOretsubaTranslation();
        return TRUE;
    }
    return TRUE;
}
