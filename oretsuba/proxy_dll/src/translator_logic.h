// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace oretsuba::proxy {

inline constexpr std::size_t kDefaultWrapColumns = 56;
inline constexpr std::size_t kDefaultScratchBufferCapacity = 4096;

class TranslationCatalog {
public:
    TranslationCatalog();
    explicit TranslationCatalog(std::size_t max_wrap_columns);
    TranslationCatalog(TranslationCatalog&& other) noexcept;
    TranslationCatalog& operator=(TranslationCatalog&& other) noexcept;
    TranslationCatalog(const TranslationCatalog&) = delete;
    TranslationCatalog& operator=(const TranslationCatalog&) = delete;
    ~TranslationCatalog();

    std::size_t LoadFromFile(const wchar_t* path);
    std::size_t LoadFromFile(const char* path);
    std::size_t LoadFromStream(std::istream& stream);
    std::size_t LoadFromString(std::string_view tsv_content);

    std::size_t Size() const noexcept;

    std::optional<std::string> LookupTranslation(std::string_view jp_key) const;
    std::optional<std::string> LookupSpeaker(std::string_view jp_speaker) const;
    const char* LookupPersistentPointer(std::string_view jp_key) const;

    std::string TranslateMessageRecord(std::string_view raw_script_str) const;

    bool FormatDialogueOrNarration(std::string_view raw_script_str,
                                   char* out_buf,
                                   std::size_t out_buf_capacity) const;

    static std::string WrapProportionalLines(
        std::string_view text,
        std::size_t max_columns = kDefaultWrapColumns);

    static std::string NormalizeLookupKey(std::string_view raw_key);
    static std::string Utf8ToCp932(std::string_view utf8_str);
    static std::string Cp932ToUtf8(std::string_view cp932_str);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace oretsuba::proxy
