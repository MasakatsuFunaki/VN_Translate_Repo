// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "runtime_hooks.h"

#include "log.h"

#include <algorithm>
#include <array>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace oretsuba::proxy {

namespace {

constexpr std::size_t kScratchRingSlotCount = 128;
constexpr std::size_t kChoiceBufferCapacity = 4096;
constexpr std::size_t kChoiceHeaderBytes = 2;
constexpr std::size_t kChoiceOptionPrefixBytes = 2;
constexpr std::uint8_t kMaxChoicesCount = 32;
constexpr std::uint8_t kMinOptionStride = 3;
constexpr std::uint8_t kMaxOptionStride = 255;
constexpr std::size_t kMaxFaceNameBytes = 32;

constexpr std::uint8_t kJmpRel32Opcode = 0xE9;
constexpr std::uint8_t kNopOpcode = 0x90;
constexpr std::size_t kJmpPatchBytes = 5;
constexpr std::size_t kTrampolineSlotSize = 32;
constexpr std::size_t kSavedPrologueCapacity = 16;

constexpr std::size_t kSobMagicBytes = 4;
constexpr std::size_t kSobHeaderPrefixBytes = 8;
constexpr std::size_t kSobCodeAndTotalSizeBytes = 8;
constexpr std::size_t kDwordBytes = 4;
constexpr std::size_t kRelocPairBytes = 8;
constexpr std::size_t kSobSubtableCount = 5;
constexpr std::size_t kSobDataSubtableIndex = 4;
constexpr std::size_t kSyscallHeaderBackOffset = 10;
constexpr std::uint32_t kSyscallSceneTitleId = 0x000001CEU;
constexpr std::size_t kMaxSobStringScanBytes = 4096;

constexpr std::uint8_t kExpectedPrologueParseMsg[] = {
    0x83, 0xEC, 0x08, 0x53, 0x55, 0x56};
constexpr std::uint8_t kExpectedPrologueParseChoices[] = {
    0x83, 0xEC, 0x14, 0x53, 0x8B, 0x5C, 0x24, 0x1C};
constexpr std::uint8_t kExpectedPrologueAppendChoice[] = {
    0x53, 0x8B, 0xD9, 0x56, 0x57};
constexpr std::uint8_t kExpectedPrologueSobLoad[] = {
    0x6A, 0xFF, 0x68, 0xCB, 0x93, 0x4E, 0x00};
constexpr std::uint8_t kExpectedPrologueMsgWndInit[] = {
    0x6A, 0xFF, 0x68, 0x76, 0xA3, 0x4E, 0x00};
constexpr std::uint8_t kExpectedPrologueMsgWndDraw[] = {
    0x83, 0xEC, 0x14, 0x53, 0x8B, 0xD9};
constexpr std::uint8_t kExpectedPrologueCreateGdiFont[] = {
    0x55, 0x8B, 0xEC, 0x6A, 0xFF};

std::uint32_t ReadU32Le(const std::uint8_t* p) noexcept {
    std::uint32_t val = 0;
    std::memcpy(&val, p, sizeof(val));
    return val;
}

void WriteRel32(std::uint8_t* instr_start, std::uintptr_t dest_addr) noexcept {
    const auto next_ip = reinterpret_cast<std::uintptr_t>(instr_start) + kJmpPatchBytes;
    const auto rel = static_cast<std::int32_t>(dest_addr - next_ip);
    instr_start[0] = kJmpRel32Opcode;
    std::memcpy(instr_start + 1, &rel, sizeof(rel));
}

bool CheckBytesAt(std::uintptr_t addr,
                  const std::uint8_t* expected,
                  std::size_t len) noexcept {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    return std::memcmp(reinterpret_cast<const void*>(addr), expected, len) == 0;
}

using ParseMessageHeaderAndBodyFn = int(__thiscall*)(void* this_ptr, char* raw_script_str);
using ParseChoicesFromScriptFn = int(__thiscall*)(void* this_ptr, const std::uint8_t* choice_data);
using AppendChoiceStringFn = int(__thiscall*)(void* this_ptr, const char* choice_str);
using SobLoadScriptFn = int(__thiscall*)(void* this_ptr, const std::uint8_t* sob_buf);
using MessageWindowInitFn = void*(__thiscall*)(void* this_ptr, int arg0);
using MessageWindowDrawCharFn = int(__thiscall*)(void* this_ptr, int a0, int a1, int a2);
using FontGgoCreateGdiFontFn = int(__thiscall*)(void* this_ptr);

RuntimeHookManager* g_active_hook_mgr = nullptr;
ParseMessageHeaderAndBodyFn g_orig_parse_msg = nullptr;
ParseChoicesFromScriptFn g_orig_parse_choices = nullptr;
AppendChoiceStringFn g_orig_append_choice = nullptr;
SobLoadScriptFn g_orig_sob_load = nullptr;
MessageWindowInitFn g_orig_msg_wnd_init = nullptr;
MessageWindowDrawCharFn g_orig_msg_wnd_draw = nullptr;
FontGgoCreateGdiFontFn g_orig_create_gdi_font = nullptr;

struct InstalledPatch {
    std::uintptr_t target_addr = 0;
    std::size_t patch_len = 0;
    std::array<std::uint8_t, kSavedPrologueCapacity> original_bytes = {};
};

}  // namespace

struct RuntimeHookManager::Impl {
    const TranslationCatalog* catalog = nullptr;
    bool installed = false;
    std::uint8_t* trampoline_slab = nullptr;
    std::size_t trampoline_used = 0;
    std::vector<InstalledPatch> patches;

    std::array<std::array<char, kDefaultScratchBufferCapacity>, kScratchRingSlotCount>
        scratch_ring = {};
    std::size_t scratch_ring_index = 0;
    std::vector<std::uint8_t> choice_scratch;

    explicit Impl(const TranslationCatalog* cat)
        : catalog(cat), choice_scratch(kChoiceBufferCapacity, 0) {}

    ~Impl() {
        RestoreAll();
    }

    char* NextScratchSlot() noexcept {
        char* slot = scratch_ring[scratch_ring_index].data();
        scratch_ring_index = (scratch_ring_index + 1U) % kScratchRingSlotCount;
        return slot;
    }

    static int __fastcall DetourParseMessageHeaderAndBody(void* this_ptr,
                                                          void* /*edx*/,
                                                          char* raw_script_str) {
        if (g_active_hook_mgr && raw_script_str && raw_script_str[0] != '\0') {
            char* rewritten = g_active_hook_mgr->FormatIntoNextScratchBuffer(raw_script_str);
            return g_orig_parse_msg(this_ptr, rewritten);
        }
        return g_orig_parse_msg(this_ptr, raw_script_str);
    }

    static int __fastcall DetourParseChoicesFromScript(void* this_ptr,
                                                       void* /*edx*/,
                                                       const std::uint8_t* choice_data) {
        if (g_active_hook_mgr && choice_data) {
            auto& buf = g_active_hook_mgr->impl_->choice_scratch;
            if (g_active_hook_mgr->RewriteChoiceBlock(choice_data, buf)) {
                return g_orig_parse_choices(this_ptr, buf.data());
            }
        }
        return g_orig_parse_choices(this_ptr, choice_data);
    }

    static int __fastcall DetourAppendChoiceString(void* this_ptr,
                                                   void* /*edx*/,
                                                   const char* choice_str) {
        if (g_active_hook_mgr && g_active_hook_mgr->impl_->catalog && choice_str) {
            if (const char* tr =
                    g_active_hook_mgr->impl_->catalog->LookupPersistentPointer(choice_str)) {
                return g_orig_append_choice(this_ptr, tr);
            }
        }
        return g_orig_append_choice(this_ptr, choice_str);
    }

    static int __fastcall DetourSobLoadScript(void* this_ptr,
                                              void* /*edx*/,
                                              const std::uint8_t* sob_buf) {
        const int rc = g_orig_sob_load(this_ptr, sob_buf);
        if (rc == 0 && g_active_hook_mgr && this_ptr && sob_buf) {
            auto* loaded_image = *reinterpret_cast<std::uint8_t**>(this_ptr);
            if (loaded_image) {
                g_active_hook_mgr->PatchLoadedSobStringTable(loaded_image, sob_buf);
            }
        }
        return rc;
    }

    static void* __fastcall DetourMessageWindowInit(void* this_ptr,
                                                    void* /*edx*/,
                                                    int arg0) {
        void* ret = g_orig_msg_wnd_init(this_ptr, arg0);
        ApplyMessageWindowProportionalFlag(this_ptr);
        return ret;
    }

    static int __fastcall DetourMessageWindowDrawChar(void* this_ptr,
                                                      void* /*edx*/,
                                                      int a0,
                                                      int a1,
                                                      int a2) {
        ApplyMessageWindowProportionalFlag(this_ptr);
        return g_orig_msg_wnd_draw(this_ptr, a0, a1, a2);
    }

    static int __fastcall DetourFontGgoCreateGdiFont(void* this_ptr, void* /*edx*/) {
        ApplyProportionalFontOverride(this_ptr);
        return g_orig_create_gdi_font(this_ptr);
    }

    void* InstallSingleTrampoline(std::uintptr_t target_addr,
                                  std::size_t stolen_len,
                                  std::uintptr_t detour_addr) {
        if (!trampoline_slab || stolen_len < kJmpPatchBytes) {
            return nullptr;
        }
        std::uint8_t* tramp = trampoline_slab + trampoline_used;
        trampoline_used += kTrampolineSlotSize;

        auto* target_ptr = reinterpret_cast<std::uint8_t*>(target_addr);
        InstalledPatch patch;
        patch.target_addr = target_addr;
        patch.patch_len = stolen_len;
        std::memcpy(patch.original_bytes.data(), target_ptr, stolen_len);

        std::memcpy(tramp, target_ptr, stolen_len);
        WriteRel32(tramp + stolen_len, target_addr + stolen_len);

        DWORD old_protect = 0;
        if (!VirtualProtect(target_ptr, stolen_len, PAGE_EXECUTE_READWRITE, &old_protect)) {
            return nullptr;
        }
        WriteRel32(target_ptr, detour_addr);
        for (std::size_t i = kJmpPatchBytes; i < stolen_len; ++i) {
            target_ptr[i] = kNopOpcode;
        }
        VirtualProtect(target_ptr, stolen_len, old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), target_ptr, stolen_len);

        patches.push_back(patch);
        return tramp;
    }

    void RestoreAll() noexcept {
        for (const auto& patch : patches) {
            auto* target_ptr = reinterpret_cast<std::uint8_t*>(patch.target_addr);
            DWORD old_protect = 0;
            if (VirtualProtect(
                    target_ptr, patch.patch_len, PAGE_EXECUTE_READWRITE, &old_protect)) {
                std::memcpy(target_ptr, patch.original_bytes.data(), patch.patch_len);
                VirtualProtect(target_ptr, patch.patch_len, old_protect, &old_protect);
                FlushInstructionCache(GetCurrentProcess(), target_ptr, patch.patch_len);
            }
        }
        patches.clear();
        if (trampoline_slab) {
            VirtualFree(trampoline_slab, 0, MEM_RELEASE);
            trampoline_slab = nullptr;
        }
        trampoline_used = 0;
        installed = false;
    }
};

RuntimeHookManager::RuntimeHookManager(const TranslationCatalog* catalog)
    : impl_(std::make_unique<Impl>(catalog)) {}

RuntimeHookManager::RuntimeHookManager(RuntimeHookManager&& other) noexcept = default;
RuntimeHookManager& RuntimeHookManager::operator=(RuntimeHookManager&& other) noexcept = default;

RuntimeHookManager::~RuntimeHookManager() {
    RemoveHooks();
}

bool RuntimeHookManager::VerifyEngineSignatures(std::uintptr_t module_base) const noexcept {
    const std::intptr_t delta =
        static_cast<std::intptr_t>(module_base) -
        static_cast<std::intptr_t>(kExpectedImageBase);

    return CheckBytesAt(
               static_cast<std::uintptr_t>(kParseMessageHeaderAndBodyVa + delta),
               kExpectedPrologueParseMsg,
               sizeof(kExpectedPrologueParseMsg)) &&
           CheckBytesAt(
               static_cast<std::uintptr_t>(kParseChoicesFromScriptVa + delta),
               kExpectedPrologueParseChoices,
               sizeof(kExpectedPrologueParseChoices)) &&
           CheckBytesAt(
               static_cast<std::uintptr_t>(kAppendChoiceStringVa + delta),
               kExpectedPrologueAppendChoice,
               sizeof(kExpectedPrologueAppendChoice)) &&
           CheckBytesAt(
               static_cast<std::uintptr_t>(kSobLoadScriptVa + delta),
               kExpectedPrologueSobLoad,
               sizeof(kExpectedPrologueSobLoad)) &&
           CheckBytesAt(
               static_cast<std::uintptr_t>(kMessageWindowInitVa + delta),
               kExpectedPrologueMsgWndInit,
               sizeof(kExpectedPrologueMsgWndInit)) &&
           CheckBytesAt(
               static_cast<std::uintptr_t>(kMessageWindowDrawCharVa + delta),
               kExpectedPrologueMsgWndDraw,
               sizeof(kExpectedPrologueMsgWndDraw)) &&
           CheckBytesAt(
               static_cast<std::uintptr_t>(kFontGgoCreateGdiFontVa + delta),
               kExpectedPrologueCreateGdiFont,
               sizeof(kExpectedPrologueCreateGdiFont));
}

bool RuntimeHookManager::InstallHooks(std::uintptr_t module_base) {
    if (impl_->installed) {
        return true;
    }
    if (!VerifyEngineSignatures(module_base)) {
        Log("RuntimeHookManager: engine prologue verification failed at base 0x%08X",
            static_cast<unsigned int>(module_base));
        return false;
    }

    constexpr std::size_t kHookCount = 7;
    impl_->trampoline_slab = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr,
        kHookCount * kTrampolineSlotSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!impl_->trampoline_slab) {
        return false;
    }

    const std::intptr_t delta =
        static_cast<std::intptr_t>(module_base) -
        static_cast<std::intptr_t>(kExpectedImageBase);

    g_active_hook_mgr = this;

    g_orig_parse_msg = reinterpret_cast<ParseMessageHeaderAndBodyFn>(
        impl_->InstallSingleTrampoline(
            static_cast<std::uintptr_t>(kParseMessageHeaderAndBodyVa + delta),
            sizeof(kExpectedPrologueParseMsg),
            reinterpret_cast<std::uintptr_t>(&Impl::DetourParseMessageHeaderAndBody)));

    g_orig_parse_choices = reinterpret_cast<ParseChoicesFromScriptFn>(
        impl_->InstallSingleTrampoline(
            static_cast<std::uintptr_t>(kParseChoicesFromScriptVa + delta),
            sizeof(kExpectedPrologueParseChoices),
            reinterpret_cast<std::uintptr_t>(&Impl::DetourParseChoicesFromScript)));

    g_orig_append_choice = reinterpret_cast<AppendChoiceStringFn>(
        impl_->InstallSingleTrampoline(
            static_cast<std::uintptr_t>(kAppendChoiceStringVa + delta),
            sizeof(kExpectedPrologueAppendChoice),
            reinterpret_cast<std::uintptr_t>(&Impl::DetourAppendChoiceString)));

    g_orig_sob_load = reinterpret_cast<SobLoadScriptFn>(
        impl_->InstallSingleTrampoline(
            static_cast<std::uintptr_t>(kSobLoadScriptVa + delta),
            sizeof(kExpectedPrologueSobLoad),
            reinterpret_cast<std::uintptr_t>(&Impl::DetourSobLoadScript)));

    g_orig_msg_wnd_init = reinterpret_cast<MessageWindowInitFn>(
        impl_->InstallSingleTrampoline(
            static_cast<std::uintptr_t>(kMessageWindowInitVa + delta),
            sizeof(kExpectedPrologueMsgWndInit),
            reinterpret_cast<std::uintptr_t>(&Impl::DetourMessageWindowInit)));

    g_orig_msg_wnd_draw = reinterpret_cast<MessageWindowDrawCharFn>(
        impl_->InstallSingleTrampoline(
            static_cast<std::uintptr_t>(kMessageWindowDrawCharVa + delta),
            sizeof(kExpectedPrologueMsgWndDraw),
            reinterpret_cast<std::uintptr_t>(&Impl::DetourMessageWindowDrawChar)));

    g_orig_create_gdi_font = reinterpret_cast<FontGgoCreateGdiFontFn>(
        impl_->InstallSingleTrampoline(
            static_cast<std::uintptr_t>(kFontGgoCreateGdiFontVa + delta),
            sizeof(kExpectedPrologueCreateGdiFont),
            reinterpret_cast<std::uintptr_t>(&Impl::DetourFontGgoCreateGdiFont)));

    impl_->installed =
        (g_orig_parse_msg != nullptr) &&
        (g_orig_parse_choices != nullptr) &&
        (g_orig_append_choice != nullptr) &&
        (g_orig_sob_load != nullptr) &&
        (g_orig_msg_wnd_init != nullptr) &&
        (g_orig_msg_wnd_draw != nullptr) &&
        (g_orig_create_gdi_font != nullptr);

    if (!impl_->installed) {
        impl_->RestoreAll();
        g_active_hook_mgr = nullptr;
        return false;
    }

    Log("RuntimeHookManager: installed %zu Lucifen runtime hooks", kHookCount);
    return true;
}

void RuntimeHookManager::RemoveHooks() noexcept {
    if (!impl_) {
        return;
    }
    impl_->RestoreAll();
    if (g_active_hook_mgr == this) {
        g_active_hook_mgr = nullptr;
    }
}

bool RuntimeHookManager::AreHooksInstalled() const noexcept {
    return impl_ && impl_->installed;
}

char* RuntimeHookManager::FormatIntoNextScratchBuffer(std::string_view raw_script_str) {
    char* slot = impl_->NextScratchSlot();
    if (!impl_->catalog) {
        const std::size_t copy_len =
            std::min(raw_script_str.size(), kDefaultScratchBufferCapacity - 1U);
        std::memcpy(slot, raw_script_str.data(), copy_len);
        slot[copy_len] = '\0';
        return slot;
    }
    impl_->catalog->FormatDialogueOrNarration(
        raw_script_str, slot, kDefaultScratchBufferCapacity);
    return slot;
}

bool RuntimeHookManager::RewriteChoiceBlock(const std::uint8_t* choice_data,
                                            std::vector<std::uint8_t>& out_buf) const {
    if (!choice_data || !impl_->catalog) {
        return false;
    }
    const std::uint8_t count = choice_data[0];
    const std::uint8_t has_inline = choice_data[1];
    if (count == 0U || count > kMaxChoicesCount || has_inline == 0U) {
        return false;
    }

    out_buf.clear();
    out_buf.push_back(count);
    out_buf.push_back(has_inline);

    const std::uint8_t* cursor = choice_data + kChoiceHeaderBytes;
    bool any_translated = false;

    for (std::uint8_t i = 0; i < count; ++i) {
        const std::uint8_t orig_stride = cursor[0];
        if (orig_stride < kMinOptionStride) {
            return false;
        }
        const std::uint8_t flag = cursor[1];
        const auto* raw_text = reinterpret_cast<const char*>(cursor + kChoiceOptionPrefixBytes);
        const std::size_t max_text_len =
            static_cast<std::size_t>(orig_stride - kChoiceOptionPrefixBytes);
        const std::size_t text_len = strnlen(raw_text, max_text_len);
        const std::string_view jp_choice(raw_text, text_len);

        std::string final_choice(jp_choice);
        if (const auto hit = impl_->catalog->LookupTranslation(jp_choice)) {
            final_choice = *hit;
            any_translated = true;
        }

        const std::size_t max_allowed_text =
            static_cast<std::size_t>(kMaxOptionStride - kChoiceOptionPrefixBytes - 1U);
        if (final_choice.size() > max_allowed_text) {
            final_choice.resize(max_allowed_text);
        }

        const auto new_stride = static_cast<std::uint8_t>(
            kChoiceOptionPrefixBytes + final_choice.size() + 1U);
        out_buf.push_back(new_stride);
        out_buf.push_back(flag);
        out_buf.insert(out_buf.end(), final_choice.begin(), final_choice.end());
        out_buf.push_back(0U);

        cursor += orig_stride;
    }

    return any_translated;
}

std::size_t RuntimeHookManager::PatchLoadedSobStringTable(
    std::uint8_t* loaded_image,
    const std::uint8_t* sob_file_buf) const {
    if (!loaded_image || !sob_file_buf || !impl_->catalog) {
        return 0;
    }
    if (std::memcmp(sob_file_buf, "SOB0", kSobMagicBytes) != 0) {
        return 0;
    }

    const std::uint32_t reloc_size = ReadU32Le(sob_file_buf + kSobMagicBytes);
    const std::size_t reloc_end = kSobHeaderPrefixBytes + static_cast<std::size_t>(reloc_size);
    const std::uint32_t code_size = ReadU32Le(sob_file_buf + reloc_end);
    const std::uint32_t total_size = ReadU32Le(sob_file_buf + reloc_end + kDwordBytes);
    if (total_size <= code_size) {
        return 0;
    }

    std::size_t pos = kSobHeaderPrefixBytes;
    std::size_t patched_count = 0;

    for (std::size_t t = 0; t < kSobSubtableCount; ++t) {
        if (pos + kDwordBytes > reloc_end) {
            return patched_count;
        }
        const std::uint32_t count = ReadU32Le(sob_file_buf + pos);
        pos += kDwordBytes;
        const std::size_t table_bytes = static_cast<std::size_t>(count) * kRelocPairBytes;
        if (pos + table_bytes > reloc_end) {
            return patched_count;
        }

        if (t != kSobDataSubtableIndex) {
            pos += table_bytes;
            continue;
        }

        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t code_off = ReadU32Le(sob_file_buf + pos);
            const std::uint32_t target_off = ReadU32Le(sob_file_buf + pos + kDwordBytes);
            pos += kRelocPairBytes;

            if (code_off + kDwordBytes > code_size ||
                target_off < code_size ||
                target_off >= total_size) {
                continue;
            }

            if (code_off >= kSyscallHeaderBackOffset &&
                ReadU32Le(loaded_image + code_off - kSyscallHeaderBackOffset) ==
                    kSyscallSceneTitleId) {
                continue;
            }

            const auto* str_ptr = reinterpret_cast<const char*>(loaded_image + target_off);
            const std::size_t max_avail =
                std::min<std::size_t>(total_size - target_off, kMaxSobStringScanBytes);
            const std::size_t len = strnlen(str_ptr, max_avail);
            if (len == 0U || len >= max_avail) {
                continue;
            }

            const std::string_view candidate(str_ptr, len);
            if (candidate.front() == '#' || candidate.back() == '$') {
                continue;
            }

            if (const char* persistent_tr =
                    impl_->catalog->LookupPersistentPointer(candidate)) {
                const auto tr_addr = reinterpret_cast<std::uint32_t>(persistent_tr);
                std::memcpy(loaded_image + code_off, &tr_addr, sizeof(tr_addr));
                ++patched_count;
            }
        }
    }

    return patched_count;
}

void RuntimeHookManager::ApplyProportionalFontOverride(void* font_ggo_this) noexcept {
    if (!font_ggo_this) {
        return;
    }
    auto* base = static_cast<std::uint8_t*>(font_ggo_this);
    base[kFontGgoLfCharSetOffset] = kShiftJisCharset;
    auto* face_dest = reinterpret_cast<char*>(base + kFontGgoLfFaceNameOffset);
    strncpy_s(face_dest, kMaxFaceNameBytes, kProportionalFontFace, _TRUNCATE);
}

void RuntimeHookManager::ApplyMessageWindowProportionalFlag(void* msg_wnd_this) noexcept {
    if (!msg_wnd_this) {
        return;
    }
    auto* base = static_cast<std::uint8_t*>(msg_wnd_this);
    base[kMsgWndProportionalFlagOffset] = 1U;
}

}  // namespace oretsuba::proxy
