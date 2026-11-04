// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "patcher.h"

#include "log.h"
#include "menu_autopatch.h"
#include "patcher_logic.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Poll configuration for waiting until the packed executable decompresses
// its code section in memory.
constexpr DWORD kSentinelRva = 0x0F89B0U;
constexpr int kSentinelMaxPolls = 3000;
constexpr DWORD kSentinelPollIntervalMs = 10U;
constexpr DWORD kPostUnpackSettleDelayMs = 50U;
constexpr DWORD kThreadShutdownTimeoutMs = 5000U;

// Per-patch readiness polling configuration.
constexpr int kWaitBytesMaxRetries = 200;
constexpr DWORD kWaitBytesPollIntervalMs = 10U;
constexpr std::size_t kMaxNopRangeBytes = 64U;

// Byte indices used when logging a short mismatch preview.
constexpr std::size_t kPreviewIndex0 = 0U;
constexpr std::size_t kPreviewIndex1 = 1U;
constexpr std::size_t kPreviewIndex2 = 2U;
constexpr std::size_t kMinLenForPreviewIndex1 = 2U;
constexpr std::size_t kMinLenForPreviewIndex2 = 3U;

// Sentinel bytes at kSentinelRva: preceding 0xFF byte followed by
// `mov dword ptr [edx+0x4648], 0x18` (C7 82 48 46 00 00 18 00 00 00).
constexpr BYTE kUnpackSentinel[] = {
    0xFF, 0xC7, 0x82, 0x48, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00
};

// PrintMessage constructor (FUN_004f8880) and chart popup renderer
// (FUN_00407340) patch site RVAs relative to the executable ImageBase.
constexpr DWORD kFontSizeDefaultRva       = 0x0F89B7U;
constexpr DWORD kNextYAddendRva           = 0x0F89E5U;
constexpr DWORD kFxfFontSizeReadRva       = 0x0F89BBU;
constexpr DWORD kFxfNextYReadRva          = 0x0F8A48U;
constexpr DWORD kFxfLNextYReadRva         = 0x0F8B37U;
constexpr DWORD kSpaceDefaultRva          = 0x0F8A71U;
constexpr DWORD kFxfSpaceReadRva          = 0x0F8A75U;
constexpr DWORD kFxfNextXReadRva          = 0x0F8A2CU;
constexpr DWORD kWindowMsgNumDefaultRva   = 0x0F8D75U;
constexpr DWORD kFxfWindowMsgNumReadRva   = 0x0F8DA4U;
constexpr DWORD kChartPopupFontSizeRva    = 0x007504U;
constexpr DWORD kChartPopupLineSpacingRva = 0x00750DU;

// Prefix lengths checked by WaitForBytes before applying each patch.
constexpr std::size_t kNoWaitCheckLen         = 0U;
constexpr std::size_t kSingleByteWaitCheckLen = 1U;
constexpr std::size_t kDwordWaitCheckLen      = 4U;
constexpr std::size_t kPushImmWaitCheckLen    = 5U;
constexpr std::size_t kFxfCallWaitCheckLen    = 7U;

// 1. PrintMessage fontSize default: 24 (0x18) -> 14 (0x0E).
constexpr BYTE kFontSizeOrig[] = { 0x18 };
constexpr BYTE kFontSizeRepl[] = { 0x0E };

// 2. PrintMessage nextY addend: fontSize + 6 -> fontSize + 4 (18 px line pitch).
constexpr BYTE kNextYAddendOrig[] = { 0x06 };
constexpr BYTE kNextYAddendRepl[] = { 0x04 };

// 3. NOP FXF "fontSize" override read (28 bytes).
constexpr BYTE kFxfFontSizeOrig[] = {
    0x68, 0xC4, 0xFB, 0x56, 0x00,
    0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,
    0x05, 0x48, 0x46, 0x00, 0x00,
    0x50,
    0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,
    0xE8, 0xE9, 0xD6, 0x02, 0x00
};

// 4. NOP FXF "nextY" override read (29 bytes).
constexpr BYTE kFxfNextYOrig[] = {
    0x68, 0xE8, 0xFB, 0x56, 0x00,
    0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,
    0x81, 0xC1, 0x58, 0x46, 0x00, 0x00,
    0x51,
    0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,
    0xE8, 0x5B, 0xD6, 0x02, 0x00
};

// 5. NOP FXF "LNextY" override read (28 bytes).
constexpr BYTE kFxfLNextYOrig[] = {
    0x68, 0x10, 0xFC, 0x56, 0x00,
    0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,
    0x05, 0x68, 0x46, 0x00, 0x00,
    0x50,
    0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,
    0xE8, 0x6D, 0xD5, 0x02, 0x00
};

// 6. PrintMessage character space default: 1 -> -4 (advance = 14 - 4 = 10 px).
constexpr BYTE kSpaceOrig[] = { 0x01, 0x00, 0x00, 0x00 };
constexpr BYTE kSpaceRepl[] = { 0xFC, 0xFF, 0xFF, 0xFF };

// 7. NOP FXF "space" override read (28 bytes).
constexpr BYTE kFxfSpaceOrig[] = {
    0x68, 0xF0, 0xFB, 0x56, 0x00,
    0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,
    0x05, 0x6C, 0x46, 0x00, 0x00,
    0x50,
    0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,
    0xE8, 0x2F, 0xD6, 0x02, 0x00
};

// 8. NOP FXF "nextX" override read (28 bytes).
constexpr BYTE kFxfNextXOrig[] = {
    0x68, 0xE0, 0xFB, 0x56, 0x00,
    0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,
    0x05, 0x54, 0x46, 0x00, 0x00,
    0x50,
    0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,
    0xE8, 0x78, 0xD6, 0x02, 0x00
};

// 9. PrintMessage windowMessageNumber default: 4 -> 7 lines per textbox.
constexpr BYTE kWindowMsgNumOrig[] = { 0x04 };
constexpr BYTE kWindowMsgNumRepl[] = { 0x07 };

// 10. NOP FXF "windowMessageNumber" override read (28 bytes).
constexpr BYTE kFxfWindowMsgNumOrig[] = {
    0x68, 0xC0, 0xFC, 0x56, 0x00,
    0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,
    0x05, 0xCC, 0x46, 0x00, 0x00,
    0x50,
    0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,
    0xE8, 0x00, 0xD3, 0x02, 0x00
};

// 11. Chart popup renderer fontSize: 20 (0x14) -> 14 (0x0E).
constexpr BYTE kChartPopupFontSizeOrig[] = { 0x6A, 0x14, 0x8B, 0x55, 0xD8 };
constexpr BYTE kChartPopupFontSizeRepl[] = { 0x6A, 0x0E, 0x8B, 0x55, 0xD8 };

// 12. Chart popup renderer lineSpacing: 28 (0x1C) -> 18 (0x12).
constexpr BYTE kChartPopupLineSpacingOrig[] = { 0x6B, 0xC0, 0x1C, 0x03, 0x45 };
constexpr BYTE kChartPopupLineSpacingRepl[] = { 0x6B, 0xC0, 0x12, 0x03, 0x45 };

struct EnginePatchDescriptor {
    patcher_logic::PatchSpec spec;
    std::size_t waitCheckLen;
};

constexpr EnginePatchDescriptor kEnginePatches[] = {
    { { kFontSizeDefaultRva,       kFontSizeOrig,              kFontSizeRepl,              sizeof(kFontSizeOrig) },              kNoWaitCheckLen },
    { { kNextYAddendRva,           kNextYAddendOrig,           kNextYAddendRepl,           sizeof(kNextYAddendOrig) },           kNoWaitCheckLen },
    { { kFxfFontSizeReadRva,       kFxfFontSizeOrig,           nullptr,                    sizeof(kFxfFontSizeOrig) },           kNoWaitCheckLen },
    { { kFxfNextYReadRva,          kFxfNextYOrig,              nullptr,                    sizeof(kFxfNextYOrig) },              kFxfCallWaitCheckLen },
    { { kFxfLNextYReadRva,         kFxfLNextYOrig,             nullptr,                    sizeof(kFxfLNextYOrig) },             kFxfCallWaitCheckLen },
    { { kSpaceDefaultRva,          kSpaceOrig,                 kSpaceRepl,                 sizeof(kSpaceOrig) },                 kDwordWaitCheckLen },
    { { kFxfSpaceReadRva,          kFxfSpaceOrig,              nullptr,                    sizeof(kFxfSpaceOrig) },              kPushImmWaitCheckLen },
    { { kFxfNextXReadRva,          kFxfNextXOrig,              nullptr,                    sizeof(kFxfNextXOrig) },              kPushImmWaitCheckLen },
    { { kWindowMsgNumDefaultRva,   kWindowMsgNumOrig,          kWindowMsgNumRepl,          sizeof(kWindowMsgNumOrig) },          kSingleByteWaitCheckLen },
    { { kFxfWindowMsgNumReadRva,   kFxfWindowMsgNumOrig,       nullptr,                    sizeof(kFxfWindowMsgNumOrig) },       kPushImmWaitCheckLen },
    { { kChartPopupFontSizeRva,    kChartPopupFontSizeOrig,    kChartPopupFontSizeRepl,    sizeof(kChartPopupFontSizeOrig) },    kPushImmWaitCheckLen },
    { { kChartPopupLineSpacingRva, kChartPopupLineSpacingOrig, kChartPopupLineSpacingRepl, sizeof(kChartPopupLineSpacingOrig) }, kNoWaitCheckLen },
};

// RAII wrapper for a Win32 thread HANDLE.
// Thread-compatible: owned by the main thread across PatcherInit/PatcherShutdown.
class ScopedThreadHandle {
public:
    ScopedThreadHandle() noexcept = default;
    explicit ScopedThreadHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedThreadHandle(const ScopedThreadHandle&) = delete;
    ScopedThreadHandle& operator=(const ScopedThreadHandle&) = delete;

    ~ScopedThreadHandle() noexcept {
        Close();
    }

    void Reset(HANDLE handle = nullptr) noexcept {
        Close();
        handle_ = handle;
    }

    void JoinAndClose(DWORD timeoutMs) noexcept {
        if (handle_ == nullptr) {
            return;
        }
        WaitForSingleObject(handle_, timeoutMs);
        CloseHandle(handle_);
        handle_ = nullptr;
    }

    bool IsValid() const noexcept {
        return handle_ != nullptr;
    }

private:
    void Close() noexcept {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    HANDLE handle_ = nullptr;
};

ScopedThreadHandle g_patchThread;
std::atomic<bool> g_shutdownRequested{false};

bool WaitForBytes(BYTE* base,
                  DWORD rva,
                  const BYTE* expected,
                  std::size_t checkLen)
{
    if (checkLen == 0U) {
        return true;
    }

    BYTE* addr = base + rva;
    for (int retry = 0; retry < kWaitBytesMaxRetries; ++retry) {
        __try {
            if (patcher_logic::BytesMatch(addr, expected, checkLen)) {
                if (retry > 0) {
                    Log("  RVA 0x%X: bytes ready after %u ms",
                        rva,
                        static_cast<unsigned>(retry) * kWaitBytesPollIntervalMs);
                }
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Target page is not yet committed by the unpacker.
        }
        Sleep(kWaitBytesPollIntervalMs);
    }

    Log("  RVA 0x%X: TIMEOUT waiting for bytes (%u ms)",
        rva,
        static_cast<unsigned>(kWaitBytesMaxRetries) * kWaitBytesPollIntervalMs);
    return false;
}

bool PatchBytes(BYTE* base,
                DWORD rva,
                const BYTE* expected,
                const BYTE* replacement,
                std::size_t len)
{
    BYTE* addr = base + rva;
    if (!patcher_logic::BytesMatch(addr, expected, len)) {
        Log("PATCH FAIL at RVA 0x%X: bytes mismatch", rva);
        Log("  Expected: %02X %02X %02X ...",
            expected[kPreviewIndex0],
            len >= kMinLenForPreviewIndex1 ? expected[kPreviewIndex1] : 0U,
            len >= kMinLenForPreviewIndex2 ? expected[kPreviewIndex2] : 0U);
        Log("  Found:    %02X %02X %02X ...",
            addr[kPreviewIndex0],
            len >= kMinLenForPreviewIndex1 ? addr[kPreviewIndex1] : 0U,
            len >= kMinLenForPreviewIndex2 ? addr[kPreviewIndex2] : 0U);
        return false;
    }

    DWORD oldProtect = 0U;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("PATCH FAIL at RVA 0x%X: VirtualProtect error %u", rva, GetLastError());
        return false;
    }

    std::memcpy(addr, replacement, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    Log("PATCH OK at RVA 0x%X (%zu bytes)", rva, len);
    return true;
}

bool ApplySinglePatch(BYTE* base, const EnginePatchDescriptor& desc)
{
    if (!patcher_logic::IsValidSpec(desc.spec)) {
        return false;
    }

    WaitForBytes(base, desc.spec.rva, desc.spec.expected, desc.waitCheckLen);

    if (desc.spec.replacement != nullptr) {
        return PatchBytes(
            base, desc.spec.rva, desc.spec.expected, desc.spec.replacement, desc.spec.len);
    }

    if (desc.spec.len > kMaxNopRangeBytes) {
        return false;
    }

    BYTE nops[kMaxNopRangeBytes] = {};
    patcher_logic::FillNops(nops, desc.spec.len);
    return PatchBytes(base, desc.spec.rva, desc.spec.expected, nops, desc.spec.len);
}

void ApplyPatches(BYTE* base)
{
    int okCount = 0;
    int failCount = 0;

    for (const EnginePatchDescriptor& desc : kEnginePatches) {
        if (ApplySinglePatch(base, desc)) {
            ++okCount;
            continue;
        }
        ++failCount;
    }

    Log("Patches applied: %d OK, %d FAILED", okCount, failCount);
}

DWORD WINAPI PatcherThread(LPVOID)
{
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    Log("Patcher: ImageBase=0x%p, waiting for code to unpack...", base);

    for (int poll = 0; poll < kSentinelMaxPolls && !g_shutdownRequested.load(); ++poll) {
        __try {
            if (patcher_logic::BytesMatch(
                    base + kSentinelRva, kUnpackSentinel, sizeof(kUnpackSentinel))) {
                Log("Patcher: Code unpacked (detected at iteration %d)", poll);
                Sleep(kPostUnpackSettleDelayMs);
                ApplyPatches(base);
                return 0U;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Unpacker has not committed the target code page yet.
        }
        Sleep(kSentinelPollIntervalMs);
    }

    Log("Patcher: TIMEOUT - sentinel pattern not found");
    return 1U;
}

}  // namespace

void PatcherInit()
{
    (void)menu_autopatch::EnsureGameFilesPatched();
    g_shutdownRequested.store(false);
    HANDLE threadHandle = CreateThread(nullptr, 0U, PatcherThread, nullptr, 0U, nullptr);
    if (threadHandle == nullptr) {
        Log("Patcher: Failed to create thread, error %u", GetLastError());
        return;
    }
    g_patchThread.Reset(threadHandle);
}

void PatcherShutdown()
{
    g_shutdownRequested.store(true);
    g_patchThread.JoinAndClose(kThreadShutdownTimeoutMs);
}
