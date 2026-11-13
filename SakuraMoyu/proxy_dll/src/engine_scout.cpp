// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "engine_scout.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {

constexpr std::size_t kInitialPathBufferChars = MAX_PATH;
constexpr std::size_t kMaxExtendedPathChars = 32768u;
constexpr std::size_t kPathBufferGrowthFactor = 2u;
constexpr std::size_t kX86PrologueByteCount = 3u;
constexpr BYTE kPushEbpOpcode = 0x55u;
constexpr BYTE kMovEbpEspOpcode0 = 0x8Bu;
constexpr BYTE kMovEbpEspOpcode1 = 0xECu;
constexpr std::size_t kMaxLoggedOffsetsPerSig = 4u;
constexpr std::size_t kMaxScannedRegionBytes = 0x800000u;
constexpr std::size_t kMemoryPageBytes = 0x1000u;
constexpr std::size_t kKilobytesPerPage = 4u;
constexpr std::size_t kSingleByteElementSize = 1u;
constexpr std::size_t kMaxLoggedTopRegions = 8u;
constexpr UINT kCp932CodePage = 932u;
constexpr DWORD kExecutableProtectMask =
    PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr std::wstring_view kRuntimeDumpFileName = L"sakura_runtime.bin";

// Strings present in the decrypted runtime engine image rather than in
// Sakura.exe's 24 KB launcher stub.
struct Signature {
    const char* str = nullptr;
    std::size_t len = 0u;

    template <std::size_t N>
    constexpr explicit Signature(const char (&literal)[N]) noexcept
        : str(literal), len(N - 1u) {}
};

constexpr Signature kSignatures[] = {
    Signature("TextFont"),
    Signature("FontRasterizer"),
    Signature("TextureRenderer"),
    Signature("FontList"),
    Signature("GetGlyphOutlineA"),
    Signature("CreateFontA"),
    Signature("D3DXCreateTexture"),
    Signature("Direct3DCreate9"),
    Signature("d3dx9_31.dll"),
    Signature(".?AVTextFont@@"),
    Signature(".?AVFontRasterizer@@"),
    Signature(".?AVTextureRenderer@@"),
};

constexpr std::size_t kSignatureCount =
    sizeof(kSignatures) / sizeof(kSignatures[0]);

const char* ProtName(DWORD p) noexcept {
    p &= ~static_cast<DWORD>(PAGE_GUARD);
    switch (p) {
        case PAGE_NOACCESS:
            return "----";
        case PAGE_READONLY:
            return "R---";
        case PAGE_READWRITE:
            return "RW--";
        case PAGE_WRITECOPY:
            return "RWC-";
        case PAGE_EXECUTE:
            return "X---";
        case PAGE_EXECUTE_READ:
            return "RX--";
        case PAGE_EXECUTE_READWRITE:
            return "RWX-";
        case PAGE_EXECUTE_WRITECOPY:
            return "RWXC";
        default:
            return "????";
    }
}

const char* TypeName(DWORD t) noexcept {
    if ((t & MEM_IMAGE) != 0u) {
        return "IMG";
    }
    if ((t & MEM_MAPPED) != 0u) {
        return "MAP";
    }
    if ((t & MEM_PRIVATE) != 0u) {
        return "PRV";
    }
    return "???";
}

std::wstring QueryModuleFileNameWide(HMODULE module) {
    std::size_t capacity = kInitialPathBufferChars;
    while (capacity <= kMaxExtendedPathChars) {
        std::wstring buf(capacity, L'\0');
        const DWORD len = GetModuleFileNameW(
            module, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0u) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(static_cast<std::size_t>(len));
            return buf;
        }
        if (capacity == kMaxExtendedPathChars) {
            break;
        }
        capacity = (std::min)(capacity * kPathBufferGrowthFactor,
                              kMaxExtendedPathChars);
    }
    return {};
}

std::wstring QueryMappedFileNameWide(HANDLE process, void* address) {
    std::size_t capacity = kInitialPathBufferChars;
    while (capacity <= kMaxExtendedPathChars) {
        std::wstring buf(capacity, L'\0');
        const DWORD len = GetMappedFileNameW(
            process, address, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0u) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(static_cast<std::size_t>(len));
            return buf;
        }
        if (capacity == kMaxExtendedPathChars) {
            break;
        }
        capacity = (std::min)(capacity * kPathBufferGrowthFactor,
                              kMaxExtendedPathChars);
    }
    return {};
}

std::wstring BuildSiblingPathWide(std::wstring_view module_path,
                                  std::wstring_view file_name) {
    const std::size_t sep = module_path.find_last_of(L"\\/");
    if (sep == std::wstring_view::npos) {
        return std::wstring(file_name);
    }
    std::wstring out(module_path.substr(0u, sep + 1u));
    out.append(file_name);
    return out;
}

// Log() formats CP932 strings and transcodes them to UTF-8 on write, so convert
// wide paths to CP932 only for diagnostic log messages while keeping all OS I/O
// purely UTF-16.
std::string WideToLogCp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        kCp932CodePage,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        kCp932CodePage,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        narrow.data(),
        needed,
        nullptr,
        nullptr);
    if (written != needed) {
        return {};
    }
    return narrow;
}

std::size_t CountOccurrences(const BYTE* hay,
                             std::size_t hay_len,
                             const char* needle,
                             std::size_t needle_len) noexcept {
    if (needle_len == 0u || hay_len < needle_len) {
        return 0u;
    }
    std::size_t count = 0u;
    const BYTE first = static_cast<BYTE>(needle[0]);
    for (std::size_t i = 0u; i + needle_len <= hay_len; ++i) {
        if (hay[i] != first) {
            continue;
        }
        if (std::memcmp(hay + i, needle, needle_len) == 0) {
            ++count;
            i += needle_len - 1u;
        }
    }
    return count;
}

std::size_t CountPrologues(const BYTE* hay, std::size_t hay_len) noexcept {
    if (hay_len < kX86PrologueByteCount) {
        return 0u;
    }
    std::size_t count = 0u;
    for (std::size_t i = 0u; i + kX86PrologueByteCount <= hay_len; ++i) {
        if (hay[i] == kPushEbpOpcode &&
            hay[i + 1u] == kMovEbpEspOpcode0 &&
            hay[i + 2u] == kMovEbpEspOpcode1) {
            ++count;
        }
    }
    return count;
}

std::vector<std::size_t> FindFirst(const BYTE* hay,
                                   std::size_t hay_len,
                                   const char* needle,
                                   std::size_t needle_len,
                                   std::size_t max_results) {
    std::vector<std::size_t> out;
    if (needle_len == 0u || hay_len < needle_len) {
        return out;
    }
    const BYTE first = static_cast<BYTE>(needle[0]);
    for (std::size_t i = 0u;
         i + needle_len <= hay_len && out.size() < max_results;
         ++i) {
        if (hay[i] != first) {
            continue;
        }
        if (std::memcmp(hay + i, needle, needle_len) == 0) {
            out.push_back(i);
            i += needle_len - 1u;
        }
    }
    return out;
}

// SEH-wrapped helpers must live in functions with no C++ unwind objects.
__declspec(noinline)
int SafeScanSignatures(const BYTE* base, std::size_t sz) noexcept {
    int total = 0;
    __try {
        for (std::size_t i = 0u; i < kSignatureCount; ++i) {
            total += static_cast<int>(CountOccurrences(
                base, sz, kSignatures[i].str, kSignatures[i].len));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return total;
}

__declspec(noinline)
std::size_t SafeCountPrologues(const BYTE* base, std::size_t sz) noexcept {
    __try {
        return CountPrologues(base, sz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0u;
    }
}

struct ScoredRegion {
    BYTE* base = nullptr;
    std::size_t size = 0u;
    DWORD protect = 0u;
    DWORD type = 0u;
    int sig_count = 0;
    std::size_t prologue_count = 0u;
    std::wstring module_path;
};

void LogTopRegion(const ScoredRegion& r) {
    Log("");
    Log("--- Region 0x%08X (size 0x%X, %s, %s) ---",
        static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(r.base)),
        static_cast<DWORD>(r.size),
        ProtName(r.protect),
        TypeName(r.type));
    if (!r.module_path.empty()) {
        const std::string log_path = WideToLogCp932(r.module_path);
        if (!log_path.empty()) {
            Log("    module: %s", log_path.c_str());
        }
    }
    Log("    prologues=%zu  sig hits=%d", r.prologue_count, r.sig_count);

    for (const Signature& s : kSignatures) {
        const auto offs = FindFirst(
            r.base, r.size, s.str, s.len, kMaxLoggedOffsetsPerSig);
        for (const std::size_t off : offs) {
            const DWORD addr = static_cast<DWORD>(
                reinterpret_cast<std::uintptr_t>(r.base + off));
            Log("    +0x%06X (VA 0x%08X): \"%s\"",
                static_cast<DWORD>(off), addr, s.str);
        }
    }
}

// Dumps the full Sakura.exe runtime image (base..base+SizeOfImage) to
// sakura_runtime.bin next to Sakura.exe so Ghidra can analyze runtime-decrypted
// sections with file offset N == VA (base + N).
void DumpEngine() {
    const HANDLE proc = GetCurrentProcess();
    const HMODULE exe_module = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    if (!GetModuleInformation(proc, exe_module, &mi, sizeof(mi))) {
        Log("DumpEngine: GetModuleInformation FAILED");
        return;
    }

    BYTE* const base = static_cast<BYTE*>(mi.lpBaseOfDll);
    const DWORD size = mi.SizeOfImage;
    const DWORD base_addr =
        static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(base));
    Log("DumpEngine: dumping Sakura.exe runtime image, base 0x%08X size 0x%X",
        base_addr, size);

    std::vector<BYTE> buf(size, 0u);
    std::size_t ok_pages = 0u;
    std::size_t bad_pages = 0u;
    BYTE* cursor = base;
    while (cursor < base + size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0u) {
            break;
        }
        BYTE* const rbase = static_cast<BYTE*>(mbi.BaseAddress);
        std::size_t rsize = mbi.RegionSize;
        if (rbase + rsize > base + size) {
            rsize = static_cast<std::size_t>((base + size) - rbase);
        }
        const bool readable = (mbi.State == MEM_COMMIT) &&
                              (mbi.Protect & PAGE_NOACCESS) == 0u &&
                              (mbi.Protect & PAGE_GUARD) == 0u;
        if (readable) {
            std::memcpy(
                buf.data() + static_cast<std::size_t>(rbase - base),
                rbase,
                rsize);
            ok_pages += rsize / kMemoryPageBytes;
        } else {
            bad_pages += rsize / kMemoryPageBytes;
        }
        cursor = rbase + rsize;
        if (cursor < rbase) {
            break;
        }
    }
    Log("DumpEngine: copied %zu KB readable, %zu KB unreadable (zero-filled)",
        ok_pages * kKilobytesPerPage, bad_pages * kKilobytesPerPage);

    const std::wstring exe_path = QueryModuleFileNameWide(exe_module);
    const std::wstring out_path =
        BuildSiblingPathWide(exe_path, kRuntimeDumpFileName);
    const std::string log_out_path = WideToLogCp932(out_path);

    using ScopedFile = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    ScopedFile dump_file(_wfopen(out_path.c_str(), L"wb"), &std::fclose);
    if (!dump_file) {
        Log("DumpEngine: _wfopen FAILED for %s", log_out_path.c_str());
        return;
    }
    const std::size_t wrote =
        std::fwrite(buf.data(), kSingleByteElementSize, size, dump_file.get());
    dump_file.reset();

    Log("DumpEngine: wrote %zu bytes to %s", wrote, log_out_path.c_str());
    Log("DumpEngine: Ghidra: Raw Binary, x86 little-endian, image base 0x%08X "
        "(file offset 0 == VA 0x%08X)",
        base_addr, base_addr);
}

}  // namespace

void ScoutEngine() {
    Log("");
    Log("=== ScoutEngine: walking process address space ===");

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    BYTE* addr = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
    BYTE* const max_addr = static_cast<BYTE*>(si.lpMaximumApplicationAddress);

    std::vector<ScoredRegion> scored;
    int total_regions = 0;
    int readable_regions = 0;
    const HANDLE proc = GetCurrentProcess();

    while (addr < max_addr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0u) {
            break;
        }
        ++total_regions;

        BYTE* const base = static_cast<BYTE*>(mbi.BaseAddress);
        const std::size_t sz = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const DWORD typ = mbi.Type;

        const bool readable = (mbi.State == MEM_COMMIT) &&
                              (prot & PAGE_NOACCESS) == 0u &&
                              (prot & PAGE_GUARD) == 0u;
        if (readable && sz > 0u && sz <= kMaxScannedRegionBytes) {
            ++readable_regions;

            const int sig_count = SafeScanSignatures(base, sz);
            std::size_t prologue_count = 0u;
            if (sig_count > 0 || (prot & kExecutableProtectMask) != 0u) {
                prologue_count = SafeCountPrologues(base, sz);
            }

            if (sig_count > 0) {
                ScoredRegion r;
                r.base = base;
                r.size = sz;
                r.protect = prot;
                r.type = typ;
                r.sig_count = sig_count;
                r.prologue_count = prologue_count;
                r.module_path = QueryMappedFileNameWide(proc, base);
                scored.push_back(std::move(r));
            }
        }

        addr = base + sz;
        if (addr < base) {
            break;
        }
    }

    Log("Walked %d VAD entries (%d committed+readable).",
        total_regions, readable_regions);
    Log("Regions with at least one engine signature hit: %zu",
        scored.size());

    std::sort(scored.begin(), scored.end(),
              [](const ScoredRegion& a, const ScoredRegion& b) {
                  return a.sig_count > b.sig_count;
              });

    const std::size_t top_n = (std::min)(scored.size(), kMaxLoggedTopRegions);
    for (std::size_t i = 0u; i < top_n; ++i) {
        LogTopRegion(scored[i]);
    }

    const HMODULE exe_module = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    if (GetModuleInformation(proc, exe_module, &mi, sizeof(mi))) {
        Log("");
        Log("--- Sakura.exe module: base 0x%08X  size 0x%X  EP 0x%08X ---",
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll)),
            static_cast<DWORD>(mi.SizeOfImage),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(mi.EntryPoint)));
    }

    DumpEngine();

    Log("=== ScoutEngine: done ===");
    Log("");
}
