// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "sob/sob_parser.h"

#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace oretsuba::sob {

namespace {

constexpr std::size_t kMagicBytes = 4U;
constexpr std::size_t kDwordBytes = 4U;
constexpr std::size_t kWordBytes = 2U;
constexpr std::size_t kRelocPairBytes = 8U;
constexpr std::size_t kSobHeaderPrefixBytes = 8U;
constexpr std::size_t kSobCodeAndTotalSizeBytes = 8U;

constexpr const char kMagicSob0[kMagicBytes] = {'S', 'O', 'B', '0'};
constexpr const char kMagicSob1[kMagicBytes] = {'S', 'O', 'B', '1'};
constexpr const char kMagicSob2[kMagicBytes] = {'S', 'O', 'B', '2'};

constexpr std::size_t kSubtableGlobalVar = 0U;
constexpr std::size_t kSubtableSyscall = 1U;
constexpr std::size_t kSubtableExtFunc = 3U;
constexpr std::size_t kSubtableSelfOffset = 4U;
constexpr std::size_t kSubtableCount = 5U;

constexpr std::uint32_t kBacktrackStartBytes = 4U;
constexpr std::uint32_t kBacktrackLimitBytes = 32U;
constexpr std::uint32_t kBacktrackStepBytes = 4U;

constexpr std::size_t kMsgRecordHeaderBytes = 3U;
constexpr std::size_t kMsgIndexFieldOffset = 1U;

constexpr std::size_t kChoiceBlockHeaderBytes = 5U;
constexpr std::size_t kChoiceIdFieldOffset = 1U;
constexpr std::size_t kChoiceCountFieldOffset = 3U;
constexpr std::size_t kChoiceOptionHeaderBytes = 2U;

constexpr std::size_t kMinUiStringBytes = 1U;
constexpr std::size_t kMaxUiStringBytes = 400U;

constexpr std::size_t kSob1VarDescriptorBytes = 10U;

constexpr std::string_view kHeaderBodyDelimiter = "$&";

std::uint32_t ReadU32Le(const std::uint8_t* ptr) {
    std::uint32_t val = 0;
    std::memcpy(&val, ptr, sizeof(val));
    return val;
}

std::uint16_t ReadU16Le(const std::uint8_t* ptr) {
    std::uint16_t val = 0;
    std::memcpy(&val, ptr, sizeof(val));
    return val;
}

std::size_t FindNullTerminator(const std::uint8_t* buf, std::size_t buf_size, std::size_t start) {
    for (std::size_t i = start; i < buf_size; ++i) {
        if (buf[i] == 0U) {
            return i;
        }
    }
    return buf_size;
}

std::string ReadNullTerminatedCp932(const std::uint8_t* buf,
                                    std::size_t buf_size,
                                    std::size_t start) {
    if (start >= buf_size) {
        return {};
    }
    const std::size_t end = FindNullTerminator(buf, buf_size, start);
    return Cp932ToUtf8(buf + start, end - start);
}

}  // namespace

std::string_view ToFormatString(SobFormat format) {
    switch (format) {
    case SobFormat::kSob0:
        return "SOB0";
    case SobFormat::kSob1:
        return "SOB1";
    case SobFormat::kSob2:
        return "SOB2";
    }
    return "SOB0";
}

std::string_view ToEntryTypeString(SobEntryType type) {
    switch (type) {
    case SobEntryType::kDialogue:
        return "dialogue";
    case SobEntryType::kNarration:
        return "narration";
    case SobEntryType::kChoice:
        return "choice";
    case SobEntryType::kTitle:
        return "title";
    case SobEntryType::kSynopsis:
        return "synopsis";
    case SobEntryType::kUi:
        return "ui";
    }
    return "narration";
}

ParsedMessageFields ParseMessageRecordString(std::string_view raw_str) {
    ParsedMessageFields out;
    if (raw_str.empty() || raw_str.front() != '#') {
        out.type = SobEntryType::kNarration;
        std::string_view body = raw_str;
        if (!body.empty() && body.back() == '$') {
            body.remove_suffix(1U);
        }
        out.text = std::string(body);
        return out;
    }

    out.type = SobEntryType::kDialogue;
    std::string_view header;
    std::string_view body;

    const std::size_t sep_idx = raw_str.find(kHeaderBodyDelimiter);
    if (sep_idx != std::string_view::npos) {
        header = raw_str.substr(1U, sep_idx - 1U);
        body = raw_str.substr(sep_idx + kHeaderBodyDelimiter.size());
    }
    if (sep_idx == std::string_view::npos) {
        const std::size_t dollar_idx = raw_str.find('$');
        if (dollar_idx != std::string_view::npos) {
            header = raw_str.substr(1U, dollar_idx - 1U);
            body = raw_str.substr(dollar_idx + 1U);
        }
        if (dollar_idx == std::string_view::npos) {
            header = raw_str.substr(1U);
        }
    }

    if (!body.empty() && body.back() == '$') {
        body.remove_suffix(1U);
    }

    const std::size_t backslash_idx = header.find('\\');
    if (backslash_idx != std::string_view::npos) {
        out.speaker = std::string(header.substr(0U, backslash_idx));
        out.voice_id = std::string(header.substr(backslash_idx + 1U));
    }
    if (backslash_idx == std::string_view::npos) {
        out.speaker = std::string(header);
    }

    out.text = std::string(body);
    return out;
}

struct SobScriptParser::Impl {
    SobParsedScript ParseSob1(std::string_view script_name, const Bytes& data) const {
        if (data.size() < kSobHeaderPrefixBytes) {
            throw std::runtime_error("SOB1 file smaller than header: " + std::string(script_name));
        }
        const std::uint32_t data_size = ReadU32Le(data.data() + kMagicBytes);
        std::size_t pos = kSobHeaderPrefixBytes + static_cast<std::size_t>(data_size);
        if (pos + kDwordBytes > data.size()) {
            throw std::runtime_error("SOB1 var_count out of bounds: " + std::string(script_name));
        }

        const std::uint32_t var_count = ReadU32Le(data.data() + pos);
        pos += kDwordBytes + static_cast<std::size_t>(var_count) * kSob1VarDescriptorBytes;
        if (pos + kDwordBytes > data.size()) {
            throw std::runtime_error("SOB1 reloc_count out of bounds: " + std::string(script_name));
        }

        const std::uint32_t reloc_count = ReadU32Le(data.data() + pos);
        pos += kDwordBytes;
        if (pos + static_cast<std::size_t>(reloc_count) * kRelocPairBytes > data.size()) {
            throw std::runtime_error("SOB1 reloc table out of bounds: " + std::string(script_name));
        }

        SobParsedScript parsed;
        parsed.script_name = std::string(script_name);
        parsed.format = SobFormat::kSob1;
        parsed.file_size = data.size();
        parsed.code_size = 0U;
        parsed.total_size = data_size;

        const std::size_t pool_end = kSobHeaderPrefixBytes + static_cast<std::size_t>(data_size);
        for (std::uint32_t i = 0; i < reloc_count; ++i) {
            const std::uint32_t ptr_off = ReadU32Le(data.data() + pos);
            const std::uint32_t target_off = ReadU32Le(data.data() + pos + kDwordBytes);
            pos += kRelocPairBytes;

            const std::size_t file_target = kSobHeaderPrefixBytes + static_cast<std::size_t>(target_off);
            if (file_target >= pool_end) {
                continue;
            }
            const std::size_t nul_pos = FindNullTerminator(data.data(), pool_end, file_target);
            if (nul_pos >= pool_end || nul_pos == file_target) {
                continue;
            }
            const auto decoded = Cp932ToUtf8Strict(data.data() + file_target, nul_pos - file_target);
            if (!decoded || !HasSobJapanese(*decoded) || !IsPrintableScriptText(*decoded)) {
                continue;
            }

            SobStringRecord rec;
            rec.type = SobEntryType::kUi;
            rec.offset = static_cast<std::uint32_t>(file_target);
            rec.code_offset = ptr_off;
            rec.text = *decoded;
            rec.raw = *decoded;
            rec.has_jp = true;
            parsed.records.push_back(std::move(rec));
        }

        return parsed;
    }

    SobParsedScript ParseSob0OrSob2(std::string_view script_name,
                                    SobFormat format,
                                    const Bytes& data) const {
        if (data.size() < kSobHeaderPrefixBytes) {
            throw std::runtime_error("SOB header truncated: " + std::string(script_name));
        }
        const std::uint32_t reloc_size = ReadU32Le(data.data() + kMagicBytes);
        const std::size_t reloc_end = kSobHeaderPrefixBytes + static_cast<std::size_t>(reloc_size);
        const std::size_t payload_start = reloc_end + kSobCodeAndTotalSizeBytes;
        if (payload_start > data.size()) {
            throw std::runtime_error("SOB relocation table exceeds file size: " + std::string(script_name));
        }

        using RelocPair = std::pair<std::uint32_t, std::uint32_t>;
        std::vector<RelocPair> subtables[kSubtableCount];
        std::size_t pos = kSobHeaderPrefixBytes;

        for (std::size_t t = 0; t < kSubtableCount; ++t) {
            if (pos + kDwordBytes > reloc_end) {
                throw std::runtime_error("SOB subtable count truncated: " + std::string(script_name));
            }
            const std::uint32_t count = ReadU32Le(data.data() + pos);
            pos += kDwordBytes;
            if (pos + static_cast<std::size_t>(count) * kRelocPairBytes > reloc_end) {
                throw std::runtime_error("SOB subtable entries truncated: " + std::string(script_name));
            }
            subtables[t].reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t code_off = ReadU32Le(data.data() + pos);
                const std::uint32_t target_val = ReadU32Le(data.data() + pos + kDwordBytes);
                pos += kRelocPairBytes;
                subtables[t].emplace_back(code_off, target_val);
            }
        }

        const std::uint32_t code_size = ReadU32Le(data.data() + reloc_end);
        const std::uint32_t total_size = ReadU32Le(data.data() + reloc_end + kDwordBytes);
        const std::uint8_t* payload = data.data() + payload_start;
        const std::size_t payload_size = data.size() - payload_start;

        std::unordered_map<std::uint32_t, std::uint32_t> t4_map;
        t4_map.reserve(subtables[kSubtableSelfOffset].size());
        for (const auto& [code_off, target] : subtables[kSubtableSelfOffset]) {
            t4_map[code_off] = target;
        }

        SobParsedScript parsed;
        parsed.script_name = std::string(script_name);
        parsed.format = format;
        parsed.file_size = data.size();
        parsed.code_size = code_size;
        parsed.total_size = total_size;

        std::set<std::uint32_t> msg_ptrs_used;
        std::set<std::uint32_t> choice_ptrs_used;
        std::set<std::uint32_t> title_ptrs_used;
        std::set<std::uint32_t> synopsis_ptrs_used;

        for (const auto& [code_off, ext_off] : subtables[kSubtableExtFunc]) {
            if (ext_off != kExtFuncSceneTitle) {
                continue;
            }
            for (std::uint32_t back = kBacktrackStartBytes;
                 back < kBacktrackLimitBytes;
                 back += kBacktrackStepBytes) {
                if (code_off < back) {
                    continue;
                }
                const auto it = t4_map.find(code_off - back);
                if (it == t4_map.end()) {
                    continue;
                }
                const std::uint32_t rec_off = it->second;
                if (rec_off >= code_size && rec_off < payload_size &&
                    title_ptrs_used.insert(rec_off).second) {
                    const std::string title_str =
                        ReadNullTerminatedCp932(payload, payload_size, rec_off);
                    SobStringRecord rec;
                    rec.type = SobEntryType::kTitle;
                    rec.offset = rec_off;
                    rec.code_offset = code_off;
                    rec.text = title_str;
                    rec.raw = title_str;
                    rec.has_jp = HasSobJapanese(title_str);
                    parsed.records.push_back(std::move(rec));
                }
                break;
            }
        }

        for (const auto& [code_off, gvar] : subtables[kSubtableGlobalVar]) {
            if (gvar != kGlobalVarSynopsis || code_off < kDwordBytes) {
                continue;
            }
            const auto it = t4_map.find(code_off - static_cast<std::uint32_t>(kDwordBytes));
            if (it == t4_map.end()) {
                continue;
            }
            const std::uint32_t rec_off = it->second;
            if (rec_off >= code_size && rec_off < payload_size &&
                synopsis_ptrs_used.insert(rec_off).second) {
                const std::string syn_str =
                    ReadNullTerminatedCp932(payload, payload_size, rec_off);
                SobStringRecord rec;
                rec.type = SobEntryType::kSynopsis;
                rec.offset = rec_off;
                rec.code_offset = code_off;
                rec.text = syn_str;
                rec.raw = syn_str;
                rec.has_jp = HasSobJapanese(syn_str);
                parsed.records.push_back(std::move(rec));
            }
        }

        for (const auto& [code_off, sys_id] : subtables[kSubtableSyscall]) {
            if (sys_id == kSyscallMessage) {
                for (std::uint32_t back = kBacktrackStartBytes;
                     back < kBacktrackLimitBytes;
                     back += kBacktrackStepBytes) {
                    if (code_off < back) {
                        continue;
                    }
                    const auto it = t4_map.find(code_off - back);
                    if (it == t4_map.end()) {
                        continue;
                    }
                    const std::uint32_t rec_off = it->second;
                    if (rec_off >= code_size &&
                        rec_off + kMsgRecordHeaderBytes <= payload_size &&
                        msg_ptrs_used.insert(rec_off).second) {
                        parsed.has_messages = true;
                        const std::uint8_t block_id = payload[rec_off];
                        const std::uint16_t msg_idx = ReadU16Le(payload + rec_off + kMsgIndexFieldOffset);
                        const std::string raw_str = ReadNullTerminatedCp932(
                            payload, payload_size, rec_off + kMsgRecordHeaderBytes);
                        ParsedMessageFields fields = ParseMessageRecordString(raw_str);

                        SobStringRecord rec;
                        rec.type = fields.type;
                        rec.offset = rec_off;
                        rec.code_offset = code_off;
                        rec.block_id = block_id;
                        rec.record_index = msg_idx;
                        rec.speaker = std::move(fields.speaker);
                        rec.voice_id = std::move(fields.voice_id);
                        rec.text = std::move(fields.text);
                        rec.raw = raw_str;
                        rec.has_jp = HasSobJapanese(rec.text);
                        parsed.records.push_back(std::move(rec));
                    }
                    break;
                }
                continue;
            }

            if (sys_id == kSyscallChoice) {
                for (std::uint32_t back = kBacktrackStartBytes;
                     back < kBacktrackLimitBytes;
                     back += kBacktrackStepBytes) {
                    if (code_off < back) {
                        continue;
                    }
                    const auto it = t4_map.find(code_off - back);
                    if (it == t4_map.end()) {
                        continue;
                    }
                    const std::uint32_t rec_off = it->second;
                    if (rec_off >= code_size &&
                        rec_off + kChoiceBlockHeaderBytes <= payload_size &&
                        choice_ptrs_used.insert(rec_off).second) {
                        ++parsed.choice_block_count;
                        const std::uint8_t c_block = payload[rec_off];
                        const std::uint16_t c_idx = ReadU16Le(payload + rec_off + kChoiceIdFieldOffset);
                        const std::uint8_t num_choices = payload[rec_off + kChoiceCountFieldOffset];
                        std::size_t cur = rec_off + kChoiceBlockHeaderBytes;

                        for (std::uint8_t opt_i = 0; opt_i < num_choices; ++opt_i) {
                            if (cur + kChoiceOptionHeaderBytes > payload_size) {
                                break;
                            }
                            const std::uint8_t stride = payload[cur];
                            const std::string opt_str = ReadNullTerminatedCp932(
                                payload, payload_size, cur + kChoiceOptionHeaderBytes);

                            SobStringRecord rec;
                            rec.type = SobEntryType::kChoice;
                            rec.offset = static_cast<std::uint32_t>(cur);
                            rec.code_offset = code_off;
                            rec.block_id = c_block;
                            rec.record_index = c_idx;
                            rec.option_index = opt_i;
                            rec.text = opt_str;
                            rec.raw = opt_str;
                            rec.has_jp = HasSobJapanese(opt_str);
                            parsed.records.push_back(std::move(rec));

                            if (stride == 0U) {
                                break;
                            }
                            cur += stride;
                        }
                    }
                    break;
                }
            }
        }

        for (const auto& [code_off, target] : subtables[kSubtableSelfOffset]) {
            if (target < code_size || target >= payload_size) {
                continue;
            }
            const bool already_claimed =
                msg_ptrs_used.count(target) > 0 ||
                choice_ptrs_used.count(target) > 0 ||
                title_ptrs_used.count(target) > 0 ||
                synopsis_ptrs_used.count(target) > 0;
            if (already_claimed) {
                continue;
            }
            const std::size_t nul_pos = FindNullTerminator(payload, payload_size, target);
            if (nul_pos >= payload_size) {
                continue;
            }
            const std::size_t byte_len = nul_pos - target;
            if (byte_len < kMinUiStringBytes || byte_len > kMaxUiStringBytes) {
                continue;
            }
            const auto decoded = Cp932ToUtf8Strict(payload + target, byte_len);
            if (!decoded || !HasSobJapanese(*decoded) || !IsPrintableScriptText(*decoded)) {
                continue;
            }

            SobStringRecord rec;
            rec.type = SobEntryType::kUi;
            rec.offset = target;
            rec.code_offset = code_off;
            rec.text = *decoded;
            rec.raw = *decoded;
            rec.has_jp = true;
            parsed.records.push_back(std::move(rec));
        }

        return parsed;
    }
};

SobScriptParser::SobScriptParser() : impl_(std::make_unique<Impl>()) {}
SobScriptParser::SobScriptParser(SobScriptParser&& other) noexcept = default;
SobScriptParser& SobScriptParser::operator=(SobScriptParser&& other) noexcept = default;
SobScriptParser::~SobScriptParser() = default;

SobParsedScript SobScriptParser::Parse(std::string_view script_name, const Bytes& data) const {
    if (data.size() < kMagicBytes) {
        throw std::runtime_error("SOB buffer shorter than 4-byte magic: " + std::string(script_name));
    }
    if (std::memcmp(data.data(), kMagicSob0, kMagicBytes) == 0) {
        return impl_->ParseSob0OrSob2(script_name, SobFormat::kSob0, data);
    }
    if (std::memcmp(data.data(), kMagicSob2, kMagicBytes) == 0) {
        return impl_->ParseSob0OrSob2(script_name, SobFormat::kSob2, data);
    }
    if (std::memcmp(data.data(), kMagicSob1, kMagicBytes) == 0) {
        return impl_->ParseSob1(script_name, data);
    }
    throw std::runtime_error("Unsupported SOB magic in " + std::string(script_name));
}

}  // namespace oretsuba::sob
