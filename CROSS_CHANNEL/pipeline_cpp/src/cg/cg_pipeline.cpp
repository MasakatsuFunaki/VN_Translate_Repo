// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cg_pipeline.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "common/util.h"
#include "image.h"
#include "text_render.h"
#include "translate/anthropic_client.h"

namespace crc::cg {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

// Sonnet, NOT the step-2 model: these are cheap one-shot vision calls.
constexpr const char* MODEL = "claude-sonnet-4-6";
constexpr int MIN_WIDTH = 180;
constexpr int MIN_HEIGHT = 60;
constexpr const char* FONT_PATH = "C:/Windows/Fonts/MTCORSVA.TTF";
constexpr const char* FONT_FALLBACK = "C:/Windows/Fonts/arial.ttf";

// sys.cpk only.  bg.cpk is 1.7 GB / 3437 files -- scanning it would cost a
// vision call per background for almost no translatable text.
const std::vector<std::string>& scan_arcs() {
    static const std::vector<std::string> v = {"sys.cpk"};
    return v;
}

const char* const DETECT_PROMPT = R"(You are analyzing a visual novel image to determine if it contains Japanese text that the player would benefit from seeing in English.

Translate-worthy text includes:
  - Narrative content: poems, literary quotes, prose passages overlaid on artwork
  - Chapter or scene title cards with story-relevant Japanese text
  - Menu / UI labels: file, open, close, save, load, settings, options, exit, yes/no, back, skip, auto, log/backlog, config, quit, etc.
  - Button labels and dialog box headers

Answer with a JSON object and nothing else:
{
  "has_narrative_text": true or false,
  "confidence": 0.0-1.0,
  "description": "<brief description of what text you see, if any>"
})";

// {W} / {H} are substituted per image; the doubled braces below are literal
// template text and collapse to single braces in the request that is sent.
const char* const EXTRACT_PROMPT_TEMPLATE = R"(This image contains Japanese text. It may be narrative prose (a poem, quote, chapter title) or a menu / UI element (button label, dialog header, save/load prompt, settings panel, etc.).

For UI labels, use a short idiomatic English equivalent — "Save", "Load", "Settings", "Options", "Back", "Skip", "Auto", "Log", "Quit", "Yes" / "No" — NOT a literal word-for-word translation.

For narrative prose, give a natural high-quality English translation that preserves tone and rhythm.

Extract every Japanese text element and provide:
1. The original Japanese text (exact)
2. The English translation
3. The bounding box [x1, y1, x2, y2] of that text region in the image (pixel coords)
4. Approximate text color as a hex string (e.g. "#2a1a0e")
5. Whether the background behind the text is light or dark: "light" or "dark"

Image dimensions: {W}x{H}

Respond with a JSON array and nothing else:
[
  {{
    "text_jp": "...",
    "text_en": "...",
    "bbox": [x1, y1, x2, y2],
    "text_color": "#rrggbb",
    "bg_tone": "light" or "dark"
  }},
  ...
])";

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

// Codepoint-aware truncation: cutting bytes could split a multi-byte
// character and produce mojibake in the log.
std::string truncate_cp(const std::string& s, std::size_t n) {
    std::size_t i = 0, k = 0;
    while (i < s.size() && k < n) {
        utf8_next(s, i);
        ++k;
    }
    return s.substr(0, i);
}

std::string json_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key))
        if (v->is_string()) return std::string(v->get_string());
    return {};
}

double json_num(const bj::value& v) {
    if (v.is_double()) return v.get_double();
    if (v.is_int64()) return static_cast<double>(v.get_int64());
    if (v.is_uint64()) return static_cast<double>(v.get_uint64());
    return 0.0;
}

struct Paths {
    std::string game_data;      // <game_dir>\data
    std::string output_dir;     // <project>\script_output
    std::string extract_dir;    // ...\narrative_extracted
    std::string out_dir;        // ...\narrative_patched
    std::string candidates;     // ...\narrative_candidates.json
    std::string scanned;        // ...\narrative_scanned.json
    std::string extract_cpk;    // <repo>\TOOLS\garbro\extract_cpk.exe
};

Paths make_paths(const CgOptions& opt) {
    Paths p;
    p.game_data = opt.game_dir + "\\data";
    p.output_dir = opt.project_dir + "\\script_output";
    p.extract_dir = p.output_dir + "\\narrative_extracted";
    p.out_dir = p.output_dir + "\\narrative_patched";
    p.candidates = p.output_dir + "\\narrative_candidates.json";
    p.scanned = p.output_dir + "\\narrative_scanned.json";
    // TOOLS is shared by every game, so it sits one level above the project
    // folder.  Derived from --dir rather than compiled in: a build-time
    // absolute path would ship one machine's layout inside the binary.
    p.extract_cpk = (fs::absolute(fs::u8path(opt.project_dir)).parent_path() /
                     "TOOLS" / "garbro" / "extract_cpk.exe")
                        .lexically_normal()
                        .u8string();
    return p;
}

std::string stem_of(const std::string& arc_name) {
    const std::size_t dot = arc_name.rfind('.');
    return dot == std::string::npos ? arc_name : arc_name.substr(0, dot);
}

// ---- CPK extraction -------------------------------------------------------

// Returns { entry_name: [width, height, bmp_path] } in extract_cpk stdout
// order.  Insertion order is load-bearing (bj::object preserves it): it is
// the order candidates are scanned and written in downstream.
bj::object extract_archive(const Paths& p, const std::string& arc_name, bool force = false) {
    const std::string out_dir = p.extract_dir + "\\" + stem_of(arc_name);
    const std::string index_path = out_dir + "\\_index.json";

    // Cached-extraction path: works with the game uninstalled, which is what
    // makes this step testable offline.
    if (!force && fs::exists(fs::u8path(index_path)))
        return json_parse_file(index_path).get_object();

    fs::create_directories(fs::u8path(out_dir));

    const std::string err_file = out_dir + "\\_extract_cpk.stderr";
    const std::string cmd = "\"\"" + p.extract_cpk + "\" \"" + p.game_data + "\\" + arc_name +
                            "\" \"" + out_dir + "\" 2>\"" + err_file + "\"\"";
    std::string stdout_text;
    if (FILE* pipe = _popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe)) stdout_text += buf;
        _pclose(pipe);
    }

    bj::object index;
    std::size_t pos = 0;
    while (pos <= stdout_text.size()) {
        const std::size_t nl = stdout_text.find('\n', pos);
        std::string line =
            stdout_text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? stdout_text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::vector<std::string> parts;
        std::size_t t = 0;
        for (;;) {
            const std::size_t tab = line.find('\t', t);
            if (tab == std::string::npos) {
                parts.push_back(line.substr(t));
                break;
            }
            parts.push_back(line.substr(t, tab - t));
            t = tab + 1;
        }
        if (parts.size() != 3) continue;
        const std::string bmp_path = out_dir + "\\" + safe_filename(parts[0]) + ".bmp";
        if (!fs::exists(fs::u8path(bmp_path))) continue;
        try {
            index[parts[0]] = bj::array{std::stoi(parts[1]), std::stoi(parts[2]), bmp_path};
        } catch (const std::exception&) {
            continue;
        }
    }

    write_file_text(index_path, json_pretty(bj::value(index), 2));

    std::string stderr_tail = "done";
    {
        std::string err;
        try {
            const Bytes raw = read_file(err_file);
            err = std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
        } catch (const std::exception&) {
        }
        err = trim(err);
        const std::size_t last_nl = err.find_last_of('\n');
        if (!err.empty())
            stderr_tail = last_nl == std::string::npos ? err : err.substr(last_nl + 1);
    }
    log_info("  extract_cpk: " + stderr_tail);
    return index;
}

std::optional<Image> open_bmp(const std::string& bmp_path) {
    try {
        return decode_image(read_file(bmp_path));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ---- vision ---------------------------------------------------------------

std::string vision(anthropic::Client& client, const std::string& prompt, const Image& img) {
    print_inline(std::string("[claude ") + MODEL + "] calling api... ");
    bj::object source;
    source["type"] = "base64";
    source["media_type"] = "image/png";
    source["data"] = base64_encode(encode_png(img));

    bj::object img_block;
    img_block["type"] = "image";
    img_block["source"] = std::move(source);
    bj::object txt_block;
    txt_block["type"] = "text";
    txt_block["text"] = prompt;

    bj::object msg;
    msg["role"] = "user";
    msg["content"] = bj::array{std::move(img_block), std::move(txt_block)};

    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = 1024;
    body["messages"] = bj::array{std::move(msg)};

    // Vision replies carry no thinking block, so content[0] is the text.
    bj::value resp = client.messages(bj::value(std::move(body)), 60);
    return std::string(
        resp.get_object().at("content").get_array()[0].get_object().at("text").get_string());
}

struct Detection {
    bool has_text = false;
    double confidence = 0.0;
    std::string description;
};

Detection detect_narrative(anthropic::Client& client, const Image& img) {
    std::vector<Image> tiles;
    if (img.width > 2000) {
        const int tile_w = 1280;
        for (int x = 0; x < img.width; x += tile_w)
            tiles.push_back(crop(img, x, 0, std::min(x + tile_w, img.width), img.height));
    } else {
        tiles.push_back(img);
    }

    for (const auto& tile : tiles) {
        try {
            const std::string resp = vision(client, DETECT_PROMPT, tile);
            boost::smatch m;
            // NON-GREEDY, so the match stops at the FIRST '}' and any prose
            // Claude appends after the object is ignored; (?s) lets '.' cross
            // the newlines inside the pretty-printed reply.
            static const boost::regex OBJ_RE("(?s)\\{.*?\\}");
            if (!boost::regex_search(resp, m, OBJ_RE)) continue;
            const bj::value data = bj::parse(m[0].str());
            const auto& o = data.get_object();
            bool has = false;
            if (auto* v = o.if_contains("has_narrative_text"))
                has = v->is_bool() ? v->get_bool() : (v->is_string() && !v->get_string().empty());
            double conf = 0.0;
            if (auto* v = o.if_contains("confidence")) conf = json_num(*v);
            if (has && conf >= 0.7) {
                Detection d;
                d.has_text = true;
                d.confidence = conf;
                d.description = json_str(o, "description");
                return d;
            }
        } catch (const std::exception& e) {
            log_info(std::string("    detect error: ") + e.what());
        }
    }
    return {};
}

bj::array extract_and_translate(anthropic::Client& client, const Image& img) {
    std::string prompt = EXTRACT_PROMPT_TEMPLATE;
    prompt = replace_all(prompt, "{W}", std::to_string(img.width));
    prompt = replace_all(prompt, "{H}", std::to_string(img.height));
    prompt = replace_all(prompt, "{{", "{");
    prompt = replace_all(prompt, "}}", "}");

    double scale = 1.0;
    Image check_img = img;
    if (img.width > 1280) {
        scale = 1280.0 / img.width;
        check_img = resize(img, 1280, static_cast<int>(img.height * scale));
    }
    try {
        const std::string resp = vision(client, prompt, check_img);
        bool found = false;
        const std::string arr_text = first_json_array(resp, &found);
        if (!found) return {};
        bj::array regions = bj::parse(arr_text).get_array();
        if (scale < 1.0) {
            for (auto& rv : regions) {
                auto* bbox = rv.get_object().if_contains("bbox");
                if (!bbox || !bbox->is_array()) continue;
                bj::array scaled;
                for (const auto& v : bbox->get_array())
                    scaled.push_back(static_cast<std::int64_t>(json_num(v) / scale));
                *bbox = std::move(scaled);
            }
        }
        return regions;
    } catch (const std::exception& e) {
        log_info(std::string("    extract error: ") + e.what());
        return {};
    }
}

// ---- render ---------------------------------------------------------------

Image render_translation(const Image& img, const bj::array& regions) {
    Image result = img;
    const int w = result.width, h = result.height;
    const std::string default_font =
        fs::exists(fs::u8path(FONT_PATH)) ? FONT_PATH : FONT_FALLBACK;

    for (const auto& rv : regions) {
        if (!rv.is_object()) continue;
        const auto& r = rv.get_object();

        const std::string text_en = json_str(r, "text_en");
        const bj::array* bbox = nullptr;
        if (auto* v = r.if_contains("bbox"))
            if (v->is_array()) bbox = &v->get_array();
        std::string bg_tone = json_str(r, "bg_tone");
        if (bg_tone.empty()) bg_tone = "dark";
        std::string txt_hex = json_str(r, "text_color");
        if (txt_hex.empty()) txt_hex = (bg_tone == "dark") ? "#ffffff" : "#1a1a1a";
        std::string font_path = json_str(r, "font_path");
        if (font_path.empty()) font_path = default_font;
        std::optional<int> font_size;
        if (auto* v = r.if_contains("font_size"))
            if (v->is_int64() || v->is_double()) font_size = static_cast<int>(json_num(*v));
        std::string align = json_str(r, "align");
        if (align.empty()) align = "center";
        std::optional<int> anchor_x, anchor_y;
        if (auto* v = r.if_contains("x"))
            if (v->is_int64() || v->is_double()) anchor_x = static_cast<int>(json_num(*v));
        if (auto* v = r.if_contains("y"))
            if (v->is_int64() || v->is_double()) anchor_y = static_cast<int>(json_num(*v));

        if (text_en.empty()) continue;

        int x1, y1, x2, y2, bw, bh;
        const bool have_bbox = bbox && bbox->size() >= 4;
        if (have_bbox) {
            x1 = std::max(0, static_cast<int>(json_num((*bbox)[0])));
            y1 = std::max(0, static_cast<int>(json_num((*bbox)[1])));
            x2 = std::min(w, std::max(0, static_cast<int>(json_num((*bbox)[2]))));
            y2 = std::min(h, std::max(0, static_cast<int>(json_num((*bbox)[3]))));
            bw = x2 - x1;
            bh = y2 - y1;
        } else if (anchor_x && anchor_y) {
            x1 = *anchor_x;
            y1 = *anchor_y;
            x2 = w;
            y2 = h;
            bw = w - *anchor_x;
            bh = h - *anchor_y;
        } else {
            continue;
        }
        if (bw < 4 || bh < 4) continue;

        if (have_bbox) fill_rect(result, x1, y1, x2, y2, sample_bg(result, x1, y1, x2, y2));

        // Strip '#' only -- NOT whitespace: a colour that arrives as " ffffff"
        // is malformed and must fall through to the bg_tone default rather
        // than being silently accepted.
        Rgb tc{};
        {
            std::string hx = txt_hex;
            std::size_t k = 0;
            while (k < hx.size() && hx[k] == '#') ++k;
            hx = hx.substr(k);
            bool ok = hx.size() >= 6;
            for (std::size_t i = 0; ok && i < 6; ++i)
                ok = std::isxdigit(static_cast<unsigned char>(hx[i])) != 0;
            if (ok) {
                for (int c = 0; c < 3; ++c)
                    tc[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(
                        std::stoi(hx.substr(static_cast<std::size_t>(c) * 2, 2), nullptr, 16));
            } else {
                tc = (bg_tone == "dark") ? Rgb{255, 255, 255} : Rgb{30, 30, 30};
            }
        }

        Font font = font_size ? load_font(font_path, *font_size)
                              : fit_font(font_path, FONT_FALLBACK,
                                         replace_all(text_en, "\n", " "), bw - 8, bh - 4, 36);
        if (font_size && !font.valid()) font = load_font(FONT_FALLBACK, *font_size);
        if (!font.valid()) continue;

        std::vector<std::string> lines;
        {
            std::size_t pos = 0;
            for (;;) {
                const std::size_t nl = text_en.find('\n', pos);
                if (nl == std::string::npos) {
                    lines.push_back(text_en.substr(pos));
                    break;
                }
                lines.push_back(text_en.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }
        if (lines.size() == 1 && !font_size) lines = wrap_lines(font, text_en, bw - 8);

        const int line_h = text_bbox(font, "Ay").y1 + 4;
        const int total_h = line_h * static_cast<int>(lines.size());

        // FLOOR division, not truncation: (bh - total_h) and (bw - lw) go
        // negative whenever the text is taller or wider than its box, and
        // truncating toward zero would shift the centring by a pixel there.
        auto floordiv = [](int a, int b) {
            int q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
            return q;
        };
        int ty = anchor_y ? *anchor_y : y1 + std::max(4, floordiv(bh - total_h, 2));

        for (const auto& line : lines) {
            const BBox bb = text_bbox(font, line);
            const int lw = bb.x1 - bb.x0;
            int tx;
            if (anchor_x) tx = *anchor_x;
            else if (align == "left") tx = x1 + 6;
            else if (align == "right") tx = x2 - lw - 6;
            else tx = x1 + floordiv(bw - lw, 2);
            draw_text(result, tx, ty, font, line, tc);
            ty += line_h;
        }
    }
    return result;
}

// ---- OCR pre-filter -------------------------------------------------------

// No OCR backend is wired up, so the pre-filter passes every image through to
// Claude.  The banner is still printed once per run so the scan log is honest
// about how many backends voted (zero); --scan-only and the confirm prompt are
// what keep the bill in check.
bool g_ocr_logged = false;
void ocr_vote_log_once() {
    if (g_ocr_logged) return;
    g_ocr_logged = true;
    log_info("[ocr-init] loading backends...");
    log_info("[ocr] 0 method(s): none");
}

// ---- state files ----------------------------------------------------------

void write_state(const Paths& p, const bj::array& candidates,
                 const std::set<std::pair<std::string, std::string>>& done_keys) {
    write_file_text(p.candidates, json_pretty(bj::value(candidates), 2));
    bj::array scanned;
    for (const auto& [arc, entry] : done_keys) scanned.push_back(bj::array{arc, entry});
    // Compact form, ", " separators -- boost::json::serialize alone emits ","
    // and would not match the format this file is written in.
    write_file_text(p.scanned, json_dump(bj::value(std::move(scanned))));
}

// ---- phases ---------------------------------------------------------------

std::size_t extract_all_assets(const Paths& p) {
    std::size_t total = 0;
    log_info("=== Extracting all assets ===");
    for (const auto& arc : scan_arcs()) {
        // The CPK file itself must exist -- a cached _index.json is not enough.
        if (!fs::exists(fs::u8path(p.game_data + "\\" + arc))) {
            log_info("[skip] " + arc + " \xE2\x80\x94 not found");
            continue;
        }
        log_info("\n--- " + arc + " ---");
        total += extract_archive(p, arc).size();
    }
    log_info("\nExtracted " + std::to_string(total) + " image(s) total across " +
             std::to_string(scan_arcs().size()) + " archive(s).");
    return total;
}

bool confirm_send_to_claude() {
    for (;;) {
        print_inline("\nSend extracted assets to Claude API for translation? [Y/N]: ");
        std::string ans;
        if (!std::getline(std::cin, ans)) return false;
        ans = trim(ans);
        std::transform(ans.begin(), ans.end(), ans.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ans == "y" || ans == "yes") return true;
        if (ans == "n" || ans == "no") return false;
        log_info("Please answer Y or N.");
    }
}

bj::array run_scan(const Paths& p, anthropic::Client& client, bool resume) {
    bj::array candidates;
    std::set<std::pair<std::string, std::string>> done_keys;

    if (resume && fs::exists(fs::u8path(p.candidates))) {
        candidates = json_parse_file(p.candidates).get_array();
        for (const auto& cv : candidates)
            done_keys.emplace(json_str(cv.get_object(), "arc"), json_str(cv.get_object(), "entry"));
        if (fs::exists(fs::u8path(p.scanned))) {
            // Bind the parsed value: in C++17 a range-for does NOT extend the
            // lifetime of a temporary the range initialiser only refers into,
            // so iterating json_parse_file(...).get_array() directly walks
            // freed memory.
            const bj::value scanned_v = json_parse_file(p.scanned);
            for (const auto& pv : scanned_v.get_array()) {
                const auto& pair = pv.get_array();
                done_keys.emplace(std::string(pair[0].get_string()),
                                  std::string(pair[1].get_string()));
            }
        }
        log_info("Resuming: " + std::to_string(candidates.size()) + " found, " +
                 std::to_string(done_keys.size()) + " total scanned.");
    }

    for (const auto& arc : scan_arcs()) {
        if (!fs::exists(fs::u8path(p.game_data + "\\" + arc))) {
            log_info("[skip] " + arc + " \xE2\x80\x94 not found");
            continue;
        }
        log_info("\n=== " + arc + " \xE2\x80\x94 extracting... ===");
        const bj::object index = extract_archive(p, arc);

        struct EntryInfo {
            std::string name;
            int w, h;
            std::string bmp;
        };
        std::vector<EntryInfo> entries;
        for (const auto& kv : index) {
            const auto& info = kv.value().get_array();
            const int ew = static_cast<int>(json_num(info[0]));
            const int eh = static_cast<int>(json_num(info[1]));
            if (ew >= MIN_WIDTH && eh >= MIN_HEIGHT)
                entries.push_back(EntryInfo{std::string(kv.key()), ew, eh,
                                            std::string(info[2].get_string())});
        }
        log_info("  " + std::to_string(index.size()) + " images total, " +
                 std::to_string(entries.size()) + " meet size threshold");

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            if (done_keys.count({arc, e.name})) continue;

            print_inline("  [" + std::to_string(i + 1) + "/" + std::to_string(entries.size()) +
                         "] " + e.name + " (" + std::to_string(e.w) + "x" + std::to_string(e.h) +
                         ")... ");

            auto img = open_bmp(e.bmp);
            if (!img) {
                // NOT marked scanned -- an unreadable BMP is retried next run.
                log_info("open failed");
                continue;
            }

            ocr_vote_log_once();  // the vote itself always passes (0 backends)
            print_inline("ocr: \"\" \xE2\x86\x92 ");
            const Detection d = detect_narrative(client, *img);
            if (!d.has_text) {
                log_info("no narrative text");
            } else {
                char conf[32];
                std::snprintf(conf, sizeof(conf), "%.2f", d.confidence);
                log_info("FOUND (conf=" + std::string(conf) + "): " +
                         truncate_cp(d.description, 80));
                bj::object c;
                c["arc"] = arc;
                c["entry"] = e.name;
                c["confidence"] = d.confidence;
                c["description"] = d.description;
                c["regions"] = nullptr;
                c["bmp_path"] = e.bmp;
                candidates.push_back(std::move(c));
            }

            done_keys.emplace(arc, e.name);
            write_state(p, candidates, done_keys);
        }
    }

    log_info("\nScan done. " + std::to_string(candidates.size()) + " candidate(s) found -> " +
             p.candidates);
    return candidates;
}

void run_translate(const Paths& p, anthropic::Client& client, bj::array& candidates) {
    fs::create_directories(fs::u8path(p.out_dir));
    for (auto& cv : candidates) {
        auto& c = cv.get_object();
        const auto* regions_v = c.if_contains("regions");
        const std::string patched_bmp = json_str(c, "patched_bmp");
        if (regions_v && !regions_v->is_null() && !patched_bmp.empty() &&
            fs::exists(fs::u8path(patched_bmp))) {
            log_info("  " + json_str(c, "entry") + ": already translated, skip");
            continue;
        }

        const std::string arc = json_str(c, "arc");
        const std::string entry = json_str(c, "entry");
        print_inline("\n  " + arc + " / " + entry + "...");

        std::string bmp_path = json_str(c, "bmp_path");
        if (bmp_path.empty() || !fs::exists(fs::u8path(bmp_path))) {
            const bj::object index = extract_archive(p, arc);
            if (auto* info = index.if_contains(entry)) {
                bmp_path = std::string(info->get_array()[2].get_string());
                c["bmp_path"] = bmp_path;
            }
        }
        if (bmp_path.empty() || !fs::exists(fs::u8path(bmp_path))) {
            log_info(" bmp not found");
            continue;
        }

        auto img = open_bmp(bmp_path);
        if (!img) {
            log_info(" open failed");
            continue;
        }

        // Every candidate takes the automatic path; there is no per-entry
        // manual override table.
        log_info(" [auto]");
        bj::array regions = extract_and_translate(client, *img);
        if (regions.empty()) {
            log_info("    no regions extracted");
            c["regions"] = bj::array{};
            continue;
        }
        log_info("    " + std::to_string(regions.size()) + " region(s):");
        for (const auto& rv : regions) {
            const auto& r = rv.get_object();
            log_info("      JP: " + truncate_cp(json_str(r, "text_jp"), 60));
            log_info("      EN: " + truncate_cp(json_str(r, "text_en"), 80));
        }
        const Image patched = render_translation(*img, regions);
        c["regions"] = regions;

        const std::string out_bmp = p.out_dir + "\\" + entry + ".bmp";
        write_file(out_bmp, encode_bmp(patched));
        c["patched_bmp"] = out_bmp;
        log_info("    saved -> " + out_bmp);

        write_file_text(p.candidates, json_pretty(bj::value(candidates), 2));
    }
}

void run_repack(const Paths& p, const bj::array& candidates) {
    std::vector<const bj::object*> patched;
    for (const auto& cv : candidates) {
        const auto& c = cv.get_object();
        const std::string pb = json_str(c, "patched_bmp");
        if (!pb.empty() && fs::exists(fs::u8path(pb))) patched.push_back(&c);
    }
    if (patched.empty()) {
        log_info("Nothing patched yet.");
        return;
    }
    log_info("\n" + std::to_string(patched.size()) + " patched BMP(s) in " + p.out_dir);
    log_info("CPK repack not implemented \xE2\x80\x94 images saved for inspection.");
    for (const auto* c : patched)
        log_info("  " + json_str(*c, "arc") + " / " + json_str(*c, "entry") + " -> " +
                 json_str(*c, "patched_bmp"));
}

}  // namespace

std::string safe_filename(const std::string& name) {
    static const std::string INVALID = "\"<>|:*?\\/";
    std::string out;
    for (unsigned char ch : name)
        out += (ch < 0x20 || INVALID.find(static_cast<char>(ch)) != std::string::npos)
                   ? '_'
                   : static_cast<char>(ch);
    return out;
}

std::string first_json_array(const std::string& resp, bool* found) {
    if (found) *found = false;
    const std::size_t start = resp.find('[');
    if (start == std::string::npos) return {};
    int depth = 0;
    bool in_str = false, escape = false;
    // Byte-wise is safe: UTF-8 continuation bytes are >= 0x80 and can never
    // equal an ASCII delimiter.
    for (std::size_t i = start; i < resp.size(); ++i) {
        const char ch = resp[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (in_str) {
            if (ch == '\\') escape = true;
            else if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') in_str = true;
        else if (ch == '[') ++depth;
        else if (ch == ']') {
            --depth;
            if (depth == 0) {
                if (found) *found = true;
                return resp.substr(start, i - start + 1);
            }
        }
    }
    return {};
}

int run_find_narrative_cg(const CgOptions& opt) {
    const Paths p = make_paths(opt);
    fs::create_directories(fs::u8path(p.output_dir));

    if (anthropic::load_api_key().empty()) {
        log_info("ERROR: ANTHROPIC_API_KEY not set.");
        return 1;
    }

    // Phase 1: extract everything locally -- no Claude calls yet.
    extract_all_assets(p);

    // Phase 2: explicit confirmation before any Claude API call.
    if (!opt.assume_yes && !confirm_send_to_claude()) {
        log_info("Aborted by user. No Claude API calls made.");
        return 0;  // NOTE: the trailing "Done." is skipped on this path
    }

    anthropic::Client client;
    bj::array candidates = run_scan(p, client, !opt.no_resume);
    if (!opt.scan_only && !candidates.empty()) {
        run_translate(p, client, candidates);
        run_repack(p, candidates);
    }

    log_info("\nDone.");
    return 0;
}

}  // namespace crc::cg
