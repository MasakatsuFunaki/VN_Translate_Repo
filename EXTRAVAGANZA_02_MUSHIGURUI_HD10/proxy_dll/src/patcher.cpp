// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime CPrintMessage layout patcher and Vectored Exception Handler for
// Mushigurui (HD10, HD10_Win7, and Complete Edition).
#include "patcher.h"

#include "log.h"
#include "menu_autopatch.h"
#include "patcher_logic.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr DWORD kExceptionMsvcCpp = 0xE06D7363u;
constexpr DWORD kExceptionOutputDebugString = 0x40010006u;
constexpr DWORD kExceptionComNotInitialized = 0x800401F0u;

constexpr DWORD kMinValidUserAddress = 0x00010000u;
constexpr DWORD kMaxValidUserAddress = 0xFFF00000u;
constexpr DWORD kMinAccessViolationParams = 2;

constexpr int kCrashDumpStackDwords = 64;
constexpr int kMaxEbpWalkFrames = 16;
constexpr int kBytesBeforeEipToLog = 4;
constexpr int kBytesAfterEipToLog = 8;
constexpr std::size_t kEipLogBufferSize = 64;
constexpr std::size_t kDwordAsciiBufferSize = sizeof(DWORD) + 1;
constexpr BYTE kPrintableAsciiMin = 0x20u;
constexpr BYTE kPrintableAsciiMax = 0x7Eu;

constexpr int kUnpackPollMaxRetries = 3000;
constexpr DWORD kUnpackPollSleepMs = 10;
constexpr DWORD kPostUnpackSettleMs = 50;
constexpr DWORD kShutdownWaitTimeoutMs = 5000;
constexpr ULONG kVehCallFirst = 1;

HANDLE g_patchThread = nullptr;
volatile bool g_shutdownRequested = false;
PVOID g_vehHandle = nullptr;

patcher_logic::PeImageLayout g_mainPeLayout{};
bool g_hasMainPeLayout = false;

const char* ExceptionCodeName(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "FLT_DIVIDE_BY_ZERO";
    default:
        return "UNKNOWN";
    }
}

bool SafeReadDword(DWORD addr, DWORD* out) noexcept {
    __try {
        *out = *reinterpret_cast<const DWORD*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeParsePeLayout(const BYTE* base, patcher_logic::PeImageLayout* out) noexcept {
    __try {
        const auto parsed = patcher_logic::ParsePeImageLayout(
            base, patcher_logic::kDefaultPeHeaderMaxBytes);
        if (!parsed.has_value()) {
            return false;
        }
        *out = *parsed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FormatDwordAscii(DWORD val, char (&out)[kDwordAsciiBufferSize]) noexcept {
    const auto* bytes = reinterpret_cast<const BYTE*>(&val);
    for (std::size_t i = 0; i < sizeof(DWORD); ++i) {
        const BYTE b = bytes[i];
        out[i] = (b >= kPrintableAsciiMin && b <= kPrintableAsciiMax)
                     ? static_cast<char>(b)
                     : '.';
    }
    out[sizeof(DWORD)] = '\0';
}

void LogEipBytesSafe(const BYTE* ip) noexcept {
    __try {
        char line[kEipLogBufferSize] = {};
        std::size_t pos = 0;
        for (int offset = -kBytesBeforeEipToLog; offset < kBytesAfterEipToLog; ++offset) {
            const char* fmt = (offset == 0) ? "[%02X] " : "%02X ";
            const int written = std::snprintf(
                line + pos, sizeof(line) - pos, fmt, ip[offset]);
            if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(line) - pos) {
                break;
            }
            pos += static_cast<std::size_t>(written);
        }
        if (pos > 0 && line[pos - 1] == ' ') {
            line[pos - 1] = '\0';
        }
        Log("  Bytes at EIP-%d: %s", kBytesBeforeEipToLog, line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  (could not read bytes at EIP)");
    }
}

void LogStackTraceSafe(DWORD esp, DWORD base_addr, DWORD image_size) noexcept {
    __try {
        const auto* stack = reinterpret_cast<const DWORD*>(esp);
        for (int i = 0; i < kCrashDumpStackDwords; ++i) {
            const DWORD val = stack[i];
            const DWORD val_rva = val - base_addr;
            if (image_size > 0 && val >= base_addr && val_rva < image_size) {
                Log("    [ESP+%02X] = 0x%08X (RVA 0x%06X)",
                    i * static_cast<int>(sizeof(DWORD)), val, val_rva);
                continue;
            }
            if (val > kMinValidUserAddress && val < kMaxValidUserAddress) {
                DWORD deref = 0;
                if (SafeReadDword(val, &deref) && patcher_logic::LooksLikeAscii(deref)) {
                    char ascii_buf[kDwordAsciiBufferSize] = {};
                    FormatDwordAscii(deref, ascii_buf);
                    Log("    [ESP+%02X] = 0x%08X -> ASCII '%s'",
                        i * static_cast<int>(sizeof(DWORD)), val, ascii_buf);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("    (could not read stack)");
    }
}

void LogEbpChainSafe(DWORD initial_ebp, DWORD base_addr, DWORD image_size) noexcept {
    __try {
        DWORD ebp = initial_ebp;
        for (int depth = 0;
             depth < kMaxEbpWalkFrames &&
             ebp > kMinValidUserAddress &&
             ebp < kMaxValidUserAddress;
             ++depth) {
            DWORD ret_addr = 0;
            if (!SafeReadDword(ebp + sizeof(DWORD), &ret_addr)) {
                break;
            }
            const DWORD ret_rva = ret_addr - base_addr;
            const bool in_image =
                (image_size > 0 && ret_addr >= base_addr && ret_rva < image_size);
            if (in_image) {
                Log("    frame[%d] EBP=0x%08X ret=0x%08X (RVA 0x%06X)",
                    depth, ebp, ret_addr, ret_rva);
            }
            if (!in_image) {
                Log("    frame[%d] EBP=0x%08X ret=0x%08X", depth, ebp, ret_addr);
            }
            DWORD next_ebp = 0;
            if (!SafeReadDword(ebp, &next_ebp) || next_ebp <= ebp) {
                break;
            }
            ebp = next_ebp;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("    (EBP walk failed)");
    }
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == kExceptionMsvcCpp ||
        code == kExceptionOutputDebugString ||
        code == kExceptionComNotInitialized ||
        code == EXCEPTION_BREAKPOINT ||
        code == EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto base_addr = reinterpret_cast<std::uintptr_t>(base);
    const auto fault_addr =
        reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);

    if (g_hasMainPeLayout &&
        patcher_logic::IsBenignPackerSehException(
            static_cast<std::uint32_t>(code),
            fault_addr,
            base_addr,
            g_mainPeLayout)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        (!g_hasMainPeLayout ||
         !patcher_logic::IsAddressInsideSection(
             fault_addr, base_addr, g_mainPeLayout.text_section))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const CONTEXT* ctx = ep->ContextRecord;
    const DWORD rva = static_cast<DWORD>(fault_addr - base_addr);
    const DWORD image_size = g_hasMainPeLayout ? g_mainPeLayout.size_of_image : 0u;

    Log("!!! CRASH: %s (0x%08X) at VA 0x%p (RVA 0x%06X)",
        ExceptionCodeName(code),
        code,
        ep->ExceptionRecord->ExceptionAddress,
        rva);

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= kMinAccessViolationParams) {
        const char* op = ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ";
        const auto target_addr =
            static_cast<DWORD>(ep->ExceptionRecord->ExceptionInformation[1]);
        Log("  %s of address 0x%08X", op, target_addr);
    }

    Log("  EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X",
        ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    Log("  ESI=0x%08X EDI=0x%08X EBP=0x%08X ESP=0x%08X",
        ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    Log("  EIP=0x%08X EFLAGS=0x%08X", ctx->Eip, ctx->EFlags);

    LogEipBytesSafe(reinterpret_cast<const BYTE*>(ep->ExceptionRecord->ExceptionAddress));

    Log("  Register dereferences:");
    const struct {
        const char* name;
        DWORD val;
    } regs[] = {
        {"EAX", ctx->Eax}, {"EBX", ctx->Ebx}, {"ECX", ctx->Ecx},
        {"EDX", ctx->Edx}, {"ESI", ctx->Esi}, {"EDI", ctx->Edi},
    };
    for (const auto& r : regs) {
        if (r.val < kMinValidUserAddress) {
            continue;
        }
        DWORD deref = 0;
        if (!SafeReadDword(r.val, &deref)) {
            continue;
        }
        const DWORD deref_rva = deref - static_cast<DWORD>(base_addr);
        if (patcher_logic::LooksLikeAscii(deref)) {
            char ascii_buf[kDwordAsciiBufferSize] = {};
            FormatDwordAscii(deref, ascii_buf);
            Log("    [%s] = 0x%08X (ASCII: '%s')", r.name, deref, ascii_buf);
            continue;
        }
        if (image_size > 0 && deref >= static_cast<DWORD>(base_addr) && deref_rva < image_size) {
            Log("    [%s] = 0x%08X (RVA 0x%06X)", r.name, deref, deref_rva);
            continue;
        }
        Log("    [%s] = 0x%08X", r.name, deref);
    }

    Log("  Stack trace (full):");
    LogStackTraceSafe(ctx->Esp, static_cast<DWORD>(base_addr), image_size);

    Log("  Call chain (EBP walk):");
    LogEbpChainSafe(ctx->Ebp, static_cast<DWORD>(base_addr), image_size);

    if (g_logFile != nullptr) {
        std::fflush(g_logFile);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool PatchSpecSite(BYTE* base, const patcher_logic::PatchSpec& spec) {
    if (!patcher_logic::IsValidSpec(spec)) {
        Log("PATCH FAIL at RVA 0x%X: invalid PatchSpec", spec.rva);
        return false;
    }

    BYTE* addr = base + spec.rva;
    if (!patcher_logic::BytesMatch(addr, spec.expected, spec.len)) {
        Log("PATCH FAIL at RVA 0x%X: bytes mismatch", spec.rva);
        Log("  Expected[0]: %02X  Found[0]: %02X", spec.expected[0], addr[0]);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(addr, spec.len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        Log("PATCH FAIL at RVA 0x%X: VirtualProtect error %u",
            spec.rva, GetLastError());
        return false;
    }

    if (spec.replacement == nullptr) {
        patcher_logic::FillNops(addr, spec.len);
    } else {
        std::memcpy(addr, spec.replacement, spec.len);
    }

    VirtualProtect(addr, spec.len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), addr, spec.len);
    Log("PATCH OK at RVA 0x%X (%zu bytes)", spec.rva, spec.len);
    return true;
}

void ApplyPrintMessagePatches(BYTE* base,
                              const patcher_logic::ResolvedPrintMessageTarget& target) {
    const auto specs = patcher_logic::BuildPrintMessagePatchSpecs(
        *target.profile, target.sentinel_rva);
    int ok = 0;
    int fail = 0;
    for (const patcher_logic::PatchSpec& spec : specs) {
        if (PatchSpecSite(base, spec)) {
            ++ok;
            continue;
        }
        ++fail;
    }
    Log("Patches applied (%s, sentinel=0x%06X): %d OK, %d FAILED",
        target.profile->name, target.sentinel_rva, ok, fail);
}

bool TryResolveUnpackedPrintMessage(
    const BYTE* base,
    patcher_logic::SectionSpan text_span,
    std::uint32_t time_date_stamp,
    patcher_logic::ResolvedPrintMessageTarget* out) noexcept {
    __try {
        const auto resolved = patcher_logic::ResolvePrintMessageTarget(
            base, text_span, time_date_stamp);
        if (!resolved.has_value()) {
            return false;
        }
        *out = *resolved;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI PatcherThread(LPVOID) {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    patcher_logic::SectionSpan text_span{
        patcher_logic::kDefaultTextSectionRva,
        patcher_logic::kHd10TextVirtualSize,
    };
    std::uint32_t time_date_stamp = 0;
    if (g_hasMainPeLayout) {
        text_span = g_mainPeLayout.text_section;
        time_date_stamp = g_mainPeLayout.time_date_stamp;
    }

    Log("Patcher: ImageBase=0x%p, .text=[0x%06X..0x%06X), TDS=0x%08X, waiting for unpack...",
        base,
        text_span.rva,
        text_span.rva + text_span.virtual_size,
        time_date_stamp);

    for (int i = 0; i < kUnpackPollMaxRetries && !g_shutdownRequested; ++i) {
        patcher_logic::ResolvedPrintMessageTarget resolved{};
        if (TryResolveUnpackedPrintMessage(base, text_span, time_date_stamp, &resolved)) {
            Log("Patcher: matched edition [%s] at iteration %d (sentinel=0x%06X, scanFallback=%s)",
                resolved.profile->name,
                i,
                resolved.sentinel_rva,
                resolved.used_scan_fallback ? "yes" : "no");
            Sleep(kPostUnpackSettleMs);
            ApplyPrintMessagePatches(base, resolved);
            return 0;
        }
        Sleep(kUnpackPollSleepMs);
    }

    Log("Patcher: TIMEOUT - no supported edition PrintMessage sentinel unpacked");
    return 1;
}

}  // namespace

void PatcherInit() {
    (void)menu_autopatch::EnsureGameFilesPatched();

    const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    g_hasMainPeLayout = SafeParsePeLayout(base, &g_mainPeLayout);

    g_vehHandle = AddVectoredExceptionHandler(kVehCallFirst, CrashHandler);
    Log("Crash handler installed: %s", g_vehHandle != nullptr ? "OK" : "FAILED");

    g_shutdownRequested = false;
    g_patchThread = CreateThread(nullptr, 0, PatcherThread, nullptr, 0, nullptr);
    if (g_patchThread == nullptr) {
        Log("Patcher: Failed to create thread, error %u", GetLastError());
    }
}

void PatcherShutdown() {
    g_shutdownRequested = true;
    if (g_patchThread != nullptr) {
        WaitForSingleObject(g_patchThread, kShutdownWaitTimeoutMs);
        CloseHandle(g_patchThread);
        g_patchThread = nullptr;
    }
    if (g_vehHandle != nullptr) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
