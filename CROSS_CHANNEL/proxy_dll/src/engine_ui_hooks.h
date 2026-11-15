// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// engine_ui_hooks.h
//
// Post-unpack in-engine UI localization controller for CROSS_CHANNEL
// (WillPlus / AdvHD, cc.exe).
//
// Installed once HookInstallerThread confirms the runtime packer has finished
// unpacking .text, .rdata, and .data and applying ASLR base relocations:
//   1. Redirects writable .data string-pointer tables (Config tooltips,
//      Scene & Diary titles, Playing Log statistics & Extra Story names, and
//      Save/Load/System modal descriptors) to persistent English CP932 buffers.
//   2. Patches fixed-slot HUD / Backlog / Pause / Achievement format strings
//      in .rdata where the English string fits inside the aligned slot.
//   3. Detours FUN_0082f5b0 (RVA 0x1F5B0), the engine's modal prompt renderer,
//      to translate direct-xref prompt lines on the fly, center English lines
//      with tight horizontal advance, and align "Yes / No" inside native mouse
//      hit-rects.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine_ui_hooks {

// Hook site metadata and post-unpack / post-.reloc anchor RVAs exposed for
// readiness polling and system-level verification (hook_sites_st.cpp).
inline constexpr std::uint32_t kLzssDecompressorRva = 0x45910u;
inline constexpr std::size_t kLzssDecompressorStolenLen = 6u;
inline constexpr std::array<std::uint8_t, kLzssDecompressorStolenLen>
    kLzssDecompressorExpectedPrologue = {0x55u, 0x8Bu, 0xECu,
                                         0x83u, 0xECu, 0x0Cu};

inline constexpr std::uint32_t kModalDialogHookRva = 0x1F5B0u;
inline constexpr std::size_t kModalDialogStolenLen = 6u;
inline constexpr std::array<std::uint8_t, kModalDialogStolenLen>
    kModalDialogExpectedPrologue = {0x55u, 0x8Bu, 0xECu, 0x83u, 0xE4u, 0xF8u};

inline constexpr std::uint32_t kDrawUiTextHelperRva = 0x0E6F0u;
inline constexpr std::uint32_t kUiGlyphScaleFloatRva = 0x0C4698u;
inline constexpr std::uint32_t kModalYesShortSlotRva = 0x0C4388u;

// cc.exe's runtime packer decompresses .text (linked for ImageBase 0x00400000),
// .rdata, and .data sequentially, and only afterward walks the 176-block .reloc
// table up to the final IMAGE_REL_BASED_HIGHLOW entry at RVA 0x0CB828.
inline constexpr std::uint32_t kTextRendererHookRva = 0x2A7A0u;
inline constexpr std::uint32_t kTextRendererA1OpcodeOffset = 474u;
inline constexpr std::uint32_t kDialogueTextPtrTargetRva = 0x06582B78u;

inline constexpr std::uint32_t kFirstConfigTooltipSlotRva = 0x0C84A0u;
inline constexpr std::uint32_t kFirstConfigTooltipTargetRva = 0x0C3A18u;
inline constexpr std::uint32_t kLastModalSlotRva = 0x0CB7FCu;
inline constexpr std::uint32_t kLastModalSlotTargetRva = 0x0C3F8Cu;
inline constexpr std::uint32_t kLastHighlowRelocRva = 0x0CB828u;
inline constexpr std::uint32_t kLastHighlowRelocTargetRva = 0x0A7774u;

// Returns true iff the cc.exe image at `exe_base` has unpacked .text, .rdata,
// and .data and finished applying ASLR relocations through RVA 0x0CB828.
// Safe to poll from a background thread (read-only SEH-guarded probes; remains
// true after inline hooks and UI table patches have been installed).
bool IsEngineImageUnpackedAndRelocated(std::uintptr_t exe_base) noexcept;

// Thread-compatible: Install() and Shutdown() require external serialization.
class EngineUiController {
public:
    EngineUiController();
    EngineUiController(const EngineUiController&) = delete;
    EngineUiController& operator=(const EngineUiController&) = delete;
    EngineUiController(EngineUiController&&) noexcept;
    EngineUiController& operator=(EngineUiController&&) noexcept;
    ~EngineUiController();

    bool Install(std::uintptr_t exe_base) noexcept;
    void Shutdown() noexcept;

    bool IsInstalled() const noexcept;
    std::size_t PatchedPointerCount() const noexcept;
    std::size_t PatchedRdataSlotCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Process-wide facade for HookInstallerThread / TranslatorShutdown().
bool InstallEngineUiHooks(std::uintptr_t exe_base) noexcept;
void ShutdownEngineUiHooks() noexcept;

}  // namespace engine_ui_hooks
