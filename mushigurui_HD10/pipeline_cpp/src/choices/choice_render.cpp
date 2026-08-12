// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "choice_render.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <boost/json.hpp>

#include "cg/text_render.h"
#include "common/util.h"
#include "translate/anthropic_client.h"

namespace mgi::choices {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

const char* const MODEL = "claude-sonnet-4-6";

// Most-stylish-first; arialbd is the last resort.
const std::vector<std::string> FONT_CANDIDATES = {
    "C:/Windows/Fonts/trebucbd.ttf",  // Trebuchet MS Bold
    "C:/Windows/Fonts/verdanab.ttf",  // Verdana Bold
    "C:/Windows/Fonts/segoeuib.ttf",  // Segoe UI Bold
    "C:/Windows/Fonts/calibrib.ttf",  // Calibri Bold
    "C:/Windows/Fonts/arialbd.ttf",   // fallback
};

const char* const PROMPT =
    R"__(You are looking at a Japanese visual novel CHOICE MENU screenshot.
Yellow text labels sit on blue parallelogram buttons stacked vertically on the screen.

Translate each Japanese choice into English. Constraints:
  - Faithful to the meaning — do not paraphrase to the point of distortion.
  - SHORT: prefer 4-7 words per choice when possible. The English must fit
    inside the same blue button at a similar visual width to the JP.
  - Casual, natural conversational English (not literal/wooden).
  - Do not add commentary, ellipses, or punctuation that wasn't in the JP.

Return ONLY a JSON array of strings (one per choice), top to bottom, no prose, no fences:
  ["...", "...", "..."]
)__";

bool path_exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(fs::u8path(p), ec);
}

std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Median of the samples: an even count averages the two middles, and the cast
// back to uint8 TRUNCATES.
std::uint8_t median_u8(std::vector<std::uint8_t>& v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    const double m = (n % 2) ? static_cast<double>(v[n / 2])
                             : (static_cast<double>(v[n / 2 - 1]) +
                                static_cast<double>(v[n / 2])) / 2.0;
    return static_cast<std::uint8_t>(static_cast<int>(m));
}

// msg.content[0].text, with the ``` fences and surrounding prose stripped.
std::vector<std::string> parse_translations(const std::string& reply) {
    std::string raw = trim(reply);
    // re.sub(r"^```(?:json)?\s*", "") then re.sub(r"\s*```$", "")
    if (raw.rfind("```", 0) == 0) {
        std::size_t i = 3;
        if (raw.compare(i, 4, "json") == 0) i += 4;
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        raw = raw.substr(i);
    }
    if (raw.size() >= 3 && raw.compare(raw.size() - 3, 3, "```") == 0) {
        std::size_t e = raw.size() - 3;
        while (e > 0 && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
        raw = raw.substr(0, e);
    }

    const std::size_t start = raw.find('[');
    const std::size_t end = raw.rfind(']');
    if (start == std::string::npos || end == std::string::npos || end < start)
        throw std::runtime_error("no JSON array in Claude reply: " + raw);

    const bj::value v = bj::parse(raw.substr(start, end + 1 - start));
    if (!v.is_array()) throw std::runtime_error("expected a list of strings");
    const bj::array& a = v.get_array();
    if (a.size() < 2) throw std::runtime_error("expected a list of strings");
    std::vector<std::string> out;
    for (const auto& e : a) {
        if (!e.is_string()) throw std::runtime_error("expected a list of strings");
        out.push_back(std::string(e.get_string()));
    }
    return out;
}

std::vector<std::string> get_translations(anthropic::Client& client,
                                          const std::string& img_path) {
    bj::object source;
    source["type"] = "base64";
    source["media_type"] = "image/jpeg";
    source["data"] = cg::base64_encode(read_file(img_path));
    bj::object image_block;
    image_block["type"] = "image";
    image_block["source"] = std::move(source);
    bj::object text_block;
    text_block["type"] = "text";
    text_block["text"] = PROMPT;
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = bj::array{std::move(image_block), std::move(text_block)};

    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = 512;
    body["messages"] = bj::array{std::move(msg)};

    const bj::value resp = client.messages(bj::value(std::move(body)));
    const bj::array& content = resp.get_object().at("content").get_array();
    if (content.empty()) throw std::runtime_error("empty response content");
    return parse_translations(
        std::string(content.at(0).get_object().at("text").get_string()));
}

}  // namespace

namespace detail {

std::string resolve_font() {
    for (const auto& p : FONT_CANDIDATES)
        if (path_exists(p)) return p;
    return "";
}

std::vector<Box> detect_choice_boxes(const cg::Image& img) {
    const int H = img.height, W = img.width;
    std::vector<std::uint8_t> blue(static_cast<std::size_t>(H) * W, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::uint8_t* p = img.at(x, y);
            const int R = p[0], G = p[1], B = p[2];
            blue[static_cast<std::size_t>(y) * W + x] =
                (B - G > 25 && G - R < 30 && G > 55 && G < 160 && B > 100) ? 1 : 0;
        }

    // Rows where >20% of the pixels are button-blue.
    std::vector<std::pair<int, int>> bands;
    bool in_band = false;
    int start = 0;
    for (int y = 0; y < H; ++y) {
        int count = 0;
        for (int x = 0; x < W; ++x) count += blue[static_cast<std::size_t>(y) * W + x];
        const bool hit = W > 0 && static_cast<double>(count) / W > 0.20;
        if (hit && !in_band) {
            start = y;
            in_band = true;
        } else if (!hit && in_band) {
            bands.emplace_back(start, y);
            in_band = false;
        }
    }
    if (in_band) bands.emplace_back(start, H);

    std::vector<std::pair<int, int>> merged;
    for (const auto& b : bands) {
        if (!merged.empty() && b.first - merged.back().second <= 3)
            merged.back().second = b.second;
        else
            merged.push_back(b);
    }

    std::vector<Box> boxes;
    for (const auto& [y0, y1] : merged) {
        if (y1 - y0 < 20) continue;
        int first = -1, last = -1;
        for (int x = 0; x < W; ++x) {
            int count = 0;
            for (int y = y0; y < y1; ++y)
                count += blue[static_cast<std::size_t>(y) * W + x];
            if (static_cast<double>(count) / (y1 - y0) > 0.5) {
                if (first < 0) first = x;
                last = x;
            }
        }
        if (first < 0) continue;
        boxes.push_back({first, y0, last + 1, y1});
    }
    return boxes;
}

cg::Image render_replacement(const cg::Image& img, const std::vector<Box>& boxes,
                             const std::vector<std::string>& texts,
                             const std::string& font_path) {
    const cg::Image src = img.channels == 4 ? cg::drop_alpha(img) : img;
    cg::Image out = src;

    for (const auto& b : boxes) {
        const int h = b.y1 - b.y0, w = b.x1 - b.x0;
        if (h <= 0 || w <= 0) continue;

        // Per row: the median of the non-yellow pixels (all of them if the row
        // is entirely yellow).
        std::vector<std::array<std::uint8_t, 3>> col(static_cast<std::size_t>(h));
        for (int i = 0; i < h; ++i) {
            std::vector<std::uint8_t> ch[3];
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* p = src.at(b.x0 + x, b.y0 + i);
                if (!(p[0] > 180 && p[1] > 140 && p[2] < 120))
                    for (int c = 0; c < 3; ++c) ch[c].push_back(p[c]);
            }
            if (ch[0].empty())
                for (int x = 0; x < w; ++x) {
                    const std::uint8_t* p = src.at(b.x0 + x, b.y0 + i);
                    for (int c = 0; c < 3; ++c) ch[c].push_back(p[c]);
                }
            for (int c = 0; c < 3; ++c)
                col[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)] =
                    median_u8(ch[c]);
        }

        constexpr int inset_x = 22, inset_y = 4;
        const int fx0 = b.x0 + inset_x, fx1 = b.x1 - inset_x;
        const int fy0 = b.y0 + inset_y, fy1 = b.y1 - inset_y;
        for (int y = fy0; y < fy1; ++y) {
            if (y < 0 || y >= out.height) continue;
            const auto& band = col[static_cast<std::size_t>(inset_y + (y - fy0))];
            for (int x = fx0; x < fx1; ++x) {
                if (x < 0 || x >= out.width) continue;
                std::uint8_t* d = out.at(x, y);
                for (int c = 0; c < 3; ++c) d[c] = band[static_cast<std::size_t>(c)];
            }
        }
    }

    constexpr int kStroke = 2;
    const std::array<std::uint8_t, 4> FILL = {255, 215, 50, 255};
    const std::array<std::uint8_t, 4> STROKE = {40, 25, 5, 255};

    for (std::size_t i = 0; i < boxes.size() && i < texts.size(); ++i) {
        const Box& b = boxes[i];
        const std::string& text = texts[i];
        const int avail_w = (b.x1 - b.x0) - 50;
        const int avail_h = (b.y1 - b.y0) - 12;

        std::unique_ptr<cg::Font> font;
        for (int size = avail_h; size > 10; --size) {
            font = cg::Font::load(font_path, size);
            if (!font) continue;
            const cg::TextBBox bb = font->measure(text, kStroke);
            if (bb.width() <= avail_w && bb.height() <= avail_h) break;
        }
        if (!font) continue;  // box too short for any usable size (avail_h <= 10)

        const cg::TextBBox bb = font->measure(text, kStroke);
        const double cx = (b.x0 + b.x1) / 2.0 - bb.width() / 2.0 - bb.x0;
        const double cy = (b.y0 + b.y1) / 2.0 - bb.height() / 2.0 - bb.y0;
        font->draw(out, static_cast<int>(cx), static_cast<int>(cy), text, FILL,
                   kStroke, STROKE);
    }
    return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

int run_replace_choice_text(const Options& opt) {
    const std::string base = opt.project_dir + "\\choices";
    const std::string cache_file = base + "\\_cache.json";

    anthropic::load_api_key();
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) {
        std::fputs("ERROR: ANTHROPIC_API_KEY env var not set\n", stderr);
        return 1;
    }

    const std::string font_path = detail::resolve_font();
    if (font_path.empty()) {
        print_line("No bold font found; tried the FONT_CANDIDATES list");
        return 1;
    }

    anthropic::Client client;

    bj::object cache;
    if (path_exists(cache_file)) {
        try {
            cache = json_parse_file(cache_file).get_object();
        } catch (const std::exception& e) {
            print_line(std::string("WARNING: cache file unreadable (") + e.what() +
                       "); starting fresh");
        }
    }
    const auto save_cache = [&] {
        write_file_text(cache_file, json_pretty(bj::value(cache), 2));
    };

    print_line("Font: " + font_path);
    print_line("Cache: " + cache_file + " (" + std::to_string(cache.size()) +
               (cache.size() == 1 ? " entry)" : " entries)"));

    // pathlib.glob is case-insensitive on Windows, and sorted() on Path
    // compares the case-folded name.
    std::vector<std::string> sources;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(fs::u8path(base), ec)) {
        if (!de.is_regular_file()) continue;
        const std::string name = de.path().filename().u8string();
        const std::string lower = ascii_lower(name);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".jpg") != 0) continue;
        const std::string stem = lower.substr(0, lower.size() - 4);
        if (stem.size() >= 3 && stem.compare(stem.size() - 3, 3, "_en") == 0) continue;
        sources.push_back(name);
    }
    std::sort(sources.begin(), sources.end(),
              [](const std::string& a, const std::string& b) {
                  return ascii_lower(a) < ascii_lower(b);
              });

    if (sources.empty()) {
        print_line("no source jpgs found");
        return 0;
    }
    {
        std::string joined;
        for (std::size_t i = 0; i < sources.size(); ++i)
            joined += (i ? ", " : "") + sources[i];
        print_line("Found " + std::to_string(sources.size()) + " source image(s): " +
                   joined);
    }

    for (const auto& name : sources) {
        try {
            const std::string src = base + "\\" + name;
            const std::string stem = name.substr(0, name.size() - 4);
            const std::string dst = base + "\\" + stem + "_en.jpg";

            std::vector<std::string> texts;
            bool cached = false;
            if (auto* v = cache.if_contains(name)) {
                cached = true;
                for (const auto& e : v->get_array()) texts.push_back(std::string(e.get_string()));
            }

            if (cached && path_exists(dst)) {
                print_line("\n" + name + ": cached + rendered, skip");
                for (std::size_t i = 0; i < texts.size(); ++i)
                    print_line("  [" + std::to_string(i + 1) + "] " + texts[i]);
                continue;
            }
            if (cached) {
                print_line("\n" + name + ": using cached translations (re-rendering)");
            } else {
                print_line("\n" + name + ": requesting translations from Claude…");
                texts = get_translations(client, src);
                bj::array arr;
                for (const auto& t : texts) arr.push_back(bj::string(t));
                cache[name] = std::move(arr);
                save_cache();  // persist incrementally so a crash doesn't re-bill
            }

            for (std::size_t i = 0; i < texts.size(); ++i)
                print_line("  [" + std::to_string(i + 1) + "] " + texts[i]);

            const auto img = cg::open_image(src);
            if (!img) throw std::runtime_error("cannot open " + src);
            const cg::Image rgb = img->channels == 4 ? cg::drop_alpha(*img) : *img;
            const auto boxes = detail::detect_choice_boxes(rgb);

            std::string shape = "[";
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                if (i) shape += ", ";
                shape += "(" + std::to_string(boxes[i].x0) + ", " +
                         std::to_string(boxes[i].y0) + ", " +
                         std::to_string(boxes[i].x1) + ", " +
                         std::to_string(boxes[i].y1) + ")";
            }
            shape += "]";
            print_line("  detected " + std::to_string(boxes.size()) + " blue boxes: " +
                       shape);

            if (boxes.size() != texts.size())
                throw std::runtime_error("detected " + std::to_string(boxes.size()) +
                                         " boxes but got " + std::to_string(texts.size()) +
                                         " translations in " + name);

            const cg::Image patched =
                detail::render_replacement(rgb, boxes, texts, font_path);
            write_file(dst, cg::encode_jpg(patched, 95));
            print_line("  saved -> " + stem + "_en.jpg");
        } catch (const std::exception& e) {
            print_line("  FAILED on " + name + ": " + e.what());
        }
    }

    save_cache();
    return 0;
}

}  // namespace mgi::choices
