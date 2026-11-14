// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"

#include "log.h"
#include "menu_autopatch.h"
#include "translator_logic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

// Verified RVAs in retail Adieu SiglusEngine.exe (PE32, ImageBase 0x00400000):
//   0x0010C3A0: basic_string<wchar_t>::append(first, last, forward_iterator_tag)
//               (__thiscall, 3 stack DWORDs -> ret 0x0C at +0xCC)
//   0x0014C8E0: Scene::get_str(std::wstring* out, int scn_no, int str_no)
//               (__thiscall, 3 stack DWORDs -> ret 0x0C)
//   0x0014C9C0: Scene::get_cur_str(std::wstring* out, int str_no)
//               (__thiscall, 2 stack DWORDs -> ret 0x08)
constexpr uint32_t kWstringAssignRangeRva = 0x0010C3A0u;
constexpr uint32_t kWstringAssignRetOffset = 0x000000CCu;
constexpr uint32_t kSceneGetStrRva = 0x0014C8E0u;
constexpr uint32_t kSceneGetCurStrRva = 0x0014C9C0u;

constexpr std::size_t kHookPatchBytes = 5u;
constexpr std::size_t kTrampolineSlotBytes = 16u;
constexpr std::size_t kTrampolineAllocBytes = 64u;
constexpr uint8_t kExpectedSehPrologue[kHookPatchBytes] = {
    0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu,  // push ebp; mov ebp, esp; push -1
};
constexpr uint8_t kExpectedRet0C[3] = {
    0xC2u, 0x0Cu, 0x00u,  // ret 0x0C (3 stack DWORDs)
};
constexpr uint8_t kJmpRel32Opcode = 0xE9u;
constexpr uint8_t kInt3PaddingByte = 0xCCu;

constexpr DWORD kInitialPathBufferChars = MAX_PATH;
constexpr DWORD kMaxPathBufferChars = 32768u;
constexpr uint32_t kMaxPeHeaderOffset = 0x1000u;

// Verified 24-byte release MSVC std::wstring layout inside SiglusEngine.exe:
//   +0x00: union { wchar_t buf[8]; wchar_t* ptr; } (heap ptr active iff res >= 8)
//   +0x10: uint32_t size (code units excluding NUL)
//   +0x14: uint32_t res  (capacity in wchar_t; SSO capacity = 7)
constexpr uint32_t kSsoMinCapacity = 7u;
constexpr uint32_t kHeapCapacityThreshold = 8u;
constexpr uint32_t kMaxSaneStringCodeUnits = 65536u;

constexpr wchar_t kTranslationTableFileName[] = L"translation_table.tsv";
constexpr wchar_t kCaptureLogFileName[] = L"adieu_jp_strings.txt";
constexpr wchar_t kCaptureEnvVarName[] = L"ADIEU_CAPTURE_JP";

struct SiglusWstring {
    union Storage {
        wchar_t buf[8];
        wchar_t* ptr;
    } storage;
    uint32_t size;
    uint32_t res;
};

static_assert(sizeof(SiglusWstring) == 24u,
              "SiglusEngine.exe std::wstring ABI must be exactly 24 bytes");
static_assert(offsetof(SiglusWstring, size) == 0x10u,
              "SiglusWstring::size must reside at +0x10");
static_assert(offsetof(SiglusWstring, res) == 0x14u,
              "SiglusWstring::res must reside at +0x14");

using WstringAssignRangeFn = void(__fastcall*)(SiglusWstring* wstr_self,
                                               void* edx_unused,
                                               const wchar_t* first,
                                               const wchar_t* last,
                                               int iter_tag_dummy);

using SceneGetStrFn = SiglusWstring*(__fastcall*)(void* scene_self,
                                                  void* edx_unused,
                                                  SiglusWstring* out,
                                                  int scn_no,
                                                  int str_no);

using SceneGetCurStrFn = SiglusWstring*(__fastcall*)(void* scene_self,
                                                     void* edx_unused,
                                                     SiglusWstring* out,
                                                     int str_no);

INIT_ONCE g_runtime_lock_init = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_capture_cs{};

translator_logic::TranslationRuntimeFacade g_runtime;
WstringAssignRangeFn g_wstring_assign = nullptr;
SceneGetStrFn g_orig_get_str = nullptr;
SceneGetCurStrFn g_orig_get_cur_str = nullptr;
uint8_t* g_trampoline_block = nullptr;
uint8_t* g_patched_get_str = nullptr;
uint8_t* g_patched_get_cur_str = nullptr;
bool g_hooks_installed = false;

bool g_capture_enabled = false;
bool g_capture_file_attempted = false;
std::wstring g_exe_dir;
std::unordered_set<std::wstring> g_captured_jp;
std::unique_ptr<std::FILE, decltype(&std::fclose)> g_capture_file(nullptr,
                                                                  &std::fclose);

thread_local bool g_in_scene_hook = false;

BOOL CALLBACK InitCaptureLockCallback(PINIT_ONCE /*init_once*/,
                                      PVOID /*param*/,
                                      PVOID* /*ctx*/) noexcept {
    ::InitializeCriticalSection(&g_capture_cs);
    return TRUE;
}

CRITICAL_SECTION* GetCaptureLock() noexcept {
    ::InitOnceExecuteOnce(&g_runtime_lock_init, InitCaptureLockCallback,
                          nullptr, nullptr);
    return &g_capture_cs;
}

class ScopedCriticalSection {
public:
    explicit ScopedCriticalSection(CRITICAL_SECTION* cs) noexcept : cs_(cs) {
        ::EnterCriticalSection(cs_);
    }
    ~ScopedCriticalSection() { ::LeaveCriticalSection(cs_); }

    ScopedCriticalSection(const ScopedCriticalSection&) = delete;
    ScopedCriticalSection& operator=(const ScopedCriticalSection&) = delete;

private:
    CRITICAL_SECTION* cs_;
};

class ScopedReentrancyGuard {
public:
    explicit ScopedReentrancyGuard(bool& flag) noexcept
        : flag_(flag), entered_(!flag) {
        if (entered_) {
            flag_ = true;
        }
    }
    ~ScopedReentrancyGuard() {
        if (entered_) {
            flag_ = false;
        }
    }

    bool entered() const noexcept { return entered_; }

    ScopedReentrancyGuard(const ScopedReentrancyGuard&) = delete;
    ScopedReentrancyGuard& operator=(const ScopedReentrancyGuard&) = delete;

private:
    bool& flag_;
    bool entered_;
};

bool IsEnvFlagEnabled(const wchar_t* env_name) noexcept {
    wchar_t value[8] = {};
    const DWORD len = ::GetEnvironmentVariableW(env_name, value, 8u);
    return len == 1u && value[0] == L'1';
}

std::wstring QueryModuleDirectory(HMODULE mod) {
    std::vector<wchar_t> buffer(kInitialPathBufferChars, L'\0');
    DWORD len = 0u;

    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        len = ::GetModuleFileNameW(mod, buffer.data(),
                                   static_cast<DWORD>(buffer.size()));
        if (len == 0u) {
            return {};
        }
        if (len < buffer.size() - 1u &&
            ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        if (buffer.size() >= kMaxPathBufferChars) {
            return {};
        }
        buffer.assign(
            std::min<std::size_t>(buffer.size() * 2u, kMaxPathBufferChars),
            L'\0');
    }

    std::wstring full_path(buffer.data(), len);
    const std::size_t sep = full_path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return {};
    }
    full_path.resize(sep + 1u);
    return full_path;
}

bool FileExistsWide(const std::wstring& path) noexcept {
    if (path.empty()) {
        return false;
    }
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

std::wstring QueryCurrentDirectoryWithSep() {
    const DWORD required = ::GetCurrentDirectoryW(0u, nullptr);
    if (required == 0u || required > kMaxPathBufferChars) {
        return {};
    }
    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD written = ::GetCurrentDirectoryW(required, buffer.data());
    if (written == 0u || written >= required) {
        return {};
    }
    std::wstring cwd(buffer.data(), written);
    if (!cwd.empty() && cwd.back() != L'\\' && cwd.back() != L'/') {
        cwd.push_back(L'\\');
    }
    return cwd;
}

std::wstring ResolveMenuPatchDirectory(HMODULE self_module) {
    if (!g_exe_dir.empty() &&
        FileExistsWide(g_exe_dir + menu_autopatch::kMenuPatchFileName)) {
        return g_exe_dir;
    }

    std::wstring dll_dir;
    if (self_module != nullptr) {
        dll_dir = QueryModuleDirectory(self_module);
        if (!dll_dir.empty() &&
            FileExistsWide(dll_dir + menu_autopatch::kMenuPatchFileName)) {
            return dll_dir;
        }
    }

    const std::wstring cwd = QueryCurrentDirectoryWithSep();
    if (!cwd.empty() &&
        FileExistsWide(cwd + menu_autopatch::kMenuPatchFileName)) {
        return cwd;
    }

    if (!g_exe_dir.empty()) {
        return g_exe_dir;
    }
    if (!dll_dir.empty()) {
        return dll_dir;
    }
    return cwd;
}

std::wstring ResolveTranslationTablePath(HMODULE self_module) {
    if (!g_exe_dir.empty()) {
        const std::wstring exe_candidate = g_exe_dir + kTranslationTableFileName;
        if (FileExistsWide(exe_candidate)) {
            return exe_candidate;
        }
    }

    if (self_module != nullptr) {
        const std::wstring dll_dir = QueryModuleDirectory(self_module);
        if (!dll_dir.empty()) {
            const std::wstring dll_candidate =
                dll_dir + kTranslationTableFileName;
            if (FileExistsWide(dll_candidate)) {
                return dll_candidate;
            }
        }
    }

    const std::wstring cwd_candidate(kTranslationTableFileName);
    if (FileExistsWide(cwd_candidate)) {
        return cwd_candidate;
    }

    return {};
}

bool IsRvaRangeExecutable(const IMAGE_NT_HEADERS32* nt,
                          uint32_t rva,
                          std::size_t byte_len) noexcept {
    if (byte_len == 0u) {
        return false;
    }
    const uint64_t end_rva = static_cast<uint64_t>(rva) + byte_len;
    if (end_rva > nt->OptionalHeader.SizeOfImage) {
        return false;
    }

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    const uint16_t count = nt->FileHeader.NumberOfSections;
    for (uint16_t i = 0u; i < count; ++i) {
        const IMAGE_SECTION_HEADER& sec = sections[i];
        if ((sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0u) {
            continue;
        }
        const uint32_t sec_start = sec.VirtualAddress;
        const uint32_t sec_span =
            std::max<uint32_t>(sec.Misc.VirtualSize, sec.SizeOfRawData);
        const uint64_t sec_end = static_cast<uint64_t>(sec_start) + sec_span;
        if (rva >= sec_start && end_rva <= sec_end) {
            return true;
        }
    }
    return false;
}

bool VerifyHostIsSiglusEngine(const uint8_t* base) noexcept {
    if (base == nullptr) {
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<uint32_t>(dos->e_lfanew) > kMaxPeHeaderOffset) {
        return false;
    }

    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }

    if (!IsRvaRangeExecutable(
            nt, kWstringAssignRangeRva,
            kWstringAssignRetOffset + sizeof(kExpectedRet0C)) ||
        !IsRvaRangeExecutable(nt, kSceneGetStrRva, kHookPatchBytes) ||
        !IsRvaRangeExecutable(nt, kSceneGetCurStrRva, kHookPatchBytes)) {
        return false;
    }

    if (std::memcmp(base + kWstringAssignRangeRva, kExpectedSehPrologue,
                    kHookPatchBytes) != 0) {
        return false;
    }
    if (std::memcmp(base + kWstringAssignRangeRva + kWstringAssignRetOffset,
                    kExpectedRet0C, sizeof(kExpectedRet0C)) != 0) {
        return false;
    }
    if (std::memcmp(base + kSceneGetStrRva, kExpectedSehPrologue,
                    kHookPatchBytes) != 0) {
        return false;
    }
    if (std::memcmp(base + kSceneGetCurStrRva, kExpectedSehPrologue,
                    kHookPatchBytes) != 0) {
        return false;
    }
    return true;
}

bool BuildTrampolineAndPatch(uint8_t* target,
                             uint8_t* trampoline_slot,
                             const void* detour_fn) noexcept {
    // Copy stolen 5-byte prologue and append `E9 rel32` back to `target + 5`.
    std::memcpy(trampoline_slot, target, kHookPatchBytes);
    trampoline_slot[kHookPatchBytes] = kJmpRel32Opcode;
    const intptr_t back_rel =
        reinterpret_cast<intptr_t>(target + kHookPatchBytes) -
        reinterpret_cast<intptr_t>(trampoline_slot + kHookPatchBytes +
                                   kHookPatchBytes);
    const auto back_rel32 = static_cast<int32_t>(back_rel);
    std::memcpy(trampoline_slot + kHookPatchBytes + 1u, &back_rel32,
                sizeof(back_rel32));
    std::memset(trampoline_slot + 10u, kInt3PaddingByte,
                kTrampolineSlotBytes - 10u);
    ::FlushInstructionCache(::GetCurrentProcess(), trampoline_slot,
                            kTrampolineSlotBytes);

    DWORD old_protect = 0u;
    if (!::VirtualProtect(target, kHookPatchBytes, PAGE_EXECUTE_READWRITE,
                          &old_protect)) {
        return false;
    }

    uint8_t patch[kHookPatchBytes];
    patch[0] = kJmpRel32Opcode;
    const intptr_t forward_rel =
        reinterpret_cast<intptr_t>(detour_fn) -
        reinterpret_cast<intptr_t>(target + kHookPatchBytes);
    const auto forward_rel32 = static_cast<int32_t>(forward_rel);
    std::memcpy(patch + 1u, &forward_rel32, sizeof(forward_rel32));
    std::memcpy(target, patch, kHookPatchBytes);

    DWORD dummy = 0u;
    ::VirtualProtect(target, kHookPatchBytes, old_protect, &dummy);
    ::FlushInstructionCache(::GetCurrentProcess(), target, kHookPatchBytes);
    return true;
}

std::wstring_view ReadSiglusWstring(const SiglusWstring* wstr) noexcept {
    if (wstr == nullptr) {
        return {};
    }
    if (wstr->res < kSsoMinCapacity || wstr->size > wstr->res ||
        wstr->size > kMaxSaneStringCodeUnits) {
        return {};
    }
    const wchar_t* data = (wstr->res >= kHeapCapacityThreshold)
                              ? wstr->storage.ptr
                              : wstr->storage.buf;
    if (data == nullptr) {
        return {};
    }
    return std::wstring_view(data, wstr->size);
}

void AssignSiglusWstring(SiglusWstring* wstr,
                         std::wstring_view replacement) noexcept {
    if (wstr == nullptr || g_wstring_assign == nullptr) {
        return;
    }
    // Reset logical length to 0 while keeping `res` and any heap pointer intact
    // so SiglusEngine.exe's RVA 0x10C3A0 (`append(first, last, iter_tag)`)
    // reuses or frees existing CRT heap storage cleanly instead of appending.
    wchar_t* cur_buf = (wstr->res >= kHeapCapacityThreshold)
                           ? wstr->storage.ptr
                           : wstr->storage.buf;
    if (cur_buf == nullptr) {
        return;
    }
    wstr->size = 0u;
    cur_buf[0] = L'\0';

    const wchar_t* first = replacement.data();
    const wchar_t* last = first + replacement.size();
    g_wstring_assign(wstr, nullptr, first, last, 0);
}

void MaybeCaptureJapaneseLine(std::wstring_view jp) {
    if (!g_capture_enabled || jp.empty()) {
        return;
    }

    ScopedCriticalSection lock(GetCaptureLock());
    if (!g_captured_jp.emplace(jp).second) {
        return;
    }

    if (!g_capture_file_attempted) {
        g_capture_file_attempted = true;
        const std::wstring cap_path =
            g_exe_dir.empty() ? std::wstring(kCaptureLogFileName)
                              : (g_exe_dir + kCaptureLogFileName);
        g_capture_file.reset(_wfopen(cap_path.c_str(), L"ab"));
        if (g_capture_file) {
            Log("[Translator] Opened optional JP capture log");
        }
    }

    std::FILE* out = g_capture_file.get();
    if (out == nullptr) {
        return;
    }

    std::string utf8;
    if (!translator_logic::Utf16ToUtf8(jp, utf8)) {
        return;
    }

    for (const unsigned char ch : utf8) {
        if (ch == '\\') {
            std::fputc('\\', out);
            std::fputc('\\', out);
            continue;
        }
        if (ch == '\r') {
            std::fputc('\\', out);
            std::fputc('r', out);
            continue;
        }
        if (ch == '\n') {
            std::fputc('\\', out);
            std::fputc('n', out);
            continue;
        }
        if (ch == '\t') {
            std::fputc('\\', out);
            std::fputc('t', out);
            continue;
        }
        std::fputc(ch, out);
    }
    std::fputc('\n', out);
    std::fflush(out);
}

void PostProcessFetchedSceneString(SiglusWstring* out) {
    const std::wstring_view current = ReadSiglusWstring(out);
    if (current.empty()) {
        return;
    }
    if (!translator_logic::ShouldBypassTranslation(current)) {
        MaybeCaptureJapaneseLine(current);
    }
    g_runtime.ApplyPostHook(
        current, [out](std::wstring_view replacement) {
            AssignSiglusWstring(out, replacement);
        });
}

SiglusWstring* __fastcall HookedSceneGetStr(void* scene_self,
                                            void* /*edx_unused*/,
                                            SiglusWstring* out,
                                            int scn_no,
                                            int str_no) {
    SiglusWstring* result =
        g_orig_get_str(scene_self, nullptr, out, scn_no, str_no);
    ScopedReentrancyGuard guard(g_in_scene_hook);
    if (guard.entered() && result != nullptr) {
        PostProcessFetchedSceneString(result);
    }
    return result;
}

SiglusWstring* __fastcall HookedSceneGetCurStr(void* scene_self,
                                               void* /*edx_unused*/,
                                               SiglusWstring* out,
                                               int str_no) {
    SiglusWstring* result =
        g_orig_get_cur_str(scene_self, nullptr, out, str_no);
    ScopedReentrancyGuard guard(g_in_scene_hook);
    if (guard.entered() && result != nullptr) {
        PostProcessFetchedSceneString(result);
    }
    return result;
}

}  // namespace

void TranslatorInit(HMODULE self_module) {
    g_exe_dir = QueryModuleDirectory(nullptr);
    g_capture_enabled = IsEnvFlagEnabled(kCaptureEnvVarName);

    std::string exe_dir_utf8;
    if (translator_logic::Utf16ToUtf8(g_exe_dir, exe_dir_utf8)) {
        Log("[Translator] Host executable directory: %s", exe_dir_utf8.c_str());
    }

    const std::wstring patch_dir = ResolveMenuPatchDirectory(self_module);
    if (!patch_dir.empty()) {
        const menu_autopatch::MenuPatchOutcome patch_outcome =
            menu_autopatch::EnsureMenuAssetsPatched(patch_dir.c_str());
        Log("[Translator] Menu auto-patch outcome: %s",
            menu_autopatch::MenuPatchOutcomeName(patch_outcome));
    }

    const std::wstring tsv_path = ResolveTranslationTablePath(self_module);
    if (!tsv_path.empty() &&
        g_runtime.LoadTsvFile(std::filesystem::path(tsv_path))) {
        std::string tsv_utf8;
        translator_logic::Utf16ToUtf8(tsv_path, tsv_utf8);
        Log("[Translator] Loaded %zu entries from %s", g_runtime.EntryCount(),
            tsv_utf8.c_str());
    } else {
        Log("[Translator] translation_table.tsv not found or empty (0 entries loaded)");
    }

    auto* base = reinterpret_cast<uint8_t*>(::GetModuleHandleW(nullptr));
    if (!VerifyHostIsSiglusEngine(base)) {
        Log("[Translator] Host process does not match SiglusEngine.exe prologues — skipping inline scene hooks");
        return;
    }

    g_trampoline_block = static_cast<uint8_t*>(::VirtualAlloc(
        nullptr, kTrampolineAllocBytes, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (g_trampoline_block == nullptr) {
        Log("[Translator] VirtualAlloc for scene trampolines failed (gle=%lu)",
            ::GetLastError());
        return;
    }

    g_wstring_assign =
        reinterpret_cast<WstringAssignRangeFn>(base + kWstringAssignRangeRva);

    uint8_t* get_str_target = base + kSceneGetStrRva;
    uint8_t* get_cur_str_target = base + kSceneGetCurStrRva;
    uint8_t* slot_get_str = g_trampoline_block;
    uint8_t* slot_get_cur_str = g_trampoline_block + kTrampolineSlotBytes;

    g_orig_get_str = reinterpret_cast<SceneGetStrFn>(slot_get_str);
    g_orig_get_cur_str = reinterpret_cast<SceneGetCurStrFn>(slot_get_cur_str);

    if (!BuildTrampolineAndPatch(get_str_target, slot_get_str,
                                 reinterpret_cast<const void*>(&HookedSceneGetStr))) {
        Log("[Translator] Failed to detour Scene::get_str at RVA 0x%X",
            kSceneGetStrRva);
        return;
    }
    g_patched_get_str = get_str_target;

    if (!BuildTrampolineAndPatch(
            get_cur_str_target, slot_get_cur_str,
            reinterpret_cast<const void*>(&HookedSceneGetCurStr))) {
        Log("[Translator] Failed to detour Scene::get_cur_str at RVA 0x%X",
            kSceneGetCurStrRva);
        return;
    }
    g_patched_get_cur_str = get_cur_str_target;
    g_hooks_installed = true;

    Log("[Translator] Installed UTF-16 scene hooks: get_str=0x%08X, get_cur_str=0x%08X, wstring_assign=0x%08X",
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(get_str_target)),
        static_cast<unsigned int>(
            reinterpret_cast<uintptr_t>(get_cur_str_target)),
        static_cast<unsigned int>(
            reinterpret_cast<uintptr_t>(base + kWstringAssignRangeRva)));
}

void TranslatorShutdown() {
    if (g_hooks_installed) {
        DWORD old_protect = 0u;
        if (g_patched_get_str != nullptr &&
            ::VirtualProtect(g_patched_get_str, kHookPatchBytes,
                             PAGE_EXECUTE_READWRITE, &old_protect)) {
            std::memcpy(g_patched_get_str, kExpectedSehPrologue,
                        kHookPatchBytes);
            DWORD dummy = 0u;
            ::VirtualProtect(g_patched_get_str, kHookPatchBytes, old_protect,
                             &dummy);
            ::FlushInstructionCache(::GetCurrentProcess(), g_patched_get_str,
                                    kHookPatchBytes);
        }
        if (g_patched_get_cur_str != nullptr &&
            ::VirtualProtect(g_patched_get_cur_str, kHookPatchBytes,
                             PAGE_EXECUTE_READWRITE, &old_protect)) {
            std::memcpy(g_patched_get_cur_str, kExpectedSehPrologue,
                        kHookPatchBytes);
            DWORD dummy = 0u;
            ::VirtualProtect(g_patched_get_cur_str, kHookPatchBytes,
                             old_protect, &dummy);
            ::FlushInstructionCache(::GetCurrentProcess(),
                                    g_patched_get_cur_str, kHookPatchBytes);
        }
        g_hooks_installed = false;
    }

    const translator_logic::RuntimeStats stats = g_runtime.Stats();
    Log("[Translator] Shutdown stats: total=%zu, translated=%zu, bypassed=%zu, misses=%zu",
        stats.total_calls, stats.translated_hits, stats.bypassed_calls,
        stats.lookup_misses);

    if (g_capture_enabled) {
        ScopedCriticalSection lock(GetCaptureLock());
        g_capture_file.reset();
    }
}
