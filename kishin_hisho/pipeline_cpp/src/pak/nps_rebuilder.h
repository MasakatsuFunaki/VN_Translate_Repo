// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Deterministic .nps script rebuilder, box-width word wrapper, inline XML tag
// restorer, and 0script.pak v4 archive builder for Kishin Hishou Demonbane.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pak/pak_archive.h"

namespace ksh::pak {

inline constexpr int kWideBoxColumnBudget = 56;
inline constexpr int kMediumBoxColumnBudget = 38;
inline constexpr int kNarrowBoxColumnBudget = 32;

// Strategy protocol for per-<BOX type="N"> visible column budgets and 4-line
// page splitting rules.
class BoxWidthPolicy {
public:
    virtual ~BoxWidthPolicy();

    virtual int ColumnBudgetForBox(int box_type) const noexcept = 0;
    virtual bool IsFourLinePaginatedBox(int box_type) const noexcept = 0;
};

// Standard Demonbane <BOX type="0..13"> column budget policy:
//   BOX 0, 9, 11..13 -> 56 columns
//   BOX 5..8, 10     -> 38 columns
//   BOX 1..4         -> 32 columns
class DefaultBoxWidthPolicy final : public BoxWidthPolicy {
public:
    DefaultBoxWidthPolicy() = default;
    ~DefaultBoxWidthPolicy() override;

    int ColumnBudgetForBox(int box_type) const noexcept override;
    bool IsFourLinePaginatedBox(int box_type) const noexcept override;
};

struct NpsTextPair {
    std::string first;
    std::string second;
};

struct NpsBlockMetadata {
    int box_type = 0;
    std::string first_indent;
    bool trailing_k = false;
    int k_clause_count = 1;
    std::vector<std::string> orig_k_clauses_clean;
    std::vector<NpsTextPair> ruby_list;
    std::vector<NpsTextPair> font_list;
    std::vector<NpsTextPair> wait_list;
    bool is_typewriter = false;
    bool has_complex_fx = false;
    std::string orig_clean_en;
    std::string orig_raw_en;
    std::string boundary_prefix_tag;
    std::string boundary_suffix_tag;
};

struct NpsBlockResult {
    std::string text;
    bool was_modified = false;
    bool was_rewrapped = false;
};

struct NpsRebuildStats {
    std::size_t total_nps_scripts = 0;
    std::size_t total_pak_files = 0;
    std::size_t total_blocks = 0;
    std::size_t modified_blocks = 0;
    std::size_t rewrapped_blocks = 0;
    std::size_t pak_size_bytes = 0;
};

// Visible column width of a UTF-8 .nps string (excluding XML markup tags;
// ASCII = 1 column, non-ASCII codepoints = 2 columns).
int ComputeVisibleColumns(std::string_view utf8_text);

// Splits an annotated clause on ASCII spaces outside of <r ...>...</r> and
// <...> tags so neither XML tags nor multi-word ruby spans are broken across
// line wraps.
std::vector<std::string> TokenizeForWrap(std::string_view annotated_clause);

// Restores <r text="...">base</r>, <FONT ...>inner</FONT>, <WAIT ...>, and
// -- -> U+2015 U+2015 on a single cleaned clause, consuming matched entries
// from ruby_list, font_list, and wait_list in place.
std::string ApplyInlineAnnotations(std::string_view clause,
                                   std::vector<NpsTextPair>& ruby_list,
                                   std::vector<NpsTextPair>& font_list,
                                   std::vector<NpsTextPair>& wait_list);

// Word-wraps an annotated clause into lines within `budget` visible columns.
std::vector<std::string> WrapAnnotatedClause(std::string_view annotated_clause,
                                             int budget,
                                             std::string_view first_indent,
                                             std::string_view cont_indent,
                                             bool attach_k);

// Thread-compatible .nps script rebuilder.
class NpsRebuilder {
public:
    NpsRebuilder();
    explicit NpsRebuilder(std::shared_ptr<const BoxWidthPolicy> width_policy);
    NpsRebuilder(const NpsRebuilder& other);
    NpsRebuilder(NpsRebuilder&& other) noexcept;
    NpsRebuilder& operator=(const NpsRebuilder& other);
    NpsRebuilder& operator=(NpsRebuilder&& other) noexcept;
    ~NpsRebuilder();

    const BoxWidthPolicy& WidthPolicy() const noexcept;

    NpsBlockResult ReconstructBlock(std::string_view current_en,
                                    const NpsBlockMetadata& meta) const;

    std::vector<PakEntry> BuildEntriesFromJsonFiles(
        const std::string& translated_json_path,
        const std::string& recon_map_json_path,
        NpsRebuildStats* out_stats = nullptr) const;

    NpsRebuildStats RebuildAndDeploy(
        const std::string& project_dir,
        const std::vector<std::string>& output_game_dirs,
        bool verify_archive = true) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ksh::pak
