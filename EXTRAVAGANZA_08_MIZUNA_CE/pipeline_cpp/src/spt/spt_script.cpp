// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "spt/spt_script.h"

#include <algorithm>
#include <cstring>

namespace exm::spt {

namespace {

constexpr std::uint8_t kXorKey = 0xFF;
constexpr std::size_t kMinHeaderSize = 0x44;
constexpr std::size_t kMagicOffset = 0x30;
constexpr const char* kHeaderMagic = "SPTHEADER0";
constexpr std::size_t kHeaderMagicLen = 10;

constexpr std::size_t kStringCountOffset = 0x18;
constexpr std::size_t kStringCountEnd = 0x1C;
constexpr unsigned int kShift8 = 8;
constexpr unsigned int kShift16 = 16;
constexpr unsigned int kShift24 = 24;

constexpr char32_t kAsciiMax = 0x7F;
constexpr char32_t kHiraganaMin = 0x3040;
constexpr char32_t kHiraganaMax = 0x309F;
constexpr char32_t kKatakanaMin = 0x30A0;
constexpr char32_t kKatakanaMax = 0x30FF;
constexpr char32_t kCjkUnifiedMin = 0x4E00;
constexpr char32_t kCjkUnifiedMax = 0x9FFF;
constexpr char32_t kCjkExtAMin = 0x3400;
constexpr char32_t kCjkExtAMax = 0x4DBF;
constexpr char32_t kFullwidthMin = 0xFF00;
constexpr char32_t kFullwidthMax = 0xFFEF;
constexpr char32_t kCjkSymbolsMin = 0x3000;
constexpr char32_t kCjkSymbolsMax = 0x303F;

constexpr std::size_t kMaxBytecodeNoiseLen = 3;
constexpr std::size_t kMinAsciiStringLen = 4;
constexpr std::size_t kMaxSpeakerPlateChars = 10;
constexpr std::size_t kMaxNameTagChars = 6;
constexpr std::size_t kStringAlignment = 4;
constexpr std::size_t kCrlfLen = 2;

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

bool all_ascii(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        if (utf8_next(utf8, i) > kAsciiMax) return false;
    }
    return true;
}

bool starts_with(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

bool is_dialogue_quote_start(const std::string& s) {
    return starts_with(s, "「") || starts_with(s, "『") || starts_with(s, "（");
}

bool is_bytecode(std::size_t byte_len, const std::string& text) {
    if (byte_len > kMaxBytecodeNoiseLen) return false;
    static constexpr const char* kNoise[] = {
        "ff", "UU", "VU", "33", "DD", "22", "f", "U", "V", "D"
    };
    for (const char* n : kNoise) {
        if (text == n) return true;
    }
    return false;
}

}  // namespace

Bytes decrypt(const Bytes& data) {
    Bytes out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(data[i] ^ kXorKey);
    }
    return out;
}

bool is_valid(const Bytes& dec) {
    return dec.size() >= kMinHeaderSize &&
           std::memcmp(dec.data() + kMagicOffset, kHeaderMagic, kHeaderMagicLen) == 0;
}

std::uint32_t header_string_count(const Bytes& dec) {
    if (dec.size() < kStringCountEnd) return 0;
    return static_cast<std::uint32_t>(dec[kStringCountOffset]) |
           (static_cast<std::uint32_t>(dec[kStringCountOffset + 1]) << kShift8) |
           (static_cast<std::uint32_t>(dec[kStringCountOffset + 2]) << kShift16) |
           (static_cast<std::uint32_t>(dec[kStringCountOffset + 3]) << kShift24);
}

bool has_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= kHiraganaMin && cp <= kHiraganaMax) ||
            (cp >= kKatakanaMin && cp <= kKatakanaMax) ||
            (cp >= kCjkUnifiedMin && cp <= kCjkUnifiedMax) ||
            (cp >= kCjkExtAMin && cp <= kCjkExtAMax) ||
            (cp >= kFullwidthMin && cp <= kFullwidthMax) ||
            (cp >= kCjkSymbolsMin && cp <= kCjkSymbolsMax)) {
            return true;
        }
    }
    return false;
}

std::string classify(const std::string& raw) {
    const std::string text = trim(raw);
    if (text.empty()) return "empty";

    if (starts_with(text, "★") ||
        starts_with(text, "■") ||
        starts_with(text, "●")) {
        return "menu";
    }

    const std::size_t crlf = text.find("\r\n");
    if (crlf != std::string::npos) {
        const std::string first = trim(text.substr(0, crlf));
        const std::string rest = trim_left(text.substr(crlf + kCrlfLen));
        if (!first.empty() && char_len(first) <= kMaxSpeakerPlateChars &&
            first.find("「") == std::string::npos &&
            is_dialogue_quote_start(rest)) {
            return "dialogue";
        }
        return "narrative";
    }
    if (starts_with(text, "「") &&
        text.find("」") != std::string::npos) {
        return "dialogue";
    }
    if (all_ascii(text)) return "label";
    if (char_len(text) <= kMaxNameTagChars && has_japanese(text)) return "name";
    if (has_japanese(text)) return "narrative";
    return "other";
}

std::vector<SptString> extract_strings(const Bytes& dec, std::size_t skip_offset) {
    std::vector<SptString> out;
    const std::size_t fsize = dec.size();
    std::size_t pos = skip_offset;

    while (pos < fsize) {
        const auto* nul = static_cast<const std::uint8_t*>(
            std::memchr(dec.data() + pos, 0, fsize - pos));
        if (!nul) break;
        const std::size_t end = static_cast<std::size_t>(nul - dec.data());
        if (end == pos) {
            ++pos;
            continue;
        }

        const std::size_t byte_len = end - pos;
        if (auto text = cp932_to_utf8_strict(dec.data() + pos, byte_len)) {
            const bool jp = has_japanese(*text);
            if (!is_bytecode(byte_len, *text) && (jp || byte_len >= kMinAsciiStringLen)) {
                SptString s;
                s.offset = static_cast<std::uint32_t>(pos);
                s.byte_len = static_cast<std::uint32_t>(byte_len);
                s.text = *text;
                s.has_jp = jp;
                out.push_back(std::move(s));
            }
        }

        pos = ((end + 1 + (kStringAlignment - 1)) / kStringAlignment) * kStringAlignment;
    }
    return out;
}

struct SptScript::Impl {
    Bytes raw;
    Bytes decrypted;
    bool valid = false;
    std::uint32_t header_count = 0;
    std::vector<SptString> strings;

    Impl(Bytes raw_in, std::size_t skip_offset)
        : raw(std::move(raw_in)),
          decrypted(decrypt(raw)),
          valid(exm::spt::is_valid(decrypted)),
          header_count(valid ? exm::spt::header_string_count(decrypted) : 0) {
        if (!valid) return;
        strings = extract_strings(decrypted, skip_offset);
        for (auto& s : strings) {
            s.type = classify(s.text);
        }
    }
};

SptScript::SptScript(Bytes raw, std::size_t skip_offset)
    : impl_(std::make_unique<Impl>(std::move(raw), skip_offset)) {}

SptScript::~SptScript() = default;
SptScript::SptScript(SptScript&&) noexcept = default;
SptScript& SptScript::operator=(SptScript&&) noexcept = default;

bool SptScript::is_valid() const noexcept {
    return impl_->valid;
}

std::size_t SptScript::file_size() const noexcept {
    return impl_->raw.size();
}

std::uint32_t SptScript::header_string_count() const noexcept {
    return impl_->header_count;
}

const Bytes& SptScript::raw() const noexcept {
    return impl_->raw;
}

const Bytes& SptScript::decrypted() const noexcept {
    return impl_->decrypted;
}

const std::vector<SptString>& SptScript::strings() const noexcept {
    return impl_->strings;
}

}  // namespace exm::spt
