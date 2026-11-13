// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translator for Mushigurui (HD10, HD10_Win7, and Complete Edition).
//
// Installs edition-aware detours on:
//   1. CPrintMessage::TextSplitter (__thiscall, 4 stack args, ret 0x10, 9B prologue)
//   2. CMyMessage::TextRenderCore  (__thiscall, 15 stack args, ret 0x3C, 12B or 9B prologue)
// plus 1-arg CMyFont::MakeFont (FontSetup, __thiscall, ret 0x04) scaling and
// full Win32 window caption restoration from init2\mainControl.fxf.
#include "translator.h"

#include "log.h"
#include "patcher_logic.h"
#include "translator_logic.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

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
constexpr ULONG kVehCallFirst = 1;

constexpr int kCrashStackPreviewDwords = 8;
constexpr std::size_t kCrashStackLineBufferSize = 96;

enum RenderCoreArgIndex : std::size_t {
    kArgStartChar = 0,
    kArgMaxChars = 1,
    kArgX = 2,
    kArgY = 3,
    kArgText = 4,
    kArgFontSize = 5,
    kArgColorR = 6,
    kArgColorG = 7,
    kArgColorB = 8,
    kArgSpace = 9,
    kArgNextY = 10,
    kArgShadowFlag = 11,
    kArgUnusedSlot = 12,
    kArgUnderlineOffset = 13,
    kArgTagEnableFlag = 14,
};

constexpr DWORD kExpandedMaxChars = 1000;

constexpr int kWindowTitlePollRetries = 50;
constexpr DWORD kWindowTitlePollSleepMs = 50;

class ScopedFile {
public:
    explicit ScopedFile(std::FILE* fp) noexcept : fp_(fp) {}
    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    ~ScopedFile() {
        if (fp_ != nullptr) {
            std::fclose(fp_);
        }
    }

    std::FILE* get() const noexcept { return fp_; }
    explicit operator bool() const noexcept { return fp_ != nullptr; }

private:
    std::FILE* fp_;
};

using OrigTextSplitterFn = int(__thiscall*)(void* self,
                                            int cmd_mode,
                                            DWORD packed_flags,
                                            const char* text,
                                            int backlog_id);

using OrigTextRenderCoreFn = int(__thiscall*)(void* self,
                                              DWORD arg0,
                                              DWORD arg1,
                                              DWORD arg2,
                                              DWORD arg3,
                                              DWORD arg4,
                                              DWORD arg5,
                                              DWORD arg6,
                                              DWORD arg7,
                                              DWORD arg8,
                                              DWORD arg9,
                                              DWORD arg10,
                                              DWORD arg11,
                                              DWORD arg12,
                                              DWORD arg13,
                                              DWORD arg14);

using FontSetupFn = void(__thiscall*)(void* font_mgr, int font_size);

struct FontOverrideSnapshot {
    void* font_mgr = nullptr;
    int orig_font_size = 0;
    int orig_active_flag = 0;
    bool active = false;
};

translator_logic::TranslationTable g_translationTable;
bool g_translationsLoaded = false;

BYTE* g_trampoline = nullptr;
BYTE* g_hookSite = nullptr;
OrigTextSplitterFn g_origTextSplitter = nullptr;

BYTE* g_renderCoreTrampoline = nullptr;
BYTE* g_renderCoreHookSite = nullptr;
OrigTextRenderCoreFn g_origTextRenderCore = nullptr;
FontSetupFn g_fontSetupFn = nullptr;

HANDLE g_translatorThread = nullptr;
volatile bool g_shutdownRequested = false;
PVOID g_vehHandle = nullptr;

int g_hookHits = 0;
int g_translationHits = 0;

char g_transPool[kTransPoolSlots][kTransSlotSize];
int g_transPoolIdx = 0;

void LogCrashStackSafe(DWORD esp) noexcept {
    __try {
        const auto* stack_ptr = reinterpret_cast<const DWORD*>(esp);
        char line[kCrashStackLineBufferSize] = {};
        std::size_t pos = 0;
        for (int i = 0; i < kCrashStackPreviewDwords; ++i) {
            const int written = std::snprintf(
                line + pos, sizeof(line) - pos, "%08lX ", stack_ptr[i]);
            if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(line) - pos) {
                break;
            }
            pos += static_cast<std::size_t>(written);
        }
        if (pos > 0 && line[pos - 1] == ' ') {
            line[pos - 1] = '\0';
        }
        Log("  Stack: %s", line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  Stack: <unreadable>");
    }
}

LONG WINAPI TranslatorVEH(EXCEPTION_POINTERS* ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD eip = ep->ContextRecord->Eip;
    const DWORD esp = ep->ContextRecord->Esp;
    const DWORD ecx = ep->ContextRecord->Ecx;
    Log("=== CRASH DETECTED ===");
    Log("  ExceptionCode: 0x%08X", code);
    Log("  EIP: 0x%08X  ESP: 0x%08X  ECX: 0x%08X", eip, esp, ecx);
    Log("  EAX: 0x%08X  EBX: 0x%08X  EDX: 0x%08X",
        ep->ContextRecord->Eax, ep->ContextRecord->Ebx, ep->ContextRecord->Edx);
    Log("  TextSplitter trampoline: 0x%p  HookSite: 0x%p", g_trampoline, g_hookSite);
    Log("  RenderCore trampoline:   0x%p  HookSite: 0x%p",
        g_renderCoreTrampoline, g_renderCoreHookSite);
    Log("  Hook hits: %d  Translation hits: %d", g_hookHits, g_translationHits);
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= kMinAccessViolationParams) {
        Log("  Access type: %s  Address: 0x%08X",
            ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
            static_cast<DWORD>(ep->ExceptionRecord->ExceptionInformation[1]));
    }
    LogCrashStackSafe(esp);
    return EXCEPTION_CONTINUE_SEARCH;
}

void LoadTsvFileIfPresent(const wchar_t* tsv_path) {
    const ScopedFile file(_wfopen(tsv_path, L"rb"));
    if (!file) {
        char narrow_path[MAX_PATH] = {};
        WideCharToMultiByte(
            CP_ACP, 0, tsv_path, -1, narrow_path, MAX_PATH, nullptr, nullptr);
        Log("Translator: translation_table.tsv not found at %s", narrow_path);
        return;
    }

    std::fseek(file.get(), 0, SEEK_END);
    const long size = std::ftell(file.get());
    std::fseek(file.get(), 0, SEEK_SET);
    int tsv_rows = 0;
    if (size > 0) {
        std::vector<char> buf(static_cast<std::size_t>(size));
        const std::size_t nread = std::fread(buf.data(), 1, buf.size(), file.get());
        tsv_rows = g_translationTable.LoadFromTsvBuffer(buf.data(), nread);
    }
    Log("Translator: loaded %d TSV rows (%zu unique keys) from translation_table.tsv",
        tsv_rows, g_translationTable.ExactSize());
}

bool LoadTranslations() {
    g_translationTable.Clear();

    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wchar_t* last_slash = std::wcsrchr(exe_path, L'\\');
    wchar_t* filename_dst = (last_slash != nullptr) ? (last_slash + 1) : exe_path;
    std::wcscpy(filename_dst, L"translation_table.tsv");

    LoadTsvFileIfPresent(exe_path);

    const std::size_t seeded_fallbacks = g_translationTable.SeedRuntimeUiFallbacks();
    Log("Translator: seeded %zu runtime UI fallback entries (total exact=%zu, stripped=%zu)",
        seeded_fallbacks,
        g_translationTable.ExactSize(),
        g_translationTable.StrippedSize());

    g_translationsLoaded = !g_translationTable.Empty();
    return g_translationsLoaded;
}

char* AllocTransSlot() noexcept {
    const int idx = g_transPoolIdx;
    g_transPoolIdx = (g_transPoolIdx + 1) % kTransPoolSlots;
    return g_transPool[idx];
}

bool IsReadablePtr(const void* ptr) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return true;
}

const char* LookupAndStoreTranslation(const char* original_text,
                                      bool single_line_mode,
                                      bool* out_was_translated) {
    if (out_was_translated != nullptr) {
        *out_was_translated = false;
    }
    if (!g_translationsLoaded || original_text == nullptr) {
        return original_text;
    }
    if (!IsReadablePtr(original_text)) {
        Log("Translator: INVALID ptr 0x%p at hit %d", original_text, g_hookHits);
        return original_text;
    }
    if (original_text[0] == '\0') {
        return original_text;
    }

    ++g_hookHits;
    if (g_hookHits <= kInitialLogHitLimit ||
        (g_hookHits % kPeriodicLogHitInterval) == 0) {
        Log("Hook #%d: ptr=0x%p text=[%.60s]",
            g_hookHits, original_text, original_text);
    }

    std::string translated;
    if (!g_translationTable.Lookup(original_text, single_line_mode, translated)) {
        return original_text;
    }

    ++g_translationHits;
    if (out_was_translated != nullptr) {
        *out_was_translated = true;
    }

    char* slot = AllocTransSlot();
    std::size_t len = translated.size();
    if (len >= kTransSlotSize) {
        len = kTransSlotSize - 1;
    }
    std::memcpy(slot, translated.c_str(), len);
    slot[len] = '\0';

    if (g_translationHits <= kInitialLogHitLimit ||
        (g_translationHits % kPeriodicLogHitInterval) == 0) {
        Log("Translate #%d: [%.50s] -> [%.50s]",
            g_translationHits, original_text, slot);
    }
    return slot;
}

const char* TranslateTextInner(const char* original_text) {
    return LookupAndStoreTranslation(original_text, false, nullptr);
}

const char* TranslateTextSafe(const char* original_text) noexcept {
    __try {
        return TranslateTextInner(original_text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in TranslateText, ptr=0x%p hit=%d",
            original_text, g_hookHits);
        return original_text;
    }
}

void PreRenderCoreFontScaleInner(void* self,
                                 DWORD* args,
                                 FontOverrideSnapshot* snapshot) {
    if (args == nullptr || snapshot == nullptr) {
        return;
    }

    const auto* orig_text = reinterpret_cast<const char*>(args[kArgText]);
    const int cur_font_size = static_cast<int>(args[kArgFontSize]);
    const int cur_space = static_cast<int>(args[kArgSpace]);
    const int cur_max_chars = static_cast<int>(args[kArgMaxChars]);

    bool was_translated = false;
    const char* current_text =
        LookupAndStoreTranslation(orig_text, cur_space > 0, &was_translated);
    if (was_translated) {
        args[kArgText] = reinterpret_cast<DWORD>(current_text);
    }

    const translator_logic::UiFontAdjustment adj =
        translator_logic::ComputeUiFontAdjustment(
            current_text, was_translated, cur_font_size, cur_space, cur_max_chars);
    if (!adj.shouldAdjust) {
        return;
    }

    if (g_fontSetupFn != nullptr && self != nullptr && IsReadablePtr(self)) {
        void* font_mgr = *reinterpret_cast<void**>(self);
        if (font_mgr != nullptr && IsReadablePtr(font_mgr)) {
            auto* active_flag = reinterpret_cast<int*>(
                static_cast<std::uint8_t*>(font_mgr) +
                patcher_logic::kFontMgrActiveFlagOffset);
            auto* cached_font_size = reinterpret_cast<int*>(
                static_cast<std::uint8_t*>(font_mgr) +
                patcher_logic::kFontMgrCachedFontSizeOffset);

            snapshot->font_mgr = font_mgr;
            snapshot->orig_font_size = cur_font_size;
            snapshot->orig_active_flag = *active_flag;
            snapshot->active = true;

            *active_flag = 0;
            *cached_font_size = 0;
            g_fontSetupFn(font_mgr, adj.newFontSize);
            args[kArgFontSize] = static_cast<DWORD>(adj.newFontSize);
        }
    }

    args[kArgSpace] = static_cast<DWORD>(adj.newSpace);
    const int adjusted_y = static_cast<int>(args[kArgY]) + adj.yOffset;
    args[kArgY] = static_cast<DWORD>(adjusted_y);
    if (adj.expandMaxChars) {
        args[kArgStartChar] = 0;
        args[kArgMaxChars] = kExpandedMaxChars;
    }
}

void PreRenderCoreFontScaleSafe(void* self,
                                DWORD* args,
                                FontOverrideSnapshot* snapshot) noexcept {
    __try {
        PreRenderCoreFontScaleInner(self, args, snapshot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in PreRenderCoreFontScale, self=0x%p", self);
    }
}

void PostRenderCoreRestoreSafe(const FontOverrideSnapshot& snapshot) noexcept {
    if (!snapshot.active || g_fontSetupFn == nullptr || snapshot.font_mgr == nullptr) {
        return;
    }
    __try {
        auto* active_flag = reinterpret_cast<int*>(
            static_cast<std::uint8_t*>(snapshot.font_mgr) +
            patcher_logic::kFontMgrActiveFlagOffset);
        auto* cached_font_size = reinterpret_cast<int*>(
            static_cast<std::uint8_t*>(snapshot.font_mgr) +
            patcher_logic::kFontMgrCachedFontSizeOffset);
        *active_flag = 0;
        *cached_font_size = 0;
        g_fontSetupFn(snapshot.font_mgr, snapshot.orig_font_size);
        *active_flag = snapshot.orig_active_flag;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in PostRenderCoreRestore, fontMgr=0x%p",
            snapshot.font_mgr);
    }
}

class ScopedFontOverride {
public:
    ScopedFontOverride(void* self, DWORD* args) noexcept {
        PreRenderCoreFontScaleSafe(self, args, &snapshot_);
    }

    ScopedFontOverride(const ScopedFontOverride&) = delete;
    ScopedFontOverride& operator=(const ScopedFontOverride&) = delete;

    ~ScopedFontOverride() {
        if (snapshot_.active) {
            PostRenderCoreRestoreSafe(snapshot_);
        }
    }

private:
    FontOverrideSnapshot snapshot_{};
};

int __fastcall HookedTextSplitter(void* self,
                                  void* /*unused_edx*/,
                                  int cmd_mode,
                                  DWORD packed_flags,
                                  const char* text,
                                  int backlog_id) {
    const char* translated = TranslateTextSafe(text);
    return g_origTextSplitter(self, cmd_mode, packed_flags, translated, backlog_id);
}

int __fastcall HookedTextRenderCore(void* self,
                                    void* /*unused_edx*/,
                                    DWORD arg0,
                                    DWORD arg1,
                                    DWORD arg2,
                                    DWORD arg3,
                                    DWORD arg4,
                                    DWORD arg5,
                                    DWORD arg6,
                                    DWORD arg7,
                                    DWORD arg8,
                                    DWORD arg9,
                                    DWORD arg10,
                                    DWORD arg11,
                                    DWORD arg12,
                                    DWORD arg13,
                                    DWORD arg14) {
    DWORD args[patcher_logic::kTextRenderCoreStackArgCount] = {
        arg0, arg1, arg2, arg3, arg4,
        arg5, arg6, arg7, arg8, arg9,
        arg10, arg11, arg12, arg13, arg14,
    };
    const ScopedFontOverride font_override(self, args);
    return g_origTextRenderCore(
        self,
        args[kArgStartChar],
        args[kArgMaxChars],
        args[kArgX],
        args[kArgY],
        args[kArgText],
        args[kArgFontSize],
        args[kArgColorR],
        args[kArgColorG],
        args[kArgColorB],
        args[kArgSpace],
        args[kArgNextY],
        args[kArgShadowFlag],
        args[kArgUnusedSlot],
        args[kArgUnderlineOffset],
        args[kArgTagEnableFlag]);
}

bool InstallDetourHook(BYTE* hook_site,
                       std::size_t hook_len,
                       std::uintptr_t detour_fn_va,
                       BYTE** out_trampoline) {
    if (hook_site == nullptr || out_trampoline == nullptr) {
        return false;
    }

    auto* trampoline = static_cast<BYTE*>(
        VirtualAlloc(nullptr, kTrampolineAllocBytes, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        Log("Translator: VirtualAlloc failed for detour trampoline");
        return false;
    }

    const std::size_t trampoline_bytes = patcher_logic::BuildDetourTrampoline(
        trampoline,
        reinterpret_cast<std::uintptr_t>(trampoline),
        hook_site,
        reinterpret_cast<std::uintptr_t>(hook_site),
        hook_len);
    if (trampoline_bytes == 0) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, trampoline_bytes);

    DWORD old_protect = 0;
    if (!VirtualProtect(hook_site, hook_len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        Log("Translator: VirtualProtect failed at 0x%p (error=%u)",
            hook_site, GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    const bool wrote = patcher_logic::WriteDetourJump(
        hook_site,
        reinterpret_cast<std::uintptr_t>(hook_site),
        hook_len,
        detour_fn_va);
    VirtualProtect(hook_site, hook_len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), hook_site, hook_len);

    if (!wrote) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    *out_trampoline = trampoline;
    return true;
}

bool InstallEditionHooks(BYTE* base,
                         patcher_logic::SectionSpan text_span,
                         const patcher_logic::ResolvedTranslatorHooks& resolved) {
    const patcher_logic::EditionProfile& profile = *resolved.profile;

    if (!patcher_logic::MatchesTextSplitterSignature(
            profile, base, text_span, resolved.text_splitter_rva)) {
        Log("TEXT HOOK: prologue mismatch for [%s] at RVA 0x%06X",
            profile.name, resolved.text_splitter_rva);
        return false;
    }

    g_hookSite = base + resolved.text_splitter_rva;
    if (!InstallDetourHook(
            g_hookSite,
            profile.text_splitter_hook_len,
            reinterpret_cast<std::uintptr_t>(&HookedTextSplitter),
            &g_trampoline)) {
        Log("TEXT HOOK: failed to install at RVA 0x%06X", resolved.text_splitter_rva);
        return false;
    }
    g_origTextSplitter = reinterpret_cast<OrigTextSplitterFn>(g_trampoline);
    Log("TEXT HOOK: installed [%s] at RVA 0x%06X (len=%zu, trampoline=0x%p)",
        profile.name,
        resolved.text_splitter_rva,
        profile.text_splitter_hook_len,
        g_trampoline);

    if (!patcher_logic::MatchesTextRenderCoreSignature(
            profile, base, text_span, resolved.text_render_core_rva)) {
        Log("RENDER CORE HOOK: signature mismatch for [%s] at RVA 0x%06X",
            profile.name, resolved.text_render_core_rva);
        return true;
    }

    const bool font_setup_ok = patcher_logic::MatchesFontSetupSignature(
        profile, base, text_span, resolved.font_setup_rva);
    if (font_setup_ok) {
        g_fontSetupFn = reinterpret_cast<FontSetupFn>(base + resolved.font_setup_rva);
    }
    if (!font_setup_ok) {
        Log("RENDER CORE HOOK: FontSetup signature mismatch at RVA 0x%06X",
            resolved.font_setup_rva);
    }

    g_renderCoreHookSite = base + resolved.text_render_core_rva;
    if (!InstallDetourHook(
            g_renderCoreHookSite,
            profile.text_render_core_hook_len,
            reinterpret_cast<std::uintptr_t>(&HookedTextRenderCore),
            &g_renderCoreTrampoline)) {
        Log("RENDER CORE HOOK: failed to install at RVA 0x%06X",
            resolved.text_render_core_rva);
        return true;
    }
    g_origTextRenderCore =
        reinterpret_cast<OrigTextRenderCoreFn>(g_renderCoreTrampoline);

    Log("RENDER CORE HOOK: installed [%s] at RVA 0x%06X (len=%zu, FontSetup=0x%06X, trampoline=0x%p)",
        profile.name,
        resolved.text_render_core_rva,
        profile.text_render_core_hook_len,
        resolved.font_setup_rva,
        g_renderCoreTrampoline);
    return true;
}

struct WindowTitleUpdateCtx {
    DWORD target_pid = 0;
    const char* title = nullptr;
    bool updated = false;
};

BOOL CALLBACK SetProcessWindowTitleEnumProc(HWND hwnd, LPARAM l_param) {
    auto* ctx = reinterpret_cast<WindowTitleUpdateCtx*>(l_param);
    if (ctx == nullptr || ctx->title == nullptr) {
        return FALSE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->target_pid) {
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
    wchar_t fxf_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, fxf_path, MAX_PATH);
    wchar_t* last_slash = std::wcsrchr(fxf_path, L'\\');
    wchar_t* filename_dst = (last_slash != nullptr) ? (last_slash + 1) : fxf_path;
    std::wcscpy(filename_dst, L"init2\\mainControl.fxf");

    const ScopedFile file(_wfopen(fxf_path, L"rb"));
    if (!file) {
        return;
    }
    std::fseek(file.get(), 0, SEEK_END);
    const long size = std::ftell(file.get());
    std::fseek(file.get(), 0, SEEK_SET);
    if (size <= 0) {
        return;
    }

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    const std::size_t nread = std::fread(buf.data(), 1, buf.size(), file.get());
    const std::string title =
        translator_logic::ExtractFxfWindowTitle(buf.data(), nread);
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

bool TryResolveUnpackedTranslatorHooks(
    const BYTE* base,
    patcher_logic::SectionSpan text_span,
    std::uint32_t time_date_stamp,
    patcher_logic::ResolvedTranslatorHooks* out) noexcept {
    __try {
        const auto resolved = patcher_logic::ResolveTranslatorHooks(
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

DWORD WINAPI TranslatorThread(LPVOID) {
    if (!LoadTranslations()) {
        Log("Translator: no translations loaded, hooks not installed");
        return 0;
    }

    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    patcher_logic::SectionSpan text_span{
        patcher_logic::kDefaultTextSectionRva,
        patcher_logic::kHd10TextVirtualSize,
    };
    std::uint32_t time_date_stamp = 0;
    patcher_logic::PeImageLayout pe_layout{};
    if (SafeParsePeLayout(base, &pe_layout)) {
        text_span = pe_layout.text_section;
        time_date_stamp = pe_layout.time_date_stamp;
    }

    Log("Translator: ImageBase=0x%p, .text=[0x%06X..0x%06X), TDS=0x%08X, waiting for unpack...",
        base,
        text_span.rva,
        text_span.rva + text_span.virtual_size,
        time_date_stamp);

    for (int i = 0; i < kUnpackPollMaxRetries && !g_shutdownRequested; ++i) {
        patcher_logic::ResolvedTranslatorHooks resolved{};
        if (!TryResolveUnpackedTranslatorHooks(base, text_span, time_date_stamp, &resolved)) {
            Sleep(kUnpackPollSleepMs);
            continue;
        }

        Log("Translator: matched edition [%s] at iteration %d (scanFallback=%s)",
            resolved.profile->name,
            i,
            resolved.used_scan_fallback ? "yes" : "no");
        Sleep(kPostUnpackSettleMs);
        if (!InstallEditionHooks(base, text_span, resolved)) {
            Log("Translator: hook installation FAILED");
            return 0;
        }
        Log("Translator: runtime translation ACTIVE (%zu entries)",
            g_translationTable.ExactSize());
        ApplyFullWindowTitle();
        return 0;
    }

    Log("Translator: TIMEOUT waiting for supported edition code to unpack");
    return 0;
}

}  // namespace

void TranslatorInit() {
    g_shutdownRequested = false;
    g_vehHandle = AddVectoredExceptionHandler(kVehCallFirst, TranslatorVEH);
    g_translatorThread =
        CreateThread(nullptr, 0, TranslatorThread, nullptr, 0, nullptr);
}

void TranslatorShutdown() {
    g_shutdownRequested = true;
    if (g_translatorThread != nullptr) {
        WaitForSingleObject(g_translatorThread, kThreadShutdownWaitMs);
        CloseHandle(g_translatorThread);
        g_translatorThread = nullptr;
    }

    if (g_translationsLoaded) {
        Log("Translator stats: %d hook hits, %d translations applied",
            g_hookHits, g_translationHits);
    }

    if (g_trampoline != nullptr) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        g_origTextSplitter = nullptr;
    }

    if (g_renderCoreTrampoline != nullptr) {
        VirtualFree(g_renderCoreTrampoline, 0, MEM_RELEASE);
        g_renderCoreTrampoline = nullptr;
        g_origTextRenderCore = nullptr;
    }

    if (g_vehHandle != nullptr) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
