// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "key_dumper.h"

#include "log.h"

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
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// Optional developer-only key recovery probe gated behind ADIEU_DUMP_KEY=1.
// Disabled unconditionally during normal player sessions.
constexpr uintptr_t kLzssDecompressRva = 0x002D1A80u;
constexpr int kMaxCaptures = 4;
constexpr std::size_t kCaptureBytes = 256u;
constexpr DWORD kInitialModulePathChars = MAX_PATH;
constexpr DWORD kMaxModulePathChars = 32768u;
constexpr uint32_t kMaxPeHeaderOffset = 0x1000u;
constexpr wchar_t kKeyDumpEnvVar[] = L"ADIEU_DUMP_KEY";
constexpr wchar_t kKeyDumpFileName[] = L"siglus_key_dump.txt";
constexpr uint8_t kExpectedPrologue[6] = {
    0x55u, 0x8Bu, 0xECu, 0x83u, 0xECu, 0x10u,
};
constexpr uint8_t kInt3Opcode = 0xCCu;
constexpr DWORD kTrapFlagMask = 0x100u;

bool g_key_dumper_armed = false;
uint8_t* g_hook_addr = nullptr;
uint8_t g_orig_byte = 0u;
PVOID g_veh_handle = nullptr;
std::unique_ptr<std::FILE, decltype(&std::fclose)> g_dump_file(nullptr,
                                                               &std::fclose);
CRITICAL_SECTION g_cs{};
int g_capture_count = 0;
uint8_t* g_rearm_addr = nullptr;

bool IsKeyDumpRequested() noexcept {
    wchar_t buf[8] = {};
    const DWORD len = ::GetEnvironmentVariableW(kKeyDumpEnvVar, buf, 8u);
    return len == 1u && buf[0] == L'1';
}

std::size_t SafeRead(const void* src, void* dst, std::size_t n) noexcept {
    SIZE_T copied = 0u;
    ::ReadProcessMemory(::GetCurrentProcess(), src, dst, n, &copied);
    return copied;
}

bool PokeByte(uint8_t* addr, uint8_t value) noexcept {
    DWORD old_prot = 0u;
    if (!::VirtualProtect(addr, 1u, PAGE_EXECUTE_READWRITE, &old_prot)) {
        return false;
    }
    *addr = value;
    DWORD dummy = 0u;
    ::VirtualProtect(addr, 1u, old_prot, &dummy);
    ::FlushInstructionCache(::GetCurrentProcess(), addr, 1u);
    return true;
}

std::wstring ResolveSiblingPathW(const wchar_t* sibling_name) {
    if (sibling_name == nullptr || *sibling_name == L'\0') {
        return {};
    }
    std::vector<wchar_t> buf(kInitialModulePathChars, L'\0');
    DWORD len = 0u;
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        len = ::GetModuleFileNameW(nullptr, buf.data(),
                                   static_cast<DWORD>(buf.size()));
        if (len == 0u) {
            return {};
        }
        if (len < buf.size() - 1u &&
            ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        if (buf.size() >= kMaxModulePathChars) {
            return {};
        }
        buf.assign(
            std::min<std::size_t>(buf.size() * 2u, kMaxModulePathChars),
            L'\0');
    }

    std::wstring full_path(buf.data(), len);
    const std::size_t sep = full_path.find_last_of(L"\\/");
    if (sep != std::wstring::npos) {
        full_path.resize(sep + 1u);
        full_path += sibling_name;
        return full_path;
    }
    return std::wstring(sibling_name);
}

void EnsureDumpFile() {
    if (g_dump_file) {
        return;
    }
    const std::wstring dump_path = ResolveSiblingPathW(kKeyDumpFileName);
    if (dump_path.empty()) {
        return;
    }
    g_dump_file.reset(_wfopen(dump_path.c_str(), L"w"));
}

bool IsRvaInExecutableImage(const uint8_t* base,
                            uintptr_t rva,
                            std::size_t len) noexcept {
    if (base == nullptr) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<uint32_t>(dos->e_lfanew) > kMaxPeHeaderOffset) {
        return false;
    }
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }
    return (rva + len) <= nt->OptionalHeader.SizeOfImage;
}

LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    auto* fault_addr =
        static_cast<uint8_t*>(ep->ExceptionRecord->ExceptionAddress);

    if (code == EXCEPTION_SINGLE_STEP) {
        if (g_rearm_addr != nullptr) {
            PokeByte(g_rearm_addr, kInt3Opcode);
            g_rearm_addr = nullptr;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code != EXCEPTION_BREAKPOINT || fault_addr != g_hook_addr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    ::EnterCriticalSection(&g_cs);

    const uint32_t ecx = ep->ContextRecord->Ecx;
    const uint32_t edx = ep->ContextRecord->Edx;
    const uint32_t eip = ep->ContextRecord->Eip;

    uint8_t buf[kCaptureBytes] = {};
    std::size_t copied = 0u;
    if (ecx != 0u) {
        copied = SafeRead(reinterpret_cast<const void*>(
                              static_cast<uintptr_t>(ecx)),
                          buf, sizeof(buf));
    }

    if (copied >= 8u) {
        uint32_t comp_size = 0u;
        uint32_t decomp_size = 0u;
        std::memcpy(&comp_size, buf, sizeof(comp_size));
        std::memcpy(&decomp_size, buf + 4u, sizeof(decomp_size));

        EnsureDumpFile();
        if (std::FILE* dump = g_dump_file.get()) {
            std::fprintf(dump,
                         "CAPTURE #%d\n"
                         "  ecx=0x%08X edx=0x%08X eip=0x%08X\n"
                         "  comp_size  = 0x%08X (%u)\n"
                         "  decomp_size= 0x%08X (%u)\n"
                         "  bytes_read = %zu\n"
                         "  hex:",
                         g_capture_count, ecx, edx, eip, comp_size, comp_size,
                         decomp_size, decomp_size, copied);
            for (std::size_t i = 0u; i < copied; ++i) {
                if ((i & 0x1Fu) == 0u) {
                    std::fprintf(dump, "\n    ");
                }
                std::fprintf(dump, "%02X", buf[i]);
            }
            std::fprintf(dump, "\n\n");
            std::fflush(dump);
        }
        ++g_capture_count;
    }

    PokeByte(g_hook_addr, g_orig_byte);
    ep->ContextRecord->Eip =
        static_cast<DWORD>(reinterpret_cast<uintptr_t>(fault_addr));

    if (g_capture_count < kMaxCaptures) {
        g_rearm_addr = g_hook_addr;
        ep->ContextRecord->EFlags |= kTrapFlagMask;
    } else {
        g_rearm_addr = nullptr;
        if (g_veh_handle != nullptr) {
            ::RemoveVectoredExceptionHandler(g_veh_handle);
            g_veh_handle = nullptr;
        }
    }

    ::LeaveCriticalSection(&g_cs);
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void KeyDumperInit() {
    if (!IsKeyDumpRequested()) {
        return;
    }

    auto* base = reinterpret_cast<uint8_t*>(::GetModuleHandleW(nullptr));
    if (!IsRvaInExecutableImage(base, kLzssDecompressRva,
                                sizeof(kExpectedPrologue))) {
        Log("[KeyDumper] Host image too small for RVA 0x%X — skipping",
            static_cast<unsigned int>(kLzssDecompressRva));
        return;
    }

    uint8_t* candidate = base + kLzssDecompressRva;
    if (std::memcmp(candidate, kExpectedPrologue,
                    sizeof(kExpectedPrologue)) != 0) {
        Log("[KeyDumper] Prologue mismatch at RVA 0x%X — skipping",
            static_cast<unsigned int>(kLzssDecompressRva));
        return;
    }

    ::InitializeCriticalSection(&g_cs);
    g_key_dumper_armed = true;
    g_hook_addr = candidate;
    g_orig_byte = g_hook_addr[0];

    g_veh_handle = ::AddVectoredExceptionHandler(1u, VehHandler);
    if (g_veh_handle == nullptr) {
        Log("[KeyDumper] AddVectoredExceptionHandler failed (gle=%lu)",
            ::GetLastError());
        return;
    }

    if (!PokeByte(g_hook_addr, kInt3Opcode)) {
        ::RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = nullptr;
        return;
    }

    Log("[KeyDumper] Armed developer key capture at RVA 0x%X",
        static_cast<unsigned int>(kLzssDecompressRva));
}

void KeyDumperShutdown() {
    if (!g_key_dumper_armed) {
        return;
    }

    ::EnterCriticalSection(&g_cs);
    if (g_veh_handle != nullptr) {
        ::RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = nullptr;
    }
    if (g_hook_addr != nullptr && g_orig_byte != 0u) {
        PokeByte(g_hook_addr, g_orig_byte);
    }
    g_dump_file.reset();
    ::LeaveCriticalSection(&g_cs);
    ::DeleteCriticalSection(&g_cs);
    g_key_dumper_armed = false;
}
