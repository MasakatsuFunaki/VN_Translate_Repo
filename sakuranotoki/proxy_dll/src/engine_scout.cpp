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
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

namespace {

// Strings we expect to see in the decrypted Artemis engine but NOT in
// sakuranotoki.exe's launcher. Mostly RTTI type names from the report.
struct Signature { const char* str; size_t len; };
#define SIG(s) { s, sizeof(s) - 1 }
const Signature kSignatures[] = {
    SIG("CFontRenderer"),
    SIG("CFontProperty"),
    SIG("CTextField"),
    SIG("CTextLayer"),
    SIG("CClippedSurface"),
    SIG("CAtlas"),
    SIG("CGlyphInfo"),
    SIG("COneLine"),
    SIG("COneBlock"),
    SIG("CScriptParser"),
    SIG("CCaret"),
    SIG("artemis"),
    SIG("babel"),
    SIG("DEVICE_FONT_NAME"),
    SIG("FONT_CACHE_SIZE"),
    SIG("FONT_ASCENT"),
    SIG("font_cache"),
    SIG("Direct3DCreate9"),
    SIG("d3d9.dll"),
    SIG("character_reference_to_utf8"),
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

size_t CountOccurrences(const BYTE* hay, size_t hayLen,
                        const char* needle, size_t needleLen) {
    if (needleLen == 0 || hayLen < needleLen) return 0;
    size_t count = 0;
    const BYTE first = (BYTE)needle[0];
    for (size_t i = 0; i + needleLen <= hayLen; ++i) {
        if (hay[i] != first) continue;
        if (std::memcmp(hay + i, needle, needleLen) == 0) {
            ++count;
            i += needleLen - 1;
        }
    }
    return count;
}

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

std::vector<size_t> FindFirst(const BYTE* hay, size_t hayLen,
                              const char* needle, size_t needleLen,
                              size_t maxResults) {
    std::vector<size_t> out;
    if (needleLen == 0 || hayLen < needleLen) return out;
    const BYTE first = (BYTE)needle[0];
    for (size_t i = 0; i + needleLen <= hayLen && out.size() < maxResults; ++i) {
        if (hay[i] != first) continue;
        if (std::memcmp(hay + i, needle, needleLen) == 0) {
            out.push_back(i);
            i += needleLen - 1;
        }
    }
    return out;
}

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

// SEH-only memcpy wrapper. Lives in a function with no C++ unwind so we
// can guard the page-fault case during the runtime image dump (some
// pages might be PAGE_GUARD or get unmapped between the VirtualQuery
// check and the memcpy).
__declspec(noinline)
static void SafeMemcpyPage(BYTE* dst, const BYTE* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // leave dst zero-filled
    }
}

struct ScoredRegion {
    BYTE* base;
    size_t size;
    DWORD protect;
    DWORD type;
    int sigCount;
    size_t prologueCount;
    char modulePath[MAX_PATH];
};

void LogTopRegion(const ScoredRegion& r) {
    // Compact one-line summary per region, no per-string dump (used to
    // be ~50 lines, now 1).
    const char* mod = r.modulePath[0] ? r.modulePath : "(no module)";
    LOG("  region 0x%08X size=0x%X %s %s sigs=%d  %s",
        (DWORD)r.base, (DWORD)r.size, ProtName(r.protect), TypeName(r.type),
        r.sigCount, mod);
}

// Dump sakuranotoki.exe's full runtime image (launcher + the
// CrackProof-decrypted engine that gets memory-mapped INSIDE the EXE's
// image range). file offset N == VA (base + N). Load into Ghidra as
// Raw Binary, x86 little-endian, image base = the reported runtime
// base (typically 0x00400000).
void DumpEngine() {
    HANDLE hProc = GetCurrentProcess();
    HMODULE hExe = GetModuleHandleA(nullptr);
    MODULEINFO mi{};
    if (!GetModuleInformation(hProc, hExe, &mi, sizeof(mi))) {
        LOG("DumpEngine: GetModuleInformation FAILED");
        return;
    }

    BYTE* base = (BYTE*)mi.lpBaseOfDll;
    DWORD size = mi.SizeOfImage;

    // Write next to the EXE.
    char outPath[MAX_PATH];
    GetModuleFileNameA(hExe, outPath, MAX_PATH);
    char* slash2 = std::strrchr(outPath, '\\');
    if (slash2) std::strcpy(slash2 + 1, "sakuranotoki_runtime.bin");
    else        std::strcpy(outPath, "sakuranotoki_runtime.bin");

    // Skip dump if the file already exists in the game folder. The user
    // moved the canonical copy into sakuranotoki/analysys/ and there's
    // no point re-dumping every launch — the runtime image is stable
    // across launches once the engine has been decrypted.
    if (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES) {
        LOG("DumpEngine: %s already exists, skipping dump", outPath);
        return;
    }

    LOG("DumpEngine: dumping sakuranotoki.exe runtime image, "
        "base 0x%08X size 0x%X", (DWORD)base, size);

    std::vector<BYTE> buf(size, 0);
    size_t okPages = 0, badPages = 0;
    BYTE* cursor = base;
    while (cursor < base + size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0) break;
        BYTE* rbase = (BYTE*)mbi.BaseAddress;
        size_t rsize = mbi.RegionSize;
        if (rbase + rsize > base + size) rsize = (base + size) - rbase;
        bool readable = (mbi.State == MEM_COMMIT)
                     && !(mbi.Protect & PAGE_NOACCESS)
                     && !(mbi.Protect & PAGE_GUARD);
        if (readable) {
            SafeMemcpyPage(buf.data() + (rbase - base), rbase, rsize);
            okPages += rsize / 0x1000;
        } else {
            badPages += rsize / 0x1000;
        }
        cursor = rbase + rsize;
        if (cursor < rbase) break;
    }
    LOG("DumpEngine: copied %zu KB readable, %zu KB unreadable (zero-filled)",
        okPages * 4, badPages * 4);

    HANDLE h = CreateFileA(outPath, GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG("DumpEngine: CreateFile FAILED for %s (gle=%lu)",
            outPath, GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(h, buf.data(), size, &written, nullptr);
    CloseHandle(h);
    LOG("DumpEngine: wrote %lu bytes to %s",
        (unsigned long)written, outPath);
    LOG("DumpEngine: Ghidra: Raw Binary, x86 little-endian, "
        "image base 0x%08X (file offset 0 == VA 0x%08X)",
        (DWORD)base, (DWORD)base);
}

} // namespace


void ScoutEngine() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE* addr = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* maxAddr = (BYTE*)si.lpMaximumApplicationAddress;

    int totalRegions = 0;
    int signatureRegions = 0;
    int topSigCount = 0;
    HANDLE hProc = GetCurrentProcess();

    // Track top-N regions by signature count so we can log their
    // addresses. This is what we actually need: at runtime we read
    // zeros at the launcher-image RVAs (CrackProof unpacks engine
    // code somewhere ELSE), so we need to find the regions where
    // engine signatures ACTUALLY are mapped right now.
    struct RegHit {
        BYTE* base;
        size_t size;
        int sigCount;
        DWORD protect;
    };
    constexpr int kTopN = 8;
    RegHit top[kTopN] = {};

    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        ++totalRegions;

        BYTE* base = (BYTE*)mbi.BaseAddress;
        size_t sz = mbi.RegionSize;
        bool readable = (mbi.State == MEM_COMMIT)
                     && !(mbi.Protect & PAGE_NOACCESS)
                     && !(mbi.Protect & PAGE_GUARD);
        if (readable && sz > 0 && sz <= 0x800000) {
            int sigCount = SafeScanSignatures(base, sz);
            if (sigCount > 0) ++signatureRegions;
            if (sigCount > topSigCount) topSigCount = sigCount;

            // Insert into top-N if this region has more signatures
            // than the weakest entry.
            if (sigCount > 0) {
                int weakest = 0;
                for (int i = 1; i < kTopN; ++i) {
                    if (top[i].sigCount < top[weakest].sigCount) weakest = i;
                }
                if (sigCount > top[weakest].sigCount) {
                    top[weakest].base     = base;
                    top[weakest].size     = sz;
                    top[weakest].sigCount = sigCount;
                    top[weakest].protect  = mbi.Protect;
                }
            }
        }

        BYTE* next = base + sz;
        if (next <= addr) break;
        addr = next;
    }

    HMODULE hExe = GetModuleHandleA(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(hProc, hExe, &mi, sizeof(mi));
    LOG("scout: %d VAD entries, %d engine-signature regions (top hits=%d) | "
        "exe base=%p size=0x%X EP=0x%08X",
        totalRegions, signatureRegions, topSigCount,
        mi.lpBaseOfDll, (DWORD)mi.SizeOfImage, (DWORD)mi.EntryPoint);

    // Log top-N signature regions with addresses + first 16 bytes so
    // we can identify the engine-code base at runtime. Sort top
    // descending by sigCount for readability.
    LOG("scout: top engine-signature regions:");
    for (int i = 0; i < kTopN - 1; ++i) {
        for (int j = i + 1; j < kTopN; ++j) {
            if (top[j].sigCount > top[i].sigCount) {
                RegHit t = top[i]; top[i] = top[j]; top[j] = t;
            }
        }
    }
    for (int i = 0; i < kTopN; ++i) {
        if (top[i].sigCount == 0) continue;
        BYTE pro[16] = {};
        SIZE_T r = 0;
        ReadProcessMemory(GetCurrentProcess(), top[i].base, pro, 16, &r);
        LOG("  region[%d] base=%p size=0x%X prot=0x%lX sigs=%d "
            "first16=%02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            i, top[i].base, (unsigned)top[i].size,
            (unsigned long)top[i].protect, top[i].sigCount,
            pro[0], pro[1], pro[2], pro[3], pro[4], pro[5], pro[6], pro[7],
            pro[8], pro[9], pro[10], pro[11], pro[12], pro[13], pro[14], pro[15]);
    }

    DumpEngine();

    // Engine-base discovery via prologue-pattern search.
    //
    // The 5MB engine dump in analysys/ shows FUN_00432795 has the
    // prologue "8B FF 55 8B EC 83 EC 64 53 56 8B 75 08 33 D2 3B"
    // — a distinctive 16-byte sequence very unlikely to appear in
    // unrelated code. We search ALL writable executable memory for
    // this pattern. Where it's found at runtime is the engine's
    // actual base + 0x32795. Engine base = match - 0x32795.
    //
    // This sidesteps the SizeOfImage / launcher-vs-engine ambiguity:
    // the 5MB dump's content is ground truth, regardless of where
    // CrackProof currently has it mapped.
    static const BYTE kFun432795Prologue[16] = {
        0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x64,
        0x53, 0x56, 0x8B, 0x75, 0x08, 0x33, 0xD2, 0x3B
    };
    LOG("scout: searching memory for FUN_00432795 prologue pattern...");
    int found_count = 0;
    addr = (BYTE*)si.lpMinimumApplicationAddress;
    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        BYTE* base = (BYTE*)mbi.BaseAddress;
        size_t sz = mbi.RegionSize;
        bool readable = (mbi.State == MEM_COMMIT)
                     && !(mbi.Protect & PAGE_NOACCESS)
                     && !(mbi.Protect & PAGE_GUARD);
        bool executable =
            (mbi.Protect & 0xFF) == PAGE_EXECUTE
         || (mbi.Protect & 0xFF) == PAGE_EXECUTE_READ
         || (mbi.Protect & 0xFF) == PAGE_EXECUTE_READWRITE
         || (mbi.Protect & 0xFF) == PAGE_EXECUTE_WRITECOPY;
        if (readable && executable && sz > 0 && sz <= 0x4000000) {
            __try {
                for (size_t i = 0; i + 16 <= sz; ++i) {
                    if (base[i] == 0x8B && base[i + 1] == 0xFF
                        && base[i + 2] == 0x55
                        && std::memcmp(base + i, kFun432795Prologue, 16) == 0) {
                        BYTE* found = base + i;
                        long engine_base = (long)found - (long)0x32795;
                        LOG("scout: FOUND prologue at %p (engine base "
                            "would be %p) | region base=%p size=0x%X "
                            "prot=0x%lX type=%s",
                            found, (void*)(uintptr_t)engine_base,
                            base, (unsigned)sz,
                            (unsigned long)mbi.Protect, TypeName(mbi.Type));
                        ++found_count;
                        if (found_count >= 8) break;  // cap log noise
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        BYTE* next = base + sz;
        if (next <= addr) break;
        addr = next;
        if (found_count >= 8) break;
    }
    LOG("scout: prologue search done (found=%d)", found_count);

    // Thread-EIP sampling: periodically suspend each non-self thread
    // and log its EIP. Where the engine's main thread executes is by
    // definition the engine's code section. Sample multiple times so
    // we don't catch the thread always in a wait-state in ntdll.
    LOG("scout: sampling thread EIPs (5x100ms passes)...");
    DWORD pid = GetCurrentProcessId();
    DWORD self_tid = GetCurrentThreadId();
    for (int pass = 0; pass < 5; ++pass) {
        Sleep(100);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            LOG("scout: snap failed gle=%lu", GetLastError());
            break;
        }
        THREADENTRY32 te{}; te.dwSize = sizeof(te);
        if (!Thread32First(snap, &te)) { CloseHandle(snap); break; }
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self_tid) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT
                                 | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th, &ctx)) {
                    LOG("scout: pass#%d tid=%lu eip=%08X",
                        pass + 1, te.th32ThreadID, (DWORD)ctx.Eip);
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    LOG("scout: thread-EIP sampling done");
}
