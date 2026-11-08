// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace oretsuba::build_tsv {

constexpr std::size_t kDefaultWrapColumns = 56U;

struct TsvBuildSummary {
    std::size_t entry_count = 0;
    std::size_t speaker_entry_count = 0;
    std::size_t cache_fill_count = 0;
    std::size_t conflict_count = 0;
};

// Thread-compatible builder that generates translation_table.tsv (<jp>\t<en>\n)
// from translated_text.json, translation_cache_gemini.json, and speaker glossary.
class TsvBuilder {
public:
    TsvBuilder();
    explicit TsvBuilder(std::size_t max_wrap_columns);
    TsvBuilder(const TsvBuilder&) = delete;
    TsvBuilder& operator=(const TsvBuilder&) = delete;
    TsvBuilder(TsvBuilder&& other) noexcept;
    TsvBuilder& operator=(TsvBuilder&& other) noexcept;
    ~TsvBuilder();

    TsvBuildSummary Build(const std::string& translated_file,
                          const std::string& cache_file,
                          const std::string& out_tsv) const;

    int Run(const std::string& translated_file,
            const std::string& cache_file,
            const std::string& out_tsv) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string StripInlineControlWrappers(std::string_view text);
std::string FlattenText(std::string_view text);
std::string NormalizeTsvKey(std::string_view jp_text);
std::string SanitizeTsvValue(std::string_view en_text);
std::string EscapeForTsv(std::string_view text);
std::string WrapEnglishLine(std::string_view text,
                            std::size_t max_columns = kDefaultWrapColumns);

int RunBuild(const std::string& translated_file,
             const std::string& cache_file,
             const std::string& out_tsv);

}  // namespace oretsuba::build_tsv
