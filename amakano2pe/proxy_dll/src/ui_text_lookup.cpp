// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "ui_text_lookup.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace translator_logic {
namespace {

constexpr std::string_view kDefaultSurnameJp = "和倉";
constexpr std::string_view kDefaultSurnameEn = "Wakura";

constexpr std::string_view kDefaultGivenNameJp = "賢一";
constexpr std::string_view kDefaultGivenNameEn = "Kenichi";

constexpr std::string_view kDefaultFullNameSpaceJp = "和倉 賢一";
constexpr std::string_view kDefaultFullNameIdeographicJp = "和倉\u3000賢一";
constexpr std::string_view kDefaultFullNameCompactJp = "和倉賢一";
constexpr std::string_view kDefaultFullNameEn = "Wakura Kenichi";

constexpr std::string_view kIdeographicSpaceUtf8 = "\u3000";
constexpr std::string_view kEngineNewlineLower = "\\n";
constexpr std::string_view kEngineNewlineUpper = "\\N";
constexpr std::size_t kSingleByteStep = 1;

constexpr std::string_view kChoiceBracketPrefixJp = "[選択肢]";
constexpr std::string_view kChoiceLenticularPrefixJp = "【選択肢】";
constexpr std::string_view kChoiceBracketPrefixEn = "[Choice]";

constexpr std::string_view kConfirmationPrefixEn = "Is ";
constexpr std::string_view kConfirmationSuffixEn = " correct?";

struct WrapperPair {
    std::string_view open;
    std::string_view close;
};

constexpr std::array<WrapperPair, 4> kBracketWrappers = {{
    {"「", "」"},
    {"『", "』"},
    {"【", "】"},
    {"\"", "\""},
}};

constexpr std::array<std::string_view, 4> kConfirmationLeadIns = {{
    "名前は",
    "お名前は",
    "主人公の名前は",
    "姓名は",
}};

constexpr std::array<std::string_view, 12> kConfirmationSuffixes = {{
    "さんでお間違いないですか？",
    "さんでお間違いないですか?",
    "さんでお間違いありませんか？",
    "さんでよろしいですか？",
    "さんでよろしいですか?",
    "でお間違いないですか？",
    "でお間違いないですか?",
    "でお間違いありませんか？",
    "でよろしいですか？",
    "でよろしいですか?",
    "に変更しますか？",
    "に設定しますか？",
}};

bool StartsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

std::string_view TrimLeadingWhitespaceAndBreaks(
    std::string_view text) noexcept {
    while (!text.empty()) {
        if (text.front() == ' ' || text.front() == '\t' ||
            text.front() == '\r' || text.front() == '\n') {
            text.remove_prefix(kSingleByteStep);
            continue;
        }
        if (StartsWith(text, kIdeographicSpaceUtf8)) {
            text.remove_prefix(kIdeographicSpaceUtf8.size());
            continue;
        }
        if (StartsWith(text, kEngineNewlineLower) ||
            StartsWith(text, kEngineNewlineUpper)) {
            text.remove_prefix(kEngineNewlineLower.size());
            continue;
        }
        break;
    }
    return text;
}

std::string_view TrimTrailingWhitespaceAndBreaks(
    std::string_view text) noexcept {
    while (!text.empty()) {
        if (text.back() == ' ' || text.back() == '\t' ||
            text.back() == '\r' || text.back() == '\n') {
            text.remove_suffix(kSingleByteStep);
            continue;
        }
        if (EndsWith(text, kIdeographicSpaceUtf8)) {
            text.remove_suffix(kIdeographicSpaceUtf8.size());
            continue;
        }
        if (EndsWith(text, kEngineNewlineLower) ||
            EndsWith(text, kEngineNewlineUpper)) {
            text.remove_suffix(kEngineNewlineLower.size());
            continue;
        }
        break;
    }
    return text;
}

std::string_view TrimWhitespaceAndBreaks(std::string_view text) noexcept {
    return TrimTrailingWhitespaceAndBreaks(
        TrimLeadingWhitespaceAndBreaks(text));
}

std::string_view StripSurroundingBrackets(std::string_view text) noexcept {
    text = TrimWhitespaceAndBreaks(text);
    for (const WrapperPair& pair : kBracketWrappers) {
        if (text.size() > pair.open.size() + pair.close.size() &&
            StartsWith(text, pair.open) && EndsWith(text, pair.close)) {
            text.remove_prefix(pair.open.size());
            text.remove_suffix(pair.close.size());
            return TrimWhitespaceAndBreaks(text);
        }
    }
    return text;
}

const std::unordered_map<std::string_view, std::string_view>&
StatelessUiLiteralTable() {
    static const std::unordered_map<std::string_view, std::string_view> kTable =
        {
            // Protagonist default name tokens (inputtxt.fes / namecheck.fes)
            {kDefaultSurnameJp, kDefaultSurnameEn},
            {kDefaultGivenNameJp, kDefaultGivenNameEn},
            {kDefaultFullNameSpaceJp, kDefaultFullNameEn},
            {kDefaultFullNameIdeographicJp, kDefaultFullNameEn},
            {kDefaultFullNameCompactJp, kDefaultFullNameEn},

            // Choice markers (adv.xml / main.kcs / backlog / save-load)
            {kChoiceBracketPrefixJp, kChoiceBracketPrefixEn},
            {kChoiceLenticularPrefixJp, kChoiceBracketPrefixEn},
            {"選択肢", "Choice"},

            // config/startup.xml <KEYCUSTOMIZE> action names (key00..key29)
            {"決定", "Confirm"},
            {"キャンセル", "Cancel"},
            {"文章を進める", "Advance Text"},
            {"メッセージウィンドウ消去", "Hide Message Window"},
            {"カーソル↑移動", "Move Cursor Up"},
            {"カーソル↓移動", "Move Cursor Down"},
            {"カーソル←移動", "Move Cursor Left"},
            {"カーソル→移動", "Move Cursor Right"},
            {"スクロール↑", "Scroll Up"},
            {"スクロール↓", "Scroll Down"},
            {"１ページ↑", "Page Up"},
            {"１ページ↓", "Page Down"},
            {"強制スキップ", "Force Skip"},
            {"既読スキップ", "Skip Read Text"},
            {"オートモード", "Auto Mode"},
            {"履歴ウィンドウ", "Backlog"},
            {"次の選択肢までスキップ", "Skip to Next Choice"},
            {"前の選択肢に戻る", "Return to Previous Choice"},
            {"音声再生", "Replay Voice"},
            {"セーブ画面", "Save Screen"},
            {"ロード画面", "Load Screen"},
            {"クイックセーブ", "Quick Save"},
            {"クイックロード", "Quick Load"},
            {"環境設定", "Settings"},
            {"オートスピードアップ", "Increase Auto Speed"},
            {"オートスピードダウン", "Decrease Auto Speed"},
            {"スクリーンモード変更", "Toggle Screen Mode"},
            {"ウィンドウ最小化", "Minimize Window"},
            {"ゲーム終了", "Exit Game"},
            {"おあずけモード切り替え", "Toggle Tease Mode"},

            // config/soundconf.xml sound group comments & part names
            {"マスターボリュームです", "Master volume."},
            {"BGMのボリュームです", "BGM volume."},
            {"効果音のボリュームです", "Sound effect volume."},
            {"ボイスのボリュームです", "Character voice volume."},
            {"システム効果音のボリュームです", "System sound effect volume."},
            {"環境音のボリュームです", "Ambient sound volume."},
            {"ＢＧＭ", "BGM"},
            {"効果音", "SE"},
            {"ループ効果音", "Loop SE"},
            {"システム音", "System SE"},
            {"キャラクタボイス", "Character Voice"},
            {"スピーチ", "Speech"},
            {"システムボイス", "System Voice"},
            {"システム効果音", "System SE"},
            {"音声", "Voice"},

            // Canonical character / voice names (config/soundconf.xml & glossary)
            {"黒姫 結灯", "Kurohime Yuuhi"},
            {"氷見山 玲", "Himiyama Rei"},
            {"蔦町 ちとせ", "Tsutamachi Chitose"},
            {"咲來", "Sara"},
            {"高社 紗雪", "Takashiro Sayuki"},
            {"上林 こはる", "Kanbayashi Koharu"},
            {"星川 こはる", "Hoshikawa Koharu"},
            {"一ノ瀬 穂波", "Ichinose Honami"},
            {"沓野 奏", "Kutsuno Kanade"},
            {"高社 雪静", "Takashiro Yuzuka"},
            {"硯川・e・涙香", "Suzurikawa E. Ruika"},
            {"倉持 聖", "Kuramochi Mizuki"},
            {"その他", "Other"},
            {"結灯", "Yuuhi"},
            {"ゆうひ", "Yuuhi"},
            {"玲", "Rei"},
            {"ちとせ", "Chitose"},
            {"紗雪", "Sayuki"},
            {"聖", "Mizuki"},
            {"こはる", "Koharu"},
            {"穂波", "Honami"},
            {"奏", "Kanade"},
            {"雪静", "Yuzuka"},
            {"涙香", "Ruika"},
            {"母さん", "Mom"},
            {"副部長", "Vice President"},
            {"女将さん", "Hostess"},
            {"寶泉路", "Housenji"},
            {"淵上", "Fuchinoe"},
            {"マスター", "Master"},
            {"放送", "Broadcast"},
            {"玲の父", "Rei's Father"},
            {"結灯の父", "Yuuhi's Father"},
            {"迷子の女の子", "Lost Girl"},
            {"プリント機", "Printer"},

            // Common Name-Input / Config sample & status strings
            {"苗字を入力", "Enter Last Name"},
            {"名前を入力", "Enter First Name"},
            {"確認", "Confirmation"},
            {"名前を入力してください。", "Please enter your name."},
            {"名前を入力してください", "Please enter your name."},
            {"主人公の名前を入力してください。", "Please enter the protagonist's name."},
            {"姓", "Last Name"},
            {"名", "First Name"},
            {"名前変更", "Change Name"},
            {"デフォルト", "Default"},
            {"初期値", "Default"},
            {"\\pcテキストサンプル", "\\pcSample Text"},
            {"テキストサンプル", "Sample Text"},
            {"サンプルテキスト", "Sample Text"},
            {"メッセージ速度のサンプルです。", "Sample text display speed."},
            {"文字表示速度のサンプルです。", "Sample text display speed."},
            {"オートモードの速度のサンプルです。", "Sample Auto Mode speed."},
            {"オートモード速度のサンプルです。", "Sample Auto Mode speed."},
        };
    return kTable;
}

std::optional<std::pair<std::string_view, std::string_view>>
SplitTwoPartName(std::string_view name) noexcept {
    const std::size_t ascii_space = name.find(' ');
    if (ascii_space != std::string_view::npos) {
        const std::string_view first =
            TrimWhitespaceAndBreaks(name.substr(0, ascii_space));
        const std::string_view second =
            TrimWhitespaceAndBreaks(name.substr(ascii_space + kSingleByteStep));
        if (!first.empty() && !second.empty()) {
            return std::make_pair(first, second);
        }
    }

    const std::size_t ideographic_space = name.find(kIdeographicSpaceUtf8);
    if (ideographic_space != std::string_view::npos) {
        const std::string_view first =
            TrimWhitespaceAndBreaks(name.substr(0, ideographic_space));
        const std::string_view second = TrimWhitespaceAndBreaks(
            name.substr(ideographic_space + kIdeographicSpaceUtf8.size()));
        if (!first.empty() && !second.empty()) {
            return std::make_pair(first, second);
        }
    }

    if (StartsWith(name, kDefaultSurnameJp) &&
        name.size() > kDefaultSurnameJp.size()) {
        const std::string_view rest =
            TrimWhitespaceAndBreaks(name.substr(kDefaultSurnameJp.size()));
        if (!rest.empty()) {
            return std::make_pair(kDefaultSurnameJp, rest);
        }
    }

    if (EndsWith(name, kDefaultGivenNameJp) &&
        name.size() > kDefaultGivenNameJp.size()) {
        const std::string_view first = TrimWhitespaceAndBreaks(
            name.substr(0, name.size() - kDefaultGivenNameJp.size()));
        if (!first.empty()) {
            return std::make_pair(first, kDefaultGivenNameJp);
        }
    }

    return std::nullopt;
}

std::optional<std::string> TryFormatChoicePrefixedUiText(
    std::string_view utf8_src) {
    std::string_view remainder;
    if (StartsWith(utf8_src, kChoiceBracketPrefixJp)) {
        remainder = utf8_src.substr(kChoiceBracketPrefixJp.size());
    } else if (StartsWith(utf8_src, kChoiceLenticularPrefixJp)) {
        remainder = utf8_src.substr(kChoiceLenticularPrefixJp.size());
    } else {
        return std::nullopt;
    }

    if (remainder.empty()) {
        return std::string(kChoiceBracketPrefixEn);
    }

    std::string out(kChoiceBracketPrefixEn);
    if (StartsWith(remainder, kIdeographicSpaceUtf8)) {
        out.push_back(' ');
        remainder.remove_prefix(kIdeographicSpaceUtf8.size());
    }
    out.append(remainder);
    return out;
}

}  // namespace

std::string TranslateProtagonistNameToken(std::string_view token) {
    const std::string_view trimmed = StripSurroundingBrackets(token);
    if (trimmed == kDefaultSurnameJp) {
        return std::string(kDefaultSurnameEn);
    }
    if (trimmed == kDefaultGivenNameJp) {
        return std::string(kDefaultGivenNameEn);
    }
    return std::string(trimmed);
}

std::string NormalizeProtagonistDisplayName(std::string_view raw_name) {
    const std::string_view cleaned = StripSurroundingBrackets(raw_name);
    if (cleaned.empty()) {
        return {};
    }
    if (cleaned == kDefaultFullNameSpaceJp ||
        cleaned == kDefaultFullNameIdeographicJp ||
        cleaned == kDefaultFullNameCompactJp) {
        return std::string(kDefaultFullNameEn);
    }

    const auto split = SplitTwoPartName(cleaned);
    if (!split.has_value()) {
        return TranslateProtagonistNameToken(cleaned);
    }

    const std::string surname_en = TranslateProtagonistNameToken(split->first);
    const std::string given_en = TranslateProtagonistNameToken(split->second);
    return surname_en + " " + given_en;
}

std::optional<std::string> TryFormatProtagonistConfirmationPrompt(
    std::string_view utf8_src) {
    const std::string_view trimmed = TrimWhitespaceAndBreaks(utf8_src);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    if (trimmed.size() >
            kConfirmationPrefixEn.size() + kConfirmationSuffixEn.size() &&
        StartsWith(trimmed, kConfirmationPrefixEn) &&
        EndsWith(trimmed, kConfirmationSuffixEn)) {
        const std::string_view inner_name = TrimWhitespaceAndBreaks(
            trimmed.substr(kConfirmationPrefixEn.size(),
                           trimmed.size() - kConfirmationPrefixEn.size() -
                               kConfirmationSuffixEn.size()));
        const std::string normalized_name =
            NormalizeProtagonistDisplayName(inner_name);
        if (!normalized_name.empty()) {
            std::string formatted;
            formatted.reserve(kConfirmationPrefixEn.size() +
                              normalized_name.size() +
                              kConfirmationSuffixEn.size());
            formatted.append(kConfirmationPrefixEn);
            formatted.append(normalized_name);
            formatted.append(kConfirmationSuffixEn);
            return formatted;
        }
    }

    for (const std::string_view suffix : kConfirmationSuffixes) {
        if (!EndsWith(trimmed, suffix)) {
            continue;
        }

        std::string_view name_part = TrimWhitespaceAndBreaks(
            trimmed.substr(0, trimmed.size() - suffix.size()));
        for (const std::string_view lead_in : kConfirmationLeadIns) {
            if (StartsWith(name_part, lead_in)) {
                name_part = TrimWhitespaceAndBreaks(
                    name_part.substr(lead_in.size()));
                break;
            }
        }

        const std::string normalized_name =
            NormalizeProtagonistDisplayName(name_part);
        if (normalized_name.empty()) {
            return std::nullopt;
        }

        std::string formatted;
        formatted.reserve(kConfirmationPrefixEn.size() +
                          normalized_name.size() +
                          kConfirmationSuffixEn.size());
        formatted.append(kConfirmationPrefixEn);
        formatted.append(normalized_name);
        formatted.append(kConfirmationSuffixEn);
        return formatted;
    }

    return std::nullopt;
}

std::optional<std::string> LookupStatelessUiText(std::string_view utf8_src) {
    if (utf8_src.empty()) {
        return std::nullopt;
    }

    const auto& table = StatelessUiLiteralTable();
    const auto it = table.find(utf8_src);
    if (it != table.end()) {
        return std::string(it->second);
    }

    if (const auto confirmation =
            TryFormatProtagonistConfirmationPrompt(utf8_src);
        confirmation.has_value()) {
        return confirmation;
    }

    if (const auto choice_prefixed = TryFormatChoicePrefixedUiText(utf8_src);
        choice_prefixed.has_value()) {
        return choice_prefixed;
    }

    return std::nullopt;
}

}  // namespace translator_logic
