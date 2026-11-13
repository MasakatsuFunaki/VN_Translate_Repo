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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {

constexpr std::size_t kInitialModulePathChars = MAX_PATH;
constexpr std::size_t kMaxModulePathChars = 32768;
constexpr std::size_t kPathGrowthFactor = 2;
constexpr std::size_t kPageSizeBytes = 0x1000;
constexpr std::size_t kKilobytesPerPage = 4;
constexpr std::size_t kMaxSigScanRegionBytes = 0x800000;
constexpr std::size_t kMaxPrologueScanRegionBytes = 0x4000000;
constexpr int kTopRegionCount = 8;
constexpr std::size_t kPreviewBytes = 16;
constexpr std::size_t kPrologueLen = 16;
constexpr std::ptrdiff_t kFun432795Rva = 0x32795;
constexpr int kMaxPrologueMatches = 8;
constexpr DWORD kProtectMask = 0xFFU;
constexpr int kThreadSampleCount = 5;
constexpr DWORD kThreadSampleSleepMs = 100U;
constexpr DWORD kSuspendFailed = static_cast<DWORD>(-1);
constexpr BYTE kPrologueByte0 = 0x8BU;
constexpr BYTE kPrologueByte1 = 0xFFU;
constexpr BYTE kPrologueByte2 = 0x55U;

// Strings we expect to see in the decrypted Artemis engine but NOT in
// sakuranotoki.exe's launcher. Mostly RTTI type names from the report.
constexpr std::string_view kSignatures[] = {
    "CFontRenderer",
    "CFontProperty",
    "CTextField",
    "CTextLayer",
    "CClippedSurface",
    "CAtlas",
    "CGlyphInfo",
    "COneLine",
    "COneBlock",
    "CScriptParser",
    "CCaret",
    "artemis",
    "babel",
    "DEVICE_FONT_NAME",
    "FONT_CACHE_SIZE",
    "FONT_ASCENT",
    "font_cache",
    "Direct3DCreate9",
    "d3d9.dll",
    "character_reference_to_utf8",
};

constexpr BYTE kFun432795Prologue[kPrologueLen] = {
    0x8BU, 0xFFU, 0x55U, 0x8BU, 0xECU, 0x83U, 0xECU, 0x64U,
    0x53U, 0x56U, 0x8BU, 0x75U, 0x08U, 0x33U, 0xD2U, 0x3BU
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

const char* TypeName(DWORD t) {
    if ((t & MEM_IMAGE) != 0) {
        return "IMG";
    }
    if ((t & MEM_MAPPED) != 0) {
        return "MAP";
    }
    if ((t & MEM_PRIVATE) != 0) {
        return "PRV";
    }
    return "???";
}

std::size_t CountOccurrences(const BYTE* hay, std::size_t hayLen,
                             const char* needle, std::size_t needleLen) {
    if (needleLen == 0 || hayLen < needleLen) {
        return 0;
    }
    std::size_t count = 0;
    const BYTE first = static_cast<BYTE>(needle[0]);
    for (std::size_t i = 0; i + needleLen <= hayLen; ++i) {
        if (hay[i] != first) {
            continue;
        }
        if (std::memcmp(hay + i, needle, needleLen) == 0) {
            ++count;
            i += needleLen - 1;
        }
    }
    return count;
}

__declspec(noinline)
int SafeScanSignatures(const BYTE* base, std::size_t sz) {
    int total = 0;
    __try {
        for (const std::string_view sig : kSignatures) {
            total += static_cast<int>(
                CountOccurrences(base, sz, sig.data(), sig.size()));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return total;
}

// SEH-only memcpy wrapper. Lives in a function with no C++ unwind so we
// can guard the page-fault case during the runtime image dump (some
// pages might be PAGE_GUARD or get unmapped between the VirtualQuery
// check and the memcpy).
__declspec(noinline)
void SafeMemcpyPage(BYTE* dst, const BYTE* src, std::size_t n) {
    __try {
        std::memcpy(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // leave dst zero-filled
    }
}

__declspec(noinline)
void SafeSearchPrologueInRegion(BYTE* base, std::size_t sz,
                                DWORD protect, DWORD type,
                                int* found_count) {
    __try {
        for (std::size_t i = 0; i + kPrologueLen <= sz; ++i) {
            if (base[i] != kPrologueByte0
                || base[i + 1] != kPrologueByte1
                || base[i + 2] != kPrologueByte2) {
                continue;
            }
            if (std::memcmp(base + i, kFun432795Prologue, kPrologueLen) != 0) {
                continue;
            }
            BYTE* const found = base + i;
            const std::uintptr_t engine_base =
                reinterpret_cast<std::uintptr_t>(found) -
                static_cast<std::uintptr_t>(kFun432795Rva);
            LOG("scout: FOUND prologue at %p (engine base would be %p) | "
                "region base=%p size=0x%X prot=0x%lX type=%s",
                found,
                reinterpret_cast<void*>(engine_base),
                base,
                static_cast<unsigned>(sz),
                static_cast<unsigned long>(protect),
                TypeName(type));
            ++(*found_count);
            if (*found_count >= kMaxPrologueMatches) {
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // ignore faults on transient pages
    }
}

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

std::string wide_to_utf8(const std::wstring& wpath) {
    if (wpath.empty()) {
        return {};
    }
    const int wlen = static_cast<int>(wpath.size());
    DWORD flags = WC_ERR_INVALID_CHARS;
    int needed = WideCharToMultiByte(
        CP_UTF8, flags, wpath.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        flags = 0;
        needed = WideCharToMultiByte(
            CP_UTF8, flags, wpath.data(), wlen, nullptr, 0, nullptr, nullptr);
    }
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, flags, wpath.data(), wlen, out.data(), needed, nullptr, nullptr);
    if (converted <= 0) {
        return {};
    }
    return out;
}

// Dump sakuranotoki.exe's full runtime image (launcher + the
// CrackProof-decrypted engine that gets memory-mapped INSIDE the EXE's
// image range). file offset N == VA (base + N). Load into Ghidra as
// Raw Binary, x86 little-endian, image base = the reported runtime
// base (typically 0x00400000).
void DumpEngine() {
    const HANDLE hProc = GetCurrentProcess();
    const HMODULE hExe = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    if (!GetModuleInformation(hProc, hExe, &mi, sizeof(mi))) {
        LOG("DumpEngine: GetModuleInformation FAILED");
        return;
    }

    BYTE* const base = static_cast<BYTE*>(mi.lpBaseOfDll);
    const DWORD size = mi.SizeOfImage;

    const std::wstring exe_path = query_module_path(hExe);
    if (exe_path.empty()) {
        LOG("DumpEngine: GetModuleFileNameW FAILED (gle=%lu)", GetLastError());
        return;
    }

    const std::size_t sep = exe_path.find_last_of(L"\\/");
    const std::wstring out_path =
        sep != std::wstring::npos
            ? exe_path.substr(0, sep + 1) + L"sakuranotoki_runtime.bin"
            : std::wstring(L"sakuranotoki_runtime.bin");
    const std::string out_path_utf8 = wide_to_utf8(out_path);

    // Skip dump if the file already exists in the game folder. The user
    // moved the canonical copy into sakuranotoki/analysys/ and there's
    // no point re-dumping every launch — the runtime image is stable
    // across launches once the engine has been decrypted.
    if (GetFileAttributesW(out_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        LOG("DumpEngine: %s already exists, skipping dump", out_path_utf8.c_str());
        return;
    }

    LOG("DumpEngine: dumping sakuranotoki.exe runtime image, "
        "base 0x%08X size 0x%X",
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(base)), size);

    std::vector<BYTE> buf(size, 0);
    std::size_t okPages = 0;
    std::size_t badPages = 0;
    BYTE* cursor = base;
    while (cursor < base + size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0) {
            break;
        }
        BYTE* const rbase = static_cast<BYTE*>(mbi.BaseAddress);
        std::size_t rsize = mbi.RegionSize;
        if (rbase + rsize > base + size) {
            rsize = static_cast<std::size_t>((base + size) - rbase);
        }
        const bool readable = (mbi.State == MEM_COMMIT)
                           && (mbi.Protect & PAGE_NOACCESS) == 0
                           && (mbi.Protect & PAGE_GUARD) == 0;
        if (readable) {
            SafeMemcpyPage(buf.data() + (rbase - base), rbase, rsize);
            okPages += rsize / kPageSizeBytes;
        } else {
            badPages += rsize / kPageSizeBytes;
        }
        cursor = rbase + rsize;
        if (cursor < rbase) {
            break;
        }
    }
    LOG("DumpEngine: copied %zu KB readable, %zu KB unreadable (zero-filled)",
        okPages * kKilobytesPerPage, badPages * kKilobytesPerPage);

    const ScopedHandle file(CreateFileW(
        out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        const DWORD create_err = GetLastError();
        LOG("DumpEngine: CreateFileW FAILED for %s (gle=%lu)",
            out_path_utf8.c_str(), create_err);
        return;
    }
    DWORD written = 0;
    WriteFile(file.get(), buf.data(), size, &written, nullptr);
    LOG("DumpEngine: wrote %lu bytes to %s",
        static_cast<unsigned long>(written), out_path_utf8.c_str());
    const unsigned base_u32 =
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(base));
    LOG("DumpEngine: Ghidra: Raw Binary, x86 little-endian, "
        "image base 0x%08X (file offset 0 == VA 0x%08X)",
        base_u32, base_u32);
}

}  // namespace

void ScoutEngine() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    BYTE* addr = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
    BYTE* const maxAddr = static_cast<BYTE*>(si.lpMaximumApplicationAddress);

    int totalRegions = 0;
    int signatureRegions = 0;
    int topSigCount = 0;
    const HANDLE hProc = GetCurrentProcess();

    // Track top-N regions by signature count so we can log their
    // addresses. At runtime we read zeros at the launcher-image RVAs
    // (CrackProof unpacks engine code somewhere ELSE), so we find the
    // regions where engine signatures are mapped right now.
    struct RegHit {
        BYTE* base = nullptr;
        std::size_t size = 0;
        int sigCount = 0;
        DWORD protect = 0;
    };
    RegHit top[kTopRegionCount] = {};

    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
            break;
        }
        ++totalRegions;

        BYTE* const base = static_cast<BYTE*>(mbi.BaseAddress);
        const std::size_t sz = mbi.RegionSize;
        const bool readable = (mbi.State == MEM_COMMIT)
                           && (mbi.Protect & PAGE_NOACCESS) == 0
                           && (mbi.Protect & PAGE_GUARD) == 0;
        if (readable && sz > 0 && sz <= kMaxSigScanRegionBytes) {
            const int sigCount = SafeScanSignatures(base, sz);
            if (sigCount > 0) {
                ++signatureRegions;
            }
            if (sigCount > topSigCount) {
                topSigCount = sigCount;
            }

            if (sigCount > 0) {
                int weakest = 0;
                for (int i = 1; i < kTopRegionCount; ++i) {
                    if (top[i].sigCount < top[weakest].sigCount) {
                        weakest = i;
                    }
                }
                if (sigCount > top[weakest].sigCount) {
                    top[weakest].base     = base;
                    top[weakest].size     = sz;
                    top[weakest].sigCount = sigCount;
                    top[weakest].protect  = mbi.Protect;
                }
            }
        }

        BYTE* const next = base + sz;
        if (next <= addr) {
            break;
        }
        addr = next;
    }

    const HMODULE hExe = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(hProc, hExe, &mi, sizeof(mi));
    LOG("scout: %d VAD entries, %d engine-signature regions (top hits=%d) | "
        "exe base=%p size=0x%X EP=0x%08X",
        totalRegions, signatureRegions, topSigCount,
        mi.lpBaseOfDll,
        static_cast<unsigned>(mi.SizeOfImage),
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(mi.EntryPoint)));

    LOG("scout: top engine-signature regions:");
    for (int i = 0; i < kTopRegionCount - 1; ++i) {
        for (int j = i + 1; j < kTopRegionCount; ++j) {
            if (top[j].sigCount > top[i].sigCount) {
                const RegHit t = top[i];
                top[i] = top[j];
                top[j] = t;
            }
        }
    }
    for (int i = 0; i < kTopRegionCount; ++i) {
        if (top[i].sigCount == 0) {
            continue;
        }
        BYTE pro[kPreviewBytes] = {};
        SIZE_T r = 0;
        ReadProcessMemory(GetCurrentProcess(), top[i].base, pro, kPreviewBytes, &r);
        LOG("  region[%d] base=%p size=0x%X prot=0x%lX sigs=%d "
            "first16=%02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            i, top[i].base, static_cast<unsigned>(top[i].size),
            static_cast<unsigned long>(top[i].protect), top[i].sigCount,
            pro[0], pro[1], pro[2], pro[3], pro[4], pro[5], pro[6], pro[7],
            pro[8], pro[9], pro[10], pro[11], pro[12], pro[13], pro[14], pro[15]);
    }

    DumpEngine();

    // Engine-base discovery via prologue-pattern search.
    // The 5MB engine dump in analysys/ shows FUN_00432795 has the
    // prologue "8B FF 55 8B EC 83 EC 64 53 56 8B 75 08 33 D2 3B"
    // — a distinctive 16-byte sequence very unlikely to appear in
    // unrelated code. Where it is found at runtime is the engine's
    // actual base + 0x32795.
    LOG("scout: searching memory for FUN_00432795 prologue pattern...");
    int found_count = 0;
    addr = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
            break;
        }
        BYTE* const base = static_cast<BYTE*>(mbi.BaseAddress);
        const std::size_t sz = mbi.RegionSize;
        const bool readable = (mbi.State == MEM_COMMIT)
                           && (mbi.Protect & PAGE_NOACCESS) == 0
                           && (mbi.Protect & PAGE_GUARD) == 0;
        const DWORD low_prot = mbi.Protect & kProtectMask;
        const bool executable =
            low_prot == PAGE_EXECUTE
            || low_prot == PAGE_EXECUTE_READ
            || low_prot == PAGE_EXECUTE_READWRITE
            || low_prot == PAGE_EXECUTE_WRITECOPY;
        if (readable && executable && sz > 0 && sz <= kMaxPrologueScanRegionBytes) {
            SafeSearchPrologueInRegion(
                base, sz, mbi.Protect, mbi.Type, &found_count);
        }
        BYTE* const next = base + sz;
        if (next <= addr) {
            break;
        }
        addr = next;
        if (found_count >= kMaxPrologueMatches) {
            break;
        }
    }
    LOG("scout: prologue search done (found=%d)", found_count);

    // Thread-EIP sampling: periodically suspend each non-self thread
    // and log its EIP. Where the engine's main thread executes is by
    // definition the engine's code section. Sample multiple times so
    // we don't catch the thread always in a wait-state in ntdll.
    LOG("scout: sampling thread EIPs (5x100ms samples)...");
    const DWORD pid = GetCurrentProcessId();
    const DWORD self_tid = GetCurrentThreadId();
    for (int sample = 0; sample < kThreadSampleCount; ++sample) {
        Sleep(kThreadSampleSleepMs);
        const ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        if (!snap.valid()) {
            LOG("scout: snap failed gle=%lu", GetLastError());
            break;
        }
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (!Thread32First(snap.get(), &te)) {
            break;
        }
        do {
            if (te.th32OwnerProcessID != pid) {
                continue;
            }
            if (te.th32ThreadID == self_tid) {
                continue;
            }
            const ScopedHandle th(OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, te.th32ThreadID));
            if (!th.valid()) {
                continue;
            }
            if (SuspendThread(th.get()) != kSuspendFailed) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th.get(), &ctx)) {
                    LOG("scout: sample#%d tid=%lu eip=%08X",
                        sample + 1, te.th32ThreadID,
                        static_cast<unsigned>(ctx.Eip));
                }
                ResumeThread(th.get());
            }
        } while (Thread32Next(snap.get(), &te));
    }
    LOG("scout: thread-EIP sampling done");
}
