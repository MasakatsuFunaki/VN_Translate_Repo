// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/gameexe_patch_rules.h"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace adi::menu {

namespace {

struct DirectiveRule {
    std::string_view key;
    std::string_view value;
};

// Pure single-quoted-string directives (#KEY = "English").
constexpr std::array<DirectiveRule, 44> kQuotedStringRules = {{
    {"GAMENAME", "Sayonara o Oshiete ~comment te dire adieu~"},
    {"DUMMY_CHECK_STR",
     "Please insert the Sayonara o Oshiete disc.\\n\\n*This is not a copy-protection error."},
    {"DUMMY_CHECK_OK_STR", "Thank you for purchasing Sayonara o Oshiete."},

    {"SYSCOMMENU.LOCAL_EXTRA_MODE.000.ITEM.000.STR", "Mode 0"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.000.ITEM.001.STR", "Mode 1"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.000.ITEM.002.STR", "Mode 2"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.001.ITEM.000.STR", "Mode 0"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.001.ITEM.001.STR", "Mode 1"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.001.ITEM.002.STR", "Mode 2"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.002.ITEM.000.STR", "Mode 0"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.002.ITEM.001.STR", "Mode 1"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.002.ITEM.002.STR", "Mode 2"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.003.ITEM.000.STR", "Mode 0"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.003.ITEM.001.STR", "Mode 1"},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.003.ITEM.002.STR", "Mode 2"},

    {"DIALOG.MESSAGE_CHRCOLOR.STR", "Color-code dialogue by character."},
    {"DIALOG.SLEEP.STR", "Reduce CPU usage so other programs run smoothly."},
    {"DIALOG.NO_WIPE_ANIME.STR", "Disable screen transition animations."},
    {"DIALOG.SKIP_WIPE_ANIME.STR", "Skip screen transitions with a mouse click."},
    {"DIALOG.NO_MWND_ANIME.STR", "Disable message window open/close animations."},
    {"DIALOG.WHEEL_NEXT_MESSAGE.STR", "Advance text by scrolling the mouse wheel down."},
    {"DIALOG.KOE_DONT_STOP.STR", "Keep playing voice lines when advancing text."},
    {"DIALOG.SKIP_UNREAD_MESSAGE.STR", "Allow skipping unread text."},
    {"DIALOG.PLAY_SILENT_SOUND.STR", "Check this if audio playback produces static noise."},

    {"WARNINGINFO.RESTART_WARNING_STR", "Start from the beginning?"},
    {"WARNINGINFO.RETURNMENU_WARNING_STR", "Return to the title screen?"},
    {"WARNINGINFO.RETURNSEL_WARNING_STR", "Return to the previous choice?"},
    {"WARNINGINFO.GAMEEND_WARNING_STR", "Quit the game?"},

    {"SAVE.NO_DATA_STR", "No Data."},
    {"SAVEINFO.DLGWND_CAPTION_TITLE_STR", "Save"},
    {"SAVEINFO.DLGWND_DESIDE_BUTTON_STR", "Save"},
    {"SAVEINFO.DLGWND_DATALIST_NAMEHEADER_STR", "Save"},
    {"SAVEINFO.DLGWND_WARNING_CHKBOX_STR", "Confirm before overwriting saved data."},
    {"SAVEINFO.DLGWND_DBLCLICK_CHKBOX_STR", "Save by double-clicking a slot."},
    {"SAVEINFO.WARNING_STR", "Save to this slot?"},
    {"SAVEINFO.QUICK_WARNING_STR", "Quick-save to this slot?"},

    {"LOADINFO.DLGWND_CAPTION_TITLE_STR", "Load"},
    {"LOADINFO.DLGWND_DESIDE_BUTTON_STR", "Load"},
    {"LOADINFO.DLGWND_DATALIST_NAMEHEADER_STR", "Load"},
    {"LOADINFO.DLGWND_WARNING_CHKBOX_STR", "Confirm before loading saved data."},
    {"LOADINFO.DLGWND_DBLCLICK_CHKBOX_STR", "Load by double-clicking a slot."},
    {"LOADINFO.WARNING_STR", "Load this saved data?"},
    {"LOADINFO.QUICK_WARNING_STR", "Quick-load this saved data?"},

    {"CHRKOE.NOT_LOOK_NAME_STR", "???"},
}};

// Multi-token context-menu directives preserving leading enable/visibility flags.
constexpr std::array<DirectiveRule, 20> kMultiTokenStringRules = {{
    {"SYSCOMMENU.READ_SKIP", "1, 1, \"Skip Read Text\""},
    {"SYSCOMMENU.UNREAD_SKIP", "1, 1, \"Skip Text\""},
    {"SYSCOMMENU.AUTO_SKIP", "1, 1, \"Auto Skip\""},
    {"SYSCOMMENU.AUTO_MODE", "1, 1, \"Auto Mode\""},
    {"SYSCOMMENU.HIDE_MWND", "1, 1, \"Hide Window\""},
    {"SYSCOMMENU.SAVE", "1, 1, \"Save\""},
    {"SYSCOMMENU.LOAD", "1, 1, \"Load\""},
    {"SYSCOMMENU.RETURN_SEL", "1, 1, \"Previous Choice\""},
    {"SYSCOMMENU.CONFIG", "1, 1, \"Settings\""},
    {"SYSCOMMENU.RETURN_MENU", "1, 1, \"Return to Title\""},
    {"SYSCOMMENU.GAME_END", "1, 1, \"Quit Game\""},
    {"SYSCOMMENU.CANCEL", "1, 1, \"Cancel\""},

    {"SYSCOMMENU.LOCAL_EXTRA_SWITCH.000", "0, 0, 0, \"Local Switch 0\""},
    {"SYSCOMMENU.LOCAL_EXTRA_SWITCH.001", "0, 0, 0, \"Local Switch 1\""},
    {"SYSCOMMENU.LOCAL_EXTRA_SWITCH.002", "0, 0, 0, \"Local Switch 2\""},
    {"SYSCOMMENU.LOCAL_EXTRA_SWITCH.003", "0, 0, 0, \"Local Switch 3\""},

    {"SYSCOMMENU.LOCAL_EXTRA_MODE.000", "0, 0, 0, \"Local Mode 0\""},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.001", "0, 0, 0, \"Local Mode 1\""},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.002", "0, 0, 0, \"Local Mode 2\""},
    {"SYSCOMMENU.LOCAL_EXTRA_MODE.003", "0, 0, 0, \"Local Mode 3\""},
}};

// Full-screen NVL (#MWND.002.*) and choice button (#SELBTN.000/001.MOJI_SIZE)
// layout adjustments tuned for proportional English text on 640x480.
constexpr std::array<DirectiveRule, 7> kLayoutMetricRules = {{
    {"MWND.002.MOJI_SIZE", "20"},
    {"MWND.002.MOJI_SPACE", "0, 3"},
    {"MWND.002.MOJI_CNT", "30, 19"},
    {"MWND.002.NAME_MOJI_SIZE", "16"},
    {"MWND.002.TALK_MARGIN", "20, 24, 0, 20"},
    {"SELBTN.000.MOJI_SIZE", "22, 0, 0, 0"},
    {"SELBTN.001.MOJI_SIZE", "22, 0, 0, 0"},
}};

void RequireUpdated(std::size_t updated_count, std::string_view key) {
    if (updated_count == 0u) {
        throw std::runtime_error(
            "GameexePatchRules: expected directive #" + std::string(key) +
            " was not found in Gameexe.dat");
    }
}

}  // namespace

struct GameexePatchRules::Impl {};

GameexePatchRules::GameexePatchRules() : impl_(std::make_unique<Impl>()) {}
GameexePatchRules::GameexePatchRules(GameexePatchRules&& other) noexcept = default;
GameexePatchRules& GameexePatchRules::operator=(GameexePatchRules&& other) noexcept = default;
GameexePatchRules::~GameexePatchRules() = default;

std::string GameexePatchRules::DomainName() const {
    return "gameexe_patch_rules";
}

void GameexePatchRules::ApplyToDocument(g00::GameexeDocument& doc) {
    for (const DirectiveRule& rule : kQuotedStringRules) {
        RequireUpdated(doc.SetDirectiveStringValue(rule.key, rule.value), rule.key);
    }
    for (const DirectiveRule& rule : kMultiTokenStringRules) {
        RequireUpdated(doc.SetDirectiveRawValue(rule.key, rule.value), rule.key);
    }
    for (const DirectiveRule& rule : kLayoutMetricRules) {
        RequireUpdated(doc.SetDirectiveRawValue(rule.key, rule.value), rule.key);
    }
}

void GameexePatchRules::Apply(PatchBuildContext& ctx) {
    g00::GameexeDocument doc = ctx.LoadGameexeDocument();
    ApplyToDocument(doc);
    ctx.SubmitPatchedGameexe(doc);
}

std::unique_ptr<IMenuDomainRenderer> CreateGameexePatchRules() {
    return std::make_unique<GameexePatchRules>();
}

}  // namespace adi::menu
