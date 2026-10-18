// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "extractor.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>

#include "bgi/bgi_archive.h"

namespace snu::extract {

namespace fs = std::filesystem;
namespace bj = boost::json;

const std::vector<std::string> SCRIPT_ARCS = {
    "data01099.arc", "data01100.arc", "data01101.arc", "data01102.arc",
    "data01103.arc", "data01104.arc", "data01105.arc", "data01106.arc",
    "data01107.arc", "data01108.arc", "data01109.arc", "data01999.arc",
};

namespace {

// BGI v1.00 bytecode parser constants (kingshriek's Bgi_script_tools).
constexpr std::size_t HDR_SIZE = 0x1C;
constexpr std::size_t HDRAS_POS = 0x1C;
constexpr std::uint32_t STR_TYPE = 0x03;
constexpr std::uint32_t TEXT_FCN = 0x140;
constexpr std::uint32_t BKLG_FCN = 0x143;
constexpr std::uint32_t RUBY_FCN = 0x14B;
constexpr std::int64_t NAME_POS = 0x0C;
constexpr std::int64_t TEXT_POS = 0x04;
constexpr std::int64_t RUBYK_POS = 0x04;
constexpr std::int64_t RUBYF_POS = 0x0C;
constexpr std::int64_t BKLG_POS = 0x0C;
const Bytes SECTION_BOUNDARY_MARKER = {0x1B, 0x00, 0x00, 0x00};

// _get_dword: nullopt outside [0, len-4].
std::optional<std::uint32_t> get_dword(const Bytes& d, std::int64_t off) {
    if (off < 0 || off + 4 > static_cast<std::int64_t>(d.size())) return std::nullopt;
    std::uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

bool check(const Bytes& code, std::int64_t pos, std::uint32_t fcn, std::int64_t fcn_pos) {
    auto v = get_dword(code, pos + fcn_pos);
    return v && *v == fcn;
}

struct TextSlot {
    std::optional<std::string> text;  // nullopt == undecodable garbage
    std::size_t raw_len = 0;          // CP932 byte length of the chunk
};

struct SplitResult {
    std::size_t hdr_size;
    Bytes code;
    Bytes text;
};

SplitResult split_data(const Bytes& data) {
    std::size_t hdr_extra = 0;
    if (auto v = get_dword(data, HDRAS_POS)) hdr_extra = *v;
    std::size_t hdr_size = HDR_SIZE + hdr_extra;

    // Section boundary: right after the LAST 1B 00 00 00 marker.
    std::size_t boundary = hdr_size;
    if (data.size() >= 4) {
        for (std::size_t i = data.size() - 4 + 1; i-- > 0;) {
            if (std::memcmp(data.data() + i, SECTION_BOUNDARY_MARKER.data(), 4) == 0) {
                boundary = i + 4;
                break;
            }
        }
    }
    SplitResult r;
    r.hdr_size = std::min(hdr_size, data.size());
    std::size_t code_end = std::max(boundary, r.hdr_size);
    r.code.assign(data.begin() + static_cast<std::ptrdiff_t>(r.hdr_size),
                  data.begin() + static_cast<std::ptrdiff_t>(std::min(code_end, data.size())));
    r.text.assign(data.begin() + static_cast<std::ptrdiff_t>(std::min(code_end, data.size())),
                  data.end());
    return r;
}

// _build_text_section: map each in-section address to its decoded string
// (split on NUL; the trailing chunk after the final NUL is included).
std::unordered_map<std::int64_t, TextSlot> build_text_section(const Bytes& text_bytes) {
    std::unordered_map<std::int64_t, TextSlot> section;
    std::size_t pos = 0;
    std::size_t start = 0;
    auto emit = [&](std::size_t chunk_start, std::size_t chunk_len) {
        TextSlot slot;
        slot.raw_len = chunk_len;
        slot.text = cp932_to_utf8_strict(text_bytes.data() + chunk_start, chunk_len);
        section.emplace(static_cast<std::int64_t>(pos), std::move(slot));
    };
    for (std::size_t i = 0; i <= text_bytes.size(); ++i) {
        if (i == text_bytes.size() || text_bytes[i] == 0x00) {
            emit(start, i - start);
            pos += (i - start) + 1;
            start = i + 1;
        }
    }
    return section;
}

}  // namespace

std::optional<std::vector<Entry>> extract_one(const Bytes& data) {
    static const char MAGIC[] = "BurikoCompiledScriptVer1.00";
    if (data.size() < 27 || std::memcmp(data.data(), MAGIC, 27) != 0)
        return std::nullopt;

    SplitResult parts = split_data(data);
    const Bytes& code = parts.code;
    const std::int64_t code_size = static_cast<std::int64_t>(code.size());
    auto text_section = build_text_section(parts.text);
    const std::int64_t text_section_file_offset =
        static_cast<std::int64_t>(parts.hdr_size) + code_size;

    // Collect message-function addresses so backlog-only strings can be kept.
    std::set<std::int64_t> message_addrs;
    for (std::int64_t pos = 4; pos < code_size; pos += 4) {
        auto type_marker = get_dword(code, pos - 4);
        auto dword = get_dword(code, pos);
        if (!dword || !type_marker || *type_marker != STR_TYPE) continue;
        if (check(code, pos, TEXT_FCN, NAME_POS) || check(code, pos, TEXT_FCN, TEXT_POS))
            message_addrs.insert(static_cast<std::int64_t>(*dword) - code_size);
    }

    std::vector<Entry> out;
    std::set<std::int64_t> seen_text_addrs;

    for (std::int64_t pos = 4; pos < code_size; pos += 4) {
        auto type_marker = get_dword(code, pos - 4);
        auto dword = get_dword(code, pos);
        if (!dword || !type_marker) continue;
        std::int64_t text_addr = static_cast<std::int64_t>(*dword) - code_size;
        auto slot_it = text_section.find(text_addr);
        if (slot_it == text_section.end() || *type_marker != STR_TYPE) continue;
        const TextSlot& slot = slot_it->second;
        if (!slot.text || seen_text_addrs.count(text_addr)) continue;
        const std::string& text = *slot.text;

        // Identify the role via TEXT_FCN+offset patterns (kingshriek's check()).
        std::optional<std::string> speaker;
        std::string entry_type;
        if (check(code, pos, TEXT_FCN, NAME_POS)) {
            entry_type = "name";
        } else if (check(code, pos, TEXT_FCN, TEXT_POS)) {
            // Inline name pointer at pos-8; 0 = narration.
            auto name_dword = get_dword(code, pos + TEXT_POS - NAME_POS);
            if (name_dword && *name_dword) {
                auto spk_it = text_section.find(
                    static_cast<std::int64_t>(*name_dword) - code_size);
                if (spk_it != text_section.end() && spk_it->second.text &&
                    !spk_it->second.text->empty())
                    speaker = *spk_it->second.text;
            }
            entry_type = speaker ? "dialogue" : "narrative";
        } else if (check(code, pos, RUBY_FCN, RUBYK_POS) ||
                   check(code, pos, RUBY_FCN, RUBYF_POS)) {
            continue;  // ruby annotations
        } else if (check(code, pos, BKLG_FCN, BKLG_POS)) {
            // Keep backlog-only strings (epigraph, title cards); skip dupes.
            if (message_addrs.count(text_addr)) continue;
            entry_type = "narrative";
        } else {
            entry_type = "other";
        }

        if (!is_clean_text(text)) continue;

        bool has_jp = is_jp(text);
        if (!has_jp && entry_type != "name")
            continue;  // asset paths and labels

        seen_text_addrs.insert(text_addr);
        Entry e;
        e.offset = text_section_file_offset + text_addr;
        e.byte_len = static_cast<std::int64_t>(slot.raw_len);
        e.text = text;
        e.has_jp = has_jp;
        e.type = entry_type;
        if (entry_type == "dialogue" || entry_type == "narrative")
            e.speaker = speaker ? std::optional<std::string>(*speaker) : std::nullopt;
        out.push_back(std::move(e));
    }

    // Preserve code-walk order (matches engine playback order).
    return out;
}

boost::json::object script_to_json(const std::string& name, const std::string& arc_basename,
                                   std::size_t file_size, const std::vector<Entry>& entries) {
    bj::array strings;
    for (const auto& e : entries) {
        bj::object o;
        o["offset"] = e.offset;
        o["byte_len"] = e.byte_len;
        o["text"] = e.text;
        o["has_jp"] = e.has_jp;
        o["type"] = e.type;
        if (e.type == "dialogue" || e.type == "narrative") {
            if (e.speaker) o["speaker"] = *e.speaker;
            else o["speaker"] = nullptr;
        }
        strings.push_back(std::move(o));
    }
    bj::object script;
    script["file"] = name;
    script["arc"] = arc_basename;
    script["file_size"] = static_cast<std::int64_t>(file_size);
    script["jp_strings"] = static_cast<std::int64_t>(entries.size());
    script["strings"] = std::move(strings);
    return script;
}

int run_extract(const std::string& game_dir, const std::string& output_file) {
    log_info("GAME_DIR: " + game_dir);

    // Walk arcs in search order: name -> (arc_path, idx).  The first arc
    // that holds a name wins, like the engine.  The patched 03_picapica_06
    // in data01099.arc once lost to the original in data01103.arc, and its
    // new lines drew in Japanese while the corpus reported full coverage.
    struct Src { std::string arc; std::size_t idx; };
    std::map<std::string, Src> overlay;  // sorted by name, like sorted(overlay)
    for (const auto& arc_name : SCRIPT_ARCS) {
        std::string arc = game_dir + "\\" + arc_name;
        if (!fs::exists(fs::u8path(arc))) {
            log_info("  [skip] " + arc_name + " not found");
            continue;
        }
        try {
            auto entries = bgi::list_arc(arc);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                auto [it, inserted] = overlay.emplace(entries[i].name, Src{arc, i});
                if (!inserted)
                    log_info("  [shadowed] " + entries[i].name + " in " + arc_name +
                             " loses to " + fs::u8path(it->second.arc).filename().u8string());
            }
        } catch (const std::exception& e) {
            log_info("  [skip] " + arc_name + ": " + e.what());
        }
    }
    log_info("Resolved " + std::to_string(overlay.size()) + " unique script names from " +
             std::to_string(SCRIPT_ARCS.size()) + " arcs");

    bj::object out;
    std::size_t total_strings = 0;
    std::size_t skipped = 0;

    for (const auto& [name, src] : overlay) {
        Bytes data;
        try {
            data = bgi::read_entry(src.arc, src.idx);
        } catch (const std::exception& e) {
            log_info("  [skip] " + name + ": read error " + e.what());
            ++skipped;
            continue;
        }

        auto entries = extract_one(data);
        if (!entries) {
            ++skipped;
            continue;
        }
        if (entries->empty())
            continue;  // scripts with no JP text (master files, IC stubs, etc.)

        total_strings += entries->size();
        std::string arc_base = fs::u8path(src.arc).filename().u8string();
        out[name] = script_to_json(name, arc_base, data.size(), *entries);

        char line[128];
        std::snprintf(line, sizeof(line), "  %-40s %5zu JP strings",
                      name.c_str(), entries->size());
        log_info(line);
    }

    const std::size_t script_count = out.size();
    write_file(output_file, bj::serialize(bj::value(std::move(out))));

    print_line("");
    log_info("Scripts with JP text: " + std::to_string(script_count));
    log_info("Skipped (non-BGI / errors / empty): " + std::to_string(skipped));
    log_info("Total JP strings: " + std::to_string(total_strings));
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(output_file))) / 1024.0 / 1024.0);
    log_info("Wrote " + output_file + " (" + sz + ")");
    return 0;
}

}  // namespace snu::extract
