// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Full-novel statistical & discourse hotspot detector (PIMPL-insulated).
//
// Combines literature-backed mathematical and document-level discourse signals:
//   1. Gale & Church (1993) + Pellegrino et al. (2011) Script-Aware
//      Information-Load Residual (Z_info) with generous literary leeway deadband
//   2. Artifact, inline romaji gloss, and spoken-quote mismatch scoring
//   3. Six Document-Level JP->EN Discourse Trap Signals (Voita et al. ACL 2019;
//      Bawden et al. NAACL 2018; Nakaiwa & Ikehara 1992; Kinsui 2003;
//      Jiang et al. NAACL 2022 BlonDe):
//      - Directional benefactive (てくれる/てもらう) & overt 1st-person contradiction
//      - Question-Answer adjacency pair & elliptical verb-echo mismatch
//      - BlonDe 5-turn cross-line kanji lexical cohesion hook-and-eye check
//      - Kinsui role-language (Yakuwarigo) & per-speaker EWMA register drift
//   4. Volansky et al. (2015) / Koppel & Ordan (2011) / Toral (2019)
//      Translationese, Yule's K repetition degeneracy & Shannon entropy divergence
//   5. Page's CUSUM (1954) Sequential Change-Point Detection + Gaussian KDE
//      with Non-Maximum Suppression (NMS) and Hybrid Adaptive Contagion.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace spot_checker {

struct ScriptEntry;
struct Improvement;

struct OlsModel {
    double alpha = 2.0;
    double beta = 1.25;
    double sigma = 14.0;
    double mean_info_bits = 24.0;
};

struct LineWeaknessScore {
    std::size_t global_index = 0;
    double length_z = 0.0;          // Gale-Church script-weighted information-load Z_info
    double artifact_score = 0.0;    // Leftover JP punctuation, romaji notes, quote mismatch
    double coupling_score = 0.0;    // Discourse traps: Q-A echo, 5-turn BlonDe cohesion, benefactive, yakuwarigo
    double style_score = 0.0;       // Translationese calques, Yule's K, entropy, Westernization/censoring
    double raw_total = 0.0;         // Combined per-line weakness s_i
    double cusum_stat = 0.0;        // Page's (1954) sequential change-point CUSUM statistic S_i
    double smoothed_density = 0.0;  // Gaussian KDE density D(i)
    std::string dominant_signal;    // Human-readable label of largest contributor
};

struct HotspotWindow {
    std::size_t offset = 0;         // Start global_index
    std::size_t length = 0;         // Window size W
    double energy = 0.0;            // Sum of D(j) in window
    double cusum_peak = 0.0;        // Maximum Page's CUSUM S_j inside window
    double z_score = 0.0;           // Standardized window score combining KDE Z + CUSUM change-point
    std::size_t peak_index = 0;     // Line with highest raw_total inside window
    double peak_line_score = 0.0;   // raw_total at peak_index
    std::string dominant_signal;    // Dominant signal inside window
};

struct HotspotConfig {
    std::size_t window_size = 40;
    double z_threshold = 1.5;
    double adaptive_defect_rate = 0.12;
    std::size_t boundary_margin = 5;
    std::size_t cohesion_lookback = 5;  // BlonDe W_lex = 5 turns
    double cusum_drift_k = 0.5;         // Page's CUSUM reference drift allowance
    bool skip_h_scenes = true;          // Skip explicit H-scene lines and windows by default
    bool fix_punctuation = false;       // Do not score punctuation/parentheses/brackets by default
    std::size_t max_h_lines_per_window = 3;
};

struct NeighborExpansion {
    bool expand_left = false;
    std::size_t left_offset = 0;
    std::size_t left_length = 0;
    bool expand_right = false;
    std::size_t right_offset = 0;
    std::size_t right_length = 0;
};

// Computes Pellegrino et al. (2011) script-weighted source information load B_S(S):
// Kanji = 2.4 bits, Katakana = 1.4 bits, Hiragana/ASCII = 1.0 bit.
double compute_script_info_bits(const std::string& jp_body);

// Returns true if the JP/EN line contains explicit H-scene anatomical/sexual or
// phonetic moan/fluid markers.
bool is_h_scene_line(const std::string& jp_text, const std::string& en_text);

class HotspotDetector {
public:
    explicit HotspotDetector(HotspotConfig config = {});
    ~HotspotDetector();

    HotspotDetector(HotspotDetector&& other) noexcept;
    HotspotDetector& operator=(HotspotDetector&& other) noexcept;

    HotspotDetector(const HotspotDetector&) = delete;
    HotspotDetector& operator=(const HotspotDetector&) = delete;

    // Fit Gale-Church / OLS information-load regression L_en = alpha + beta * B_S(jp)
    OlsModel fit_length_regression(const std::vector<ScriptEntry>& entries) const;

    // Evaluate intrinsic single-line weakness score s(jp, en) against an OLS model.
    // Used both during full-novel scoring and by the Do-No-Harm post-edit validator.
    LineWeaknessScore score_single_line(
        const std::string& jp_text,
        const std::string& en_text,
        const OlsModel& ols = {}) const;

    // Score every line in the novel (including 5-turn discourse cohesion, Q-A adjacency,
    // per-speaker register EWMA, Page's CUSUM, and Gaussian KDE smoothed density).
    std::vector<LineWeaknessScore> score_entries(
        const std::vector<ScriptEntry>& entries) const;

    // Rank disjoint hotspot windows via Gaussian KDE + Page's CUSUM + NMS.
    std::vector<HotspotWindow> select_hotspots(
        const std::vector<ScriptEntry>& entries,
        std::size_t max_windows = 0,
        std::vector<LineWeaknessScore>* out_scores = nullptr) const;

    // Hybrid adaptive contagion: given a checked window [window_offset, window_offset + window_len)
    // and the LLM improvements discovered in it, determine whether adjacent windows should be queued.
    NeighborExpansion evaluate_contagion(
        std::size_t window_offset,
        std::size_t window_len,
        std::size_t total_entries,
        const std::vector<Improvement>& window_improvements) const;

    const HotspotConfig& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spot_checker
