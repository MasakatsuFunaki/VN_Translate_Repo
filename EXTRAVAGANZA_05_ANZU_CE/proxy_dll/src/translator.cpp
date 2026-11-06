// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Runtime translator for Welcome to AnzU! CE (System-NNN / BCmkri engine).
//
// Hooks TextSplitter to intercept the CP932 text string ([esp+0x0C] at function
// entry) and replace it with the English translation from translation_table.tsv.
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
constexpr BYTE kOpLeaRegDisp8Base = 0x8D;
constexpr BYTE kModRmEaxEspDisp8 = 0x44;
constexpr BYTE kSibEsp = 0x24;
constexpr BYTE kStackOffsetTextArg = 0x30;
constexpr BYTE kStackOffsetRenderCoreArgs = 0x28;
constexpr BYTE kOpPushEax = 0x50;
constexpr BYTE kOpAddEspImm8_0 = 0x83;
constexpr BYTE kOpAddEspImm8_1 = 0xC4;
constexpr BYTE kCdeclArgBytes = 0x04;
constexpr BYTE kCdeclTwoArgsBytes = 0x08;
constexpr BYTE kOpPopad = 0x61;
constexpr BYTE kOpPopEcx = 0x59;

constexpr std::size_t kArgIdxStartChar = 0;
constexpr std::size_t kArgIdxMaxChars = 1;
constexpr std::size_t kArgIdxY = 3;
constexpr std::size_t kArgIdxText = 4;
constexpr std::size_t kArgIdxFontSize = 5;
constexpr std::size_t kArgIdxSpace = 9;
constexpr std::size_t kArgIdxBoldFlag = 12;
constexpr DWORD kExpandedMaxChars = 1000;
constexpr int kWindowTitlePollRetries = 50;
constexpr DWORD kWindowTitlePollSleepMs = 50;

using FontSetupFn = void(__thiscall*)(void* fontMgr, int fontSize, int boldFlag);

std::unordered_map<std::string, std::string> g_translations;
bool g_translationsLoaded = false;

BYTE* g_trampoline = nullptr;
BYTE* g_hookSite = nullptr;
BYTE* g_renderCoreTrampoline = nullptr;
BYTE* g_renderCoreHookSite = nullptr;
FontSetupFn g_fontSetupFn = nullptr;
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

const char* LookupAndStoreTranslation(const char* originalText,
                                      bool singleLineMode,
                                      bool* outWasTranslated) {
    if (outWasTranslated) {
        *outWasTranslated = false;
    }
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

    std::string translated;
    if (!translator_logic::LookupTranslation(
            g_translations, originalText, singleLineMode, translated)) {
        return originalText;
    }

    ++g_translationHits;
    if (outWasTranslated) {
        *outWasTranslated = true;
    }

    char* slot = AllocTransSlot();
    std::size_t len = translated.size();
    if (len >= kTransSlotSize) len = kTransSlotSize - 1;
    std::memcpy(slot, translated.c_str(), len);
    slot[len] = '\0';

    if (g_translationHits <= kInitialLogHitLimit ||
        (g_translationHits % kPeriodicLogHitInterval) == 0) {
        Log("Translate #%d: [%.50s] -> [%.50s]",
            g_translationHits, originalText, slot);
    }
    return slot;
}

const char* TranslateTextInner(const char* originalText) {
    return LookupAndStoreTranslation(originalText, false, nullptr);
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

void OnTextRenderCoreInner(void* self, DWORD* args) {
    if (!args || !IsReadablePtr(args)) {
        return;
    }

    const auto* origText = reinterpret_cast<const char*>(args[kArgIdxText]);
    const int curFontSize = static_cast<int>(args[kArgIdxFontSize]);
    const int curSpace = static_cast<int>(args[kArgIdxSpace]);
    const int curMaxChars = static_cast<int>(args[kArgIdxMaxChars]);

    bool wasTranslated = false;
    const char* currentText =
        LookupAndStoreTranslation(origText, curSpace > 0, &wasTranslated);
    if (wasTranslated) {
        args[kArgIdxText] = reinterpret_cast<DWORD>(currentText);
    }

    const translator_logic::UiFontAdjustment adj =
        translator_logic::ComputeUiFontAdjustment(
            currentText, wasTranslated, curFontSize, curSpace, curMaxChars);
    if (!adj.shouldAdjust) {
        return;
    }

    if (g_fontSetupFn && self && IsReadablePtr(self)) {
        void* fontMgr = *reinterpret_cast<void**>(self);
        if (fontMgr && IsReadablePtr(fontMgr)) {
            auto* activeFlag = reinterpret_cast<int*>(
                static_cast<char*>(fontMgr) + patcher_logic::kFontMgrActiveFlagOffset);
            *activeFlag = 0;
            g_fontSetupFn(fontMgr, adj.newFontSize, static_cast<int>(args[kArgIdxBoldFlag]));
            args[kArgIdxFontSize] = static_cast<DWORD>(adj.newFontSize);
        }
    }

    args[kArgIdxSpace] = static_cast<DWORD>(adj.newSpace);
    args[kArgIdxY] = static_cast<DWORD>(static_cast<int>(args[kArgIdxY]) + adj.yOffset);
    if (adj.expandMaxChars) {
        args[kArgIdxStartChar] = 0;
        args[kArgIdxMaxChars] = kExpandedMaxChars;
    }
}

void __cdecl OnTextRenderCore(void* self, DWORD* args) {
    __try {
        OnTextRenderCoreInner(self, args);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in OnTextRenderCore, self=0x%p args=0x%p", self, args);
    }
}

bool InstallTextHook(BYTE* base, DWORD hookRVA) {
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

bool InstallRenderCoreHook(BYTE* base, DWORD splitterRva) {
    const DWORD renderCoreRva =
        (splitterRva == patcher_logic::kAltTextSplitterRva)
            ? patcher_logic::kAltTextRenderCoreRva
            : patcher_logic::kTextRenderCoreRva;
    const DWORD fontSetupRva =
        (splitterRva == patcher_logic::kAltTextSplitterRva)
            ? patcher_logic::kAltFontSetupRva
            : patcher_logic::kFontSetupRva;

    const std::size_t hookLen = patcher_logic::kTextRenderCoreHookLen;
    g_renderCoreHookSite = base + renderCoreRva;
    BYTE* returnAddr = g_renderCoreHookSite + hookLen;

    if (!patcher_logic::MatchesTextRenderCorePrologue(g_renderCoreHookSite)) {
        Log("RENDER CORE HOOK: prologue mismatch at RVA 0x%X", renderCoreRva);
        return false;
    }

    if (patcher_logic::BytesMatch(base + fontSetupRva,
                                  patcher_logic::kFontSetupPrologue,
                                  patcher_logic::kFontSetupPrologueLen)) {
        g_fontSetupFn = reinterpret_cast<FontSetupFn>(base + fontSetupRva);
    } else {
        Log("RENDER CORE HOOK: FontSetup prologue mismatch at RVA 0x%X", fontSetupRva);
    }

    g_renderCoreTrampoline = static_cast<BYTE*>(
        VirtualAlloc(nullptr, kTrampolineAllocBytes, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!g_renderCoreTrampoline) {
        Log("RENDER CORE HOOK: VirtualAlloc failed");
        return false;
    }

    BYTE* p = g_renderCoreTrampoline;
    *p++ = kOpPushEcx;
    *p++ = kOpPushad;

    *p++ = kOpLeaRegDisp8Base;
    *p++ = kModRmEaxEspDisp8;
    *p++ = kSibEsp;
    *p++ = kStackOffsetRenderCoreArgs;

    *p++ = kOpPushEax;
    *p++ = kOpPushEcx;

    p = patcher_logic::EmitRelCall32(p, reinterpret_cast<std::uintptr_t>(&OnTextRenderCore));

    *p++ = kOpAddEspImm8_0;
    *p++ = kOpAddEspImm8_1;
    *p++ = kCdeclTwoArgsBytes;

    *p++ = kOpPopad;
    *p++ = kOpPopEcx;

    p = patcher_logic::EmitBytes(p, g_renderCoreHookSite, hookLen);
    p = patcher_logic::EmitRelJmp32(p, reinterpret_cast<std::uintptr_t>(returnAddr));

    DWORD oldProtect = 0;
    if (!VirtualProtect(g_renderCoreHookSite, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("RENDER CORE HOOK: VirtualProtect failed");
        return false;
    }

    patcher_logic::EmitRelJmp32(
        g_renderCoreHookSite, reinterpret_cast<std::uintptr_t>(g_renderCoreTrampoline));
    patcher_logic::FillNops(
        g_renderCoreHookSite + patcher_logic::kRelBranchInstructionSize,
        hookLen - patcher_logic::kRelBranchInstructionSize);

    VirtualProtect(g_renderCoreHookSite, hookLen, oldProtect, &oldProtect);

    Log("RENDER CORE HOOK: installed at RVA 0x%X (FontSetup=0x%X, trampoline=0x%p)",
        renderCoreRva, fontSetupRva, g_renderCoreTrampoline);
    return true;
}

struct WindowTitleUpdateCtx {
    DWORD targetPid = 0;
    const char* title = nullptr;
    bool updated = false;
};

BOOL CALLBACK SetProcessWindowTitleEnumProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowTitleUpdateCtx*>(lParam);
    if (!ctx || !ctx->title) {
        return FALSE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->targetPid) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    if (SetWindowTextA(hwnd, ctx->title)) {
        ctx->updated = true;
        return FALSE;
    }
    return TRUE;
}

void ApplyFullWindowTitle() {
    wchar_t fxfPath[MAX_PATH];
    GetModuleFileNameW(nullptr, fxfPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(fxfPath, L'\\');
    if (lastSlash) {
        wcscpy(lastSlash + 1, L"init2\\mainControl.fxf");
    } else {
        wcscpy(fxfPath, L"init2\\mainControl.fxf");
    }

    const ScopedFile f(_wfopen(fxfPath, L"rb"));
    if (!f) {
        return;
    }
    std::fseek(f.get(), 0, SEEK_END);
    const long size = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    if (size <= 0) {
        return;
    }

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    const std::size_t nread = std::fread(buf.data(), 1, buf.size(), f.get());
    const std::string title = translator_logic::ExtractFxfWindowTitle(buf.data(), nread);
    if (title.empty()) {
        return;
    }

    WindowTitleUpdateCtx ctx{GetCurrentProcessId(), title.c_str(), false};
    for (int i = 0; i < kWindowTitlePollRetries && !g_shutdownRequested; ++i) {
        EnumWindows(SetProcessWindowTitleEnumProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.updated) {
            Log("Translator: set full window title to [%s]", title.c_str());
            return;
        }
        Sleep(kWindowTitlePollSleepMs);
    }
}

DWORD FindUnpackedTextSplitterRva(BYTE* base) {
    if (patcher_logic::BytesMatch(base + patcher_logic::kTextSplitterRva,
                                  patcher_logic::kTextSplitterPrologue,
                                  patcher_logic::kTextSplitterHookLen)) {
        return patcher_logic::kTextSplitterRva;
    }
    if (patcher_logic::BytesMatch(base + patcher_logic::kFallbackTextSplitterRva,
                                  patcher_logic::kTextSplitterPrologue,
                                  patcher_logic::kTextSplitterHookLen)) {
        return patcher_logic::kFallbackTextSplitterRva;
    }
    if (patcher_logic::BytesMatch(base + patcher_logic::kAltTextSplitterRva,
                                  patcher_logic::kTextSplitterPrologue,
                                  patcher_logic::kTextSplitterHookLen)) {
        return patcher_logic::kAltTextSplitterRva;
    }
    for (DWORD rva = patcher_logic::kTextSectionScanStartRva;
         rva + patcher_logic::kPrintMessageSentinelLen <= patcher_logic::kTextSectionScanEndRva;
         ++rva) {
        if (patcher_logic::BytesMatch(base + rva,
                                      patcher_logic::kPrintMessageSentinel,
                                      patcher_logic::kPrintMessageSentinelLen)) {
            if (rva >= patcher_logic::kRelTextSplitterFromSentinel) {
                const DWORD candRva = rva - patcher_logic::kRelTextSplitterFromSentinel;
                if (patcher_logic::BytesMatch(base + candRva,
                                              patcher_logic::kTextSplitterPrologue,
                                              patcher_logic::kTextSplitterHookLen)) {
                    return candRva;
                }
            }
            if (rva >= patcher_logic::kAltRelTextSplitterFromSentinel) {
                const DWORD altCandRva = rva - patcher_logic::kAltRelTextSplitterFromSentinel;
                if (patcher_logic::BytesMatch(base + altCandRva,
                                              patcher_logic::kTextSplitterPrologue,
                                              patcher_logic::kTextSplitterHookLen)) {
                    return altCandRva;
                }
            }
        }
    }
    return 0;
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
            const DWORD hookRva = FindUnpackedTextSplitterRva(base);
            if (hookRva != 0) {
                Log("Translator: code unpacked at RVA 0x%X (iteration %d)", hookRva, i);
                Sleep(kPostUnpackSettleMs);
                if (InstallTextHook(base, hookRva)) {
                    InstallRenderCoreHook(base, hookRva);
                    Log("Translator: runtime translation ACTIVE (%d entries)",
                        static_cast<int>(g_translations.size()));
                    ApplyFullWindowTitle();
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

    if (g_renderCoreTrampoline) {
        VirtualFree(g_renderCoreTrampoline, 0, MEM_RELEASE);
        g_renderCoreTrampoline = nullptr;
    }

    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
