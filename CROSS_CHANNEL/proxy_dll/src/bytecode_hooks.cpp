// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "bytecode_hooks.h"

#include "log.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <utility>

namespace bytecode_hooks {

namespace {

constexpr std::size_t kOp86ProloguePrefixLen = 3u;
constexpr std::size_t kOp86BodyAnchorOffset  = 0x07u;
constexpr std::size_t kOp86BodyAnchorLen     = 10u;

constexpr std::uint8_t kOp86ProloguePrefix[kOp86ProloguePrefixLen] = {
    0x53u, 0x8Bu, 0x1Du  // push ebx; mov ebx, dword ptr [imm32]
};

constexpr std::uint8_t kOp86BodyAnchor[kOp86BodyAnchorLen] = {
    0x56u, 0x8Bu, 0xF3u, 0x57u,
    0x69u, 0xF6u, 0x84u, 0x05u, 0x00u, 0x00u
    // push esi; mov esi, ebx; push edi; imul esi, esi, 0x584
};

constexpr std::uint8_t kMovEdiFromImm32Prefix[2] = {0x8Bu, 0x3Du};
constexpr std::uint8_t kMovImm32FromEdiPrefix[2] = {0x89u, 0x3Du};

constexpr std::size_t kTrampolinePageBytes = 256u;
constexpr std::size_t kPostTrampOffset     = 64u;
constexpr std::size_t kMaxOp86SourceBytes  = 1024u;

constexpr std::uint8_t kOpPushad     = 0x60u;
constexpr std::uint8_t kOpPopad      = 0x61u;
constexpr std::uint8_t kOpPushImm32  = 0x68u;
constexpr std::uint8_t kOpNop        = 0x90u;
constexpr std::uint8_t kOpPushfd     = 0x9Cu;
constexpr std::uint8_t kOpPopfd      = 0x9Du;
constexpr std::uint8_t kOpRet        = 0xC3u;
constexpr std::uint8_t kOpCallRel32  = 0xE8u;

// SEH-isolated POD memory helpers (kept free of C++ stack objects with
// destructors so MSVC /EHsc never triggers C2712).
bool ProbeReadableBytes(const void* src, std::uint8_t* dst, std::size_t len) noexcept {
    if (!src || !dst || len == 0) {
        return false;
    }
    __try {
        std::memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadScriptCursorCString(std::uintptr_t cursor_global_va,
                             const std::uint8_t*& out_jp_ptr,
                             std::size_t& out_jp_len) noexcept {
    out_jp_ptr = nullptr;
    out_jp_len = 0;
    if (cursor_global_va == 0) {
        return false;
    }
    __try {
        const auto* p = *reinterpret_cast<const std::uint8_t* const*>(cursor_global_va);
        if (!p) {
            return false;
        }
        std::size_t len = 0;
        while (len <= kMaxOp86SourceBytes && p[len] != 0) {
            ++len;
        }
        if (len > kMaxOp86SourceBytes || p[len] != 0) {
            return false;
        }
        out_jp_ptr = p;
        out_jp_len = len;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteScriptCursorPtr(std::uintptr_t cursor_global_va,
                          std::uintptr_t new_cursor_va) noexcept {
    if (cursor_global_va == 0) {
        return false;
    }
    __try {
        *reinterpret_cast<std::uintptr_t*>(cursor_global_va) = new_cursor_va;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PatchCodeBytes(std::uint8_t* site,
                    const std::uint8_t* patch,
                    std::size_t len) noexcept {
    if (!site || !patch || len == 0) {
        return false;
    }
    DWORD old_prot = 0;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old_prot)) {
        return false;
    }
    std::memcpy(site, patch, len);
    DWORD dummy = 0;
    VirtualProtect(site, len, old_prot, &dummy);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

class ExecutablePage {
public:
    ExecutablePage() = default;
    ~ExecutablePage() { Reset(); }

    ExecutablePage(const ExecutablePage&) = delete;
    ExecutablePage& operator=(const ExecutablePage&) = delete;

    ExecutablePage(ExecutablePage&& other) noexcept
        : base_(std::exchange(other.base_, nullptr)) {}

    ExecutablePage& operator=(ExecutablePage&& other) noexcept {
        if (this != &other) {
            Reset();
            base_ = std::exchange(other.base_, nullptr);
        }
        return *this;
    }

    bool Allocate(std::size_t bytes) noexcept {
        Reset();
        base_ = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE));
        return base_ != nullptr;
    }

    void Reset() noexcept {
        if (base_) {
            VirtualFree(base_, 0, MEM_RELEASE);
            base_ = nullptr;
        }
    }

    std::uint8_t* Get() const noexcept { return base_; }

private:
    std::uint8_t* base_ = nullptr;
};

void EmitU32Le(std::uint8_t*& p, std::uint32_t v) noexcept {
    std::memcpy(p, &v, sizeof(v));
    p += sizeof(v);
}

void EmitCallPlusNopPatch(std::uint8_t (&patch)[kOp86HookPatchLen],
                          const std::uint8_t* site,
                          const std::uint8_t* target) noexcept {
    const auto site_va   = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(site));
    const auto target_va = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target));
    const std::uint32_t rel32 = target_va - (site_va + 5u);
    patch[0] = kOpCallRel32;
    std::memcpy(patch + 1, &rel32, sizeof(rel32));
    patch[5] = kOpNop;
}

}  // namespace

bool ValidateOp86HandlerBytes(const std::uint8_t* fn_bytes,
                              std::size_t avail_bytes,
                              std::uint32_t* out_text_ptr_global_va) noexcept {
    if (!fn_bytes || avail_bytes < kOp86HandlerMinBytes) {
        return false;
    }
    if (std::memcmp(fn_bytes, kOp86ProloguePrefix, kOp86ProloguePrefixLen) != 0) {
        return false;
    }
    if (std::memcmp(fn_bytes + kOp86BodyAnchorOffset,
                    kOp86BodyAnchor, kOp86BodyAnchorLen) != 0) {
        return false;
    }
    if (std::memcmp(fn_bytes + kOp86PreHookOffset,
                    kMovEdiFromImm32Prefix, sizeof(kMovEdiFromImm32Prefix)) != 0) {
        return false;
    }
    if (std::memcmp(fn_bytes + kOp86PostHookOffset,
                    kMovImm32FromEdiPrefix, sizeof(kMovImm32FromEdiPrefix)) != 0) {
        return false;
    }

    std::uint32_t pre_global_va  = 0;
    std::uint32_t post_global_va = 0;
    std::memcpy(&pre_global_va,
                fn_bytes + kOp86PreHookOffset + sizeof(kMovEdiFromImm32Prefix),
                sizeof(pre_global_va));
    std::memcpy(&post_global_va,
                fn_bytes + kOp86PostHookOffset + sizeof(kMovImm32FromEdiPrefix),
                sizeof(post_global_va));
    if (pre_global_va == 0 || pre_global_va != post_global_va) {
        return false;
    }
    if (out_text_ptr_global_va) {
        *out_text_ptr_global_va = pre_global_va;
    }
    return true;
}

struct BytecodeHookController::Impl {
    const translator_logic::TranslationMap* translations = nullptr;
    ExecutablePage                          trampoline_page;
    std::uint8_t*                           pre_site = nullptr;
    std::uint8_t*                           post_site = nullptr;
    std::uint8_t                            orig_pre_bytes[kOp86HookPatchLen]{};
    std::uint8_t                            orig_post_bytes[kOp86HookPatchLen]{};
    std::uintptr_t                          text_ptr_global_va = 0;
    std::uintptr_t                          saved_edi = 0;
    const std::uint8_t*                     orig_jp_end = nullptr;
    bool                                    substituted = false;
    bool                                    installed = false;
    int                                     calls = 0;
    int                                     substitutions = 0;
    char                                    slot_buf[kOp86SlotTextCapacity]{};

    static void __cdecl OnPreOp86Thunk(Impl* self) noexcept {
        if (!self) {
            return;
        }
        ++self->calls;
        self->substituted = false;
        self->orig_jp_end = nullptr;

        if (!self->translations || self->translations->empty()) {
            return;
        }

        const std::uint8_t* jp_ptr = nullptr;
        std::size_t jp_len = 0;
        if (!ReadScriptCursorCString(self->text_ptr_global_va, jp_ptr, jp_len) ||
            jp_len == 0) {
            return;
        }

        const std::string jp_cp932(reinterpret_cast<const char*>(jp_ptr), jp_len);
        std::string formatted;
        if (!translator_logic::FormatBacklogSlotText(
                jp_cp932,
                *self->translations,
                formatted,
                translator_logic::kBacklogWrapCols,
                kOp86SlotTextCapacity)) {
            return;
        }

        if (formatted.size() >= kOp86SlotTextCapacity) {
            return;
        }

        std::memcpy(self->slot_buf, formatted.data(), formatted.size());
        self->slot_buf[formatted.size()] = '\0';

        if (!WriteScriptCursorPtr(
                self->text_ptr_global_va,
                reinterpret_cast<std::uintptr_t>(self->slot_buf))) {
            return;
        }

        self->orig_jp_end = jp_ptr + jp_len + 1;
        self->substituted = true;
    }

    static void __cdecl OnPostOp86Thunk(Impl* self) noexcept {
        if (!self) {
            return;
        }
        if (self->substituted && self->orig_jp_end) {
            WriteScriptCursorPtr(
                self->text_ptr_global_va,
                reinterpret_cast<std::uintptr_t>(self->orig_jp_end));
            self->substituted = false;
            self->orig_jp_end = nullptr;
            ++self->substitutions;
        } else {
            WriteScriptCursorPtr(self->text_ptr_global_va, self->saved_edi);
        }
    }
};

BytecodeHookController::BytecodeHookController()
    : impl_(std::make_unique<Impl>()) {}

BytecodeHookController::~BytecodeHookController() {
    Shutdown();
}

BytecodeHookController::BytecodeHookController(BytecodeHookController&&) noexcept = default;
BytecodeHookController& BytecodeHookController::operator=(BytecodeHookController&&) noexcept = default;

bool BytecodeHookController::IsInstalled() const noexcept {
    return impl_ && impl_->installed;
}

bool BytecodeHookController::Install(std::uintptr_t exe_base,
                                     const translator_logic::TranslationMap* map) {
#if INTPTR_MAX != INT32_MAX
    (void)exe_base;
    (void)map;
    return false;
#else
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    if (impl_->installed) {
        impl_->translations = map;
        return true;
    }
    if (exe_base == 0 || !map) {
        Log("OP86-HOOK: invalid install arguments (exe_base=0x%08X map=%p)",
            static_cast<unsigned>(exe_base), static_cast<const void*>(map));
        return false;
    }

    auto* const fn_base = reinterpret_cast<std::uint8_t*>(exe_base + kOp86HandlerRva);
    std::uint8_t snapshot[kOp86HandlerMinBytes]{};
    if (!ProbeReadableBytes(fn_base, snapshot, sizeof(snapshot))) {
        Log("OP86-HOOK: RVA 0x%X (%p) not readable — skip", kOp86HandlerRva, fn_base);
        return false;
    }

    std::uint32_t resolved_global_va = 0;
    if (!ValidateOp86HandlerBytes(snapshot, sizeof(snapshot), &resolved_global_va)) {
        Log("OP86-HOOK: signature mismatch at RVA 0x%X (%p) "
            "got=%02X %02X %02X ... +0x22=%02X %02X +0x57=%02X %02X — skip",
            kOp86HandlerRva, fn_base,
            snapshot[0], snapshot[1], snapshot[2],
            snapshot[kOp86PreHookOffset], snapshot[kOp86PreHookOffset + 1],
            snapshot[kOp86PostHookOffset], snapshot[kOp86PostHookOffset + 1]);
        return false;
    }

    if (!impl_->trampoline_page.Allocate(kTrampolinePageBytes)) {
        Log("OP86-HOOK: VirtualAlloc failed gle=%lu", GetLastError());
        return false;
    }

    impl_->translations       = map;
    impl_->text_ptr_global_va = static_cast<std::uintptr_t>(resolved_global_va);
    impl_->pre_site           = fn_base + kOp86PreHookOffset;
    impl_->post_site          = fn_base + kOp86PostHookOffset;
    std::memcpy(impl_->orig_pre_bytes,
                snapshot + kOp86PreHookOffset,
                kOp86HookPatchLen);
    std::memcpy(impl_->orig_post_bytes,
                snapshot + kOp86PostHookOffset,
                kOp86HookPatchLen);

    const auto self_va =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(impl_.get()));
    const auto saved_edi_va =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&impl_->saved_edi));
    const auto pre_fn_va =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&Impl::OnPreOp86Thunk));
    const auto post_fn_va =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&Impl::OnPostOp86Thunk));

    std::uint8_t* const pre_tramp  = impl_->trampoline_page.Get();
    std::uint8_t* const post_tramp = pre_tramp + kPostTrampOffset;

    // Pre-hook trampoline (replaces `8B 3D <g_textPtrGlobal>` at +0x22):
    //   pushad; pushfd; push self; call OnPreOp86Thunk; add esp, 4;
    //   popfd; popad; mov edi, [g_textPtrGlobal]; ret
    {
        std::uint8_t* p = pre_tramp;
        *p++ = kOpPushad;
        *p++ = kOpPushfd;
        *p++ = kOpPushImm32;
        EmitU32Le(p, self_va);
        *p++ = kOpCallRel32;
        const auto next_va =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p + 4));
        EmitU32Le(p, pre_fn_va - next_va);
        *p++ = 0x83u; *p++ = 0xC4u; *p++ = 0x04u;  // add esp, 4
        *p++ = kOpPopfd;
        *p++ = kOpPopad;
        *p++ = kMovEdiFromImm32Prefix[0];
        *p++ = kMovEdiFromImm32Prefix[1];
        EmitU32Le(p, resolved_global_va);
        *p++ = kOpRet;
    }

    // Post-hook trampoline (replaces `89 3D <g_textPtrGlobal>` at +0x57):
    //   mov [&self->saved_edi], edi;
    //   pushad; pushfd; push self; call OnPostOp86Thunk; add esp, 4;
    //   popfd; popad; ret
    {
        std::uint8_t* p = post_tramp;
        *p++ = kMovImm32FromEdiPrefix[0];
        *p++ = kMovImm32FromEdiPrefix[1];
        EmitU32Le(p, saved_edi_va);
        *p++ = kOpPushad;
        *p++ = kOpPushfd;
        *p++ = kOpPushImm32;
        EmitU32Le(p, self_va);
        *p++ = kOpCallRel32;
        const auto next_va =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p + 4));
        EmitU32Le(p, post_fn_va - next_va);
        *p++ = 0x83u; *p++ = 0xC4u; *p++ = 0x04u;  // add esp, 4
        *p++ = kOpPopfd;
        *p++ = kOpPopad;
        *p++ = kOpRet;
    }

    FlushInstructionCache(GetCurrentProcess(), pre_tramp, kTrampolinePageBytes);

    std::uint8_t pre_patch[kOp86HookPatchLen]{};
    std::uint8_t post_patch[kOp86HookPatchLen]{};
    EmitCallPlusNopPatch(pre_patch, impl_->pre_site, pre_tramp);
    EmitCallPlusNopPatch(post_patch, impl_->post_site, post_tramp);

    if (!PatchCodeBytes(impl_->post_site, post_patch, kOp86HookPatchLen)) {
        Log("OP86-HOOK: VirtualProtect failed at post-site %p gle=%lu",
            impl_->post_site, GetLastError());
        impl_->trampoline_page.Reset();
        return false;
    }
    if (!PatchCodeBytes(impl_->pre_site, pre_patch, kOp86HookPatchLen)) {
        Log("OP86-HOOK: VirtualProtect failed at pre-site %p gle=%lu",
            impl_->pre_site, GetLastError());
        PatchCodeBytes(impl_->post_site, impl_->orig_post_bytes, kOp86HookPatchLen);
        impl_->trampoline_page.Reset();
        return false;
    }

    impl_->installed = true;
    Log("OP86-HOOK: hooked RVA 0x%X (+0x22=%p, +0x57=%p, textPtrGlobal=0x%08X)",
        kOp86HandlerRva, impl_->pre_site, impl_->post_site, resolved_global_va);
    return true;
#endif
}

void BytecodeHookController::Shutdown() noexcept {
    if (!impl_ || !impl_->installed) {
        return;
    }
    if (impl_->pre_site) {
        PatchCodeBytes(impl_->pre_site, impl_->orig_pre_bytes, kOp86HookPatchLen);
    }
    if (impl_->post_site) {
        PatchCodeBytes(impl_->post_site, impl_->orig_post_bytes, kOp86HookPatchLen);
    }
    Log("OP86-HOOK: shutdown — calls=%d substitutions=%d",
        impl_->calls, impl_->substitutions);
    impl_->installed   = false;
    impl_->substituted = false;
    impl_->orig_jp_end = nullptr;
    impl_->pre_site    = nullptr;
    impl_->post_site   = nullptr;
    impl_->trampoline_page.Reset();
}

namespace {

BytecodeHookController& GlobalController() {
    static BytecodeHookController controller;
    return controller;
}

}  // namespace

bool InstallBytecodeMenuHooks(std::uintptr_t exe_base,
                              const translator_logic::TranslationMap* map) {
    return GlobalController().Install(exe_base, map);
}

void ShutdownBytecodeMenuHooks() {
    GlobalController().Shutdown();
}

}  // namespace bytecode_hooks
