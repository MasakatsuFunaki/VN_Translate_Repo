// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <boost/json.hpp>

#include "translate/glossary.h"

namespace oretsuba::translate {

struct SpeakerCheckReport {
    bool passed = false;
    int failed_check = 0;
    std::string reason;
    std::size_t corpus_speakers = 0;
    std::size_t glossary_entries = 0;
    std::size_t inline_tag_lines = 0;
};

struct InlineControlDecomposition {
    std::string leading_control_prefix;
    std::string clean_text;
    bool has_midline_pause = false;
    bool has_inline_linebreak = false;
};

// Thread-compatible validator for speaker glossary coverage and inline
// Lucifen control-code preservation (\, $&, <N>\, <"base|ruby">).
class SpeakerGate {
public:
    SpeakerGate();
    SpeakerGate(const SpeakerGate&) = delete;
    SpeakerGate& operator=(const SpeakerGate&) = delete;
    SpeakerGate(SpeakerGate&& other) noexcept;
    SpeakerGate& operator=(SpeakerGate&& other) noexcept;
    ~SpeakerGate();

    SpeakerCheckReport ValidateCorpus(const boost::json::value& extracted_root,
                                      const Glossary& glossary) const;

    SpeakerCheckReport ValidateExtractedFile(const std::string& extracted_json_path,
                                             const Glossary& glossary) const;

    InlineControlDecomposition DecomposeInlineControls(std::string_view text) const;

    std::string PreserveInlineControls(std::string_view source_jp,
                                       std::string_view translated_en) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

SpeakerCheckReport RunSpeakerChecks(const std::string& extracted_json_path,
                                    const Glossary& glossary = DefaultGlossary());

}  // namespace oretsuba::translate
