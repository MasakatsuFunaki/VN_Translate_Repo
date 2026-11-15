// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "chrome_text_logic.h"

#include "translator_logic.h"

#include <unordered_map>
#include <utility>

namespace chrome_text_logic {

namespace {

struct Utf8UiPair {
    const char* jp_utf8;
    const char* en_utf8;
};

using NarrowLookupMap = std::unordered_map<std::string, std::string>;

NarrowLookupMap BuildDualEncodedMap(const Utf8UiPair* pairs, std::size_t count) {
    NarrowLookupMap map;
    map.reserve(count * 2u);
    for (std::size_t i = 0u; i < count; ++i) {
        const std::string en_cp932 = translator_logic::Utf8ToCp932(pairs[i].en_utf8);
        const std::string jp_cp932 = translator_logic::Utf8ToCp932(pairs[i].jp_utf8);
        map.emplace(jp_cp932, en_cp932);
        map.emplace(pairs[i].jp_utf8, en_cp932);
    }
    return map;
}

const NarrowLookupMap& EngineUiMap() {
    static const NarrowLookupMap kMap = [] {
        static constexpr Utf8UiPair kPairs[] = {
            // Modal buttons, status banners, and placeholders
            {"はい", "Yes"},
            {"／", "/"},
            {"いいえ", "No"},
            {"保存中…", "Saving..."},
            {"時間切れ", "Time's Up"},
            {"？？？？？？？？？？", "??????????"},
            {"？？？？？？？？", "????????"},
            {"－－－－－－", "------"},

            // HUD, Backlog, and Achievement format templates
            {"実績解除【%s : %dG】", "Unlocked [%s: %dG]"},
            {"SCENE：%s", "SCENE: %s"},
            {"♪：－－－－－－", "♪: ------"},
            {"♪：%s", "♪: %s"},
            {"シーン　　：－－－－－－", "Scene: ------"},
            {"シーン　　：%s", "Scene: %s"},

            // Config reset, Backlog jump, and Return-to-Title modal prompts
            {"表示している設定項目を初期化してもよろしいでしょうか？",
             "Reset the settings on this page to default?"},
            {"全ての設定項目を初期化してもよろしいでしょうか？",
             "Reset all settings to default?"},
            {"現在の状態をクイックセーブして、選択した箇所から",
             "Quick-save current progress and jump"},
            {"再スタートします。よろしいですか？",
             "to the selected line. Are you sure?"},
            {"ゲームを終了してタイトル画面へ戻ります",
             "Return to the title screen."},
            {"セーブを行っていない場合、現在プレイ中のデータは失われます",
             "Any unsaved progress will be lost."},
            {"セーブを行っていない場合、現在プレイ中のデータは失われます。",
             "Any unsaved progress will be lost."},
            {"よろしいですか？", "Are you sure?"},

            // Save / Load / System / Storage modal prompts
            {"データをロードします。", "Load selected save data."},
            {"データを上書きしてもよろしいですか？",
             "Overwrite existing save data?"},
            {"データのセーブに失敗しました。", "Failed to save data."},
            {"もう一度セーブを行ってください。", "Please try saving again."},
            {"セーブデータが破損しています。", "Save data is corrupted."},
            {"セーブデータのロードに失敗しました。", "Failed to load save data."},
            {"セーブデータが破損している可能性があります。",
             "Save data may be corrupted."},
            {"このままセーブを行うと破損データに上書きすることになりますが、",
             "Saving now will overwrite the corrupted data."},
            {"システムデータをセーブしますか？", "Save system data?"},
            {"システムデータをロードしますか？", "Load system data?"},
            {"システムデータをロードしました。", "System data loaded."},
            {"処理が中断されました。", "Operation canceled."},
            {"セーブデータがありません。", "No save data found."},
            {"「\"TITLE_NAME L\"」の", "For CROSS†CHANNEL:"},
            {"もう一度チェックしますか？", "Check again?"},
            {"メモリースティックが挿入されていません。",
             "No storage media inserted."},
            {"ゲームを開始してもよろしいですか？", "Start the game anyway?"},
            {"セーブするには%s以上の空き容量が必要です。",
             "At least %s of free space is required to save."},
            {"記録メディアの空き容量が不足しています。",
             "Not enough free space on storage media."},
            {"ストレージデバイスが選択されていないため、",
             "No storage device has been selected."},
            {"セーブを行うことは出来ません。セーブを行うには、",
             "Cannot save data. To save data,"},
            {"ロードを行うことは出来ません。ロードを行うには、",
             "Cannot load data. To load data,"},
            {"ゲームを再起動してストレージデバイスを選択してください。",
             "please restart and select a storage device."},
            {"サインインされていないため、セーブを行うことは出来ません。",
             "Cannot save data while not signed in."},
            {"セーブを行うには、サインインを行ってください。",
             "Please sign in to save data."},
            {"サインインされていないため、ロードを行うことは出来ません。",
             "Cannot load data while not signed in."},
            {"ロードを行うには、サインインを行ってください。",
             "Please sign in to load data."},

            // Playing Log Tab 1: Statistics (6 rows)
            {"エンディング達成数", "Endings Reached"},
            {"シーンタイトル達成数", "Scene Titles Unlocked"},
            {"アルバム完成度", "Album Completion"},
            {"ミュージック完成度", "Music Completion"},
            {"メッセージ既読数", "Messages Read"},
            {"選択肢既読数", "Choices Selected"},

            // Playing Log Tab 1 & Scene Table: Main + Extra Stories
            {"クロスチャンネル", "CROSS†CHANNEL"},
            {"EX:桐原冬子の場合", "EX: Touko Kirihara's Case"},
            {"EX:宮澄見里の場合", "EX: Misato Miyasumi's Case"},
            {"EX:佐倉霧の場合", "EX: Kiri Sakura's Case"},
            {"EX:山辺美希の場合", "EX: Miki Yamanobe's Case"},
            {"EX:支倉曜子の場合", "EX: Youko Hasekura's Case"},
            {"EX:島友貴の場合", "EX: Tomoki Shima's Case"},
            {"EX:桜庭浩の場合", "EX: Hiroshi Sakuraba's Case"},
            {"EX:CROSS†'CHANNEL", "EX: CROSS†'CHANNEL"},
            {"EX:AFTER STORY", "EX: AFTER STORY"},

            // Playing Log Tab 2 & Save/Pause/Backlog: Scene Titles
            {"崩壊", "Collapse"},
            {"純化", "Purification"},
            {"たった一つのもの", "The One and Only Thing"},
            {"謝りに", "To Apologize"},
            {"大切な人", "Someone Precious"},
            {"いつか、わたし", "Someday, I..."},
            {"弱虫", "Coward"},
            {"親友", "Best Friend"},
            {"黒須ちゃん†寝る", "Kurosu-chan†Sleeps"},

            // Playing Log Tab 2: Diary (DI:) Scene Titles (14 entries)
            {"DI:冬子のフェラチオ", "DI: Touko's Fellatio"},
            {"DI:冬子とのセックス", "DI: Sex with Touko"},
            {"DI:寝てる冬子へのいたずら", "DI: Mischief with Sleeping Touko"},
            {"DI:見里とのセックス", "DI: Sex with Misato"},
            {"DI:砂浜で見里のパイズリ", "DI: Misato's Paizuri on the Beach"},
            {"DI:砂浜で見里とセックス", "DI: Sex with Misato on the Beach"},
            {"DI:霧への羞恥プレイ", "DI: Humiliation Play with Kiri"},
            {"DI:霧のフェラチオ", "DI: Kiri's Fellatio"},
            {"DI:霧をバックから", "DI: Kiri from Behind"},
            {"DI:美希と後背位で", "DI: Doggy Style with Miki"},
            {"DI:美希と正常位で", "DI: Missionary with Miki"},
            {"DI:曜子と密かな悪戯", "DI: Secret Mischief with Youko"},
            {"DI:曜子の拘束フェラ", "DI: Youko's Bound Fellatio"},
            {"DI:曜子の拘束騎乗位", "DI: Youko's Bound Cowgirl"},
        };
        return BuildDualEncodedMap(kPairs, sizeof(kPairs) / sizeof(kPairs[0]));
    }();
    return kMap;
}

const NarrowLookupMap& ConfigTooltipMap() {
    static const NarrowLookupMap kMap = [] {
        static constexpr Utf8UiPair kPairs[] = {
            // 0x0C84A0: Window Opacity
            {"ゲーム中にメッセージが表示される、ウィンドウの透明度を変更します。%N%N『透明』の方に設定するほど、ウィンドウが透ける度合いが大きくなります。%N%N『濃い』の方に設定するほど、ウィンドウが透ける度合いが小さくなります。",
             "Adjusts the opacity of the message window.%N%NMove toward \"Clear\" to make the window more transparent.%N%NMove toward \"Solid\" to make the window more opaque."},

            // 0x0C84A4: Text Speed
            {"ゲーム中に表示されるメッセージの速度を変更します。%N%N『ＭＩＮ』の方に設定するほど、表示される速度が遅くなります。%N『ＭＡＸ』の方に設定するほど、表示される速度が速くなります。%N最大に設定すると、１文字ずつの表示ではなく、一括表示となります。ただし、音声があるメッセージで音声同期が『ＯＮ』の場合、そちらが優先されます。",
             "Changes message display speed.%N%NMove toward \"MIN\" for slower text or \"MAX\" for faster text.%N%NAt maximum, lines appear instantly unless Voice Sync is \"ON\"."},

            // 0x0C84A8: Voice Sync
            {"音声があるメッセージの表示を音声と同期させます。%N%N『ＯＮ』に設定すると、メッセージが音声の進みに合わせて表示されます。%N%N『ＯＦＦ』に設定すると、メッセージはメッセージ速度で設定した速度で表示されます。",
             "Synchronizes voiced text display with character voice playback.%N%N\"ON\" paces text to match the spoken line.%N%N\"OFF\" uses the configured Text Speed."},

            // 0x0C84AC: Read Choice Color
            {"ゲーム中に表示される選択肢の文字色を、選択したことがあるかどうかで変更します。%N%N『ＯＮ』に設定すると、選んだことがある選択肢の文字が赤色に変更されます。%N%N『ＯＦＦ』に設定すると、文字色は変更されません。",
             "Changes the text color of previously selected choices.%N%N\"ON\" highlights previously chosen options in red.%N%N\"OFF\" keeps all choices the same color."},

            // 0x0C84B0: Text Advance
            {"ゲーム中に表示されるメッセージを送るボタンを設定します。%N%N『○ボタン』に設定すると、○ボタンでのみメッセージ送りをする事ができます。%N%N『○ボタン／方向キー』に設定すると、○ボタンだけでなく方向キーや左スティックでメッセージを送ることができます。",
             "Sets which inputs advance dialogue messages.%N%N\"Click Only\" advances text with click / confirm only.%N%N\"Click & Keys\" also allows directional keys and the left stick."},

            // 0x0C84B4: Auto Speed
            {"オートモード時のメッセージが送られる速度を設定します。%N%N『ＭＩＮ』の方に設定するほど、メッセージが送られるまでの待ち時間が長くなります。%N%N『ＭＡＸ』の方に設定するほど、メッセージが送られるまでの待ち時間が短くなります。",
             "Adjusts how quickly messages advance in Auto Mode.%N%NMove toward \"MIN\" for a longer wait between lines.%N%NMove toward \"MAX\" for a shorter wait between lines."},

            // 0x0C84B8: Skip Trigger ([Ctrl])
            {"[Ctrl]を押した時のスキップ機能の設定をします。%N%N『既読のみ』に設定すると、過去に表示したことがあるメッセージのみスキップし、一度も表示したことがないメッセージはスキップしません。%N%N『すべて』に設定すると、過去の表示に関係なくメッセージをスキップします。",
             "Sets skip behavior while holding [Ctrl].%N%N\"Read Only\" skips only previously read text.%N%N\"All\" skips both read and unread text."},

            // 0x0C84BC: Skip Mode ([Z])
            {"[Z]を押してスキップモードになった時の、スキップ機能の設定をします。%N%N『既読のみ』に設定すると、過去に表示したことがあるメッセージのみスキップし、一度も表示したことがないメッセージはスキップしません。%N%N『すべて』に設定すると、過去の表示に関係なくメッセージをスキップします。",
             "Sets skip behavior when toggling Skip Mode with [Z].%N%N\"Read Only\" skips only previously read text.%N%N\"All\" skips both read and unread text."},

            // 0x0C84C0: Scene Titles
            {"シーンが変わった際にシーンタイトルを表示するか設定します。%N%N『ＯＮ』に設定すると、シーンが変わった際にそのシーンのタイトルが表示されます。%N%N『ＯＦＦ』に設定すると、シーンが変わってもタイトルは表示されません。",
             "Controls whether scene titles appear when a new scene begins.%N%N\"ON\" displays the scene title on transitions.%N%N\"OFF\" hides scene title popups."},

            // 0x0C84C4: Vibration
            {"ゲーム中にコントローラが振動するかを設定します。%N%N『ＯＮ』に設定すると、画面が揺れる様な場面でコントローラが振動します。%N%N『ＯＦＦ』に設定すると、コントローラは振動しません。",
             "Controls gamepad vibration during gameplay.%N%N\"ON\" vibrates the controller during screen shakes.%N%N\"OFF\" disables controller vibration."},

            // 0x0C84C8: BGM Volume
            {"ゲーム中に流れるＢＧＭのボリュームを設定します。%N%N『ＭＩＮ』の方に設定するほど、小さな音で再生されます。%N%N『ＭＡＸ』の方に設定するほど、大きな音で再生されます。",
             "Adjusts background music (BGM) volume.%N%NMove toward \"MIN\" for quieter music.%N%NMove toward \"MAX\" for louder music."},

            // 0x0C84CC: SFX Volume
            {"ゲーム中に流れるＳＥのボリュームを設定します。%N%N『ＭＩＮ』の方に設定するほど、小さな音で再生されます。%N%N『ＭＡＸ』の方に設定するほど、大きな音で再生されます。",
             "Adjusts in-game sound effects (SFX) volume.%N%NMove toward \"MIN\" for quieter effects.%N%NMove toward \"MAX\" for louder effects."},

            // 0x0C84D0: System SE
            {"操作をした時に再生されるＳＥのボリュームを設定します。%N%N『ＭＩＮ』の方に設定するほど、小さな音で再生されます。%N%N『ＭＡＸ』の方に設定するほど、大きな音で再生されます。",
             "Adjusts UI and menu sound effect volume.%N%NMove toward \"MIN\" for quieter UI sounds.%N%NMove toward \"MAX\" for louder UI sounds."},

            // 0x0C84D4: Voice Volume
            {"ゲーム中に再生される音声のボリュームを設定します。%N%N『ＭＩＮ』の方に設定するほど、小さな音で再生されます。%N%N『ＭＡＸ』の方に設定するほど、大きな音で再生されます。",
             "Adjusts character voice playback volume.%N%NMove toward \"MIN\" for quieter voices.%N%NMove toward \"MAX\" for louder voices."},

            // 0x0C84D8: Display Area
            {"テレビ画面に表示する領域を設定します。%N%N『ＭＩＮ』の方に設定するほど、画面表示領域が小さくなります。%N%N『ＭＡＸ』の方に設定するほど、画面表示領域が大きくなります。",
             "Adjusts the active display area scaling.%N%NMove toward \"MIN\" for a smaller display region.%N%NMove toward \"MAX\" for a larger display region."},

            // 0x0C84DC: Screen Mode
            {"ゲーム画面のスクリーンモードを設定します。%N%N『Ｗｉｎｄｏｗ』に設定すると、ゲーム画面はウィンドウ形式で表示されます。%N%N『Ｆｕｌｌ』に設定すると、ゲーム画面はフルスクリーン形式で表示されます。",
             "Selects the display mode.%N%N\"Window\" runs the game in a desktop window.%N%N\"Full\" runs the game in fullscreen mode."},

            // 0x0C84E0: Dialog: Save
            {"セーブの上書きとロード・クイックロードを行う際の、確認メッセージの表示を行うか設定します。%N『ＯＮ』の場合は、セーブやロードが行われる際に確認メッセージが表示されます。%N%N『ＯＦＦ』の場合は、確認メッセージは表示されません。",
             "Controls confirmation prompts when overwriting saves or loading.%N%N\"ON\" asks for confirmation before saving or loading.%N%N\"OFF\" skips confirmation prompts."},

            // 0x0C84E4: Dialog: Log
            {"バックログジャンプを行う際の、確認メッセージの表示を行うか設定します。%N%N『ＯＮ』の場合は、バックログジャンプが行われる際に確認メッセージが表示されます。%N%N『ＯＦＦ』の場合は、確認メッセージは表示されません。",
             "Controls confirmation prompts when jumping from the backlog.%N%N\"ON\" asks before jumping to an earlier line.%N%N\"OFF\" jumps immediately without asking."},

            // 0x0C84E8: Dialog: Quit
            {"ゲームを終了する際の、確認メッセージの表示を行うか設定します。%N%N『ＯＮ』の場合は、ゲームが終了される際に確認メッセージが表示されます。%N%N『ＯＦＦ』の場合は、確認メッセージは表示されません。",
             "Controls confirmation prompts when exiting or returning to title.%N%N\"ON\" asks for confirmation before quitting.%N%N\"OFF\" exits without confirmation."},

            // 0x0C84EC: Skip at Choices
            {"選択肢に来た際に、スキップモードとオートモードを解除するかどうか設定します。%N%N『ＯＮ』に設定すると選択肢に来た際に、モードが解除されます。%N%N『ＯＦＦ』に設定すると選択肢に来ても、モードは解除されません。",
             "Controls whether Skip and Auto modes stop at choices.%N%N\"ON\" cancels Skip and Auto when a choice appears.%N%N\"OFF\" keeps the mode active across choices."},
        };
        return BuildDualEncodedMap(kPairs, sizeof(kPairs) / sizeof(kPairs[0]));
    }();
    return kMap;
}

std::string WrapSingleParagraph(std::string_view para, std::size_t max_cols) {
    if (para.empty() || max_cols == 0u) {
        return std::string(para);
    }

    std::string out;
    out.reserve(para.size() + 16u);
    std::size_t col = 0u;
    std::size_t i = 0u;

    while (i < para.size()) {
        while (i < para.size() && para[i] == ' ') {
            ++i;
        }
        if (i >= para.size()) {
            break;
        }

        std::size_t word_end = i;
        while (word_end < para.size() && para[word_end] != ' ') {
            ++word_end;
        }
        std::string_view word = para.substr(i, word_end - i);
        i = word_end;

        while (word.size() > max_cols) {
            if (col > 0u) {
                out += "%N";
                col = 0u;
            }
            out.append(word.data(), max_cols);
            word.remove_prefix(max_cols);
            if (!word.empty()) {
                out += "%N";
            } else {
                col = max_cols;
            }
        }

        if (word.empty()) {
            continue;
        }

        if (col == 0u) {
            out.append(word.data(), word.size());
            col = word.size();
        } else if (col + 1u + word.size() <= max_cols) {
            out.push_back(' ');
            out.append(word.data(), word.size());
            col += 1u + word.size();
        } else {
            out += "%N";
            out.append(word.data(), word.size());
            col = word.size();
        }
    }

    return out;
}

}  // namespace

std::optional<std::wstring> TranslateWindowTitle(std::wstring_view /*text*/) {
    // Preserve "CROSS†CHANNEL Ver.1.00" so the startup FindWindowW(L"PH", ...)
    // single-instance check continues to match existing instances.
    return std::nullopt;
}

std::optional<std::wstring> TranslateMessageBoxCaption(std::wstring_view text) {
    if (text == L"確認") {
        return std::wstring(L"Confirm");
    }
    if (text == L"エラー") {
        return std::wstring(L"Error");
    }
    return std::nullopt;
}

std::optional<std::wstring> TranslateMessageBoxBody(std::wstring_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (text == L"終了しますか？") {
        return std::wstring(L"Exit the game?");
    }
    if (text == L"ピクセルシェーダ2.0に対応していないため、起動できません") {
        return std::wstring(
            L"Cannot start because Pixel Shader 2.0 is not supported.");
    }
    if (text == L"頂点シェーダ2.0に対応していないため、起動できません") {
        return std::wstring(
            L"Cannot start because Vertex Shader 2.0 is not supported.");
    }
    if (text == L"最大テクスチャサイズが4096未満のため、起動できません") {
        return std::wstring(
            L"Cannot start because the maximum texture size is less than 4096.");
    }
    if (text ==
        L"ディスプレイ アダプタに問題があるためアプリケーションを続行できません") {
        return std::wstring(
            L"Cannot continue because there is a problem with the display adapter.");
    }
    return std::nullopt;
}

std::optional<std::wstring> TranslateDialogText(std::wstring_view text) {
    if (text == L"バージョン情報 ph") {
        return std::wstring(L"About CROSS†CHANNEL");
    }
    if (text == L"cc, Version 1.0") {
        return std::wstring(L"CROSS†CHANNEL Version 1.00");
    }
    return std::nullopt;
}

std::optional<std::string> TranslateEngineUiCp932(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string key(text);
    const auto& ui_map = EngineUiMap();
    if (const auto it = ui_map.find(key); it != ui_map.end()) {
        return it->second;
    }
    const auto& tip_map = ConfigTooltipMap();
    if (const auto it = tip_map.find(key); it != tip_map.end()) {
        return WrapEngineTooltipCp932(it->second, kDefaultTooltipMaxCols);
    }
    return std::nullopt;
}

std::optional<std::string> TranslateConfigTooltipCp932(std::string_view text,
                                                       std::size_t max_cols) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto& tip_map = ConfigTooltipMap();
    const auto it = tip_map.find(std::string(text));
    if (it == tip_map.end()) {
        return std::nullopt;
    }
    return WrapEngineTooltipCp932(it->second, max_cols);
}

std::string WrapEngineTooltipCp932(std::string_view text,
                                   std::size_t max_cols) {
    if (text.empty() || max_cols == 0u) {
        return std::string(text);
    }

    std::string result;
    result.reserve(text.size() + 32u);

    std::size_t pos = 0u;
    while (pos <= text.size()) {
        const std::size_t nl = text.find("%N", pos);
        if (nl == std::string_view::npos) {
            result += WrapSingleParagraph(text.substr(pos), max_cols);
            break;
        }
        result += WrapSingleParagraph(text.substr(pos, nl - pos), max_cols);
        result += "%N";
        pos = nl + 2u;
    }

    return result;
}

std::size_t MeasureEngineDisplayUnits(std::string_view cp932_text) {
    std::size_t units = 0u;
    std::size_t i = 0u;
    while (i < cp932_text.size()) {
        if (cp932_text[i] == '\0') {
            break;
        }
        if (cp932_text[i] == '%' && i + 1u < cp932_text.size() &&
            cp932_text[i + 1u] == 'N') {
            break;
        }
        const auto lead = static_cast<unsigned char>(cp932_text[i]);
        if (translator_logic::IsCp932LeadByte(lead) &&
            i + 1u < cp932_text.size()) {
            units += 2u;
            i += 2u;
        } else {
            units += 1u;
            i += 1u;
        }
    }
    return units;
}

float ComputeModalLineX(std::string_view cp932_text,
                        int modal_offset_x,
                        float step_x) {
    const float unit_advance = step_x * 0.5f;
    const float line_width =
        static_cast<float>(MeasureEngineDisplayUnits(cp932_text)) * unit_advance;
    return static_cast<float>(modal_offset_x) + kScreenCenterX -
           (line_width * 0.5f);
}

int ComputeModalFirstLineOffsetY(int line_count, bool has_buttons) {
    const int half_block = (line_count * kModalLineStrideY - 2) / 2;
    const int base_y =
        has_buttons ? kModalTextBaseWithButtonsY : kModalTextBaseNoButtonsY;
    return base_y - half_block;
}

ModalButtonLayout ComputeModalButtonLayout(int modal_offset_x,
                                           int modal_offset_y,
                                           float step_x) {
    const float unit_px = step_x * 0.5f;
    const float base_x = static_cast<float>(modal_offset_x);

    const float yes_width = 3.0f * unit_px;
    const float slash_width = 1.0f * unit_px;

    const float yes_center = base_x + kModalYesRectLeft + (kModalYesRectWidth * 0.5f);
    const float yes_x = yes_center - (yes_width * 0.5f);

    const float gap_left = base_x + kModalYesRectLeft + kModalYesRectWidth;
    const float gap_right = base_x + kModalNoRectLeft;
    const float slash_center = (gap_left + gap_right) * 0.5f;
    const float slash_x = slash_center - (slash_width * 0.5f);

    const float yes_to_slash_gap = slash_x - (yes_x + yes_width);
    const float no_x = slash_x + slash_width + yes_to_slash_gap;

    ModalButtonLayout out;
    out.yes_x = yes_x;
    out.slash_x = slash_x;
    out.no_x = no_x;
    out.y = static_cast<float>(modal_offset_y + kModalButtonOffsetY);
    return out;
}

}  // namespace chrome_text_logic
