// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// alpharom_helper.dll — injected into KishinHisho.exe before AlphaROM stub runs.
//
// Phase 1 (this file): observe-only VEH that logs every exception with full
// register + memory context. Returns EXCEPTION_CONTINUE_SEARCH so the OS
// handles it normally (process still crashes — but now we have logs).
//
// Phase 2 will add active fixups for the specific AVs we identify.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstddef>

typedef struct _UNICODE_STRING_LOCAL {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING_LOCAL;
typedef struct _PEB_LDR_DATA32 {
    ULONG  Length;
    BYTE   Initialized[4];
    PVOID  SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA32;
typedef struct _LDR_ENTRY32 {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING_LOCAL FullDllName;
    UNICODE_STRING_LOCAL BaseDllName;
} LDR_ENTRY32;

static FILE* g_log = nullptr;
static volatile LONG g_exception_count = 0;
static HMODULE g_self = NULL;
static DWORD g_self_base = 0;
static DWORD g_self_size = 0;
static volatile LONG g_drm_patched = 0;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void LogHex(const char* label, const void* p, size_t n) {
    if (!g_log) return;
    fprintf(g_log, "  %s @ %p:", label, p);
    // Read bytes carefully — page may be unreadable.
    __try {
        const BYTE* b = (const BYTE*)p;
        for (size_t i = 0; i < n; ++i) {
            if (i % 16 == 0) fprintf(g_log, "\n    ");
            fprintf(g_log, "%02X ", b[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(g_log, " <unreadable>");
    }
    fputc('\n', g_log);
    fflush(g_log);
}

// Resolve EIP -> module+offset using PSAPI-free walk of PEB.Ldr (works in VEH).
static bool ResolveAddress(DWORD addr, char* out, size_t out_size) {
    DWORD peb = __readfsdword(0x30);
    PEB_LDR_DATA32* ldr = *(PEB_LDR_DATA32**)(peb + 0x0C);
    LIST_ENTRY* head = &ldr->InMemoryOrderModuleList;
    for (LIST_ENTRY* curr = head->Flink; curr != head; curr = curr->Flink) {
        LDR_ENTRY32* e = (LDR_ENTRY32*)((BYTE*)curr - offsetof(LDR_ENTRY32, InMemoryOrderLinks));
        DWORD base = (DWORD)e->DllBase;
        DWORD size = e->SizeOfImage;
        if (addr >= base && addr < base + size) {
            char name[64] = {0};
            for (USHORT i = 0; i < e->BaseDllName.Length/2 && i < 63; ++i) {
                name[i] = (char)e->BaseDllName.Buffer[i];
            }
            _snprintf_s(out, out_size, _TRUNCATE, "%s+0x%X", name, addr - base);
            return true;
        }
    }
    _snprintf_s(out, out_size, _TRUNCATE, "0x%08X (not in any module)", addr);
    return false;
}

// Patch recognizedisc.dll the moment it appears at its fixed base
// 0x10000000. Returns true if patched (or already patched).
static bool TryPatchRecognizedisc() {
    if (g_drm_patched) return true;
    BYTE* base = (BYTE*)0x10000000;

    // Header probe — must look like an MZ/PE before we touch anything.
    __try {
        if (*(WORD*)base != 0x5A4D) return false;
        DWORD pe_off = *(DWORD*)(base + 0x3C);
        if (pe_off >= 0x1000) return false;
        if (*(DWORD*)(base + pe_off) != 0x4550) return false;
        DWORD exp_rva = *(DWORD*)(base + pe_off + 24 + 96);
        if (!exp_rva || exp_rva > 0x20000) return false;
        DWORD name_rva = *(DWORD*)(base + exp_rva + 0x0C);
        if (!name_rva || name_rva > 0x20000) return false;
        const char* dll = (const char*)(base + name_rva);
        const char* want = "recognizedisc";
        for (int i = 0; want[i]; ++i) if (dll[i] != want[i]) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    if (InterlockedExchange(&g_drm_patched, 1) != 0) return true;

    // fnRecognizedisc @ RVA 0x1020 -> push 1; pop eax; ret; nop
    BYTE* fn = base + 0x1020;
    DWORD oldp;
    if (VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &oldp)) {
        fn[0] = 0x6A; fn[1] = 0x01; fn[2] = 0x58; fn[3] = 0xC3; fn[4] = 0x90;
        DWORD r2; VirtualProtect(fn, 5, oldp, &r2);
        Log("  >>> DRM patch applied: fnRecognizedisc @ 0x%08X -> push 1; pop eax; ret", (DWORD)fn);
    }
    DWORD* glob = (DWORD*)(base + 0xDE00);
    if (VirtualProtect(glob, 4, PAGE_READWRITE, &oldp)) {
        *glob = 1;
        DWORD r2; VirtualProtect(glob, 4, oldp, &r2);
    }
    return true;
}

// Polling thread: spins very fast for the first ~100ms (the AlphaROM stub
// init window where recognizedisc.dll gets mapped), then backs off. Exits
// once the patch is applied so we don't burn CPU forever.
static DWORD WINAPI DrmHeartbeat(LPVOID) {
    // Phase 1: tight spin (no sleep) — covers the narrow window between
    // recognizedisc.dll being mapped and fnRecognizedisc being called.
    for (int i = 0; i < 100000; ++i) {
        if (TryPatchRecognizedisc()) return 0;
    }
    // Phase 2: 1ms polling for another ~5 seconds in case the mapping is
    // delayed (game configurator dialogs, etc.).
    for (int i = 0; i < 5000; ++i) {
        if (TryPatchRecognizedisc()) return 0;
        Sleep(1);
    }
    Log("  !!! DrmHeartbeat: gave up waiting for recognizedisc.dll to appear");
    return 0;
}

static LONG NTAPI MyVEH(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* ctx = ep->ContextRecord;

    // CRITICAL: if the fault is inside OUR helper DLL (our probe reads can
    // trigger AVs), let the local __try / SEH handle it. Otherwise we'd
    // re-enter ourselves recursively.
    DWORD eip = (DWORD)er->ExceptionAddress;
    if (g_self_base && eip >= g_self_base && eip < g_self_base + g_self_size) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    LONG idx = InterlockedIncrement(&g_exception_count);

    char loc[128];
    ResolveAddress((DWORD)er->ExceptionAddress, loc, sizeof(loc));

    // Identify the EIP module by walking PEB.Ldr right now (newly-loaded
    // DLLs that didn't exist at DLL_PROCESS_ATTACH show up here).
    char eip_mod[128] = "?";
    {
        DWORD peb = __readfsdword(0x30);
        BYTE* ldr = *(BYTE**)(peb + 0x0C);
        LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x14);
        DWORD eip = (DWORD)er->ExceptionAddress;
        for (LIST_ENTRY* c = head->Flink; c != head; c = c->Flink) {
            BYTE* e = (BYTE*)c - 8;
            DWORD base = *(DWORD*)(e + 0x18);
            DWORD size = *(DWORD*)(e + 0x20);
            if (eip >= base && eip < base + size) {
                wchar_t* nm = *(wchar_t**)(e + 0x30);
                USHORT nlen = *(USHORT*)(e + 0x2C);
                char ascii[64] = {0};
                for (USHORT i = 0; i < nlen/2 && i < 63; ++i) ascii[i] = (char)nm[i];
                _snprintf_s(eip_mod, sizeof(eip_mod), _TRUNCATE,
                            "%s+0x%X", ascii, eip - base);
                break;
            }
        }
    }

    Log("=== Exception #%ld === EIP_module: %s", idx, eip_mod);
    Log("  Code:    0x%08X", er->ExceptionCode);
    Log("  Flags:   0x%08X", er->ExceptionFlags);
    Log("  Address: 0x%08X  (%s)", (DWORD)er->ExceptionAddress, loc);
    Log("  Params:  n=%u", er->NumberParameters);
    for (DWORD i = 0; i < er->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
        const char* p0note = "";
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && i == 0) {
            switch (er->ExceptionInformation[i]) {
                case 0: p0note = " (READ)"; break;
                case 1: p0note = " (WRITE)"; break;
                case 8: p0note = " (DEP/EXEC)"; break;
            }
        } else if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && i == 1) {
            char tgt[128];
            ResolveAddress((DWORD)er->ExceptionInformation[i], tgt, sizeof(tgt));
            Log("  param[%u]: 0x%p  (fault addr: %s)", i, (void*)er->ExceptionInformation[i], tgt);
            continue;
        }
        Log("  param[%u]: 0x%p%s", i, (void*)er->ExceptionInformation[i], p0note);
    }
    Log("  CONTEXT:");
    Log("    EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X",
        ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    Log("    ESI=0x%08X EDI=0x%08X EBP=0x%08X ESP=0x%08X",
        ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    Log("    EIP=0x%08X EFL=0x%08X CS=0x%04X SS=0x%04X",
        ctx->Eip, ctx->EFlags, ctx->SegCs, ctx->SegSs);

    LogHex("code at EIP", (void*)ctx->Eip, 32);
    LogHex("data at ESP", (void*)ctx->Esp, 256);
    LogHex("data near EBP-0x10", (void*)(ctx->Ebp - 0x10), 64);

    // If EIP isn't in any listed module, scan a window around it to find a
    // PE header — the unpacker likely manually-mapped a DLL into memory
    // that doesn't appear in PEB.Ldr.
    if (eip_mod[0] == '?') {
        DWORD eip = (DWORD)er->ExceptionAddress;
        // Walk back in 0x1000 steps up to ~0x100000 looking for "MZ"
        for (DWORD probe = eip & ~0xFFF; probe > (eip - 0x100000) && probe < eip; probe -= 0x1000) {
            __try {
                if (*(WORD*)probe == 0x5A4D /* "MZ" */) {
                    DWORD pe_off = *(DWORD*)(probe + 0x3C);
                    if (pe_off < 0x1000 && *(DWORD*)(probe + pe_off) == 0x4550 /* "PE\0\0" */) {
                        Log("  >>> Manually-mapped PE found at base=0x%08X (eip-0x%X)", probe, eip - probe);
                        WORD machine = *(WORD*)(probe + pe_off + 4);
                        DWORD size_of_image = *(DWORD*)(probe + pe_off + 24 + 56);
                        Log("      Machine=0x%X SizeOfImage=0x%X", machine, size_of_image);

                        // Dump exports (table is at OptionalHeader + 96 + 0)
                        DWORD exp_rva = *(DWORD*)(probe + pe_off + 24 + 96);
                        DWORD exp_size = *(DWORD*)(probe + pe_off + 24 + 100);
                        if (exp_rva && exp_rva < size_of_image) {
                            DWORD exp_dir = probe + exp_rva;
                            DWORD name_rva = *(DWORD*)(exp_dir + 0x0C);
                            if (name_rva && name_rva < size_of_image) {
                                Log("      DLL Name: %s", (char*)(probe + name_rva));
                            }
                            DWORD num_names = *(DWORD*)(exp_dir + 0x18);
                            DWORD names_rva = *(DWORD*)(exp_dir + 0x20);
                            Log("      Exports (%lu names):", num_names);
                            DWORD* names_arr = (DWORD*)(probe + names_rva);
                            for (DWORD i = 0; i < num_names && i < 32; ++i) {
                                if (names_arr[i] && names_arr[i] < size_of_image) {
                                    Log("        %s", (char*)(probe + names_arr[i]));
                                }
                            }
                        }

                        // Patch fnRecognizedisc (RVA 0x1020) to immediately return 1 (positive = success).
                        // 6A 01 58 C3 90 = push 1; pop eax; ret; nop  (5 bytes — fits over the 5-byte jmp).
                        BYTE* target = (BYTE*)(probe + 0x1020);
                        DWORD oldp;
                        if (VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldp)) {
                            target[0] = 0x6A; target[1] = 0x01; target[2] = 0x58;
                            target[3] = 0xC3; target[4] = 0x90;
                            DWORD restored;
                            VirtualProtect(target, 5, oldp, &restored);
                            Log("      >>> Patched fnRecognizedisc @ 0x%08X to push 1; pop eax; ret", (DWORD)target);
                        } else {
                            Log("      !!! Could not patch fnRecognizedisc (err %lu)", GetLastError());
                        }
                        // Also set the global nRecognizedisc (RVA 0xDE00) to 1.
                        DWORD* glob = (DWORD*)(probe + 0xDE00);
                        if (VirtualProtect(glob, 4, PAGE_READWRITE, &oldp)) {
                            *glob = 1;
                            DWORD r2;
                            VirtualProtect(glob, 4, oldp, &r2);
                            Log("      >>> Set global nRecognizedisc @ 0x%08X to 1", (DWORD)glob);
                        }

                        // Dump the mapped image to disk for offline analysis,
                        // next to the game EXE (same folder as the log).
                        char dumppath[MAX_PATH];
                        GetModuleFileNameA(NULL, dumppath, MAX_PATH);
                        char* sl = strrchr(dumppath, '\\');
                        if (sl) _snprintf_s(sl + 1, MAX_PATH - (sl + 1 - dumppath), _TRUNCATE,
                                            "mapped_pe_at_%08X.bin", probe);
                        FILE* df = fopen(dumppath, "wb");
                        if (df) {
                            // Cap dump at 256 KB to be safe
                            DWORD dump_size = size_of_image > 0x40000 ? 0x40000 : size_of_image;
                            __try {
                                fwrite((void*)probe, 1, dump_size, df);
                            } __except (EXCEPTION_EXECUTE_HANDLER) { /* truncate */ }
                            fclose(df);
                            Log("      Dumped %u bytes to %s", dump_size, dumppath);
                        }
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip */ }
        }
    }

    // FIXUP: AlphaROM's stage-2 stack thunk uses a PE walker that dereferences
    // ECX as a DLL base. ECX is computed from a buggy PEB.Ldr scan (offsets
    // hardcoded for WinXP) and ends up in kernel32's reserved alignment gap.
    //
    // Match VERY narrowly:
    //   * EIP must be inside KishinHisho.exe's *stack* range (the thunk lives
    //     on the stack — the stub built it there at runtime)
    //   * Instruction at EIP must be 66 8B 51 3C (mov dx, [ecx+0x3C])
    //   * Fault address must lie in kernel32's image range (so we know we're
    //     dealing with the PE walker, not some other random AV)
    //
    // Other AVs are passed through — the stub uses deliberate AVs (writes to
    // NULL, BOUND traps) for anti-debug. Those have stub-installed SEH that
    // expects to catch them.
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
        && er->NumberParameters >= 2
        && er->ExceptionInformation[0] == 0 /* READ */)
    {
        BYTE* eip = (BYTE*)ctx->Eip;

        // Is EIP in stack memory? Check the TEB's stack range.
        NT_TIB* tib = (NT_TIB*)__readfsdword(0x18);
        bool eip_in_stack = (DWORD)eip >= (DWORD)tib->StackLimit
                            && (DWORD)eip <  (DWORD)tib->StackBase;

        // Is the fault address inside kernel32's range?
        DWORD fault = (DWORD)er->ExceptionInformation[1];
        DWORD peb = __readfsdword(0x30);
        BYTE* ldr = *(BYTE**)(peb + 0x0C);
        LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x14);
        DWORD k32_base = 0, k32_size = 0;
        for (LIST_ENTRY* c = head->Flink; c != head; c = c->Flink) {
            BYTE* e = (BYTE*)c - 8;
            wchar_t* nm = *(wchar_t**)(e + 0x30);
            USHORT len = *(USHORT*)(e + 0x2C);
            if (nm && len == 24) {
                bool match = true;
                const wchar_t* want = L"kernel32.dll";
                for (int i = 0; i < 12; ++i) {
                    wchar_t a = nm[i]; if (a >= L'A' && a <= L'Z') a += 32;
                    if (a != want[i]) { match = false; break; }
                }
                if (match) {
                    k32_base = *(DWORD*)(e + 0x18);
                    k32_size = *(DWORD*)(e + 0x20);
                    break;
                }
            }
        }
        bool fault_in_k32 = k32_base && fault >= k32_base && fault < k32_base + k32_size;

        bool insn_match = eip_in_stack
                          && IsBadReadPtr(eip, 4) == FALSE
                          && eip[0] == 0x66 && eip[1] == 0x8B
                          && eip[2] == 0x51 && eip[3] == 0x3C;

        if (insn_match && fault_in_k32) {
            Log("  >>> FIXUP: stack-thunk PE walker, ECX=0x%08X bad. Redirecting ECX -> kernel32 base 0x%08X",
                ctx->Ecx, k32_base);
            ctx->Ecx = k32_base;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // FIXUP #2: AV inside the manually-mapped DRM check DLL
        // (recognizedisc.dll at base 0x10000000). Walk the EBP frame chain:
        // each [EBP+4] is the next return address, [EBP] is next EBP.
        // Find the first frame whose return address is OUTSIDE the DRM
        // module — that's the AlphaROM stub's call site. Unwind there
        // with EAX=0 (fnRecognizedisc returned success).
        const DWORD DRM_BASE = 0x10000000;
        const DWORD DRM_SIZE = 0x11000;
        if (ctx->Eip >= DRM_BASE && ctx->Eip < DRM_BASE + DRM_SIZE) {
            DWORD ebp = ctx->Ebp;
            for (int hops = 0; hops < 16; ++hops) {
                MEMORY_BASIC_INFORMATION mbi2;
                if (!VirtualQuery((LPCVOID)ebp, &mbi2, sizeof(mbi2))
                    || mbi2.State != MEM_COMMIT) break;
                DWORD ret_addr = 0, prev_ebp = 0;
                __try {
                    ret_addr = *(DWORD*)(ebp + 4);
                    prev_ebp = *(DWORD*)(ebp);
                } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                if (ret_addr < DRM_BASE || ret_addr >= DRM_BASE + DRM_SIZE) {
                    // First frame whose ret address is outside DRM. Unwind.
                    // Convention: positive return = success (cmp/jle pattern in
                    // the function suggests "<=0 means failure"). Use 1.
                    Log("  >>> FIXUP: AV in recognizedisc.dll. Unwound %d frame(s); ret=0x%08X (from ebp=0x%08X), eax=1",
                        hops, ret_addr, ebp);
                    ctx->Eip = ret_addr;
                    ctx->Esp = ebp + 8;
                    ctx->Ebp = prev_ebp;
                    ctx->Eax = 1;
                    // Also patch fnRecognizedisc itself to return 1 going forward
                    BYTE* tgt = (BYTE*)0x10001020;
                    DWORD oldp;
                    if (VirtualProtect(tgt, 5, PAGE_EXECUTE_READWRITE, &oldp)) {
                        // mov eax, 1; ret  =>  B8 01 00 00 00 C3
                        // We have only 5 bytes; use: 6A 01 58 C3 90  (push 1; pop eax; ret; nop)
                        tgt[0] = 0x6A; tgt[1] = 0x01; tgt[2] = 0x58; tgt[3] = 0xC3; tgt[4] = 0x90;
                        DWORD r2;
                        VirtualProtect(tgt, 5, oldp, &r2);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                if (prev_ebp <= ebp) break;  // sanity: EBP must move up
                ebp = prev_ebp;
            }
            Log("  !!! AV in recognizedisc.dll but EBP frame chain didn't escape it (last ebp=0x%08X)", ebp);
        }
    }

    // Otherwise: pass through.
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        // Compute our DLL's address range for VEH self-skip
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(h, &mbi, sizeof(mbi))) {
            g_self_base = (DWORD)mbi.AllocationBase;
            // Walk forward to find the end of our image
            DWORD addr = g_self_base;
            while (true) {
                if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
                if (mbi.AllocationBase != (LPVOID)g_self_base) break;
                addr = (DWORD)mbi.BaseAddress + (DWORD)mbi.RegionSize;
            }
            g_self_size = addr - g_self_base;
        }
        // Open log file in the game's install directory (next to KishinHisho.exe).
        // GetModuleFileNameA(NULL, ...) returns the running EXE's path. After
        // deployment via 01_patch, the game folder is writable to whoever
        // ran 01_patch (admin); subsequent runs write the log there.
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) {
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "alpharom_helper.log");
        } else {
            strcpy_s(path, "alpharom_helper.log");
        }
        g_log = fopen(path, "w");
        Log("=== alpharom_helper.dll loaded into PID %lu ===", GetCurrentProcessId());
        Log("Log file: %s", path);

        // Install VEH FIRST — so we catch the first AV before any SEH the stub installs.
        if (AddVectoredExceptionHandler(1 /* first */, MyVEH)) {
            Log("VEH installed.");
        } else {
            Log("VEH install FAILED, error %lu", GetLastError());
        }

        // Spawn heartbeat thread that polls for recognizedisc.dll mapping
        // and patches it the instant it appears. This makes the DRM bypass
        // robust regardless of whether the AlphaROM stub crashes inside
        // fnRecognizedisc (timing/state-dependent) or completes it normally.
        HANDLE ht = CreateThread(NULL, 0, DrmHeartbeat, NULL, 0, NULL);
        if (ht) { CloseHandle(ht); Log("DRM heartbeat thread spawned."); }
        else    { Log("Failed to spawn DRM heartbeat thread, error %lu", GetLastError()); }

        // Dump initial PEB.Ldr state — useful for designing the LDR fixup later
        Log("--- Loaded modules (current Win11 layout, what the stub SHOULD see) ---");
        DWORD peb = __readfsdword(0x30);
        BYTE* ldr = *(BYTE**)(peb + 0x0C);
        LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x14); // InMemoryOrderModuleList
        for (LIST_ENTRY* c = head->Flink; c != head; c = c->Flink) {
            BYTE* e = (BYTE*)c - 8;
            DWORD base = *(DWORD*)(e + 0x18);
            DWORD size = *(DWORD*)(e + 0x20);
            USHORT nameLen = *(USHORT*)(e + 0x2C);
            wchar_t* nameBuf = *(wchar_t**)(e + 0x30);
            char name[64] = {0};
            for (USHORT i = 0; i < nameLen/2 && i < 63; ++i) name[i] = (char)nameBuf[i];
            Log("  base=0x%08X size=0x%08X  %s", base, size, name);
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        Log("=== alpharom_helper.dll unloading (exception count: %ld) ===",
            g_exception_count);
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
