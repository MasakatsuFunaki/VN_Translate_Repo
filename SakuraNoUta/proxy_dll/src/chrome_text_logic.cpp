// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "chrome_text_logic.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace chrome_text {
namespace {

constexpr std::uint16_t kAsciiUpperExclusive = 0x0080u;
constexpr std::size_t kMnemonicSuffixLen = 4u;
constexpr std::size_t kInitialBucketCount = 256u;
constexpr std::size_t kEmptyBucketSlot = 0u;
constexpr std::size_t kBucketLoadFactorMultiplier = 2u;
constexpr std::size_t kSecondsSuffixReserveChars = 2u;

// Ordered longest-first so full-title subtitle forms match before bare stems.
constexpr std::wstring_view kJapaneseWindowTitlePrefixes[] = {
    L"サクラノ詩 －櫻の森の上を舞う－",
    L"サクラノ詩 ―櫻の森の上を舞う―",
    L"サクラノ詩 —櫻の森の上を舞う—",
    L"サクラノ詩 -櫻の森の上を舞う-",
    L"サクラノ詩－櫻の森の上を舞う－",
    L"サクラノ詩―櫻の森の上を舞う―",
    L"サクラノ詩—櫻の森の上を舞う—",
    L"サクラノ詩-櫻の森の上を舞う-",
    L"桜の詩 －櫻の森の上を舞う－",
    L"桜の詩 ―櫻の森の上を舞う―",
    L"桜の詩 —櫻の森の上を舞う—",
    L"桜の詩 -櫻の森の上を舞う-",
    L"桜の詩－櫻の森の上を舞う－",
    L"サクラノ詩",
    L"桜の詩",
};

struct StaticChromePair {
    std::wstring_view jp;
    std::wstring_view en;
};

constexpr StaticChromePair kDefaultChromePairs[] = {
    // 1. BGI.exe RT_DIALOG id=127 (Configuration)
    {L"環境設定", L"Configuration"},
    {L"閉じる", L"Close"},
    {L"ウィンドウ", L"Windowed"},
    {L"フルスクリーン", L"Fullscreen"},
    {L"既読のみ", L"Read Only"},
    {L"全て", L"All Text"},
    {L"すべて", L"All Text"},
    {L"通常遷移", L"Normal"},
    {L"瞬間遷移", L"Instant"},
    {L"する", L"On"},
    {L"しない", L"Off"},
    {L"文字表示速度", L"Text Speed"},
    {L"オートモード速度", L"Auto-Read Speed"},
    {L"ＢＧＭ", L"BGM"},
    {L"効果音", L"SE"},
    {L"音声", L"Voice"},
    {L"表示スタイル", L"Display Mode"},
    {L"スキップの動作", L"Skip Mode"},
    {L"画面切り替え", L"Screen Transitions"},
    {L"改ページ時の音声停止", L"Stop Voice on Click"},

    // 2. BGI.exe RT_DIALOG id=119 (Name / Birthday Input)
    {L"名前／誕生日を入力して下さい", L"Please enter your name / birthday."},
    {L"名前／誕生日を入力してください", L"Please enter your name / birthday."},
    {L"姓", L"Surname"},
    {L"名", L"Given Name"},
    {L"一人称", L"First-Person Pronoun"},
    {L"誕生日", L"Birthday"},
    {L"月", L"Month"},
    {L"日", L"Day"},
    {L"ニックネーム", L"Nickname"},

    // 3. BGI.exe RT_DIALOG id=121/125 (Product Key) & id=108..111 (Installer)
    {L"プロダクトキーを入力してください", L"Please enter your product key."},
    {L"プロダクトキーを入力して下さい", L"Please enter your product key."},
    {L"ｷｬﾝｾﾙ", L"Cancel"},
    {L"キャンセル", L"Cancel"},
    {L"中止", L"Cancel"},
    {L"インストールフォルダ", L"Destination Folder"},
    {L"参照", L"Browse..."},
    {L"ショートカット", L"Shortcut"},
    {L"プログラムフォルダにショートカットを作成する",
     L"Create Start Menu entry"},
    {L"デスクトップにショートカットを作成する", L"Create Desktop icon"},
    {L"次へ", L"Next"},
    {L"インストール", L"Install"},
    {L"インストール構成", L"Installation Type"},
    {L"最小", L"Minimum"},
    {L"標準", L"Standard"},
    {L"最大", L"Full"},
    {L"説明文", L"Description"},
    {L"戻る", L"Back"},

    // 4. yesnownd._bp confirmation prompts & system menu items
    {L"終了しますか？", L"Exit the game?"},
    {L"タイトル画面へ戻りますか？", L"Return to the title screen?"},
    {L"セーブを行いますか？", L"Save your progress?"},
    {L"データを上書きしますか？", L"Overwrite this save data?"},
    {L"データを読み込みますか？", L"Load this save data?"},
    {L"セーブデータを消去しますか？", L"Delete this save data?"},
    {L"ゲームに戻りますか？", L"Return to the game?"},
    {L"クイックセーブを行いました。", L"Quick save complete."},
    {L"クイックセーブしました。", L"Quick save complete."},
    {L"クイックロードしますか？", L"Quick load save data?"},
    {L"選択肢", L"Choice"},
    {L"はい", L"Yes"},
    {L"いいえ", L"No"},
    {L"決定", L"Confirm"},
    {L"セーブ", L"Save"},
    {L"ロード", L"Load"},
    {L"クイックセーブ", L"Quick Save"},
    {L"クイックロード", L"Quick Load"},
    {L"コンフィグ", L"Config"},
    {L"タイトルへ戻る", L"Return to Title"},
    {L"タイトルに戻る", L"Return to Title"},
    {L"ゲーム終了", L"Exit"},
    {L"終了", L"Exit"},
    {L"メッセージ履歴", L"Backlog"},
    {L"バックログ", L"Backlog"},
    {L"ウィンドウを消す", L"Hide Window"},
    {L"オートモード", L"Auto Mode"},
    {L"スキップ", L"Skip"},
    {L"フォントを選択してください", L"Please select a font."},
    {L"入力エラー", L"Input Error"},

    // 5. BGI.exe .rdata DirectSound / Direct3D / Win32 startup error strings
    {L"DirectSoundの初期化に失敗しました。",
     L"Failed to initialize DirectSound."},
    {L"DirectSoundの初期化に失敗しました",
     L"Failed to initialize DirectSound."},
    {L"DirectSoundの強調レベルの設定に失敗しました。",
     L"Failed to set DirectSound cooperative level."},
    {L"DirectSoundの強調レベルの設定に失敗しました",
     L"Failed to set DirectSound cooperative level."},
    {L"DirectSoundの協調レベルの設定に失敗しました。",
     L"Failed to set DirectSound cooperative level."},
    {L"プライマリバッファの生成に失敗しました。",
     L"Failed to create DirectSound primary buffer."},
    {L"プライマリバッファの生成に失敗しました",
     L"Failed to create DirectSound primary buffer."},
    {L"メインバッファの生成に失敗しました。",
     L"Failed to create DirectSound main buffer."},
    {L"メインバッファの生成に失敗しました",
     L"Failed to create DirectSound main buffer."},
    {L"Direct3Dの初期化に失敗しました", L"Failed to initialize Direct3D."},
    {L"Direct3Dの初期化に失敗しました。", L"Failed to initialize Direct3D."},
    {L"デスクトップ解像度の取得に失敗しました",
     L"Failed to query desktop resolution."},
    {L"ディスプレイアダプタの情報の取得に失敗しました",
     L"Failed to query display adapter information."},
    {L"Direct3Dデバイスが作成できませんでした",
     L"Failed to create Direct3D device."},
    {L"Direct3Dリソースが確保できませんでした",
     L"Failed to allocate Direct3D resources."},
    {L"メインウィンドウクラスの登録に失敗しました",
     L"Failed to register main window class."},
    {L"チャイルドウィンドウクラスの登録に失敗しました",
     L"Failed to register child window class."},
    {L"メインウィンドウの生成に失敗しました",
     L"Failed to create main window."},
    {L"何らかの要因によりメモリが確保できません", L"Out of memory."},
    {L"読み込みエラーが発生しました", L"A read error occurred."},
    {L"書き込みエラーが発生しました", L"A write error occurred."},
    {L"ファイルの作成に失敗しました", L"Failed to create file."},
};

bool HasInteriorNul(std::wstring_view text) noexcept {
    return text.find(L'\0') != std::wstring_view::npos;
}

bool IsAllAscii(std::wstring_view text) noexcept {
    for (const wchar_t ch : text) {
        if (static_cast<std::uint16_t>(ch) >= kAsciiUpperExclusive) {
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

std::optional<std::wstring> TryTranslateFormattedDirectSoundError(
    std::wstring_view text) {
    constexpr std::wstring_view kJpPrimaryBufPrefix =
        L"プライマリバッファの生成に失敗しました。result[";
    constexpr std::wstring_view kEnPrimaryBufPrefix =
        L"Failed to create DirectSound primary buffer. result[";

    if (text.size() <= kJpPrimaryBufPrefix.size() ||
        text.substr(0u, kJpPrimaryBufPrefix.size()) != kJpPrimaryBufPrefix) {
        return std::nullopt;
    }

    std::wstring translated(kEnPrimaryBufPrefix);
    translated.append(text.substr(kJpPrimaryBufPrefix.size()));
    return translated;
}

std::optional<std::wstring> TryTranslateSecondsReadout(
    std::wstring_view text) {
    constexpr std::wstring_view kSpacedSecSuffix = L" 秒";
    constexpr std::wstring_view kBareSecSuffix = L"秒";

    std::wstring_view num_part;
    if (text.size() > kSpacedSecSuffix.size() &&
        text.substr(text.size() - kSpacedSecSuffix.size()) ==
            kSpacedSecSuffix) {
        num_part = text.substr(0u, text.size() - kSpacedSecSuffix.size());
    } else if (text.size() > kBareSecSuffix.size() &&
               text.substr(text.size() - kBareSecSuffix.size()) ==
                   kBareSecSuffix) {
        num_part = text.substr(0u, text.size() - kBareSecSuffix.size());
    } else {
        return std::nullopt;
    }

    std::wstring normalized;
    normalized.reserve(num_part.size() + kSecondsSuffixReserveChars);
    bool saw_digit_before_dot = false;
    bool saw_decimal = false;
    bool saw_digit_after_dot = false;

    for (const wchar_t ch : num_part) {
        if (ch >= L'0' && ch <= L'9') {
            normalized.push_back(ch);
            if (saw_decimal) {
                saw_digit_after_dot = true;
            } else {
                saw_digit_before_dot = true;
            }
            continue;
        }
        if (ch >= L'０' && ch <= L'９') {
            normalized.push_back(static_cast<wchar_t>(L'0' + (ch - L'０')));
            if (saw_decimal) {
                saw_digit_after_dot = true;
            } else {
                saw_digit_before_dot = true;
            }
            continue;
        }
        if ((ch == L'.' || ch == L'．') && saw_digit_before_dot &&
            !saw_decimal) {
            normalized.push_back(L'.');
            saw_decimal = true;
            continue;
        }
        return std::nullopt;
    }

    if (!saw_digit_before_dot || (saw_decimal && !saw_digit_after_dot)) {
        return std::nullopt;
    }
    normalized.append(L" s");
    return normalized;
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
            cap *= kBucketLoadFactorMultiplier;
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
        if ((entries.size() + 1u) * kBucketLoadFactorMultiplier >
            buckets.size()) {
            Rehash(buckets.size() * kBucketLoadFactorMultiplier);
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
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

ChromeDictionary::ChromeDictionary(ChromeDictionary&& other) noexcept = default;

ChromeDictionary& ChromeDictionary::operator=(const ChromeDictionary& other) {
    if (this == &other) {
        return *this;
    }
    if (!other.impl_) {
        impl_.reset();
        return *this;
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>(*other.impl_);
        return *this;
    }
    *impl_ = *other.impl_;
    return *this;
}

ChromeDictionary& ChromeDictionary::operator=(
    ChromeDictionary&& other) noexcept = default;

ChromeDictionary::~ChromeDictionary() = default;

std::optional<std::wstring> ChromeDictionary::TranslateWindowTitle(
    std::wstring_view title) const {
    if (title.empty() || HasInteriorNul(title)) {
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
    if (!impl_ || text.empty() || HasInteriorNul(text) || IsAllAscii(text)) {
        return std::nullopt;
    }

    if (const std::wstring* exact = impl_->FindExact(text)) {
        return *exact;
    }

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

    if (auto ds_err = TryTranslateFormattedDirectSoundError(text)) {
        return ds_err;
    }

    if (auto seconds = TryTranslateSecondsReadout(text)) {
        return seconds;
    }

    return std::nullopt;
}

std::size_t ChromeDictionary::Size() const noexcept {
    return impl_ ? impl_->entries.size() : 0u;
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
