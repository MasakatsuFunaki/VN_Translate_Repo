// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv/build_tsv.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <boost/json.hpp>

#include "build_tsv/name_fixups.h"
#include "common/util.h"

namespace frat::build_tsv {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr char32_t kRubyOpen = 0x226A;      // ≪
constexpr char32_t kRubySep = 0xFF0F;       // ／
constexpr char32_t kRubyClose = 0x226B;     // ≫
constexpr char32_t kOpenBracket = 0x300C;   // 「
constexpr char32_t kCloseBracket = 0x300D;  // 」
constexpr char32_t kFullQuestion = 0xFF1F;  // ？

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

// CP932 byte length; unencodable codepoints contribute zero bytes.
std::size_t cp932_len_ignore(const std::string& s) {
    std::size_t n = 0, i = 0;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        if (!cp932_encodable(cp)) continue;
        n += utf8_to_cp932_strict(utf8_encode_cp(cp))->size();
    }
    return n;
}

std::string bytes_to_string(const Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Insertion order is load-bearing: it decides TSV line order.
class OrderedSet {
public:
    bool insert(const std::string& v) {
        if (!index_.insert(v).second) return false;
        order_.push_back(v);
        return true;
    }
    bool contains(const std::string& v) const { return index_.count(v) != 0; }
    const std::vector<std::string>& items() const { return order_; }

private:
    std::vector<std::string> order_;
    std::unordered_set<std::string> index_;
};

}  // namespace

// Defined here because name_fixups.cpp is a generated table dump.
const std::vector<std::pair<std::string, std::string>>& NameFixupsByLenDesc() {
    static const std::vector<std::pair<std::string, std::string>> data = [] {
        std::vector<std::pair<std::string, std::string>> v = NameFixups();
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return char_len(a.first) > char_len(b.first);
        });
        return v;
    }();
    return data;
}

std::string flatten(const std::string& text) {
    std::string s = replace_all(text, "\r\n", " ");
    s = replace_all(s, "\r", " ");
    s = replace_all(s, "\n", " ");
    return trim(s);
}

std::string escape_for_tsv(const std::string& text) {
    return replace_all(replace_all(text, "\\", "\\\\"), "\t", "\\t");
}

std::string cp932_safe(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        switch (cp) {
        case 0x2014: out += utf8_encode_cp(0x2015); continue;  // em dash -> horizontal bar
        case 0x2018:
        case 0x2019: out += '\''; continue;
        case 0x00A0: out += ' '; continue;
        default: break;
        }
        // Best-fit would silently map accented Latin to ASCII; '?' is honest.
        out += cp932_encodable(cp) ? utf8_encode_cp(cp) : std::string("?");
    }
    return out;
}

std::optional<Bytes> to_cp932(const std::string& s) { return utf8_to_cp932_strict(s); }

std::string clean_en(const std::string& en) {
    bool any_jp = false;
    for (std::size_t i = 0; i < en.size();) {
        const char32_t cp = utf8_next(en, i);
        if (cp >= 0x3040 && cp <= 0x9FFF) { any_jp = true; break; }
    }
    if (!any_jp) return en;
    std::string out = en;
    for (const auto& [name_jp, name_en] : NameFixupsByLenDesc())
        if (!name_en.empty() && out.find(name_jp) != std::string::npos)
            out = replace_all(out, name_jp, name_en);
    return out;
}

std::string strip_ruby(const std::string& s) {
    const std::vector<char32_t> cps = utf8_decode(s);
    std::vector<char32_t> out;
    std::size_t i = 0;
    while (i < cps.size()) {
        if (cps[i] != kRubyOpen) {
            out.push_back(cps[i++]);
            continue;
        }
        // ≪([^／≫]+)／[^≫]+≫  -- both runs need at least one codepoint.
        std::size_t j = i + 1;
        while (j < cps.size() && cps[j] != kRubySep && cps[j] != kRubyClose) ++j;
        if (j == i + 1 || j >= cps.size() || cps[j] != kRubySep) {
            out.push_back(cps[i++]);
            continue;
        }
        std::size_t k = j + 1;
        while (k < cps.size() && cps[k] != kRubyClose) ++k;
        if (k == j + 1 || k >= cps.size()) {
            out.push_back(cps[i++]);
            continue;
        }
        out.insert(out.end(), cps.begin() + static_cast<std::ptrdiff_t>(i + 1),
                   cps.begin() + static_cast<std::ptrdiff_t>(j));
        i = k + 1;
    }
    return utf8_encode(out);
}

std::string ensure_close_bracket(const std::string& s) {
    std::size_t opens = 0, closes = 0;
    for (std::size_t i = 0; i < s.size();) {
        const char32_t cp = utf8_next(s, i);
        if (cp == kOpenBracket) ++opens;
        if (cp == kCloseBracket) ++closes;
    }
    if (opens && opens > closes) return s + utf8_encode_cp(kCloseBracket);
    return s;
}

bool speaker_match(const std::string& s, std::string& name, std::string& quote) {
    const std::vector<char32_t> cps = utf8_decode(s);
    if (cps.empty()) return false;
    // Greedy ／? first, then the backtracked empty alternative.
    std::vector<std::size_t> skips;
    if (cps[0] == kRubySep) skips.push_back(1);
    skips.push_back(0);
    for (std::size_t skip : skips) {
        for (std::size_t j = skip; j < cps.size(); ++j) {
            if (cps[j] == kOpenBracket) {
                if (j > skip) {
                    name = utf8_encode(std::vector<char32_t>(
                        cps.begin() + static_cast<std::ptrdiff_t>(skip),
                        cps.begin() + static_cast<std::ptrdiff_t>(j)));
                    quote = utf8_encode(std::vector<char32_t>(
                        cps.begin() + static_cast<std::ptrdiff_t>(j), cps.end()));
                    return true;
                }
                break;  // group 1 needs at least one character
            }
            if (is_unicode_space(cps[j])) break;  // \s is excluded from group 1
        }
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> variants(const std::string& jp,
                                                          const std::string& en) {
    std::vector<std::pair<std::string, std::string>> out;
    OrderedSet seen;
    const auto push = [&](const std::string& jp_v, const std::string& en_v) {
        if (!jp_v.empty() && seen.insert(jp_v)) out.emplace_back(jp_v, en_v);
    };

    push(jp, en);
    const std::string jp_noruby = strip_ruby(jp);
    push(jp_noruby, en);
    const std::string slash = utf8_encode_cp(kRubySep);
    for (const std::string& base : {jp, jp_noruby})
        if (base.rfind(slash, 0) == 0) push(cp_substr(base, 1), en);

    // Missing-」 fixup over a snapshot of current keys.
    {
        std::vector<std::string> extra;
        for (const auto& base : seen.items()) {
            const std::string closed = ensure_close_bracket(base);
            if (closed != base) extra.push_back(closed);
        }
        for (const auto& c : extra) push(c, en);
    }
    // Missing-。 fixup. U+301C is unreachable (CP932 0x8160 -> U+FF5E) but
    // removing it would change the TSV.
    {
        static const std::vector<char32_t> terminators = {0x3002, 0xFF01, 0xFF1F, 0x300D,
                                                          0x301C, 0x2026, '!', '?', '.'};
        std::vector<std::string> extra;
        for (const auto& base : seen.items()) {
            if (base.empty()) continue;
            const std::vector<char32_t> cps = utf8_decode(base);
            if (std::find(terminators.begin(), terminators.end(), cps.back()) ==
                terminators.end())
                extra.push_back(base + utf8_encode_cp(0x3002));
        }
        for (const auto& c : extra) push(c, en);
    }
    // ？？？ mystery-speaker variant for long enough quotes.
    std::string en_name, en_quote;
    if (speaker_match(trim(en), en_name, en_quote) && !en_quote.empty()) {
        const std::string mystery_prefix(3, '?');
        const std::string jp_mystery = utf8_encode({kFullQuestion, kFullQuestion, kFullQuestion});
        std::vector<std::string> snapshot = seen.items();
        for (const auto& base : snapshot) {
            std::string name_jp, quote_jp;
            if (!speaker_match(trim(base), name_jp, quote_jp)) continue;
            if (cp932_len_ignore(quote_jp) < MIN_MYSTERY_QUOTE_BYTES) continue;
            const std::string mystery_en = mystery_prefix + en_quote;
            const std::string key = jp_mystery + quote_jp;
            push(key, mystery_en);
            const std::string closed = ensure_close_bracket(key);
            if (closed != key) push(closed, mystery_en);
        }
    }
    return out;
}

namespace {

// First non-empty string value among `keys`.
std::string or_str(const bj::object& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        if (auto* v = o.if_contains(k))
            if (v->is_string() && !v->get_string().empty()) return std::string(v->get_string());
    return {};
}

const bj::array* or_array(const bj::object& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        if (auto* v = o.if_contains(k))
            if (v->is_array() && !v->get_array().empty()) return &v->get_array();
    return nullptr;
}

}  // namespace

int run_build(const std::string& translated_file, const std::string& out_tsv) {
    log_info("Loading " + translated_file + "...");
    bj::value root = json_parse_file(translated_file);

    std::vector<std::pair<std::string, std::string>> table;  // cp932 jp -> cp932 en
    std::unordered_map<std::string, std::size_t> table_index;
    long long conflicts = 0, skipped_unmappable = 0, skipped_empty = 0, variants_added = 0;

    for (const auto& fkv : root.get_object()) {
        if (!fkv.value().is_object()) continue;
        const bj::array* strings = or_array(fkv.value().get_object(), {"strings", "lines"});
        if (!strings) continue;
        for (const auto& sv : *strings) {
            if (!sv.is_object()) continue;
            const auto& entry = sv.get_object();
            const std::string jp_full = or_str(entry, {"text", "content"});
            const std::string en_raw = or_str(entry, {"translated"});
            if (jp_full.empty() || en_raw.empty() || jp_full == en_raw) continue;
            const std::string en_full = clean_en(en_raw);

            for (const auto& [jp_variant, en_variant] : variants(jp_full, en_full)) {
                const std::string jp_clean = flatten(jp_variant);
                const std::string en_clean = flatten(cp932_safe(en_variant));
                if (jp_clean.empty() || en_clean.empty()) {
                    ++skipped_empty;
                    continue;
                }
                const auto jp_b = to_cp932(jp_clean);
                const auto en_b = to_cp932(en_clean);
                if (!jp_b || !en_b) {
                    ++skipped_unmappable;
                    continue;
                }
                const std::string jp_key = bytes_to_string(*jp_b);
                const std::string en_val = bytes_to_string(*en_b);
                auto it = table_index.find(jp_key);
                if (it == table_index.end()) {
                    table_index.emplace(jp_key, table.size());
                    table.emplace_back(jp_key, en_val);
                    if (jp_variant != jp_full) ++variants_added;
                } else if (table[it->second].second != en_val) {
                    ++conflicts;  // same JP, different EN -- first kept
                }
            }
        }
    }

    Bytes out;
    for (const auto& [jp_b, en_b] : table) {
        // Escape AFTER dedup: unmappable-only differences must collide.
        const auto jp_e = to_cp932(escape_for_tsv(*cp932_to_utf8_strict(
            reinterpret_cast<const std::uint8_t*>(jp_b.data()), jp_b.size())));
        const auto en_e = to_cp932(escape_for_tsv(*cp932_to_utf8_strict(
            reinterpret_cast<const std::uint8_t*>(en_b.data()), en_b.size())));
        out.insert(out.end(), jp_e->begin(), jp_e->end());
        out.push_back('\t');
        out.insert(out.end(), en_e->begin(), en_e->end());
        out.push_back('\n');
    }
    const fs::path parent = fs::u8path(out_tsv).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    write_file(out_tsv, out);

    log_info("Generated " + out_tsv);
    log_info("  Entries:               " + comma(static_cast<long long>(table.size())));
    log_info("  Extra variants added:  " + comma(variants_added) +
             "  (ruby-stripped / leading-slash / ??-speaker / closed-bracket)");
    log_info("  Conflicts:             " + comma(conflicts) +
             "  (same JP, different EN - first kept)");
    log_info("  Skipped unmappable:    " + comma(skipped_unmappable));
    log_info("  Skipped empty:         " + comma(skipped_empty));
    log_info("  File size:             " +
             comma(static_cast<long long>(fs::file_size(fs::u8path(out_tsv)))) + " bytes");
    return 0;
}

}  // namespace frat::build_tsv
