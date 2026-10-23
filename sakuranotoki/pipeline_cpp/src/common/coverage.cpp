// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "coverage.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

#include "util.h"

namespace snt {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

// Kana, kanji, and latin letters and digits including the fullwidth forms.
// KATAKANA MIDDLE DOT (U+30FB) is punctuation and is excluded.
bool carries_meaning(char32_t cp) {
    if (cp >= 0x3040 && cp <= 0x309F) return true;          // hiragana
    if (cp >= 0x30A0 && cp <= 0x30FF) return cp != 0x30FB;  // katakana
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;          // CJK
    if (cp >= 0x3400 && cp <= 0x4DBF) return true;          // CJK ext-A
    if (cp >= 0xFF66 && cp <= 0xFF9D) return true;          // halfwidth kana
    if (cp >= U'0' && cp <= U'9') return true;
    if (cp >= U'A' && cp <= U'Z') return true;
    if (cp >= U'a' && cp <= U'z') return true;
    if (cp >= 0xFF10 && cp <= 0xFF19) return true;          // fullwidth digits
    if (cp >= 0xFF21 && cp <= 0xFF3A) return true;          // fullwidth A-Z
    if (cp >= 0xFF41 && cp <= 0xFF5A) return true;          // fullwidth a-z
    return false;
}

}  // namespace

bool is_exempt_line(const std::string& text) {
    if (text.empty()) return false;
    std::size_t i = 0;
    while (i < text.size())
        if (carries_meaning(utf8_next(text, i))) return false;
    return true;
}

CoverageReport count_coverage(const bj::object& translated,
                              const std::string& source_file) {
    CoverageReport report;
    report.source_file = source_file;
    for (const auto& kv : translated) {
        if (!kv.value().is_object()) continue;
        const auto& script = kv.value().get_object();
        const auto* strings = script.if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        CoverageCounts counts;
        for (const auto& ev : strings->get_array()) {
            if (!ev.is_object()) continue;
            const auto& entry = ev.get_object();
            const auto* text = entry.if_contains("text");
            if (!text || !text->is_string()) continue;
            const std::string jp(text->get_string());
            std::string en = jp;
            if (const auto* t = entry.if_contains("translated"))
                if (t->is_string()) en = std::string(t->get_string());
            ++counts.total;
            if (en != jp) ++counts.translated;
            else if (is_exempt_line(jp)) ++counts.exempt;
            else ++counts.identity;
        }
        report.per_script.emplace(std::string(kv.key()), counts);
        report.totals.translated += counts.translated;
        report.totals.identity += counts.identity;
        report.totals.exempt += counts.exempt;
        report.totals.total += counts.total;
    }
    return report;
}

std::string coverage_report_path(const std::string& counted_file) {
    const fs::path p = fs::u8path(counted_file);
    return (p.parent_path() / fs::u8path("coverage_report.json")).u8string();
}

void write_coverage_report(const CoverageReport& report, const std::string& out_file) {
    auto counts_json = [](const CoverageCounts& c) {
        bj::object o;
        o["translated"] = c.translated;
        o["identity"] = c.identity;
        o["exempt"] = c.exempt;
        o["total"] = c.total;
        o["translatable"] = c.translatable();
        return o;
    };
    bj::object scripts;
    for (const auto& [name, counts] : report.per_script)
        scripts[name] = counts_json(counts);
    bj::object root;
    root["source_file"] = report.source_file;
    root["totals"] = counts_json(report.totals);
    root["scripts"] = std::move(scripts);
    write_file_atomic_text(out_file, json_pretty(bj::value(std::move(root)), 2));
    log_info("Wrote " + out_file);
}

void log_coverage_summary(const CoverageReport& report) {
    const auto& t = report.totals;
    // Measured against translatable lines: an exempt line cannot change.
    const double pct = t.translatable() ? 100.0 * static_cast<double>(t.translated) /
                                              static_cast<double>(t.translatable())
                                        : 0.0;
    log_info("Coverage of " + report.source_file);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "  scripts %zu   translated %lld   identity %lld   exempt %lld   "
                  "total %lld   (%.2f%% of %lld translatable)",
                  report.per_script.size(), static_cast<long long>(t.translated),
                  static_cast<long long>(t.identity), static_cast<long long>(t.exempt),
                  static_cast<long long>(t.total), pct,
                  static_cast<long long>(t.translatable()));
    log_info(line);

    // The scripts still carrying the most Japanese, which is where a resumed
    // run picks up.
    std::vector<std::pair<std::string, CoverageCounts>> worst(report.per_script.begin(),
                                                              report.per_script.end());
    std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) {
        return a.second.identity > b.second.identity;
    });
    const std::size_t shown = std::min<std::size_t>(worst.size(), 5);
    for (std::size_t i = 0; i < shown; ++i) {
        if (worst[i].second.identity == 0) break;
        std::snprintf(line, sizeof(line), "  %-40s %lld of %lld untranslated",
                      worst[i].first.c_str(),
                      static_cast<long long>(worst[i].second.identity),
                      static_cast<long long>(worst[i].second.total));
        log_info(line);
    }
}

void report_coverage(const bj::object& document, const std::string& source_file) {
    const CoverageReport report = count_coverage(document, source_file);
    log_coverage_summary(report);
    write_coverage_report(report, coverage_report_path(source_file));
}

}  // namespace snt
