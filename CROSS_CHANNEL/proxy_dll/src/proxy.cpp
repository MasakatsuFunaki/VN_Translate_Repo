// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "proxy.h"
#include "log.h"

// XINPUT1_3.dll proxy. cc.exe imports exactly one function (XInputGetState,
// ordinal #2). We forward the 8 named exports so any other DLL that also
// imports xinput keeps working through us.

#define WINMM_FUNC_COUNT 8

static HMODULE g_realDll = nullptr;
static FARPROC g_procs[WINMM_FUNC_COUNT] = {0};

static const char* g_names[WINMM_FUNC_COUNT] = {
    "XInputGetState",                   // 0
    "XInputSetState",                   // 1
    "XInputGetCapabilities",            // 2
    "XInputEnable",                     // 3
    "XInputGetDSoundAudioDeviceGuids",  // 4
    "XInputGetBatteryInformation",      // 5
    "XInputGetKeystroke",               // 6
    nullptr,                            // (reserved slot — DllMain forwarded
                                        //  by /DEF: PRIVATE)
};

void ProxyInit() {
    // Always load from the system directory so we resolve to the real
    // XInput DLL even if some other proxy named xinput1_3.dll exists
    // earlier on the search path.
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wcscat_s(sysDir, L"\\XINPUT1_3.dll");

    g_realDll = LoadLibraryW(sysDir);
    if (!g_realDll) {
        Log("Proxy: FAILED to load real XINPUT1_3.dll from %ls (gle=%lu)",
            sysDir, GetLastError());
        return;
    }

    int resolved = 0;
    for (int i = 0; i < WINMM_FUNC_COUNT - 1; i++) {
        g_procs[i] = GetProcAddress(g_realDll, g_names[i]);
        if (g_procs[i]) resolved++;
    }
    Log("Proxy: real XINPUT1_3.dll loaded, resolved %d/%d exports",
        resolved, WINMM_FUNC_COUNT - 1);
}

void ProxyShutdown() {
    if (g_realDll) { FreeLibrary(g_realDll); g_realDll = nullptr; }
}

// Naked forwarding stubs — straight jmp through the function-pointer
// table, no prologue/epilogue. The .def file maps each export name to
// its system-DLL ordinal so cc.exe's IAT lookup hits the right slot.

#define PROXY_STUB(name, idx) \
    extern "C" __declspec(naked) void __stdcall proxy_##name() { \
        __asm { jmp dword ptr [g_procs + idx * 4] } \
    }

PROXY_STUB(XInputGetState, 0)
PROXY_STUB(XInputSetState, 1)
PROXY_STUB(XInputGetCapabilities, 2)
PROXY_STUB(XInputEnable, 3)
PROXY_STUB(XInputGetDSoundAudioDeviceGuids, 4)
PROXY_STUB(XInputGetBatteryInformation, 5)
PROXY_STUB(XInputGetKeystroke, 6)
// .def file aliases each public export name to the proxy_<name> stub
// above and pins the ordinal to the system DLL's value, so cc.exe's
// ordinal-2 import lands on proxy_XInputGetState.
