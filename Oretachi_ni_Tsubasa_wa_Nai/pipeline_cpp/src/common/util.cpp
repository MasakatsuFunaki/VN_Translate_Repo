// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace oretsuba {

namespace {

constexpr UINT kCodePageCp932 = 932;
constexpr std::size_t kCrlfReserveDivisor = 16;
constexpr std::size_t kSingleByteStep = 1;
constexpr std::size_t kDoubleByteStep = 2;

constexpr std::uint8_t kAsciiMax = 0x7FU;
constexpr std::uint8_t kHalfwidthKanaMin = 0xA1U;
constexpr std::uint8_t kHalfwidthKanaMax = 0xDFU;
constexpr std::uint8_t kLeadRange1Min = 0x81U;
constexpr std::uint8_t kLeadRange1Max = 0x9FU;
constexpr std::uint8_t kLeadRange2Min = 0xE0U;
constexpr std::uint8_t kLeadRange2Max = 0xFCU;
constexpr std::uint8_t kTrailRange1Min = 0x40U;
constexpr std::uint8_t kTrailRange1Max = 0x7EU;
constexpr std::uint8_t kTrailRange2Min = 0x80U;
constexpr std::uint8_t kTrailRange2Max = 0xFCU;

constexpr std::uint8_t kSpecialByte80 = 0x80U;
constexpr std::uint8_t kSpecialByteA0 = 0xA0U;
constexpr std::uint8_t kSpecialByteFd = 0xFDU;
constexpr std::uint8_t kSpecialByteFe = 0xFEU;
constexpr std::uint8_t kSpecialByteFf = 0xFFU;

constexpr char32_t kSpecialCp80 = 0x0080U;
constexpr char32_t kSpecialCpA0 = 0xF8F0U;
constexpr char32_t kSpecialCpFd = 0xF8F1U;
constexpr char32_t kSpecialCpFe = 0xF8F2U;
constexpr char32_t kSpecialCpFf = 0xF8F3U;

constexpr char32_t kUtf8OneByteLimit = 0x80U;
constexpr char32_t kUtf8TwoByteLimit = 0x800U;
constexpr std::uint8_t kUtf8TwoBytePrefix = 0xC0U;
constexpr std::uint8_t kUtf8ThreeBytePrefix = 0xE0U;
constexpr std::uint8_t kUtf8FourBytePrefix = 0xF0U;
constexpr std::uint8_t kUtf8TrailPrefix = 0x80U;
constexpr std::uint8_t kUtf8TrailMask = 0x3FU;
constexpr int kUtf8Shift6 = 6;
constexpr int kUtf8Shift12 = 12;
constexpr int kUtf8Extra1 = 1;
constexpr int kUtf8Extra2 = 2;
constexpr int kUtf8Extra3 = 3;

constexpr char32_t kHiraganaMin = 0x3040U;
constexpr char32_t kHiraganaMax = 0x309FU;
constexpr char32_t kKatakanaMin = 0x30A0U;
constexpr char32_t kKatakanaMax = 0x30FFU;
constexpr char32_t kCjkUnifiedMin = 0x4E00U;
constexpr char32_t kCjkUnifiedMax = 0x9FFFU;
constexpr char32_t kCjkExtAMin = 0x3400U;
constexpr char32_t kCjkExtAMax = 0x4DBFU;
constexpr char32_t kCjkSymbolsMin = 0x3000U;
constexpr char32_t kFullwidthFormsMin = 0xFF00U;
constexpr char32_t kFullwidthFormsMax = 0xFFEFU;
constexpr char32_t kPrintableAsciiMin = 0x20U;

constexpr char32_t kSpaceTab = 0x09U;
constexpr char32_t kSpaceLf = 0x0AU;
constexpr char32_t kSpaceVt = 0x0BU;
constexpr char32_t kSpaceFf = 0x0CU;
constexpr char32_t kSpaceCr = 0x0DU;
constexpr char32_t kSpaceFs = 0x1CU;
constexpr char32_t kSpaceGs = 0x1DU;
constexpr char32_t kSpaceRs = 0x1EU;
constexpr char32_t kSpaceUs = 0x1FU;
constexpr char32_t kSpaceAscii = 0x20U;
constexpr char32_t kSpaceNel = 0x85U;
constexpr char32_t kSpaceNbsp = 0xA0U;
constexpr char32_t kSpaceOgham = 0x1680U;
constexpr char32_t kSpaceEnQuadMin = 0x2000U;
constexpr char32_t kSpaceHairMax = 0x200AU;
constexpr char32_t kSpaceLineSep = 0x2028U;
constexpr char32_t kSpaceParaSep = 0x2029U;
constexpr char32_t kSpaceNarrowNbsp = 0x202FU;
constexpr char32_t kSpaceMediumMath = 0x205FU;
constexpr char32_t kSpaceIdeographic = 0x3000U;

constexpr std::size_t kTimestampBufSize = 16;
constexpr int kCommaGroupSize = 3;

std::string ConvertToCrlf(std::string_view input) {
    std::string out;
    out.reserve(input.size() + input.size() / kCrlfReserveDivisor);
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\n' && (i == 0 || input[i - 1] != '\r')) {
            out += '\r';
        }
        out += input[i];
    }
    return out;
}

bool MapCp932SpecialSingle(std::uint8_t byte_val, char32_t* codepoint) {
    switch (byte_val) {
    case kSpecialByte80:
        *codepoint = kSpecialCp80;
        return true;
    case kSpecialByteA0:
        *codepoint = kSpecialCpA0;
        return true;
    case kSpecialByteFd:
        *codepoint = kSpecialCpFd;
        return true;
    case kSpecialByteFe:
        *codepoint = kSpecialCpFe;
        return true;
    case kSpecialByteFf:
        *codepoint = kSpecialCpFf;
        return true;
    default:
        return false;
    }
}

bool IsCp932SingleByte(std::uint8_t byte_val) {
    char32_t ignored = 0;
    return byte_val <= kAsciiMax ||
           (byte_val >= kHalfwidthKanaMin && byte_val <= kHalfwidthKanaMax) ||
           MapCp932SpecialSingle(byte_val, &ignored);
}

std::size_t Cp932CharLength(const std::uint8_t* data, std::size_t size, std::size_t index) {
    const std::uint8_t lead = data[index];
    if (IsCp932SingleByte(lead)) {
        return kSingleByteStep;
    }
    const bool is_lead = (lead >= kLeadRange1Min && lead <= kLeadRange1Max) ||
                         (lead >= kLeadRange2Min && lead <= kLeadRange2Max);
    if (!is_lead) {
        return 0;
    }
    if (index + kSingleByteStep >= size) {
        return 0;
    }
    const std::uint8_t trail = data[index + kSingleByteStep];
    const bool is_trail = (trail >= kTrailRange1Min && trail <= kTrailRange1Max) ||
                          (trail >= kTrailRange2Min && trail <= kTrailRange2Max);
    if (!is_trail) {
        return 0;
    }
    return kDoubleByteStep;
}

void AppendUtf8Codepoint(std::string& out, char32_t codepoint) {
    if (codepoint < kUtf8OneByteLimit) {
        out += static_cast<char>(codepoint);
        return;
    }
    if (codepoint < kUtf8TwoByteLimit) {
        out += static_cast<char>(kUtf8TwoBytePrefix | (codepoint >> kUtf8Shift6));
        out += static_cast<char>(kUtf8TrailPrefix | (codepoint & kUtf8TrailMask));
        return;
    }
    out += static_cast<char>(kUtf8ThreeBytePrefix | (codepoint >> kUtf8Shift12));
    out += static_cast<char>(kUtf8TrailPrefix | ((codepoint >> kUtf8Shift6) & kUtf8TrailMask));
    out += static_cast<char>(kUtf8TrailPrefix | (codepoint & kUtf8TrailMask));
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), len);
    return out;
}

std::string CurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &now_time);
    char buf[kTimestampBufSize];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    return buf;
}

void FormatJsonPrettyImpl(const boost::json::value& val,
                          std::string& out,
                          int indent,
                          int depth) {
    namespace bj = boost::json;
    const std::string pad(static_cast<std::size_t>(indent * depth), ' ');
    const std::string child_pad(static_cast<std::size_t>(indent * (depth + 1)), ' ');

    if (val.is_object()) {
        const auto& obj = val.get_object();
        if (obj.empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (const auto& kv : obj) {
            if (!first) {
                out += ",\n";
            }
            first = false;
            out += child_pad;
            out += bj::serialize(bj::value(kv.key()));
            out += ": ";
            FormatJsonPrettyImpl(kv.value(), out, indent, depth + 1);
        }
        out += "\n" + pad + "}";
        return;
    }

    if (val.is_array()) {
        const auto& arr = val.get_array();
        if (arr.empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        bool first = true;
        for (const auto& elem : arr) {
            if (!first) {
                out += ",\n";
            }
            first = false;
            out += child_pad;
            FormatJsonPrettyImpl(elem, out, indent, depth + 1);
        }
        out += "\n" + pad + "]";
        return;
    }

    out += bj::serialize(val);
}

void FormatJsonDumpImpl(const boost::json::value& val, std::string& out) {
    namespace bj = boost::json;
    if (val.is_object()) {
        out += '{';
        bool first = true;
        for (const auto& kv : val.get_object()) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += bj::serialize(bj::value(kv.key()));
            out += ": ";
            FormatJsonDumpImpl(kv.value(), out);
        }
        out += '}';
        return;
    }

    if (val.is_array()) {
        out += '[';
        bool first = true;
        for (const auto& elem : val.get_array()) {
            if (!first) {
                out += ", ";
            }
            first = false;
            FormatJsonDumpImpl(elem, out);
        }
        out += ']';
        return;
    }

    out += bj::serialize(val);
}

}  // namespace

Bytes ReadFileBytes(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path);
    }
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    Bytes out(size);
    if (size > 0) {
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    }
    if (!file) {
        throw std::runtime_error("read failed: " + path);
    }
    return out;
}

void WriteFileBytes(const std::string& path, const void* data, std::size_t size) {
    const auto fs_path = std::filesystem::u8path(path);
    if (fs_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }
    std::ofstream file(fs_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot create " + path);
    }
    if (size > 0) {
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!file) {
        throw std::runtime_error("write failed: " + path);
    }
}

void WriteFileBytes(const std::string& path, const Bytes& bytes) {
    WriteFileBytes(path, bytes.data(), bytes.size());
}

void WriteFileBytes(const std::string& path, std::string_view content) {
    WriteFileBytes(path, content.data(), content.size());
}

void WriteFileAtomic(const std::string& path, std::string_view content) {
    const std::string tmp_path = path + ".tmp";
    WriteFileBytes(tmp_path, content);
    std::filesystem::rename(
        std::filesystem::u8path(tmp_path),
        std::filesystem::u8path(path));
}

void WriteFileText(const std::string& path, std::string_view content) {
    WriteFileBytes(path, ConvertToCrlf(content));
}

void WriteFileAtomicText(const std::string& path, std::string_view content) {
    WriteFileAtomic(path, ConvertToCrlf(content));
}

std::optional<std::string> Cp932ToUtf8Strict(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return std::string();
    }
    std::string out;
    std::size_t i = 0;
    while (i < size) {
        char32_t special = 0;
        if (MapCp932SpecialSingle(data[i], &special)) {
            AppendUtf8Codepoint(out, special);
            ++i;
            continue;
        }
        const std::size_t run_start = i;
        while (i < size) {
            char32_t ignored = 0;
            if (MapCp932SpecialSingle(data[i], &ignored)) {
                break;
            }
            const std::size_t char_len = Cp932CharLength(data, size, i);
            if (char_len == 0) {
                return std::nullopt;
            }
            i += char_len;
        }
        const int run_bytes = static_cast<int>(i - run_start);
        if (run_bytes <= 0) {
            return std::nullopt;
        }
        const int wide_len = MultiByteToWideChar(
            kCodePageCp932, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(data + run_start),
            run_bytes, nullptr, 0);
        if (wide_len <= 0) {
            return std::nullopt;
        }
        std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
        MultiByteToWideChar(
            kCodePageCp932, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(data + run_start),
            run_bytes, wide.data(), wide_len);
        out += WideToUtf8(wide);
    }
    return out;
}

std::optional<std::string> Cp932ToUtf8Strict(const Bytes& bytes) {
    return Cp932ToUtf8Strict(bytes.data(), bytes.size());
}

std::optional<std::string> Cp932ToUtf8Strict(std::string_view raw_bytes) {
    return Cp932ToUtf8Strict(
        reinterpret_cast<const std::uint8_t*>(raw_bytes.data()),
        raw_bytes.size());
}

std::string Cp932ToUtf8(const std::uint8_t* data, std::size_t size) {
    if (const auto strict = Cp932ToUtf8Strict(data, size)) {
        return *strict;
    }
    std::string out;
    std::size_t i = 0;
    while (i < size) {
        const std::uint8_t lead = data[i];
        const std::size_t step = IsCp932SingleByte(lead) ? kSingleByteStep : kDoubleByteStep;
        if (i + step <= size) {
            if (const auto piece = Cp932ToUtf8Strict(data + i, step)) {
                out += *piece;
                i += step;
                continue;
            }
        }
        out += "\xEF\xBF\xBD";
        ++i;
    }
    return out;
}

std::string Cp932ToUtf8(const Bytes& bytes) {
    return Cp932ToUtf8(bytes.data(), bytes.size());
}

std::string Cp932ToUtf8(std::string_view raw_bytes) {
    return Cp932ToUtf8(
        reinterpret_cast<const std::uint8_t*>(raw_bytes.data()),
        raw_bytes.size());
}

Bytes Utf8ToCp932(std::string_view utf8) {
    const std::wstring wide = Utf8ToWide(utf8);
    if (wide.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(
        kCodePageCp932, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    Bytes out(static_cast<std::size_t>(len));
    WideCharToMultiByte(
        kCodePageCp932, 0, wide.data(), static_cast<int>(wide.size()),
        reinterpret_cast<char*>(out.data()), len, nullptr, nullptr);
    return out;
}

char32_t Utf8NextCodepoint(std::string_view utf8, std::size_t& index) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(utf8.data());
    const std::uint8_t lead = bytes[index];
    if (lead < kUtf8OneByteLimit) {
        ++index;
        return lead;
    }
    const int extra = (lead >= kUtf8FourBytePrefix) ? kUtf8Extra3
                    : (lead >= kUtf8ThreeBytePrefix) ? kUtf8Extra2
                    : (lead >= kUtf8TwoBytePrefix) ? kUtf8Extra1
                    : 0;
    if (extra == 0 || index + static_cast<std::size_t>(extra) >= utf8.size()) {
        ++index;
        return lead;
    }
    char32_t cp = lead & (kUtf8TrailMask >> extra);
    for (int k = 1; k <= extra; ++k) {
        cp = (cp << kUtf8Shift6) | (bytes[index + static_cast<std::size_t>(k)] & kUtf8TrailMask);
    }
    index += static_cast<std::size_t>(extra) + kSingleByteStep;
    return cp;
}

int JapaneseCharCount(std::string_view utf8) {
    int count = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = Utf8NextCodepoint(utf8, i);
        const bool is_kana_or_kanji =
            (cp >= kHiraganaMin && cp <= kHiraganaMax) ||
            (cp >= kKatakanaMin && cp <= kKatakanaMax) ||
            (cp >= kCjkUnifiedMin && cp <= kCjkUnifiedMax) ||
            (cp >= kCjkExtAMin && cp <= kCjkExtAMax);
        if (is_kana_or_kanji) {
            ++count;
        }
    }
    return count;
}

bool HasJapanese(std::string_view utf8) {
    return JapaneseCharCount(utf8) > 0;
}

bool HasSobJapanese(std::string_view utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = Utf8NextCodepoint(utf8, i);
        if ((cp >= kCjkSymbolsMin && cp <= kCjkUnifiedMax) ||
            (cp >= kFullwidthFormsMin && cp <= kFullwidthFormsMax)) {
            return true;
        }
    }
    return false;
}

bool IsPrintableScriptText(std::string_view utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = Utf8NextCodepoint(utf8, i);
        const bool allowed = cp >= kPrintableAsciiMin ||
                             cp == kSpaceCr || cp == kSpaceLf || cp == kSpaceTab;
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool IsUnicodeSpace(char32_t codepoint) {
    switch (codepoint) {
    case kSpaceTab:
    case kSpaceLf:
    case kSpaceVt:
    case kSpaceFf:
    case kSpaceCr:
    case kSpaceFs:
    case kSpaceGs:
    case kSpaceRs:
    case kSpaceUs:
    case kSpaceAscii:
    case kSpaceNel:
    case kSpaceNbsp:
    case kSpaceOgham:
    case kSpaceLineSep:
    case kSpaceParaSep:
    case kSpaceNarrowNbsp:
    case kSpaceMediumMath:
    case kSpaceIdeographic:
        return true;
    default:
        return codepoint >= kSpaceEnQuadMin && codepoint <= kSpaceHairMax;
    }
}

std::string TrimUnicode(std::string_view utf8) {
    std::vector<std::size_t> starts;
    std::vector<char32_t> codepoints;
    std::size_t i = 0;
    while (i < utf8.size()) {
        starts.push_back(i);
        codepoints.push_back(Utf8NextCodepoint(utf8, i));
    }
    std::size_t begin = 0;
    std::size_t end = codepoints.size();
    while (begin < end && IsUnicodeSpace(codepoints[begin])) {
        ++begin;
    }
    while (end > begin && IsUnicodeSpace(codepoints[end - 1])) {
        --end;
    }
    if (begin == end) {
        return {};
    }
    const std::size_t byte_begin = starts[begin];
    const std::size_t byte_end = (end == codepoints.size()) ? utf8.size() : starts[end];
    return std::string(utf8.substr(byte_begin, byte_end - byte_begin));
}

void LogInfo(std::string_view message) {
    std::string line = CurrentTimestamp();
    line += " INFO ";
    line.append(message.data(), message.size());
    line += '\n';
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

void PrintLine(std::string_view message) {
    std::string line(message.data(), message.size());
    line += '\n';
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

void SetupConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

std::string FormatWithCommas(long long value) {
    const std::string digits = std::to_string(value < 0 ? -value : value);
    std::string reversed;
    int group_count = 0;
    for (std::size_t i = digits.size(); i-- > 0;) {
        reversed += digits[i];
        if (++group_count % kCommaGroupSize == 0 && i != 0) {
            reversed += ',';
        }
    }
    if (value < 0) {
        reversed += '-';
    }
    return std::string(reversed.rbegin(), reversed.rend());
}

std::string JsonPretty(const boost::json::value& value, int indent) {
    std::string out;
    FormatJsonPrettyImpl(value, out, indent, 0);
    return out;
}

std::string JsonDump(const boost::json::value& value) {
    std::string out;
    FormatJsonDumpImpl(value, out);
    return out;
}

boost::json::value JsonParseFile(const std::string& path) {
    const Bytes raw = ReadFileBytes(path);
    try {
        return boost::json::parse(boost::json::string_view(
            reinterpret_cast<const char*>(raw.data()), raw.size()));
    } catch (const std::exception& ex) {
        throw std::runtime_error("cannot parse " + path + ": " + ex.what());
    }
}

}  // namespace oretsuba
