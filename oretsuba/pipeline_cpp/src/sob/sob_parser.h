// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/util.h"

namespace oretsuba::sob {

constexpr std::uint32_t kSyscallMessage = 462U;
constexpr std::uint32_t kSyscallChoice = 491U;
constexpr std::uint32_t kExtFuncSceneTitle = 0x1C8D0U;
constexpr std::uint32_t kGlobalVarSynopsis = 0x035DU;

enum class SobFormat {
    kSob0,
    kSob1,
    kSob2,
};

enum class SobEntryType {
    kDialogue,
    kNarration,
    kChoice,
    kTitle,
    kSynopsis,
    kUi,
};

std::string_view ToFormatString(SobFormat format);
std::string_view ToEntryTypeString(SobEntryType type);

struct ParsedMessageFields {
    SobEntryType type = SobEntryType::kNarration;
    std::string speaker;
    std::string voice_id;
    std::string text;
};

ParsedMessageFields ParseMessageRecordString(std::string_view raw_str);

struct SobStringRecord {
    SobEntryType type = SobEntryType::kNarration;
    std::uint32_t offset = 0;
    std::uint32_t code_offset = 0;
    std::uint8_t block_id = 0;
    std::uint16_t record_index = 0;
    std::uint8_t option_index = 0;
    std::string speaker;
    std::string voice_id;
    std::string text;
    std::string raw;
    bool has_jp = false;
};

struct SobParsedScript {
    std::string script_name;
    SobFormat format = SobFormat::kSob0;
    std::size_t file_size = 0;
    std::uint32_t code_size = 0;
    std::uint32_t total_size = 0;
    bool has_messages = false;
    std::size_t choice_block_count = 0;
    std::vector<SobStringRecord> records;
};

// Thread-compatible parser for Lucifen SOB0, SOB1, and SOB2 script binaries.
class SobScriptParser {
public:
    SobScriptParser();
    SobScriptParser(const SobScriptParser&) = delete;
    SobScriptParser& operator=(const SobScriptParser&) = delete;
    SobScriptParser(SobScriptParser&& other) noexcept;
    SobScriptParser& operator=(SobScriptParser&& other) noexcept;
    ~SobScriptParser();

    SobParsedScript Parse(std::string_view script_name, const Bytes& data) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace oretsuba::sob
