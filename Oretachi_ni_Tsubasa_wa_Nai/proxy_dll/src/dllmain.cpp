// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "log.h"
#include "proxy.h"
#include "runtime_hooks.h"
#include "translator_logic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr DWORD kInitialModulePathChars = MAX_PATH;
constexpr DWORD kMaxModulePathChars = 32768U;

std::unique_ptr<oretsuba::proxy::TranslationCatalog> g_catalog;
std::unique_ptr<oretsuba::proxy::RuntimeHookManager> g_hook_mgr;

std::wstring ResolveModuleDirectoryW(HMODULE module) {
    std::vector<wchar_t> buf(kInitialModulePathChars, L'\0');
    DWORD len = 0;
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        len = ::GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
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

    std::wstring module_dir(buf.data(), len);
    const std::size_t sep = module_dir.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return {};
    }
    module_dir.resize(sep + 1U);
    return module_dir;
}

void InitializeOretsubaTranslation(HMODULE hModule) {
    const std::wstring dll_dir = ResolveModuleDirectoryW(hModule);
    const std::wstring log_path = dll_dir + L"vn_translate_log.txt";
    oretsuba::proxy::LogInit(log_path.c_str());

    oretsuba::proxy::ProxyInit();

    const std::wstring tsv_path = dll_dir + L"translation_table.tsv";

    g_catalog = std::make_unique<oretsuba::proxy::TranslationCatalog>();
    const std::size_t loaded_count = g_catalog->LoadFromFile(tsv_path.c_str());
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
