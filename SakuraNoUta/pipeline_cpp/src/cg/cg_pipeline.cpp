// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cg_pipeline.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "bgi/bgi_archive.h"
#include "bgi/bgi_cbg.h"
#include "common/util.h"
#include "image.h"
#include "text_render.h"
#include "translate/anthropic_client.h"

namespace snu::cg {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Config — adjust for each game
// ---------------------------------------------------------------------------

// Arcs to scan — event CGs and special image arcs.  Sprite arcs (thousands
// of small character images) are excluded: they never carry narrative text.
const std::vector<std::string> SCAN_ARCS = {
    "data02099.arc", "data02100.arc", "data02500.arc",
    "data02501.arc", "data02502.arc", "data02503.arc",
};

// Entries to skip: decorative calligraphy with clipped vertical text.
const std::set<std::string> SKIP_ENTRIES = {"kt_ib", "kt_mh", "kt_ny"};

const char* FONT_PATH = "C:/Windows/Fonts/MTCORSVA.TTF";  // Monotype Corsiva
const char* FONT_FALLBACK = "C:/Windows/Fonts/arial.ttf";

const char* VISION_MODEL = "claude-sonnet-4-6";

// Minimum dimensions to bother scanning (skip tiny sprites / thumbnails).
constexpr int MIN_WIDTH = 800;
constexpr int MIN_HEIGHT = 400;

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

const char* DETECT_PROMPT = R"__(You are analyzing a visual novel CG image to determine if it contains narrative Japanese text.

"Narrative text" means text that is part of the story — such as:
  - A poem, literary quote, or passage overlaid on the artwork
  - A chapter or scene title card with story-relevant Japanese text

Do NOT classify as narrative: staff credits, legal disclaimers, UI buttons, shop signs painted into backgrounds, character name plates, or any text rendered by a standard text box.

Answer with a JSON object and nothing else:
{
  "has_narrative_text": true or false,
  "confidence": 0.0-1.0,
  "description": "<brief description of what text you see, if any>"
})__";

std::string extract_prompt(int w, int h) {
    return R"__(This CG image contains narrative Japanese text (a poem, quote, or title).

Extract every Japanese text element and provide:
1. The original Japanese text (exact)
2. A natural, high-quality English translation
3. The bounding box [x1, y1, x2, y2] of that text region in the image (pixel coords)
4. Approximate text color as a hex string (e.g. "#2a1a0e")
5. Whether the background behind the text is light or dark: "light" or "dark"

Image dimensions: )__" + std::to_string(w) + "x" + std::to_string(h) + R"__(

Respond with a JSON array and nothing else:
[
  {
    "text_jp": "...",
    "text_en": "...",
    "bbox": [x1, y1, x2, y2],
    "text_color": "#rrggbb",
    "bg_tone": "light" or "dark"
  },
  ...
])__";
}

// ---------------------------------------------------------------------------
// Claude API
// ---------------------------------------------------------------------------

std::string claude_vision(anthropic::Client& client, const std::string& prompt,
                          const Image& img) {
    print_inline(std::string("[claude ") + VISION_MODEL + "] calling api... ");
    bj::object image_block;
    image_block["type"] = "image";
    image_block["source"] = bj::object{
        {"type", "base64"},
        {"media_type", "image/png"},
        {"data", base64_encode(encode_png(img))},
    };
    bj::object text_block;
    text_block["type"] = "text";
    text_block["text"] = prompt;
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = bj::array{std::move(image_block), std::move(text_block)};
    bj::object body;
    body["model"] = VISION_MODEL;
    body["max_tokens"] = 1024;
    body["messages"] = bj::array{std::move(msg)};

    bj::value resp = client.messages(bj::value(std::move(body)), /*timeout_sec=*/60);
    const auto& content = resp.get_object().at("content").get_array();
    if (content.empty()) return {};
    return std::string(content[0].get_object().at("text").get_string());
}

// Extract the first JSON object from Claude's prose-wrapped response.
std::optional<bj::value> first_json_object(const std::string& s) {
    std::size_t b = s.find('{');
    if (b == std::string::npos) return std::nullopt;
    std::size_t e = s.find('}', b);
    if (e == std::string::npos) return std::nullopt;
    try {
        return bj::parse(s.substr(b, e - b + 1));
    } catch (...) {
        return std::nullopt;
    }
}

// Same scrape for an array; greedy match (first '[' to last ']').
std::optional<bj::value> outer_json_array(const std::string& s) {
    std::size_t b = s.find('[');
    std::size_t e = s.rfind(']');
    if (b == std::string::npos || e == std::string::npos || e <= b)
        return std::nullopt;
    try {
        return bj::parse(s.substr(b, e - b + 1));
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Image decoding
// ---------------------------------------------------------------------------

std::optional<std::pair<int, int>> cbg_dimensions(const Bytes& raw) {
    if (!bgi::is_cbg(raw)) return std::nullopt;
    int w = raw[0x10] | (raw[0x11] << 8);
    int h = raw[0x12] | (raw[0x13] << 8);
    return std::make_pair(w, h);
}

// TOOLS/garbro/extract_cbg.exe -- handles every CBG version (notably the
// v2 DCT format most of this game's CGs use, which src/bgi does not decode).
std::string g_project_dir;

std::optional<Image> decode_cbg_via_tool(const Bytes& raw) {
    std::string exe = g_project_dir + "\\..\\TOOLS\\garbro\\extract_cbg.exe";
    if (!fs::exists(fs::u8path(exe))) return std::nullopt;
    std::string cbg = (fs::temp_directory_path() / "snu_cg_decode.cbg").u8string();
    std::string bmp = cbg + ".bmp";
    write_file(cbg, raw);
    std::string cmd = "\"\"" + exe + "\" \"" + cbg + "\" \"" + bmp + "\"\"";
    int rc = std::system(cmd.c_str());
    std::optional<Image> img;
    if (rc == 0 && fs::exists(fs::u8path(bmp)))
        img = decode_image(read_file(bmp));
    std::error_code ec;
    fs::remove(fs::u8path(cbg), ec);
    fs::remove(fs::u8path(bmp), ec);
    return img;
}

std::optional<Image> decode_entry(const std::string& arc_path, const std::string& name) {
    Bytes raw;
    try {
        raw = bgi::read_entry_by_name(arc_path, name, /*decompress=*/false);
    } catch (...) {
        return std::nullopt;
    }
    if (raw.size() >= 2 && raw[0] == 'B' && raw[1] == 'M')
        return decode_image(raw);
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

// ---------------------------------------------------------------------------
// Phase 1: detect narrative text
// ---------------------------------------------------------------------------

struct DetectResult {
    bool has_text = false;
    double confidence = 0.0;
    std::string description;
};

DetectResult detect_narrative(anthropic::Client& client, const Image& img) {
    // For very wide images (scrolling credits etc.) tile into chunks.
    std::vector<Image> images_to_check;
    if (img.width > 2000) {
        constexpr int tile_w = 1280;
        for (int x = 0; x < img.width; x += tile_w)
            images_to_check.push_back(
                crop(img, x, 0, std::min(x + tile_w, img.width), img.height));
    } else {
        images_to_check.push_back(img);
    }

    for (const auto& tile : images_to_check) {
        try {
            std::string resp = claude_vision(client, DETECT_PROMPT, tile);
            auto data = first_json_object(resp);
            if (!data || !data->is_object()) continue;
            const auto& o = data->get_object();
            bool has = false;
            if (auto* v = o.if_contains("has_narrative_text"))
                has = v->is_bool() && v->get_bool();
            double conf = 0.0;
            if (auto* v = o.if_contains("confidence"))
                conf = v->is_double() ? v->get_double()
                       : v->is_int64() ? static_cast<double>(v->get_int64())
                                       : 0.0;
            if (has && conf >= 0.7) {
                DetectResult r;
                r.has_text = true;
                r.confidence = conf;
                if (auto* v = o.if_contains("description"))
                    if (v->is_string()) r.description = std::string(v->get_string());
                return r;
            }
        } catch (const std::exception& e) {
            log_info(std::string("    detect error: ") + e.what());
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Phase 2: extract text + translate
// ---------------------------------------------------------------------------

bj::array extract_and_translate(anthropic::Client& client, const Image& img) {
    std::string prompt = extract_prompt(img.width, img.height);
    Image check_img = img;
    double scale = 1.0;
    if (img.width > 1280) {
        scale = 1280.0 / img.width;
        check_img = resize(img, 1280, static_cast<int>(img.height * scale));
    }

    try {
        std::string resp = claude_vision(client, prompt, check_img);
        auto arr = outer_json_array(resp);
        if (!arr || !arr->is_array()) return {};
        bj::array regions = arr->get_array();
        if (scale < 1.0) {
            for (auto& rv : regions) {
                if (!rv.is_object()) continue;
                auto& r = rv.get_object();
                if (auto* bb = r.if_contains("bbox")) {
                    if (!bb->is_array()) continue;
                    bj::array scaled;
                    for (const auto& v : bb->get_array()) {
                        double d = v.is_double() ? v.get_double()
                                   : v.is_int64() ? static_cast<double>(v.get_int64())
                                                  : 0.0;
                        scaled.push_back(static_cast<std::int64_t>(d / scale));
                    }
                    r["bbox"] = std::move(scaled);
                }
            }
        }
        return regions;
    } catch (const std::exception& e) {
        log_info(std::string("    extract error: ") + e.what());
        return {};
    }
}

// ---------------------------------------------------------------------------
// Phase 3: render English text onto image
// ---------------------------------------------------------------------------

std::array<std::uint8_t, 3> sample_bg(const Image& img, int x1, int y1, int x2, int y2) {
    Image arr = crop(img, x1, y1, x2, y2);
    std::vector<std::array<std::uint8_t, 3>> edges;
    auto push_row = [&](int y) {
        for (int x = 0; x < arr.width; ++x)
            edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
    };
    auto push_col = [&](int x) {
        for (int y = 0; y < arr.height; ++y)
            edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
    };
    if (arr.height > 4) { push_row(0); push_row(1); push_row(arr.height - 2); push_row(arr.height - 1); }
    if (arr.width > 4) { push_col(0); push_col(1); push_col(arr.width - 2); push_col(arr.width - 1); }
    if (edges.empty()) return {255, 255, 255};
    std::array<std::uint8_t, 3> med{};
    for (int c = 0; c < 3; ++c) {
        std::vector<std::uint8_t> ch;
        ch.reserve(edges.size());
        for (const auto& e : edges) ch.push_back(e[c]);
        std::nth_element(ch.begin(), ch.begin() + ch.size() / 2, ch.end());
        med[c] = ch[ch.size() / 2];
    }
    return med;
}

std::unique_ptr<Font> fit_font(const std::string& text, const std::string& font_path,
                               int max_w, int max_h, int start_size = 36) {
    for (int sz = start_size; sz >= 8; --sz) {
        auto font = Font::load(font_path, sz);
        if (!font) continue;
        TextBBox bb = font->measure(text);
        if (bb.width() <= max_w && bb.height() <= max_h) return font;
    }
    return Font::load(font_path, 8);
}

std::vector<std::string> wrap_lines(const std::string& text, const Font& font, int max_w) {
    std::vector<std::string> words;
    {
        std::size_t pos = 0;
        while (pos < text.size()) {
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
                ++pos;
            std::size_t end = pos;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
                ++end;
            if (end > pos) words.push_back(text.substr(pos, end - pos));
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
        } else {
            cur = test;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::array<std::uint8_t, 3> parse_hex_color(const std::string& hex,
                                            const std::string& bg_tone) {
    std::string hx = hex;
    if (!hx.empty() && hx[0] == '#') hx = hx.substr(1);
    if (hx.size() >= 6) {
        auto conv = [&](int i) -> int {
            try {
                return std::stoi(hx.substr(i, 2), nullptr, 16);
            } catch (...) {
                return -1;
            }
        };
        int r = conv(0), g = conv(2), b = conv(4);
        if (r >= 0 && g >= 0 && b >= 0)
            return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                    static_cast<std::uint8_t>(b)};
    }
    return bg_tone == "dark" ? std::array<std::uint8_t, 3>{255, 255, 255}
                             : std::array<std::uint8_t, 3>{30, 30, 30};
}

std::string default_font_path() {
    return fs::exists(FONT_PATH) ? FONT_PATH : FONT_FALLBACK;
}

// Paint English over JP text regions.  Hand-tuned keys in the region object
// (font_path, font_size, align, x, y) override the bbox-derived defaults.
Image render_translation(const Image& img, const bj::array& regions) {
    Image result = img;
    const int w = result.width, h = result.height;

    for (const auto& rv : regions) {
        if (!rv.is_object()) continue;
        const auto& r = rv.get_object();
        auto gets = [&](const char* k, const std::string& dflt) -> std::string {
            if (auto* v = r.if_contains(k))
                if (v->is_string()) return std::string(v->get_string());
            return dflt;
        };
        auto geti = [&](const char* k) -> std::optional<int> {
            if (auto* v = r.if_contains(k)) {
                if (v->is_int64()) return static_cast<int>(v->get_int64());
                if (v->is_double()) return static_cast<int>(v->get_double());
            }
            return std::nullopt;
        };

        std::string text_en = gets("text_en", "");
        std::string bg_tone = gets("bg_tone", "dark");
        std::string txt_hex = gets("text_color", bg_tone == "dark" ? "#ffffff" : "#1a1a1a");
        std::string font_path = gets("font_path", default_font_path());
        std::optional<int> font_size = geti("font_size");  // nullopt → auto-fit
        std::string align = gets("align", "center");
        std::optional<int> anchor_x = geti("x");
        std::optional<int> anchor_y = geti("y");

        if (text_en.empty()) continue;

        // Resolve bbox (required unless explicit x,y given)
        std::vector<int> bbox;
        if (auto* bb = r.if_contains("bbox"))
            if (bb->is_array())
                for (const auto& v : bb->get_array()) {
                    if (v.is_int64()) bbox.push_back(static_cast<int>(v.get_int64()));
                    else if (v.is_double()) bbox.push_back(static_cast<int>(v.get_double()));
                }
        int x1, y1, x2, y2, bw, bh;
        if (bbox.size() >= 4) {
            x1 = std::max(0, bbox[0]); y1 = std::max(0, bbox[1]);
            x2 = std::min(w, std::max(0, bbox[2])); y2 = std::min(h, std::max(0, bbox[3]));
            bw = x2 - x1; bh = y2 - y1;
        } else if (anchor_x && anchor_y) {
            x1 = *anchor_x; y1 = *anchor_y; x2 = w; y2 = h;
            bw = w - *anchor_x; bh = h - *anchor_y;
        } else {
            continue;
        }
        if (bw < 4 || bh < 4) continue;

        // Inpaint only when we have a real bbox (not just an anchor)
        if (bbox.size() >= 4) {
            auto bg = sample_bg(result, x1, y1, x2, y2);
            for (int y = y1; y < y2; ++y)
                for (int x = x1; x < x2; ++x) {
                    std::uint8_t* d = result.px(x, y);
                    d[0] = bg[0]; d[1] = bg[1]; d[2] = bg[2];
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
            font = fit_font(flat, font_path, bw - 8, bh - 4);
        }
        if (!font) {
            font = Font::load(FONT_FALLBACK, font_size ? *font_size : 12);
            if (!font) continue;
        }

        // Split into lines
        std::vector<std::string> lines;
        {
            std::size_t pos = 0;
            while (pos <= text_en.size()) {
                std::size_t nl = text_en.find('\n', pos);
                lines.push_back(text_en.substr(
                    pos, nl == std::string::npos ? std::string::npos : nl - pos));
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        }
        if (lines.size() == 1 && !font_size)
            lines = wrap_lines(text_en, *font, bw - 8);

        int line_h = font->measure("Ay").y1 + 4;
        int total_h = line_h * static_cast<int>(lines.size());

        int ty = anchor_y ? *anchor_y : y1 + std::max(4, (bh - total_h) / 2);

        for (const auto& line : lines) {
            TextBBox bb = font->measure(line);
            int lw = bb.width();
            int tx;
            if (anchor_x) tx = *anchor_x;
            else if (align == "left") tx = x1 + 6;
            else if (align == "right") tx = x2 - lw - 6;
            else tx = x1 + (bw - lw) / 2;
            font->draw(result, tx, ty, line, tc);
            ty += line_h;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Manual overrides — hand-tuned entries that bypass Claude detection.
// ---------------------------------------------------------------------------

// sp0065a..f — Miyazawa 'Spring and Asura' preface, progressive 6-frame
// reveal onto sp0065a (the blank background) for idempotent re-runs.
Image epigraph_render(const CgOptions& opt, int n_lines, const Image& img) {
    const std::array<std::uint8_t, 3> INK = {38, 26, 18};  // dark sepia
    struct Line { const char* text; int y; int x; int size; };
    const std::vector<Line> LINES = {
        {"If this be nothingness, then nothingness itself is just this,", 316, 260, 28},
        {"and to some degree, it is shared by us all;",                   357, 390, 28},
        {"for just as everything within me is everyone,",                 512, 440, 28},
        {"so too is everything within each and every one.",               553, 350, 28},
        {"  -- 'Spring and Asura' Preface",                               614, 950, 22},
    };

    if (n_lines == 0) return img;
    Image base = img;
    if (auto blank = decode_entry(opt.game_dir + "\\data02501.arc.backup", "sp0065a"))
        base = *blank;
    else if (auto blank2 = decode_entry(opt.game_dir + "\\data02501.arc", "sp0065a"))
        base = *blank2;
    std::string fp = default_font_path();
    for (int i = 0; i < n_lines && i < static_cast<int>(LINES.size()); ++i) {
        const Line& ln = LINES[static_cast<std::size_t>(i)];
        auto font = Font::load(fp, ln.size);
        if (!font) font = Font::load(FONT_FALLBACK, ln.size);
        if (font) font->draw(base, ln.x, ln.y, ln.text, INK);
    }
    return base;
}

// sp0067a..f — Nakahara Chuuya 'Spring Day Rhapsody' (春日狂想), progressive
// reveal onto sp0067a (the blank background) so petals are never boxy-inpainted.
Image chuuya_render(const CgOptions& opt, int frame, const Image& img) {
    const std::array<std::uint8_t, 3> INK = {236, 232, 246};
    struct Line { const char* text; int y; int x; int size; };
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

    if (frame == 0) return img;
    Image base = img;
    if (auto blank = decode_entry(opt.game_dir + "\\data02501.arc.backup", "sp0067a"))
        base = *blank;
    else if (auto blank2 = decode_entry(opt.game_dir + "\\data02501.arc", "sp0067a"))
        base = *blank2;

    std::string fp = default_font_path();
    if (frame >= 1 && frame <= 4) {
        for (int i = 0; i < frame && i < static_cast<int>(STANZA_1.size()); ++i) {
            const Line& ln = STANZA_1[static_cast<std::size_t>(i)];
            auto font = Font::load(fp, ln.size);
            if (!font) font = Font::load(FONT_FALLBACK, ln.size);
            if (font) font->draw(base, ln.x, ln.y, ln.text, INK);
        }
    } else if (frame == 5) {
        for (const auto& ln : STANZA_2) {
            auto font = Font::load(fp, ln.size);
            if (!font) font = Font::load(FONT_FALLBACK, ln.size);
            if (font) font->draw(base, ln.x, ln.y, ln.text, INK);
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
            for (int n = 0; n < 6; ++n) {
                std::string entry = std::string("sp0065") + static_cast<char>('a' + n);
                t.emplace_back(std::make_pair(std::string("data02501.arc"), entry),
                               [n](const CgOptions& o, const Image& img) {
                                   return epigraph_render(o, n, img);
                               });
            }
            for (int n = 0; n < 6; ++n) {
                std::string entry = std::string("sp0067") + static_cast<char>('a' + n);
                t.emplace_back(std::make_pair(std::string("data02501.arc"), entry),
                               [n](const CgOptions& o, const Image& img) {
                                   return chuuya_render(o, n, img);
                               });
            }
            // Add more hand-tuned entries here.
            return t;
        }();
    return table;
}

const OverrideFn* find_override(const std::string& arc, const std::string& entry) {
    for (const auto& [key, fn] : manual_overrides())
        if (key.first == arc && key.second == entry) return &fn;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Main pipeline
// ---------------------------------------------------------------------------

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

// Phase 1: enumerate every entry in all SCAN_ARCS.  No Claude calls.
void extract_all_assets(const CgOptions& opt) {
    std::size_t total = 0;
    log_info("=== Listing all assets ===");
    for (const auto& arc_name : SCAN_ARCS) {
        std::string arc_path = opt.game_dir + "\\" + arc_name;
        if (!fs::exists(fs::u8path(arc_path))) {
            log_info("[skip] " + arc_name + " — not found");
            continue;
        }
        auto entries = bgi::list_arc(arc_path);
        log_info("  " + arc_name + ": " + std::to_string(entries.size()) + " entries");
        total += entries.size();
    }
    log_info("\n" + std::to_string(total) + " entries total across " +
             std::to_string(SCAN_ARCS.size()) + " archive(s).");
}

// Y/N prompt gating any Claude API call.
bool confirm_send_to_claude() {
    for (;;) {
        print_inline("\nSend assets to Claude API for translation? [Y/N]: ");
        std::string ans;
        if (!std::getline(std::cin, ans)) return false;
        std::string t;
        for (char c : ans)
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // strip
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        if (t == "y" || t == "yes") return true;
        if (t == "n" || t == "no") return false;
        log_info("Please answer Y or N.");
    }
}

void save_candidates(const Paths& paths, const bj::array& candidates) {
    write_file(paths.candidates, json_pretty(bj::value(candidates), 2));
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
    for (const auto& [arc, entry] : done)
        arr.push_back(bj::array{arc, entry});
    write_file(paths.scanned, bj::serialize(bj::value(std::move(arr))));
}

bj::array run_scan(const CgOptions& opt, anthropic::LazyClient& client, bool resume) {
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
                done_keys.emplace(std::string(pair.at(0).get_string()),
                                  std::string(pair.at(1).get_string()));
            }
        }
        log_info("Resuming: " + std::to_string(candidates.size()) + " found, " +
                 std::to_string(done_keys.size()) + " total scanned.");
    }

    // Inject manual overrides that aren't already in the candidates list.
    for (const auto& [key, fn] : manual_overrides()) {
        if (done_keys.count(key)) continue;
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

    // No local OCR pre-filter: every entry below is sent to the vision API,
    // which is what decides whether a CG carries narrative text.
    log_info("[ocr] 0 method(s): none");

    // Every archive is listed before anything is sent, so the coverage count
    // and the scan below walk one and the same list.
    std::vector<ArcEntries> arcs;
    std::vector<ScanKey> targets;
    for (const auto& arc_name : SCAN_ARCS) {
        const std::string arc_path = opt.game_dir + "\\" + arc_name;
        if (!fs::exists(fs::u8path(arc_path))) {
            log_info("[skip] " + arc_name + " — not found");
            continue;
        }

        ArcEntries ae;
        ae.arc = arc_name;
        ae.entries = bgi::list_arc(arc_path);
        log_info("\n=== " + arc_name + " (" + std::to_string(ae.entries.size()) +
                 " entries) ===");

        for (std::size_t i = 0; i < ae.entries.size(); ++i)
            if (!SKIP_ENTRIES.count(ae.entries[i].name)) ae.scan_idx.push_back(i);
        for (const std::size_t i : ae.scan_idx)
            targets.emplace_back(arc_name, ae.entries[i].name);
        arcs.push_back(std::move(ae));
    }

    // The resume file can already cover every target, and then the loop below
    // makes no request at all.  Resolved here rather than inside the loop: its
    // handlers catch everything, so a missing key would be reported once per
    // image and the run would still end successfully.
    if (count_left_to_scan(targets, done_keys)) client.get();

    for (const auto& ae : arcs) {
        const std::string arc_path = opt.game_dir + "\\" + ae.arc;
        const std::string& arc_name = ae.arc;
        const auto& entries = ae.entries;

        // i indexes the whole listing, which is what read_entry() takes and
        // what the progress numbers count over, so they run 1..entries.size()
        // and leave a gap at every skipped entry.
        for (const std::size_t i : ae.scan_idx) {
            const auto& [name, off, sz] = std::tie(entries[i].name, entries[i].offset,
                                                   entries[i].size);
            if (!needs_scan(done_keys, {arc_name, name})) continue;

            // Quick dimension pre-check without full decode
            Bytes raw = bgi::read_entry(arc_path, i, /*decompress=*/false);
            if (auto dims = cbg_dimensions(raw))
                if (dims->first < MIN_WIDTH || dims->second < MIN_HEIGHT)
                    continue;  // thumbnails / sprites

            print_inline("  [" + std::to_string(i + 1) + "/" +
                         std::to_string(entries.size()) + "] " + name + " (" +
                         std::to_string(sz / 1024) + "KB)... ");

            auto img = decode_entry(arc_path, name);
            if (!img) {
                log_info("decode failed");
                continue;
            }

            DetectResult det = detect_narrative(client.get(), *img);
            if (!det.has_text) {
                log_info("no narrative text");
            } else {
                char conf[16];
                std::snprintf(conf, sizeof(conf), "%.2f", det.confidence);
                log_info("FOUND (conf=" + std::string(conf) + "): " +
                         det.description.substr(0, 80));
                bj::object c;
                c["arc"] = arc_name;
                c["entry"] = name;
                c["confidence"] = det.confidence;
                c["description"] = det.description;
                c["regions"] = nullptr;
                candidates.push_back(std::move(c));
            }

            done_keys.emplace(arc_name, name);
            save_candidates(paths, candidates);
            save_scanned(paths, done_keys);
        }
    }

    log_info("\nScan done. " + std::to_string(candidates.size()) +
             " candidate(s) found -> " + paths.candidates);
    return candidates;
}

void run_translate(const CgOptions& opt, anthropic::LazyClient& client,
                   bj::array& candidates) {
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

        Image patched;
        if (const OverrideFn* fn = find_override(arc, entry)) {
            log_info(" [manual fn]");
            patched = (*fn)(opt, *img);
            c["regions"] = bj::array{};
        } else {
            log_info(" [auto]");
            // Already resolved by the caller, which counted the same
            // candidates this branch admits.  Reaching it is what would prove
            // the two had drifted, and it throws rather than letting the run
            // continue.
            bj::array regions = extract_and_translate(client.get(), *img);
            if (regions.empty()) {
                log_info("    no regions extracted");
                c["regions"] = bj::array{};
                continue;
            }
            log_info("    " + std::to_string(regions.size()) + " region(s):");
            for (const auto& rv : regions) {
                const auto& r = rv.get_object();
                std::string jp, en;
                if (auto* v = r.if_contains("text_jp"))
                    if (v->is_string()) jp = std::string(v->get_string());
                if (auto* v = r.if_contains("text_en"))
                    if (v->is_string()) en = std::string(v->get_string());
                log_info("      JP: " + jp.substr(0, 60));
                log_info("      EN: " + en.substr(0, 80));
            }
            patched = render_translation(*img, regions);
            c["regions"] = regions;
        }

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
        if (auto* p = c.if_contains("patched_bmp"))
            if (p->is_string()) bmp = std::string(p->get_string());
        // The path the image was actually found at, which is what the
        // replacement is read from: a recorded path from before the project
        // folder moved no longer names a readable file.
        bmp = resolve_patched_path(bmp, paths.out_dir);
        if (!bmp.empty()) {
            by_arc[arc][entry] = read_file(bmp);
            continue;
        }
        std::string arc_src = opt.game_dir + "\\" + arc + ".backup";
        if (!fs::exists(fs::u8path(arc_src)))
            arc_src = opt.game_dir + "\\" + arc;
        if (const OverrideFn* fn = find_override(arc, entry)) {
            if (auto img = decode_entry(arc_src, entry))
                by_arc[arc][entry] = encode_bmp((*fn)(opt, *img));
        } else if (auto* r = c.if_contains("regions");
                   r && r->is_array() && !r->get_array().empty()) {
            if (auto img = decode_entry(arc_src, entry))
                by_arc[arc][entry] =
                    encode_bmp(render_translation(*img, r->get_array()));
        }
    }

    if (by_arc.empty()) {
        log_info("Nothing to repack.");
        return;
    }

    for (const auto& [arc_name, replacements] : by_arc) {
        std::string arc_path = opt.game_dir + "\\" + arc_name;
        std::string bak = arc_path + ".backup";
        std::string tmp = arc_path + ".tmp";
        log_info("\nRepacking " + arc_name + " (" +
                 std::to_string(replacements.size()) + " entries)...");
        try {
            bgi::repack_arc(arc_path, tmp, replacements);
        } catch (const std::exception& ex) {
            // One archive that cannot be rebuilt -- missing, unreadable or of
            // an unexpected shape -- must not take the other archives with it.
            log_info(std::string("  ERROR: ") + ex.what());
            std::error_code ec;
            fs::remove(fs::u8path(tmp), ec);
            continue;
        }
        if (!fs::exists(fs::u8path(bak)))
            fs::copy_file(fs::u8path(arc_path), fs::u8path(bak));
        fs::rename(fs::u8path(tmp), fs::u8path(arc_path));
        log_info("  deployed -> " + arc_path + "  (backup: " + bak + ")");
    }
}

}  // namespace

bool needs_scan(const ScanKeySet& done, const ScanKey& key) {
    return done.find(key) == done.end();
}

std::size_t count_left_to_scan(const std::vector<ScanKey>& targets, const ScanKeySet& done) {
    std::size_t left = 0;
    for (const auto& key : targets)
        if (needs_scan(done, key)) ++left;
    return left;
}

std::string resolve_patched_path(const std::string& recorded,
                                 const std::string& patched_dir) {
    if (recorded.empty()) return {};
    if (fs::exists(fs::u8path(recorded))) return recorded;
    if (patched_dir.empty()) return {};
    const std::string name = fs::u8path(recorded).filename().u8string();
    if (name.empty()) return {};
    const std::string here = patched_dir + "\\" + name;
    return fs::exists(fs::u8path(here)) ? here : std::string();
}

bool needs_translation(const bj::object& candidate, const std::string& patched_dir) {
    const bool has_regions =
        candidate.if_contains("regions") && !candidate.at("regions").is_null();
    std::string patched_path;
    if (auto* p = candidate.if_contains("patched_bmp"))
        if (p->is_string()) patched_path = std::string(p->get_string());
    return !(has_regions &&
             !resolve_patched_path(patched_path, patched_dir).empty());
}

std::size_t count_left_to_translate(const bj::array& candidates,
                                    const std::string& patched_dir) {
    std::size_t left = 0;
    for (const auto& cv : candidates) {
        const auto& c = cv.get_object();
        if (!needs_translation(c, patched_dir)) continue;
        // The pass paints a hand-tuned entry from the override table itself,
        // so those entries cost no request however many of them are pending.
        if (find_override(std::string(c.at("arc").get_string()),
                          std::string(c.at("entry").get_string())))
            continue;
        ++left;
    }
    return left;
}

int run_find_narrative_cg(const CgOptions& opt) {
    g_project_dir = opt.project_dir;
    const Paths paths = make_paths(opt);
    fs::create_directories(fs::u8path(opt.project_dir + "\\script_output"));

    // Runs on both paths; only the non-repack path requires a key.
    anthropic::load_api_key();

    if (opt.repack_only) {
        if (!fs::exists(fs::u8path(paths.candidates))) {
            log_info("ERROR: " + paths.candidates +
                     " not found. Run without --repack first.");
            return 1;
        }
        bj::array candidates = json_parse_file(paths.candidates).get_array();
        run_repack(opt, candidates);
    } else {
        // Phase 1: enumerate everything locally — no Claude calls yet.
        extract_all_assets(opt);
        // Phase 2: explicit user confirmation before any Claude API call.
        if (!confirm_send_to_claude()) {
            log_info("Aborted by user. No Claude API calls made.");
            return 0;
        }
        // Phase 3: scan + translate + repack.  Nothing below builds a client
        // until it has a request to make, so --repack, an abort at the prompt
        // and a scan the resume file already covers all run without a key.
        anthropic::LazyClient client;
        bj::array candidates = run_scan(opt, client, /*resume=*/!opt.no_resume);
        if (!opt.scan_only && !candidates.empty()) {
            // The pass sends a request only for a candidate it has not already
            // painted and no override table entry covers, so a fully patched
            // candidates list -- or one holding nothing but the injected
            // overrides -- leaves it nothing to send.  Resolved here rather
            // than inside the pass, so a run that cannot proceed stops before
            // it decodes an entry or rewrites the candidates file.
            if (count_left_to_translate(candidates, paths.out_dir)) client.get();
            run_translate(opt, client, candidates);
            run_repack(opt, candidates);
        }
    }

    log_info("\nDone.");
    return 0;
}

}  // namespace snu::cg
