// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Runtime translator for AGEHA ~EXTRAVAGANZA~ CE (nnn/BCmkri engine).
//
// Hooks FUN_004FF140 (TextSplitter, RVA 0x0FF140) to intercept the CP932 text
// string ([esp+0x0C] at function entry) and replace it with the English
// translation from translation_table.tsv.
#include "translator.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "patcher_logic.h"
#include "translator_logic.h"

namespace {

constexpr int kTransPoolSlots = 64;
constexpr std::size_t kTransSlotSize = 4096;
constexpr int kInitialLogHitLimit = 20;
constexpr int kPeriodicLogHitInterval = 200;
constexpr std::size_t kTrampolineAllocBytes = 256;
constexpr int kUnpackPollMaxRetries = 3000;
constexpr DWORD kUnpackPollSleepMs = 10;
constexpr DWORD kPostUnpackSettleMs = 50;
constexpr DWORD kThreadShutdownWaitMs = 2000;
constexpr DWORD kMinAccessViolationParams = 2;

class ScopedFile {
public:
    explicit ScopedFile(std::FILE* fp) noexcept : fp_(fp) {}
    ~ScopedFile() {
        if (fp_ != nullptr) {
            std::fclose(fp_);
        }
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    std::FILE* get() const noexcept { return fp_; }
    explicit operator bool() const noexcept { return fp_ != nullptr; }

private:
    std::FILE* fp_;
};

constexpr BYTE kOpPushEcx = 0x51;
constexpr BYTE kOpPushad = 0x60;
constexpr BYTE kOpMovRegDisp8Base = 0x8B;
constexpr BYTE kOpMovDisp8RegBase = 0x89;
constexpr BYTE kModRmEaxEspDisp8 = 0x44;
constexpr BYTE kSibEsp = 0x24;
constexpr BYTE kStackOffsetTextArg = 0x30;
constexpr BYTE kOpPushEax = 0x50;
constexpr BYTE kOpAddEspImm8_0 = 0x83;
constexpr BYTE kOpAddEspImm8_1 = 0xC4;
constexpr BYTE kCdeclArgBytes = 0x04;
constexpr BYTE kOpPopad = 0x61;
constexpr BYTE kOpPopEcx = 0x59;

std::unordered_map<std::string, std::string> g_translations;
bool g_translationsLoaded = false;

BYTE* g_trampoline = nullptr;
BYTE* g_hookSite = nullptr;
HANDLE g_translatorThread = nullptr;
volatile bool g_shutdownRequested = false;
PVOID g_vehHandle = nullptr;

int g_hookHits = 0;
int g_translationHits = 0;

char g_transPool[kTransPoolSlots][kTransSlotSize];
int g_transPoolIdx = 0;

LONG WINAPI TranslatorVEH(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW) {
            const DWORD eip = ep->ContextRecord->Eip;
            const DWORD esp = ep->ContextRecord->Esp;
            const DWORD ecx = ep->ContextRecord->Ecx;
            const auto trampolineAddr = reinterpret_cast<DWORD>(g_trampoline);
            const auto hookAddr = reinterpret_cast<DWORD>(g_hookSite);
            Log("=== CRASH DETECTED ===");
            Log("  ExceptionCode: 0x%08X", code);
            Log("  EIP: 0x%08X  ESP: 0x%08X  ECX: 0x%08X", eip, esp, ecx);
            Log("  EAX: 0x%08X  EBX: 0x%08X  EDX: 0x%08X",
                ep->ContextRecord->Eax, ep->ContextRecord->Ebx, ep->ContextRecord->Edx);
            Log("  Trampoline: 0x%08X  HookSite: 0x%08X", trampolineAddr, hookAddr);
            Log("  Hook hits: %d  Translation hits: %d", g_hookHits, g_translationHits);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                ep->ExceptionRecord->NumberParameters >= kMinAccessViolationParams) {
                Log("  Access type: %s  Address: 0x%08X",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                    static_cast<DWORD>(ep->ExceptionRecord->ExceptionInformation[1]));
            }
            __try {
                auto* stackPtr = reinterpret_cast<DWORD*>(esp);
                Log("  Stack: %08X %08X %08X %08X %08X %08X %08X %08X",
                    stackPtr[0], stackPtr[1], stackPtr[2], stackPtr[3],
                    stackPtr[4], stackPtr[5], stackPtr[6], stackPtr[7]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("  Stack: <unreadable>");
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool LoadTranslations() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        wcscpy(lastSlash + 1, L"translation_table.tsv");
    } else {
        wcscpy(exePath, L"translation_table.tsv");
    }

    const ScopedFile f(_wfopen(exePath, L"rb"));
    if (!f) {
        Log("Translator: translation_table.tsv not found");
        char pathA[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, exePath, -1, pathA, MAX_PATH, nullptr, nullptr);
        Log("  Tried: %s", pathA);
        return false;
    }

    std::fseek(f.get(), 0, SEEK_END);
    const long size = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);

    if (size <= 0) {
        return false;
    }

    std::vector<char> buf(static_cast<std::size_t>(size));
    const std::size_t nread = std::fread(buf.data(), 1, buf.size(), f.get());

    const std::size_t bomLen = translator_logic::Utf8BomLen(buf.data(), nread);
    const int count = translator_logic::ParseTsvBuffer(
        buf.data() + bomLen, nread - bomLen, g_translations);

    Log("Translator: loaded %d translations from translation_table.tsv", count);
    g_translationsLoaded = true;
    return count > 0;
}

char* AllocTransSlot() {
    const int idx = g_transPoolIdx;
    g_transPoolIdx = (g_transPoolIdx + 1) % kTransPoolSlots;
    return g_transPool[idx];
}

bool IsReadablePtr(const void* ptr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

const char* TranslateTextInner(const char* originalText) {
    if (!g_translationsLoaded || !originalText) {
        return originalText;
    }

    if (!IsReadablePtr(originalText)) {
        Log("Translator: INVALID ptr 0x%p at hit %d", originalText, g_hookHits);
        return originalText;
    }

    if (!originalText[0]) {
        return originalText;
    }

    ++g_hookHits;

    if (g_hookHits <= kInitialLogHitLimit || (g_hookHits % kPeriodicLogHitInterval) == 0) {
        Log("Hook #%d: ptr=0x%p text=[%.60s]", g_hookHits, originalText, originalText);
    }

    const auto it = g_translations.find(originalText);
    if (it == g_translations.end()) {
        return originalText;
    }

    ++g_translationHits;

    char* slot = AllocTransSlot();
    std::size_t len = it->second.size();
    if (len >= kTransSlotSize) len = kTransSlotSize - 1;
    std::memcpy(slot, it->second.c_str(), len);
    slot[len] = '\0';

    if (g_translationHits <= kInitialLogHitLimit ||
        (g_translationHits % kPeriodicLogHitInterval) == 0) {
        Log("Translate #%d: [%.50s] -> [%.50s]",
            g_translationHits, originalText, slot);
    }
    return slot;
}

const char* __cdecl TranslateText(const char* originalText) {
    __try {
        return TranslateTextInner(originalText);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in TranslateText, ptr=0x%p hit=%d",
            originalText, g_hookHits);
        return originalText;
    }
}

bool InstallTextHook(BYTE* base) {
    const DWORD hookRVA = patcher_logic::kTextSplitterRva;
    const std::size_t hookLen = patcher_logic::kTextSplitterHookLen;
    g_hookSite = base + hookRVA;
    BYTE* returnAddr = g_hookSite + hookLen;

    const BYTE* expected = patcher_logic::kTextSplitterPrologue;
    if (!patcher_logic::BytesMatch(g_hookSite, expected, hookLen)) {
        Log("TEXT HOOK: bytes mismatch at RVA 0x%X", hookRVA);
        Log("  Expected: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            expected[0], expected[1], expected[2], expected[3],
            expected[4], expected[5], expected[6], expected[7], expected[8]);
        Log("  Found:    %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            g_hookSite[0], g_hookSite[1], g_hookSite[2], g_hookSite[3],
            g_hookSite[4], g_hookSite[5], g_hookSite[6], g_hookSite[7], g_hookSite[8]);
        return false;
    }

    g_trampoline = static_cast<BYTE*>(
        VirtualAlloc(nullptr, kTrampolineAllocBytes, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!g_trampoline) {
        Log("TEXT HOOK: VirtualAlloc failed");
        return false;
    }

    BYTE* p = g_trampoline;

    *p++ = kOpPushEcx;
    *p++ = kOpPushad;

    *p++ = kOpMovRegDisp8Base;
    *p++ = kModRmEaxEspDisp8;
    *p++ = kSibEsp;
    *p++ = kStackOffsetTextArg;

    *p++ = kOpPushEax;

    p = patcher_logic::EmitRelCall32(p, reinterpret_cast<std::uintptr_t>(&TranslateText));

    *p++ = kOpAddEspImm8_0;
    *p++ = kOpAddEspImm8_1;
    *p++ = kCdeclArgBytes;

    *p++ = kOpMovDisp8RegBase;
    *p++ = kModRmEaxEspDisp8;
    *p++ = kSibEsp;
    *p++ = kStackOffsetTextArg;

    *p++ = kOpPopad;
    *p++ = kOpPopEcx;

    p = patcher_logic::EmitBytes(p, expected, hookLen);
    p = patcher_logic::EmitRelJmp32(p, reinterpret_cast<std::uintptr_t>(returnAddr));

    DWORD oldProtect = 0;
    if (!VirtualProtect(g_hookSite, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("TEXT HOOK: VirtualProtect failed");
        return false;
    }

    patcher_logic::EmitRelJmp32(g_hookSite, reinterpret_cast<std::uintptr_t>(g_trampoline));
    patcher_logic::FillNops(
        g_hookSite + patcher_logic::kRelBranchInstructionSize,
        hookLen - patcher_logic::kRelBranchInstructionSize);

    VirtualProtect(g_hookSite, hookLen, oldProtect, &oldProtect);

    Log("TEXT HOOK: installed at RVA 0x%X (trampoline at 0x%p)", hookRVA, g_trampoline);
    return true;
}

DWORD WINAPI TranslatorThread(LPVOID) {
    LoadTranslations();
    if (!g_translationsLoaded) {
        Log("Translator: no translations loaded, hook not installed");
        return 0;
    }

    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    Log("Translator: ImageBase=0x%p, waiting for code to unpack...", base);

    for (int i = 0; i < kUnpackPollMaxRetries && !g_shutdownRequested; ++i) {
        __try {
            if (patcher_logic::BytesMatch(
                    base + patcher_logic::kTextSplitterRva,
                    patcher_logic::kTextSplitterSentinel,
                    patcher_logic::kTextSplitterSentinelLen)) {
                Log("Translator: code unpacked (iteration %d)", i);
                Sleep(kPostUnpackSettleMs);
                if (InstallTextHook(base)) {
                    Log("Translator: runtime translation ACTIVE (%d entries)",
                        static_cast<int>(g_translations.size()));
                } else {
                    Log("Translator: hook installation FAILED");
                }
                return 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        Sleep(kUnpackPollSleepMs);
    }

    Log("Translator: TIMEOUT waiting for code to unpack");
    return 0;
}

}  // namespace

void TranslatorInit() {
    g_vehHandle = AddVectoredExceptionHandler(1, TranslatorVEH);
    g_translatorThread = CreateThread(nullptr, 0, TranslatorThread, nullptr, 0, nullptr);
}

void TranslatorShutdown() {
    g_shutdownRequested = true;
    if (g_translatorThread) {
        WaitForSingleObject(g_translatorThread, kThreadShutdownWaitMs);
        CloseHandle(g_translatorThread);
        g_translatorThread = nullptr;
    }

    if (g_translationsLoaded) {
        Log("Translator stats: %d hook hits, %d translations applied",
            g_hookHits, g_translationHits);
    }

    if (g_trampoline) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
    }

    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
