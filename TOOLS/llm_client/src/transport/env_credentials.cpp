// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "transport/env_credentials.h"

#include "core/ascii_text.h"
#include "vn/llm/llm_client.h"

#include <windows.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace vn::llm {
namespace {

namespace fs = std::filesystem;

constexpr const char* kGeminiEnvVarName = "GEMINI_API_KEY";
constexpr std::string_view kExportPrefix = "export";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

constexpr std::size_t kOpeningQuoteIndex = 1U;
constexpr std::size_t kNextCharStep = 1U;
constexpr std::size_t kStartOffset = 0U;
constexpr std::size_t kRegistryKeyBufferChars = 512U;
constexpr int kInvalidUtf8ConversionLength = 0;

[[nodiscard]] static std::string parse_double_quoted_dotenv(
    std::string_view rhs) {
    std::string out;
    out.reserve(rhs.size());
    for (std::size_t i = kOpeningQuoteIndex; i < rhs.size(); ++i) {
        const char ch = rhs[i];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\' && i + kNextCharStep < rhs.size()) {
            const char next = rhs[++i];
            switch (next) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default:
                out += '\\';
                out += next;
                break;
            }
            continue;
        }
        out += ch;
    }
    return out;
}

[[nodiscard]] static std::string parse_dotenv_rhs(std::string_view rhs) {
    rhs = detail::trim_ascii_view(rhs);
    if (rhs.empty()) {
        return {};
    }
    if (rhs.front() == '\'') {
        const std::size_t close_quote = rhs.find('\'', kOpeningQuoteIndex);
        if (close_quote != std::string_view::npos) {
            return std::string(
                rhs.substr(kOpeningQuoteIndex, close_quote - kOpeningQuoteIndex));
        }
        return std::string(rhs.substr(kOpeningQuoteIndex));
    }
    if (rhs.front() == '"') {
        return parse_double_quoted_dotenv(rhs);
    }

    const std::size_t hash_pos = rhs.find('#');
    if (hash_pos != std::string_view::npos) {
        rhs = rhs.substr(kStartOffset, hash_pos);
    }
    return std::string(detail::trim_ascii_view(rhs));
}

[[nodiscard]] static std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    if (wide.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int src_len = static_cast<int>(wide.size());
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), src_len,
        nullptr, kInvalidUtf8ConversionLength, nullptr, nullptr);
    if (needed <= kInvalidUtf8ConversionLength) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), src_len,
        out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return out;
}

[[nodiscard]] static std::string read_dotenv_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

[[nodiscard]] static fs::path default_user_dotenv_path() {
    if (const wchar_t* home = ::_wgetenv(L"USERPROFILE");
        home != nullptr && *home != L'\0') {
        return fs::path(home) / ".gemini" / ".env";
    }
    if (const wchar_t* posix_home = ::_wgetenv(L"HOME");
        posix_home != nullptr && *posix_home != L'\0') {
        return fs::path(posix_home) / ".gemini" / ".env";
    }
    return {};
}

}  // namespace

std::string parse_dotenv_value(std::string_view dotenv_contents,
                               std::string_view key_name) {
    key_name = detail::trim_ascii_view(key_name);
    if (key_name.empty()) {
        return {};
    }

    if (dotenv_contents.starts_with(kUtf8Bom)) {
        dotenv_contents.remove_prefix(kUtf8Bom.size());
    }

    std::string resolved;
    std::size_t pos = kStartOffset;
    while (pos <= dotenv_contents.size()) {
        const std::size_t nl = dotenv_contents.find('\n', pos);
        const std::string_view line_raw =
            (nl == std::string_view::npos)
                ? dotenv_contents.substr(pos)
                : dotenv_contents.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos)
                  ? (dotenv_contents.size() + kNextCharStep)
                  : (nl + kNextCharStep);

        std::string_view line = detail::trim_ascii_view(line_raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.size() > kExportPrefix.size() &&
            line.starts_with(kExportPrefix) &&
            (line[kExportPrefix.size()] == ' ' ||
             line[kExportPrefix.size()] == '\t')) {
            line = detail::trim_ascii_view(line.substr(kExportPrefix.size()));
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view lhs =
            detail::trim_ascii_view(line.substr(kStartOffset, eq));
        if (lhs != key_name) {
            continue;
        }
        resolved = parse_dotenv_rhs(line.substr(eq + kNextCharStep));
    }
    return resolved;
}

std::string resolve_gemini_api_key(
    const std::filesystem::path& dotenv_override) {
    if (!dotenv_override.empty()) {
        return parse_dotenv_value(read_dotenv_file(dotenv_override),
                                  kGeminiEnvVarName);
    }

    if (const wchar_t* env_val = ::_wgetenv(L"GEMINI_API_KEY");
        env_val != nullptr && *env_val != L'\0') {
        const std::string utf8 = wide_to_utf8_strict(env_val);
        const std::string_view trimmed = detail::trim_ascii_view(utf8);
        if (!trimmed.empty()) {
            return std::string(trimmed);
        }
    }

    const fs::path dotenv_path = default_user_dotenv_path();
    if (!dotenv_path.empty()) {
        const std::string key = parse_dotenv_value(read_dotenv_file(dotenv_path),
                                                   kGeminiEnvVarName);
        if (!key.empty()) {
            return key;
        }
    }

    wchar_t reg_buf[kRegistryKeyBufferChars]{};
    DWORD reg_sz = sizeof(reg_buf);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"GEMINI_API_KEY",
                       RRF_RT_REG_SZ, nullptr, reg_buf, &reg_sz) ==
        ERROR_SUCCESS) {
        const std::string utf8 = wide_to_utf8_strict(reg_buf);
        const std::string_view trimmed = detail::trim_ascii_view(utf8);
        if (!trimmed.empty()) {
            return std::string(trimmed);
        }
    }
    return {};
}

}  // namespace vn::llm
