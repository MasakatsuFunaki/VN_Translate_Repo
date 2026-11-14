// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "g00/gameexe_codec.h"

#include "g00/g00_codec.h"
#include "siglus/siglus_decoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adi::g00 {
namespace {

constexpr std::uint8_t kByteMask = 0xFFu;
constexpr unsigned kShift8 = 8u;
constexpr unsigned kShift16 = 16u;
constexpr unsigned kShift24 = 24u;

constexpr std::size_t kHeaderUsePrivateKeyOffset = 4u;
constexpr std::size_t kPrivateKeyIndexMask = 0x0Fu;
constexpr std::size_t kKey256IndexMask = 0xFFu;
constexpr std::size_t kBytesPerUtf16Unit = 2u;

constexpr char16_t kUtf16Bom = u'\uFEFF';
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

constexpr std::uint32_t kSurrogateHighStart = 0xD800u;
constexpr std::uint32_t kSurrogateHighEnd = 0xDBFFu;
constexpr std::uint32_t kSurrogateLowStart = 0xDC00u;
constexpr std::uint32_t kSurrogateLowEnd = 0xDFFFu;
constexpr std::uint32_t kSupplementaryBase = 0x10000u;
constexpr unsigned kSurrogateShift = 10u;
constexpr std::uint32_t kSurrogateLowMask = 0x3FFu;
constexpr std::uint32_t kMaxUnicodeCodePoint = 0x10FFFFu;

constexpr std::uint32_t kUtf8OneByteMax = 0x7Fu;
constexpr std::uint32_t kUtf8TwoByteMax = 0x7FFu;
constexpr std::uint32_t kUtf8ThreeByteMax = 0xFFFFu;

constexpr std::uint8_t kUtf8ContPrefix = 0x80u;
constexpr std::uint8_t kUtf8ContMask = 0x3Fu;
constexpr std::uint8_t kUtf8ContCheckMask = 0xC0u;
constexpr std::uint8_t kUtf8TwoBytePrefix = 0xC0u;
constexpr std::uint8_t kUtf8TwoByteCheckMask = 0xE0u;
constexpr std::uint8_t kUtf8TwoByteLeadValMask = 0x1Fu;
constexpr std::uint8_t kUtf8ThreeBytePrefix = 0xE0u;
constexpr std::uint8_t kUtf8ThreeByteCheckMask = 0xF0u;
constexpr std::uint8_t kUtf8ThreeByteLeadValMask = 0x0Fu;
constexpr std::uint8_t kUtf8FourBytePrefix = 0xF0u;
constexpr std::uint8_t kUtf8FourByteCheckMask = 0xF8u;
constexpr std::uint8_t kUtf8FourByteLeadValMask = 0x07u;
constexpr unsigned kUtf8Shift6 = 6u;
constexpr unsigned kUtf8Shift12 = 12u;
constexpr unsigned kUtf8Shift18 = 18u;

std::uint32_t ReadU32Le(boost::span<const std::uint8_t> buf, std::size_t off) {
    return static_cast<std::uint32_t>(buf[off]) |
           (static_cast<std::uint32_t>(buf[off + 1u]) << kShift8) |
           (static_cast<std::uint32_t>(buf[off + 2u]) << kShift16) |
           (static_cast<std::uint32_t>(buf[off + 3u]) << kShift24);
}

void WriteU32LeAt(std::vector<std::uint8_t>& out, std::size_t off, std::uint32_t v) {
    out[off] = static_cast<std::uint8_t>(v & kByteMask);
    out[off + 1u] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    out[off + 2u] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    out[off + 3u] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

bool IsHorizontalWs(char ch) noexcept {
    return ch == ' ' || ch == '\t';
}

std::string_view TrimHorizontalWs(std::string_view sv) noexcept {
    std::size_t beg = 0u;
    while (beg < sv.size() && IsHorizontalWs(sv[beg])) {
        ++beg;
    }
    std::size_t end = sv.size();
    while (end > beg && IsHorizontalWs(sv[end - 1u])) {
        --end;
    }
    return sv.substr(beg, end - beg);
}

char ToAsciiUpper(char ch) noexcept {
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<char>(ch - ('a' - 'A'));
    }
    return ch;
}

std::string NormalizeDirectiveKey(std::string_view raw_key) {
    std::string_view trimmed = TrimHorizontalWs(raw_key);
    if (!trimmed.empty() && trimmed.front() == '#') {
        trimmed.remove_prefix(1u);
        trimmed = TrimHorizontalWs(trimmed);
    }
    std::string out;
    out.reserve(trimmed.size());
    for (const char ch : trimmed) {
        out.push_back(ToAsciiUpper(ch));
    }
    return out;
}

bool StartsWith(std::string_view sv, std::string_view prefix) noexcept {
    return sv.size() >= prefix.size() && sv.substr(0u, prefix.size()) == prefix;
}

bool EndsWith(std::string_view sv, std::string_view suffix) noexcept {
    return sv.size() >= suffix.size() &&
           sv.substr(sv.size() - suffix.size(), suffix.size()) == suffix;
}

struct ParsedDirective {
    std::string_view key;
    std::size_t key_end_pos = 0u;
    bool has_equals = false;
    std::size_t val_start = 0u;
    std::size_t val_end = 0u;
};

std::optional<ParsedDirective> ParseDirectiveLine(std::string_view line) noexcept {
    std::size_t pos = 0u;
    while (pos < line.size() && IsHorizontalWs(line[pos])) {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != '#') {
        return std::nullopt;
    }

    std::size_t scan = pos + 1u;
    while (scan < line.size() && line[scan] != '=' && line[scan] != ';') {
        ++scan;
    }

    const std::string_view raw_key = line.substr(pos + 1u, scan - (pos + 1u));
    const std::string_view key = TrimHorizontalWs(raw_key);
    if (key.empty()) {
        return std::nullopt;
    }

    if (scan >= line.size() || line[scan] == ';') {
        const std::size_t key_end =
            static_cast<std::size_t>((key.data() - line.data()) + key.size());
        return ParsedDirective{key, key_end, false, 0u, 0u};
    }

    const std::size_t eq_pos = scan;
    bool in_quotes = false;
    std::size_t comment_pos = line.size();
    for (std::size_t j = eq_pos + 1u; j < line.size(); ++j) {
        if (line[j] == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && line[j] == ';') {
            comment_pos = j;
            break;
        }
    }

    std::size_t lead = eq_pos + 1u;
    while (lead < comment_pos && IsHorizontalWs(line[lead])) {
        ++lead;
    }

    if (lead == comment_pos) {
        std::size_t insert_at = comment_pos;
        if (comment_pos < line.size() && comment_pos > eq_pos + 1u) {
            insert_at = eq_pos + 2u;
        }
        return ParsedDirective{key, eq_pos, true, insert_at, insert_at};
    }

    std::size_t trail = comment_pos;
    while (trail > lead && IsHorizontalWs(line[trail - 1u])) {
        --trail;
    }
    return ParsedDirective{key, eq_pos, true, lead, trail};
}

bool IsCriticalSaveDirective(std::string_view upper_key) noexcept {
    if (upper_key == "DISCMARK" || upper_key == "EXENAME" ||
        upper_key == "REG_NAME" || upper_key == "SAVEDIR") {
        return true;
    }
    if (StartsWith(upper_key, "SAVE.") || StartsWith(upper_key, "SAVE_") ||
        StartsWith(upper_key, "SAVEDATA")) {
        if (EndsWith(upper_key, "_STR") || EndsWith(upper_key, ".STR")) {
            return false;
        }
        return true;
    }
    return false;
}

std::map<std::string, std::vector<std::string>> CollectCriticalDirectives(
    std::string_view text_utf8) {
    if (StartsWith(text_utf8, kUtf8Bom)) {
        text_utf8.remove_prefix(kUtf8Bom.size());
    }

    std::map<std::string, std::vector<std::string>> out;
    std::size_t pos = 0u;
    while (pos < text_utf8.size()) {
        std::size_t eol = pos;
        while (eol < text_utf8.size() && text_utf8[eol] != '\r' &&
               text_utf8[eol] != '\n') {
            ++eol;
        }
        const std::string_view line = text_utf8.substr(pos, eol - pos);
        if (const auto parsed = ParseDirectiveLine(line)) {
            const std::string upper_key = NormalizeDirectiveKey(parsed->key);
            if (IsCriticalSaveDirective(upper_key)) {
                const std::string raw_val = parsed->has_equals
                    ? std::string(line.substr(parsed->val_start,
                                              parsed->val_end - parsed->val_start))
                    : std::string{};
                out[upper_key].push_back(raw_val);
            }
        }
        pos = eol;
        if (pos < text_utf8.size() && text_utf8[pos] == '\r') {
            ++pos;
        }
        if (pos < text_utf8.size() && text_utf8[pos] == '\n') {
            ++pos;
        }
    }
    return out;
}

void AppendCodePointUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= kUtf8OneByteMax) {
        out.push_back(static_cast<char>(cp));
        return;
    }
    if (cp <= kUtf8TwoByteMax) {
        out.push_back(static_cast<char>(kUtf8TwoBytePrefix | (cp >> kUtf8Shift6)));
        out.push_back(static_cast<char>(kUtf8ContPrefix | (cp & kUtf8ContMask)));
        return;
    }
    if (cp <= kUtf8ThreeByteMax) {
        out.push_back(static_cast<char>(kUtf8ThreeBytePrefix | (cp >> kUtf8Shift12)));
        out.push_back(
            static_cast<char>(kUtf8ContPrefix | ((cp >> kUtf8Shift6) & kUtf8ContMask)));
        out.push_back(static_cast<char>(kUtf8ContPrefix | (cp & kUtf8ContMask)));
        return;
    }
    out.push_back(static_cast<char>(kUtf8FourBytePrefix | (cp >> kUtf8Shift18)));
    out.push_back(
        static_cast<char>(kUtf8ContPrefix | ((cp >> kUtf8Shift12) & kUtf8ContMask)));
    out.push_back(
        static_cast<char>(kUtf8ContPrefix | ((cp >> kUtf8Shift6) & kUtf8ContMask)));
    out.push_back(static_cast<char>(kUtf8ContPrefix | (cp & kUtf8ContMask)));
}

bool IsUtf8Cont(std::uint8_t b) noexcept {
    return (b & kUtf8ContCheckMask) == kUtf8ContPrefix;
}

}  // namespace

GameexeKey256 LoadGameexeKeyFromExe(const std::filesystem::path& siglus_exe_path) {
    std::ifstream in(siglus_exe_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("LoadGameexeKeyFromExe failed to open executable: " +
                                 siglus_exe_path.u8string());
    }

    in.seekg(0, std::ios::end);
    const std::streamoff end_pos = in.tellg();
    const std::uint64_t required_size =
        kAdieuGameexeKeyFileOffset + static_cast<std::uint64_t>(kGameexeKey256Size);
    if (end_pos < 0 || static_cast<std::uint64_t>(end_pos) < required_size) {
        throw std::runtime_error(
            "LoadGameexeKeyFromExe executable shorter than offset 0x5F7D90 + 256 bytes");
    }

    in.seekg(static_cast<std::streamoff>(kAdieuGameexeKeyFileOffset), std::ios::beg);
    GameexeKey256 key{};
    in.read(reinterpret_cast<char*>(key.data()),
            static_cast<std::streamsize>(key.size()));
    if (!in || in.gcount() != static_cast<std::streamsize>(key.size())) {
        throw std::runtime_error(
            "LoadGameexeKeyFromExe failed reading 256-byte key table at 0x5F7D90");
    }

    if (!std::equal(kExpectedAdieuGameexeKeyPrefix.begin(),
                    kExpectedAdieuGameexeKeyPrefix.end(), key.begin())) {
        throw std::runtime_error(
            "LoadGameexeKeyFromExe 16-byte prefix mismatch at SiglusEngine.exe+0x5F7D90");
    }
    return key;
}

std::u16string DecodeGameexe(
    boost::span<const std::uint8_t> raw_dat,
    boost::span<const std::uint8_t, kGameexeKey256Size> key256) {
    if (raw_dat.size() < kGameexeHeaderBytes + kLzssFrameHeaderBytes) {
        throw std::runtime_error(
            "DecodeGameexe buffer shorter than 8-byte header + 8-byte LZSS header");
    }

    const std::uint32_t header_version = ReadU32Le(raw_dat, 0u);
    const std::uint32_t use_private_key = ReadU32Le(raw_dat, kHeaderUsePrivateKeyOffset);
    if (header_version != kGameexeHeaderVersion) {
        throw std::runtime_error("DecodeGameexe unsupported header_version " +
                                 std::to_string(header_version));
    }
    if (use_private_key != 0u && use_private_key != kGameexeUsePrivateKey) {
        throw std::runtime_error("DecodeGameexe unsupported use_private_key mode " +
                                 std::to_string(use_private_key));
    }

    const std::size_t payload_len = raw_dat.size() - kGameexeHeaderBytes;
    std::vector<std::uint8_t> decrypted(payload_len, 0u);
    for (std::size_t i = 0u; i < payload_len; ++i) {
        std::uint8_t b = static_cast<std::uint8_t>(
            raw_dat[kGameexeHeaderBytes + i] ^ key256[i & kKey256IndexMask]);
        if (use_private_key == kGameexeUsePrivateKey) {
            b = static_cast<std::uint8_t>(
                b ^ siglus::ADIEU_PRIVATE_KEY[i & kPrivateKeyIndexMask]);
        }
        decrypted[i] = b;
    }

    const std::vector<std::uint8_t> plain = DecompressByteLzss(decrypted);
    if ((plain.size() % kBytesPerUtf16Unit) != 0u) {
        throw std::runtime_error(
            "DecodeGameexe decompressed stream has odd UTF-16LE byte count");
    }

    const std::size_t unit_count = plain.size() / kBytesPerUtf16Unit;
    std::u16string out(unit_count, u'\0');
    for (std::size_t u = 0u; u < unit_count; ++u) {
        const std::uint16_t code_unit = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(plain[u * kBytesPerUtf16Unit]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(plain[u * kBytesPerUtf16Unit + 1u])
                << kShift8));
        out[u] = static_cast<char16_t>(code_unit);
    }
    return out;
}

std::vector<std::uint8_t> EncodeGameexe(
    std::u16string_view text_utf16,
    boost::span<const std::uint8_t, kGameexeKey256Size> key256,
    std::uint32_t use_private_key) {
    if (use_private_key != 0u && use_private_key != kGameexeUsePrivateKey) {
        throw std::runtime_error("EncodeGameexe unsupported use_private_key mode " +
                                 std::to_string(use_private_key));
    }

    std::vector<std::uint8_t> plain(text_utf16.size() * kBytesPerUtf16Unit, 0u);
    for (std::size_t u = 0u; u < text_utf16.size(); ++u) {
        const auto code_unit = static_cast<std::uint16_t>(text_utf16[u]);
        plain[u * kBytesPerUtf16Unit] = static_cast<std::uint8_t>(code_unit & kByteMask);
        plain[u * kBytesPerUtf16Unit + 1u] =
            static_cast<std::uint8_t>((code_unit >> kShift8) & kByteMask);
    }

    const std::vector<std::uint8_t> framed = CompressByteLzss(plain);
    if (DecompressByteLzss(framed) != plain) {
        throw std::runtime_error("EncodeGameexe LZSS self-verification failed");
    }

    std::vector<std::uint8_t> out(kGameexeHeaderBytes + framed.size(), 0u);
    WriteU32LeAt(out, 0u, kGameexeHeaderVersion);
    WriteU32LeAt(out, kHeaderUsePrivateKeyOffset, use_private_key);

    for (std::size_t i = 0u; i < framed.size(); ++i) {
        std::uint8_t b =
            static_cast<std::uint8_t>(framed[i] ^ key256[i & kKey256IndexMask]);
        if (use_private_key == kGameexeUsePrivateKey) {
            b = static_cast<std::uint8_t>(
                b ^ siglus::ADIEU_PRIVATE_KEY[i & kPrivateKeyIndexMask]);
        }
        out[kGameexeHeaderBytes + i] = b;
    }
    return out;
}

std::string GameexeUtf16ToUtf8(std::u16string_view text_utf16,
                               bool strip_leading_bom) {
    if (strip_leading_bom && !text_utf16.empty() && text_utf16.front() == kUtf16Bom) {
        text_utf16.remove_prefix(1u);
    }

    std::string out;
    out.reserve(text_utf16.size() * 2u);

    std::size_t i = 0u;
    while (i < text_utf16.size()) {
        const std::uint32_t u0 = static_cast<std::uint16_t>(text_utf16[i]);
        if (u0 >= kSurrogateHighStart && u0 <= kSurrogateHighEnd) {
            if (i + 1u >= text_utf16.size()) {
                throw std::runtime_error(
                    "GameexeUtf16ToUtf8 trailing unpaired high surrogate");
            }
            const std::uint32_t u1 = static_cast<std::uint16_t>(text_utf16[i + 1u]);
            if (u1 < kSurrogateLowStart || u1 > kSurrogateLowEnd) {
                throw std::runtime_error(
                    "GameexeUtf16ToUtf8 high surrogate not followed by low surrogate");
            }
            const std::uint32_t cp =
                kSupplementaryBase +
                (((u0 - kSurrogateHighStart) << kSurrogateShift) |
                 (u1 - kSurrogateLowStart));
            AppendCodePointUtf8(out, cp);
            i += 2u;
            continue;
        }
        if (u0 >= kSurrogateLowStart && u0 <= kSurrogateLowEnd) {
            throw std::runtime_error("GameexeUtf16ToUtf8 unpaired low surrogate");
        }
        AppendCodePointUtf8(out, u0);
        ++i;
    }
    return out;
}

std::u16string GameexeUtf8ToUtf16(std::string_view text_utf8,
                                  bool prepend_bom) {
    std::u16string out;
    out.reserve(text_utf8.size() + (prepend_bom ? 1u : 0u));
    if (prepend_bom && !StartsWith(text_utf8, kUtf8Bom)) {
        out.push_back(kUtf16Bom);
    }

    std::size_t i = 0u;
    while (i < text_utf8.size()) {
        const auto b0 = static_cast<std::uint8_t>(text_utf8[i]);
        std::uint32_t cp = 0u;

        if (b0 <= kUtf8OneByteMax) {
            cp = b0;
            ++i;
        } else if ((b0 & kUtf8TwoByteCheckMask) == kUtf8TwoBytePrefix) {
            if (i + 1u >= text_utf8.size()) {
                throw std::runtime_error("GameexeUtf8ToUtf16 truncated 2-byte UTF-8");
            }
            const auto b1 = static_cast<std::uint8_t>(text_utf8[i + 1u]);
            if (!IsUtf8Cont(b1)) {
                throw std::runtime_error("GameexeUtf8ToUtf16 invalid 2-byte continuation");
            }
            cp = (static_cast<std::uint32_t>(b0 & kUtf8TwoByteLeadValMask) << kUtf8Shift6) |
                 static_cast<std::uint32_t>(b1 & kUtf8ContMask);
            if (cp <= kUtf8OneByteMax) {
                throw std::runtime_error("GameexeUtf8ToUtf16 overlong 2-byte sequence");
            }
            i += 2u;
        } else if ((b0 & kUtf8ThreeByteCheckMask) == kUtf8ThreeBytePrefix) {
            if (i + 2u >= text_utf8.size()) {
                throw std::runtime_error("GameexeUtf8ToUtf16 truncated 3-byte UTF-8");
            }
            const auto b1 = static_cast<std::uint8_t>(text_utf8[i + 1u]);
            const auto b2 = static_cast<std::uint8_t>(text_utf8[i + 2u]);
            if (!IsUtf8Cont(b1) || !IsUtf8Cont(b2)) {
                throw std::runtime_error("GameexeUtf8ToUtf16 invalid 3-byte continuation");
            }
            cp = (static_cast<std::uint32_t>(b0 & kUtf8ThreeByteLeadValMask) << kUtf8Shift12) |
                 (static_cast<std::uint32_t>(b1 & kUtf8ContMask) << kUtf8Shift6) |
                 static_cast<std::uint32_t>(b2 & kUtf8ContMask);
            if (cp <= kUtf8TwoByteMax ||
                (cp >= kSurrogateHighStart && cp <= kSurrogateLowEnd)) {
                throw std::runtime_error("GameexeUtf8ToUtf16 overlong or surrogate 3-byte");
            }
            i += 3u;
        } else if ((b0 & kUtf8FourByteCheckMask) == kUtf8FourBytePrefix) {
            if (i + 3u >= text_utf8.size()) {
                throw std::runtime_error("GameexeUtf8ToUtf16 truncated 4-byte UTF-8");
            }
            const auto b1 = static_cast<std::uint8_t>(text_utf8[i + 1u]);
            const auto b2 = static_cast<std::uint8_t>(text_utf8[i + 2u]);
            const auto b3 = static_cast<std::uint8_t>(text_utf8[i + 3u]);
            if (!IsUtf8Cont(b1) || !IsUtf8Cont(b2) || !IsUtf8Cont(b3)) {
                throw std::runtime_error("GameexeUtf8ToUtf16 invalid 4-byte continuation");
            }
            cp = (static_cast<std::uint32_t>(b0 & kUtf8FourByteLeadValMask) << kUtf8Shift18) |
                 (static_cast<std::uint32_t>(b1 & kUtf8ContMask) << kUtf8Shift12) |
                 (static_cast<std::uint32_t>(b2 & kUtf8ContMask) << kUtf8Shift6) |
                 static_cast<std::uint32_t>(b3 & kUtf8ContMask);
            if (cp <= kUtf8ThreeByteMax || cp > kMaxUnicodeCodePoint) {
                throw std::runtime_error("GameexeUtf8ToUtf16 invalid 4-byte code point");
            }
            i += 4u;
        } else {
            throw std::runtime_error("GameexeUtf8ToUtf16 invalid UTF-8 lead byte");
        }

        if (cp <= kUtf8ThreeByteMax) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const std::uint32_t adj = cp - kSupplementaryBase;
            out.push_back(
                static_cast<char16_t>(kSurrogateHighStart + (adj >> kSurrogateShift)));
            out.push_back(
                static_cast<char16_t>(kSurrogateLowStart + (adj & kSurrogateLowMask)));
        }
    }

    return out;
}

GameexeDocument GameexeDocument::FromUtf8(std::string_view text_utf8) {
    GameexeDocument doc;
    if (StartsWith(text_utf8, kUtf8Bom)) {
        doc.has_leading_bom_ = true;
        text_utf8.remove_prefix(kUtf8Bom.size());
    }

    std::size_t pos = 0u;
    while (pos < text_utf8.size()) {
        std::size_t eol = pos;
        while (eol < text_utf8.size() && text_utf8[eol] != '\r' &&
               text_utf8[eol] != '\n') {
            ++eol;
        }

        LineRecord rec;
        rec.content_utf8.assign(text_utf8.substr(pos, eol - pos));

        if (eol < text_utf8.size()) {
            if (text_utf8[eol] == '\r') {
                if (eol + 1u < text_utf8.size() && text_utf8[eol + 1u] == '\n') {
                    rec.ending_utf8 = "\r\n";
                    pos = eol + 2u;
                } else {
                    rec.ending_utf8 = "\r";
                    pos = eol + 1u;
                }
            } else {
                rec.ending_utf8 = "\n";
                pos = eol + 1u;
            }
        } else {
            pos = eol;
        }
        doc.lines_.push_back(std::move(rec));
    }

    return doc;
}

GameexeDocument GameexeDocument::FromUtf16(std::u16string_view text_utf16) {
    const bool had_bom = !text_utf16.empty() && text_utf16.front() == kUtf16Bom;
    const std::string utf8 = GameexeUtf16ToUtf8(text_utf16, true);
    GameexeDocument doc = FromUtf8(utf8);
    doc.has_leading_bom_ = had_bom;
    return doc;
}

std::size_t GameexeDocument::SetDirectiveRawValue(std::string_view directive_key,
                                                  std::string_view new_raw_value) {
    const std::string target_key = NormalizeDirectiveKey(directive_key);
    if (target_key.empty()) {
        return 0u;
    }

    std::size_t updated = 0u;
    for (LineRecord& rec : lines_) {
        const auto parsed = ParseDirectiveLine(rec.content_utf8);
        if (!parsed.has_value()) {
            continue;
        }
        if (NormalizeDirectiveKey(parsed->key) != target_key) {
            continue;
        }

        if (parsed->has_equals) {
            std::string rebuilt;
            rebuilt.reserve(parsed->val_start + new_raw_value.size() +
                            (rec.content_utf8.size() - parsed->val_end));
            rebuilt.append(rec.content_utf8, 0u, parsed->val_start);
            rebuilt.append(new_raw_value);
            rebuilt.append(rec.content_utf8, parsed->val_end,
                           rec.content_utf8.size() - parsed->val_end);
            rec.content_utf8 = std::move(rebuilt);
        } else {
            rec.content_utf8.insert(parsed->key_end_pos,
                                    " = " + std::string(new_raw_value));
        }
        ++updated;
    }
    return updated;
}

std::size_t GameexeDocument::SetDirectiveStringValue(std::string_view directive_key,
                                                     std::string_view unquoted_utf8) {
    std::string quoted;
    quoted.reserve(unquoted_utf8.size() + 2u);
    quoted.push_back('"');
    quoted.append(unquoted_utf8);
    quoted.push_back('"');
    return SetDirectiveRawValue(directive_key, quoted);
}

std::size_t GameexeDocument::directive_count() const noexcept {
    std::size_t count = 0u;
    for (const LineRecord& rec : lines_) {
        if (ParseDirectiveLine(rec.content_utf8).has_value()) {
            ++count;
        }
    }
    return count;
}

std::optional<std::string> GameexeDocument::FindDirectiveRawValue(
    std::string_view directive_key) const {
    const std::string target_key = NormalizeDirectiveKey(directive_key);
    if (target_key.empty()) {
        return std::nullopt;
    }

    for (const LineRecord& rec : lines_) {
        const auto parsed = ParseDirectiveLine(rec.content_utf8);
        if (!parsed.has_value()) {
            continue;
        }
        if (NormalizeDirectiveKey(parsed->key) != target_key) {
            continue;
        }
        if (!parsed->has_equals) {
            return std::string{};
        }
        return rec.content_utf8.substr(parsed->val_start,
                                       parsed->val_end - parsed->val_start);
    }
    return std::nullopt;
}

std::vector<std::string> GameexeDocument::FindAllDirectiveRawValues(
    std::string_view directive_key) const {
    std::vector<std::string> results;
    const std::string target_key = NormalizeDirectiveKey(directive_key);
    if (target_key.empty()) {
        return results;
    }

    for (const LineRecord& rec : lines_) {
        const auto parsed = ParseDirectiveLine(rec.content_utf8);
        if (!parsed.has_value()) {
            continue;
        }
        if (NormalizeDirectiveKey(parsed->key) != target_key) {
            continue;
        }
        if (!parsed->has_equals) {
            results.emplace_back();
            continue;
        }
        results.push_back(rec.content_utf8.substr(
            parsed->val_start, parsed->val_end - parsed->val_start));
    }
    return results;
}

std::string GameexeDocument::ToUtf8(bool include_bom) const {
    std::size_t total_len = (include_bom && has_leading_bom_) ? kUtf8Bom.size() : 0u;
    for (const LineRecord& rec : lines_) {
        total_len += rec.content_utf8.size() + rec.ending_utf8.size();
    }

    std::string out;
    out.reserve(total_len);
    if (include_bom && has_leading_bom_) {
        out.append(kUtf8Bom);
    }
    for (const LineRecord& rec : lines_) {
        out.append(rec.content_utf8);
        out.append(rec.ending_utf8);
    }
    return out;
}

std::u16string GameexeDocument::ToUtf16() const {
    const std::string body_utf8 = ToUtf8(false);
    std::u16string out = GameexeUtf8ToUtf16(body_utf8, false);
    if (has_leading_bom_) {
        out.insert(out.begin(), kUtf16Bom);
    }
    return out;
}

void VerifyCriticalDirectivesUnchanged(std::string_view orig_utf8,
                                       std::string_view patched_utf8) {
    const auto orig_crit = CollectCriticalDirectives(orig_utf8);
    const auto patched_crit = CollectCriticalDirectives(patched_utf8);

    for (const auto& [key, orig_vals] : orig_crit) {
        const auto it = patched_crit.find(key);
        if (it == patched_crit.end()) {
            throw std::runtime_error(
                "VerifyCriticalDirectivesUnchanged: critical directive #" + key +
                " was removed");
        }
        if (it->second != orig_vals) {
            throw std::runtime_error(
                "VerifyCriticalDirectivesUnchanged: critical directive #" + key +
                " was modified");
        }
    }

    for (const auto& [key, patched_vals] : patched_crit) {
        (void)patched_vals;
        if (orig_crit.find(key) == orig_crit.end()) {
            throw std::runtime_error(
                "VerifyCriticalDirectivesUnchanged: critical directive #" + key +
                " was added");
        }
    }
}

}  // namespace adi::g00
