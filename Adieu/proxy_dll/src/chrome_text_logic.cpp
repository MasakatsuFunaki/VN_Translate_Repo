// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "chrome_text_logic.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace chrome_text {

namespace {

constexpr uint16_t kAsciiUpperExclusive = 0x0080u;
constexpr std::size_t kMnemonicSuffixLen = 4u;
constexpr std::size_t kInitialBucketCount = 256u;
constexpr std::size_t kEmptyBucketSlot = 0u;

// Ordered longest-first so full-title forms match before the bare Japanese stem.
constexpr std::wstring_view kJapaneseWindowTitlePrefixes[] = {
    L"さよならを教えて \uFF5Ecomment te dire adieu\uFF5E",
    L"さよならを教えて \u301Ccomment te dire adieu\u301C",
    L"さよならを教えて\uFF5Ecomment te dire adieu\uFF5E",
    L"さよならを教えて\u301Ccomment te dire adieu\u301C",
    L"さよならを教えて ~comment te dire adieu~",
    L"さよならを教えて",
};

struct StaticChromePair {
    std::wstring_view jp;
    std::wstring_view en;
};

constexpr StaticChromePair kDefaultChromePairs[] = {
    // 1. Gameexe.dat confirmation prompts (MessageBoxW)
    {L"最初から始めてもよろしいですか？", L"Start from the beginning?"},
    {L"タイトルに戻ってもよろしいですか？", L"Return to the title screen?"},
    {L"前の選択肢に戻ってもよろしいですか？",
     L"Return to the previous choice?"},
    {L"終了してもよろしいですか？", L"Are you sure you want to quit?"},
    {L"ゲームを終了してもよろしいですか？", L"Are you sure you want to quit?"},
    {L"セーブしてもよろしいですか？", L"Save the game?"},
    {L"クイックセーブしてもよろしいですか？", L"Quick save the game?"},
    {L"ロードしてもよろしいですか？", L"Load this save data?"},
    {L"クイックロードしてもよろしいですか？", L"Quick load save data?"},
    {L"上書きしてもよろしいですか？", L"Overwrite this save data?"},
    {L"データを削除してもよろしいですか？", L"Delete this save data?"},
    {L"設定を初期値に戻してもよろしいですか？",
     L"Reset all settings to default?"},

    // 2. Gameexe.dat #SYSCOMMENU.* right-click popup menu items
    {L"既読文章を早送り", L"Skip Read Text"},
    {L"文章を早送り", L"Fast Forward"},
    {L"自動早送り", L"Auto Fast Forward"},
    {L"オートモード", L"Auto Mode"},
    {L"ウィンドウを消す", L"Hide Window"},
    {L"セーブ", L"Save"},
    {L"ロード", L"Load"},
    {L"クイックセーブ", L"Quick Save"},
    {L"クイックロード", L"Quick Load"},
    {L"前の選択肢に戻る", L"Previous Choice"},
    {L"環境設定", L"Configuration"},
    {L"タイトルに戻る", L"Return to Title"},
    {L"ゲームを終了する", L"Quit Game"},
    {L"キャンセル", L"Cancel"},
    {L"メッセージ履歴", L"Backlog"},

    // 3. Gameexe.dat #DIALOG.* checkbox labels
    {L"文章を色分けする。", L"Color-code dialogue by character."},
    {L"本プログラムの動作を遅くして、他のプログラムがスムーズに動作するようにする。",
     L"Reduce CPU usage so other programs run smoothly."},
    {L"画面暗転効果のアニメを無効にする。",
     L"Disable screen transition animations."},
    {L"画面暗転効果をマウスクリックで飛ばす。",
     L"Skip screen transitions on mouse click."},
    {L"メッセージウィンドウの開閉時のアニメを無効にする。",
     L"Disable message window open/close animations."},
    {L"マウスのホイールボタンの下回しで文章を読み進める。",
     L"Advance text by scrolling the mouse wheel down."},
    {L"声の再生中に次の文章に進んでも再生を続ける。",
     L"Continue playing voice when advancing to the next line."},
    {L"未読の文章も早送りできるようにする。",
     L"Allow skipping unread text."},
    {L"サウンド再生時に雑音が入る場合はチェックして下さい。",
     L"Check this if audio playback crackles."},

    // 4. Gameexe.dat #SAVE* / #LOAD* & Dialog #146 Save/Load controls
    {L"セーブ／ロード", L"Save / Load"},
    {L"データがありません。", L"No Data"},
    {L"データがありません", L"No Data"},
    {L"セーブする", L"Save"},
    {L"セーブする場所を選択してください。", L"Select a slot to save."},
    {L"セーブする前に上書きの確認メッセージを表示する。",
     L"Confirm before overwriting existing save data."},
    {L"リストをダブルクリックでセーブする。",
     L"Double-click a slot to save."},
    {L"ロードする", L"Load"},
    {L"ロードするデータを選択してください。", L"Select save data to load."},
    {L"ロードする前に確認メッセージを表示する。",
     L"Confirm before loading save data."},
    {L"リストをダブルクリックでロードする。",
     L"Double-click a slot to load."},

    // 5. Dialog #146 SysListView32 column headers
    {L"番号", L"No."},
    {L"日付", L"Date"},
    {L"日時", L"Date"},
    {L"タイトル", L"Scene"},
    {L"コメント", L"Comment"},
    {L"テキスト", L"Text"},

    // 6. Native SiglusEngine Win32 dialog (#101-#150) captions, tabs, & buttons
    {L"初期値に戻す", L"Reset to Default"},
    {L"このページを初期値に戻す", L"Reset Page to Default"},
    {L"すべて初期値に戻す", L"Reset All to Default"},
    {L"決定", L"Confirm"},
    {L"閉じる", L"Close"},
    {L"適用", L"Apply"},
    {L"はい", L"Yes"},
    {L"いいえ", L"No"},
    {L"削除", L"Delete"},
    {L"編集", L"Edit"},
    {L"画面", L"Screen"},
    {L"画面設定", L"Screen Settings"},
    {L"画面モード", L"Display Mode"},
    {L"ウィンドウ", L"Windowed"},
    {L"ウィンドウモード", L"Windowed Mode"},
    {L"フルスクリーン", L"Fullscreen"},
    {L"フルスクリーンモード", L"Fullscreen Mode"},
    {L"解像度", L"Resolution"},
    {L"フォント", L"Font"},
    {L"フォント設定", L"Font Settings"},
    {L"プロポーショナルフォントを表示する", L"Show proportional fonts"},
    {L"影をつける", L"Draw text shadow"},
    {L"縁取りをつける", L"Draw text outline"},
    {L"太字にする", L"Bold text"},
    {L"文字表示", L"Text Display"},
    {L"文字表示速度", L"Text Speed"},
    {L"ノーウェイト", L"Instant"},
    {L"速い", L"Fast"},
    {L"普通", L"Normal"},
    {L"遅い", L"Slow"},
    {L"音量", L"Volume"},
    {L"音量設定", L"Volume Settings"},
    {L"全体音量", L"Master Volume"},
    {L"全体", L"Master"},
    {L"ＢＧＭ", L"BGM"},
    {L"効果音", L"SE"},
    {L"ＰＣＭ", L"SE"},
    {L"システム音", L"System SE"},
    {L"声", L"Voice"},
    {L"音声", L"Voice"},
    {L"ムービー", L"Movie"},
    {L"消音", L"Mute"},
    {L"キャラクター別の音量", L"Character Voice Volume"},
    {L"キャラクター音量", L"Character Volume"},
    {L"システム", L"System"},
    {L"システム設定", L"System Settings"},
    {L"操作", L"Controls"},
    {L"操作設定", L"Control Settings"},
    {L"ウィンドウの透明度", L"Window Opacity"},
    {L"名前入力", L"Enter Name"},
    {L"主人公の名前を入力してください。", L"Enter the protagonist's name."},
    {L"姓", L"Surname"},
    {L"名", L"Given Name"},
    {L"バージョン情報", L"About"},

    // 7. Unknown speaker placeholder (#CHRKOE.NOT_LOOK_NAME_STR)
    {L"？？？", L"???"},
};

bool IsAllAscii(std::wstring_view text) noexcept {
    for (const wchar_t ch : text) {
        if (static_cast<uint16_t>(ch) >= kAsciiUpperExclusive) {
            return false;
        }
    }
    return true;
}

bool IsAsciiAlphaNum(wchar_t ch) noexcept {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
           (ch >= L'0' && ch <= L'9');
}

// Splits a trailing Win32 mnemonic suffix like `(&C)` or ` (&O)` from `head`.
bool SplitWin32MnemonicSuffix(std::wstring_view head,
                              std::wstring_view& out_stem,
                              std::wstring_view& out_mnemonic) noexcept {
    if (head.size() <= kMnemonicSuffixLen) {
        return false;
    }
    const std::size_t paren_pos = head.size() - kMnemonicSuffixLen;
    if (head[paren_pos] != L'(' || head[paren_pos + 1u] != L'&' ||
        !IsAsciiAlphaNum(head[paren_pos + 2u]) ||
        head[paren_pos + 3u] != L')') {
        return false;
    }

    std::size_t stem_end = paren_pos;
    if (stem_end > 0u && head[stem_end - 1u] == L' ') {
        --stem_end;
    }
    if (stem_end == 0u) {
        return false;
    }

    out_stem = head.substr(0u, stem_end);
    out_mnemonic = head.substr(stem_end);
    return true;
}

}  // namespace

struct ChromeDictionary::Impl {
    struct Entry {
        std::wstring jp;
        std::wstring en;
    };

    std::vector<Entry> entries;
    std::vector<std::size_t> buckets;

    Impl() {
        entries.reserve(std::size(kDefaultChromePairs));
        Rehash(kInitialBucketCount);
        for (const auto& pair : kDefaultChromePairs) {
            InsertFirstWins(pair.jp, pair.en);
        }
    }

    void Rehash(std::size_t desired_buckets) {
        std::size_t cap = kInitialBucketCount;
        while (cap < desired_buckets) {
            cap *= 2u;
        }
        buckets.assign(cap, kEmptyBucketSlot);
        const std::size_t mask = cap - 1u;
        std::hash<std::wstring_view> hasher;

        for (std::size_t i = 0u; i < entries.size(); ++i) {
            std::size_t slot = hasher(entries[i].jp) & mask;
            while (buckets[slot] != kEmptyBucketSlot) {
                slot = (slot + 1u) & mask;
            }
            buckets[slot] = i + 1u;
        }
    }

    void InsertFirstWins(std::wstring_view jp, std::wstring_view en) {
        if ((entries.size() + 1u) * 2u > buckets.size()) {
            Rehash(buckets.size() * 2u);
        }
        const std::size_t mask = buckets.size() - 1u;
        std::size_t slot = std::hash<std::wstring_view>{}(jp) & mask;

        for (;;) {
            const std::size_t encoded = buckets[slot];
            if (encoded == kEmptyBucketSlot) {
                entries.push_back(Entry{std::wstring(jp), std::wstring(en)});
                buckets[slot] = entries.size();
                return;
            }
            if (entries[encoded - 1u].jp == jp) {
                return;
            }
            slot = (slot + 1u) & mask;
        }
    }

    const std::wstring* FindExact(std::wstring_view jp) const noexcept {
        if (entries.empty() || buckets.empty()) {
            return nullptr;
        }
        const std::size_t mask = buckets.size() - 1u;
        std::size_t slot = std::hash<std::wstring_view>{}(jp) & mask;

        for (;;) {
            const std::size_t encoded = buckets[slot];
            if (encoded == kEmptyBucketSlot) {
                return nullptr;
            }
            const Entry& candidate = entries[encoded - 1u];
            if (candidate.jp == jp) {
                return &candidate.en;
            }
            slot = (slot + 1u) & mask;
        }
    }
};

ChromeDictionary::ChromeDictionary() : impl_(std::make_unique<Impl>()) {}

ChromeDictionary::ChromeDictionary(const ChromeDictionary& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

ChromeDictionary::ChromeDictionary(ChromeDictionary&& other) noexcept = default;

ChromeDictionary& ChromeDictionary::operator=(const ChromeDictionary& other) {
    if (this != &other) {
        *impl_ = *other.impl_;
    }
    return *this;
}

ChromeDictionary& ChromeDictionary::operator=(
    ChromeDictionary&& other) noexcept = default;

ChromeDictionary::~ChromeDictionary() = default;

std::optional<std::wstring> ChromeDictionary::TranslateWindowTitle(
    std::wstring_view title) const {
    if (title.empty()) {
        return std::nullopt;
    }
    if (title.size() >= kEnglishWindowTitle.size() &&
        title.substr(0u, kEnglishWindowTitle.size()) == kEnglishWindowTitle) {
        return std::nullopt;
    }

    for (const std::wstring_view prefix : kJapaneseWindowTitlePrefixes) {
        if (title.size() >= prefix.size() &&
            title.substr(0u, prefix.size()) == prefix) {
            std::wstring translated(kEnglishWindowTitle);
            translated.append(title.substr(prefix.size()));
            return translated;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> ChromeDictionary::TranslateControlOrMenuText(
    std::wstring_view text) const {
    if (text.empty() || IsAllAscii(text)) {
        return std::nullopt;
    }

    if (const std::wstring* exact = impl_->FindExact(text)) {
        return *exact;
    }

    // Preserve Win32 menu tab accelerators (`\tCtrl+S`) and/or mnemonic tails
    // (`(&C)`, ` (&O)`).
    std::wstring_view head = text;
    std::wstring_view tab_tail;
    const std::size_t tab_pos = text.find(L'\t');
    if (tab_pos != std::wstring_view::npos) {
        head = text.substr(0u, tab_pos);
        tab_tail = text.substr(tab_pos);
        if (const std::wstring* hit = impl_->FindExact(head)) {
            std::wstring combined(*hit);
            combined.append(tab_tail);
            return combined;
        }
    }

    std::wstring_view stem;
    std::wstring_view mnemonic_tail;
    if (SplitWin32MnemonicSuffix(head, stem, mnemonic_tail)) {
        if (const std::wstring* hit = impl_->FindExact(stem)) {
            std::wstring combined(*hit);
            combined.append(mnemonic_tail);
            combined.append(tab_tail);
            return combined;
        }
    }

    return std::nullopt;
}

std::size_t ChromeDictionary::Size() const noexcept {
    return impl_->entries.size();
}

const ChromeDictionary& DefaultChromeDictionary() {
    static const ChromeDictionary kDict;
    return kDict;
}

std::optional<std::wstring> TranslateWindowTitle(std::wstring_view title) {
    return DefaultChromeDictionary().TranslateWindowTitle(title);
}

std::optional<std::wstring> TranslateChromeWide(std::wstring_view text) {
    return DefaultChromeDictionary().TranslateControlOrMenuText(text);
}

}  // namespace chrome_text
