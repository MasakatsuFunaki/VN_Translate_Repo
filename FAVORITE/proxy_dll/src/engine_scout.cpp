// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "engine_scout.h"
#include "log.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

namespace {

// Strings we expect to see somewhere in the decrypted engine but NOT in
// Sakura.exe's mapped 24KB launcher .text. Anywhere these show up at
// runtime is the engine.
struct Signature { const char* str; size_t len; };

#define SIG(s) { s, sizeof(s) - 1 }
const Signature kSignatures[] = {
    SIG("TextFont"),
    SIG("FontRasterizer"),
    SIG("TextureRenderer"),
    SIG("FontList"),
    SIG("GetGlyphOutlineA"),
    SIG("CreateFontA"),
    SIG("D3DXCreateTexture"),
    SIG("Direct3DCreate9"),
    SIG("d3dx9_31.dll"),
    SIG(".?AVTextFont@@"),
    SIG(".?AVFontRasterizer@@"),
    SIG(".?AVTextureRenderer@@"),
};
#undef SIG

const char* ProtName(DWORD p) {
    p &= ~PAGE_GUARD;
    switch (p) {
        case PAGE_NOACCESS: return "----";
        case PAGE_READONLY: return "R---";
        case PAGE_READWRITE: return "RW--";
        case PAGE_WRITECOPY: return "RWC-";
        case PAGE_EXECUTE: return "X---";
        case PAGE_EXECUTE_READ: return "RX--";
        case PAGE_EXECUTE_READWRITE: return "RWX-";
        case PAGE_EXECUTE_WRITECOPY: return "RWXC";
        default: return "????";
    }
}

const char* TypeName(DWORD t) {
    if (t & MEM_IMAGE)   return "IMG";
    if (t & MEM_MAPPED)  return "MAP";
    if (t & MEM_PRIVATE) return "PRV";
    return "???";
}

// Naive byte search. Region sizes are bounded (we cap at 8MB); the engine
// itself is O(MB). This runs once at DLL load — no need for KMP/BM.
size_t CountOccurrences(const BYTE* hay, size_t hayLen,
                        const char* needle, size_t needleLen) {
    if (needleLen == 0 || hayLen < needleLen) return 0;
    size_t count = 0;
    const BYTE first = (BYTE)needle[0];
    for (size_t i = 0; i + needleLen <= hayLen; ++i) {
        if (hay[i] != first) continue;
        if (memcmp(hay + i, needle, needleLen) == 0) {
            ++count;
            i += needleLen - 1;
        }
    }
    return count;
}

// Function-prologue density: count `55 8B EC` (push ebp; mov ebp,esp).
// Useful as a "is this region full of x86 functions?" indicator.
size_t CountPrologues(const BYTE* hay, size_t hayLen) {
    if (hayLen < 3) return 0;
    size_t count = 0;
    for (size_t i = 0; i + 3 <= hayLen; ++i) {
        if (hay[i] == 0x55 && hay[i + 1] == 0x8B && hay[i + 2] == 0xEC) {
            ++count;
        }
    }
    return count;
}

// Find first N occurrences of needle in hay. Returns offsets within hay.
std::vector<size_t> FindFirst(const BYTE* hay, size_t hayLen,
                              const char* needle, size_t needleLen,
                              size_t maxResults) {
    std::vector<size_t> out;
    if (needleLen == 0 || hayLen < needleLen) return out;
    const BYTE first = (BYTE)needle[0];
    for (size_t i = 0; i + needleLen <= hayLen && out.size() < maxResults; ++i) {
        if (hay[i] != first) continue;
        if (memcmp(hay + i, needle, needleLen) == 0) {
            out.push_back(i);
            i += needleLen - 1;
        }
    }
    return out;
}

// SEH-wrapped helpers must live in functions with NO C++ unwind semantics
// (no objects with destructors, no try/catch). Declared __declspec(noinline)
// so MSVC can't merge them back into ScoutEngine's frame.

__declspec(noinline)
static int SafeScanSignatures(const BYTE* base, size_t sz) {
    int total = 0;
    __try {
        for (size_t i = 0; i < sizeof(kSignatures) / sizeof(kSignatures[0]); ++i) {
            total += (int)CountOccurrences(base, sz,
                                            kSignatures[i].str,
                                            kSignatures[i].len);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return total;
}

__declspec(noinline)
static size_t SafeCountPrologues(const BYTE* base, size_t sz) {
    __try {
        return CountPrologues(base, sz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct ScoredRegion {
    BYTE* base;
    size_t size;
    DWORD protect;
    DWORD type;
    int sigCount;       // total signature hits across all sigs
    size_t prologueCount;
    char modulePath[MAX_PATH];
};

void LogTopRegion(const ScoredRegion& r) {
    Log("");
    Log("--- Region 0x%08X (size 0x%X, %s, %s) ---",
        (DWORD)r.base, (DWORD)r.size, ProtName(r.protect), TypeName(r.type));
    if (r.modulePath[0]) Log("    module: %s", r.modulePath);
    Log("    prologues=%zu  sig hits=%d", r.prologueCount, r.sigCount);

    // Re-scan to log first-occurrence offsets per signature.
    BYTE* hay = r.base;
    for (const Signature& s : kSignatures) {
        auto offs = FindFirst(hay, r.size, s.str, s.len, 4);
        for (size_t off : offs) {
            DWORD addr = (DWORD)(r.base + off);
            Log("    +0x%06X (VA 0x%08X): \"%s\"",
                (DWORD)off, addr, s.str);
        }
    }
}

// Returns true iff this region's module path identifies it as the
// FVP engine (Sakura.log mapping inside Sakura.exe's image).
bool IsEngineRegion(const ScoredRegion& r) {
    if (r.sigCount < 5) return false;
    // case-insensitive substring match for "Sakura.log"
    const char* needle = "sakura.log";
    char path[MAX_PATH];
    size_t n = 0;
    while (n + 1 < MAX_PATH && r.modulePath[n]) {
        path[n] = (char)tolower((unsigned char)r.modulePath[n]);
        ++n;
    }
    path[n] = 0;
    return strstr(path, needle) != nullptr;
}

// Dump the full Sakura.exe runtime image (the launcher stub PLUS the
// runtime-mapped engine sections that the original on-disk PE doesn't
// expose). Vtables in the Sakura.log-backed region point at class
// methods that live in lower address ranges (around 0x40xxxx) so we
// need everything from base..base+SizeOfImage.
//
// Output: sakura_runtime.bin next to Sakura.exe, sized = SizeOfImage,
// file offset N == VA (base + N). Load in Ghidra as Raw Binary, x86
// little-endian, image base = the reported runtime base (typically
// 0x00400000).
void DumpEngine(const std::vector<ScoredRegion>& /*scored*/) {
    HANDLE hProc = GetCurrentProcess();
    HMODULE hExe = GetModuleHandleA(nullptr);
    MODULEINFO mi{};
    if (!GetModuleInformation(hProc, hExe, &mi, sizeof(mi))) {
        Log("DumpEngine: GetModuleInformation FAILED");
        return;
    }

    BYTE* base = (BYTE*)mi.lpBaseOfDll;
    DWORD size = mi.SizeOfImage;
    Log("DumpEngine: dumping Sakura.exe runtime image, base 0x%08X size 0x%X",
        (DWORD)base, size);

    // Allocate buffer; we'll fill page by page so unreadable pages stay
    // zero rather than failing the whole dump.
    std::vector<BYTE> buf(size, 0);
    size_t okPages = 0, badPages = 0;
    BYTE* cursor = base;
    while (cursor < base + size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0) break;
        BYTE* rbase = (BYTE*)mbi.BaseAddress;
        size_t rsize = mbi.RegionSize;
        // Cap to image bounds
        if (rbase + rsize > base + size) rsize = (base + size) - rbase;
        bool readable = (mbi.State == MEM_COMMIT)
                     && !(mbi.Protect & PAGE_NOACCESS)
                     && !(mbi.Protect & PAGE_GUARD);
        if (readable) {
            memcpy(buf.data() + (rbase - base), rbase, rsize);
            okPages += rsize / 0x1000;
        } else {
            badPages += rsize / 0x1000;
        }
        cursor = rbase + rsize;
        if (cursor < rbase) break;
    }
    Log("DumpEngine: copied %zu KB readable, %zu KB unreadable (zero-filled)",
        okPages * 4, badPages * 4);

    // Write next to Sakura.exe.
    char outPath[MAX_PATH];
    GetModuleFileNameA(hExe, outPath, MAX_PATH);
    char* slash = strrchr(outPath, '\\');
    if (slash) strcpy(slash + 1, "sakura_runtime.bin");
    else       strcpy(outPath, "sakura_runtime.bin");

    FILE* f = fopen(outPath, "wb");
    if (!f) {
        Log("DumpEngine: fopen FAILED for %s", outPath);
        return;
    }
    size_t wrote = fwrite(buf.data(), 1, size, f);
    fclose(f);
    Log("DumpEngine: wrote %zu bytes to %s", wrote, outPath);
    Log("DumpEngine: Ghidra: Raw Binary, x86 little-endian, image base 0x%08X "
        "(file offset 0 == VA 0x%08X)", (DWORD)base, (DWORD)base);
}

} // namespace


void ScoutEngine() {
    Log("");
    Log("=== ScoutEngine: walking process address space ===");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE* addr = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* maxAddr = (BYTE*)si.lpMaximumApplicationAddress;

    std::vector<ScoredRegion> scored;
    int totalRegions = 0;
    int readableRegions = 0;
    HANDLE hProc = GetCurrentProcess();

    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        ++totalRegions;

        BYTE* base = (BYTE*)mbi.BaseAddress;
        size_t sz = mbi.RegionSize;
        DWORD prot = mbi.Protect;
        DWORD typ = mbi.Type;

        bool readable = (mbi.State == MEM_COMMIT)
                     && !(prot & PAGE_NOACCESS)
                     && !(prot & PAGE_GUARD);
        // Cap individual region scans at 8MB (engine should fit comfortably).
        if (readable && sz > 0 && sz <= 0x800000) {
            ++readableRegions;

            // Scan all signatures.
            int sigCount = SafeScanSignatures(base, sz);
            size_t prologueCount = 0;
            if (sigCount > 0 || (prot & 0xF0)) {
                prologueCount = SafeCountPrologues(base, sz);
            }

            if (sigCount > 0) {
                ScoredRegion r{};
                r.base = base;
                r.size = sz;
                r.protect = prot;
                r.type = typ;
                r.sigCount = sigCount;
                r.prologueCount = prologueCount;
                GetMappedFileNameA(hProc, base, r.modulePath, MAX_PATH);
                scored.push_back(r);
            }
        }

        addr = base + sz;
        if (addr < base) break;  // overflow guard
    }

    Log("Walked %d VAD entries (%d committed+readable).",
        totalRegions, readableRegions);
    Log("Regions with at least one engine signature hit: %zu",
        scored.size());

    // Sort by signature count desc.
    std::sort(scored.begin(), scored.end(),
              [](const ScoredRegion& a, const ScoredRegion& b) {
                  return a.sigCount > b.sigCount;
              });

    // Log top 8.
    size_t topN = (scored.size() < 8) ? scored.size() : 8;
    for (size_t i = 0; i < topN; ++i) {
        LogTopRegion(scored[i]);
    }

    // Also log the Sakura.exe module image for reference.
    HMODULE hExe = GetModuleHandleA(nullptr);
    MODULEINFO mi{};
    if (GetModuleInformation(hProc, hExe, &mi, sizeof(mi))) {
        Log("");
        Log("--- Sakura.exe module: base 0x%08X  size 0x%X  EP 0x%08X ---",
            (DWORD)mi.lpBaseOfDll, (DWORD)mi.SizeOfImage,
            (DWORD)mi.EntryPoint);
    }

    // Dump the engine regions (Sakura.log-backed mappings) to disk so
    // we can analyze them in Ghidra. Idempotent — overwrites if rerun.
    DumpEngine(scored);

    Log("=== ScoutEngine: done ===");
    Log("");
}
