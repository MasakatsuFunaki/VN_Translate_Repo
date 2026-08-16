// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cg_pipeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "common/util.h"
#include "gpk/gpk_archive.h"
#include "translate/anthropic_client.h"

namespace mgi::cg {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace detail {

const char* const FONT_PATH = "C:/Windows/Fonts/MTCORSVA.TTF";
const char* const FONT_FALLBACK = "C:/Windows/Fonts/arial.ttf";

}  // namespace detail

namespace {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

const std::vector<std::string> SCAN_ARCS = {"ev0.gpk", "ta0.gpk", "sys0.gpk"};

constexpr int MIN_WIDTH = 180;
constexpr int MIN_HEIGHT = 60;

const char* const MODEL = "claude-sonnet-4-6";

// ---------------------------------------------------------------------------
// Prompts.  Wrapped fragments are joined with NO space inserted -- the line
// breaks below are source formatting only and must not reach the model.
// ---------------------------------------------------------------------------

const char* const DETECT_PROMPT =
    R"__(You are analyzing a visual novel image to determine if it contains Japanese text that the player would benefit from seeing in English.

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
})__";

std::string extract_prompt(int w, int h) {
    return R"__(This image contains Japanese text. It may be narrative prose (a poem, quote, chapter title) or a menu / UI element (button label, dialog header, save/load prompt, settings panel, etc.).

For UI labels, use a short idiomatic English equivalent — "Save", "Load", "Settings", "Options", "Back", "Skip", "Auto", "Log", "Quit", "Yes" / "No" — NOT a literal word-for-word translation.

For narrative prose, give a natural high-quality English translation that preserves tone and rhythm.

Extract every Japanese text element and provide:
1. The original Japanese text (exact)
2. The English translation
3. The bounding box [x1, y1, x2, y2] of that text region in the image (pixel coords)
4. Approximate text color as a hex string (e.g. "#2a1a0e")
5. Whether the background behind the text is light or dark: "light" or "dark"

Image dimensions: )__" +
           std::to_string(w) + "x" + std::to_string(h) + R"__(

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
// Small path / fs helpers
// ---------------------------------------------------------------------------

bool path_exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(fs::u8path(p), ec);
}

// os.path.splitext: split at the last '.' of the final path component, unless
// it is the component's leading character.
std::pair<std::string, std::string> splitext(const std::string& p) {
    const std::size_t sep = p.find_last_of("\\/");
    const std::size_t base = (sep == std::string::npos) ? 0 : sep + 1;
    const std::size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || dot <= base) return {p, ""};
    return {p.substr(0, dot), p.substr(dot)};
}

std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

struct Paths {
    std::string dwq_dir;
    std::string output_dir;
    std::string candidates;
    std::string scanned;
    std::string out_dir;
    std::string extract_dir;
};

Paths make_paths(const CgOptions& opt) {
    Paths p;
    p.dwq_dir = opt.game_dir + "\\dwq";
    p.output_dir = opt.project_dir + "\\script_output";
    p.candidates = p.output_dir + "\\narrative_candidates.json";
    p.scanned = p.output_dir + "\\narrative_scanned.json";
    p.out_dir = p.output_dir + "\\narrative_patched";
    p.extract_dir = p.output_dir + "\\narrative_extracted";
    return p;
}

// ---------------------------------------------------------------------------
// Archive extraction + on-disk index cache
// ---------------------------------------------------------------------------

struct IndexEntry {
    int width = 0;
    int height = 0;
    std::string path;
};

// R5: insertion-ordered with upsert semantics -- a duplicate entry name
// overwrites in place rather than appending a second key, which would change
// both _index.json and the counts.
class ArchiveIndex {
public:
    void upsert(const std::string& name, IndexEntry e) {
        auto it = pos_.find(name);
        if (it != pos_.end()) {
            items_[it->second].second = std::move(e);
            return;
        }
        pos_.emplace(name, items_.size());
        items_.emplace_back(name, std::move(e));
    }
    const IndexEntry* find(const std::string& name) const {
        auto it = pos_.find(name);
        return it == pos_.end() ? nullptr : &items_[it->second].second;
    }
    std::size_t size() const { return items_.size(); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }

private:
    std::vector<std::pair<std::string, IndexEntry>> items_;
    std::map<std::string, std::size_t> pos_;
};

ArchiveIndex extract_archive(const CgOptions& opt, const std::string& arc_name,
                             bool force = false) {
    const Paths paths = make_paths(opt);
    const std::string arc_path = paths.dwq_dir + "\\" + arc_name;
    const std::string arc_stem = splitext(arc_name).first;
    const std::string out_dir = paths.extract_dir + "\\" + arc_stem;
    const std::string index_path = out_dir + "\\_index.json";

    ArchiveIndex index;

    // Disk cache: a rerun after a crash never reopens the GPK.
    if (!force && path_exists(index_path)) {
        const bj::value v = json_parse_file(index_path);
        for (const auto& kv : v.get_object()) {
            const bj::array& a = kv.value().get_array();
            index.upsert(std::string(kv.key()),
                         {static_cast<int>(a.at(0).to_number<std::int64_t>()),
                          static_cast<int>(a.at(1).to_number<std::int64_t>()),
                          std::string(a.at(2).get_string())});
        }
        return index;
    }

    fs::create_directories(fs::u8path(out_dir));
    const std::string gtb_path = splitext(arc_path).first + ".gtb";
    const gpk::Archive arc = gpk::read_archive(arc_path, gtb_path);

    for (const auto& e : arc.entries) {
        const std::string ext = detail::ext_for_type_tag(e.type_tag);
        if (ext.empty()) continue;  // non-image entry (conditional preambles, ...)
        if (e.width < 1 || e.height < 1) continue;

        const Bytes payload = gpk::extract_entry(arc.gpk_buf, e.offset);
        const std::string out_path = out_dir + "\\" + detail::safe_name(e.name) + ext;
        write_file(out_path, payload);
        index.upsert(e.name, {static_cast<int>(e.width), static_cast<int>(e.height),
                              out_path});
    }

    bj::object obj;
    for (const auto& [name, info] : index)
        obj[name] = bj::array{info.width, info.height, info.path};
    write_file_text(index_path, json_pretty(bj::value(std::move(obj)), 2));

    log_info("  extracted " + std::to_string(index.size()) + " images from " + arc_name);
    return index;
}

// ---------------------------------------------------------------------------
// OCR pre-filter
// ---------------------------------------------------------------------------

// The OCR pre-filter is a cost optimisation, not a correctness requirement:
// with no backend available it degrades to "every image passes", logs the
// summary line once, and Claude does all the classification.  Keep the vote
// seam so a backend can be dropped in later without touching the scan loop.
bool g_ocr_initialised = false;

std::pair<bool, std::string> ocr_vote(const Image&) {
    if (!g_ocr_initialised) {
        g_ocr_initialised = true;
        log_info("[ocr] 0 method(s): none");
    }
    return {true, ""};
}

// ---------------------------------------------------------------------------
// Claude vision
// ---------------------------------------------------------------------------

// --replay transcript, consumed in call order.  The seam sits below request
// construction on purpose: a replayed run still builds and logs the real
// request body, so a prompt regression shows up offline.
std::vector<std::string> g_replay;
std::size_t g_replay_pos = 0;
bool g_replay_active = false;

std::string claude_vision(anthropic::Client& client, const std::string& prompt,
                          const Image& img) {
    print_inline(std::string("[claude ") + MODEL + "] calling api... ");

    if (g_replay_active) {
        if (g_replay_pos >= g_replay.size())
            throw std::runtime_error("list index out of range");
        const bj::value resp = bj::parse(g_replay[g_replay_pos++]);
        const bj::array& content = resp.get_object().at("content").get_array();
        if (content.empty()) throw std::runtime_error("list index out of range");
        return std::string(content.at(0).get_object().at("text").get_string());
    }

    const Image upload = img.channels == 4 ? drop_alpha(img) : img;

    bj::object source;
    source["type"] = "base64";
    source["media_type"] = "image/png";
    source["data"] = base64_encode(encode_png(upload));
    bj::object image_block;
    image_block["type"] = "image";
    image_block["source"] = std::move(source);
    bj::object text_block;
    text_block["type"] = "text";
    text_block["text"] = prompt;
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = bj::array{std::move(image_block), std::move(text_block)};

    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = 1024;
    body["messages"] = bj::array{std::move(msg)};

    const bj::value resp = client.messages(bj::value(std::move(body)), /*timeout_sec=*/60);
    const bj::array& content = resp.get_object().at("content").get_array();
    if (content.empty()) throw std::runtime_error("list index out of range");
    return std::string(content.at(0).get_object().at("text").get_string());
}

// ---------------------------------------------------------------------------
// Detect / extract
// ---------------------------------------------------------------------------

struct DetectResult {
    bool has_text = false;
    bj::value confidence = bj::value(0.0);  // stored VERBATIM: an int stays an int
    std::string description;
};

DetectResult detect_narrative(anthropic::Client& client, const Image& img) {
    std::vector<Image> tiles;
    if (img.width > 2000) {
        constexpr int tile_w = 1280;
        for (int x = 0; x < img.width; x += tile_w)
            tiles.push_back(crop(img, x, 0, std::min(x + tile_w, img.width), img.height));
    } else {
        tiles.push_back(img);
    }

    for (const auto& tile : tiles) {
        try {
            const std::string resp = claude_vision(client, DETECT_PROMPT, tile);
            const auto text = detail::first_json_object_text(resp);
            if (!text) continue;  // no JSON in the reply: try the next tile
            // A malformed object throws here and is reported by the catch
            // below -- it is NOT a silent skip.
            const bj::value data = bj::parse(*text);
            const bj::object& o = data.get_object();
            if (detail::detect_hit(o)) {
                DetectResult r;
                r.has_text = true;
                r.confidence = o.at("confidence");
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

bj::array extract_and_translate(anthropic::Client& client, const Image& img) {
    const std::string prompt = extract_prompt(img.width, img.height);
    Image check_img = img;
    double scale = 1.0;
    if (img.width > 1280) {
        scale = 1280.0 / img.width;
        check_img = resize(img, 1280, static_cast<int>(img.height * scale));
    }

    try {
        const std::string resp = claude_vision(client, prompt, check_img);
        const auto text = detail::balanced_json_array_text(resp);
        if (!text) return {};  // no array in the reply -> no regions
        bj::array regions = bj::parse(*text).get_array();
        if (scale < 1.0) {
            // A missing or non-numeric bbox throws INSIDE the try, so a single
            // bad region discards every region for this image.
            for (auto& rv : regions) {
                bj::object& r = rv.get_object();
                bj::array scaled;
                for (const auto& v : r.at("bbox").get_array())
                    scaled.push_back(static_cast<std::int64_t>(
                        std::trunc(v.to_number<double>() / scale)));
                r["bbox"] = std::move(scaled);
            }
        }
        return regions;
    } catch (const std::exception& e) {
        log_info(std::string("    extract error: ") + e.what());
        return {};
    }
}

// ---------------------------------------------------------------------------
// Manual overrides -- EMPTY in this game.  Kept as live scaffolding so a
// hand-tuned entry can be dropped in without re-deriving the control flow.
// ---------------------------------------------------------------------------

struct ManualOverride {
    std::function<Image(const Image&)> fn;  // callable branch
    std::optional<bj::array> regions;       // list branch
};

const std::vector<std::pair<std::pair<std::string, std::string>, ManualOverride>>&
manual_overrides() {
    static const std::vector<std::pair<std::pair<std::string, std::string>, ManualOverride>>
        table;
    return table;
}

const ManualOverride* find_override(const std::string& arc, const std::string& entry) {
    for (const auto& [key, ov] : manual_overrides())
        if (key.first == arc && key.second == entry) return &ov;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Pipeline stages
// ---------------------------------------------------------------------------

std::size_t extract_all_assets(const CgOptions& opt) {
    const Paths paths = make_paths(opt);
    std::size_t total = 0;
    log_info("=== Extracting all assets ===");
    for (const auto& arc_name : SCAN_ARCS) {
        if (!path_exists(paths.dwq_dir + "\\" + arc_name)) {
            log_info("[skip] " + arc_name + " — not found");
            continue;
        }
        log_info("\n--- " + arc_name + " ---");
        total += extract_archive(opt, arc_name).size();
    }
    log_info("\nExtracted " + std::to_string(total) + " image(s) total across " +
             std::to_string(SCAN_ARCS.size()) + " archive(s).");
    return total;
}

bool confirm_send_to_claude() {
    for (;;) {
        print_inline("\nSend extracted assets to Claude API for translation? [Y/N]: ");
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        const std::string ans = ascii_lower(trim(line));
        if (ans == "y" || ans == "yes") return true;
        if (ans == "n" || ans == "no") return false;
        log_info("Please answer Y or N.");
    }
}

// One archive's entries that clear the size threshold, in scan order.
struct ArcEntries {
    std::string arc;
    std::vector<std::pair<std::string, IndexEntry>> entries;
};

void save_scanned(const Paths& paths, const detail::ScanKeySet& done) {
    // The resume file is written sorted so a diff between two runs is
    // meaningful; std::set<std::pair<...>> already gives codepoint order,
    // because UTF-8 byte order preserves it.
    bj::array arr;
    for (const auto& [arc, entry] : done) arr.push_back(bj::array{arc, entry});
    write_file_text(paths.scanned, json_pretty(bj::value(std::move(arr)), 2));
}

bj::array run_scan(const CgOptions& opt, anthropic::LazyClient& client, bool resume) {
    const Paths paths = make_paths(opt);
    bj::array candidates;
    detail::ScanKeySet done_keys;

    if (resume && path_exists(paths.candidates)) {
        candidates = json_parse_file(paths.candidates).get_array();
        for (const auto& cv : candidates) {
            const bj::object& c = cv.get_object();
            done_keys.emplace(std::string(c.at("arc").get_string()),
                              std::string(c.at("entry").get_string()));
        }
        if (path_exists(paths.scanned)) {
            // Bind the parsed value: in C++17 a range-for does NOT extend the
            // lifetime of a temporary the range initialiser only refers into,
            // so iterating json_parse_file(...).get_array() directly walks
            // freed memory.
            const bj::value scanned_v = json_parse_file(paths.scanned);
            for (const auto& pv : scanned_v.get_array()) {
                const bj::array& pair = pv.get_array();
                done_keys.emplace(std::string(pair.at(0).get_string()),
                                  std::string(pair.at(1).get_string()));
            }
        }
        log_info("Resuming: " + std::to_string(candidates.size()) + " found, " +
                 std::to_string(done_keys.size()) + " total scanned.");
    }

    for (const auto& [key, ov] : manual_overrides()) {
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

    // Every archive is enumerated before anything is sent, so the coverage
    // count and the scan below walk one and the same list.
    std::vector<ArcEntries> arcs;
    std::vector<detail::ScanKey> targets;
    for (const auto& arc_name : SCAN_ARCS) {
        if (!path_exists(paths.dwq_dir + "\\" + arc_name)) {
            log_info("[skip] " + arc_name + " — not found");
            continue;
        }

        log_info("\n=== " + arc_name + " — extracting... ===");
        const ArchiveIndex index = extract_archive(opt, arc_name);

        ArcEntries ae;
        ae.arc = arc_name;
        for (const auto& [name, info] : index)
            if (info.width >= MIN_WIDTH && info.height >= MIN_HEIGHT)
                ae.entries.emplace_back(name, info);
        log_info("  " + std::to_string(index.size()) + " images total, " +
                 std::to_string(ae.entries.size()) + " meet size threshold");

        for (const auto& [name, info] : ae.entries) targets.emplace_back(arc_name, name);
        arcs.push_back(std::move(ae));
    }

    // The resume file can already cover every target, and then the loop below
    // makes no request at all.  Resolved here rather than inside the loop: its
    // handlers catch everything, so a missing key would be reported once per
    // image and the run would still end successfully.
    if (detail::count_left_to_scan(targets, done_keys)) client.get();

    for (const auto& ae : arcs) {
        const std::string& arc_name = ae.arc;
        const std::vector<std::pair<std::string, IndexEntry>>& entries = ae.entries;

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const std::string& name = entries[i].first;
            const IndexEntry& info = entries[i].second;
            // i still advances over an entry the resume set already covers.
            if (!detail::needs_scan(done_keys, {arc_name, name})) continue;

            print_inline("  [" + std::to_string(i + 1) + "/" +
                         std::to_string(entries.size()) + "] " + name + " (" +
                         std::to_string(info.width) + "x" +
                         std::to_string(info.height) + ")... ");

            const auto img = open_image(info.path);
            if (!img) {
                // NOT marked done: a resumed run retries the decode, and the
                // key never lands in narrative_scanned.json.
                log_info("open failed");
                continue;
            }

            const auto [ocr_pass, ocr_snippet] = ocr_vote(*img);
            if (!ocr_pass) {
                log_info("no japanese (ocr)");
            } else {
                print_inline("ocr: \"" + ocr_snippet + "\" → ");
                const DetectResult det = detect_narrative(client.get(), *img);
                if (!det.has_text) {
                    log_info("no narrative text");
                } else {
                    char conf[32];
                    std::snprintf(conf, sizeof(conf), "%.2f",
                                  det.confidence.to_number<double>());
                    log_info("FOUND (conf=" + std::string(conf) + "): " +
                             utf8_prefix(det.description, 80));
                    bj::object c;
                    c["arc"] = arc_name;
                    c["entry"] = name;
                    c["confidence"] = det.confidence;
                    c["description"] = det.description;
                    c["regions"] = nullptr;
                    c["img_path"] = info.path;
                    candidates.push_back(std::move(c));
                }
            }

            done_keys.emplace(arc_name, name);
            write_file_text(paths.candidates, json_pretty(bj::value(candidates), 2));
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
        bj::object& c = cv.get_object();
        const std::string arc(c.at("arc").get_string());
        const std::string entry(c.at("entry").get_string());

        if (!detail::needs_translation(c, paths.out_dir)) {
            log_info("  " + entry + ": already translated, skip");
            continue;
        }

        print_inline("\n  " + arc + " / " + entry + "...");

        std::string img_path;
        if (auto* p = c.if_contains("img_path"))
            if (p->is_string()) img_path = std::string(p->get_string());
        if (img_path.empty() || !path_exists(img_path)) {
            const ArchiveIndex index = extract_archive(opt, arc);
            if (const IndexEntry* info = index.find(entry)) {
                img_path = info->path;
                c["img_path"] = img_path;
            }
        }
        if (img_path.empty() || !path_exists(img_path)) {
            log_info(" image not found");
            continue;
        }

        const auto img = open_image(img_path);
        if (!img) {
            log_info(" open failed");
            continue;
        }

        const ManualOverride* override_ = find_override(arc, entry);

        Image patched;
        if (override_ && override_->fn) {
            log_info(" [manual fn]");
            patched = override_->fn(*img);
            c["regions"] = bj::array{};
        } else if (override_ && override_->regions) {
            log_info(" [manual regions x" + std::to_string(override_->regions->size()) +
                     "]");
            patched = detail::render_translation(*img, *override_->regions);
            c["regions"] = *override_->regions;
        } else {
            log_info(" [auto]");
            // Already resolved by the caller, which counted the same
            // candidates this branch admits.  Reaching it is what would prove
            // the two had drifted, and it throws rather than letting the run
            // continue.
            const bj::array regions = extract_and_translate(client.get(), *img);
            if (regions.empty()) {
                // Record the empty region list but do NOT rewrite
                // candidates.json here -- the change only lands if a later
                // candidate succeeds, so a run that extracts nothing leaves
                // the resume state untouched.
                log_info("    no regions extracted");
                c["regions"] = bj::array{};
                continue;
            }
            log_info("    " + std::to_string(regions.size()) + " region(s):");
            for (const auto& rv : regions) {
                const bj::object& r = rv.get_object();
                std::string jp, en;
                if (auto* v = r.if_contains("text_jp"))
                    if (v->is_string()) jp = std::string(v->get_string());
                if (auto* v = r.if_contains("text_en"))
                    if (v->is_string()) en = std::string(v->get_string());
                log_info("      JP: " + utf8_prefix(jp, 60));
                log_info("      EN: " + utf8_prefix(en, 80));
            }
            patched = detail::render_translation(*img, regions);
            c["regions"] = regions;
        }

        std::string ext = splitext(img_path).second;
        if (ext.empty()) ext = ".png";
        const std::string out_img = paths.out_dir + "\\" + entry + ext;
        const std::string lower_ext = ascii_lower(ext);
        if (lower_ext == ".png")
            write_file(out_img, encode_png(patched));
        else if (lower_ext == ".bmp")
            write_file(out_img, encode_bmp(drop_alpha(patched)));
        else
            write_file(out_img, encode_jpg(patched, 75));  // quality 75
        c["patched_img"] = out_img;
        log_info("    saved -> " + out_img);

        write_file_text(paths.candidates, json_pretty(bj::value(candidates), 2));
    }
}

void run_repack(const CgOptions& opt, const bj::array& candidates) {
    const Paths paths = make_paths(opt);

    // R5: `by_arc` is iterated with .items(), so archives are repacked in
    // first-candidate-appearance order.  A std::map would silently re-sort.
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>
        by_arc;
    using EntryMap = std::vector<std::pair<std::string, std::string>>;
    const auto arc_slot = [&](const std::string& arc) -> EntryMap& {
        for (auto& kv : by_arc)
            if (kv.first == arc) return kv.second;
        by_arc.emplace_back(arc, std::vector<std::pair<std::string, std::string>>{});
        return by_arc.back().second;
    };

    for (const auto& cv : candidates) {
        const bj::object& c = cv.get_object();
        std::string p;
        if (auto* v = c.if_contains("patched_img"))
            if (v->is_string()) p = std::string(v->get_string());
        // The path the image was actually found at, which is what the replacement
        // is read from: a recorded path from before the project folder moved no
        // longer names a readable file.
        p = detail::resolve_patched_path(p, paths.out_dir);
        if (p.empty()) continue;
        auto& entry_map = arc_slot(std::string(c.at("arc").get_string()));
        const std::string entry(c.at("entry").get_string());
        auto it = std::find_if(entry_map.begin(), entry_map.end(),
                               [&](const auto& kv) { return kv.first == entry; });
        if (it != entry_map.end())
            it->second = p;
        else
            entry_map.emplace_back(entry, p);
    }

    if (by_arc.empty()) {
        log_info("Nothing to repack (no patched images found).");
        return;
    }

    for (const auto& [arc_name, entry_map] : by_arc) {
        const std::string arc_path = paths.dwq_dir + "\\" + arc_name;
        const std::string gtb_path = splitext(arc_path).first + ".gtb";
        const std::string bak_gpk = arc_path + ".backup";
        const std::string bak_gtb = gtb_path + ".backup";
        const std::string tmp_gpk = arc_path + ".tmp";
        const std::string tmp_gtb = gtb_path + ".tmp";

        std::vector<std::pair<std::string, Bytes>> replacements;
        for (const auto& [entry_name, img_path] : entry_map)
            replacements.emplace_back(entry_name, read_file(img_path));

        // Always repack from the backup so repeated runs never compound.
        const std::string src_gpk = path_exists(bak_gpk) ? bak_gpk : arc_path;
        const std::string src_gtb = path_exists(bak_gtb) ? bak_gtb : gtb_path;

        log_info("\nRepacking " + arc_name + " (" + std::to_string(replacements.size()) +
                 " entries)...");
        try {
            gpk::repack(src_gpk, src_gtb, tmp_gpk, tmp_gtb, replacements);
        } catch (const std::exception& ex) {
            log_info(std::string("  ERROR: ") + ex.what());
            std::error_code ec;
            fs::remove(fs::u8path(tmp_gpk), ec);
            fs::remove(fs::u8path(tmp_gtb), ec);
            continue;
        }

        if (!path_exists(bak_gpk)) {
            // shutil.copy2 preserves mtime.
            fs::copy_file(fs::u8path(arc_path), fs::u8path(bak_gpk),
                          fs::copy_options::overwrite_existing);
            fs::last_write_time(fs::u8path(bak_gpk),
                                fs::last_write_time(fs::u8path(arc_path)));
            fs::copy_file(fs::u8path(gtb_path), fs::u8path(bak_gtb),
                          fs::copy_options::overwrite_existing);
            fs::last_write_time(fs::u8path(bak_gtb),
                                fs::last_write_time(fs::u8path(gtb_path)));
            log_info("  backup -> " + bak_gpk);
        }
        fs::rename(fs::u8path(tmp_gpk), fs::u8path(arc_path));
        fs::rename(fs::u8path(tmp_gtb), fs::u8path(gtb_path));
        log_info("  deployed -> " + arc_path);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// detail:: the deterministic helpers
// ---------------------------------------------------------------------------

namespace detail {

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
    if (path_exists(recorded)) return recorded;
    if (patched_dir.empty()) return {};
    const std::string name = fs::u8path(recorded).filename().u8string();
    if (name.empty()) return {};
    const std::string here = patched_dir + "\\" + name;
    return path_exists(here) ? here : std::string();
}

bool needs_translation(const bj::object& candidate, const std::string& patched_dir) {
    const bool has_regions =
        candidate.if_contains("regions") && !candidate.at("regions").is_null();
    std::string patched_path;
    if (auto* p = candidate.if_contains("patched_img"))
        if (p->is_string()) patched_path = std::string(p->get_string());
    return !(has_regions &&
             !resolve_patched_path(patched_path, patched_dir).empty());
}

std::size_t count_left_to_translate(const bj::array& candidates,
                                    const std::string& patched_dir) {
    std::size_t left = 0;
    for (const auto& cv : candidates) {
        const bj::object& c = cv.get_object();
        if (!needs_translation(c, patched_dir)) continue;
        // The pass paints a hand-tuned entry from the override table itself,
        // so those entries cost no request.  The table is empty here, so the
        // test always falls through today.
        const ManualOverride* ov = find_override(std::string(c.at("arc").get_string()),
                                                 std::string(c.at("entry").get_string()));
        if (ov && (ov->fn || ov->regions)) continue;
        ++left;
    }
    return left;
}

std::string safe_name(const std::string& name) {
    // A byte scan is safe here even though names are UTF-8: every member of
    // the invalid set is < 0x80 and UTF-8 continuation bytes are >= 0x80, so
    // no multi-byte character can be hit by accident.
    static const std::string kInvalid = "\"<>|:*?\\/";
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
        out += (c < 0x20 || kInvalid.find(static_cast<char>(c)) != std::string::npos)
                   ? '_'
                   : static_cast<char>(c);
    return out;
}

std::string ext_for_type_tag(const std::string& type_tag) {
    // Substring match, not equality: shipped tags carry padding and suffixes,
    // so a tag like "MYPNGTHING" must still map to .png.
    const std::string t = ascii_lower(type_tag);
    if (t.find("png") != std::string::npos) return ".png";
    if (t.find("bmp") != std::string::npos) return ".bmp";
    if (t.find("jpg") != std::string::npos || t.find("jpeg") != std::string::npos)
        return ".jpg";
    return "";
}

std::optional<std::string> first_json_object_text(const std::string& s) {
    const std::size_t b = s.find('{');
    if (b == std::string::npos) return std::nullopt;
    const std::size_t e = s.find('}', b);
    if (e == std::string::npos) return std::nullopt;
    return s.substr(b, e - b + 1);
}

std::optional<std::string> balanced_json_array_text(const std::string& s) {
    const std::size_t start = s.find('[');
    if (start == std::string::npos) return std::nullopt;
    int depth = 0;
    bool in_str = false, escape = false;
    for (std::size_t i = start; i < s.size(); ++i) {
        const char ch = s[i];
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
            if (--depth == 0) return s.substr(start, i + 1 - start);
        }
    }
    return std::nullopt;
}

int floor_div(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

bool is_truthy(const bj::value& v) {
    switch (v.kind()) {
    case bj::kind::null: return false;
    case bj::kind::bool_: return v.get_bool();
    case bj::kind::int64: return v.get_int64() != 0;
    case bj::kind::uint64: return v.get_uint64() != 0;
    case bj::kind::double_: return v.get_double() != 0.0;
    case bj::kind::string: return !v.get_string().empty();
    case bj::kind::array: return !v.get_array().empty();
    case bj::kind::object: return !v.get_object().empty();
    }
    return false;
}

bool detect_hit(const bj::object& data) {
    const bj::value* has = data.if_contains("has_narrative_text");
    if (!has || !is_truthy(*has)) return false;  // short-circuit: no text, no hit
    const bj::value* conf = data.if_contains("confidence");
    if (!conf) return false;  // missing confidence defaults to 0, which fails
    switch (conf->kind()) {
    case bj::kind::bool_: return (conf->get_bool() ? 1.0 : 0.0) >= 0.7;
    case bj::kind::int64: return static_cast<double>(conf->get_int64()) >= 0.7;
    case bj::kind::uint64: return static_cast<double>(conf->get_uint64()) >= 0.7;
    case bj::kind::double_: return conf->get_double() >= 0.7;
    default:
        // A string/null/array confidence is a malformed reply, not a zero
        // score: throw so the caller logs `    detect error: ...`.
        throw std::runtime_error(
            "'>=' not supported between instances of a non-number and 'float'");
    }
}

int to_int(const bj::value& v) {
    switch (v.kind()) {
    case bj::kind::int64: return static_cast<int>(v.get_int64());
    case bj::kind::uint64: return static_cast<int>(v.get_uint64());
    case bj::kind::bool_: return v.get_bool() ? 1 : 0;
    case bj::kind::double_: return static_cast<int>(std::trunc(v.get_double()));
    case bj::kind::string: {
        const std::string s = trim(std::string(v.get_string()));
        std::size_t idx = 0;
        try {
            const int n = std::stoi(s, &idx, 10);
            if (idx == s.size() && !s.empty()) return n;
        } catch (...) {
        }
        throw std::runtime_error("invalid literal for int(): '" + s + "'");
    }
    default:
        throw std::runtime_error("int() argument must be a string or a number");
    }
}

namespace {

// Base-16 parse of a 0..2 character slice: leading/trailing whitespace is
// tolerated, an empty or non-hex slice throws.
int int_base16(const std::string& slice) {
    std::string s = trim(slice);
    bool neg = false;
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;
    if (i >= s.size()) throw std::runtime_error("invalid literal for int() with base 16");
    int value = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else throw std::runtime_error("invalid literal for int() with base 16");
        value = value * 16 + d;
    }
    return neg ? -value : value;
}

// Half-open [a, b) slice over CODEPOINTS, clamped at both ends so a short
// colour string never throws.
std::string cp_slice(const std::string& s, std::size_t a, std::size_t b) {
    std::vector<std::size_t> starts;
    std::size_t i = 0;
    while (i < s.size()) {
        starts.push_back(i);
        utf8_next(s, i);
    }
    starts.push_back(s.size());
    const std::size_t n = starts.size() - 1;
    a = std::min(a, n);
    b = std::min(b, n);
    if (b <= a) return "";
    return s.substr(starts[a], starts[b] - starts[a]);
}

}  // namespace

std::array<std::uint8_t, 4> parse_hex(const std::string& hex,
                                      const std::string& bg_tone) {
    try {
        // Strip ALL leading '#' characters, not just one.
        std::size_t start = 0;
        while (start < hex.size() && hex[start] == '#') ++start;
        const std::string hx = hex.substr(start);
        // Three independent codepoint slices -- a 5-char hx yields "ff","ff","a"
        // and SUCCEEDS as (255,255,10); a 4-char one yields an empty third slice
        // and throws.  Do NOT pre-check for six characters.
        const int r = int_base16(cp_slice(hx, 0, 2));
        const int g = int_base16(cp_slice(hx, 2, 4));
        const int b = int_base16(cp_slice(hx, 4, 6));
        return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                static_cast<std::uint8_t>(b), 255};
    } catch (...) {
        return bg_tone == "dark" ? std::array<std::uint8_t, 4>{255, 255, 255, 255}
                                 : std::array<std::uint8_t, 4>{30, 30, 30, 255};
    }
}

std::array<std::uint8_t, 3> sample_bg(const Image& img, int x1, int y1, int x2,
                                      int y2) {
    Image c = crop(img, x1, y1, x2, y2);
    if (c.channels == 4) c = drop_alpha(c);

    // Order and duplication matter: the border is sampled as top 2 rows,
    // bottom 2 rows, left 2 columns, right 2 columns, so each of the four
    // corners is counted TWICE.  That double weight is what keeps the median
    // on the frame colour when the bbox clips into the artwork.
    std::vector<std::array<std::uint8_t, 3>> edges;
    const auto push_row = [&](int y) {
        for (int x = 0; x < c.width; ++x)
            edges.push_back({c.at(x, y)[0], c.at(x, y)[1], c.at(x, y)[2]});
    };
    const auto push_col = [&](int x) {
        for (int y = 0; y < c.height; ++y)
            edges.push_back({c.at(x, y)[0], c.at(x, y)[1], c.at(x, y)[2]});
    };
    if (c.height > 4) {
        push_row(0);
        push_row(1);
        push_row(c.height - 2);
        push_row(c.height - 1);
    }
    if (c.width > 4) {
        push_col(0);
        push_col(1);
        push_col(c.width - 2);
        push_col(c.width - 1);
    }
    if (edges.empty()) return {255, 255, 255};

    std::array<std::uint8_t, 3> med{};
    std::vector<std::uint8_t> ch;
    ch.reserve(edges.size());
    for (int k = 0; k < 3; ++k) {
        ch.clear();
        for (const auto& e : edges) ch.push_back(e[static_cast<std::size_t>(k)]);
        std::sort(ch.begin(), ch.end());
        const std::size_t n = ch.size();
        // Median: for an even count average the two middles, then TRUNCATE.
        // nth_element would take the upper middle and bias the inpaint colour.
        const double m = (n % 2) ? static_cast<double>(ch[n / 2])
                                 : (static_cast<double>(ch[n / 2 - 1]) +
                                    static_cast<double>(ch[n / 2])) / 2.0;
        med[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(static_cast<int>(m));
    }
    return med;
}

std::unique_ptr<Font> fit_font(const std::string& text, const std::string& font_path,
                               int max_w, int max_h) {
    for (int sz = 36; sz >= 8; --sz) {  // largest that fits wins; 8px is the floor
        auto f = Font::load(font_path, sz);
        // A missing face must not silently drop the region: retry the same
        // size against arial and skip the size only if that fails too.  Both
        // fonts exist on this box, so it never fires in practice.
        if (!f) f = Font::load(FONT_FALLBACK, sz);
        if (!f) continue;
        const TextBBox bb = f->measure(text);
        if (bb.width() <= max_w && bb.height() <= max_h) return f;
    }
    return Font::load(FONT_FALLBACK, 8);
}

std::vector<std::string> wrap_lines(const std::string& text, const Font& font,
                                    int max_w) {
    const std::vector<std::string> words = split_whitespace(text);
    std::vector<std::string> lines;
    std::string cur;
    for (const auto& word : words) {
        const std::string test = trim(cur + " " + word);
        if (font.measure(test).width() > max_w && !cur.empty()) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = test;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

Image render_translation(const Image& img, const bj::array& regions) {
    Image result = img;
    const int w = result.width, h = result.height;
    const std::string default_font =
        path_exists(FONT_PATH) ? std::string(FONT_PATH) : std::string(FONT_FALLBACK);

    for (const auto& rv : regions) {
        // Regions are always objects in practice; a stray scalar is skipped
        // rather than allowed to abort the whole image.
        if (!rv.is_object()) continue;
        const bj::object& r = rv.get_object();

        const auto gets = [&](const char* k, const std::string& dflt) {
            if (auto* v = r.if_contains(k))
                if (v->is_string()) return std::string(v->get_string());
            return dflt;
        };
        const auto geti = [&](const char* k) -> std::optional<int> {
            if (auto* v = r.if_contains(k)) {
                if (v->is_int64()) return static_cast<int>(v->get_int64());
                if (v->is_uint64()) return static_cast<int>(v->get_uint64());
                if (v->is_double()) return static_cast<int>(v->get_double());
            }
            return std::nullopt;
        };

        const std::string text_en = gets("text_en", "");
        const std::string bg_tone = gets("bg_tone", "dark");
        const std::string txt_hex =
            gets("text_color", bg_tone == "dark" ? "#ffffff" : "#1a1a1a");
        const std::string font_path = gets("font_path", default_font);
        const std::optional<int> font_size = geti("font_size");
        const bool font_size_set = font_size && *font_size != 0;  // 0 means "auto-fit"
        const std::string align = gets("align", "center");
        const std::optional<int> anchor_x = geti("x");
        const std::optional<int> anchor_y = geti("y");

        if (text_en.empty()) continue;

        std::vector<int> bbox;
        if (auto* v = r.if_contains("bbox"))
            if (v->is_array())
                for (const auto& e : v->get_array()) bbox.push_back(to_int(e));

        int x1, y1, x2, y2, bw, bh;
        if (bbox.size() >= 4) {
            // A bbox is exactly four values.  A longer list means the model
            // returned something we do not understand -- fail loudly instead
            // of rendering over a guessed rectangle.
            if (bbox.size() != 4)
                throw std::runtime_error("too many values to unpack (expected 4)");
            x1 = std::max(0, bbox[0]);
            y1 = std::max(0, bbox[1]);
            x2 = std::max(0, bbox[2]);
            y2 = std::max(0, bbox[3]);
            // max(0, ..) applies to all four FIRST; min(w/h, ..) only to x2/y2.
            x2 = std::min(w, x2);
            y2 = std::min(h, y2);
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

        if (bbox.size() >= 4) {
            const auto bg = sample_bg(result, x1, y1, x2, y2);
            fill_rect(result, x1, y1, x2, y2, {bg[0], bg[1], bg[2], 255});
        }

        const auto tc = parse_hex(txt_hex, bg_tone);

        std::unique_ptr<Font> font;
        if (font_size_set) {
            font = Font::load(font_path, *font_size);
            if (!font) font = Font::load(FONT_FALLBACK, *font_size);
        } else {
            std::string flat = text_en;
            std::replace(flat.begin(), flat.end(), '\n', ' ');
            font = fit_font(flat, font_path, bw - 8, bh - 4);
        }
        if (!font) continue;

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
        if (lines.size() == 1 && !font_size_set)
            lines = wrap_lines(text_en, *font, bw - 8);

        const int line_h = font->measure("Ay").y1 + 4;
        const int total_h = line_h * static_cast<int>(lines.size());
        int ty = anchor_y ? *anchor_y : y1 + std::max(4, floor_div(bh - total_h, 2));

        for (const auto& line : lines) {
            const int lw = font->measure(line).width();
            int tx;
            if (anchor_x) tx = *anchor_x;
            else if (align == "left") tx = x1 + 6;
            else if (align == "right") tx = x2 - lw - 6;
            else tx = x1 + floor_div(bw - lw, 2);
            font->draw(result, tx, ty, line, tc);
            ty += line_h;
        }
    }

    return result;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int run_find_narrative_cg(const CgOptions& opt) {
    const Paths paths = make_paths(opt);
    fs::create_directories(fs::u8path(paths.output_dir));  // module-level makedirs

    if (!opt.replay_file.empty()) {
        const Bytes raw = read_file(opt.replay_file);
        std::string cur;
        for (std::uint8_t b : raw) {
            if (b == '\n') {
                if (!trim(cur).empty()) g_replay.push_back(cur);
                cur.clear();
            } else if (b != '\r') {
                cur += static_cast<char>(b);
            }
        }
        if (!trim(cur).empty()) g_replay.push_back(cur);
        g_replay_active = true;
    }

    // _load_key() runs on both paths; only the non-repack path requires a key.
    anthropic::load_api_key();

    if (opt.repack_only) {
        if (!path_exists(paths.candidates)) {
            log_info("ERROR: " + paths.candidates + " not found. Run scan first.");
            return 1;
        }
        const bj::array candidates = json_parse_file(paths.candidates).get_array();
        run_repack(opt, candidates);
    } else {
        // Phase 1: extract everything locally -- no Claude calls yet.
        extract_all_assets(opt);
        // Phase 2: explicit user confirmation before any Claude API call.
        if (!confirm_send_to_claude()) {
            log_info("Aborted by user. No Claude API calls made.");
            return 0;  // note: no "\nDone."
        }
        // Phase 3: scan + translate + repack.  Nothing below builds a client
        // until it has a request to make, so --repack, an abort at the prompt
        // and a scan the resume file already covers all run without a key.
        anthropic::LazyClient client;
        bj::array candidates = run_scan(opt, client, /*resume=*/!opt.no_resume);
        if (!opt.scan_only && !candidates.empty()) {
            // The pass sends a request only for a candidate it has not already
            // painted and no override table entry covers, so a candidates file
            // whose entries all carry a patched image leaves it nothing to
            // send.  Resolved here rather than inside the pass, so a run that
            // cannot proceed stops before it opens an image or rewrites the
            // candidates file.
            if (detail::count_left_to_translate(candidates, paths.out_dir)) client.get();
            run_translate(opt, client, candidates);
            run_repack(opt, candidates);
        }
    }

    log_info("\nDone.");
    return 0;
}

}  // namespace mgi::cg
