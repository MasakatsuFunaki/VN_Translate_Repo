// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include "translator_logic.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oretsuba::proxy {

inline constexpr std::uintptr_t kExpectedImageBase = 0x00400000;
inline constexpr std::uintptr_t kParseMessageHeaderAndBodyVa = 0x00455AB0;
inline constexpr std::uintptr_t kParseChoicesFromScriptVa = 0x00455F60;
inline constexpr std::uintptr_t kAppendChoiceStringVa = 0x00456110;
inline constexpr std::uintptr_t kSobLoadScriptVa = 0x00424D30;
inline constexpr std::uintptr_t kMessageWindowInitVa = 0x00458CD0;
inline constexpr std::uintptr_t kMessageWindowDrawCharVa = 0x00459130;
inline constexpr std::uintptr_t kFontGgoCreateGdiFontVa = 0x0044B500;

inline constexpr std::size_t kMsgWndProportionalFlagOffset = 0x3D;
inline constexpr std::size_t kFontGgoLogFontOffset = 0x0C;
inline constexpr std::size_t kFontGgoLfCharSetOffset = 0x23;
inline constexpr std::size_t kFontGgoLfFaceNameOffset = 0x28;
inline constexpr std::uint8_t kShiftJisCharset = 0x80;
inline constexpr const char* kProportionalFontFace = "MS UI Gothic";

class RuntimeHookManager {
public:
    explicit RuntimeHookManager(const TranslationCatalog* catalog);
    RuntimeHookManager(RuntimeHookManager&& other) noexcept;
    RuntimeHookManager& operator=(RuntimeHookManager&& other) noexcept;
    RuntimeHookManager(const RuntimeHookManager&) = delete;
    RuntimeHookManager& operator=(const RuntimeHookManager&) = delete;
    ~RuntimeHookManager();

    bool VerifyEngineSignatures(std::uintptr_t module_base = kExpectedImageBase) const noexcept;
    bool InstallHooks(std::uintptr_t module_base = kExpectedImageBase);
    void RemoveHooks() noexcept;
    bool AreHooksInstalled() const noexcept;

    char* FormatIntoNextScratchBuffer(std::string_view raw_script_str);

    bool RewriteChoiceBlock(const std::uint8_t* choice_data,
                            std::vector<std::uint8_t>& out_buf) const;

    std::size_t PatchLoadedSobStringTable(std::uint8_t* loaded_image,
                                          const std::uint8_t* sob_file_buf) const;

    static void ApplyProportionalFontOverride(void* font_ggo_this) noexcept;
    static void ApplyMessageWindowProportionalFlag(void* msg_wnd_this) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace oretsuba::proxy
