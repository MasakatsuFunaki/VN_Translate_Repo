// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// dllmain.cpp — entry point for the version.dll proxy.
//
// sakuranotoki.exe is a CrackProof launcher: a 704KB on-disk EXE that
// decrypts the ~5MB Artemis engine into sakuranotoki.log at runtime,
// memory-mapped INSIDE the EXE's image range. The launcher imports
// version.dll (verified via dumpbin), and version.dll is not in
// HKLM\...\Session Manager\KnownDLLs, so DLL search order resolves
// the EXE folder before System32 — our proxy version.dll is loaded.
//
// proxy.cpp's naked-jmp stubs forward all 17 exports to System32's
// version.dll (resolved at ProxyInit), so no version_orig.dll alongside
// us is needed (same pattern FAVORITE uses for winmm.dll).
//
// Translation strategy: a memory scanner (memory_scanner.cpp) walks
// committed-writable memory every 50 ms and overwrites JP-bearing TSV
// keys in place with the EN translation + a NUL terminator. No code
// patching => no anti-tamper risk. Earlier iterations tried inline
// hook + VEH+HW-BP at FUN_0064b7e0 and 16 sibling candidates — none
// caught the dialogue path. The memory scanner does, by side-stepping
// the function-finding problem entirely.
//
// One-time diagnostic at startup: engine_scout.cpp dumps the runtime
// image to sakuranotoki/analysys/sakuranotoki_runtime.bin (skipped
// when the dump file already exists in the game folder, which it
// usually doesn't after the move out — handled by the scout).

#include "engine_scout.h"
#include "function_hook.h"
#include "iat_translator.h"
#include "log.h"
#include "memory_scanner.h"
#include "page_guard.h"
#include "proxy.h"
#include "text_finder.h"
#include "translator.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace {

constexpr DWORD kUnpackWaitMs = 1500U;
constexpr std::size_t kInitialModulePathChars = MAX_PATH;
constexpr std::size_t kMaxModulePathChars = 32768;
constexpr std::size_t kPathGrowthFactor = 2;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

private:
    HANDLE handle_ = nullptr;
};

std::wstring query_module_path(HMODULE mod) {
    std::size_t cap = kInitialModulePathChars;
    while (cap <= kMaxModulePathChars) {
        std::vector<wchar_t> buf(cap, L'\0');
        const DWORD len = GetModuleFileNameW(
            mod, buf.data(), static_cast<DWORD>(cap));
        if (len == 0) {
            return {};
        }
        if (len < cap) {
            return std::wstring(buf.data(), len);
        }
        if (cap == kMaxModulePathChars) {
            return {};
        }
        cap = std::min(cap * kPathGrowthFactor, kMaxModulePathChars);
    }
    return {};
}

std::wstring build_sibling_log_path(HMODULE mod) {
    const std::wstring mod_path = query_module_path(mod);
    if (mod_path.empty()) {
        return L"sakuranotoki_proxy.log";
    }
    const std::size_t sep = mod_path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return L"sakuranotoki_proxy.log";
    }
    return mod_path.substr(0, sep + 1) + L"sakuranotoki_proxy.log";
}

DWORD WINAPI worker_thread(LPVOID) {
    // Pin: load ourselves a second time. Bumps refcount so any
    // FreeLibrary("version.dll") CrackProof's unpacker performs
    // CANNOT unload us. Done from a worker thread (not DllMain)
    // because LoadLibrary inside DllMain hits the loader lock.
    const HMODULE pin = LoadLibraryW(L"version.dll");
    const DWORD pin_err = pin != nullptr ? 0U : GetLastError();
    LOG("worker: pinned via LoadLibraryW -> %p (gle=%lu)", pin, pin_err);

    // Start the scanner FIRST so it can race the engine's first render.
    // Earlier ordering (Sleep(1500) + ScoutEngine + scanner) put the
    // scanner's first scan ~1.6s after attach — by then the engine had
    // already rendered the first dialogue line ("「いらっしゃい」")
    // from JP bytes, leaving the user's first screen untranslated.
    // Spawning here means the scanner loops over memory while CrackProof
    // is still unpacking; once the script blob is decrypted into RW
    // memory, the next 100 ms tick patches it before the engine gets
    // there. Empty early scans are cheap (region_is_scannable rejects
    // PAGE_NOACCESS / PAGE_GUARD pages).
    start_memory_scanner();

    // Wait for CrackProof to finish unpacking before scouting (the
    // scout dumps the runtime image; doing it before unpacking would
    // capture encrypted bytes).
    Sleep(kUnpackWaitMs);
    ScoutEngine();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(inst);

            const std::wstring log_path = build_sibling_log_path(inst);
            log_init(log_path.c_str());
            LOG("PROCESS_ATTACH inst=%p", inst);

            ProxyInit();
            tr_runtime::init();
            // Install IAT hooks IMMEDIATELY at PROCESS_ATTACH (before
            // CrackProof unpacks). The hook entries get rewritten by the
            // loader anyway when the EXE's imports resolve, so installing
            // them here means our hook is in place from the very first
            // engine call. Anti-tamper-safe: IAT lives in .idata, not
            // .text.
            start_iat_translator();
            // text_finder (HW data BPs) and page_guard (PAGE_GUARD)
            // remain disabled — they didn't catch the engine read or
            // crashed the heap. function_hook is RE-ENABLED with new
            // candidates from FindStringConsumers.java which targets
            // functions that read std::string size+ptr (the actual
            // text-consumer signature, not just byte-iteration).
            //
            // The new candidates are ranked by xref count: heavy use
            // = likely on the dialogue rendering path.
            start_function_hook();
            const ScopedHandle worker(
                CreateThread(nullptr, 0, worker_thread, nullptr, 0, nullptr));
            break;
        }
        case DLL_PROCESS_DETACH:
            log_scanner_summary();
            log_iat_translator_summary();
            log_function_hook_summary();
            LOG("PROCESS_DETACH");
            ProxyShutdown();
            log_close();
            break;
    }
    return TRUE;
}
