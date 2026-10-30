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
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "artemis/pfs_archive.h"
#include "common/util.h"
#include "image.h"
#include "translate/gemini_client.h"

namespace snt::cg {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* MODEL = "gemini-flash-latest";

// Minimum image dimensions to bother scanning (cheap pre-filter).
constexpr int MIN_WIDTH = 800;
constexpr int MIN_HEIGHT = 400;

// Image path-prefix filter: only scan these.  Sprites (image\fg) and tiny UI
// images are excluded -- they never contain narrative text.
const std::vector<std::string>& scan_prefixes() {
    static const std::vector<std::string> v = {"image\\ev\\", "image\\cg\\", "image\\bg\\"};
    return v;
}

const char* DETECT_PROMPT =
    R"(You are analyzing a visual novel CG image to determine if it contains narrative Japanese text.

"Narrative text" means text that is part of the story -- such as:
  - A poem, literary quote, or passage overlaid on the artwork
  - A chapter or scene title card with story-relevant Japanese text

Do NOT classify as narrative: staff credits, legal disclaimers, UI buttons, shop signs painted into backgrounds, character name plates, or any text rendered by a standard text box.

Answer with a JSON object and nothing else:
{
  "has_narrative_text": true or false,
  "confidence": 0.0-1.0,
  "description": "<brief description of what text you see, if any>"
}
)";

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with_any(const std::string& s, const std::vector<std::string>& prefixes) {
    for (const auto& p : prefixes)
        if (s.rfind(p, 0) == 0) return true;
    return false;
}

// (w, h) from a PNG IHDR chunk, or nullopt.
std::optional<std::pair<int, int>> png_dimensions(const Bytes& raw) {
    static const std::uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (raw.size() < 24 || std::memcmp(raw.data(), SIG, 8) != 0) return std::nullopt;
    if (std::memcmp(raw.data() + 12, "IHDR", 4) != 0) return std::nullopt;
    auto be32 = [&](std::size_t o) {
        return (static_cast<int>(raw[o]) << 24) | (static_cast<int>(raw[o + 1]) << 16) |
               (static_cast<int>(raw[o + 2]) << 8) | static_cast<int>(raw[o + 3]);
    };
    return std::make_pair(be32(16), be32(20));
}

struct Target {
    std::string arc_path;
    std::string name;
};

std::vector<Target> collect_image_targets(const pfs::Overlay& ov) {
    std::vector<Target> out;
    for (const auto& [name, loc] : ov.entries) {
        if (!ends_with(name, ".png")) continue;
        if (!starts_with_any(name, scan_prefixes())) continue;
        out.push_back(Target{loc.first, name});
    }
    return out;
}

// One vision call: image + prompt -> response text.
std::string vision(gemini::Client& client, const std::string& prompt, const Image& img) {
    print_inline(std::string("[gemini ") + MODEL + "] ");
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

    bj::value resp = client.messages(bj::value(std::move(body)), 60);
    return std::string(resp.get_object().at("content").get_array()[0].get_object()
                           .at("text").get_string());
}

struct Detection {
    bool has_text = false;
    double confidence = 0.0;
    std::string description;
};

Detection detect_narrative(gemini::Client& client, const Image& img) {
    // Very wide images are tiled so no single upload loses detail.
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
            // (?s) so '.' spans newlines: the model pretty-prints the JSON
            // object across several lines.
            static const boost::regex OBJ_RE_S("(?s)\\{.*?\\}");
            if (!boost::regex_search(resp, m, OBJ_RE_S)) continue;
            bj::value data = bj::parse(m[0].str());
            const auto& o = data.get_object();
            bool has = false;
            if (auto* v = o.if_contains("has_narrative_text"))
                has = v->is_bool() && v->get_bool();
            double conf = 0.0;
            if (auto* v = o.if_contains("confidence")) {
                if (v->is_double()) conf = v->get_double();
                else if (v->is_int64()) conf = static_cast<double>(v->get_int64());
            }
            if (has && conf >= 0.7) {
                Detection d;
                d.has_text = true;
                d.confidence = conf;
                if (auto* v = o.if_contains("description"))
                    if (v->is_string()) d.description = std::string(v->get_string());
                return d;
            }
        } catch (const std::exception& e) {
            log_info(std::string("    detect error: ") + e.what());
        }
    }
    return {};
}

// The local OCR pre-filter would go here, rejecting text-free images before
// they cost vision tokens.  No OCR engine is wired in, so the count is zero
// and the filter is skipped -- logged explicitly so a run that quietly burns
// tokens on every image is never a surprise.
void log_ocr_status() { log_info("[ocr] 0 method(s): none"); }

std::string basename_of(const std::string& path) {
    return fs::u8path(path).filename().u8string();
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

int run_find_narrative_cg(const CgOptions& opt) {
    const std::string out_dir = opt.project_dir + "\\script_output";
    const std::string candidates_file = out_dir + "\\narrative_candidates.json";
    const std::string scanned_file = out_dir + "\\narrative_scanned.json";
    fs::create_directories(fs::u8path(out_dir));

    // ---- Phase 1: enumerate everything locally -- no Gemini calls yet ----
    log_info("=== Listing all assets ===");
    pfs::Overlay ov = pfs::build_overlay(opt.game_dir);
    std::vector<Target> targets = collect_image_targets(ov);
    {
        std::map<std::string, std::size_t> by_arc;
        for (const auto& t : targets) ++by_arc[basename_of(t.arc_path)];
        for (const auto& [arc, n] : by_arc)
            log_info("  " + arc + ": " + std::to_string(n) + " entries");
        log_info("\n" + std::to_string(targets.size()) + " scan target(s) total across " +
                 std::to_string(by_arc.size()) + " archive(s).");
    }

    // ---- Phase 2: explicit confirmation before any Gemini API call ----
    if (!opt.assume_yes) {
        for (;;) {
            print_inline("\nSend assets to Gemini API for translation? [Y/N]: ");
            std::string ans;
            if (!std::getline(std::cin, ans)) return 0;
            ans = trim(ans);
            std::transform(ans.begin(), ans.end(), ans.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ans == "y" || ans == "yes") break;
            if (ans == "n" || ans == "no") {
                log_info("Aborted by user. No Gemini API calls made.");
                return 0;
            }
            log_info("Please answer Y or N.");
        }
    }

    // ---- Phase 3: scan ----
    bj::array candidates;
    ScanKeySet done_keys;

    if (!opt.no_resume && fs::exists(fs::u8path(candidates_file))) {
        candidates = json_parse_file(candidates_file).get_array();
        for (const auto& cv : candidates) {
            const auto& c = cv.get_object();
            done_keys.emplace(std::string(c.at("arc").get_string()),
                              std::string(c.at("entry").get_string()));
        }
        if (fs::exists(fs::u8path(scanned_file))) {
            // Bind the parsed value: in C++17 a range-for does NOT extend the
            // lifetime of a temporary the range initialiser only refers into,
            // so iterating json_parse_file(...).get_array() directly walks
            // freed memory.
            const bj::value scanned_v = json_parse_file(scanned_file);
            for (const auto& pv : scanned_v.get_array()) {
                const auto& pair = pv.get_array();
                done_keys.emplace(std::string(pair[0].get_string()),
                                  std::string(pair[1].get_string()));
            }
        }
        log_info("Resuming: " + std::to_string(candidates.size()) + " found, " +
                 std::to_string(done_keys.size()) + " total scanned.");
    }

    log_info("\nTotal scan targets: " + std::to_string(targets.size()));
    if (opt.max_images && static_cast<std::size_t>(*opt.max_images) < targets.size()) {
        targets.resize(static_cast<std::size_t>(*opt.max_images));
        log_info("Limited to first " + std::to_string(*opt.max_images) + " images");
    }

    log_ocr_status();

    // Enumerated here rather than in the loop, so the coverage count and the
    // scan below walk one and the same list.
    std::vector<ScanKey> scan_keys;
    scan_keys.reserve(targets.size());
    for (const auto& t : targets) scan_keys.emplace_back(basename_of(t.arc_path), t.name);

    // The resume file can already cover every target, and then the loop below
    // makes no request at all.  Resolved here rather than inside the loop:
    // its handlers catch everything, so a missing key would be reported once
    // per image and the run would still end successfully.
    gemini::LazyClient client;
    if (count_left_to_scan(scan_keys, done_keys)) client.get();

    auto flush_state = [&] {
        write_file_text(candidates_file, json_pretty(bj::value(candidates), 2));
        bj::array scanned;
        for (const auto& [arc, entry] : done_keys)
            scanned.push_back(bj::array{arc, entry});
        write_file(scanned_file, json_dump(bj::value(std::move(scanned))));
    };

    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto& t = targets[i];
        const std::string arc_basename = basename_of(t.arc_path);
        if (!needs_scan(done_keys, {arc_basename, t.name})) continue;

        Bytes raw;
        try {
            raw = pfs::read_entry(t.arc_path, ov.entries.at(t.name).second,
                                  ov.keys.at(t.arc_path));
        } catch (const std::exception& e) {
            log_info("  read failed: " + t.name + ": " + e.what());
            done_keys.emplace(arc_basename, t.name);
            continue;
        }

        // Quick dimension pre-check.
        if (auto dims = png_dimensions(raw))
            if (dims->first < MIN_WIDTH || dims->second < MIN_HEIGHT) {
                done_keys.emplace(arc_basename, t.name);
                continue;
            }

        print_inline("  [" + std::to_string(i + 1) + "/" + std::to_string(targets.size()) +
                     "] " + t.name + " (" + std::to_string(raw.size() / 1024) + "KB)... ");

        auto img = decode_image(raw);
        if (!img) {
            log_info("decode failed");
            done_keys.emplace(arc_basename, t.name);
            continue;
        }

        // OCR pre-filter passes everything through (see log_ocr_status).
        Detection d = detect_narrative(client.get(), *img);
        if (!d.has_text) {
            log_info("no narrative text");
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", d.confidence);
            log_info("FOUND (conf=" + std::string(buf) + "): " + d.description.substr(0, 80));
            bj::object c;
            c["arc"] = arc_basename;
            c["entry"] = t.name;
            c["confidence"] = d.confidence;
            c["description"] = d.description;
            c["regions"] = nullptr;
            candidates.push_back(std::move(c));
        }

        done_keys.emplace(arc_basename, t.name);
        flush_state();
    }

    log_info("\nScan done. " + std::to_string(candidates.size()) + " candidate(s) found -> " +
             candidates_file);

    if (opt.scan_only) {
        log_info("\n--scan-only: skipping translate/render");
        return 0;
    }
    if (!candidates.empty()) {
        log_info("\n" + std::to_string(candidates.size()) + " candidates ready for translation.");
        log_info("  (run translate phase separately when ready)");
    }
    log_info("\nDone.");
    return 0;
}

}  // namespace snt::cg
