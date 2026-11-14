// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "win32_chrome.h"

#include "chrome_text_logic.h"
#include "log.h"
#include "ui_text_lookup.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>

namespace win32_chrome {
namespace {

constexpr wchar_t kUser32ModuleName[] = L"user32.dll";
constexpr char kUser32ImportName[] = "user32.dll";

// Titles at least this long, or unreadable ones, pass through untouched.
constexpr std::size_t kMaxTitleChars = 4096;

// CatSystem2's narrow strings are Shift-JIS (CP932) regardless of the
// system ANSI code page.
constexpr UINT kShiftJisCodePage = 932;

constexpr int kMaxDetourLogLines = 32;
constexpr int kSingleDetourHit = 1;
constexpr std::size_t kNullTerminatorBytes = 1;

constexpr DWORD kImportDescriptorBytes = sizeof(IMAGE_IMPORT_DESCRIPTOR);
constexpr DWORD kThunkBytes = sizeof(IMAGE_THUNK_DATA);
constexpr DWORD kImportByNameBytes = sizeof(IMAGE_IMPORT_BY_NAME);

constexpr DWORD kExecutableProtectionMask =
    PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
    PAGE_EXECUTE_WRITECOPY;

using SetWindowTextWFn = BOOL(WINAPI*)(HWND, LPCWSTR);
using SetWindowTextAFn = BOOL(WINAPI*)(HWND, LPCSTR);
using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
using MessageBoxWFn = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
using MessageBoxAFn = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
using FindWindowWFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
using FindWindowAFn = HWND(WINAPI*)(LPCSTR, LPCSTR);

// Function each detour forwards to. Written once, before the first import
// slot is routed to the detour, and never changed afterwards.
std::atomic<void*> g_original_set_window_text_w{nullptr};
std::atomic<void*> g_original_set_window_text_a{nullptr};
std::atomic<void*> g_original_create_window_ex_w{nullptr};
std::atomic<void*> g_original_create_window_ex_a{nullptr};
std::atomic<void*> g_original_message_box_w{nullptr};
std::atomic<void*> g_original_message_box_a{nullptr};
std::atomic<void*> g_original_find_window_w{nullptr};
std::atomic<void*> g_original_find_window_a{nullptr};

std::atomic<int> g_detour_log_lines{0};

SRWLOCK g_install_lock = SRWLOCK_INIT;

class ScopedExclusiveLock {
public:
    explicit ScopedExclusiveLock(SRWLOCK* lock) noexcept : lock_(lock) {
        AcquireSRWLockExclusive(lock_);
    }
    ~ScopedExclusiveLock() { ReleaseSRWLockExclusive(lock_); }

    ScopedExclusiveLock(const ScopedExclusiveLock&) = delete;
    ScopedExclusiveLock& operator=(const ScopedExclusiveLock&) = delete;

private:
    SRWLOCK* lock_;
};

// Makes [address, address + size) writable for its lifetime and restores the
// original page protection on destruction. Pages that were executable stay
// executable so concurrently running code on them never faults.
class ScopedWritableMemory {
public:
    ScopedWritableMemory(void* address, SIZE_T size) noexcept
        : address_(address), size_(size) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address_, &info, sizeof(info)) == 0) {
            return;
        }
        const DWORD writable_protect =
            (info.Protect & kExecutableProtectionMask) != 0
                ? PAGE_EXECUTE_READWRITE
                : PAGE_READWRITE;
        writable_ = VirtualProtect(address_, size_, writable_protect,
                                   &original_protect_) != FALSE;
    }

    ~ScopedWritableMemory() {
        if (!writable_) {
            return;
        }
        DWORD replaced_protect = 0;
        VirtualProtect(address_, size_, original_protect_, &replaced_protect);
    }

    ScopedWritableMemory(const ScopedWritableMemory&) = delete;
    ScopedWritableMemory& operator=(const ScopedWritableMemory&) = delete;

    bool writable() const noexcept { return writable_; }

private:
    void* address_;
    SIZE_T size_;
    DWORD original_protect_ = 0;
    bool writable_ = false;
};

template <class Fn>
Fn LoadOriginal(const std::atomic<void*>& original) noexcept {
    return reinterpret_cast<Fn>(original.load(std::memory_order_acquire));
}

bool ShouldLogDetour() noexcept {
    return g_detour_log_lines.fetch_add(kSingleDetourHit,
                                        std::memory_order_relaxed) <
           kMaxDetourLogLines;
}

void LogTitleSwap(const char* site, const std::wstring& english) noexcept {
    if (!ShouldLogDetour()) {
        return;
    }
    Log("CHROME HOOK: %s -> \"%ls\"", site, english.c_str());
}

// Length of the NUL-terminated `text`, or kMaxTitleChars when `text` is
// unreadable or not terminated within kMaxTitleChars characters.
std::size_t ProbeWideLength(const wchar_t* text) noexcept {
    __try {
        return wcsnlen(text, kMaxTitleChars);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kMaxTitleChars;
    }
}

// Narrow counterpart of ProbeWideLength.
std::size_t ProbeNarrowLength(const char* text) noexcept {
    __try {
        return strnlen(text, kMaxTitleChars);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kMaxTitleChars;
    }
}

std::optional<std::wstring> WideFromCodePage(UINT code_page,
                                             const char* text,
                                             std::size_t length) noexcept {
    if (text == nullptr || length == 0 || length >= kMaxTitleChars) {
        return std::nullopt;
    }
    const int narrow_length = static_cast<int>(length);
    const int wide_length = MultiByteToWideChar(code_page, 0, text,
                                                narrow_length, nullptr, 0);
    if (wide_length <= 0) {
        return std::nullopt;
    }
    try {
        std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
        const int written = MultiByteToWideChar(
            code_page, 0, text, narrow_length, wide.data(), wide_length);
        if (written != wide_length) {
            return std::nullopt;
        }
        return wide;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> NarrowToCodePage(UINT code_page,
                                            std::wstring_view wide) noexcept {
    if (wide.empty() || wide.size() >= kMaxTitleChars) {
        return std::nullopt;
    }
    const int wide_length = static_cast<int>(wide.size());
    const int narrow_length = WideCharToMultiByte(
        code_page, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr);
    if (narrow_length <= 0) {
        return std::nullopt;
    }
    try {
        std::string narrow(static_cast<std::size_t>(narrow_length), '\0');
        const int written = WideCharToMultiByte(code_page, 0, wide.data(),
                                                wide_length, narrow.data(),
                                                narrow_length, nullptr, nullptr);
        if (written != narrow_length) {
            return std::nullopt;
        }
        return narrow;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::wstring> TranslateWideTitle(const wchar_t* text) noexcept {
    if (text == nullptr) {
        return std::nullopt;
    }
    const std::size_t length = ProbeWideLength(text);
    if (length >= kMaxTitleChars) {
        return std::nullopt;
    }
    try {
        return chrome_text::TranslateWindowTitle(std::wstring_view(text, length));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::wstring> TranslateShiftJisTitle(const char* text) noexcept {
    if (text == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::wstring> wide =
        WideFromCodePage(kShiftJisCodePage, text, ProbeNarrowLength(text));
    if (!wide) {
        return std::nullopt;
    }
    try {
        return chrome_text::TranslateWindowTitle(*wide);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::wstring> TranslateWideDialogText(
    const wchar_t* text) noexcept {
    if (text == nullptr) {
        return std::nullopt;
    }
    const std::size_t length = ProbeWideLength(text);
    if (length == 0 || length >= kMaxTitleChars) {
        return std::nullopt;
    }
    const std::wstring_view view(text, length);
    try {
        if (const auto title = chrome_text::TranslateWindowTitle(view);
            title.has_value()) {
            return title;
        }
        const std::optional<std::string> utf8 = NarrowToCodePage(CP_UTF8, view);
        if (!utf8) {
            return std::nullopt;
        }
        const std::optional<std::string> ui_en =
            translator_logic::LookupStatelessUiText(*utf8);
        if (!ui_en) {
            return std::nullopt;
        }
        return WideFromCodePage(CP_UTF8, ui_en->data(), ui_en->size());
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> TranslateShiftJisDialogText(
    const char* text) noexcept {
    if (text == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::wstring> wide =
        WideFromCodePage(kShiftJisCodePage, text, ProbeNarrowLength(text));
    if (!wide) {
        return std::nullopt;
    }
    const std::optional<std::wstring> translated =
        TranslateWideDialogText(wide->c_str());
    if (!translated) {
        return std::nullopt;
    }
    return NarrowToCodePage(kShiftJisCodePage, *translated);
}

bool IsTopLevelStyle(DWORD style, HWND parent) noexcept {
    return parent == nullptr && (style & WS_CHILD) == 0;
}

bool IsTopLevelWindow(HWND window) noexcept {
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }
    const auto style = static_cast<DWORD>(
        static_cast<ULONG_PTR>(GetWindowLongPtrW(window, GWL_STYLE)));
    return (style & WS_CHILD) == 0;
}

BOOL WINAPI DetourSetWindowTextW(HWND window, LPCWSTR text) noexcept {
    const auto original =
        LoadOriginal<SetWindowTextWFn>(g_original_set_window_text_w);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    if (!IsTopLevelWindow(window)) {
        return original(window, text);
    }
    const std::optional<std::wstring> english = TranslateWideTitle(text);
    if (!english) {
        return original(window, text);
    }
    LogTitleSwap("SetWindowTextW", *english);
    return original(window, english->c_str());
}

BOOL WINAPI DetourSetWindowTextA(HWND window, LPCSTR text) noexcept {
    const auto original =
        LoadOriginal<SetWindowTextAFn>(g_original_set_window_text_a);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    const auto wide_original =
        LoadOriginal<SetWindowTextWFn>(g_original_set_window_text_w);
    if (wide_original == nullptr || !IsTopLevelWindow(window)) {
        return original(window, text);
    }
    const std::optional<std::wstring> english = TranslateShiftJisTitle(text);
    if (!english) {
        return original(window, text);
    }
    LogTitleSwap("SetWindowTextA", *english);
    return wide_original(window, english->c_str());
}

HWND WINAPI DetourCreateWindowExW(DWORD ex_style,
                                  LPCWSTR class_name,
                                  LPCWSTR window_name,
                                  DWORD style,
                                  int x,
                                  int y,
                                  int width,
                                  int height,
                                  HWND parent,
                                  HMENU menu,
                                  HINSTANCE instance,
                                  LPVOID create_param) noexcept {
    const auto original =
        LoadOriginal<CreateWindowExWFn>(g_original_create_window_ex_w);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    const std::optional<std::wstring> english =
        IsTopLevelStyle(style, parent) ? TranslateWideTitle(window_name)
                                       : std::nullopt;
    if (!english) {
        return original(ex_style, class_name, window_name, style, x, y, width,
                        height, parent, menu, instance, create_param);
    }
    LogTitleSwap("CreateWindowExW", *english);
    return original(ex_style, class_name, english->c_str(), style, x, y, width,
                    height, parent, menu, instance, create_param);
}

HWND WINAPI DetourCreateWindowExA(DWORD ex_style,
                                  LPCSTR class_name,
                                  LPCSTR window_name,
                                  DWORD style,
                                  int x,
                                  int y,
                                  int width,
                                  int height,
                                  HWND parent,
                                  HMENU menu,
                                  HINSTANCE instance,
                                  LPVOID create_param) noexcept {
    const auto original =
        LoadOriginal<CreateWindowExAFn>(g_original_create_window_ex_a);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    const std::optional<std::wstring> english_wide =
        IsTopLevelStyle(style, parent) ? TranslateShiftJisTitle(window_name)
                                       : std::nullopt;
    const std::optional<std::string> english_narrow =
        english_wide ? NarrowToCodePage(kShiftJisCodePage, *english_wide)
                     : std::nullopt;
    if (!english_narrow) {
        return original(ex_style, class_name, window_name, style, x, y, width,
                        height, parent, menu, instance, create_param);
    }
    LogTitleSwap("CreateWindowExA", *english_wide);
    return original(ex_style, class_name, english_narrow->c_str(), style, x, y,
                    width, height, parent, menu, instance, create_param);
}

int WINAPI DetourMessageBoxW(HWND owner,
                             LPCWSTR text,
                             LPCWSTR caption,
                             UINT type) noexcept {
    const auto original =
        LoadOriginal<MessageBoxWFn>(g_original_message_box_w);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
    const std::optional<std::wstring> english_text =
        TranslateWideDialogText(text);
    const std::optional<std::wstring> english_caption =
        TranslateWideDialogText(caption);
    if (english_caption) {
        LogTitleSwap("MessageBoxW.caption", *english_caption);
    }
    if (english_text) {
        LogTitleSwap("MessageBoxW.text", *english_text);
    }
    return original(owner,
                    english_text ? english_text->c_str() : text,
                    english_caption ? english_caption->c_str() : caption,
                    type);
}

int WINAPI DetourMessageBoxA(HWND owner,
                             LPCSTR text,
                             LPCSTR caption,
                             UINT type) noexcept {
    const auto original =
        LoadOriginal<MessageBoxAFn>(g_original_message_box_a);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
    const std::optional<std::string> english_text =
        TranslateShiftJisDialogText(text);
    const std::optional<std::string> english_caption =
        TranslateShiftJisDialogText(caption);
    return original(owner,
                    english_text ? english_text->c_str() : text,
                    english_caption ? english_caption->c_str() : caption,
                    type);
}

HWND WINAPI DetourFindWindowW(LPCWSTR class_name, LPCWSTR window_name) noexcept {
    const auto original = LoadOriginal<FindWindowWFn>(g_original_find_window_w);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    const std::optional<std::wstring> english = TranslateWideTitle(window_name);
    if (!english) {
        return original(class_name, window_name);
    }
    const HWND found = original(class_name, english->c_str());
    if (found == nullptr) {
        return original(class_name, window_name);
    }
    LogTitleSwap("FindWindowW", *english);
    return found;
}

HWND WINAPI DetourFindWindowA(LPCSTR class_name, LPCSTR window_name) noexcept {
    const auto original = LoadOriginal<FindWindowAFn>(g_original_find_window_a);
    if (original == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    const std::optional<std::wstring> english_wide =
        TranslateShiftJisTitle(window_name);
    const std::optional<std::string> english_narrow =
        english_wide ? NarrowToCodePage(kShiftJisCodePage, *english_wide)
                     : std::nullopt;
    if (!english_narrow) {
        return original(class_name, window_name);
    }
    const HWND found = original(class_name, english_narrow->c_str());
    if (found == nullptr) {
        return original(class_name, window_name);
    }
    LogTitleSwap("FindWindowA", *english_wide);
    return found;
}

struct ImportDetour {
    const char* import_name;
    void* detour;
    std::atomic<void*>* original;
    void* export_address = nullptr;
    std::size_t routed_slots = 0;
};

auto MakeChromeImportDetours(HMODULE user32) noexcept {
    std::array detours{
        ImportDetour{"SetWindowTextW",
                     reinterpret_cast<void*>(&DetourSetWindowTextW),
                     &g_original_set_window_text_w},
        ImportDetour{"SetWindowTextA",
                     reinterpret_cast<void*>(&DetourSetWindowTextA),
                     &g_original_set_window_text_a},
        ImportDetour{"CreateWindowExW",
                     reinterpret_cast<void*>(&DetourCreateWindowExW),
                     &g_original_create_window_ex_w},
        ImportDetour{"CreateWindowExA",
                     reinterpret_cast<void*>(&DetourCreateWindowExA),
                     &g_original_create_window_ex_a},
        ImportDetour{"MessageBoxW",
                     reinterpret_cast<void*>(&DetourMessageBoxW),
                     &g_original_message_box_w},
        ImportDetour{"MessageBoxA",
                     reinterpret_cast<void*>(&DetourMessageBoxA),
                     &g_original_message_box_a},
        ImportDetour{"FindWindowW",
                     reinterpret_cast<void*>(&DetourFindWindowW),
                     &g_original_find_window_w},
        ImportDetour{"FindWindowA",
                     reinterpret_cast<void*>(&DetourFindWindowA),
                     &g_original_find_window_a},
    };
    for (ImportDetour& detour : detours) {
        detour.export_address =
            reinterpret_cast<void*>(GetProcAddress(user32, detour.import_name));
    }
    return detours;
}

bool IsLoadedImageBase(const void* base) noexcept {
    if (base == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(base, &info, sizeof(info)) == 0) {
        return false;
    }
    return info.Type == MEM_IMAGE && info.AllocationBase == base;
}

bool IsRvaRangeInImage(DWORD rva, DWORD size, DWORD image_size) noexcept {
    return rva <= image_size && size <= image_size - rva;
}

bool IndexedThunkRva(DWORD table_rva,
                     DWORD index,
                     DWORD image_size,
                     DWORD& entry_rva_out) noexcept {
    if (!IsRvaRangeInImage(table_rva, kThunkBytes, image_size)) {
        return false;
    }
    const DWORD max_index = (image_size - table_rva - kThunkBytes) / kThunkBytes;
    if (index > max_index) {
        return false;
    }
    entry_rva_out = table_rva + index * kThunkBytes;
    return true;
}

const IMAGE_NT_HEADERS* FindNtHeaders(const BYTE* base) noexcept {
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        return nullptr;
    }
    const DWORD header_end =
        static_cast<DWORD>(dos->e_lfanew) +
        static_cast<DWORD>(sizeof(IMAGE_NT_HEADERS));
    if (header_end < static_cast<DWORD>(sizeof(IMAGE_NT_HEADERS))) {
        return nullptr;
    }
    const auto* const nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.SizeOfImage < header_end ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return nullptr;
    }
    return nt;
}

bool IsUser32Descriptor(const BYTE* base,
                        const IMAGE_IMPORT_DESCRIPTOR& descriptor,
                        DWORD image_size) noexcept {
    constexpr std::size_t kNameChars =
        sizeof(kUser32ImportName) - kNullTerminatorBytes;
    if (!IsRvaRangeInImage(descriptor.Name, sizeof(kUser32ImportName),
                           image_size)) {
        return false;
    }
    const auto* const dll_name =
        reinterpret_cast<const char*>(base + descriptor.Name);
    return _strnicmp(dll_name, kUser32ImportName, kNameChars) == 0 &&
           dll_name[kNameChars] == '\0';
}

// Name of import `index` of a descriptor, or empty view for ordinal imports
// and images bound without an import name table.
std::string_view ImportNameAt(const BYTE* base,
                              DWORD name_table_rva,
                              DWORD index,
                              DWORD image_size) noexcept {
    if (name_table_rva == 0) {
        return {};
    }
    DWORD entry_rva = 0;
    if (!IndexedThunkRva(name_table_rva, index, image_size, entry_rva)) {
        return {};
    }
    const auto* const entry =
        reinterpret_cast<const IMAGE_THUNK_DATA*>(base + entry_rva);
    if (IMAGE_SNAP_BY_ORDINAL(entry->u1.Ordinal)) {
        return {};
    }
    const auto by_name_rva = static_cast<DWORD>(entry->u1.AddressOfData);
    if (!IsRvaRangeInImage(by_name_rva, kImportByNameBytes, image_size)) {
        return {};
    }
    const DWORD name_rva =
        by_name_rva + static_cast<DWORD>(offsetof(IMAGE_IMPORT_BY_NAME, Name));
    const auto* const raw_name = reinterpret_cast<const char*>(base + name_rva);
    const std::size_t max_chars = image_size - name_rva;
    const std::size_t name_len = strnlen(raw_name, max_chars);
    if (name_len == 0 || name_len == max_chars) {
        return {};
    }
    return std::string_view(raw_name, name_len);
}

// Calls visit(import_name, slot) for every import address table slot the image
// binds to user32.dll. Returns false when the image has no readable import
// directory.
template <class SlotVisitor>
bool ForEachUser32ImportSlot(BYTE* base, SlotVisitor&& visit) noexcept {
    const IMAGE_NT_HEADERS* const nt = FindNtHeaders(base);
    if (nt == nullptr) {
        return false;
    }
    const DWORD image_size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return false;
    }
    for (DWORD descriptor_rva = imports.VirtualAddress;
         IsRvaRangeInImage(descriptor_rva, kImportDescriptorBytes, image_size);
         descriptor_rva += kImportDescriptorBytes) {
        const auto* const descriptor =
            reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + descriptor_rva);
        if (descriptor->Name == 0) {
            break;
        }
        if (!IsUser32Descriptor(base, *descriptor, image_size)) {
            continue;
        }
        for (DWORD index = 0;; ++index) {
            DWORD slot_rva = 0;
            if (!IndexedThunkRva(descriptor->FirstThunk, index, image_size,
                                 slot_rva)) {
                break;
            }
            auto* const slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + slot_rva);
            if (slot->u1.Function == 0) {
                break;
            }
            visit(ImportNameAt(base, descriptor->OriginalFirstThunk, index,
                               image_size),
                  slot);
        }
    }
    return true;
}

bool SlotMatches(std::string_view import_name,
                 const IMAGE_THUNK_DATA* slot,
                 const ImportDetour& detour) noexcept {
    if (!import_name.empty()) {
        return import_name == detour.import_name;
    }
    const auto* const target = reinterpret_cast<const void*>(slot->u1.Function);
    return target == detour.detour ||
           (detour.export_address != nullptr && target == detour.export_address);
}

// REQUIRES: g_install_lock is held exclusively.
bool RouteSlot(IMAGE_THUNK_DATA* slot, ImportDetour& detour) noexcept {
    void* const target = reinterpret_cast<void*>(slot->u1.Function);
    if (target == detour.detour) {
        return true;
    }
    void* no_original = nullptr;
    detour.original->compare_exchange_strong(no_original, target,
                                             std::memory_order_acq_rel);

    ScopedWritableMemory writable(&slot->u1.Function, sizeof(slot->u1.Function));
    if (!writable.writable()) {
        Log("CHROME HOOK: %s slot 0x%p VirtualProtect failed (gle=%lu)",
            detour.import_name, static_cast<void*>(slot), GetLastError());
        return false;
    }
    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&slot->u1.Function), detour.detour);
    return true;
}

// Detours whose import the module does not bind forward to the export itself,
// which lets DetourSetWindowTextA reach SetWindowTextW in every image.
// REQUIRES: g_install_lock is held exclusively.
template <class ImportDetourTable>
void FillMissingOriginals(const ImportDetourTable& detours) noexcept {
    for (const ImportDetour& detour : detours) {
        void* no_original = nullptr;
        detour.original->compare_exchange_strong(no_original, detour.export_address,
                                                 std::memory_order_acq_rel);
    }
}

}  // namespace

bool InstallWindowChromeHooks(void* host_module_base) noexcept {
    if (!IsLoadedImageBase(host_module_base)) {
        Log("CHROME HOOK: 0x%p is not a loaded module base", host_module_base);
        return false;
    }
    const HMODULE user32 = GetModuleHandleW(kUser32ModuleName);
    if (user32 == nullptr) {
        Log("CHROME HOOK: user32.dll is not loaded");
        return false;
    }

    ScopedExclusiveLock lock(&g_install_lock);
    auto detours = MakeChromeImportDetours(user32);
    auto* const base = static_cast<BYTE*>(host_module_base);
    const bool walked = ForEachUser32ImportSlot(
        base, [&detours](std::string_view import_name, IMAGE_THUNK_DATA* slot) {
            for (ImportDetour& detour : detours) {
                if (!SlotMatches(import_name, slot, detour)) {
                    continue;
                }
                if (RouteSlot(slot, detour)) {
                    ++detour.routed_slots;
                }
                return;
            }
        });
    if (!walked) {
        Log("CHROME HOOK: module 0x%p has no readable import table",
            host_module_base);
        return false;
    }
    FillMissingOriginals(detours);

    bool any_routed = false;
    for (const ImportDetour& detour : detours) {
        any_routed = any_routed || detour.routed_slots != 0;
        Log("CHROME HOOK: %s %s (%zu slot(s)) in module 0x%p",
            detour.import_name,
            detour.routed_slots != 0 ? "routed" : "not imported",
            detour.routed_slots, host_module_base);
    }
    return any_routed;
}

}  // namespace win32_chrome
