// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace translator_logic {

namespace {

constexpr uint8_t kUtf8AsciiMax = 0x7Fu;
constexpr uint8_t kUtf8ContMask = 0xC0u;
constexpr uint8_t kUtf8ContTag = 0x80u;
constexpr uint8_t kUtf8Payload6Mask = 0x3Fu;
constexpr uint8_t kUtf8Lead2Min = 0xC2u;
constexpr uint8_t kUtf8Lead2Max = 0xDFu;
constexpr uint8_t kUtf8Lead3Min = 0xE0u;
constexpr uint8_t kUtf8Lead3Max = 0xEFu;
constexpr uint8_t kUtf8Lead4Min = 0xF0u;
constexpr uint8_t kUtf8Lead4Max = 0xF4u;

constexpr uint32_t kUnicodeSurrogateMin = 0xD800u;
constexpr uint32_t kUnicodeHighSurrogateMax = 0xDBFFu;
constexpr uint32_t kUnicodeLowSurrogateMin = 0xDC00u;
constexpr uint32_t kUnicodeSurrogateMax = 0xDFFFu;
constexpr uint32_t kUnicodeBmpMax = 0xFFFFu;
constexpr uint32_t kUnicodeMaxCodePoint = 0x10FFFFu;
constexpr uint32_t kUnicodeSupplementalBase = 0x10000u;
constexpr unsigned int kSurrogateShift = 10u;
constexpr uint32_t kSurrogateLowMask = 0x3FFu;

constexpr uint32_t kUtf8TwoByteMax = 0x7FFu;
constexpr uint16_t kAsciiUpperExclusive = 0x0080u;

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
constexpr std::wstring_view kNamaeMacroA = L"＊Ａ";
constexpr std::wstring_view kNamaeMacroB = L"＊Ｂ";

constexpr std::size_t kInitialBucketCount = 64u;
constexpr std::size_t kEmptyBucketSlot = 0u;

bool IsContinuationByte(uint8_t b) noexcept {
    return (b & kUtf8ContMask) == kUtf8ContTag;
}

template <typename StringT>
void UnescapeStringImpl(StringT& s) {
    using CharT = typename StringT::value_type;
    std::size_t write_pos = 0u;
    const std::size_t len = s.size();

    for (std::size_t read_pos = 0u; read_pos < len; ++read_pos) {
        const CharT ch = s[read_pos];
        if (ch == static_cast<CharT>('\\') && read_pos + 1u < len) {
            const CharT next = s[read_pos + 1u];
            if (next == static_cast<CharT>('n')) {
                s[write_pos++] = static_cast<CharT>('\n');
                ++read_pos;
                continue;
            }
            if (next == static_cast<CharT>('r')) {
                s[write_pos++] = static_cast<CharT>('\r');
                ++read_pos;
                continue;
            }
            if (next == static_cast<CharT>('t')) {
                s[write_pos++] = static_cast<CharT>('\t');
                ++read_pos;
                continue;
            }
            if (next == static_cast<CharT>('\\')) {
                s[write_pos++] = static_cast<CharT>('\\');
                ++read_pos;
                continue;
            }
        }
        s[write_pos++] = ch;
    }
    s.resize(write_pos);
}

bool IsLeadingWhitespace(wchar_t ch) noexcept {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' ||
           ch == L'　';
}

bool IsOpeningDialogueQuote(wchar_t ch) noexcept {
    return ch == L'"' || ch == L'\'' || ch == L'“' || ch == L'‘' ||
           ch == L'「' || ch == L'『';
}

void WrapSingleParagraph(std::wstring_view seg,
                         std::size_t max_cols,
                         std::wstring& out) {
    if (seg.size() <= max_cols) {
        out.append(seg);
        return;
    }

    const std::size_t len = seg.size();
    std::size_t pos = 0u;
    bool first_piece = true;

    while (pos < len) {
        if (!first_piece) {
            out.push_back(L'\n');
        }
        first_piece = false;

        if (len - pos <= max_cols) {
            out.append(seg.substr(pos));
            break;
        }

        // Find the rightmost ASCII space within [pos, pos + max_cols] that
        // leaves at least one non-space character on the current line.
        std::size_t break_space = std::wstring_view::npos;
        for (std::size_t idx = pos + max_cols; idx > pos; --idx) {
            if (seg[idx] == L' ') {
                break_space = idx;
                break;
            }
        }

        if (break_space != std::wstring_view::npos) {
            std::size_t line_end = break_space;
            while (line_end > pos && seg[line_end - 1u] == L' ') {
                --line_end;
            }
            if (line_end > pos) {
                out.append(seg.substr(pos, line_end - pos));
                pos = break_space + 1u;
                while (pos < len && seg[pos] == L' ') {
                    ++pos;
                }
                continue;
            }
        }

        // No word boundary available inside the active column budget:
        // hard-split an overlong unbroken token cleanly at `max_cols`.
        out.append(seg.substr(pos, max_cols));
        pos += max_cols;
        while (pos < len && seg[pos] == L' ') {
            ++pos;
        }
    }
}

}  // namespace

bool Utf8ToUtf16(std::string_view utf8, std::wstring& out) {
    out.clear();
    out.reserve(utf8.size());

    const std::size_t len = utf8.size();
    std::size_t i = 0u;

    while (i < len) {
        const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        if (b0 <= kUtf8AsciiMax) {
            out.push_back(static_cast<wchar_t>(b0));
            ++i;
            continue;
        }

        if (b0 >= kUtf8Lead2Min && b0 <= kUtf8Lead2Max) {
            if (i + 1u >= len) {
                out.clear();
                return false;
            }
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1u]);
            if (!IsContinuationByte(b1)) {
                out.clear();
                return false;
            }
            const uint32_t cp =
                ((static_cast<uint32_t>(b0 & 0x1Fu) << 6u) |
                 static_cast<uint32_t>(b1 & kUtf8Payload6Mask));
            out.push_back(static_cast<wchar_t>(cp));
            i += 2u;
            continue;
        }

        if (b0 >= kUtf8Lead3Min && b0 <= kUtf8Lead3Max) {
            if (i + 2u >= len) {
                out.clear();
                return false;
            }
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1u]);
            const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2u]);
            if (!IsContinuationByte(b1) || !IsContinuationByte(b2)) {
                out.clear();
                return false;
            }
            if ((b0 == 0xE0u && b1 < 0xA0u) || (b0 == 0xEDu && b1 >= 0xA0u)) {
                out.clear();
                return false;
            }
            const uint32_t cp =
                (static_cast<uint32_t>(b0 & 0x0Fu) << 12u) |
                (static_cast<uint32_t>(b1 & kUtf8Payload6Mask) << 6u) |
                static_cast<uint32_t>(b2 & kUtf8Payload6Mask);
            out.push_back(static_cast<wchar_t>(cp));
            i += 3u;
            continue;
        }

        if (b0 >= kUtf8Lead4Min && b0 <= kUtf8Lead4Max) {
            if (i + 3u >= len) {
                out.clear();
                return false;
            }
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1u]);
            const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2u]);
            const uint8_t b3 = static_cast<uint8_t>(utf8[i + 3u]);
            if (!IsContinuationByte(b1) || !IsContinuationByte(b2) ||
                !IsContinuationByte(b3)) {
                out.clear();
                return false;
            }
            if ((b0 == 0xF0u && b1 < 0x90u) || (b0 == 0xF4u && b1 >= 0x90u)) {
                out.clear();
                return false;
            }
            const uint32_t cp =
                (static_cast<uint32_t>(b0 & 0x07u) << 18u) |
                (static_cast<uint32_t>(b1 & kUtf8Payload6Mask) << 12u) |
                (static_cast<uint32_t>(b2 & kUtf8Payload6Mask) << 6u) |
                static_cast<uint32_t>(b3 & kUtf8Payload6Mask);
            if (cp > kUnicodeMaxCodePoint) {
                out.clear();
                return false;
            }
            const uint32_t adj = cp - kUnicodeSupplementalBase;
            out.push_back(static_cast<wchar_t>(
                kUnicodeSurrogateMin + (adj >> kSurrogateShift)));
            out.push_back(static_cast<wchar_t>(
                kUnicodeLowSurrogateMin + (adj & kSurrogateLowMask)));
            i += 4u;
            continue;
        }

        out.clear();
        return false;
    }

    return true;
}

bool Utf16ToUtf8(std::wstring_view utf16, std::string& out) {
    out.clear();
    out.reserve(utf16.size() * 3u);

    const std::size_t len = utf16.size();
    std::size_t i = 0u;

    while (i < len) {
        uint32_t cp = static_cast<uint16_t>(utf16[i]);
        if (cp >= kUnicodeSurrogateMin && cp <= kUnicodeHighSurrogateMax) {
            if (i + 1u >= len) {
                out.clear();
                return false;
            }
            const uint32_t low = static_cast<uint16_t>(utf16[i + 1u]);
            if (low < kUnicodeLowSurrogateMin || low > kUnicodeSurrogateMax) {
                out.clear();
                return false;
            }
            cp = kUnicodeSupplementalBase +
                 (((cp - kUnicodeSurrogateMin) << kSurrogateShift) |
                  (low - kUnicodeLowSurrogateMin));
            i += 2u;
        } else if (cp >= kUnicodeLowSurrogateMin &&
                   cp <= kUnicodeSurrogateMax) {
            out.clear();
            return false;
        } else {
            ++i;
        }

        if (cp <= kUtf8AsciiMax) {
            out.push_back(static_cast<char>(cp));
            continue;
        }
        if (cp <= kUtf8TwoByteMax) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
            out.push_back(
                static_cast<char>(kUtf8ContTag | (cp & kUtf8Payload6Mask)));
            continue;
        }
        if (cp <= kUnicodeBmpMax) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
            out.push_back(static_cast<char>(
                kUtf8ContTag | ((cp >> 6u) & kUtf8Payload6Mask)));
            out.push_back(
                static_cast<char>(kUtf8ContTag | (cp & kUtf8Payload6Mask)));
            continue;
        }

        out.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
        out.push_back(
            static_cast<char>(kUtf8ContTag | ((cp >> 12u) & kUtf8Payload6Mask)));
        out.push_back(
            static_cast<char>(kUtf8ContTag | ((cp >> 6u) & kUtf8Payload6Mask)));
        out.push_back(
            static_cast<char>(kUtf8ContTag | (cp & kUtf8Payload6Mask)));
    }

    return true;
}

void UnescapeInPlace(std::string& s) {
    UnescapeStringImpl(s);
}

void UnescapeInPlace(std::wstring& s) {
    UnescapeStringImpl(s);
}

bool ShouldBypassTranslation(std::wstring_view s) noexcept {
    if (s.empty()) {
        return true;
    }
    if (s == kNamaeMacroA || s == kNamaeMacroB) {
        return true;
    }
    for (const wchar_t ch : s) {
        if (static_cast<uint16_t>(ch) >= kAsciiUpperExclusive) {
            return false;
        }
    }
    return true;
}

bool IsDialogueLine(std::wstring_view text) noexcept {
    for (const wchar_t ch : text) {
        if (IsLeadingWhitespace(ch)) {
            continue;
        }
        return IsOpeningDialogueQuote(ch);
    }
    return false;
}

std::wstring WrapForSiglusNvl(std::wstring_view en, NvlWrapConfig cfg) {
    if (en.empty()) {
        return {};
    }
    if (cfg.is_choice) {
        return std::wstring(en);
    }

    const std::size_t max_cols =
        IsDialogueLine(en) ? cfg.dialogue_cols : cfg.narration_cols;
    if (max_cols == 0u) {
        return std::wstring(en);
    }

    std::wstring out;
    out.reserve(en.size() + 8u);

    std::size_t seg_start = 0u;
    const std::size_t total_len = en.size();

    while (seg_start <= total_len) {
        const std::size_t nl = en.find(L'\n', seg_start);
        const std::size_t raw_end =
            (nl == std::wstring_view::npos) ? total_len : nl;
        std::size_t clean_end = raw_end;
        if (clean_end > seg_start && en[clean_end - 1u] == L'\r') {
            --clean_end;
        }

        WrapSingleParagraph(en.substr(seg_start, clean_end - seg_start),
                            max_cols, out);

        if (nl == std::wstring_view::npos) {
            break;
        }
        out.push_back(L'\n');
        seg_start = nl + 1u;
    }

    return out;
}

struct TranslationTable::Impl {
    struct Entry {
        std::wstring jp;
        std::wstring en;
    };

    std::vector<Entry> entries;
    std::vector<std::size_t> buckets;

    void Clear() noexcept {
        entries.clear();
        std::fill(buckets.begin(), buckets.end(), kEmptyBucketSlot);
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

    const std::wstring* Find(std::wstring_view jp) const noexcept {
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

    bool TryInsertFirstWins(std::wstring&& jp, std::wstring&& en) {
        if ((entries.size() + 1u) * 2u > buckets.size()) {
            Rehash(std::max(kInitialBucketCount, buckets.size() * 2u));
        }
        const std::size_t mask = buckets.size() - 1u;
        std::size_t slot = std::hash<std::wstring_view>{}(jp) & mask;

        for (;;) {
            const std::size_t encoded = buckets[slot];
            if (encoded == kEmptyBucketSlot) {
                entries.push_back(Entry{std::move(jp), std::move(en)});
                buckets[slot] = entries.size();
                return true;
            }
            if (entries[encoded - 1u].jp == jp) {
                return false;
            }
            slot = (slot + 1u) & mask;
        }
    }
};

TranslationTable::TranslationTable() : impl_(std::make_unique<Impl>()) {}

TranslationTable::TranslationTable(const TranslationTable& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

TranslationTable::TranslationTable(TranslationTable&& other) noexcept = default;

TranslationTable& TranslationTable::operator=(const TranslationTable& other) {
    if (this != &other) {
        *impl_ = *other.impl_;
    }
    return *this;
}

TranslationTable& TranslationTable::operator=(
    TranslationTable&& other) noexcept = default;

TranslationTable::~TranslationTable() = default;

std::size_t TranslationTable::ParseTsvBytes(std::string_view utf8_tsv) {
    if (utf8_tsv.size() >= kUtf8Bom.size() &&
        utf8_tsv.substr(0u, kUtf8Bom.size()) == kUtf8Bom) {
        utf8_tsv.remove_prefix(kUtf8Bom.size());
    }
    if (utf8_tsv.empty()) {
        return 0u;
    }

    std::size_t inserted = 0u;
    std::size_t line_start = 0u;
    const std::size_t total = utf8_tsv.size();
    std::wstring jp_wide;
    std::wstring en_wide;

    while (line_start < total) {
        const std::size_t nl = utf8_tsv.find('\n', line_start);
        const std::size_t line_end =
            (nl == std::string_view::npos) ? total : nl;

        std::string_view line = utf8_tsv.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1u);
        }

        line_start = (nl == std::string_view::npos) ? total : (nl + 1u);
        if (line.empty()) {
            continue;
        }

        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) {
            continue;
        }

        const std::string_view raw_jp = line.substr(0u, tab);
        const std::string_view raw_en = line.substr(tab + 1u);
        if (raw_jp.empty() || raw_en.empty()) {
            continue;
        }

        if (!Utf8ToUtf16(raw_jp, jp_wide) || !Utf8ToUtf16(raw_en, en_wide)) {
            continue;
        }

        UnescapeInPlace(jp_wide);
        UnescapeInPlace(en_wide);
        if (jp_wide.empty() || en_wide.empty()) {
            continue;
        }
        if (jp_wide.find(L'\0') != std::wstring::npos ||
            en_wide.find(L'\0') != std::wstring::npos) {
            continue;
        }

        if (impl_->TryInsertFirstWins(std::move(jp_wide), std::move(en_wide))) {
            ++inserted;
        }
    }

    return inserted;
}

bool TranslationTable::LoadFromFile(const std::filesystem::path& path) {
    impl_->Clear();
    if (path.empty()) {
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (in.bad() || contents.empty()) {
        return false;
    }

    ParseTsvBytes(contents);
    return !Empty();
}

bool TranslationTable::LoadFromUtf8Path(std::string_view utf8_path) {
    impl_->Clear();
    if (utf8_path.empty()) {
        return false;
    }

    std::wstring wide_path;
    if (!Utf8ToUtf16(utf8_path, wide_path) || wide_path.empty()) {
        return false;
    }

    return LoadFromFile(std::filesystem::path(wide_path));
}

void TranslationTable::Clear() noexcept {
    impl_->Clear();
}

const std::wstring* TranslationTable::Find(
    std::wstring_view jp) const noexcept {
    return impl_->Find(jp);
}

bool TranslationTable::Contains(std::wstring_view jp) const noexcept {
    return impl_->Find(jp) != nullptr;
}

std::size_t TranslationTable::Size() const noexcept {
    return impl_->entries.size();
}

bool TranslationTable::Empty() const noexcept {
    return impl_->entries.empty();
}

void TranslationTable::ForEach(const EntryVisitor& visitor) const {
    if (!visitor) {
        return;
    }
    for (const auto& entry : impl_->entries) {
        visitor(entry.jp, entry.en);
    }
}

struct TranslationRuntimeFacade::Impl {
    TranslationTable table;
    NvlWrapConfig wrap_config{};
    RuntimeStats stats{};
};

TranslationRuntimeFacade::TranslationRuntimeFacade()
    : impl_(std::make_unique<Impl>()) {}

TranslationRuntimeFacade::TranslationRuntimeFacade(NvlWrapConfig wrap_config)
    : impl_(std::make_unique<Impl>()) {
    impl_->wrap_config = wrap_config;
}

TranslationRuntimeFacade::TranslationRuntimeFacade(
    const TranslationRuntimeFacade& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

TranslationRuntimeFacade::TranslationRuntimeFacade(
    TranslationRuntimeFacade&& other) noexcept = default;

TranslationRuntimeFacade& TranslationRuntimeFacade::operator=(
    const TranslationRuntimeFacade& other) {
    if (this != &other) {
        *impl_ = *other.impl_;
    }
    return *this;
}

TranslationRuntimeFacade& TranslationRuntimeFacade::operator=(
    TranslationRuntimeFacade&& other) noexcept = default;

TranslationRuntimeFacade::~TranslationRuntimeFacade() = default;

bool TranslationRuntimeFacade::LoadTsvFile(const std::filesystem::path& path) {
    return impl_->table.LoadFromFile(path);
}

bool TranslationRuntimeFacade::LoadTsvUtf8Path(std::string_view utf8_path) {
    return impl_->table.LoadFromUtf8Path(utf8_path);
}

std::size_t TranslationRuntimeFacade::LoadTsvBuffer(std::string_view utf8_tsv) {
    impl_->table.Clear();
    return impl_->table.ParseTsvBytes(utf8_tsv);
}

void TranslationRuntimeFacade::SetWrapConfig(NvlWrapConfig cfg) noexcept {
    impl_->wrap_config = cfg;
}

NvlWrapConfig TranslationRuntimeFacade::WrapConfig() const noexcept {
    return impl_->wrap_config;
}

bool TranslationRuntimeFacade::ApplyPostHook(
    std::wstring_view current_str,
    const AssignCallback& assign_fn,
    bool is_choice) {
    ++impl_->stats.total_calls;

    if (ShouldBypassTranslation(current_str)) {
        ++impl_->stats.bypassed_calls;
        return false;
    }

    const std::wstring* raw_en = impl_->table.Find(current_str);
    if (raw_en == nullptr) {
        ++impl_->stats.lookup_misses;
        return false;
    }

    NvlWrapConfig active_cfg = impl_->wrap_config;
    if (is_choice) {
        active_cfg.is_choice = true;
    }

    const std::wstring formatted = WrapForSiglusNvl(*raw_en, active_cfg);
    if (assign_fn) {
        assign_fn(formatted);
    }

    ++impl_->stats.translated_hits;
    return true;
}

std::size_t TranslationRuntimeFacade::EntryCount() const noexcept {
    return impl_->table.Size();
}

RuntimeStats TranslationRuntimeFacade::Stats() const noexcept {
    return impl_->stats;
}

void TranslationRuntimeFacade::ResetStats() noexcept {
    impl_->stats = RuntimeStats{};
}

}  // namespace translator_logic
