// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace oretsuba::extract {

struct ExtractionSummary {
    std::size_t total_archive_entries = 0;
    std::size_t sob_file_count = 0;
    std::size_t sob0_count = 0;
    std::size_t sob1_count = 0;
    std::size_t sob2_count = 0;
    std::size_t scenario_script_count = 0;
    std::size_t message_count = 0;
    std::size_t voiced_dialogue_count = 0;
    std::size_t unvoiced_dialogue_count = 0;
    std::size_t voice_only_dialogue_count = 0;
    std::size_t narration_count = 0;
    std::size_t choice_block_count = 0;
    std::size_t choice_option_count = 0;
    std::size_t title_count = 0;
    std::size_t synopsis_count = 0;
    std::size_t ui_string_count = 0;
    std::size_t total_entry_count = 0;
    std::size_t unique_speaker_count = 0;
    std::size_t unique_jp_string_count = 0;
    std::size_t replacement_char_count = 0;
};

// Thread-compatible extractor that unpacks SCRIPT.LPK and writes extracted_text.json.
class Extractor {
public:
    Extractor();
    Extractor(const Extractor&) = delete;
    Extractor& operator=(const Extractor&) = delete;
    Extractor(Extractor&& other) noexcept;
    Extractor& operator=(Extractor&& other) noexcept;
    ~Extractor();

    ExtractionSummary ExtractArchive(const std::string& lpk_path,
                                     const std::string& output_json_path) const;

    int Run(const std::string& game_dir,
            const std::string& output_json_path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace oretsuba::extract
