// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/menu_patch_builder.h"

#include "bgi/bgi_archive.h"
#include "bgi/bgi_cbg.h"
#include "bgi/bgi_sysgrp.h"
#include "common/util.h"
#include "menu/bgra_image.h"
#include "menu/menu_catalog.h"
#include "menu/menu_renderer.h"
#include "menu/sprite_canvas.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace snu::menu {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::uint8_t, 8> kPngSignature = {
    0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
constexpr std::string_view kArc20Signature = "BURIKO ARC20";
constexpr std::string_view kDscSignature = "DSC FORMAT 1.00";
constexpr std::string_view kCbgSignature = "CompressedBG___";

constexpr std::uint32_t kDefaultDscHashKey = 0x0007D4C2u;
constexpr std::size_t kDscKeyOffset = 0x10u;
constexpr std::size_t kDscMinHeaderForKey = 0x14u;
constexpr std::size_t kCbgShapeMinBytes = 0x16u;
constexpr std::size_t kBmpBppOffset = 28u;
constexpr std::size_t kBmpMinHeaderBytes = 30u;
constexpr int kSplashOutputBpp = 24;
constexpr int kChoiceOutputBpp = 32;

// Record wire layout sizes after 32-byte header:
//   u16 src_len + src + u16 dst_len + dst + u16 entry_len + entry
//   + u64 orig_offset + u64 orig_size + u32 orig_hash
//   + u64 new_size + u32 new_hash + u32 flags (= 36 metadata bytes)
//   + xor_payload[new_size]
constexpr std::size_t kRecordFixedFieldsBytes =
    8u + 8u + 4u + 8u + 4u + 4u;
constexpr std::size_t kEscapedBackslashMultiplier = 2u;

// Contact-sheet visual constants:
constexpr BgraColor kSheetPageBg{0x18, 0x1A, 0x1F, 0xFF};
constexpr BgraColor kSheetPanelBorder{0x46, 0x4C, 0x58, 0xFF};
constexpr BgraColor kSheetCheckerDark{0x32, 0x35, 0x3C, 0xFF};
constexpr BgraColor kSheetCheckerLight{0x4E, 0x53, 0x5D, 0xFF};
constexpr BgraColor kSheetCaptionColor{0xEA, 0xEE, 0xF5, 0xFF};
constexpr BgraColor kSheetSubCaptionColor{0x9E, 0xAA, 0xBC, 0xFF};
constexpr BgraColor kSheetDiffZero{0x1C, 0x1E, 0x24, 0xFF};
constexpr int kSheetCheckerCellPx = 8;
constexpr int kSheetOuterPadPx = 12;
constexpr int kSheetGapPx = 10;
constexpr int kSheetHeaderH = 16;
constexpr int kSheetDiffAmp = 4;

constexpr std::array<std::array<std::uint8_t, 7>, 95> kBitmapFont5x7 = {{
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},  // ' '
    {{0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},  // '!'
    {{0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}},  // '"'
    {{0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},  // '#'
    {{0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}},  // '$'
    {{0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},  // '%'
    {{0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},  // '&'
    {{0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},  // '\''
    {{0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},  // '('
    {{0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},  // ')'
    {{0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},  // '*'
    {{0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},  // '+'
    {{0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},  // ','
    {{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},  // '-'
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},  // '.'
    {{0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},  // '/'
    {{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},  // '0'
    {{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},  // '1'
    {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},  // '2'
    {{0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},  // '3'
    {{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},  // '4'
    {{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},  // '5'
    {{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},  // '6'
    {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},  // '7'
    {{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},  // '8'
    {{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},  // '9'
    {{0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},  // ':'
    {{0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}},  // ';'
    {{0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},  // '<'
    {{0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},  // '='
    {{0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},  // '>'
    {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},  // '?'
    {{0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E}},  // '@'
    {{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},  // 'A'
    {{0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},  // 'B'
    {{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},  // 'C'
    {{0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},  // 'D'
    {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},  // 'E'
    {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},  // 'F'
    {{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},  // 'G'
    {{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},  // 'H'
    {{0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},  // 'I'
    {{0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},  // 'J'
    {{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},  // 'K'
    {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},  // 'L'
    {{0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},  // 'M'
    {{0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},  // 'N'
    {{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},  // 'O'
    {{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},  // 'P'
    {{0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},  // 'Q'
    {{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},  // 'R'
    {{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},  // 'S'
    {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},  // 'T'
    {{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},  // 'U'
    {{0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},  // 'V'
    {{0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},  // 'W'
    {{0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},  // 'X'
    {{0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},  // 'Y'
    {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},  // 'Z'
    {{0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},  // '['
    {{0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}},  // '\\'
    {{0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},  // ']'
    {{0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}},  // '^'
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},  // '_'
    {{0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00}},  // '`'
    {{0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},  // 'a'
    {{0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E}},  // 'b'
    {{0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}},  // 'c'
    {{0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F}},  // 'd'
    {{0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},  // 'e'
    {{0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}},  // 'f'
    {{0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}},  // 'g'
    {{0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}},  // 'h'
    {{0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},  // 'i'
    {{0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}},  // 'j'
    {{0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}},  // 'k'
    {{0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},  // 'l'
    {{0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}},  // 'm'
    {{0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}},  // 'n'
    {{0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},  // 'o'
    {{0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10}},  // 'p'
    {{0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01}},  // 'q'
    {{0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},  // 'r'
    {{0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}},  // 's'
    {{0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06}},  // 't'
    {{0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}},  // 'u'
    {{0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}},  // 'v'
    {{0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}},  // 'w'
    {{0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},  // 'x'
    {{0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E}},  // 'y'
    {{0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}},  // 'z'
    {{0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02}},  // '{'
    {{0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},  // '|'
    {{0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08}},  // '}'
    {{0x00, 0x08, 0x15, 0x02, 0x00, 0x00, 0x00}},  // '~'
}};

std::uint16_t ReadLe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << 8u));
}

std::uint32_t ReadLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

std::uint64_t ReadLe64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(ReadLe32(p)) |
           (static_cast<std::uint64_t>(ReadLe32(p + 4u)) << 32u);
}

bool ContainsSpan(menu_patch_format::ByteSpan haystack,
                  const std::uint8_t* needle,
                  std::size_t needle_len) noexcept {
    if (needle_len == 0u || haystack.size() < needle_len) {
        return false;
    }
    const auto it = std::search(haystack.begin(), haystack.end(),
                                needle, needle + needle_len);
    return it != haystack.end();
}

bool ContainsAscii(menu_patch_format::ByteSpan haystack,
                   std::string_view needle) noexcept {
    return ContainsSpan(
        haystack, reinterpret_cast<const std::uint8_t*>(needle.data()),
        needle.size());
}

fs::path ResolveUnpatchedArchivePath(const fs::path& game_dir,
                                     std::string_view rel_arc_name) {
    const fs::path base = game_dir / fs::u8path(rel_arc_name);
    const fs::path bak = fs::path(base.wstring() + L".bak");
    const fs::path backup = fs::path(base.wstring() + L".backup");
    std::error_code ec;
    if (fs::is_regular_file(bak, ec)) {
        return bak;
    }
    if (fs::is_regular_file(backup, ec)) {
        return backup;
    }
    return base;
}

class ScopedProcessHandles {
public:
    explicit ScopedProcessHandles(const PROCESS_INFORMATION& pi) noexcept
        : process_(pi.hProcess), thread_(pi.hThread) {}
    ScopedProcessHandles(const ScopedProcessHandles&) = delete;
    ScopedProcessHandles& operator=(const ScopedProcessHandles&) = delete;
    ~ScopedProcessHandles() {
        if (thread_ != nullptr && thread_ != INVALID_HANDLE_VALUE) {
            CloseHandle(thread_);
        }
        if (process_ != nullptr && process_ != INVALID_HANDLE_VALUE) {
            CloseHandle(process_);
        }
    }
    HANDLE process() const noexcept { return process_; }

private:
    HANDLE process_;
    HANDLE thread_;
};

class ScopedTempDir {
public:
    ScopedTempDir() {
        const DWORD pid = GetCurrentProcessId();
        const ULONGLONG tick = GetTickCount64();
        path_ = fs::temp_directory_path() /
                fs::u8path("snu_menu_cbg_" + std::to_string(pid) + "_" +
                           std::to_string(tick));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::wstring QuoteWideArg(std::wstring_view arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0u;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * kEscapedBackslashMultiplier + 1u, L'\\');
            quoted.push_back(L'"');
            backslashes = 0u;
            continue;
        }
        quoted.append(backslashes, L'\\');
        quoted.push_back(ch);
        backslashes = 0u;
    }
    quoted.append(backslashes * kEscapedBackslashMultiplier, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

fs::path LocateExtractCbgTool(const fs::path& project_dir) {
    auto search_up = [](fs::path start) -> fs::path {
        std::error_code ec;
        for (int depth = 0; depth < 8 && !start.empty(); ++depth) {
            const fs::path candidate =
                start / "TOOLS" / "garbro" / "extract_cbg.exe";
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
            if (start == start.root_path()) {
                break;
            }
            start = start.parent_path();
        }
        return {};
    };

    if (!project_dir.empty()) {
        if (fs::path found = search_up(project_dir); !found.empty()) {
            return found;
        }
    }

    std::wstring exe_buf(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe_buf.data(),
                                       static_cast<DWORD>(exe_buf.size()));
    if (n > 0 && n < exe_buf.size()) {
        exe_buf.resize(n);
        if (fs::path found = search_up(fs::path(exe_buf).parent_path());
            !found.empty()) {
            return found;
        }
    }
    return {};
}

std::optional<BgraImage> DecodeCbgViaExternalTool(const Bytes& raw_cbg,
                                                  const fs::path& project_dir) {
    const fs::path tool_path = LocateExtractCbgTool(project_dir);
    if (tool_path.empty()) {
        return std::nullopt;
    }
    const ScopedTempDir temp_dir;
    const fs::path in_cbg = temp_dir.path() / "entry.cbg";
    const fs::path out_bmp = temp_dir.path() / "entry.bmp";
    write_file(in_cbg.u8string(), raw_cbg);

    const std::wstring exe_w = tool_path.wstring();
    std::wstring cmd_w = L"\"" + exe_w + L"\" " +
                         QuoteWideArg(in_cbg.wstring()) + L" " +
                         QuoteWideArg(out_bmp.wstring());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return std::nullopt;
    }
    const ScopedProcessHandles handles(pi);
    if (WaitForSingleObject(handles.process(), INFINITE) == WAIT_FAILED) {
        return std::nullopt;
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(handles.process(), &exit_code) || exit_code != 0) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::is_regular_file(out_bmp, ec)) {
        return std::nullopt;
    }
    BgraImage img = BgraImage::DecodeBmpOrPng(read_file(out_bmp.u8string()));
    // extract_cbg.exe emits 32-bpp BGRX BMPs with alpha == 0 for 24-bpp CBGs.
    const bool cbg_is_24bpp =
        (raw_cbg.size() >= kCbgShapeMinBytes &&
         ReadLe16(raw_cbg.data() + 0x14u) == 24u);
    if (cbg_is_24bpp || img.NonZeroAlphaBounds().empty()) {
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                BgraColor p = img.PixelAt(x, y);
                p.a = kOpaqueAlpha;
                img.SetPixel(x, y, p);
            }
        }
    }
    return img;
}

BgraImage DecodeSplashOrChoiceImage(const Bytes& raw_entry,
                                    const fs::path& project_dir) {
    Bytes payload = raw_entry;
    if (bgi::is_dsc(payload)) {
        payload = bgi::dsc_decompress(payload);
    }
    if (payload.size() >= 2u &&
        ((payload[0] == 'B' && payload[1] == 'M') ||
         (payload.size() >= kPngSignature.size() &&
          std::memcmp(payload.data(), kPngSignature.data(),
                      kPngSignature.size()) == 0))) {
        return BgraImage::DecodeBmpOrPng(payload);
    }
    if (bgi::is_cbg(payload)) {
        try {
            const bgi::CbgImage cbg = bgi::decode_cbg(payload);
            const Bytes bmp =
                bgi::to_bmp(cbg.pixels, cbg.width, cbg.height, cbg.bpp);
            return BgraImage::DecodeBmpOrPng(bmp);
        } catch (...) {
            if (auto via_tool = DecodeCbgViaExternalTool(payload, project_dir)) {
                return std::move(*via_tool);
            }
            throw std::runtime_error(
                "Failed to decode CompressedBG v2 entry (extract_cbg.exe unavailable)");
        }
    }
    throw std::runtime_error("Unsupported image container in archive entry");
}

struct ImageShape {
    int width = 0;
    int height = 0;
    int bpp = 32;
};

ImageShape ReadEntryImageShape(const Bytes& raw_entry) {
    Bytes payload = raw_entry;
    if (bgi::is_dsc(payload)) {
        payload = bgi::dsc_decompress(payload);
    }
    if (bgi::is_cbg(payload) && payload.size() >= kCbgShapeMinBytes) {
        ImageShape s;
        s.width = static_cast<int>(ReadLe16(payload.data() + 0x10u));
        s.height = static_cast<int>(ReadLe16(payload.data() + 0x12u));
        s.bpp = static_cast<int>(ReadLe16(payload.data() + 0x14u));
        return s;
    }
    if (payload.size() >= kBmpMinHeaderBytes && payload[0] == 'B' &&
        payload[1] == 'M') {
        const BgraImage decoded = BgraImage::DecodeBmpOrPng(payload);
        ImageShape s;
        s.width = decoded.width();
        s.height = decoded.height();
        s.bpp = static_cast<int>(ReadLe16(payload.data() + kBmpBppOffset));
        return s;
    }
    const BgraImage decoded = BgraImage::DecodeBmpOrPng(payload);
    return ImageShape{decoded.width(), decoded.height(), 32};
}

// ---- Contact-sheet rendering helpers ---------------------------------------

void FillImageRect(BgraImage& dst, const RectI& r, BgraColor c) {
    const int x0 = std::clamp(r.x, 0, dst.width());
    const int y0 = std::clamp(r.y, 0, dst.height());
    const int x1 = std::clamp(r.x + r.w, x0, dst.width());
    const int y1 = std::clamp(r.y + r.h, y0, dst.height());
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            dst.SetPixel(x, y, c);
        }
    }
}

void DrawAscii5x7(BgraImage& dst, int x, int y, std::string_view text,
                  BgraColor color, int max_w) {
    int pen_x = x;
    for (const unsigned char uch : text) {
        if (pen_x + 5 > x + max_w) {
            break;
        }
        const std::size_t idx =
            (uch >= 32u && uch <= 126u) ? static_cast<std::size_t>(uch - 32u)
                                        : static_cast<std::size_t>('?' - 32);
        const auto& glyph = kBitmapFont5x7[idx];
        for (int gy = 0; gy < 7; ++gy) {
            const int py = y + gy;
            if (py < 0 || py >= dst.height()) {
                continue;
            }
            for (int gx = 0; gx < 5; ++gx) {
                if ((glyph[static_cast<std::size_t>(gy)] &
                     (1u << (4 - gx))) != 0u) {
                    const int px = pen_x + gx;
                    if (px >= 0 && px < dst.width()) {
                        dst.SetPixel(px, py, color);
                    }
                }
            }
        }
        pen_x += 6;
    }
}

BgraImage DownscaleImageBox(const BgraImage& src, int max_w, int max_h) {
    if (src.empty()) {
        return BgraImage(1, 1, BgraColor{0, 0, 0, 0});
    }
    if (src.width() <= max_w && src.height() <= max_h) {
        return src;
    }
    const double scale = std::min(
        static_cast<double>(max_w) / static_cast<double>(src.width()),
        static_cast<double>(max_h) / static_cast<double>(src.height()));
    const int dst_w = std::max(1, static_cast<int>(std::lround(src.width() * scale)));
    const int dst_h = std::max(1, static_cast<int>(std::lround(src.height() * scale)));

    BgraImage dst(dst_w, dst_h, BgraColor{0, 0, 0, 0});
    for (int dy = 0; dy < dst_h; ++dy) {
        const int sy0 = (dy * src.height()) / dst_h;
        const int sy1 = std::max(sy0 + 1, ((dy + 1) * src.height()) / dst_h);
        for (int dx = 0; dx < dst_w; ++dx) {
            const int sx0 = (dx * src.width()) / dst_w;
            const int sx1 = std::max(sx0 + 1, ((dx + 1) * src.width()) / dst_w);
            double sum_pb = 0.0;
            double sum_pg = 0.0;
            double sum_pr = 0.0;
            double sum_a = 0.0;
            int count = 0;
            for (int sy = sy0; sy < std::min(src.height(), sy1); ++sy) {
                for (int sx = sx0; sx < std::min(src.width(), sx1); ++sx) {
                    const BgraColor p = src.PixelAt(sx, sy);
                    const double a = static_cast<double>(p.a);
                    sum_pb += static_cast<double>(p.b) * a;
                    sum_pg += static_cast<double>(p.g) * a;
                    sum_pr += static_cast<double>(p.r) * a;
                    sum_a += a;
                    ++count;
                }
            }
            if (count <= 0 || sum_a <= 0.5) {
                dst.SetPixel(dx, dy, BgraColor{0, 0, 0, 0});
                continue;
            }
            const auto out_a = static_cast<std::uint8_t>(
                std::clamp(std::lround(sum_a / count), 0L, 255L));
            if (out_a == 0u) {
                dst.SetPixel(dx, dy, BgraColor{0, 0, 0, 0});
                continue;
            }
            dst.SetPixel(
                dx, dy,
                BgraColor{
                    static_cast<std::uint8_t>(
                        std::clamp(std::lround(sum_pb / sum_a), 0L, 255L)),
                    static_cast<std::uint8_t>(
                        std::clamp(std::lround(sum_pg / sum_a), 0L, 255L)),
                    static_cast<std::uint8_t>(
                        std::clamp(std::lround(sum_pr / sum_a), 0L, 255L)),
                    out_a,
                });
        }
    }
    return dst;
}

void BlitOverCheckerPanel(BgraImage& page, int px, int py, int pw, int ph,
                          const BgraImage& sprite) {
    FillImageRect(page, RectI{px - 1, py - 1, pw + 2, ph + 2}, kSheetPanelBorder);
    for (int ry = 0; ry < ph; ++ry) {
        for (int rx = 0; rx < pw; ++rx) {
            const bool even =
                (((rx / kSheetCheckerCellPx) + (ry / kSheetCheckerCellPx)) & 1) == 0;
            page.SetPixel(px + rx, py + ry,
                          even ? kSheetCheckerDark : kSheetCheckerLight);
        }
    }
    const int ox = px + (pw - sprite.width()) / 2;
    const int oy = py + (ph - sprite.height()) / 2;
    CompositeStraightOverStraight(page, sprite, ox, oy);
}

struct ComparisonRowSpec {
    std::string label;
    BgraImage orig;
    BgraImage patched;
};

BgraImage RenderComparisonSheet(const std::vector<ComparisonRowSpec>& rows,
                                int max_panel_w,
                                int max_panel_h) {
    if (rows.empty()) {
        return BgraImage(1, 1, kSheetPageBg);
    }
    struct Prepared {
        std::string caption;
        BgraImage orig_fit;
        BgraImage patched_fit;
        BgraImage diff_fit;
        int row_h = 1;
    };
    std::vector<Prepared> prep;
    prep.reserve(rows.size());
    int col_w = 120;
    int total_h = kSheetOuterPadPx * 2 + kSheetHeaderH;

    for (const auto& r : rows) {
        const int dw = std::max({1, r.orig.width(), r.patched.width()});
        const int dh = std::max({1, r.orig.height(), r.patched.height()});
        BgraImage diff(dw, dh, kSheetDiffZero);
        std::size_t diff_px = 0u;
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const BgraColor o = (x < r.orig.width() && y < r.orig.height())
                                        ? r.orig.PixelAt(x, y)
                                        : BgraColor{};
                const BgraColor p =
                    (x < r.patched.width() && y < r.patched.height())
                        ? r.patched.PixelAt(x, y)
                        : BgraColor{};
                const int db = std::abs(static_cast<int>(o.b) - p.b);
                const int dg = std::abs(static_cast<int>(o.g) - p.g);
                const int dr = std::abs(static_cast<int>(o.r) - p.r);
                const int da = std::abs(static_cast<int>(o.a) - p.a);
                const int mx = std::max({db, dg, dr, da});
                if (mx == 0) {
                    continue;
                }
                ++diff_px;
                const auto sc =
                    static_cast<std::uint8_t>(std::min(255, mx * kSheetDiffAmp));
                const auto sa =
                    static_cast<std::uint8_t>(std::min(255, da * kSheetDiffAmp));
                diff.SetPixel(x, y,
                              BgraColor{sa, static_cast<std::uint8_t>(sc / 3),
                                        sc, 255u});
            }
        }

        Prepared pr;
        pr.orig_fit = DownscaleImageBox(r.orig, max_panel_w, max_panel_h);
        pr.patched_fit = DownscaleImageBox(r.patched, max_panel_w, max_panel_h);
        pr.diff_fit = DownscaleImageBox(diff, max_panel_w, max_panel_h);
        col_w = std::max({col_w, pr.orig_fit.width(), pr.patched_fit.width(),
                          pr.diff_fit.width()});
        pr.row_h = std::max({pr.orig_fit.height(), pr.patched_fit.height(),
                             pr.diff_fit.height()});
        std::ostringstream oss;
        oss << r.label << " [" << r.patched.width() << "x" << r.patched.height()
            << ", diff_px=" << diff_px << "]";
        pr.caption = oss.str();
        total_h += kSheetHeaderH + pr.row_h + 2 + kSheetGapPx;
        prep.push_back(std::move(pr));
    }

    const int col_stride = col_w + 2 + kSheetGapPx;
    const int page_w = kSheetOuterPadPx * 2 + 3 * col_stride - kSheetGapPx;
    BgraImage page(page_w, total_h, kSheetPageBg);

    DrawAscii5x7(page, kSheetOuterPadPx, kSheetOuterPadPx, "ORIGINAL",
                 kSheetSubCaptionColor, col_w);
    DrawAscii5x7(page, kSheetOuterPadPx + col_stride, kSheetOuterPadPx, "PATCHED",
                 kSheetSubCaptionColor, col_w);
    DrawAscii5x7(page, kSheetOuterPadPx + 2 * col_stride, kSheetOuterPadPx,
                 "AMPLIFIED DIFF", kSheetSubCaptionColor, col_w);

    int cy = kSheetOuterPadPx + kSheetHeaderH;
    for (const auto& pr : prep) {
        DrawAscii5x7(page, kSheetOuterPadPx, cy + 2, pr.caption,
                     kSheetCaptionColor, page_w - 2 * kSheetOuterPadPx);
        cy += kSheetHeaderH + 1;
        BlitOverCheckerPanel(page, kSheetOuterPadPx + 1, cy, col_w, pr.row_h,
                             pr.orig_fit);
        BlitOverCheckerPanel(page, kSheetOuterPadPx + col_stride + 1, cy, col_w,
                             pr.row_h, pr.patched_fit);
        BlitOverCheckerPanel(page, kSheetOuterPadPx + 2 * col_stride + 1, cy,
                             col_w, pr.row_h, pr.diff_fit);
        cy += pr.row_h + 1 + kSheetGapPx;
    }
    return page;
}

BgraImage RenderGridSheet(
    const std::vector<std::pair<std::string, BgraImage>>& cells,
    int columns,
    int max_cell_w,
    int max_cell_h) {
    if (cells.empty()) {
        return BgraImage(1, 1, kSheetPageBg);
    }
    const int cols = std::max(1, columns);
    const int rows = static_cast<int>(
        (cells.size() + static_cast<std::size_t>(cols) - 1u) /
        static_cast<std::size_t>(cols));

    std::vector<BgraImage> fitted;
    fitted.reserve(cells.size());
    int cell_w = 120;
    int cell_h = 24;
    for (const auto& [label, img] : cells) {
        (void)label;
        BgraImage fit = DownscaleImageBox(img, max_cell_w, max_cell_h);
        cell_w = std::max(cell_w, fit.width());
        cell_h = std::max(cell_h, fit.height());
        fitted.push_back(std::move(fit));
    }

    const int col_stride = cell_w + 2 + kSheetGapPx;
    const int row_stride = kSheetHeaderH + cell_h + 2 + kSheetGapPx;
    const int page_w = kSheetOuterPadPx * 2 + cols * col_stride - kSheetGapPx;
    const int page_h = kSheetOuterPadPx * 2 + rows * row_stride - kSheetGapPx;

    BgraImage page(page_w, page_h, kSheetPageBg);
    for (std::size_t i = 0u; i < cells.size(); ++i) {
        const int r = static_cast<int>(i) / cols;
        const int c = static_cast<int>(i) % cols;
        const int x0 = kSheetOuterPadPx + c * col_stride;
        const int y0 = kSheetOuterPadPx + r * row_stride;
        DrawAscii5x7(page, x0, y0 + 2, cells[i].first, kSheetCaptionColor, cell_w);
        BlitOverCheckerPanel(page, x0 + 1, y0 + kSheetHeaderH + 1, cell_w,
                             cell_h, fitted[i]);
    }
    return page;
}

}  // namespace

bool VerifyNoRawAssetSignatures(menu_patch_format::ByteSpan blob,
                                std::string* violation_out) {
    if (blob.size() <= menu_patch_format::kPatchHeaderBytes) {
        if (violation_out != nullptr) {
            *violation_out = "Container smaller than 32-byte header + body";
        }
        return false;
    }

    const menu_patch_format::ByteSpan body =
        blob.subspan(menu_patch_format::kPatchHeaderBytes);

    if (ContainsSpan(body, kPngSignature.data(), kPngSignature.size())) {
        if (violation_out != nullptr) {
            *violation_out = "Raw PNG signature detected in container body";
        }
        return false;
    }
    if (ContainsAscii(body, kArc20Signature)) {
        if (violation_out != nullptr) {
            *violation_out = "Raw BURIKO ARC20 signature detected in container body";
        }
        return false;
    }
    if (ContainsAscii(body, kDscSignature)) {
        if (violation_out != nullptr) {
            *violation_out =
                "Raw DSC FORMAT 1.00 signature detected in container body";
        }
        return false;
    }
    if (ContainsAscii(body, kCbgSignature)) {
        if (violation_out != nullptr) {
            *violation_out =
                "Raw CompressedBG___ signature detected in container body";
        }
        return false;
    }

    std::size_t zero_run = 0u;
    for (const std::uint8_t byte : body) {
        if (byte == 0u) {
            ++zero_run;
            if (zero_run >= kForbiddenContainerZeroRunBytes) {
                if (violation_out != nullptr) {
                    *violation_out =
                        "Run of >= 64 consecutive 0x00 bytes detected in container body";
                }
                return false;
            }
        } else {
            zero_run = 0u;
        }
    }

    // Walk every record header from +0x20 to inspect the start of xor_payload
    // for raw Windows BMP `"BM"` magic even on mutated test buffers.
    const std::uint32_t record_count = ReadLe32(blob.data() + 0x0Cu);
    std::size_t cur = menu_patch_format::kPatchHeaderBytes;
    for (std::uint32_t idx = 0u; idx < record_count && cur < blob.size(); ++idx) {
        if (blob.size() - cur < 2u) {
            break;
        }
        const std::uint16_t src_len = ReadLe16(blob.data() + cur);
        cur += 2u;
        if (blob.size() - cur < src_len + 2u) {
            break;
        }
        cur += src_len;
        const std::uint16_t dst_len = ReadLe16(blob.data() + cur);
        cur += 2u;
        if (blob.size() - cur < dst_len + 2u) {
            break;
        }
        cur += dst_len;
        const std::uint16_t entry_len = ReadLe16(blob.data() + cur);
        cur += 2u;
        if (blob.size() - cur < entry_len + kRecordFixedFieldsBytes) {
            break;
        }
        cur += entry_len;
        const std::uint64_t new_size = ReadLe64(blob.data() + cur + 20u);
        cur += kRecordFixedFieldsBytes;
        if (new_size >= 2u && blob.size() - cur >= 2u &&
            blob[cur] == static_cast<std::uint8_t>('B') &&
            blob[cur + 1u] == static_cast<std::uint8_t>('M')) {
            if (violation_out != nullptr) {
                *violation_out =
                    "Raw BMP 'BM' header detected at start of record payload";
            }
            return false;
        }
        if (new_size > blob.size() - cur) {
            break;
        }
        cur += static_cast<std::size_t>(new_size);
    }

    return true;
}

void AssertNoRawAssetSignatures(menu_patch_format::ByteSpan blob) {
    std::string violation;
    if (!VerifyNoRawAssetSignatures(blob, &violation)) {
        throw std::runtime_error(
            "SNUMENU1 raw-asset-signature verification failed: " + violation);
    }
}

void WriteBinaryFileAtomic(const fs::path& output_path,
                           menu_patch_format::ByteSpan blob) {
    if (output_path.empty()) {
        throw std::invalid_argument("WriteBinaryFileAtomic requires non-empty path");
    }
    if (output_path.has_parent_path() && !output_path.parent_path().empty()) {
        fs::create_directories(output_path.parent_path());
    }
    const fs::path tmp_path(output_path.wstring() + L".tmp");
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw std::runtime_error("Cannot open temporary output file: " +
                                     tmp_path.u8string());
        }
        if (!blob.empty()) {
            ofs.write(reinterpret_cast<const char*>(blob.data()),
                      static_cast<std::streamsize>(blob.size()));
        }
        ofs.flush();
        if (!ofs) {
            std::error_code rm_ec;
            fs::remove(tmp_path, rm_ec);
            throw std::runtime_error("Failed writing temporary output file: " +
                                     tmp_path.u8string());
        }
    }
    if (!MoveFileExW(tmp_path.c_str(), output_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        std::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        throw std::runtime_error("Atomic rename failed for " +
                                 output_path.u8string() + " (Win32 error " +
                                 std::to_string(err) + ")");
    }
}

struct MenuPatchBuilder::Impl {
    MenuRenderer renderer;
};

MenuPatchBuilder::MenuPatchBuilder() : impl_(std::make_unique<Impl>()) {}
MenuPatchBuilder::MenuPatchBuilder(MenuPatchBuilder&& other) noexcept = default;
MenuPatchBuilder& MenuPatchBuilder::operator=(MenuPatchBuilder&& other) noexcept =
    default;
MenuPatchBuilder::~MenuPatchBuilder() = default;

MenuPatchBuildSummary MenuPatchBuilder::Build(
    const MenuPatchBuildOptions& options,
    std::vector<std::uint8_t>* patch_bytes_out) const {
    if (options.game_dir.empty()) {
        throw std::invalid_argument("MenuPatchBuildOptions::game_dir is required");
    }

    const bool want_visuals =
        !options.contact_sheets_dir.empty() || !options.previews_dir.empty();

    std::vector<menu_patch_format::MenuPatchRecordInput> records;
    records.reserve(kExpectedRetailRecordCount);

    MenuPatchBuildSummary summary;

    // Visual caches populated for contact sheets and 1280x720 mockups:
    std::optional<BgraImage> orig_att01_img;
    std::optional<BgraImage> orig_att02_img;
    std::optional<BgraImage> patched_att01_img;
    std::optional<BgraImage> patched_att02_img;
    std::map<std::string, BgraImage> orig_sysgrp_imgs;
    std::map<std::string, BgraImage> patched_sysgrp_imgs;
    std::vector<std::pair<std::string, BgraImage>> patched_choice_imgs;

    // ------------------------------------------------------------------------
    // 1. Startup Splash Screens (`att01`, `att02` in `data02501.arc`)
    // ------------------------------------------------------------------------
    const fs::path splash_arc_path =
        ResolveUnpatchedArchivePath(options.game_dir, kSplashArcRelPath);
    std::error_code ec;
    if (fs::is_regular_file(splash_arc_path, ec)) {
        const Bytes splash_arc_bytes = read_file(splash_arc_path.u8string());
        std::uint64_t off1 = 0u;
        std::uint64_t sz1 = 0u;
        std::uint64_t off2 = 0u;
        std::uint64_t sz2 = 0u;
        const bool has1 =
            (menu_patch_format::FindArc20EntrySlice(
                 splash_arc_bytes, kSplash1EntryName, off1, sz1) ==
             menu_patch_format::ArcStatus::kOk);
        const bool has2 =
            (menu_patch_format::FindArc20EntrySlice(
                 splash_arc_bytes, kSplash2EntryName, off2, sz2) ==
             menu_patch_format::ArcStatus::kOk);

        if (has1 && has2) {
            const Bytes slice1(
                splash_arc_bytes.begin() + static_cast<std::ptrdiff_t>(off1),
                splash_arc_bytes.begin() +
                    static_cast<std::ptrdiff_t>(off1 + sz1));
            const Bytes slice2(
                splash_arc_bytes.begin() + static_cast<std::ptrdiff_t>(off2),
                splash_arc_bytes.begin() +
                    static_cast<std::ptrdiff_t>(off2 + sz2));
            orig_att01_img =
                DecodeSplashOrChoiceImage(slice1, options.project_dir);
            orig_att02_img =
                DecodeSplashOrChoiceImage(slice2, options.project_dir);

            SplashPairRenderResult pair = impl_->renderer.RenderSplashScreenPair(
                *orig_att01_img, *orig_att02_img);
            patched_att01_img = std::move(pair.att01);
            patched_att02_img = std::move(pair.att02);

            {
                menu_patch_format::MenuPatchRecordInput r1;
                r1.source_rel_path = std::string(kSplashArcRelPath);
                r1.target_rel_path = std::string(kSplash1EntryName);
                r1.entry_name = std::string(kSplash1EntryName);
                r1.orig_offset = off1;
                r1.orig_slice = slice1;
                r1.new_raw = patched_att01_img->EncodeBmp(kSplashOutputBpp);
                r1.flags = menu_patch_format::kFlagBackupStandalone;
                records.push_back(std::move(r1));
                ++summary.splash_records;
            }
            {
                menu_patch_format::MenuPatchRecordInput r2;
                r2.source_rel_path = std::string(kSplashArcRelPath);
                r2.target_rel_path = std::string(kSplash2EntryName);
                r2.entry_name = std::string(kSplash2EntryName);
                r2.orig_offset = off2;
                r2.orig_slice = slice2;
                r2.new_raw = patched_att02_img->EncodeBmp(kSplashOutputBpp);
                r2.flags = menu_patch_format::kFlagBackupStandalone;
                records.push_back(std::move(r2));
                ++summary.splash_records;
            }
        } else {
            for (const SplashScreenSpec& spec : SplashScreenCatalog()) {
                std::uint64_t off = 0u;
                std::uint64_t sz = 0u;
                if (menu_patch_format::FindArc20EntrySlice(
                        splash_arc_bytes, spec.entry_name, off, sz) !=
                    menu_patch_format::ArcStatus::kOk) {
                    continue;
                }
                const Bytes slice(
                    splash_arc_bytes.begin() + static_cast<std::ptrdiff_t>(off),
                    splash_arc_bytes.begin() +
                        static_cast<std::ptrdiff_t>(off + sz));
                const BgraImage orig_img =
                    DecodeSplashOrChoiceImage(slice, options.project_dir);
                const BgraImage patched_img =
                    impl_->renderer.RenderSingleSplashScreen(orig_img, spec);
                if (spec.entry_name == kSplash1EntryName) {
                    orig_att01_img = orig_img;
                    patched_att01_img = patched_img;
                } else {
                    orig_att02_img = orig_img;
                    patched_att02_img = patched_img;
                }
                menu_patch_format::MenuPatchRecordInput rec;
                rec.source_rel_path = std::string(kSplashArcRelPath);
                rec.target_rel_path = std::string(spec.entry_name);
                rec.entry_name = std::string(spec.entry_name);
                rec.orig_offset = off;
                rec.orig_slice = slice;
                rec.new_raw = patched_img.EncodeBmp(kSplashOutputBpp);
                rec.flags = menu_patch_format::kFlagBackupStandalone;
                records.push_back(std::move(rec));
                ++summary.splash_records;
            }
        }
    } else if (options.strict_retail_inventory) {
        throw std::runtime_error("Missing required archive: " +
                                 splash_arc_path.u8string());
    }

    // ------------------------------------------------------------------------
    // 2. System UI Sprites (`sysgrp.arc` -> `kFlagArchiveEntry`)
    // ------------------------------------------------------------------------
    const fs::path sysgrp_arc_path =
        ResolveUnpatchedArchivePath(options.game_dir, kSysGrpArcRelPath);
    if (fs::is_regular_file(sysgrp_arc_path, ec)) {
        const Bytes sysgrp_arc_bytes = read_file(sysgrp_arc_path.u8string());

        struct LoadedSysGrpEntry {
            std::uint64_t orig_offset = 0u;
            Bytes orig_slice;
            std::uint32_t dsc_key = kDefaultDscHashKey;
            bgi::SysGrpImage decoded;
        };

        auto load_sysgrp_entry = [&](std::string_view name)
            -> std::optional<LoadedSysGrpEntry> {
            std::uint64_t off = 0u;
            std::uint64_t sz = 0u;
            if (menu_patch_format::FindArc20EntrySlice(
                    sysgrp_arc_bytes, name, off, sz) !=
                menu_patch_format::ArcStatus::kOk) {
                return std::nullopt;
            }
            LoadedSysGrpEntry entry;
            entry.orig_offset = off;
            entry.orig_slice.assign(
                sysgrp_arc_bytes.begin() + static_cast<std::ptrdiff_t>(off),
                sysgrp_arc_bytes.begin() +
                    static_cast<std::ptrdiff_t>(off + sz));
            Bytes raw_sysgrp = entry.orig_slice;
            if (bgi::is_dsc(entry.orig_slice)) {
                if (entry.orig_slice.size() >= kDscMinHeaderForKey) {
                    entry.dsc_key =
                        ReadLe32(entry.orig_slice.data() + kDscKeyOffset);
                }
                raw_sysgrp = bgi::dsc_decompress(entry.orig_slice);
            }
            entry.decoded = bgi::decode_sysgrp(raw_sysgrp);
            return entry;
        };

        auto emit_sysgrp_record = [&](std::string_view name,
                                      LoadedSysGrpEntry& loaded,
                                      BgraImage patched) {
            const Bytes encoded = bgi::encode_sysgrp(
                patched, loaded.decoded.orig_bpp, loaded.decoded.reserved);
            Bytes compressed = bgi::dsc_compress(encoded, loaded.dsc_key);
            if (bgi::dsc_decompress(compressed) != encoded) {
                throw std::runtime_error(
                    "DSC compression round-trip self-check failed for " +
                    std::string(name));
            }
            if (want_visuals) {
                orig_sysgrp_imgs.emplace(std::string(name),
                                         loaded.decoded.image);
                patched_sysgrp_imgs.emplace(std::string(name),
                                            std::move(patched));
            }
            menu_patch_format::MenuPatchRecordInput rec;
            rec.source_rel_path = std::string(kSysGrpArcRelPath);
            rec.target_rel_path = std::string(kSysGrpArcRelPath);
            rec.entry_name = std::string(name);
            rec.orig_offset = loaded.orig_offset;
            rec.orig_slice = std::move(loaded.orig_slice);
            rec.new_raw = std::move(compressed);
            rec.flags = menu_patch_format::kFlagArchiveEntry;
            records.push_back(std::move(rec));
            ++summary.sysgrp_records;
        };

        // 2a. Title & Locked CG:
        if (auto e = load_sysgrp_entry(kTitleBgEntryName)) {
            BgraImage p = impl_->renderer.RenderTitleBackground(e->decoded.image);
            emit_sysgrp_record(kTitleBgEntryName, *e, std::move(p));
        }
        if (auto e = load_sysgrp_entry(kLockedCgThumbEntryName)) {
            BgraImage p =
                impl_->renderer.RenderLockedCgThumbnail(e->decoded.image);
            emit_sysgrp_record(kLockedCgThumbEntryName, *e, std::move(p));
        }

        // 2b. Config Background + 12 Toggle Strips + 8 Voice Portraits:
        if (auto e = load_sysgrp_entry(kConfigBgEntryName)) {
            BgraImage p =
                impl_->renderer.RenderConfigBackground(e->decoded.image);
            emit_sysgrp_record(kConfigBgEntryName, *e, std::move(p));
        }

        std::map<std::string, LoadedSysGrpEntry> loaded_toggles;
        std::map<std::string, BgraImage> toggle_orig_map;
        for (const ConfigToggleStripSpec& spec : ConfigToggleStripCatalog()) {
            if (auto e = load_sysgrp_entry(spec.entry_name)) {
                toggle_orig_map.emplace(std::string(spec.entry_name),
                                        e->decoded.image);
                loaded_toggles.emplace(std::string(spec.entry_name),
                                       std::move(*e));
            }
        }
        if (!toggle_orig_map.empty()) {
            std::map<std::string, BgraImage> rendered_toggles =
                impl_->renderer.RenderConfigToggleStrips(toggle_orig_map);
            for (const ConfigToggleStripSpec& spec : ConfigToggleStripCatalog()) {
                const std::string key(spec.entry_name);
                const auto lit = loaded_toggles.find(key);
                const auto rit = rendered_toggles.find(key);
                if (lit != loaded_toggles.end() &&
                    rit != rendered_toggles.end()) {
                    emit_sysgrp_record(spec.entry_name, lit->second,
                                       std::move(rit->second));
                }
            }
        }

        for (const ConfigVoicePortraitSpec& spec :
             ConfigVoicePortraitCatalog()) {
            if (auto e = load_sysgrp_entry(spec.entry_name)) {
                BgraImage p = impl_->renderer.RenderConfigVoicePortrait(
                    e->decoded.image, spec);
                emit_sysgrp_record(spec.entry_name, *e, std::move(p));
            }
        }

        // 2c. Confirmation Dialogs (7 plates):
        for (const DialogPlateSpec& spec : DialogPlateCatalog()) {
            if (auto e = load_sysgrp_entry(spec.entry_name)) {
                BgraImage p =
                    impl_->renderer.RenderDialogPlate(e->decoded.image, spec);
                emit_sysgrp_record(spec.entry_name, *e, std::move(p));
            }
        }

        // 2d. Message Window / Quick-Menu Tooltips (10 bars):
        for (const MsgTooltipSpec& spec : MsgTooltipCatalog()) {
            if (auto e = load_sysgrp_entry(spec.entry_name)) {
                BgraImage p =
                    impl_->renderer.RenderMsgTooltip(e->decoded.image, spec);
                emit_sysgrp_record(spec.entry_name, *e, std::move(p));
            }
        }

        // 2e. Extra -> Music Mode Track Banners (42 tracks):
        BgraImage sakura_icon;
        if (auto first_music = load_sysgrp_entry("SGMusic000100")) {
            sakura_icon = impl_->renderer.ExtractMusicSakuraIcon(
                first_music->decoded.image);
        }
        for (const MusicTrackSpec& spec : MusicTrackCatalog()) {
            if (auto e = load_sysgrp_entry(spec.entry_name)) {
                BgraImage p = impl_->renderer.RenderMusicTrackBanner(
                    spec, sakura_icon, e->decoded.image.width(),
                    e->decoded.image.height());
                emit_sysgrp_record(spec.entry_name, *e, std::move(p));
            }
        }

        // Load untouched interactive sprites needed for 1280x720 preview mockups:
        if (!options.previews_dir.empty()) {
            constexpr std::array<std::string_view, 14> kPreviewHelperSprites = {
                "SGTitle000000",   "SGTitle000100",   "SGTitle000200",
                "SGTitle000300",   "SGTitle000400",   "SGCnfgWnd000000",
                "SGCnfgWnd200000", "SGCnfgWnd200100", "SGCnfgWnd200200",
                "SGMusic990000",   "SGCG000400",      "SGDialog000000",
                "SGDialog000100",
            };
            for (const std::string_view helper_name : kPreviewHelperSprites) {
                if (helper_name.empty()) {
                    continue;
                }
                if (auto e = load_sysgrp_entry(helper_name)) {
                    orig_sysgrp_imgs.emplace(std::string(helper_name),
                                             std::move(e->decoded.image));
                }
            }
        }
    } else if (options.strict_retail_inventory) {
        throw std::runtime_error("Missing required archive: " +
                                 sysgrp_arc_path.u8string());
    }

    // ------------------------------------------------------------------------
    // 3. Choice Option Plates (`select01-1..select17-2` in `data02502.arc`)
    // ------------------------------------------------------------------------
    const fs::path choice_arc_path =
        ResolveUnpatchedArchivePath(options.game_dir, kChoiceArcRelPath);
    if (fs::is_regular_file(choice_arc_path, ec)) {
        const Bytes choice_arc_bytes = read_file(choice_arc_path.u8string());
        const std::vector<bgi::ArcEntry> arc_entries =
            bgi::list_arc(choice_arc_path.u8string());
        std::vector<std::string> entry_names;
        entry_names.reserve(arc_entries.size());
        for (const bgi::ArcEntry& ae : arc_entries) {
            entry_names.push_back(ae.name);
        }

        const auto census = ScanChoiceOptionCensus(options.game_dir);
        const auto en_table =
            LoadChoiceTranslationTable(options.project_dir, options.game_dir);
        const std::vector<ChoiceOptionItem> resolved_choices =
            ResolveChoiceOptions(entry_names, census, en_table);

        if (!resolved_choices.empty()) {
            std::vector<std::string> all_en;
            all_en.reserve(resolved_choices.size());
            for (const ChoiceOptionItem& item : resolved_choices) {
                all_en.push_back(item.english_text);
            }

            std::uint64_t first_off = 0u;
            std::uint64_t first_sz = 0u;
            if (menu_patch_format::FindArc20EntrySlice(
                    choice_arc_bytes, resolved_choices.front().entry_name,
                    first_off, first_sz) != menu_patch_format::ArcStatus::kOk) {
                throw std::runtime_error("Failed locating first choice entry in " +
                                         choice_arc_path.u8string());
            }
            const Bytes first_slice(
                choice_arc_bytes.begin() +
                    static_cast<std::ptrdiff_t>(first_off),
                choice_arc_bytes.begin() +
                    static_cast<std::ptrdiff_t>(first_off + first_sz));
            const ImageShape first_shape = ReadEntryImageShape(first_slice);
            const int fitted_choice_px = impl_->renderer.ComputeChoiceFontSizePx(
                all_en, std::max(1, first_shape.width / 3));

            for (const ChoiceOptionItem& item : resolved_choices) {
                std::uint64_t off = 0u;
                std::uint64_t sz = 0u;
                if (menu_patch_format::FindArc20EntrySlice(
                        choice_arc_bytes, item.entry_name, off, sz) !=
                    menu_patch_format::ArcStatus::kOk) {
                    throw std::runtime_error(
                        "Choice entry missing from data02502.arc: " +
                        item.entry_name);
                }
                const Bytes slice(
                    choice_arc_bytes.begin() + static_cast<std::ptrdiff_t>(off),
                    choice_arc_bytes.begin() +
                        static_cast<std::ptrdiff_t>(off + sz));
                const ImageShape shape = ReadEntryImageShape(slice);
                BgraImage plate = impl_->renderer.RenderChoiceOptionPlate(
                    item.english_text, shape.width, shape.height,
                    fitted_choice_px);
                Bytes bmp32 = plate.EncodeBmp(kChoiceOutputBpp);
                if (want_visuals) {
                    patched_choice_imgs.emplace_back(item.entry_name,
                                                     std::move(plate));
                }
                menu_patch_format::MenuPatchRecordInput rec;
                rec.source_rel_path = std::string(kChoiceArcRelPath);
                rec.target_rel_path = item.entry_name;
                rec.entry_name = item.entry_name;
                rec.orig_offset = off;
                rec.orig_slice = slice;
                rec.new_raw = std::move(bmp32);
                rec.flags = menu_patch_format::kFlagBackupStandalone;
                records.push_back(std::move(rec));
                ++summary.choice_records;
            }
        }
    } else if (options.strict_retail_inventory) {
        throw std::runtime_error("Missing required archive: " +
                                 choice_arc_path.u8string());
    }

    if (options.strict_retail_inventory) {
        if (summary.splash_records != kExpectedSplashRecordCount ||
            summary.sysgrp_records != kExpectedSysGrpRecordCount ||
            summary.choice_records != kExpectedChoiceRecordCount) {
            throw std::runtime_error(
                "Retail inventory mismatch: built splash=" +
                std::to_string(summary.splash_records) + "/" +
                std::to_string(kExpectedSplashRecordCount) +
                ", sysgrp=" + std::to_string(summary.sysgrp_records) + "/" +
                std::to_string(kExpectedSysGrpRecordCount) +
                ", choices=" + std::to_string(summary.choice_records) + "/" +
                std::to_string(kExpectedChoiceRecordCount));
        }
    } else if (records.empty()) {
        throw std::runtime_error(
            "MenuPatchBuilder produced zero localization records from " +
            options.game_dir.u8string());
    }

    // ------------------------------------------------------------------------
    // 4. Serialize `"SNUMENU1"` v1 container, verify signatures, write output
    // ------------------------------------------------------------------------
    std::vector<std::uint8_t> blob;
    const menu_patch_format::ManifestStatus ser_status =
        menu_patch_format::SerializeMenuPatchManifest(records, blob);
    if (ser_status != menu_patch_format::ManifestStatus::kOk) {
        throw std::runtime_error(
            std::string("SerializeMenuPatchManifest failed: ") +
            menu_patch_format::ManifestStatusName(ser_status));
    }

    AssertNoRawAssetSignatures(blob);

    menu_patch_format::MenuPatchManifest parsed_back;
    const menu_patch_format::ManifestStatus parse_status =
        menu_patch_format::ParseMenuPatchManifest(blob, parsed_back);
    if (parse_status != menu_patch_format::ManifestStatus::kOk) {
        throw std::runtime_error(
            std::string("ParseMenuPatchManifest round-trip check failed: ") +
            menu_patch_format::ManifestStatusName(parse_status));
    }

    if (!options.output_patch_path.empty()) {
        WriteBinaryFileAtomic(options.output_patch_path, blob);
    }

    summary.total_records = records.size();
    summary.patch_bytes = blob.size();
    summary.manifest_hash = parsed_back.header.manifest_hash;
    if (patch_bytes_out != nullptr) {
        *patch_bytes_out = std::move(blob);
    }

    // ------------------------------------------------------------------------
    // 5. Optional QA Contact Sheets (`--contact-sheets <dir>`)
    // ------------------------------------------------------------------------
    if (!options.contact_sheets_dir.empty()) {
        fs::create_directories(options.contact_sheets_dir);
        auto save_sheet = [&](const std::string& filename, const BgraImage& img) {
            if (img.empty()) {
                return;
            }
            const fs::path p = options.contact_sheets_dir / filename;
            img.SavePng(p);
            summary.contact_sheet_files.push_back(p);
        };

        // Sheet 1: Splash screens + Title + Locked CG
        {
            std::vector<ComparisonRowSpec> s1;
            if (orig_att01_img && patched_att01_img) {
                s1.push_back({"att01 (Splash 1)", *orig_att01_img,
                              *patched_att01_img});
            }
            if (orig_att02_img && patched_att02_img) {
                s1.push_back({"att02 (Splash 2)", *orig_att02_img,
                              *patched_att02_img});
            }
            for (const std::string_view k :
                 {kTitleBgEntryName, kLockedCgThumbEntryName}) {
                const std::string ks(k);
                if (orig_sysgrp_imgs.count(ks) && patched_sysgrp_imgs.count(ks)) {
                    s1.push_back(
                        {ks, orig_sysgrp_imgs[ks], patched_sysgrp_imgs[ks]});
                }
            }
            if (!s1.empty()) {
                save_sheet("01_splash_and_title.png",
                           RenderComparisonSheet(s1, 480, 300));
            }
        }

        // Sheet 2: Config Background + 12 Toggle Strips
        {
            std::vector<ComparisonRowSpec> s2;
            const std::string bg_key(kConfigBgEntryName);
            if (orig_sysgrp_imgs.count(bg_key) &&
                patched_sysgrp_imgs.count(bg_key)) {
                s2.push_back({bg_key, orig_sysgrp_imgs[bg_key],
                              patched_sysgrp_imgs[bg_key]});
            }
            for (const auto& spec : ConfigToggleStripCatalog()) {
                const std::string k(spec.entry_name);
                if (orig_sysgrp_imgs.count(k) && patched_sysgrp_imgs.count(k)) {
                    s2.push_back({k + " (" + std::string(spec.label_en) + ")",
                                  orig_sysgrp_imgs[k], patched_sysgrp_imgs[k]});
                }
            }
            if (!s2.empty()) {
                save_sheet("02_config_bg_and_toggles.png",
                           RenderComparisonSheet(s2, 560, 320));
            }
        }

        // Sheet 3: 8 Character Voice Portraits (1:1 200x130)
        {
            std::vector<ComparisonRowSpec> s3;
            for (const auto& spec : ConfigVoicePortraitCatalog()) {
                const std::string k(spec.entry_name);
                if (orig_sysgrp_imgs.count(k) && patched_sysgrp_imgs.count(k)) {
                    s3.push_back({k + " (" + std::string(spec.name_en) + ")",
                                  orig_sysgrp_imgs[k], patched_sysgrp_imgs[k]});
                }
            }
            if (!s3.empty()) {
                save_sheet("03_config_voice_portraits.png",
                           RenderComparisonSheet(s3, 320, 200));
            }
        }

        // Sheet 4: 7 Confirmation Dialogs + 10 Tooltips
        {
            std::vector<ComparisonRowSpec> s4;
            for (const auto& spec : DialogPlateCatalog()) {
                const std::string k(spec.entry_name);
                if (orig_sysgrp_imgs.count(k) && patched_sysgrp_imgs.count(k)) {
                    s4.push_back({k, orig_sysgrp_imgs[k], patched_sysgrp_imgs[k]});
                }
            }
            for (const auto& spec : MsgTooltipCatalog()) {
                const std::string k(spec.entry_name);
                if (orig_sysgrp_imgs.count(k) && patched_sysgrp_imgs.count(k)) {
                    s4.push_back({k, orig_sysgrp_imgs[k], patched_sysgrp_imgs[k]});
                }
            }
            if (!s4.empty()) {
                save_sheet("04_dialogs_and_tooltips.png",
                           RenderComparisonSheet(s4, 460, 180));
            }
        }

        // Sheets 5 & 6: All 42 Music Track Banners over alpha checkerboard
        {
            std::vector<std::pair<std::string, BgraImage>> m1;
            std::vector<std::pair<std::string, BgraImage>> m2;
            const auto& tracks = MusicTrackCatalog();
            for (std::size_t i = 0u; i < tracks.size(); ++i) {
                const std::string k(tracks[i].entry_name);
                if (!patched_sysgrp_imgs.count(k)) {
                    continue;
                }
                auto& bucket = (i < 21u) ? m1 : m2;
                bucket.emplace_back(k, patched_sysgrp_imgs[k]);
            }
            if (!m1.empty()) {
                save_sheet("05_music_tracks_01_21.png",
                           RenderGridSheet(m1, 1, 864, 42));
            }
            if (!m2.empty()) {
                save_sheet("06_music_tracks_22_42.png",
                           RenderGridSheet(m2, 1, 864, 42));
            }
        }

        // Sheet 7: All 34 Choice Option Plates (Tile 0 normal + Tile 1 hover)
        if (!patched_choice_imgs.empty()) {
            std::vector<std::pair<std::string, BgraImage>> choice_cells;
            choice_cells.reserve(patched_choice_imgs.size());
            for (const auto& [name, sheet] : patched_choice_imgs) {
                const int two_tiles_w = std::max(1, (sheet.width() * 2) / 3);
                choice_cells.emplace_back(
                    name, sheet.Crop(RectI{0, 0, two_tiles_w, sheet.height()}));
            }
            save_sheet("07_choice_options.png",
                       RenderGridSheet(choice_cells, 2, 720, 58));
        }
    }

    // ------------------------------------------------------------------------
    // 6. Optional 1280x720 Full-Screen Previews (`--previews <dir>`)
    // ------------------------------------------------------------------------
    if (!options.previews_dir.empty()) {
        fs::create_directories(options.previews_dir);
        auto lookup_orig = [&](std::string_view name) -> const BgraImage* {
            const auto it = orig_sysgrp_imgs.find(std::string(name));
            return (it != orig_sysgrp_imgs.end()) ? &it->second : nullptr;
        };
        auto lookup_patched = [&](std::string_view name) -> const BgraImage* {
            const auto it = patched_sysgrp_imgs.find(std::string(name));
            return (it != patched_sysgrp_imgs.end()) ? &it->second : nullptr;
        };

        const std::vector<PreviewMockupItem> previews =
            impl_->renderer.BuildFullScreenPreviews(
                patched_att01_img ? &*patched_att01_img : nullptr,
                patched_att02_img ? &*patched_att02_img : nullptr,
                lookup_orig, lookup_patched, patched_choice_imgs);
        for (const PreviewMockupItem& item : previews) {
            if (item.image.empty()) {
                continue;
            }
            const fs::path p = options.previews_dir / (item.file_stem + ".png");
            item.image.SavePng(p);
            summary.preview_files.push_back(p);
        }
    }

    return summary;
}

MenuPatchBuildSummary BuildMenuPatch(
    const MenuPatchBuildOptions& options,
    std::vector<std::uint8_t>* patch_bytes_out) {
    const MenuPatchBuilder builder;
    return builder.Build(options, patch_bytes_out);
}

}  // namespace snu::menu
