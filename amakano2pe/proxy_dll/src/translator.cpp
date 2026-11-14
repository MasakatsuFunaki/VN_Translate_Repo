// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"

#include "gdi_text_measurer.h"
#include "log.h"
#include "menu_autopatch.h"
#include "translator_logic.h"
#include "win32_chrome.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Runtime translator for amakano2pe (CatSystem2 / cs2.exe).
//
// Hooks FUN_0063a9b0 (RVA 0x23A9B0), the universal UTF-8 -> UTF-16 converter:
//   void __cdecl FUN_0063a9b0(const char* src, wchar_t* dst, std::size_t max_wchars);
//
// Entry stack layout (cdecl):
//   [esp+0x00] = return address
//   [esp+0x04] = src        (const char*)
//   [esp+0x08] = dst        (wchar_t*)
//   [esp+0x0C] = max_wchars (std::size_t)
//
// Also hooks FUN_0063d000 (RVA 0x23D000), CatSystem2's FontContext::SetHeight:
//   void __thiscall FUN_0063d000(Cs2FontContext* ctx, int raw_height);
// to scale LOGFONTW.lfHeight by 9/10 and keep GdiTextWidthMeasurer synchronized
// exclusively with meswnd.fes's STRING8 base dialogue font and active font
// weight across all 4 STRING8 base-font setup sites (`0x22596B`, `0x225C03`,
// `0x22BA7B`, `0x22BD09`), ignoring log.fes, inline \f glyph overrides, ruby,
// and speaker nameplates.

constexpr DWORD kTextHookRva = 0x23A9B0u;
constexpr std::size_t kTextHookPrologueLen = 14;
constexpr SIZE_T kTextTrampolineAllocBytes = 256;

constexpr DWORD kFontHookRva = 0x23D000u;
constexpr std::size_t kFontHookPrologueLen = 7;
constexpr SIZE_T kFontTrampolineAllocBytes = 128;

constexpr std::size_t kJmpOpcodeBytes = 1;
constexpr std::size_t kRel32Bytes = 4;
constexpr std::size_t kJmpPatchBytes = kJmpOpcodeBytes + kRel32Bytes;
constexpr std::size_t kMinReadableStringBytes = 1;

constexpr BYTE kOpPushEax = 0x50u;
constexpr BYTE kOpPushEcx = 0x51u;
constexpr BYTE kOpPushEdx = 0x52u;
constexpr BYTE kOpPushEbx = 0x53u;
constexpr BYTE kOpPushEbp = 0x55u;
constexpr BYTE kOpPushEsi = 0x56u;
constexpr BYTE kOpPopEdx = 0x5Au;
constexpr BYTE kOpPopEcx = 0x59u;
constexpr BYTE kOpPushad = 0x60u;
constexpr BYTE kOpPopad = 0x61u;
constexpr BYTE kOpArithRm32Imm8 = 0x83u;
constexpr BYTE kOpMovRm32Reg = 0x89u;
constexpr BYTE kOpMovRegRm32 = 0x8Bu;
constexpr BYTE kModRmEaxSibDisp8 = 0x44u;
constexpr BYTE kModRmAddEspImm8 = 0xC4u;
constexpr BYTE kSibEspBaseNoIndex = 0x24u;
constexpr BYTE kOpNop = 0x90u;
constexpr BYTE kOpCallRel32 = 0xE8u;
constexpr BYTE kOpJmpRel32 = 0xE9u;
constexpr BYTE kStackCleanTwoArgs = 0x08u;
constexpr BYTE kStackCleanSixArgs = 0x18u;

// Stack offsets inside InstallTextHook after `pushad` (0x20 bytes):
//   [esp+0x24] = src, [esp+0x2C] = max_wchars.
// Pushing max_wchars shifts esp by -4, placing src at [esp+0x28].
constexpr BYTE kStackArgSrcAfterPushad = 0x24u;
constexpr BYTE kStackArgSrcAfterOnePush = 0x28u;
constexpr BYTE kStackArgMaxWcharsAfterPushad = 0x2Cu;

// Stack offsets inside InstallFontScaleHook:
// Entry: [esp+0x00] = caller_ret, [esp+0x04] = raw_height.
// After `push ecx; push edx; push ebp; push ebx; push esi` (20 bytes = 0x14):
//   [esp+0x14] = caller_ret.
// After `push caller_ret` (24 bytes = 0x18):
//   [esp+0x1C] = raw_height.
constexpr BYTE kFontStackEntryHeightDisp = 0x04u;
constexpr BYTE kFontStackRetAfterFivePushes = 0x14u;
constexpr BYTE kFontStackHeightAfterSixPushes = 0x1Cu;

constexpr std::size_t kInitialModulePathChars = 260;
constexpr std::size_t kMaxModulePathChars = 32768;
constexpr std::size_t kPathGrowthFactor = 2;

constexpr int kInitialHookLogBurst = 200;
constexpr int kInitialTranslateLogBurst = 50;
constexpr int kInitialTruncateLogBurst = 30;
constexpr int kPeriodicLogStride = 100;

constexpr std::size_t kHookLogPreviewMaxBytes = 80;
constexpr std::size_t kWrapLogPreviewMaxBytes = 80;
constexpr std::size_t kTranslateLogPreviewMaxBytes = 50;
constexpr std::size_t kTruncateSrcLogPreviewMaxBytes = 40;
constexpr std::size_t kTruncateOutLogPreviewMaxBytes = 60;

constexpr std::size_t kFontContextSlotCount = 32;
constexpr int kPrimaryFontSlotIndex = 0;

constexpr wchar_t kTranslationTableFilename[] = L"translation_table.tsv";
constexpr wchar_t kConfigPluginModuleName[] = L"cs2confx.dll";

// Exact layout of CatSystem2's FontContext (`sizeof == 0xB8C`, verified against
// `imul eax, [ecx+0xB84], 0x5C` at 0x0063D000 and `push 0xB8C` at 0x00625AC9).
struct Cs2FontContext {
    HFONT current_font;
    std::array<LOGFONTW, kFontContextSlotCount> slots;
    int active_slot;
    int dirty;
};

static_assert(sizeof(LOGFONTW) == 0x5Cu,
              "Win32 LOGFONTW size must match CatSystem2 0x5C stride");
static_assert(offsetof(Cs2FontContext, active_slot) == 0xB84u,
              "Cs2FontContext::active_slot offset must match [ecx+0xB84]");
static_assert(sizeof(Cs2FontContext) == 0xB8Cu,
              "Cs2FontContext size must match CatSystem2 0xB8C allocation");

class CriticalSection {
public:
    CriticalSection() noexcept { InitializeCriticalSection(&cs_); }
    ~CriticalSection() { DeleteCriticalSection(&cs_); }

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;

    CRITICAL_SECTION* native_handle() noexcept { return &cs_; }

private:
    CRITICAL_SECTION cs_{};
};

class ScopedLock {
public:
    explicit ScopedLock(CriticalSection* mutex) noexcept : mutex_(mutex) {
        if (mutex_ != nullptr) {
            EnterCriticalSection(mutex_->native_handle());
        }
    }
    ~ScopedLock() {
        if (mutex_ != nullptr) {
            LeaveCriticalSection(mutex_->native_handle());
        }
    }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    CriticalSection* mutex_;
};

struct RuntimeTranslatorState {
    translator_logic::TranslationMap translations;
    bool loaded = false;

    gdi_text::GdiTextWidthMeasurer measurer{
        gdi_text::kDefaultScaledDialogueLfHeight};
    translator_logic::DialogueTranslationSession session{
        &translations,
        &measurer,
        translator_logic::DialogueSessionConfig{
            gdi_text::kDefaultMaxDialogueLineWidthPx,
            translator_logic::kSingleCharDecoderMaxWchars,
            translator_logic::kMaxSpeakerWideChars,
            translator_logic::kDefaultMaxOutputBytes,
        }};

    translator_logic::TranslationSlotRingBuffer slots;

    int hook_hits = 0;
    int translation_hits = 0;
    int truncations = 0;
    int font_scale_hits = 0;
};

// Leaky heap singletons initialized via InitOnceExecuteOnce so CRT static
// destruction on DLL_PROCESS_DETACH never destroys the mutex or GDI/session
// state while cs2.exe's patched detour trampolines remain live.
INIT_ONCE g_translator_mutex_init_once = INIT_ONCE_STATIC_INIT;
CriticalSection* g_translator_mutex = nullptr;

INIT_ONCE g_translator_state_init_once = INIT_ONCE_STATIC_INIT;
RuntimeTranslatorState* g_translator_state = nullptr;

BYTE* g_module_base = nullptr;
BYTE* g_text_trampoline = nullptr;
BYTE* g_font_trampoline = nullptr;
bool g_text_hook_installed = false;
bool g_font_hook_installed = false;

BOOL CALLBACK InitTranslatorMutexCallback(PINIT_ONCE /*init_once*/,
                                          PVOID /*parameter*/,
                                          PVOID* /*context*/) noexcept {
    g_translator_mutex = new (std::nothrow) CriticalSection();
    return g_translator_mutex != nullptr ? TRUE : FALSE;
}

CriticalSection* GetTranslatorMutex() noexcept {
    if (!InitOnceExecuteOnce(&g_translator_mutex_init_once,
                             InitTranslatorMutexCallback, nullptr, nullptr)) {
        return nullptr;
    }
    return g_translator_mutex;
}

BOOL CALLBACK InitTranslatorStateCallback(PINIT_ONCE /*init_once*/,
                                          PVOID /*parameter*/,
                                          PVOID* /*context*/) noexcept {
    try {
        g_translator_state = new RuntimeTranslatorState();
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

RuntimeTranslatorState* GetTranslatorState() noexcept {
    if (!InitOnceExecuteOnce(&g_translator_state_init_once,
                             InitTranslatorStateCallback, nullptr, nullptr)) {
        return nullptr;
    }
    return g_translator_state;
}

void ReleaseTrampoline(BYTE*& trampoline) noexcept {
    if (trampoline != nullptr) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        trampoline = nullptr;
    }
}

bool WideToUtf8(std::wstring_view wide, std::string& utf8_out) {
    if (wide.empty()) {
        utf8_out.clear();
        return true;
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) {
        return false;
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
        needed, nullptr, nullptr);
    if (written != needed) {
        return false;
    }
    utf8_out = std::move(utf8);
    return true;
}

bool ResolveExecutableDirectoryPath(std::filesystem::path& dir_out) {
    std::size_t capacity = kInitialModulePathChars;
    while (capacity <= kMaxModulePathChars) {
        std::vector<wchar_t> buf(capacity, L'\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD len = GetModuleFileNameW(
            nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return false;
        }
        if (len < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            dir_out = std::filesystem::path(
                          std::wstring_view(buf.data(), len))
                          .parent_path();
            return true;
        }
        capacity *= kPathGrowthFactor;
    }
    return false;
}

bool LoadTranslations() {
    std::filesystem::path exe_dir;
    if (!ResolveExecutableDirectoryPath(exe_dir)) {
        Log("Translator: failed to resolve executable path");
        return false;
    }

    const std::filesystem::path tsv_path = exe_dir / kTranslationTableFilename;
    std::string tsv_utf8;
    if (!WideToUtf8(tsv_path.native(), tsv_utf8)) {
        Log("Translator: failed to encode translation_table.tsv path");
        return false;
    }

    CriticalSection* const mutex = GetTranslatorMutex();
    RuntimeTranslatorState* const state = GetTranslatorState();
    if (mutex == nullptr || state == nullptr) {
        Log("Translator: failed to initialize runtime state");
        return false;
    }

    ScopedLock lock(mutex);
    state->translations.clear();
    state->session.ResetDialogueState();
    state->slots.Reset();
    if (!translator_logic::LoadTsvFile(tsv_utf8, state->translations)) {
        Log("Translator: translation_table.tsv not found or empty");
        state->loaded = false;
        return false;
    }

    state->loaded = true;
    Log("Translator: loaded %zu translations from translation_table.tsv",
        state->translations.size());
    return true;
}

void WriteRel32(BYTE* operand_addr, const BYTE* target) noexcept {
    const auto next_ip = reinterpret_cast<std::uintptr_t>(operand_addr + kRel32Bytes);
    const auto dest = reinterpret_cast<std::uintptr_t>(target);
    const auto rel = static_cast<std::uint32_t>(dest - next_ip);
    std::memcpy(operand_addr, &rel, sizeof(rel));
}

int SafeUtf8LogPreviewLen(const char* text, std::size_t max_bytes) noexcept {
    return static_cast<int>(
        translator_logic::Utf8SafeTruncate(text, max_bytes));
}

const char* TranslateTextInner(const char* src, std::size_t max_wchars) {
    if (src == nullptr ||
        !gdi_text::IsReadableMemoryRange(src, kMinReadableStringBytes) ||
        src[0] == '\0') {
        return src;
    }

    CriticalSection* const mutex = GetTranslatorMutex();
    RuntimeTranslatorState* const state = GetTranslatorState();
    if (mutex == nullptr || state == nullptr) {
        return src;
    }

    ScopedLock lock(mutex);
    if (!state->loaded) {
        return src;
    }

    ++state->hook_hits;
    if (state->hook_hits <= kInitialHookLogBurst ||
        (state->hook_hits % kPeriodicLogStride) == 0) {
        Log("Hook #%d: ptr=0x%p max_wchars=%zu [%.*s]",
            state->hook_hits, src, max_wchars,
            SafeUtf8LogPreviewLen(src, kHookLogPreviewMaxBytes), src);
    }

    const std::string_view src_view(src);
    const translator_logic::TranslationResult result =
        state->session.Translate(src_view, max_wchars);
    if (!result.translated) {
        return src;
    }

    if (result.deduplicated) {
        const char* const cached_slot =
            state->slots.FindReusableDialogueSlot(result.text.size());
        if (cached_slot != nullptr) {
            if (state->translation_hits <= kInitialTranslateLogBurst ||
                (state->translation_hits % kPeriodicLogStride) == 0) {
                Log("Translate dedup: [%.*s] -> [%.*s] width=%zu",
                    SafeUtf8LogPreviewLen(src, kTranslateLogPreviewMaxBytes),
                    src,
                    SafeUtf8LogPreviewLen(cached_slot,
                                          kTranslateLogPreviewMaxBytes),
                    cached_slot, result.end_width);
            }
            return cached_slot;
        }
    }

    ++state->translation_hits;
    const bool is_dialogue = !result.is_speaker_name && !result.is_ui_text;
    std::size_t copy_len = 0;
    const char* const slot =
        state->slots.Store(result.text, is_dialogue, &copy_len);

    if (is_dialogue &&
        (state->translation_hits <= kInitialTranslateLogBurst ||
         (state->translation_hits % kPeriodicLogStride) == 0)) {
        Log("Wrap #%d: cont=%d sw=%zu ew=%zu max_w=%zu out=[%.*s]",
            state->translation_hits, static_cast<int>(result.is_continuation),
            result.start_width, result.end_width, max_wchars,
            SafeUtf8LogPreviewLen(slot, kWrapLogPreviewMaxBytes), slot);
    }

    if (result.truncated) {
        ++state->truncations;
        if (state->truncations <= kInitialTruncateLogBurst ||
            (state->truncations % kPeriodicLogStride) == 0) {
            Log("Truncate #%d: max_wchars=%zu kept=%zu [%.*s] -> [%.*s]",
                state->truncations, max_wchars, copy_len,
                SafeUtf8LogPreviewLen(src, kTruncateSrcLogPreviewMaxBytes), src,
                SafeUtf8LogPreviewLen(slot, kTruncateOutLogPreviewMaxBytes),
                slot);
        }
    }

    if (state->translation_hits <= kInitialTranslateLogBurst ||
        (state->translation_hits % kPeriodicLogStride) == 0) {
        const char* const kind_label =
            result.is_speaker_name ? "name"
                                   : (result.is_ui_text ? "ui" : "text");
        Log("Translate #%d (%s): [%.*s] -> [%.*s]",
            state->translation_hits, kind_label,
            SafeUtf8LogPreviewLen(src, kTranslateLogPreviewMaxBytes), src,
            SafeUtf8LogPreviewLen(slot, kTranslateLogPreviewMaxBytes), slot);
    }

    return slot;
}

const char* __cdecl TranslateText(const char* src, std::size_t max_wchars) {
    __try {
        return TranslateTextInner(src, max_wchars);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in TranslateText, ptr=0x%p", src);
        return src;
    }
}

int ScaleHeightInner(const Cs2FontContext* ctx,
                     int raw_height,
                     std::uintptr_t caller_ret,
                     const void* caller_esi,
                     const void* caller_ebx,
                     std::uintptr_t caller_ebp) {
    const int scaled = gdi_text::ScaleEngineFontHeight(raw_height);

    CriticalSection* const mutex = GetTranslatorMutex();
    RuntimeTranslatorState* const state = GetTranslatorState();
    if (mutex == nullptr || state == nullptr) {
        return scaled;
    }

    ScopedLock lock(mutex);
    ++state->font_scale_hits;

    if (ctx == nullptr || g_module_base == nullptr ||
        !gdi_text::IsReadableMemoryRange(ctx, sizeof(Cs2FontContext)) ||
        ctx->active_slot != kPrimaryFontSlotIndex) {
        return scaled;
    }

    gdi_text::String8BaseFontObservation obs{};
    const auto base_addr = reinterpret_cast<std::uintptr_t>(g_module_base);
    if (!gdi_text::ResolveString8BaseFontObservation(
            base_addr, caller_ret, caller_esi, caller_ebx, caller_ebp, obs)) {
        return scaled;
    }

    const LOGFONTW& slot0 = ctx->slots[kPrimaryFontSlotIndex];
    const std::size_t face_len = wcsnlen(slot0.lfFaceName, LF_FACESIZE);
    const std::wstring_view face_view(slot0.lfFaceName, face_len);
    return state->measurer.SyncFromString8BaseFont(
        ctx->active_slot, &obs.layout_sizes, raw_height, face_view,
        obs.font_weight);
}

int __cdecl ScaleHeight(const Cs2FontContext* ctx,
                        int raw_height,
                        std::uintptr_t caller_ret,
                        const void* caller_esi,
                        const void* caller_ebx,
                        std::uintptr_t caller_ebp) {
    __try {
        return ScaleHeightInner(ctx, raw_height, caller_ret, caller_esi,
                                caller_ebx, caller_ebp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return gdi_text::ScaleEngineFontHeight(raw_height);
    }
}

bool InstallTextHook(BYTE* base) {
    BYTE* const hook_site = base + kTextHookRva;
    const BYTE* const return_addr = hook_site + kTextHookPrologueLen;

    constexpr std::array<BYTE, kTextHookPrologueLen> kExpectedPrologue = {
        0x83u, 0xECu, 0x20u,                          // sub esp, 20h
        0xA1u, 0xA4u, 0xC5u, 0xA6u, 0x00u,            // mov eax, [00A6C5A4h]
        0x33u, 0xC4u,                                 // xor eax, esp
        0x89u, 0x44u, 0x24u, 0x1Cu,                   // mov [esp+1Ch], eax
    };

    if (std::memcmp(hook_site, kExpectedPrologue.data(),
                    kExpectedPrologue.size()) != 0) {
        Log("TEXT HOOK: bytes mismatch at RVA 0x%lX", kTextHookRva);
        return false;
    }

    g_text_trampoline = static_cast<BYTE*>(VirtualAlloc(
        nullptr, kTextTrampolineAllocBytes, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (g_text_trampoline == nullptr) {
        Log("TEXT HOOK: VirtualAlloc failed (gle=%lu)", GetLastError());
        return false;
    }

    BYTE* p = g_text_trampoline;

    // Entry (cdecl): [esp+0x04] = src, [esp+0x08] = dst, [esp+0x0C] = max_wchars.
    // After pushad (0x20 bytes): [esp+0x24] = src, [esp+0x2C] = max_wchars.
    *p++ = kOpPushad;
    *p++ = kOpMovRegRm32;
    *p++ = kModRmEaxSibDisp8;
    *p++ = kSibEspBaseNoIndex;
    *p++ = kStackArgMaxWcharsAfterPushad;       // mov eax, [esp+0x2C] (max_wchars)
    *p++ = kOpPushEax;                          // push eax (esp -= 4; src at +0x28)
    *p++ = kOpMovRegRm32;
    *p++ = kModRmEaxSibDisp8;
    *p++ = kSibEspBaseNoIndex;
    *p++ = kStackArgSrcAfterOnePush;            // mov eax, [esp+0x28] (src)
    *p++ = kOpPushEax;                          // push eax
    *p++ = kOpCallRel32;                        // call TranslateText
    WriteRel32(p, reinterpret_cast<const BYTE*>(&TranslateText));
    p += kRel32Bytes;
    *p++ = kOpArithRm32Imm8;
    *p++ = kModRmAddEspImm8;
    *p++ = kStackCleanTwoArgs;                  // add esp, 8
    *p++ = kOpMovRm32Reg;
    *p++ = kModRmEaxSibDisp8;
    *p++ = kSibEspBaseNoIndex;
    *p++ = kStackArgSrcAfterPushad;             // mov [esp+0x24], eax
    *p++ = kOpPopad;

    std::memcpy(p, kExpectedPrologue.data(), kExpectedPrologue.size());
    p += kExpectedPrologue.size();

    *p++ = kOpJmpRel32;
    WriteRel32(p, return_addr);
    p += kRel32Bytes;

    DWORD trampoline_old_protect = 0;
    if (!VirtualProtect(g_text_trampoline, kTextTrampolineAllocBytes,
                        PAGE_EXECUTE_READ, &trampoline_old_protect)) {
        Log("TEXT HOOK: trampoline VirtualProtect failed (gle=%lu)",
            GetLastError());
        ReleaseTrampoline(g_text_trampoline);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_text_trampoline,
                          kTextTrampolineAllocBytes);

    DWORD old_protect = 0;
    if (!VirtualProtect(hook_site, kTextHookPrologueLen,
                        PAGE_EXECUTE_READWRITE, &old_protect)) {
        Log("TEXT HOOK: hook_site VirtualProtect failed (gle=%lu)",
            GetLastError());
        ReleaseTrampoline(g_text_trampoline);
        return false;
    }

    hook_site[0] = kOpJmpRel32;
    WriteRel32(hook_site + kJmpOpcodeBytes, g_text_trampoline);
    for (std::size_t i = kJmpPatchBytes; i < kTextHookPrologueLen; ++i) {
        hook_site[i] = kOpNop;
    }

    DWORD restored_protect = 0;
    VirtualProtect(hook_site, kTextHookPrologueLen, old_protect,
                   &restored_protect);
    FlushInstructionCache(GetCurrentProcess(), hook_site, kTextHookPrologueLen);
    g_text_hook_installed = true;

    Log("TEXT HOOK: installed at 0x%08X (RVA 0x%lX), trampoline=0x%08X",
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(hook_site)),
        kTextHookRva,
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_text_trampoline)));
    return true;
}

bool InstallFontScaleHook(BYTE* base) {
    BYTE* const site = base + kFontHookRva;
    const BYTE* const return_addr = site + kFontHookPrologueLen;

    constexpr std::array<BYTE, kFontHookPrologueLen> kExpectedPrologue = {
        0x6Bu, 0x81u, 0x84u, 0x0Bu, 0x00u, 0x00u, 0x5Cu,  // imul eax,[ecx+0xB84],0x5C
    };

    if (std::memcmp(site, kExpectedPrologue.data(),
                    kExpectedPrologue.size()) != 0) {
        Log("FONT HOOK: bytes mismatch at RVA 0x%lX", kFontHookRva);
        return false;
    }

    g_font_trampoline = static_cast<BYTE*>(VirtualAlloc(
        nullptr, kFontTrampolineAllocBytes, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (g_font_trampoline == nullptr) {
        Log("FONT HOOK: VirtualAlloc failed (gle=%lu)", GetLastError());
        return false;
    }

    BYTE* p = g_font_trampoline;
    *p++ = kOpPushEcx;                          // save ecx (this)
    *p++ = kOpPushEdx;                          // save edx
    *p++ = kOpPushEbp;                          // arg6: caller_ebp
    *p++ = kOpPushEbx;                          // arg5: caller_ebx
    *p++ = kOpPushEsi;                          // arg4: caller_esi
    *p++ = kOpMovRegRm32;
    *p++ = kModRmEaxSibDisp8;
    *p++ = kSibEspBaseNoIndex;
    *p++ = kFontStackRetAfterFivePushes;        // mov eax, [esp+0x14] (caller_ret)
    *p++ = kOpPushEax;                          // arg3: caller_ret
    *p++ = kOpMovRegRm32;
    *p++ = kModRmEaxSibDisp8;
    *p++ = kSibEspBaseNoIndex;
    *p++ = kFontStackHeightAfterSixPushes;      // mov eax, [esp+0x1C] (raw_height)
    *p++ = kOpPushEax;                          // arg2: raw_height
    *p++ = kOpPushEcx;                          // arg1: ctx (this)
    *p++ = kOpCallRel32;                        // call ScaleHeight
    WriteRel32(p, reinterpret_cast<const BYTE*>(&ScaleHeight));
    p += kRel32Bytes;
    *p++ = kOpArithRm32Imm8;
    *p++ = kModRmAddEspImm8;
    *p++ = kStackCleanSixArgs;                  // add esp, 0x18
    *p++ = kOpPopEdx;                           // restore edx
    *p++ = kOpPopEcx;                           // restore ecx
    *p++ = kOpMovRm32Reg;
    *p++ = kModRmEaxSibDisp8;
    *p++ = kSibEspBaseNoIndex;
    *p++ = kFontStackEntryHeightDisp;           // mov [esp+0x04], eax (scaled height)

    std::memcpy(p, kExpectedPrologue.data(), kExpectedPrologue.size());
    p += kExpectedPrologue.size();

    *p++ = kOpJmpRel32;
    WriteRel32(p, return_addr);
    p += kRel32Bytes;

    DWORD trampoline_old_protect = 0;
    if (!VirtualProtect(g_font_trampoline, kFontTrampolineAllocBytes,
                        PAGE_EXECUTE_READ, &trampoline_old_protect)) {
        Log("FONT HOOK: trampoline VirtualProtect failed (gle=%lu)",
            GetLastError());
        ReleaseTrampoline(g_font_trampoline);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_font_trampoline,
                          kFontTrampolineAllocBytes);

    DWORD old_protect = 0;
    if (!VirtualProtect(site, kFontHookPrologueLen, PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
        Log("FONT HOOK: site VirtualProtect failed (gle=%lu)", GetLastError());
        ReleaseTrampoline(g_font_trampoline);
        return false;
    }

    site[0] = kOpJmpRel32;
    WriteRel32(site + kJmpOpcodeBytes, g_font_trampoline);
    for (std::size_t i = kJmpPatchBytes; i < kFontHookPrologueLen; ++i) {
        site[i] = kOpNop;
    }

    DWORD restored_protect = 0;
    VirtualProtect(site, kFontHookPrologueLen, old_protect, &restored_protect);
    FlushInstructionCache(GetCurrentProcess(), site, kFontHookPrologueLen);
    g_font_hook_installed = true;

    Log("FONT HOOK: installed at 0x%08X (RVA 0x%lX), scale=%d/%d",
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(site)),
        kFontHookRva, gdi_text::kFontScaleNumerator,
        gdi_text::kFontScaleDenominator);
    return true;
}

}  // namespace

void TranslatorInit() {
    menu_autopatch::EnsureMenuAssetsPatchedForHostProcess();
    LoadTranslations();
    auto* const base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    g_module_base = base;
    Log("Translator: host base=0x%08X",
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(base)));
    if (base == nullptr) {
        return;
    }
    win32_chrome::InstallWindowChromeHooks(base);
    if (HMODULE confx = GetModuleHandleW(kConfigPluginModuleName);
        confx != nullptr) {
        win32_chrome::InstallWindowChromeHooks(confx);
    }
    if (!InstallTextHook(base)) {
        Log("Translator: hook install FAILED - translation disabled");
    }
    if (!InstallFontScaleHook(base)) {
        Log("Translator: font-scale hook install FAILED - text size unchanged");
    }
}

void TranslatorShutdown() {
    CriticalSection* const mutex = GetTranslatorMutex();
    RuntimeTranslatorState* const state = GetTranslatorState();
    ScopedLock lock(mutex);
    // Installed trampolines remain mapped during DLL_PROCESS_DETACH because
    // cs2.exe's patched prologue sites still jump into them and background
    // engine threads may not yet have terminated; the OS reclaims the address
    // space on process exit. Any unpatched trampoline allocation is freed.
    if (!g_text_hook_installed && g_text_trampoline != nullptr) {
        ReleaseTrampoline(g_text_trampoline);
    }
    if (!g_font_hook_installed && g_font_trampoline != nullptr) {
        ReleaseTrampoline(g_font_trampoline);
    }
    if (state != nullptr) {
        Log("Translator: %d hook hits, %d translations applied, %d truncations, %d font scales",
            state->hook_hits, state->translation_hits, state->truncations,
            state->font_scale_hits);
    }
}
