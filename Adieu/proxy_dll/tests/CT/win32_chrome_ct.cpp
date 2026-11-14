// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests for win32_chrome IAT hooks + native Win32 dialog/tab/column
// localization, exercising real USER32/COMCTL32 windows and SiglusEngine's
// C_window_base::attach_window DWLP_DLGPROC subclassing contract.

#include "win32_chrome.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kIdResetAllButton = 1001;
constexpr int kIdCloseButton = 1010;
constexpr int kIdTabControl = 2001;
constexpr int kIdListView = 2002;

constexpr int kIdChildResetButton = 3001;
constexpr int kIdChildBoldCheck = 3002;
constexpr int kIdChildPlayButton = 3003;
constexpr int kIdChildSystemGroup = 3004;
constexpr int kIdChildDurationStatic = 3005;
constexpr int kIdChildSameRowRightRadio = 3006;

constexpr UINT kMsgInspectAndCloseModal = WM_APP + 17u;
constexpr std::size_t kTextReadBufChars = 256u;
constexpr int kMaxRecursionGuardDepth = 64;
constexpr int kMinWidenedCheckboxWidthPx = 80;

void EnsureCommonControlsInitialized() {
    static const bool kInitialized = []() {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
        return ::InitCommonControlsEx(&icc) != FALSE;
    }();
    (void)kInitialized;
}

class InMemoryDlgTemplateBuilder {
public:
    InMemoryDlgTemplateBuilder(DWORD style,
                               short x,
                               short y,
                               short cx,
                               short cy,
                               std::wstring_view title) {
        DLGTEMPLATE header{};
        header.style = style;
        header.dwExtendedStyle = 0u;
        header.cdit = 0u;
        header.x = x;
        header.y = y;
        header.cx = cx;
        header.cy = cy;

        AppendBytes(&header, sizeof(header));
        AppendWord(0u);  // No menu
        AppendWord(0u);  // Default dialog class
        AppendWideString(title);
    }

    void AddControl(WORD atom_class,
                    DWORD style,
                    short x,
                    short y,
                    short cx,
                    short cy,
                    WORD id,
                    std::wstring_view text) {
        AlignDword();

        DLGITEMTEMPLATE item{};
        item.style = WS_CHILD | WS_VISIBLE | style;
        item.dwExtendedStyle = 0u;
        item.x = x;
        item.y = y;
        item.cx = cx;
        item.cy = cy;
        item.id = id;

        AppendBytes(&item, sizeof(item));
        AppendWord(0xFFFFu);
        AppendWord(atom_class);
        AppendWideString(text);
        AppendWord(0u);  // Zero creation data

        auto* header = reinterpret_cast<DLGTEMPLATE*>(bytes_.data());
        ++header->cdit;
    }

    LPCDLGTEMPLATEW Data() const noexcept {
        return reinterpret_cast<LPCDLGTEMPLATEW>(bytes_.data());
    }

private:
    void AlignDword() {
        while ((bytes_.size() % sizeof(DWORD)) != 0u) {
            bytes_.push_back(0u);
        }
    }

    void AppendBytes(const void* src, std::size_t len) {
        const auto* raw = static_cast<const uint8_t*>(src);
        bytes_.insert(bytes_.end(), raw, raw + len);
    }

    void AppendWord(WORD value) { AppendBytes(&value, sizeof(value)); }

    void AppendWideString(std::wstring_view str) {
        for (const wchar_t ch : str) {
            AppendWord(static_cast<WORD>(ch));
        }
        AppendWord(0u);
    }

    std::vector<uint8_t> bytes_;
};

std::wstring ReadControlText(HWND hwnd) {
    wchar_t buf[kTextReadBufChars] = {};
    const int copied =
        ::GetWindowTextW(hwnd, buf, static_cast<int>(kTextReadBufChars));
    if (copied <= 0) {
        return {};
    }
    return std::wstring(buf, static_cast<std::size_t>(copied));
}

std::wstring ReadTabItemText(HWND tab_hwnd, int index) {
    wchar_t buf[kTextReadBufChars] = {};
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = buf;
    item.cchTextMax = static_cast<int>(kTextReadBufChars);
    if (!::SendMessageW(tab_hwnd, TCM_GETITEMW, static_cast<WPARAM>(index),
                        reinterpret_cast<LPARAM>(&item))) {
        return {};
    }
    return std::wstring(buf);
}

std::wstring ReadListViewColumnText(HWND lv_hwnd, int col) {
    wchar_t buf[kTextReadBufChars] = {};
    LVCOLUMNW lvcol{};
    lvcol.mask = LVCF_TEXT;
    lvcol.pszText = buf;
    lvcol.cchTextMax = static_cast<int>(kTextReadBufChars);
    if (!::SendMessageW(lv_hwnd, LVM_GETCOLUMNW, static_cast<WPARAM>(col),
                        reinterpret_cast<LPARAM>(&lvcol))) {
        return {};
    }
    return std::wstring(buf);
}

void InsertTabItem(HWND tab_hwnd, int index, std::wstring_view text) {
    std::wstring mutable_text(text);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = mutable_text.data();
    ::SendMessageW(tab_hwnd, TCM_INSERTITEMW, static_cast<WPARAM>(index),
                   reinterpret_cast<LPARAM>(&item));
}

void InsertListViewColumn(HWND lv_hwnd, int col, std::wstring_view text) {
    std::wstring mutable_text(text);
    LVCOLUMNW lvcol{};
    lvcol.mask = LVCF_TEXT | LVCF_WIDTH;
    lvcol.cx = 80;
    lvcol.pszText = mutable_text.data();
    ::SendMessageW(lv_hwnd, LVM_INSERTCOLUMNW, static_cast<WPARAM>(col),
                   reinterpret_cast<LPARAM>(&lvcol));
}

struct ChildTabPageContext {
    WNDPROC subclassed_old_proc = nullptr;
    int dispatch_depth = 0;
    bool recursion_overflow_detected = false;
};

INT_PTR CALLBACK TonyuChildTabDlgProc(HWND hdlg,
                                      UINT msg,
                                      WPARAM wparam,
                                      LPARAM lparam) {
    auto* ctx = reinterpret_cast<ChildTabPageContext*>(
        ::GetWindowLongPtrW(hdlg, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        ctx = reinterpret_cast<ChildTabPageContext*>(lparam);
        ::SetWindowLongPtrW(hdlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));

        const LONG_PTR current_dlg_proc =
            ::GetWindowLongPtrW(hdlg, DWLP_DLGPROC);
        if (current_dlg_proc !=
            reinterpret_cast<LONG_PTR>(&TonyuChildTabDlgProc)) {
            ctx->subclassed_old_proc = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(
                    hdlg, DWLP_DLGPROC,
                    reinterpret_cast<LONG_PTR>(&TonyuChildTabDlgProc)));
        }
        return TRUE;
    }

    if (ctx != nullptr && ctx->subclassed_old_proc != nullptr) {
        if (ctx->dispatch_depth >= kMaxRecursionGuardDepth) {
            ctx->recursion_overflow_detected = true;
            return FALSE;
        }
        ++ctx->dispatch_depth;
        const LRESULT forwarded = ::CallWindowProcW(
            ctx->subclassed_old_proc, hdlg, msg, wparam, lparam);
        --ctx->dispatch_depth;
        return forwarded;
    }
    return FALSE;
}

struct OptionDialogContext {
    WNDPROC subclassed_old_proc = nullptr;
    int dispatch_depth = 0;
    int notify_format_messages = 0;
    bool recursion_overflow_detected = false;
    HWND tab_hwnd = nullptr;
    HWND listview_hwnd = nullptr;
    HWND child_font_page = nullptr;
    HWND child_volume_page = nullptr;
    ChildTabPageContext font_page_ctx{};
    ChildTabPageContext volume_page_ctx{};
};

INT_PTR CALLBACK TonyuOptionDlgProc(HWND hdlg,
                                    UINT msg,
                                    WPARAM wparam,
                                    LPARAM lparam) {
    auto* ctx = reinterpret_cast<OptionDialogContext*>(
        ::GetWindowLongPtrW(hdlg, DWLP_USER));

    if (msg == WM_INITDIALOG) {
        ctx = reinterpret_cast<OptionDialogContext*>(lparam);
        ::SetWindowLongPtrW(hdlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));

        // Mirrors SiglusEngine C_window_base::attach_window (+0x2B4526):
        // reads DWL_DLGPROC and subclasses with CallWindowProcW forwarding
        // whenever DWL_DLGPROC != &TonyuOptionDlgProc.
        const LONG_PTR current_dlg_proc =
            ::GetWindowLongPtrW(hdlg, DWLP_DLGPROC);
        if (current_dlg_proc !=
            reinterpret_cast<LONG_PTR>(&TonyuOptionDlgProc)) {
            ctx->subclassed_old_proc =
                reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
                    hdlg, DWLP_DLGPROC,
                    reinterpret_cast<LONG_PTR>(&TonyuOptionDlgProc)));
        }

        const HINSTANCE hinst = ::GetModuleHandleW(nullptr);

        // Creating SysTabControl32 and SysListView32 sends synchronous
        // WM_NOTIFYFORMAT (0x0055) back to `hdlg` inside WM_INITDIALOG.
        ctx->tab_hwnd = ::CreateWindowExW(
            0u, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            10, 10, 320, 180, hdlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTabControl)), hinst,
            nullptr);
        InsertTabItem(ctx->tab_hwnd, 0, L"画面");
        InsertTabItem(ctx->tab_hwnd, 1, L"音量");
        InsertTabItem(ctx->tab_hwnd, 2, L"システム");

        ctx->listview_hwnd = ::CreateWindowExW(
            0u, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 195,
            320, 80, hdlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdListView)), hinst,
            nullptr);
        InsertListViewColumn(ctx->listview_hwnd, 0, L"番号");
        InsertListViewColumn(ctx->listview_hwnd, 1, L"日付");
        InsertListViewColumn(ctx->listview_hwnd, 2, L"タイトル");

        // Nested modeless tab-page child dialogs created inside WM_INITDIALOG,
        // matching C_cfg_wnd::on_init_dialog.
        InMemoryDlgTemplateBuilder font_page_tpl(WS_CHILD | DS_CONTROL, 14, 36,
                                                 290, 130, L"");
        // Intentionally narrow (28 DLU) checkbox with a WS_VISIBLE sibling on
        // the same row (x = 72 DLU) while the parent dialog is not yet shown:
        // verifies checkbox auto-widening stops cleanly before the sibling.
        font_page_tpl.AddControl(0x0080u, BS_AUTOCHECKBOX, 8, 8, 28, 14,
                                 kIdChildBoldCheck,
                                 L"プロポーショナルフォントを表示する");
        font_page_tpl.AddControl(0x0080u, BS_AUTORADIOBUTTON, 72, 8, 45, 14,
                                 kIdChildSameRowRightRadio, L"影あり");
        font_page_tpl.AddControl(0x0080u, BS_PUSHBUTTON, 8, 30, 100, 16,
                                 kIdChildResetButton, L"初期状態に戻す");
        font_page_tpl.AddControl(0x0082u, 0u, 8, 54, 120, 14,
                                 kIdChildDurationStatic, L"スタティック");
        ctx->child_font_page = ::CreateDialogIndirectParamW(
            hinst, font_page_tpl.Data(), hdlg, &TonyuChildTabDlgProc,
            reinterpret_cast<LPARAM>(&ctx->font_page_ctx));

        InMemoryDlgTemplateBuilder vol_page_tpl(WS_CHILD | DS_CONTROL, 14, 36,
                                                290, 130, L"");
        vol_page_tpl.AddControl(0x0080u, BS_GROUPBOX, 8, 8, 120, 40,
                                kIdChildSystemGroup, L"ｼｽﾃﾑ");
        vol_page_tpl.AddControl(0x0080u, BS_PUSHBUTTON, 16, 22, 60, 14,
                                kIdChildPlayButton, L"再生");
        ctx->child_volume_page = ::CreateDialogIndirectParamW(
            hinst, vol_page_tpl.Data(), hdlg, &TonyuChildTabDlgProc,
            reinterpret_cast<LPARAM>(&ctx->volume_page_ctx));

        return TRUE;
    }

    if (msg == WM_NOTIFYFORMAT && ctx != nullptr) {
        ++ctx->notify_format_messages;
    }

    if (ctx != nullptr && ctx->subclassed_old_proc != nullptr) {
        if (ctx->dispatch_depth >= kMaxRecursionGuardDepth) {
            ctx->recursion_overflow_detected = true;
            return FALSE;
        }
        ++ctx->dispatch_depth;
        const LRESULT forwarded = ::CallWindowProcW(
            ctx->subclassed_old_proc, hdlg, msg, wparam, lparam);
        --ctx->dispatch_depth;
        return forwarded;
    }
    return FALSE;
}

struct ModalDialogCapture {
    std::wstring post_init_caption;
    std::wstring post_init_confirm_btn;
    std::wstring post_init_cancel_btn;
    HFONT post_init_ok_font = nullptr;
    WNDPROC subclassed_old_proc = nullptr;
};

INT_PTR CALLBACK ModalConfirmDlgProc(HWND hdlg,
                                     UINT msg,
                                     WPARAM /*wparam*/,
                                     LPARAM lparam) {
    auto* cap = reinterpret_cast<ModalDialogCapture*>(
        ::GetWindowLongPtrW(hdlg, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        cap = reinterpret_cast<ModalDialogCapture*>(lparam);
        ::SetWindowLongPtrW(hdlg, DWLP_USER, reinterpret_cast<LONG_PTR>(cap));

        const LONG_PTR current_dlg_proc =
            ::GetWindowLongPtrW(hdlg, DWLP_DLGPROC);
        if (current_dlg_proc !=
            reinterpret_cast<LONG_PTR>(&ModalConfirmDlgProc)) {
            cap->subclassed_old_proc =
                reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
                    hdlg, DWLP_DLGPROC,
                    reinterpret_cast<LONG_PTR>(&ModalConfirmDlgProc)));
        }

        // Post an async message so WH_CALLWNDPROCRET finishes WM_INITDIALOG
        // localization before we snapshot texts and EndDialog.
        ::PostMessageW(hdlg, kMsgInspectAndCloseModal, 0u, 0);
        return TRUE;
    }

    if (msg == kMsgInspectAndCloseModal && cap != nullptr) {
        const HWND ok_btn = ::GetDlgItem(hdlg, IDOK);
        cap->post_init_caption = ReadControlText(hdlg);
        cap->post_init_confirm_btn = ReadControlText(ok_btn);
        cap->post_init_cancel_btn =
            ReadControlText(::GetDlgItem(hdlg, IDCANCEL));
        cap->post_init_ok_font =
            reinterpret_cast<HFONT>(::SendMessageW(ok_btn, WM_GETFONT, 0, 0));
        ::EndDialog(hdlg, IDOK);
        return TRUE;
    }
    return FALSE;
}

class Win32ChromeComponentTest : public ::testing::Test {
protected:
    void SetUp() override {
        EnsureCommonControlsInitialized();
        ASSERT_TRUE(win32_chrome::Win32ChromeInit());
    }

    void TearDown() override { win32_chrome::Win32ChromeShutdown(); }
};

}  // namespace

TEST_F(Win32ChromeComponentTest,
       OptionDialogWithTonyuSubclassingAndNestedTabsOpensWithoutRecursionAndLocalizesAllChrome) {
    InMemoryDlgTemplateBuilder option_tpl(
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 350, 300, L"環境設定");
    option_tpl.AddControl(0x0080u, BS_PUSHBUTTON, 10, 278, 110, 16,
                          kIdResetAllButton, L"全て初期状態に戻す");
    option_tpl.AddControl(0x0080u, BS_PUSHBUTTON, 260, 278, 70, 16,
                          kIdCloseButton, L"閉じる");

    OptionDialogContext ctx{};
    const HWND hdlg = ::CreateDialogIndirectParamW(
        ::GetModuleHandleW(nullptr), option_tpl.Data(), nullptr,
        &TonyuOptionDlgProc, reinterpret_cast<LPARAM>(&ctx));
    ASSERT_NE(hdlg, nullptr);

    // Zero DWLP_DLGPROC wrapper substitution -> zero mutual recursion on
    // synchronous WM_NOTIFYFORMAT during child control creation.
    EXPECT_EQ(ctx.subclassed_old_proc, nullptr);
    EXPECT_FALSE(ctx.recursion_overflow_detected);
    EXPECT_GE(ctx.notify_format_messages, 1);
    EXPECT_EQ(ctx.font_page_ctx.subclassed_old_proc, nullptr);
    EXPECT_FALSE(ctx.font_page_ctx.recursion_overflow_detected);
    EXPECT_EQ(ctx.volume_page_ctx.subclassed_old_proc, nullptr);
    EXPECT_FALSE(ctx.volume_page_ctx.recursion_overflow_detected);

    // Top-level Option dialog caption & buttons.
    EXPECT_EQ(ReadControlText(hdlg), L"Configuration");
    EXPECT_EQ(ReadControlText(::GetDlgItem(hdlg, kIdResetAllButton)),
              L"Reset All to Default");
    EXPECT_EQ(ReadControlText(::GetDlgItem(hdlg, kIdCloseButton)), L"Close");

    // SysTabControl32 headers and tab switching across pages.
    ASSERT_NE(ctx.tab_hwnd, nullptr);
    EXPECT_EQ(ReadTabItemText(ctx.tab_hwnd, 0), L"Screen");
    EXPECT_EQ(ReadTabItemText(ctx.tab_hwnd, 1), L"Volume");
    EXPECT_EQ(ReadTabItemText(ctx.tab_hwnd, 2), L"System");

    ::SendMessageW(ctx.tab_hwnd, TCM_SETCURSEL, 1, 0);
    EXPECT_EQ(::SendMessageW(ctx.tab_hwnd, TCM_GETCURSEL, 0, 0), 1);
    ::ShowWindow(ctx.child_font_page, SW_HIDE);
    ::ShowWindow(ctx.child_volume_page, SW_SHOWNOACTIVATE);

    // SysListView32 column headers (Dialog #146 Save/Load pattern).
    ASSERT_NE(ctx.listview_hwnd, nullptr);
    EXPECT_EQ(ReadListViewColumnText(ctx.listview_hwnd, 0), L"No.");
    EXPECT_EQ(ReadListViewColumnText(ctx.listview_hwnd, 1), L"Date");
    EXPECT_EQ(ReadListViewColumnText(ctx.listview_hwnd, 2), L"Scene");

    // Nested child tab-page controls & proportional dialog font.
    ASSERT_NE(ctx.child_font_page, nullptr);
    ASSERT_NE(ctx.child_volume_page, nullptr);
    const HWND bold_chk = ::GetDlgItem(ctx.child_font_page, kIdChildBoldCheck);
    const HWND right_sib =
        ::GetDlgItem(ctx.child_font_page, kIdChildSameRowRightRadio);
    const HWND reset_btn =
        ::GetDlgItem(ctx.child_font_page, kIdChildResetButton);
    const HWND sys_grp =
        ::GetDlgItem(ctx.child_volume_page, kIdChildSystemGroup);
    const HWND play_btn =
        ::GetDlgItem(ctx.child_volume_page, kIdChildPlayButton);
    ASSERT_NE(bold_chk, nullptr);
    ASSERT_NE(right_sib, nullptr);
    EXPECT_EQ(ReadControlText(bold_chk), L"Show proportional fonts");
    EXPECT_EQ(ReadControlText(right_sib), L"Shadow");
    EXPECT_EQ(ReadControlText(reset_btn), L"Reset to Default");
    EXPECT_EQ(ReadControlText(sys_grp), L"System");
    EXPECT_EQ(ReadControlText(play_btn), L"Play");
    EXPECT_NE(::SendMessageW(bold_chk, WM_GETFONT, 0, 0), 0);

    RECT bold_rc{};
    RECT right_sib_rc{};
    RECT page_rc{};
    ASSERT_NE(::GetWindowRect(bold_chk, &bold_rc), FALSE);
    ASSERT_NE(::GetWindowRect(right_sib, &right_sib_rc), FALSE);
    ASSERT_NE(::GetWindowRect(ctx.child_font_page, &page_rc), FALSE);
    EXPECT_GT(bold_rc.right - bold_rc.left, kMinWidenedCheckboxWidthPx);
    EXPECT_LE(bold_rc.right, right_sib_rc.left);
    EXPECT_LT(bold_rc.right, page_rc.right);

    // Post-WM_INITDIALOG dynamic SetDlgItemTextW update (Auto Mode duration).
    ASSERT_NE(::SetDlgItemTextW(ctx.child_font_page, kIdChildDurationStatic,
                                L"0．25 秒"),
              FALSE);
    EXPECT_EQ(
        ReadControlText(::GetDlgItem(ctx.child_font_page, kIdChildDurationStatic)),
        L"0.25 s");

    ASSERT_NE(::DestroyWindow(hdlg), FALSE);
}

TEST_F(Win32ChromeComponentTest,
       SequentialModalDialogsWindowTitleLookupAndPopupMenusRemainIdempotent) {
    // 1. Two sequential modal DialogBoxIndirectParamW invocations verify that
    // scoped thread hook installation/uninstallation is clean across runs.
    for (int run = 0; run < 2; ++run) {
        InMemoryDlgTemplateBuilder modal_tpl(
            WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 220, 90,
            L"音量の設定");
        modal_tpl.AddControl(0x0080u, BS_DEFPUSHBUTTON, 20, 60, 70, 16, IDOK,
                             L"決定");
        modal_tpl.AddControl(0x0080u, BS_PUSHBUTTON, 120, 60, 70, 16, IDCANCEL,
                             L"キャンセル");

        ModalDialogCapture capture{};
        const INT_PTR rc = ::DialogBoxIndirectParamW(
            ::GetModuleHandleW(nullptr), modal_tpl.Data(), nullptr,
            &ModalConfirmDlgProc, reinterpret_cast<LPARAM>(&capture));
        EXPECT_EQ(rc, IDOK);
        EXPECT_EQ(capture.subclassed_old_proc, nullptr);
        EXPECT_EQ(capture.post_init_caption, L"Volume Settings");
        EXPECT_EQ(capture.post_init_confirm_btn, L"Confirm");
        EXPECT_EQ(capture.post_init_cancel_btn, L"Cancel");
        EXPECT_NE(capture.post_init_ok_font, nullptr);
        EXPECT_EQ(::GetObjectType(capture.post_init_ok_font),
                  static_cast<DWORD>(OBJ_FONT));
    }

    // 2. Host window title creation, dynamic FPS suffix update, and FindWindowW
    // fallback from Japanese #GAMENAME to localized English caption.
    const HWND main_wnd = ::CreateWindowExW(
        0u, L"STATIC", L"さよならを教えて", WS_POPUP, 0, 0, 100, 40, nullptr,
        nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(main_wnd, nullptr);
    EXPECT_EQ(ReadControlText(main_wnd),
              L"Sayonara o Oshiete ~comment te dire adieu~");
    EXPECT_EQ(::FindWindowW(L"STATIC", L"さよならを教えて"), main_wnd);

    ASSERT_NE(::SetWindowTextW(main_wnd, L"さよならを教えて [60.0 fps]"),
              FALSE);
    EXPECT_EQ(ReadControlText(main_wnd),
              L"Sayonara o Oshiete ~comment te dire adieu~ [60.0 fps]");
    ASSERT_NE(::DestroyWindow(main_wnd), FALSE);

    // 3. Right-click #SYSCOMMENU popup menu via InsertMenuItemW.
    const HMENU popup = ::CreatePopupMenu();
    ASSERT_NE(popup, nullptr);

    wchar_t jp_config[] = L"環境設定 (&O)\tCtrl+O";
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING | MIIM_ID;
    mii.wID = 401u;
    mii.dwTypeData = jp_config;
    ASSERT_NE(::InsertMenuItemW(popup, 0u, TRUE, &mii), FALSE);

    wchar_t menu_buf[kTextReadBufChars] = {};
    MENUITEMINFOW query{};
    query.cbSize = sizeof(query);
    query.fMask = MIIM_STRING;
    query.dwTypeData = menu_buf;
    query.cch = static_cast<UINT>(kTextReadBufChars);
    ASSERT_NE(::GetMenuItemInfoW(popup, 0u, TRUE, &query), FALSE);
    EXPECT_EQ(std::wstring(menu_buf), L"Configuration (&O)\tCtrl+O");

    ASSERT_NE(::DestroyMenu(popup), FALSE);
}
