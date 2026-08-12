// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "repack.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>

#include <boost/json.hpp>

namespace exc::repack {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
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

std::vector<std::string> split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(sep, pos);
        if (hit == std::string::npos) {
            out.push_back(s.substr(pos));
            return out;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + sep.size();
    }
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

void wr_u32(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
    b[off + 2] = static_cast<std::uint8_t>(v >> 16);
    b[off + 3] = static_cast<std::uint8_t>(v >> 24);
}

Bytes u32le(std::uint32_t v) {
    return Bytes{static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                 static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
}

// First occurrence of `needle` at or after `from`, npos when absent.
std::size_t find_bytes(const Bytes& hay, const Bytes& needle, std::size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    const auto it = std::search(hay.begin() + static_cast<std::ptrdiff_t>(
                                    std::min(from, hay.size())),
                                hay.end(), needle.begin(), needle.end());
    return it == hay.end() ? std::string::npos
                           : static_cast<std::size_t>(it - hay.begin());
}

// Last occurrence of `needle` that ends at or before `end`, npos when absent.
std::size_t rfind_bytes(const Bytes& hay, const Bytes& needle, std::size_t end) {
    if (needle.empty() || end < needle.size()) return std::string::npos;
    const auto stop = hay.begin() + static_cast<std::ptrdiff_t>(std::min(end, hay.size()));
    const auto it = std::find_end(hay.begin(), stop, needle.begin(), needle.end());
    return it == stop ? std::string::npos : static_cast<std::size_t>(it - hay.begin());
}

// The three consecutive markers that identify the text section.
Bytes text_section_marker() {
    Bytes m;
    for (std::uint32_t v : {0x66660001u, 0x55550002u, 0x44440002u}) {
        const Bytes w = u32le(v);
        m.insert(m.end(), w.begin(), w.end());
    }
    return m;
}

}  // namespace

// ---- text shaping ----------------------------------------------------------

std::string merge_speaker_line(const std::string& text) {
    const std::size_t idx = text.find("\r\n");
    if (idx == std::string::npos) return text;
    const std::string first_line = text.substr(0, idx);
    const std::string rest = text.substr(idx + 2);
    if (!first_line.empty() && char_len(first_line) <= 20 && !rest.empty() &&
        first_line.front() != '"')
        return first_line + ": " + rest;
    return text;
}

std::string sanitize_newlines(const std::string& text) {
    // Strip leading newlines ONLY -- leading spaces are deliberate indentation
    // in the translation and must survive.
    std::size_t b = 0;
    while (b < text.size() && text[b] == '\n') ++b;
    std::string s = text.substr(b);
    s = replace_all(s, "\r\n", "\n");
    s = replace_all(s, "\n", "\r\n");
    return s;
}

std::string word_wrap(const std::string& text, int max_chars) {
    const auto segments = split(text, "\r\n");
    std::vector<std::string> result;
    for (const auto& seg : segments) {
        if (char_len(seg) <= static_cast<std::size_t>(max_chars)) {
            result.push_back(seg);
            continue;
        }
        std::string current;
        for (const auto& word : split(seg, " ")) {
            if (word.empty()) {
                if (!current.empty()) current += ' ';
                continue;
            }
            if (!current.empty() &&
                char_len(current) + 1 + char_len(word) > static_cast<std::size_t>(max_chars)) {
                result.push_back(current);
                current = word;
            } else if (!current.empty()) {
                current += " " + word;
            } else {
                current = word;
            }
        }
        if (!current.empty()) result.push_back(current);
    }
    // Truncate to MAX_LINES so the engine's own cap never silently eats text.
    if (result.size() > static_cast<std::size_t>(MAX_LINES))
        result.resize(static_cast<std::size_t>(MAX_LINES));
    return join(result, "\r\n");
}

// ---- SPT envelope ----------------------------------------------------------

Bytes encrypt_spt(const Bytes& data) {
    Bytes out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
        out[i] = static_cast<std::uint8_t>(data[i] ^ 0xFF);
    return out;
}

Bytes decrypt_spt(const Bytes& data) { return encrypt_spt(data); }

// ---- text block ------------------------------------------------------------

std::size_t find_next_section_marker(const Bytes& dec, std::size_t start) {
    const Bytes marker = u32le(0x66660001u);
    std::size_t pos = start;
    while (pos + 12 < dec.size()) {
        const std::size_t idx = find_bytes(dec, marker, pos);
        if (idx == std::string::npos) return dec.size();
        // A real section header is preceded by its size field and followed by
        // a 0x5555xxxx type word.
        if (idx >= 4 && idx + 8 <= dec.size()) {
            const std::uint32_t next_val = rd_u32(dec, idx + 4);
            if ((next_val & 0xFFFF0000u) == 0x55550000u) return idx - 4;
        }
        pos = idx + 4;
    }
    return dec.size();
}

std::size_t find_text_block_start(const Bytes& dec,
                                  const std::vector<std::size_t>& translated_offsets) {
    if (translated_offsets.empty()) return static_cast<std::size_t>(-1);
    if (dec.size() < 0x14) return static_cast<std::size_t>(-1);
    const std::uint32_t h_10 = rd_u32(dec, 0x10);
    // Search FORWARD through the whole file: a backward search from the lowest
    // translated offset fails when the extractor picked up strings in sections
    // that precede the text-section marker.
    const std::size_t idx = find_bytes(dec, text_section_marker(), 0);
    if (idx != std::string::npos) return idx + 12 + static_cast<std::size_t>(h_10) * 4;
    return *std::min_element(translated_offsets.begin(), translated_offsets.end());
}

std::vector<BlockString> parse_text_block(const Bytes& dec, std::size_t start,
                                          std::size_t* block_end) {
    const std::size_t block_limit = find_next_section_marker(dec, start);
    if (block_end) *block_end = block_limit;

    std::vector<BlockString> strings;
    std::size_t pos = start;
    while (pos < block_limit) {
        const auto* nul = static_cast<const std::uint8_t*>(
            std::memchr(dec.data() + pos, 0, dec.size() - pos));
        if (!nul) break;
        const std::size_t end = static_cast<std::size_t>(nul - dec.data());
        if (end >= block_limit) break;
        if (end == pos) {
            ++pos;
            continue;
        }
        // A strict-decode failure means we have walked off the text data.
        if (!cp932_to_utf8_strict(dec.data() + pos, end - pos)) break;

        BlockString s;
        s.offset = pos;
        s.byte_len = end - pos;
        s.raw.assign(dec.begin() + static_cast<std::ptrdiff_t>(pos),
                     dec.begin() + static_cast<std::ptrdiff_t>(end));
        strings.push_back(std::move(s));
        pos = ((end + 1 + 3) / 4) * 4;
    }
    return strings;
}

namespace {

// Rebuild the block, applying translations by offset first and by content
// second.  Returns the new bytes plus each string's byte offset within it.
Bytes build_text_block(const std::vector<BlockString>& block_strings,
                       const std::map<std::size_t, std::string>& trans_map,
                       const std::unordered_map<std::string, std::string>& content_trans,
                       long long* applied, std::vector<std::size_t>* string_offsets) {
    Bytes out;
    *applied = 0;
    string_offsets->clear();
    std::size_t current_offset = 0;

    for (const auto& s : block_strings) {
        string_offsets->push_back(current_offset);

        const std::string* translation = nullptr;
        auto it = trans_map.find(s.offset);
        if (it != trans_map.end()) {
            translation = &it->second;
        } else if (!content_trans.empty()) {
            const std::string text_content =
                cp932_to_utf8_replace(s.raw.data(), s.raw.size());
            auto cit = content_trans.find(text_content);
            if (cit != content_trans.end()) translation = &cit->second;
        }

        Bytes encoded;
        if (translation) {
            const std::string text =
                word_wrap(merge_speaker_line(sanitize_newlines(*translation)));
            encoded = utf8_to_cp932_replace(text);
            ++*applied;
        } else {
            encoded = s.raw;
        }

        Bytes data = encoded;
        data.push_back(0);
        while (data.size() % 4) data.push_back(0);
        out.insert(out.end(), data.begin(), data.end());
        current_offset += data.size();
    }
    return out;
}

}  // namespace

int run_repack(const std::string& translated_file, const std::string& spt_dir,
               const std::string& backup_dir) {
    log_info("Loading translated text...");
    bj::value root = json_parse_file(translated_file);

    if (!fs::exists(fs::u8path(backup_dir))) {
        fs::create_directories(fs::u8path(backup_dir));
        log_info("Created backup directory: " + backup_dir);
    }

    long long total_patched = 0, total_files = 0;

    for (const auto& fkv : root.get_object()) {
        const std::string fname(fkv.key());
        if (!fkv.value().is_object()) continue;
        const auto* strings_v = fkv.value().get_object().if_contains("strings");
        if (!strings_v || !strings_v->is_array()) continue;
        const auto& entries = strings_v->get_array();

        // offset -> translated
        std::map<std::size_t, std::string> trans_map;
        for (const auto& sv : entries) {
            const auto& s = sv.get_object();
            std::string translated, jp;
            if (auto* t = s.if_contains("translated"))
                if (t->is_string()) translated = std::string(t->get_string());
            if (auto* t = s.if_contains("text"))
                if (t->is_string()) jp = std::string(t->get_string());
            if (!translated.empty() && translated != jp)
                trans_map[static_cast<std::size_t>(s.at("offset").get_int64())] = translated;
        }
        if (trans_map.empty()) continue;

        // Content-based lookup, to cope with leading control-byte prefixes.
        // The 4-byte-aligned text block puts 1-3 non-NUL control bytes between
        // strings; the linear extractor absorbs them into the NEXT string,
        // shifting its recorded offset by 1-3 bytes, while the repacker parses
        // at the 4-aligned offset-table positions and sees the string WITHOUT
        // the prefix.  Registering the 1/2/3-byte-stripped variants lets the
        // content lookup match either form.
        std::unordered_map<std::string, std::string> content_trans;
        for (const auto& sv : entries) {
            const auto& s = sv.get_object();
            std::string jp, translated;
            if (auto* t = s.if_contains("text"))
                if (t->is_string()) jp = std::string(t->get_string());
            if (auto* t = s.if_contains("translated"))
                if (t->is_string()) translated = std::string(t->get_string());
            if (translated.empty() || translated == jp) continue;
            content_trans[jp] = translated;
            const Bytes jp_bytes = utf8_to_cp932_replace(jp);
            for (std::size_t n = 1; n < 4; ++n) {
                if (jp_bytes.size() <= n) continue;
                if (auto stripped =
                        cp932_to_utf8_strict(jp_bytes.data() + n, jp_bytes.size() - n))
                    content_trans.emplace(*stripped, translated);
            }
        }

        const std::string filepath = spt_dir + "\\" + fname;
        if (!fs::exists(fs::u8path(filepath))) {
            log_info("  SKIP " + fname + ": file not found");
            continue;
        }
        const std::string backup_path = backup_dir + "\\" + fname;
        if (!fs::exists(fs::u8path(backup_path)))
            fs::copy_file(fs::u8path(filepath), fs::u8path(backup_path));

        // ALWAYS read the pristine backup, so repacking is idempotent.
        const Bytes dec = decrypt_spt(read_file(backup_path));

        std::vector<std::size_t> translated_offsets;
        translated_offsets.reserve(trans_map.size());
        for (const auto& kv : trans_map) translated_offsets.push_back(kv.first);

        const std::size_t text_start = find_text_block_start(dec, translated_offsets);
        if (text_start == static_cast<std::size_t>(-1)) {
            log_info("  SKIP " + fname + ": no text block found");
            continue;
        }

        std::size_t block_end = 0;
        const auto block_strings = parse_text_block(dec, text_start, &block_end);
        if (block_strings.empty()) {
            log_info("  SKIP " + fname + ": empty text block");
            continue;
        }

        long long applied = 0;
        std::vector<std::size_t> string_offsets;
        const Bytes new_block =
            build_text_block(block_strings, trans_map, content_trans, &applied, &string_offsets);
        if (applied == 0) {
            log_info("  " + fname + ": no translations matched text block");
            continue;
        }

        // ---- structural fixup ----
        const std::uint32_t h_10 = rd_u32(dec, 0x10);
        const std::uint32_t h_14 = rd_u32(dec, 0x14);
        const std::uint32_t ot_base = h_14 + h_10;  // VA base for string pointers

        const std::size_t sec_marker_pos = rfind_bytes(dec, text_section_marker(), text_start);
        if (sec_marker_pos == std::string::npos) {
            log_info("  SKIP " + fname + ": cannot find text section header");
            continue;
        }
        const std::size_t sec_pre_off = sec_marker_pos - 4;  // pre_value

        // 1. Rebuild the offset table: entry[i] = ot_base + byte_offset[i] / 4.
        Bytes new_ot(static_cast<std::size_t>(h_10) * 4, 0);
        for (std::size_t i = 0; i < std::min<std::size_t>(h_10, string_offsets.size()); ++i)
            wr_u32(new_ot, i * 4, ot_base + static_cast<std::uint32_t>(string_offsets[i] / 4));

        // 2. New section pre_value (total section size / 4, header included).
        const std::size_t new_section_total = new_ot.size() + new_block.size() + 16;
        const std::uint32_t new_pre = static_cast<std::uint32_t>(new_section_total / 4);

        // 3. Assemble.
        Bytes new_dec(dec.begin(), dec.begin() + static_cast<std::ptrdiff_t>(sec_pre_off));
        const Bytes pre_w = u32le(new_pre);
        new_dec.insert(new_dec.end(), pre_w.begin(), pre_w.end());
        new_dec.insert(new_dec.end(), dec.begin() + static_cast<std::ptrdiff_t>(sec_marker_pos),
                       dec.begin() + static_cast<std::ptrdiff_t>(sec_marker_pos + 12));
        new_dec.insert(new_dec.end(), new_ot.begin(), new_ot.end());
        new_dec.insert(new_dec.end(), new_block.begin(), new_block.end());
        new_dec.insert(new_dec.end(), dec.begin() + static_cast<std::ptrdiff_t>(block_end),
                       dec.end());

        // 4. Header field h_1C = h_14 + new_pre.
        wr_u32(new_dec, 0x1C, h_14 + new_pre);

        // 5. Post-text VA relocation.  Sections after the text block shift when
        //    the text grows or shrinks; their DWORD-aligned VAs must follow.
        //    Bytecode operands are NOT touched -- byte-scan analysis showed
        //    every match there was a false positive at a non-DWORD offset.
        const std::uint32_t old_pre = rd_u32(dec, sec_pre_off);
        const long long delta_bytes =
            static_cast<long long>(new_block.size()) -
            (static_cast<long long>(old_pre) * 4 - 16 - static_cast<long long>(h_10) * 4);
        const long long delta_va = delta_bytes / 4;
        const std::size_t text_sec_end =
            sec_pre_off + 4 + 12 + new_ot.size() + new_block.size();

        if (delta_va != 0) {
            const std::uint32_t h_1C_orig = rd_u32(dec, 0x1C);
            const std::uint32_t post_va_min = h_1C_orig;
            const std::uint32_t post_va_max =
                ot_base + static_cast<std::uint32_t>((dec.size() - text_start) / 4) + 1;

            long long post_reloc = 0;
            for (std::size_t off = text_sec_end; off + 3 < new_dec.size(); off += 4) {
                const std::uint32_t val = rd_u32(new_dec, off);
                if (val >= post_va_min && val < post_va_max) {
                    wr_u32(new_dec, off, static_cast<std::uint32_t>(val + delta_va));
                    ++post_reloc;
                }
            }

            // Header/bytecode sections before the text block also reference
            // post-text sections and must be relocated too.
            long long pre_reloc = 0;
            for (std::size_t off = 0x20; off + 3 < sec_pre_off; off += 4) {
                const std::uint32_t val = rd_u32(new_dec, off);
                if (val >= post_va_min && val < post_va_max) {
                    wr_u32(new_dec, off, static_cast<std::uint32_t>(val + delta_va));
                    ++pre_reloc;
                }
            }

            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "    VA reloc: %lld post-text + %lld bytecode refs shifted by "
                          "%+lld (%+lld bytes)",
                          post_reloc, pre_reloc, delta_va, delta_bytes);
            log_info(buf);
        }

        write_file(filepath, encrypt_spt(new_dec));

        const long long size_diff =
            static_cast<long long>(new_dec.size()) - static_cast<long long>(dec.size());
        total_patched += applied;
        ++total_files;
        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      "  %s: patched %lld/%zu strings (block: %zu strings, size %+lld bytes)",
                      fname.c_str(), applied, trans_map.size(), block_strings.size(), size_diff);
        log_info(buf);
    }

    log_info("\nRepack complete:");
    log_info("  Files modified: " + std::to_string(total_files));
    log_info("  Strings patched: " + std::to_string(total_patched));
    log_info("  Backups in: " + backup_dir);
    return 0;
}

int run_restore(const std::string& spt_dir, const std::string& backup_dir) {
    if (!fs::exists(fs::u8path(backup_dir))) {
        log_info("No backup found. Nothing to restore.");
        return 0;
    }
    int restored = 0;
    for (const auto& e : fs::directory_iterator(fs::u8path(backup_dir))) {
        const std::string name = e.path().filename().u8string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".spt") != 0) continue;
        fs::copy_file(e.path(), fs::u8path(spt_dir + "\\" + name),
                      fs::copy_options::overwrite_existing);
        ++restored;
    }
    log_info("Restored " + std::to_string(restored) + " files from backup.");
    return 0;
}

}  // namespace exc::repack
