// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "unpack_dump.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

// ============================================================================
// Self-dumper for the cc.exe (packed) module.
//
// cc.exe's static IAT is bare-bones because the packer resolves the rest
// via GetProcAddress at runtime — meaning static analysis of the on-disk
// EXE shows ~13 symbols total. To get useful disassembly we need the
// UNPACKED image, which only exists in process memory after the packer
// stub at the entry point has run and decrypted the real code in place.
//
// We're already a DLL inside cc.exe's address space, so dumping the
// unpacked image is trivial: walk the module's virtual pages with
// VirtualQuery, write each committed page to a file. The catch is
// timing — at DLL_PROCESS_ATTACH the packer hasn't run yet. We spawn a
// background thread that sleeps 10 seconds then dumps. By then the
// game has reached the title screen (= packer is long done) and the
// .text section holds the unpacked code.
//
// Dump file: cc_unpacked.bin in the game directory (cwd at DllMain).
// To re-import in Ghidra:
//   File > Import > Add raw binary
//   Language:  x86:LE:32:default:windows
//   Image base: 0x00400000
//   ...then run auto-analysis.
//
// Cap on output size: 64 MB. cc.exe's SizeOfImage is ~118 MB but most
// of that is virtual scratch space (one section has VSize=100 MB,
// RawSize=7 KB) — those pages are reserved-but-uncommitted, so we'd
// just be writing zeros. 64 MB is enough headroom for the actual
// decrypted code + data.
// ============================================================================

static constexpr DWORD kDelayMs   = 10000;     // 10s post-DllMain
static constexpr DWORD kMaxDump   = 64u * 1024u * 1024u;
static constexpr DWORD kPageSize  = 4096;

// __try / __except cannot live in a function that performs C++ object
// unwinding (RAII destructors), so isolate the per-page SEH-guarded
// write into a tiny standalone helper.
static bool TryWritePage(HANDLE f, BYTE* page, BYTE* zeros, DWORD pageSize,
                        DWORD* written) {
    __try {
        return WriteFile(f, page, pageSize, written, nullptr) != 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        WriteFile(f, zeros, pageSize, written, nullptr);
        return false;
    }
}

static DWORD WINAPI DumperThread(LPVOID) {
    Sleep(kDelayMs);

    HMODULE hMod = GetModuleHandleW(nullptr);
    BYTE* base   = reinterpret_cast<BYTE*>(hMod);
    auto* dos    = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    DWORD soi      = nt->OptionalHeader.SizeOfImage;
    DWORD dumpSize = (soi < kMaxDump ? soi : kMaxDump);

    Log("Dumper: cc.exe base=%p, SizeOfImage=0x%X, dump size capped to 0x%X",
        base, soi, dumpSize);

    HANDLE f = CreateFileA("cc_unpacked.bin", GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("Dumper: CreateFile cc_unpacked.bin FAILED gle=%lu",
            GetLastError());
        return 0;
    }

    static BYTE zeroPage[kPageSize] = {0};   // .bss, zero-initialised
    DWORD written = 0;
    DWORD totalWritten = 0;
    DWORD readablePages = 0, zeroedPages = 0;

    for (DWORD off = 0; off < dumpSize; off += kPageSize) {
        MEMORY_BASIC_INFORMATION mbi = {};
        BYTE* p = base + off;
        bool readable = false;
        if (VirtualQuery(p, &mbi, sizeof(mbi))) {
            DWORD prot = mbi.Protect;
            bool committed = (mbi.State == MEM_COMMIT);
            bool denied = (prot == 0) ||
                          (prot & (PAGE_NOACCESS | PAGE_GUARD));
            readable = committed && !denied;
        }
        if (readable) {
            if (TryWritePage(f, p, zeroPage, kPageSize, &written))
                readablePages++;
            else
                zeroedPages++;
        } else {
            WriteFile(f, zeroPage, kPageSize, &written, nullptr);
            zeroedPages++;
        }
        totalWritten += written;
    }

    CloseHandle(f);
    Log("Dumper: wrote %u bytes to cc_unpacked.bin "
        "(%u readable pages, %u zeroed)",
        totalWritten, readablePages, zeroedPages);
    return 0;
}

void UnpackDumpInit() {
    HANDLE h = CreateThread(nullptr, 0, DumperThread, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
        Log("Dumper: background thread spawned, will dump in %u ms", kDelayMs);
    } else {
        Log("Dumper: CreateThread FAILED gle=%lu", GetLastError());
    }
}
