// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "win32_chrome.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

// "アマカノ2～Perfect Edition～" (U+FF5E FULLWIDTH TILDE), as config\startup.xml spells it.
constexpr std::wstring_view kJapaneseTitle =
    L"アマカノ2\uFF5EPerfect Edition\uFF5E";
constexpr std::wstring_view kEnglishTitle = L"Amakano 2 ~Perfect Edition~";
constexpr std::wstring_view kTitleSuffix = L" - Chapter 1";
constexpr wchar_t kUnrelatedTitle[] = L"win32_chrome_ut unrelated window";
constexpr wchar_t kPlaceholderTitle[] = L"win32_chrome_ut placeholder";

constexpr wchar_t kStaticWindowClass[] = L"Static";
constexpr UINT kShiftJisCodePage = 932;
constexpr std::size_t kTerminatingNullChars = 1;
constexpr int kWindowOrigin = 0;
constexpr int kWindowWidth = 320;
constexpr int kWindowHeight = 240;

class ScopedWindow {
public:
    explicit ScopedWindow(HWND window) noexcept : window_(window) {}
    ~ScopedWindow() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
    }

    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;

    HWND get() const noexcept { return window_; }

private:
    HWND window_;
};

HWND CreateHiddenWindow(std::wstring_view title, DWORD style, HWND parent) {
    const std::wstring terminated(title);
    return CreateWindowExW(0, kStaticWindowClass, terminated.c_str(), style,
                           kWindowOrigin, kWindowOrigin, kWindowWidth,
                           kWindowHeight, parent, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

HWND CreateHiddenTopLevelWindow(std::wstring_view title) {
    return CreateHiddenWindow(title, WS_OVERLAPPED, nullptr);
}

std::wstring ReadWindowTitle(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return std::wstring();
    }
    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(length) + kTerminatingNullChars, L'\0');
    const int copied =
        GetWindowTextW(window, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data(), static_cast<std::size_t>(copied));
}

std::string ToShiftJis(std::wstring_view wide) {
    const int wide_length = static_cast<int>(wide.size());
    const int needed = WideCharToMultiByte(kShiftJisCodePage, 0, wide.data(),
                                           wide_length, nullptr, 0, nullptr,
                                           nullptr);
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(kShiftJisCodePage, 0, wide.data(), wide_length,
                        narrow.data(), needed, nullptr, nullptr);
    return narrow;
}

std::wstring Concat(std::wstring_view head, std::wstring_view tail) {
    std::wstring joined(head);
    joined.append(tail);
    return joined;
}

class WindowChromeHooksTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(win32_chrome::InstallWindowChromeHooks(
            GetModuleHandleW(nullptr)));
    }
};

TEST(WindowChromeInstallTest,
     RejectsInvalidModuleBasesAndInstallsIdempotently) {
    EXPECT_FALSE(win32_chrome::InstallWindowChromeHooks(nullptr));
    int not_a_module = 0;
    EXPECT_FALSE(win32_chrome::InstallWindowChromeHooks(&not_a_module));

    ASSERT_TRUE(
        win32_chrome::InstallWindowChromeHooks(GetModuleHandleW(nullptr)));
    ASSERT_TRUE(
        win32_chrome::InstallWindowChromeHooks(GetModuleHandleW(nullptr)));
}

TEST_F(
    WindowChromeHooksTest,
    TopLevelWindowCreationAndTitleUpdatesTranslateJapaneseWhilePreservingChildAndPopupTitles) {
    const ScopedWindow top(CreateHiddenTopLevelWindow(kJapaneseTitle));
    ASSERT_NE(top.get(), nullptr);
    EXPECT_EQ(ReadWindowTitle(top.get()), kEnglishTitle);

    const ScopedWindow child(
        CreateHiddenWindow(kJapaneseTitle, WS_CHILD, top.get()));
    ASSERT_NE(child.get(), nullptr);
    EXPECT_EQ(ReadWindowTitle(child.get()), kJapaneseTitle);

    const ScopedWindow popup(
        CreateHiddenWindow(kJapaneseTitle, WS_POPUP, top.get()));
    ASSERT_NE(popup.get(), nullptr);
    EXPECT_EQ(ReadWindowTitle(popup.get()), kJapaneseTitle);

    const std::wstring jp_with_suffix = Concat(kJapaneseTitle, kTitleSuffix);
    const std::wstring en_with_suffix = Concat(kEnglishTitle, kTitleSuffix);
    ASSERT_TRUE(SetWindowTextW(top.get(), jp_with_suffix.c_str()));
    EXPECT_EQ(ReadWindowTitle(top.get()), en_with_suffix);

    ASSERT_TRUE(SetWindowTextW(child.get(), jp_with_suffix.c_str()));
    EXPECT_EQ(ReadWindowTitle(child.get()), jp_with_suffix);

    ASSERT_TRUE(SetWindowTextW(top.get(), kUnrelatedTitle));
    EXPECT_EQ(ReadWindowTitle(top.get()), kUnrelatedTitle);

    const std::string shift_jis = ToShiftJis(jp_with_suffix);
    ASSERT_FALSE(shift_jis.empty());
    ASSERT_TRUE(SetWindowTextA(top.get(), shift_jis.c_str()));
    EXPECT_EQ(ReadWindowTitle(top.get()), en_with_suffix);

    const std::string shift_jis_base = ToShiftJis(kJapaneseTitle);
    ASSERT_FALSE(shift_jis_base.empty());
    const ScopedWindow top_narrow(CreateWindowExA(
        0, "Static", shift_jis_base.c_str(), WS_OVERLAPPED, kWindowOrigin,
        kWindowOrigin, kWindowWidth, kWindowHeight, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr));
    ASSERT_NE(top_narrow.get(), nullptr);
    EXPECT_EQ(ReadWindowTitle(top_narrow.get()), kEnglishTitle);

    ASSERT_TRUE(SetWindowTextW(top.get(), nullptr));
    EXPECT_EQ(ReadWindowTitle(top.get()), std::wstring());
}

TEST_F(WindowChromeHooksTest,
       FindWindowWResolvesTranslatedTopLevelWindowFromJapaneseQuery) {
    const ScopedWindow top(CreateHiddenTopLevelWindow(kJapaneseTitle));
    ASSERT_NE(top.get(), nullptr);

    const std::wstring japanese(kJapaneseTitle);
    EXPECT_EQ(FindWindowW(kStaticWindowClass, japanese.c_str()), top.get());

    const std::string japanese_shift_jis = ToShiftJis(kJapaneseTitle);
    EXPECT_EQ(FindWindowA("Static", japanese_shift_jis.c_str()), top.get());

    const ScopedWindow unrelated(CreateHiddenTopLevelWindow(kUnrelatedTitle));
    ASSERT_NE(unrelated.get(), nullptr);
    EXPECT_EQ(FindWindowW(kStaticWindowClass, kUnrelatedTitle), unrelated.get());
}

}  // namespace
