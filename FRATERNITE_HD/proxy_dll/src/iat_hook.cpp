// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "iat_hook.h"
#include "log.h"
#include <cstring>

void* IATHook(HMODULE module, const char* dllName, const char* funcName, void* hookFunc) {
    if (!module) module = GetModuleHandleW(nullptr);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0) return nullptr;

    auto* importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<BYTE*>(module) + importDir.VirtualAddress);

    for (; importDesc->Name != 0; importDesc++) {
        auto* name = reinterpret_cast<const char*>(
            reinterpret_cast<BYTE*>(module) + importDesc->Name);

        if (_stricmp(name, dllName) != 0) continue;

        // Walk the ILT (OriginalFirstThunk) and IAT (FirstThunk) in parallel
        auto* ilt = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + importDesc->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + importDesc->FirstThunk);

        for (; ilt->u1.AddressOfData != 0; ilt++, iat++) {
            // Skip ordinal imports
            if (IMAGE_SNAP_BY_ORDINAL(ilt->u1.Ordinal)) continue;

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                reinterpret_cast<BYTE*>(module) + ilt->u1.AddressOfData);

            if (strcmp(importByName->Name, funcName) != 0) continue;

            // Found it — replace IAT entry
            void* original = reinterpret_cast<void*>(iat->u1.Function);

            DWORD oldProtect;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect);
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(hookFunc);
            VirtualProtect(&iat->u1.Function, sizeof(void*), oldProtect, &oldProtect);

            Log("IAT Hook: %s!%s  orig=%p -> hook=%p", dllName, funcName, original, hookFunc);
            return original;
        }
    }

    Log("IAT Hook: FAILED to find %s!%s", dllName, funcName);
    return nullptr;
}
