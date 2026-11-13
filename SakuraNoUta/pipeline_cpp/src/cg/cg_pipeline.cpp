// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cg_pipeline.h"

#include "bgi/bgi_archive.h"
#include "bgi/bgi_cbg.h"
#include "common/util.h"
#include "image.h"
#include "text_render.h"

#include <boost/json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace snu::cg {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr std::size_t kCbgWidthLoOffset = 0x10;
constexpr std::size_t kCbgWidthHiOffset = 0x11;
constexpr std::size_t kCbgHeightLoOffset = 0x12;
constexpr std::size_t kCbgHeightHiOffset = 0x13;
constexpr int kBitsPerByte = 8;
constexpr std::size_t kEscapedBackslashMultiplier = 2;
constexpr std::size_t kBmpHeaderMinSize = 2;
constexpr int kMinCropEdgeSampleDim = 4;
constexpr int kRgbChannels = 3;
constexpr std::uint8_t kWhiteChannel = 255;
constexpr std::uint8_t kLightBgTextChannel = 30;
constexpr int kDefaultFitFontSize = 36;
constexpr int kMinFontSize = 8;
constexpr int kFallbackFontSize = 12;
constexpr std::size_t kHexColorDigits = 6;
constexpr int kHexBase = 16;
constexpr std::size_t kHexComponentChars = 2;
constexpr std::size_t kHexRedPos = 0;
constexpr std::size_t kHexGreenPos = 2;
constexpr std::size_t kHexBluePos = 4;
constexpr std::size_t kBboxElementCount = 4;
constexpr int kMinRegionDimension = 4;
constexpr int kHorizontalPadding = 8;
constexpr int kVerticalPadding = 4;
constexpr int kLineSpacingExtra = 4;
constexpr int kHorizontalMargin = 6;
constexpr int kMinTopPadding = 4;
constexpr int kOverrideVariantCount = 6;
constexpr int kChuuyaFirstStanzaMaxFrame = 4;
constexpr int kChuuyaSecondStanzaFrame = 5;
constexpr int kJsonIndentSpaces = 2;
constexpr std::size_t kKilobyteBytes = 1024;
constexpr std::size_t kDisplayIndexOffset = 1;
constexpr std::size_t kScannedPairArchiveIndex = 0;
constexpr std::size_t kScannedPairEntryIndex = 1;

// Event CGs and special image arcs.  Sprite arcs (thousands of small character
// images) are excluded because they never carry narrative text.
const std::vector<std::string> SCAN_ARCS = {
    "data02099.arc", "data02100.arc", "data02500.arc",
    "data02501.arc", "data02502.arc", "data02503.arc",
};

// Entries to skip: decorative calligraphy with clipped vertical text.
const std::set<std::string> SKIP_ENTRIES = {"kt_ib", "kt_mh", "kt_ny"};

const char* FONT_PATH = "C:/Windows/Fonts/MTCORSVA.TTF";  // Monotype Corsiva
const char* FONT_FALLBACK = "C:/Windows/Fonts/arial.ttf";

// Minimum dimensions to bother scanning (skip tiny sprites / thumbnails).
constexpr int MIN_WIDTH = 800;
constexpr int MIN_HEIGHT = 400;

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
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
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

class ScopedTempFile {
public:
    explicit ScopedTempFile(fs::path path) noexcept : path_(std::move(path)) {}

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    ~ScopedTempFile() {
        if (!released_) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }

    void release() noexcept { released_ = true; }

private:
    fs::path path_;
    bool released_ = false;
};

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (len <= 0) {
        throw std::runtime_error("invalid UTF-8 path in CG pipeline");
    }
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), len);
    if (written != len) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed in CG pipeline");
    }
    return out;
}

std::wstring quote_wide_arg(std::wstring_view arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(pending_backslashes * kEscapedBackslashMultiplier + 1, L'\\');
            quoted.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        quoted.append(pending_backslashes, L'\\');
        quoted.push_back(ch);
        pending_backslashes = 0;
    }
    quoted.append(pending_backslashes * kEscapedBackslashMultiplier, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

int run_wide_tool(const std::string& exe_utf8,
                  const std::vector<std::string>& args_utf8) {
    if (exe_utf8.find('"') != std::string::npos) {
        return kExitFailure;
    }
    const std::wstring exe_w = utf8_to_wide_strict(exe_utf8);
    std::wstring cmd_w = L"\"" + exe_w + L"\"";
    for (const auto& arg : args_utf8) {
        cmd_w.push_back(L' ');
        cmd_w += quote_wide_arg(utf8_to_wide_strict(arg));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return kExitFailure;
    }
    const ScopedProcessHandles handles(pi);
    if (WaitForSingleObject(handles.process(), INFINITE) == WAIT_FAILED) {
        return kExitFailure;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(handles.process(), &exit_code)) {
        return kExitFailure;
    }
    return static_cast<int>(exit_code);
}

std::optional<std::pair<int, int>> cbg_dimensions(const Bytes& raw) {
    if (!bgi::is_cbg(raw)) {
        return std::nullopt;
    }
    const int w = raw[kCbgWidthLoOffset] | (raw[kCbgWidthHiOffset] << kBitsPerByte);
    const int h = raw[kCbgHeightLoOffset] | (raw[kCbgHeightHiOffset] << kBitsPerByte);
    return std::make_pair(w, h);
}

// TOOLS/garbro/extract_cbg.exe -- handles every CBG version (notably the
// v2 DCT format most of this game's CGs use, which src/bgi does not decode).
std::string g_project_dir;

std::optional<Image> decode_cbg_via_tool(const Bytes& raw) {
    const std::string exe = g_project_dir + "\\..\\TOOLS\\garbro\\extract_cbg.exe";
    if (!fs::exists(fs::u8path(exe))) {
        return std::nullopt;
    }
    const ScopedTempDir scratch("snu_cg_decode");
    const fs::path cbg_path = scratch.path() / fs::u8path("decode.cbg");
    const fs::path bmp_path = scratch.path() / fs::u8path("decode.cbg.bmp");
    const std::string cbg = cbg_path.u8string();
    const std::string bmp = bmp_path.u8string();
    write_file(cbg, raw);
    const int rc = run_wide_tool(exe, {cbg, bmp});
    if (rc != kExitOk || !fs::exists(bmp_path)) {
        return std::nullopt;
    }
    return decode_image(read_file(bmp));
}

std::optional<Image> decode_entry(const std::string& arc_path, const std::string& name) {
    Bytes raw;
    try {
        raw = bgi::read_entry_by_name(arc_path, name, /*decompress=*/false);
    } catch (...) {
        return std::nullopt;
    }
    if (raw.size() >= kBmpHeaderMinSize && raw[0] == 'B' && raw[1] == 'M') {
        return decode_image(raw);
    }
    if (bgi::is_cbg(raw)) {
        try {
            bgi::CbgImage cbg = bgi::decode_cbg(raw);
            return from_cbg_pixels(cbg.pixels, cbg.width, cbg.height, cbg.bpp);
        } catch (...) {
            return decode_cbg_via_tool(raw);  // v2 / anything v1 can't handle
        }
    }
    return std::nullopt;
}

std::array<std::uint8_t, 3> sample_bg(const Image& img, int x1, int y1, int x2, int y2) {
    Image arr = crop(img, x1, y1, x2, y2);
    std::vector<std::array<std::uint8_t, 3>> edges;
    auto push_row = [&](int y) {
        for (int x = 0; x < arr.width; ++x) {
            edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
        }
    };
    auto push_col = [&](int x) {
        for (int y = 0; y < arr.height; ++y) {
            edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
        }
    };
    if (arr.height > kMinCropEdgeSampleDim) {
        push_row(0);
        push_row(1);
        push_row(arr.height - 2);
        push_row(arr.height - 1);
    }
    if (arr.width > kMinCropEdgeSampleDim) {
        push_col(0);
        push_col(1);
        push_col(arr.width - 2);
        push_col(arr.width - 1);
    }
    if (edges.empty()) {
        return {kWhiteChannel, kWhiteChannel, kWhiteChannel};
    }
    std::array<std::uint8_t, 3> med{};
    for (int c = 0; c < kRgbChannels; ++c) {
        std::vector<std::uint8_t> ch;
        ch.reserve(edges.size());
        for (const auto& e : edges) {
            ch.push_back(e[static_cast<std::size_t>(c)]);
        }
        std::nth_element(ch.begin(), ch.begin() + ch.size() / 2, ch.end());
        med[static_cast<std::size_t>(c)] = ch[ch.size() / 2];
    }
    return med;
}

std::unique_ptr<Font> fit_font(const std::string& text, const std::string& font_path,
                               int max_w, int max_h,
                               int start_size = kDefaultFitFontSize) {
    for (int sz = start_size; sz >= kMinFontSize; --sz) {
        auto font = Font::load(font_path, sz);
        if (!font) {
            continue;
        }
        TextBBox bb = font->measure(text);
        if (bb.width() <= max_w && bb.height() <= max_h) {
            return font;
        }
    }
    return Font::load(font_path, kMinFontSize);
}

std::vector<std::string> wrap_lines(const std::string& text, const Font& font, int max_w) {
    std::vector<std::string> words;
    {
        std::size_t pos = 0;
        while (pos < text.size()) {
            while (pos < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[pos]))) {
                ++pos;
            }
            std::size_t end = pos;
            while (end < text.size() &&
                   !std::isspace(static_cast<unsigned char>(text[end]))) {
                ++end;
            }
            if (end > pos) {
                words.push_back(text.substr(pos, end - pos));
            }
            pos = end;
        }
    }
    std::vector<std::string> lines;
    std::string cur;
    for (const auto& word : words) {
        std::string test = cur.empty() ? word : cur + " " + word;
        TextBBox bb = font.measure(test);
        if (bb.width() > max_w && !cur.empty()) {
            lines.push_back(cur);
            cur = word;
            continue;
        }
        cur = test;
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

std::array<std::uint8_t, 3> parse_hex_color(const std::string& hex,
                                            const std::string& bg_tone) {
    std::string hx = hex;
    if (!hx.empty() && hx[0] == '#') {
        hx = hx.substr(1);
    }
    if (hx.size() >= kHexColorDigits) {
        auto conv = [&](std::size_t i) -> int {
            try {
                return std::stoi(hx.substr(i, kHexComponentChars), nullptr, kHexBase);
            } catch (...) {
                return -1;
            }
        };
        const int r = conv(kHexRedPos);
        const int g = conv(kHexGreenPos);
        const int b = conv(kHexBluePos);
        if (r >= 0 && g >= 0 && b >= 0) {
            return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                    static_cast<std::uint8_t>(b)};
        }
    }
    if (bg_tone == "dark") {
        return {kWhiteChannel, kWhiteChannel, kWhiteChannel};
    }
    return {kLightBgTextChannel, kLightBgTextChannel, kLightBgTextChannel};
}

std::string default_font_path() {
    return fs::exists(FONT_PATH) ? FONT_PATH : FONT_FALLBACK;
}

// Paint English over JP text regions.  Hand-tuned keys in the region object
// (font_path, font_size, align, x, y) override the bbox-derived defaults.
Image render_translation(const Image& img, const bj::array& regions) {
    Image result = img;
    const int w = result.width;
    const int h = result.height;

    for (const auto& rv : regions) {
        if (!rv.is_object()) {
            continue;
        }
        const auto& r = rv.get_object();
        auto gets = [&](const char* k, const std::string& dflt) -> std::string {
            if (auto* v = r.if_contains(k)) {
                if (v->is_string()) {
                    return std::string(v->get_string());
                }
            }
            return dflt;
        };
        auto geti = [&](const char* k) -> std::optional<int> {
            if (auto* v = r.if_contains(k)) {
                if (v->is_int64()) {
                    return static_cast<int>(v->get_int64());
                }
                if (v->is_double()) {
                    return static_cast<int>(v->get_double());
                }
            }
            return std::nullopt;
        };

        std::string text_en = gets("text_en", "");
        std::string bg_tone = gets("bg_tone", "dark");
        std::string txt_hex = gets("text_color", bg_tone == "dark" ? "#ffffff" : "#1a1a1a");
        std::string font_path = gets("font_path", default_font_path());
        std::optional<int> font_size = geti("font_size");  // nullopt -> auto-fit
        std::string align = gets("align", "center");
        std::optional<int> anchor_x = geti("x");
        std::optional<int> anchor_y = geti("y");

        if (text_en.empty()) {
            continue;
        }

        // Resolve bbox (required unless explicit x,y given)
        std::vector<int> bbox;
        if (auto* bb = r.if_contains("bbox")) {
            if (bb->is_array()) {
                for (const auto& v : bb->get_array()) {
                    if (v.is_int64()) {
                        bbox.push_back(static_cast<int>(v.get_int64()));
                    } else if (v.is_double()) {
                        bbox.push_back(static_cast<int>(v.get_double()));
                    }
                }
            }
        }
        if (bbox.size() < kBboxElementCount && (!anchor_x || !anchor_y)) {
            continue;
        }
        int x1 = 0;
        int y1 = 0;
        int x2 = 0;
        int y2 = 0;
        if (bbox.size() >= kBboxElementCount) {
            x1 = std::max(0, bbox[0]);
            y1 = std::max(0, bbox[1]);
            x2 = std::min(w, std::max(0, bbox[2]));
            y2 = std::min(h, std::max(0, bbox[3]));
        } else {
            x1 = *anchor_x;
            y1 = *anchor_y;
            x2 = w;
            y2 = h;
        }
        const int bw = x2 - x1;
        const int bh = y2 - y1;
        if (bw < kMinRegionDimension || bh < kMinRegionDimension) {
            continue;
        }

        // Inpaint only when we have a real bbox (not just an anchor)
        if (bbox.size() >= kBboxElementCount) {
            auto bg = sample_bg(result, x1, y1, x2, y2);
            for (int y = y1; y < y2; ++y) {
                for (int x = x1; x < x2; ++x) {
                    std::uint8_t* d = result.px(x, y);
                    d[0] = bg[0];
                    d[1] = bg[1];
                    d[2] = bg[2];
                }
            }
        }

        auto tc = parse_hex_color(txt_hex, bg_tone);

        // Build font
        std::unique_ptr<Font> font;
        if (font_size) {
            font = Font::load(font_path, *font_size);
        } else {
            std::string flat = text_en;
            std::replace(flat.begin(), flat.end(), '\n', ' ');
            font = fit_font(flat, font_path, bw - kHorizontalPadding,
                            bh - kVerticalPadding);
        }
        if (!font) {
            font = Font::load(FONT_FALLBACK,
                              font_size ? *font_size : kFallbackFontSize);
            if (!font) {
                continue;
            }
        }

        // Split into lines
        std::vector<std::string> lines;
        {
            std::size_t pos = 0;
            while (pos <= text_en.size()) {
                std::size_t nl = text_en.find('\n', pos);
                lines.push_back(text_en.substr(
                    pos, nl == std::string::npos ? std::string::npos : nl - pos));
                if (nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
            }
        }
        if (lines.size() == 1 && !font_size) {
            lines = wrap_lines(text_en, *font, bw - kHorizontalPadding);
        }

        const int line_h = font->measure("Ay").y1 + kLineSpacingExtra;
        const int total_h = line_h * static_cast<int>(lines.size());

        int ty = anchor_y ? *anchor_y
                          : y1 + std::max(kMinTopPadding, (bh - total_h) / 2);

        for (const auto& line : lines) {
            TextBBox bb = font->measure(line);
            const int lw = bb.width();
            int tx = x1 + (bw - lw) / 2;
            if (anchor_x) {
                tx = *anchor_x;
            } else if (align == "left") {
                tx = x1 + kHorizontalMargin;
            } else if (align == "right") {
                tx = x2 - lw - kHorizontalMargin;
            }
            font->draw(result, tx, ty, line, tc);
            ty += line_h;
        }
    }

    return result;
}

// sp0065a..f -- Miyazawa 'Spring and Asura' preface, progressive 6-frame
// reveal onto sp0065a (the blank background) for idempotent re-runs.
Image epigraph_render(const CgOptions& opt, int n_lines, const Image& img) {
    const std::array<std::uint8_t, 3> INK = {38, 26, 18};  // dark sepia
    struct Line {
        const char* text;
        int y;
        int x;
        int size;
    };
    const std::vector<Line> LINES = {
        {"If this be nothingness, then nothingness itself is just this,", 316, 260, 28},
        {"and to some degree, it is shared by us all;",                   357, 390, 28},
        {"for just as everything within me is everyone,",                 512, 440, 28},
        {"so too is everything within each and every one.",               553, 350, 28},
        {"  -- 'Spring and Asura' Preface",                               614, 950, 22},
    };

    if (n_lines == 0) {
        return img;
    }
    Image base = img;
    if (auto blank = decode_entry(opt.game_dir + "\\data02501.arc.backup", "sp0065a")) {
        base = *blank;
    } else if (auto blank2 = decode_entry(opt.game_dir + "\\data02501.arc", "sp0065a")) {
        base = *blank2;
    }
    std::string fp = default_font_path();
    for (int i = 0; i < n_lines && i < static_cast<int>(LINES.size()); ++i) {
        const Line& ln = LINES[static_cast<std::size_t>(i)];
        auto font = Font::load(fp, ln.size);
        if (!font) {
            font = Font::load(FONT_FALLBACK, ln.size);
        }
        if (font) {
            font->draw(base, ln.x, ln.y, ln.text, INK);
        }
    }
    return base;
}

// sp0067a..f -- Nakahara Chuuya 'Spring Day Rhapsody' (春日狂想), progressive
// reveal onto sp0067a (the blank background) so petals are never boxy-inpainted.
Image chuuya_render(const CgOptions& opt, int frame, const Image& img) {
    const std::array<std::uint8_t, 3> INK = {236, 232, 246};
    struct Line {
        const char* text;
        int y;
        int x;
        int size;
    };
    const std::vector<Line> STANZA_1 = {
        {"When the one you love has died,", 252, 135, 34},
        {"you must take your own life.",    304, 285, 34},
        {"When the one you love has died,", 460, 540, 34},
        {"there is no other way.",          512, 720, 34},
    };
    const std::vector<Line> STANZA_2 = {
        {"Come now, everyone, come now, all together --", 322, 180, 36},
        {"In proper tempo, let us shake hands.",          378, 430, 36},
    };

    if (frame == 0) {
        return img;
    }
    Image base = img;
    if (auto blank = decode_entry(opt.game_dir + "\\data02501.arc.backup", "sp0067a")) {
        base = *blank;
    } else if (auto blank2 = decode_entry(opt.game_dir + "\\data02501.arc", "sp0067a")) {
        base = *blank2;
    }

    std::string fp = default_font_path();
    if (frame >= 1 && frame <= kChuuyaFirstStanzaMaxFrame) {
        for (int i = 0; i < frame && i < static_cast<int>(STANZA_1.size()); ++i) {
            const Line& ln = STANZA_1[static_cast<std::size_t>(i)];
            auto font = Font::load(fp, ln.size);
            if (!font) {
                font = Font::load(FONT_FALLBACK, ln.size);
            }
            if (font) {
                font->draw(base, ln.x, ln.y, ln.text, INK);
            }
        }
        return base;
    }
    if (frame == kChuuyaSecondStanzaFrame) {
        for (const auto& ln : STANZA_2) {
            auto font = Font::load(fp, ln.size);
            if (!font) {
                font = Font::load(FONT_FALLBACK, ln.size);
            }
            if (font) {
                font->draw(base, ln.x, ln.y, ln.text, INK);
            }
        }
    }
    return base;
}

using OverrideFn = std::function<Image(const CgOptions&, const Image&)>;

const std::vector<std::pair<std::pair<std::string, std::string>, OverrideFn>>&
manual_overrides() {
    static const std::vector<std::pair<std::pair<std::string, std::string>, OverrideFn>>
        table = [] {
            std::vector<std::pair<std::pair<std::string, std::string>, OverrideFn>> t;
            for (int n = 0; n < kOverrideVariantCount; ++n) {
                std::string entry = std::string("sp0065") + static_cast<char>('a' + n);
                t.emplace_back(std::make_pair(std::string("data02501.arc"), entry),
                               [n](const CgOptions& o, const Image& img) {
                                   return epigraph_render(o, n, img);
                               });
            }
            for (int n = 0; n < kOverrideVariantCount; ++n) {
                std::string entry = std::string("sp0067") + static_cast<char>('a' + n);
                t.emplace_back(std::make_pair(std::string("data02501.arc"), entry),
                               [n](const CgOptions& o, const Image& img) {
                                   return chuuya_render(o, n, img);
                               });
            }
            return t;
        }();
    return table;
}

const OverrideFn* find_override(const std::string& arc, const std::string& entry) {
    for (const auto& [key, fn] : manual_overrides()) {
        if (key.first == arc && key.second == entry) {
            return &fn;
        }
    }
    return nullptr;
}

struct Paths {
    std::string candidates;
    std::string scanned;
    std::string out_dir;
};

Paths make_paths(const CgOptions& opt) {
    std::string out = opt.project_dir + "\\script_output";
    return {out + "\\narrative_candidates.json", out + "\\narrative_scanned.json",
            out + "\\narrative_patched"};
}

void extract_all_assets(const CgOptions& opt) {
    std::size_t total = 0;
    log_info("=== Listing all assets ===");
    for (const auto& arc_name : SCAN_ARCS) {
        std::string arc_path = opt.game_dir + "\\" + arc_name;
        if (!fs::exists(fs::u8path(arc_path))) {
            log_info("[skip] " + arc_name + " -- not found");
            continue;
        }
        auto entries = bgi::list_arc(arc_path);
        log_info("  " + arc_name + ": " + std::to_string(entries.size()) + " entries");
        total += entries.size();
    }
    log_info("\n" + std::to_string(total) + " entries total across " +
             std::to_string(SCAN_ARCS.size()) + " archive(s).");
}

void save_candidates(const Paths& paths, const bj::array& candidates) {
    write_file(paths.candidates, json_pretty(bj::value(candidates), kJsonIndentSpaces));
}

// One archive's listing, whole -- skipped entries included, because
// read_entry() addresses an entry by its index in it.  scan_idx names the
// entries the scan targets, and is where SKIP_ENTRIES is applied; the coverage
// count and the scan loop both read it, so neither can walk a set the other
// does not.
struct ArcEntries {
    std::string arc;
    std::vector<bgi::ArcEntry> entries;
    std::vector<std::size_t> scan_idx;
};

void save_scanned(const Paths& paths, const ScanKeySet& done) {
    bj::array arr;
    for (const auto& [arc, entry] : done) {
        arr.push_back(bj::array{arc, entry});
    }
    write_file(paths.scanned, bj::serialize(bj::value(std::move(arr))));
}

bj::array run_scan(const CgOptions& opt, bool resume) {
    const Paths paths = make_paths(opt);
    bj::array candidates;
    ScanKeySet done_keys;

    if (resume && fs::exists(fs::u8path(paths.candidates))) {
        candidates = json_parse_file(paths.candidates).get_array();
        for (const auto& cv : candidates) {
            const auto& c = cv.get_object();
            done_keys.emplace(std::string(c.at("arc").get_string()),
                              std::string(c.at("entry").get_string()));
        }
        if (fs::exists(fs::u8path(paths.scanned))) {
            // Bind the parsed value: in C++17 a range-for does NOT extend the
            // lifetime of a temporary the range initialiser only refers into,
            // so iterating json_parse_file(...).get_array() directly walks
            // freed memory.
            const bj::value scanned_v = json_parse_file(paths.scanned);
            for (const auto& pv : scanned_v.get_array()) {
                const auto& pair = pv.get_array();
                done_keys.emplace(
                    std::string(pair.at(kScannedPairArchiveIndex).get_string()),
                    std::string(pair.at(kScannedPairEntryIndex).get_string()));
            }
        }
        log_info("Resuming: " + std::to_string(candidates.size()) + " found, " +
                 std::to_string(done_keys.size()) + " total scanned.");
    }

    // Inject manual overrides that aren't already in the candidates list.
    for (const auto& [key, fn] : manual_overrides()) {
        if (done_keys.count(key)) {
            continue;
        }
        bj::object c;
        c["arc"] = key.first;
        c["entry"] = key.second;
        c["confidence"] = 1.0;
        c["description"] = "manual override";
        c["regions"] = nullptr;
        candidates.push_back(std::move(c));
        done_keys.insert(key);
        log_info("[manual] " + key.first + " / " + key.second);
    }

    log_info("[ocr] 0 method(s): none");

    // Every archive is listed before anything is sent, so the coverage count
    // and the scan below walk one and the same list.
    std::vector<ArcEntries> arcs;
    std::vector<ScanKey> targets;
    for (const auto& arc_name : SCAN_ARCS) {
        const std::string arc_path = opt.game_dir + "\\" + arc_name;
        if (!fs::exists(fs::u8path(arc_path))) {
            log_info("[skip] " + arc_name + " -- not found");
            continue;
        }

        ArcEntries ae;
        ae.arc = arc_name;
        ae.entries = bgi::list_arc(arc_path);
        log_info("\n=== " + arc_name + " (" + std::to_string(ae.entries.size()) +
                 " entries) ===");

        for (std::size_t i = 0; i < ae.entries.size(); ++i) {
            if (!SKIP_ENTRIES.count(ae.entries[i].name)) {
                ae.scan_idx.push_back(i);
            }
        }
        for (const std::size_t i : ae.scan_idx) {
            targets.emplace_back(arc_name, ae.entries[i].name);
        }
        arcs.push_back(std::move(ae));
    }

    // The resume file must already cover every target in an offline build.
    if (count_left_to_scan(targets, done_keys)) {
        throw std::runtime_error(
            "Offline build: missing cached CG scan; live API client removed for finished game");
    }

    for (const auto& ae : arcs) {
        const std::string arc_path = opt.game_dir + "\\" + ae.arc;
        const std::string& arc_name = ae.arc;
        const auto& entries = ae.entries;

        for (const std::size_t i : ae.scan_idx) {
            const auto& [name, off, sz] = std::tie(entries[i].name, entries[i].offset,
                                                   entries[i].size);
            if (!needs_scan(done_keys, {arc_name, name})) {
                continue;
            }

            Bytes raw = bgi::read_entry(arc_path, i, /*decompress=*/false);
            if (auto dims = cbg_dimensions(raw)) {
                if (dims->first < MIN_WIDTH || dims->second < MIN_HEIGHT) {
                    continue;  // thumbnails / sprites
                }
            }

            print_inline("  [" + std::to_string(i + kDisplayIndexOffset) + "/" +
                         std::to_string(entries.size()) + "] " + name + " (" +
                         std::to_string(sz / kKilobyteBytes) + "KB)... ");

            auto img = decode_entry(arc_path, name);
            if (!img) {
                log_info("decode failed");
                continue;
            }

            throw std::runtime_error(
                "Offline build: missing cached CG scan; live API client removed for finished game");
        }
    }

    log_info("\nScan done. " + std::to_string(candidates.size()) +
             " candidate(s) found -> " + paths.candidates);
    return candidates;
}

void run_translate(const CgOptions& opt, bj::array& candidates) {
    const Paths paths = make_paths(opt);
    fs::create_directories(fs::u8path(paths.out_dir));

    for (auto& cv : candidates) {
        auto& c = cv.get_object();
        const std::string arc(c.at("arc").get_string());
        const std::string entry(c.at("entry").get_string());

        if (!needs_translation(c, paths.out_dir)) {
            log_info("  " + entry + ": already translated, skip");
            continue;
        }

        std::string arc_path = opt.game_dir + "\\" + arc;
        print_inline("\n  " + arc + " / " + entry + "...");

        auto img = decode_entry(arc_path, entry);
        if (!img) {
            log_info(" decode failed");
            continue;
        }

        const OverrideFn* fn = find_override(arc, entry);
        if (fn == nullptr) {
            throw std::runtime_error(
                "Offline build: missing cached CG translation; live API client removed for finished game");
        }

        log_info(" [manual fn]");
        Image patched = (*fn)(opt, *img);
        c["regions"] = bj::array{};

        std::string out_bmp = paths.out_dir + "\\" + entry + ".bmp";
        write_file(out_bmp, encode_bmp(patched));
        c["patched_bmp"] = out_bmp;
        log_info("    saved -> " + out_bmp);

        save_candidates(paths, candidates);
    }
}

void run_repack(const CgOptions& opt, const bj::array& candidates) {
    const Paths paths = make_paths(opt);

    // Group replacements by arc.
    std::map<std::string, std::map<std::string, Bytes>> by_arc;
    for (const auto& cv : candidates) {
        const auto& c = cv.get_object();
        const std::string arc(c.at("arc").get_string());
        const std::string entry(c.at("entry").get_string());
        std::string bmp;
        if (auto* p = c.if_contains("patched_bmp")) {
            if (p->is_string()) {
                bmp = std::string(p->get_string());
            }
        }
        // The path the image was actually found at, which is what the
        // replacement is read from: a recorded path from before the project
        // folder moved no longer names a readable file.
        bmp = resolve_patched_path(bmp, paths.out_dir);
        if (!bmp.empty()) {
            by_arc[arc][entry] = read_file(bmp);
            continue;
        }
        std::string arc_src = opt.game_dir + "\\" + arc + ".backup";
        if (!fs::exists(fs::u8path(arc_src))) {
            arc_src = opt.game_dir + "\\" + arc;
        }
        if (const OverrideFn* fn = find_override(arc, entry)) {
            if (auto img = decode_entry(arc_src, entry)) {
                by_arc[arc][entry] = encode_bmp((*fn)(opt, *img));
            }
            continue;
        }
        if (auto* r = c.if_contains("regions");
            r && r->is_array() && !r->get_array().empty()) {
            if (auto img = decode_entry(arc_src, entry)) {
                by_arc[arc][entry] =
                    encode_bmp(render_translation(*img, r->get_array()));
            }
        }
    }

    if (by_arc.empty()) {
        log_info("Nothing to repack.");
        return;
    }

    for (const auto& [arc_name, replacements] : by_arc) {
        const std::string arc_path = opt.game_dir + "\\" + arc_name;
        const std::string bak = arc_path + ".backup";
        const std::string tmp = arc_path + ".tmp";
        ScopedTempFile tmp_guard(fs::u8path(tmp));
        log_info("\nRepacking " + arc_name + " (" +
                 std::to_string(replacements.size()) + " entries)...");
        try {
            bgi::repack_arc(arc_path, tmp, replacements);
        } catch (const std::exception& ex) {
            // One archive that cannot be rebuilt -- missing, unreadable or of
            // an unexpected shape -- must not take the other archives with it.
            log_info(std::string("  ERROR: ") + ex.what());
            continue;
        }
        if (!fs::exists(fs::u8path(bak))) {
            fs::copy_file(fs::u8path(arc_path), fs::u8path(bak));
        }
        fs::rename(fs::u8path(tmp), fs::u8path(arc_path));
        tmp_guard.release();
        log_info("  deployed -> " + arc_path + "  (backup: " + bak + ")");
    }
}

}  // namespace

bool needs_scan(const ScanKeySet& done, const ScanKey& key) {
    return done.find(key) == done.end();
}

std::size_t count_left_to_scan(const std::vector<ScanKey>& targets, const ScanKeySet& done) {
    std::size_t left = 0;
    for (const auto& key : targets) {
        if (needs_scan(done, key)) {
            ++left;
        }
    }
    return left;
}

std::string resolve_patched_path(const std::string& recorded,
                                 const std::string& patched_dir) {
    if (recorded.empty()) {
        return {};
    }
    if (fs::exists(fs::u8path(recorded))) {
        return recorded;
    }
    if (patched_dir.empty()) {
        return {};
    }
    const std::string name = fs::u8path(recorded).filename().u8string();
    if (name.empty()) {
        return {};
    }
    const std::string here = patched_dir + "\\" + name;
    return fs::exists(fs::u8path(here)) ? here : std::string();
}

bool needs_translation(const bj::object& candidate, const std::string& patched_dir) {
    const bool has_regions =
        candidate.if_contains("regions") && !candidate.at("regions").is_null();
    std::string patched_path;
    if (auto* p = candidate.if_contains("patched_bmp")) {
        if (p->is_string()) {
            patched_path = std::string(p->get_string());
        }
    }
    return !(has_regions &&
             !resolve_patched_path(patched_path, patched_dir).empty());
}

std::size_t count_left_to_translate(const bj::array& candidates,
                                    const std::string& patched_dir) {
    std::size_t left = 0;
    for (const auto& cv : candidates) {
        const auto& c = cv.get_object();
        if (!needs_translation(c, patched_dir)) {
            continue;
        }
        // Hand-tuned override entries cost no external request.
        if (find_override(std::string(c.at("arc").get_string()),
                          std::string(c.at("entry").get_string()))) {
            continue;
        }
        ++left;
    }
    return left;
}

int run_find_narrative_cg(const CgOptions& opt) {
    g_project_dir = opt.project_dir;
    const Paths paths = make_paths(opt);
    fs::create_directories(fs::u8path(opt.project_dir + "\\script_output"));

    if (opt.repack_only) {
        if (!fs::exists(fs::u8path(paths.candidates))) {
            log_info("ERROR: " + paths.candidates +
                     " not found. Run without --repack first.");
            return kExitFailure;
        }
        bj::array candidates = json_parse_file(paths.candidates).get_array();
        run_repack(opt, candidates);
        log_info("\nDone.");
        return kExitOk;
    }

    extract_all_assets(opt);
    bj::array candidates = run_scan(opt, /*resume=*/!opt.no_resume);
    if (!opt.scan_only && !candidates.empty()) {
        if (count_left_to_translate(candidates, paths.out_dir)) {
            throw std::runtime_error(
                "Offline build: missing cached CG translation; live API client removed for finished game");
        }
        run_translate(opt, candidates);
        run_repack(opt, candidates);
    }

    log_info("\nDone.");
    return kExitOk;
}

}  // namespace snu::cg
