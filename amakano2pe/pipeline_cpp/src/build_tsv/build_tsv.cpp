// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "build_tsv.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"

namespace ama::build_tsv {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

const std::unordered_set<std::string>& translatable_types() {
    static const std::unordered_set<std::string> t = {"MESSAGE", "NAME"};
    return t;
}

// Runtime-substituted name strings that never appear as static script text.
// CatScene NAME entries for the protagonist are stored as the placeholder
// `$str20`; the engine resolves it to `賢一` *before* calling the text-render
// hook, so the extractor never sees `賢一` and no TSV row gets generated for
// it.  Result: the namebox shows Japanese every time the protagonist speaks.
// Inject the resolved JP->EN mapping here so the runtime hook can match it.
const std::vector<std::pair<std::string, std::string>>& default_runtime_names() {
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"\xE8\xB3\xA2\xE4\xB8\x80", "Kenichi"},  // 賢一
    };
    return v;
}

// Script string-variable substitutions.  The engine resolves `$strNN` to its
// current value *before* calling the text-render hook, so any TSV key that
// still contains a `$strNN` literal would never match at runtime.  For each
// entry below, sibling rows are emitted with the placeholder replaced by
// `jp_value` in the JP key and `en_value` in the EN value.
//
// Only `$str20` (protagonist first name) is mapped here -- the runtime value
// is confirmed by the proxy_log capturing literal `[賢一]` at the namebox
// render hook.  The default for `$str10` (surname) is not stored as plain
// bytes anywhere in the game files (cs2.exe, BootMenu.exe, every .int archive,
// m2setting.xml -- all checked, no hits), so its runtime value is not yet
// known.  Lines containing `$str10` are therefore left alone; the substitution
// loop skips them so we never emit a half-substituted key.
struct VarSub {
    std::string var;
    std::string jp_value;
    std::string en_value;
};

const std::vector<VarSub>& runtime_var_subs() {
    static const std::vector<VarSub> v = {
        {"$str20", "\xE8\xB3\xA2\xE4\xB8\x80", "Kenichi"},  // 賢一
    };
    return v;
}

// Placeholders we know about but have no runtime value for.
const std::vector<std::string>& unsubbed_vars() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const char* candidate : {"$str10"}) {
            bool known = false;
            for (const auto& s : runtime_var_subs())
                if (s.var == candidate) known = true;
            if (!known) out.emplace_back(candidate);
        }
        return out;
    }();
    return v;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

// Insertion-ordered jp -> en map.  Insertion order is load-bearing: it decides
// the row order of translation_table.tsv, and first-seen wins on a conflict.
class OrderedMap {
public:
    const std::string* get(const std::string& k) const {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &order_[it->second].second;
    }
    // Returns true when the key was new.
    bool insert(const std::string& k, const std::string& v) {
        auto it = index_.find(k);
        if (it != index_.end()) return false;
        index_.emplace(k, order_.size());
        order_.emplace_back(k, v);
        return true;
    }
    std::size_t size() const { return order_.size(); }
    const std::vector<std::pair<std::string, std::string>>& items() const { return order_; }

private:
    std::vector<std::pair<std::string, std::string>> order_;
    std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace

std::string flatten(const std::string& text) {
    std::string s = replace_all(text, "\r\n", " ");
    s = replace_all(s, "\r", " ");
    s = replace_all(s, "\n", " ");
    return trim(s);
}

std::string escape_for_tsv(const std::string& text) {
    return replace_all(replace_all(text, "\\", "\\\\"), "\t", "\\t");
}

int run_build(const std::string& translated_file, const std::string& out_tsv) {
    log_info("Loading " + translated_file + "...");
    bj::value root = json_parse_file(translated_file);

    OrderedMap seen;
    long long conflicts = 0;

    for (const auto& fkv : root.get_object()) {
        if (!fkv.value().is_object()) continue;
        const auto& fdata = fkv.value().get_object();
        auto lines_it = fdata.find("lines");
        if (lines_it == fdata.end() || !lines_it->value().is_array()) continue;
        for (const auto& lv : lines_it->value().get_array()) {
            const auto& ln = lv.get_object();
            const std::string type(ln.at("type").get_string());
            if (!translatable_types().count(type)) continue;
            std::string jp, en;
            if (auto* c = ln.if_contains("content"))
                if (c->is_string()) jp = std::string(c->get_string());
            if (auto* t = ln.if_contains("translated"))
                if (t->is_string()) en = std::string(t->get_string());
            if (jp.empty() || en.empty() || jp == en) continue;
            const std::string en_clean = flatten(en);
            const std::string jp_clean = flatten(jp);
            if (en_clean.empty() || jp_clean.empty()) continue;
            const std::string* prev = seen.get(jp_clean);
            if (!prev) seen.insert(jp_clean, en_clean);
            else if (*prev != en_clean) ++conflicts;
        }
    }

    // Inject runtime-substituted names.  Only fill gaps -- a script-derived
    // translation always wins, since it carries the real context.
    long long injected = 0;
    for (const auto& [jp, en] : default_runtime_names()) {
        const std::string* prev = seen.get(jp);
        if (!prev) {
            seen.insert(jp, en);
            ++injected;
        } else if (*prev != en) {
            log_info("  WARN: runtime-name '" + jp + "' already present as '" + *prev +
                     "' (kept), not overwriting with '" + en + "'");
        }
    }

    // Emit substituted-key siblings for entries containing script string vars.
    // Skip any entry that contains a placeholder we don't have a runtime value
    // for (e.g. `$str10`) so we never emit a half-resolved key the engine would
    // never produce.
    long long substituted = 0;
    {
        // Snapshot first: the loop inserts into `seen`, and the new sibling
        // rows must not themselves be re-scanned for substitution.
        const auto snapshot = seen.items();
        for (const auto& [jp, en] : snapshot) {
            bool blocked = false;
            for (const auto& v : unsubbed_vars())
                if (jp.find(v) != std::string::npos) blocked = true;
            if (blocked) continue;
            bool has_var = false;
            for (const auto& s : runtime_var_subs())
                if (jp.find(s.var) != std::string::npos) has_var = true;
            if (!has_var) continue;

            std::string jp_sub = jp, en_sub = en;
            for (const auto& s : runtime_var_subs()) {
                jp_sub = replace_all(jp_sub, s.var, s.jp_value);
                en_sub = replace_all(en_sub, s.var, s.en_value);
            }
            if (jp_sub == jp) continue;
            if (seen.get(jp_sub)) continue;
            seen.insert(jp_sub, en_sub);
            ++substituted;
        }
    }

    fs::create_directories(fs::u8path(out_tsv).parent_path());
    std::string out;
    for (const auto& [jp, en] : seen.items())
        out += escape_for_tsv(jp) + "\t" + escape_for_tsv(en) + "\n";
    write_file(out_tsv, out);

    log_info("Generated " + out_tsv);
    log_info("  Entries:     " + comma(static_cast<long long>(seen.size())));
    log_info("  Conflicts:   " + comma(conflicts) + "  (same JP, different EN -- first kept)");
    log_info("  Injected:    " + comma(injected) + "  (runtime-substituted names)");
    log_info("  Substituted: " + comma(substituted) +
             "  (sibling rows with $strNN resolved)");
    log_info("  File size:   " +
             comma(static_cast<long long>(fs::file_size(fs::u8path(out_tsv)))) + " bytes");
    return 0;
}

}  // namespace ama::build_tsv
