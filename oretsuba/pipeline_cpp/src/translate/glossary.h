// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oretsuba::translate {

constexpr const char* kDefaultModel = "gemini-4-argon";
constexpr std::size_t kContextWindowLines = 80U;
constexpr std::size_t kContextAfterLines = 20U;
constexpr int kDefaultBatchSize = 100;

// Thread-compatible glossary holding the system prompt, speaker translations,
// and canonical terminology loaded from TONE_OF_NOVEL.md.
class Glossary {
public:
    Glossary();
    explicit Glossary(const std::string& tone_markdown_path);
    Glossary(const Glossary&) = delete;
    Glossary& operator=(const Glossary&) = delete;
    Glossary(Glossary&& other) noexcept;
    Glossary& operator=(Glossary&& other) noexcept;
    ~Glossary();

    bool LoadFromToneFile(const std::string& tone_markdown_path);

    const std::string& SystemPrompt() const;
    const std::string& NeutralSystemPrompt() const;
    const std::vector<std::pair<std::string, std::string>>& OrderedSpeakerTranslations() const;
    const std::unordered_map<std::string, std::string>& SpeakerTranslationMap() const;
    const std::vector<std::pair<std::string, std::string>>& OrderedTermTranslations() const;
    std::optional<std::string> LookupSpeaker(std::string_view jp_speaker) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const Glossary& DefaultGlossary();
const std::vector<std::pair<std::string, std::string>>& NameTranslationsOrdered();
const std::unordered_map<std::string, std::string>& NameTranslations();
const std::string& DefaultSystemPrompt();

}  // namespace oretsuba::translate
