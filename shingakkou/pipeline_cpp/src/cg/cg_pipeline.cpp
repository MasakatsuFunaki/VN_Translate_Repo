// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cg_pipeline.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
#include <thread>
#include <utility>

#include "common/util.h"
#include "ddp/ddp_archive.h"
#include "translate/anthropic_client.h"

namespace shin::cg {

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

// sin_cgev.dat: event CGs (800x600) -- most likely to carry narrative text.
// sin_sysd.dat: system/title images -- may include title cards.
const std::vector<std::string> SCAN_ARCS = {"sin_cgev.dat", "sin_sysd.dat"};

// Low enough to catch menu panels and large UI labels, not just full-screen
// narrative CGs; sprite-sized icons are still filtered out.
constexpr int MIN_WIDTH = 180;
constexpr int MIN_HEIGHT = 60;

const char* const MODEL = "claude-sonnet-4-6";

// extract_ddp.exe gets 10 minutes: unpacking sin_cgev.dat is ~800 MB of IO.
constexpr unsigned kExtractTimeoutMs = 600 * 1000;

// ---------------------------------------------------------------------------
// Prompts.  Where a sentence is split across source lines the fragments are
// joined with NO space inserted -- mind the line breaks when editing.
// ---------------------------------------------------------------------------

const char* const DETECT_PROMPT =
    R"__(You are analyzing a visual novel image to determine if it contains Japanese text that the player would benefit from seeing in English.

Translate-worthy text includes:
  - Narrative content: poems, literary quotes, prose passages overlaid on artwork
  - Chapter or scene title cards with story-relevant Japanese text
  - Menu / UI labels: file, open, close, save, load, settings, options, exit, yes/no, back, skip, auto, log/backlog, config, quit, etc.
  - Button labels and dialog box headers

Do NOT classify as translate-worthy: staff credits, legal disclaimers / copyright notices, or ambient signage painted into background scenery (shop signs, posters, graffiti) — these are decorative environmental detail, not interface or story content.

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

// str.splitlines(): splits on \n, drops a trailing empty piece, tolerates \r\n.
std::vector<std::string> splitlines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------------------
// Child process with captured stdout/stderr (subprocess.run(capture_output=True))
// ---------------------------------------------------------------------------

struct ProcResult {
    int rc = 0;
    std::string out;
    std::string err;
};

ProcResult run_capture(const std::vector<std::string>& argv, unsigned timeout_ms);

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

struct Paths {
    std::string game_dir;  // the archives sit directly in it, not in a subfolder
    std::string output_dir;
    std::string candidates;
    std::string scanned;
    std::string out_dir;
    std::string extract_dir;
};

Paths make_paths(const CgOptions& opt) {
    Paths p;
    // This step works out of <install>\data, but --game-dir is the install root
    // everywhere else (01_extract appends \Data itself), so append it here
    // rather than giving this one app a different --game-dir convention.
    p.game_dir = opt.game_dir + "\\data";
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

// Insertion-ordered with upsert semantics: a duplicate entry name overwrites in
// place rather than appending a second key, which would change both
// _index.json's key order and the entry counts derived from it.
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

// Default location of the GARbro extractor:
// <project>/../TOOLS/garbro/extract_ddp.exe.
std::string default_extract_ddp(const CgOptions& opt) {
    return fs::absolute(fs::u8path(opt.project_dir + "\\..\\TOOLS\\garbro\\extract_ddp.exe"))
        .u8string();
}

ArchiveIndex extract_archive(const CgOptions& opt, const std::string& arc_name,
                             bool force = false) {
    const Paths paths = make_paths(opt);
    const std::string arc_path = paths.game_dir + "\\" + arc_name;
    const std::string arc_stem = splitext(arc_name).first;
    const std::string out_dir = paths.extract_dir + "\\" + arc_stem;
    const std::string index_path = out_dir + "\\_index.json";

    ArchiveIndex index;

    // Disk cache: a rerun after a crash never re-runs the extractor.
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
    const std::string ext =
        opt.extract_ddp.empty() ? default_extract_ddp(opt) : opt.extract_ddp;
    const ProcResult r = run_capture({ext, arc_path, out_dir}, kExtractTimeoutMs);

    // "<name>\t<width>\t<height>" per extracted image; a name whose BMP is
    // missing (GARbro skipped a non-image entry) is dropped.
    for (const auto& line : splitlines(r.out)) {
        detail::ExtractLine parsed;
        if (!detail::parse_extract_line(line, &parsed)) continue;
        const std::string bmp_path =
            out_dir + "\\" + detail::safe_name(parsed.name) + ".bmp";
        if (path_exists(bmp_path))
            index.upsert(parsed.name, {parsed.width, parsed.height, bmp_path});
    }

    bj::object obj;
    for (const auto& [name, info] : index)
        obj[name] = bj::array{info.width, info.height, info.path};
    write_file_text(index_path, json_pretty(bj::value(std::move(obj)), 2));

    // Report the LAST line of stderr, or "done" when it trims to nothing.
    const std::vector<std::string> err_lines = splitlines(trim(r.err));
    log_info("  extract_ddp: " + (err_lines.empty() ? std::string("done") : err_lines.back()));
    return index;
}

// ---------------------------------------------------------------------------
// Child process implementation
// ---------------------------------------------------------------------------

namespace {

// Quote one argument for CreateProcess, which takes a single flat command line:
// wrap in quotes when it contains a space or a quote, escaping quotes and the
// backslashes that precede them.
std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    return out + "\"";
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A read end plus the thread draining it -- a single-threaded read of two pipes
// deadlocks as soon as the child fills the other one.
struct PipeReader {
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    std::string data;
    std::thread thread;

    bool open() {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&read, &write, &sa, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        return true;
    }
    void start() {
        thread = std::thread([this] {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n) data.append(buf, n);
        });
    }
    void finish() {
        if (thread.joinable()) thread.join();
        if (read) CloseHandle(read);
    }
};

}  // namespace

ProcResult run_capture(const std::vector<std::string>& argv, unsigned timeout_ms) {
    std::string cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline += ' ';
        cmdline += quote_arg(argv[i]);
    }

    PipeReader out, err;
    if (!out.open() || !err.open()) throw std::runtime_error("cannot create pipe");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out.write;
    si.hStdError = err.write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring wcmd = widen(cmdline);
    wcmd.push_back(L'\0');
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(out.read);
        CloseHandle(out.write);
        CloseHandle(err.read);
        CloseHandle(err.write);
        throw std::runtime_error("cannot run " + argv.front());
    }
    // The parent's copies must go, or the reads never see EOF.
    CloseHandle(out.write);
    CloseHandle(err.write);
    out.start();
    err.start();

    ProcResult r;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        out.finish();
        err.finish();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        throw std::runtime_error("timed out: " + argv.front());
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    out.finish();
    err.finish();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    r.rc = static_cast<int>(code);
    r.out = std::move(out.data);
    r.err = std::move(err.data);
    return r;
}

// ---------------------------------------------------------------------------
// OCR pre-filter
// ---------------------------------------------------------------------------

// There is no OCR pre-filter: no backend is wired up, so ocr_vote() votes yes
// unconditionally and every image above the size threshold reaches Claude.  The
// summary line is still logged once, so a run says plainly that nothing was
// filtered -- otherwise the vision-call count looks inexplicable.
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

// --replay transcript, consumed in call order.  The seam sits below the
// "[claude ...] calling api..." line, so a replayed run logs exactly like a
// live one.
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

    // open_bmp() converted to RGB, so nothing here is ever RGBA -- but a
    // hand-built region image could be, and PNG carries RGBA perfectly well.
    // Encode whatever we have.
    const Image& upload = img;

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
            if (!text) continue;  // `if not m: continue`
            // A parse failure throws and is caught below and logged -- it is
            // NOT a silent skip.
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
        if (!text) return {};  // `if regions is None: return []`
        bj::array regions = bj::parse(*text).get_array();
        if (scale < 1.0) {
            // A missing or non-numeric bbox raises INSIDE the try, so a single
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
        if (!path_exists(paths.game_dir + "\\" + arc_name)) {
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

void save_scanned(const Paths& paths,
                  const std::set<std::pair<std::string, std::string>>& done) {
    // Sorted by (archive, entry): std::set<std::pair<...>> already gives that,
    // and UTF-8 preserves codepoint ordering, so the file is stable run to run.
    // NOTE: no indent here -- narrative_scanned.json is dumped compact in this
    // game (unlike narrative_candidates.json, and unlike mushigurui's).
    bj::array arr;
    for (const auto& [arc, entry] : done) arr.push_back(bj::array{arc, entry});
    write_file_text(paths.scanned, json_dump(bj::value(std::move(arr))));
}

bj::array run_scan(const CgOptions& opt, anthropic::Client& client, bool resume) {
    const Paths paths = make_paths(opt);
    bj::array candidates;
    std::set<std::pair<std::string, std::string>> done_keys;

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

    for (const auto& arc_name : SCAN_ARCS) {
        if (!path_exists(paths.game_dir + "\\" + arc_name)) {
            log_info("[skip] " + arc_name + " — not found");
            continue;
        }

        log_info("\n=== " + arc_name + " — extracting... ===");
        const ArchiveIndex index = extract_archive(opt, arc_name);

        std::vector<std::pair<std::string, IndexEntry>> entries;
        for (const auto& [name, info] : index)
            if (info.width >= MIN_WIDTH && info.height >= MIN_HEIGHT)
                entries.emplace_back(name, info);
        log_info("  " + std::to_string(index.size()) + " images total, " +
                 std::to_string(entries.size()) + " meet size threshold");

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const std::string& name = entries[i].first;
            const IndexEntry& info = entries[i].second;
            if (done_keys.count({arc_name, name})) continue;  // i still advances

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
                const DetectResult det = detect_narrative(client, *img);
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
                    c["bmp_path"] = info.path;
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

void run_translate(const CgOptions& opt, anthropic::Client& client,
                   bj::array& candidates) {
    const Paths paths = make_paths(opt);
    fs::create_directories(fs::u8path(paths.out_dir));

    for (auto& cv : candidates) {
        bj::object& c = cv.get_object();
        const std::string arc(c.at("arc").get_string());
        const std::string entry(c.at("entry").get_string());

        const bool has_regions = c.if_contains("regions") && !c.at("regions").is_null();
        std::string patched_path;
        if (auto* p = c.if_contains("patched_bmp"))
            if (p->is_string()) patched_path = std::string(p->get_string());
        if (has_regions && !patched_path.empty() && path_exists(patched_path)) {
            log_info("  " + entry + ": already translated, skip");
            continue;
        }

        print_inline("\n  " + arc + " / " + entry + "...");

        // Cached BMP first, then re-extract.
        std::string img_path;
        if (auto* p = c.if_contains("bmp_path"))
            if (p->is_string()) img_path = std::string(p->get_string());
        if (img_path.empty() || !path_exists(img_path)) {
            const ArchiveIndex index = extract_archive(opt, arc);
            if (const IndexEntry* info = index.find(entry)) {
                img_path = info->path;
                c["bmp_path"] = img_path;
            }
        }
        if (img_path.empty() || !path_exists(img_path)) {
            log_info(" bmp not found");
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
            const bj::array regions = extract_and_translate(client, *img);
            if (regions.empty()) {
                // Record the empty region list but skip the rewrite of
                // candidates.json -- the change only lands if a later candidate
                // succeeds.
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

        // Always BMP: extract_ddp.exe writes BMPs, and the repack substitutes
        // the patched image straight back into the archive.
        const std::string out_bmp = paths.out_dir + "\\" + entry + ".bmp";
        write_file(out_bmp, encode_bmp(patched.channels == 4 ? drop_alpha(patched) : patched));
        c["patched_bmp"] = out_bmp;
        log_info("    saved -> " + out_bmp);

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
        if (auto* v = c.if_contains("patched_bmp"))
            if (v->is_string()) p = std::string(v->get_string());
        if (p.empty() || !path_exists(p)) continue;
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
        log_info("Nothing to repack (no patched_bmp paths found).");
        return;
    }

    for (const auto& [arc_name, entry_map] : by_arc) {
        const std::string arc_path = paths.game_dir + "\\" + arc_name;
        const std::string bak_path = arc_path + ".backup";
        const std::string tmp_path = arc_path + ".tmp";

        ddp::Replacements replacements;
        for (const auto& [entry_name, img_path] : entry_map)
            replacements.emplace_back(entry_name, read_file(img_path));

        log_info("\nRepacking " + arc_name + " (" + std::to_string(replacements.size()) +
                 " entries)...");
        try {
            // Note: unlike mushigurui's GPK step this repacks from the LIVE
            // archive, not from the backup -- so a second run with different
            // replacements compounds onto the first.
            ddp::repack(arc_path, tmp_path, replacements);
        } catch (const std::exception& ex) {
            log_info(std::string("  ERROR: ") + ex.what());
            std::error_code ec;
            fs::remove(fs::u8path(tmp_path), ec);
            continue;
        }

        if (!path_exists(bak_path)) {
            // shutil.copy2 preserves mtime.
            fs::copy_file(fs::u8path(arc_path), fs::u8path(bak_path),
                          fs::copy_options::overwrite_existing);
            fs::last_write_time(fs::u8path(bak_path),
                                fs::last_write_time(fs::u8path(arc_path)));
            log_info("  backup -> " + bak_path);
        }
        fs::rename(fs::u8path(tmp_path), fs::u8path(arc_path));
        log_info("  deployed -> " + arc_path);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// detail:: the deterministic helpers
// ---------------------------------------------------------------------------

namespace detail {

std::string safe_name(const std::string& name) {
    // A byte scan is safe on UTF-8 here: every member of the invalid set is
    // < 0x80 and continuation bytes are >= 0x80, so no multi-byte character can
    // be clipped in half.
    static const std::string kInvalid = "\"<>|:*?\\/";
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
        out += (c < 0x20 || kInvalid.find(static_cast<char>(c)) != std::string::npos)
                   ? '_'
                   : static_cast<char>(c);
    return out;
}

bool parse_extract_line(const std::string& line, ExtractLine* out) {
    // Split on tabs -- exactly three fields, and an empty field is a field.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    if (parts.size() != 3) return false;

    out->name = parts[0];
    // A non-numeric size throws and is deliberately NOT caught: the extractor
    // is malformed and the run should stop, not silently lose images.
    out->width = to_int(bj::value(parts[1]));
    out->height = to_int(bj::value(parts[2]));
    return true;
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
    if (!has || !is_truthy(*has)) return false;  // `and` short-circuits
    const bj::value* conf = data.if_contains("confidence");
    if (!conf) return false;  // .get('confidence', 0) -> 0 >= 0.7 is False
    switch (conf->kind()) {
    case bj::kind::bool_: return (conf->get_bool() ? 1.0 : 0.0) >= 0.7;
    case bj::kind::int64: return static_cast<double>(conf->get_int64()) >= 0.7;
    case bj::kind::uint64: return static_cast<double>(conf->get_uint64()) >= 0.7;
    case bj::kind::double_: return conf->get_double() >= 0.7;
    default:
        // A string/null/list confidence is a malformed response, not a low
        // score; the caller catches this and logs `    detect error: ...`.
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

// Slice [a,b) over CODEPOINTS, clamped to the string -- never throws.
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
        // .lstrip('#') strips ALL leading '#' characters, not just one.
        std::size_t start = 0;
        while (start < hex.size() && hex[start] == '#') ++start;
        const std::string hx = hex.substr(start);
        // Three independent codepoint slices -- a 5-char hx yields "ff","ff","a"
        // and SUCCEEDS as (255,255,10); a 4-char one yields an empty third slice
        // and raises.  Do NOT pre-check for six characters.
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

    // Order and duplication matter: the top/bottom two rows and the left/right
    // two columns are each gathered whole, so the four corners are counted
    // twice and weigh double in the median below.  That is intended.
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
        // Median: average the two middle samples on an even count, then
        // TRUNCATE.  nth_element would take the upper middle instead, which
        // shifts the sampled background by a level on flat gradients.
        const double m = (n % 2) ? static_cast<double>(ch[n / 2])
                                 : (static_cast<double>(ch[n / 2 - 1]) +
                                    static_cast<double>(ch[n / 2])) / 2.0;
        med[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(static_cast<int>(m));
    }
    return med;
}

std::unique_ptr<Font> fit_font(const std::string& text, const std::string& font_path,
                               int max_w, int max_h) {
    for (int sz = 36; sz >= 8; --sz) {  // largest size that fits wins
        auto f = Font::load(font_path, sz);
        // If the preferred face will not load, retry the SAME size against
        // arial and skip the size only if that fails too -- never silently
        // shrink the text because a font was missing.  Both fonts exist on this
        // box, so it never fires in practice.
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
        // Nothing in this corpus produces a non-object region.
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
        const bool font_size_set = font_size && *font_size != 0;  // `if font_size:`
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
            // A bbox is exactly four values; a longer list is a malformed
            // response and throws uncaught rather than being truncated to the
            // first four and rendered somewhere plausible-looking.
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
        const char* key = std::getenv("ANTHROPIC_API_KEY");
        if (!key || !*key) {
            log_info("ERROR: ANTHROPIC_API_KEY not set.");
            return 1;
        }

        // Phase 1: extract everything locally -- no Claude calls yet.
        extract_all_assets(opt);
        // Phase 2: explicit user confirmation before any Claude API call.
        if (!confirm_send_to_claude()) {
            log_info("Aborted by user. No Claude API calls made.");
            return 0;  // note: no "\nDone."
        }
        // Phase 3: scan + translate + repack.  The client is built here so the
        // --repack path never needs a key at all.
        anthropic::Client client;
        bj::array candidates = run_scan(opt, client, /*resume=*/!opt.no_resume);
        if (!opt.scan_only && !candidates.empty()) {
            run_translate(opt, client, candidates);
            run_repack(opt, candidates);
        }
    }

    log_info("\nDone.");
    return 0;
}

}  // namespace shin::cg
