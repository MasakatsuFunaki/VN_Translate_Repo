// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extractor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "artemis/pfs_archive.h"

namespace snt::extract {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

// These patterns all contain the `)"` digraph, so they need a custom raw
// string delimiter -- the default R"(...)" would terminate mid-pattern.
//
// Bare quoted string on its own line:  `  "……some japanese……",  `
const boost::regex RE_BARE_STR(R"RE(^\s*"((?:[^"\\]|\\.)*)"[,\s]*$)RE");
// Speaker plate:  `  name = {"foo"},  ` or `  name = {"foo", "bar"},  `.
// The brace list is captured whole and split by RE_PLATE_ELEMENT, so a plate
// with any number of elements is recognised instead of silently skipped.
const boost::regex RE_NAME(R"RE(^\s*name\s*=\s*\{(.*)\}[,\s]*$)RE");
// One quoted element of that brace list.
const boost::regex RE_PLATE_ELEMENT(R"RE("((?:[^"\\]|\\.)*)")RE");
// savetitle:  `{"savetitle", text="第I章"},`
const boost::regex RE_SAVETITLE(
    R"RE(\{"savetitle"\s*,\s*text\s*=\s*"((?:[^"\\]|\\.)*)"\s*\})RE");
// Engine command like `{"bg", file="..."}` -- even when it carries JP, those
// are asset paths.  Only tested on lines that already failed NAME/SAVETITLE.
const boost::regex RE_ENGINE_CMD(R"RE(^\s*\{"[a-zA-Z_]\w*")RE");
// `{"ruby", text="つぶらぎ"},` opens a reading and `{"/ruby"},` closes it.
// The kanji the reading annotates is a bare string between the two, so it
// reaches the corpus as an entry of its own in the middle of a sentence.
const boost::regex RE_RUBY_OPEN(R"RE(^\s*\{"ruby"\s*,)RE");
const boost::regex RE_RUBY_CLOSE(R"RE(^\s*\{"/ruby"\s*\})RE");
// `ja = {` opens a localisation block (never opens mid-line in this format).
const boost::regex RE_JA_OPEN(R"RE(\bja\s*=\s*\{)RE");
// A line that is nothing but a closing brace.
const boost::regex RE_CLOSE_ONLY(R"RE(^\s*\},?\s*$)RE");

// The quoted elements of a name plate's brace list, in source order.
std::vector<std::string> plate_elements(const std::string& brace_list) {
    std::vector<std::string> parts;
    for (boost::sregex_iterator it(brace_list.begin(), brace_list.end(), RE_PLATE_ELEMENT),
         end;
         it != end; ++it)
        parts.push_back((*it)[1].str());
    return parts;
}

// Bare string -- dialogue if wrapped in 「」/『』, else narrative.
std::string classify_bare(const std::string& text) {
    const std::string s = trim(text);
    static const char* OPEN1 = "\xE3\x80\x8C";   // 「
    static const char* OPEN2 = "\xE3\x80\x8E";   // 『
    static const char* CLOSE1 = "\xE3\x80\x8D";  // 」
    static const char* CLOSE2 = "\xE3\x80\x8F";  // 』
    auto starts = [&](const char* p) { return s.rfind(p, 0) == 0; };
    auto ends = [&](const char* p) {
        const std::size_t n = std::strlen(p);
        return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
    };
    if (starts(OPEN1) || starts(OPEN2) || ends(CLOSE1) || ends(CLOSE2))
        return "dialogue";
    return "narrative";
}

// The speaker a script has in force while the walker moves down it.
//
// Artemis names a speaker once, on a plate of its own, and lets every line
// after it inherit that name, so attribution is state rather than a field on
// the line.  A chapter title ends the scene the state belongs to.
class SpeakerTracker {
public:
    // The plate in force now, empty while the scene has named nobody.
    const std::string& standing() const { return standing_; }

    // The voice of the entry the walker wrote last, empty when that entry was
    // narration.  A string split out of the middle of a sentence -- a ruby
    // base -- belongs to that voice and not to the narration around it.
    const std::string& carried_voice() const { return voice_; }

    // Take one written entry into account, in script order.
    void observe(const StringEntry& e) {
        if (e.type == "title") {
            standing_.clear();
            voice_.clear();
        } else if (e.type == "name") {
            standing_ = trim(e.text);
            voice_ = standing_;
        } else {
            voice_ = e.speaker ? *e.speaker : std::string();
        }
    }

private:
    std::string standing_;
    std::string voice_;
};

}  // namespace

std::optional<std::vector<StringEntry>> extract_one(const Bytes& plaintext) {
    static const char* MAGIC = "astver";
    if (plaintext.size() < 6 || std::memcmp(plaintext.data(), MAGIC, 6) != 0)
        return std::nullopt;  // not an Artemis ast file

    // The payload is UTF-8 Lua source, so the bytes pass through untouched;
    // any stray non-UTF-8 byte survives into the captured string and simply
    // fails the JP filter below.
    const std::string text(reinterpret_cast<const char*>(plaintext.data()),
                           plaintext.size());

    std::vector<StringEntry> out;
    int in_ja = 0;    // depth of ja = { ... } block (nested)
    int in_ruby = 0;  // depth of {"ruby"} ... {"/ruby"} (never nested here)
    SpeakerTracker speakers;

    std::size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string raw =
            text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++lineno;

        if (boost::regex_search(raw, RE_JA_OPEN)) ++in_ja;
        // Closing brace: for simplicity drop on any `},` line while inside ja.
        if (in_ja > 0 && boost::regex_match(raw, RE_CLOSE_ONLY)) --in_ja;

        boost::smatch m;
        // The patterns below are mutually exclusive on any one line, so a
        // line can never be captured twice and no dedupe guard is needed.
        if (boost::regex_match(raw, m, RE_NAME)) {
            // A plate is kept whether or not it holds a Japanese character:
            // `name = {"？？？"}` is how the engine says "not yet named", and
            // dropping it leaves the previous speaker standing over lines
            // that nobody in the scene has been introduced for.
            const std::vector<std::string> parts = plate_elements(m[1].str());
            if (!parts.empty() && !trim(parts[0]).empty()) {
                StringEntry e{lineno, parts[0], "name"};
                if (parts.size() > 1 && parts[1] != parts[0]) e.display = parts[1];
                speakers.observe(e);
                out.push_back(std::move(e));
            }
            continue;
        }
        if (boost::regex_search(raw, m, RE_SAVETITLE)) {
            const std::string txt = m[1].str();
            if (is_jp(txt)) {
                StringEntry e{lineno, txt, "title"};
                speakers.observe(e);
                out.push_back(std::move(e));
            }
            continue;
        }
        if (boost::regex_search(raw, RE_RUBY_OPEN)) {
            ++in_ruby;
            continue;
        }
        if (boost::regex_search(raw, RE_RUBY_CLOSE)) {
            if (in_ruby > 0) --in_ruby;
            continue;
        }
        if (boost::regex_search(raw, RE_ENGINE_CMD)) continue;
        if (boost::regex_match(raw, m, RE_BARE_STR)) {
            const std::string txt = m[1].str();
            // Lua escapes are uncommon in this format but safe to leave intact.
            if (is_jp(txt) && in_ja > 0) {
                StringEntry e{lineno, txt, classify_bare(txt)};
                if (e.type == "dialogue") {
                    if (!speakers.standing().empty()) e.speaker = speakers.standing();
                } else if (in_ruby > 0 && !speakers.carried_voice().empty()) {
                    // A ruby base is the middle of the sentence before it, so
                    // it keeps that sentence's voice.  Narration around it
                    // stays narration: the fragment can only repeat a speaker
                    // an adjacent entry already carries, never invent one.
                    e.speaker = speakers.carried_voice();
                }
                speakers.observe(e);
                out.push_back(std::move(e));
            }
        }
    }

    return out;
}

int run_extract(const std::string& game_dir, const std::string& output_file) {
    log_info("GAME_DIR: " + game_dir);
    pfs::Overlay ov = pfs::build_overlay(game_dir);
    log_info("Overlay: " + std::to_string(ov.entries.size()) + " unique entries from " +
             std::to_string(ov.keys.size()) + " archives");

    // Filter to .ast scripts only (translation targets).  Overlay::entries is
    // an ordered map, so the result is already sorted by name.
    std::vector<std::string> ast_names;
    for (const auto& kv : ov.entries)
        if (kv.first.size() >= 4 && kv.first.compare(kv.first.size() - 4, 4, ".ast") == 0)
            ast_names.push_back(kv.first);
    log_info(".ast scripts: " + std::to_string(ast_names.size()));

    bj::object out;
    long long total_strings = 0, skipped_nonast = 0, skipped_empty = 0;

    for (const auto& name : ast_names) {
        const auto& [arc_path, idx] = ov.entries.at(name);
        Bytes data;
        try {
            data = pfs::read_entry(arc_path, idx, ov.keys.at(arc_path));
        } catch (const std::exception& e) {
            log_info("  [error] " + name + ": " + e.what());
            continue;
        }

        auto entries = extract_one(data);
        if (!entries) {
            ++skipped_nonast;
            continue;
        }
        if (entries->empty()) {
            ++skipped_empty;
            continue;
        }

        total_strings += static_cast<long long>(entries->size());

        bj::array strings;
        strings.reserve(entries->size());
        for (const auto& e : *entries) {
            bj::object o;
            o["offset"] = e.offset;
            o["text"] = e.text;
            o["has_jp"] = is_jp(e.text);
            o["type"] = e.type;
            // Written on every spoken and narrated entry, null included: a
            // reader that has to tell "no speaker" from "no field" cannot,
            // and a missing key would send the line back to a state machine.
            if (e.type == "dialogue" || e.type == "narrative") {
                if (e.speaker) o["speaker"] = *e.speaker;
                else o["speaker"] = nullptr;
            }
            if (!e.display.empty()) o["display"] = e.display;
            strings.push_back(std::move(o));
        }

        bj::object script;
        script["file"] = name;
        script["arc"] = fs::u8path(arc_path).filename().u8string();
        script["file_size"] = static_cast<std::int64_t>(data.size());
        script["jp_strings"] = static_cast<std::int64_t>(entries->size());
        script["strings"] = std::move(strings);
        out[name] = std::move(script);

        char buf[160];
        std::snprintf(buf, sizeof(buf), "  %-40s %5zu JP strings", name.c_str(),
                      entries->size());
        log_info(buf);
    }

    // Capture the count before the move -- serialising `out` leaves it empty.
    const std::size_t script_count = out.size();
    write_file(output_file, json_dump(bj::value(std::move(out))));

    print_line("");
    log_info("Scripts with JP text: " + std::to_string(script_count));
    log_info("Skipped (non-ast magic): " + std::to_string(skipped_nonast));
    log_info("Skipped (empty):         " + std::to_string(skipped_empty));
    log_info("Total JP strings:        " + std::to_string(total_strings));
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(output_file))) / 1024.0 /
                      1024.0);
    log_info("Wrote " + output_file + " (" + sz + ")");
    return 0;
}

}  // namespace snt::extract
