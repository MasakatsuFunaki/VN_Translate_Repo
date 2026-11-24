// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/menu_catalog.h"

#include "bgi/bgi_archive.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace snu::menu {
namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;

constexpr char kCompiledScriptMagic[] = "BurikoCompiledScriptVer1.00";
constexpr std::size_t kCompiledScriptMagicLen = sizeof(kCompiledScriptMagic) - 1u;
constexpr std::size_t kScriptHeaderBaseBytes = 0x1Cu;
constexpr std::size_t kScriptHeaderExtraOffset = 0x1Cu;
constexpr std::size_t kDwordBytes = 4u;
constexpr std::uint8_t kEndOfCodeMarker[kDwordBytes] = {0x1Bu, 0x00u, 0x00u, 0x00u};

constexpr std::uint32_t kOpPushInt = 0x00u;
constexpr std::uint32_t kOpPushCodeAddr = 0x01u;
constexpr std::uint32_t kOpPushVarRef = 0x02u;
constexpr std::uint32_t kOpPushStrAddr = 0x03u;
constexpr std::uint32_t kOpLoad = 0x08u;
constexpr std::uint32_t kOpStore = 0x09u;
constexpr std::uint32_t kOpStoreDestAfter = 0x0Au;
constexpr std::uint32_t kOpJumpIfFalse = 0x19u;
constexpr std::uint32_t kOpLineRecordVariant = 0x7Eu;
constexpr std::uint32_t kOpLineRecord = 0x7Fu;

constexpr std::size_t kDefaultInstructionDwords = 1u;
constexpr std::size_t kTwoDwordInstruction = 2u;
constexpr std::size_t kThreeDwordInstruction = 3u;

constexpr std::string_view kSelectPrefix = "select";
constexpr char kIdeographicSpaceUtf8[] = "　";
constexpr std::size_t kIdeographicSpaceBytes = sizeof(kIdeographicSpaceUtf8) - 1u;

constexpr std::array<SplashScreenSpec, 2> kSplashScreens = {{
    {
        kSplash1EntryName,
        {{
            {
                "This work is not for sale to persons under the age of 18.\n"
                "(Purchase by anyone under 18 is strictly prohibited.)",
                RectI{125, 72, 995, 93},
            },
            {
                "This work is a work of fiction.\n"
                "All characters, organizations, places, and events depicted herein\n"
                "are fictitious and bear no relation to any real person or entity.\n"
                "This work does not condone or encourage any criminal act.\n"
                "All romanceable characters depicted in this work are 18 or older.",
                RectI{125, 208, 1035, 232},
            },
            {
                "This product is intended for sale in Japan only.\n"
                "Technical support is not provided outside Japan.",
                RectI{125, 482, 560, 132},
            },
        }},
    },
    {
        kSplash2EntryName,
        {{
            {
                "In recent years, offenses involving e-mail and file-sharing software\n"
                "have become increasingly widespread.",
                RectI{125, 75, 995, 90},
            },
            {
                "All video, artwork, audio, and other content included in this work\n"
                "(including packaging and printed materials) are legally protected\n"
                "under copyright law.",
                RectI{125, 212, 1035, 145},
            },
            {
                "Unauthorized reproduction, distribution, rental, adaptation,\n"
                "modification, or public transmission is strictly prohibited by law.",
                RectI{125, 392, 965, 92},
            },
        }},
    },
}};

constexpr std::array<ConfigSettingRowSpec, kConfigLeftRowCount> kConfigLeftRows = {{
    {"Text Speed", true, "Slow", "Fast"},
    {"Auto-Read Speed", true, "Slow", "Fast"},
    {"Window Opacity", true, "Clear", "Opaque"},
    {"Display Mode", false, "", ""},
    {"Skip Mode", false, "", ""},
    {"Skip After Choices", false, "", ""},
    {"Auto After Choices", false, "", ""},
    {"Instant Transitions", false, "", ""},
    {"Keep Voice on Click", false, "", ""},
}};

constexpr std::array<ConfigSettingRowSpec, kConfigRightVolumeRowCount>
    kConfigRightVolumeRows = {{
        {"Master Volume", true, "Mute", "Max"},
        {"BGM Volume", true, "Mute", "Max"},
        {"SE Volume", true, "Mute", "Max"},
        {"Voice Volume", true, "Mute", "Max"},
    }};

constexpr std::array<ConfigToggleStripSpec, kConfigToggleStripCount>
    kConfigToggleStrips = {{
        {"SGCnfgWnd010000", "Windowed", ConfigToggleTheme::kLavenderInactiveCell0,
         "SGCnfgWnd030000"},
        {"SGCnfgWnd010100", "Fullscreen", ConfigToggleTheme::kSkyBlueActiveCell0,
         "SGCnfgWnd030100"},
        {"SGCnfgWnd020000", "Read Only", ConfigToggleTheme::kSkyBlueActiveCell0,
         "SGCnfgWnd030100"},
        {"SGCnfgWnd020100", "All Text", ConfigToggleTheme::kLavenderInactiveCell0,
         "SGCnfgWnd030000"},
        {"SGCnfgWnd030000", "Off", ConfigToggleTheme::kLavenderInactiveCell0,
         "SGCnfgWnd020100"},
        {"SGCnfgWnd030100", "On", ConfigToggleTheme::kSkyBlueActiveCell0,
         "SGCnfgWnd020000"},
        {"SGCnfgWnd040000", "Off", ConfigToggleTheme::kLavenderInactiveCell0,
         "SGCnfgWnd020100"},
        {"SGCnfgWnd040100", "On", ConfigToggleTheme::kSkyBlueActiveCell0,
         "SGCnfgWnd020000"},
        {"SGCnfgWnd050000", "Off", ConfigToggleTheme::kLavenderInactiveCell0,
         "SGCnfgWnd020100"},
        {"SGCnfgWnd050100", "On", ConfigToggleTheme::kSkyBlueActiveCell0,
         "SGCnfgWnd020000"},
        {"SGCnfgWnd060000", "Off", ConfigToggleTheme::kLavenderInactiveCell0,
         "SGCnfgWnd020100"},
        {"SGCnfgWnd060100", "On", ConfigToggleTheme::kSkyBlueActiveCell0,
         "SGCnfgWnd020000"},
    }};

constexpr std::array<ConfigVoicePortraitSpec, kConfigVoicePortraitCount>
    kConfigVoicePortraits = {{
        {"SGCnfgWnd100000", "Rin", false},
        {"SGCnfgWnd100100", "Shizuku", false},
        {"SGCnfgWnd100200", "Makoto", false},
        {"SGCnfgWnd100300", "Ai", false},
        {"SGCnfgWnd100400", "Rina", false},
        {"SGCnfgWnd100500", "Yumi", false},
        {"SGCnfgWnd110000", "Other Male", true},
        {"SGCnfgWnd110100", "Other Female", true},
    }};

constexpr std::array<DialogPlateSpec, kDialogPlateCount> kDialogPlates = {{
    {"SGDialog990000", "Exit the game?"},
    {"SGDialog990001", "Return to the title screen?"},
    {"SGDialog990002", "Save your progress?"},
    {"SGDialog990003", "Overwrite this save data?"},
    {"SGDialog990005", "Load this save data?"},
    {"SGDialog990006", "Delete this save data?"},
    {"SGDialog990007", "Quick save complete."},
}};

constexpr std::array<MsgTooltipSpec, kMsgTooltipCount> kMsgTooltips = {{
    {"SGMsgWnd800000", "Start Auto Mode"},
    {"SGMsgWnd800001", "Start Skip Mode"},
    {"SGMsgWnd800002", "Open Backlog"},
    {"SGMsgWnd800003", "Open Save Screen"},
    {"SGMsgWnd800004", "Open Load Screen"},
    {"SGMsgWnd800005", "Quick Save"},
    {"SGMsgWnd800006", "Quick Load"},
    {"SGMsgWnd800007", "Open Config"},
    {"SGMsgWnd800008", "Replay Voice"},
    {"SGMsgWnd800009", "Hide Message Window"},
}};

constexpr std::array<MusicTrackSpec, kMusicTrackCount> kMusicTracks = {{
    {1, "SGMusic000100", "Soaring Light of Karmic Communion"},
    {2, "SGMusic000200", "Infinite Power of Color"},
    {3, "SGMusic000300", "Sunlit Window"},
    {4, "SGMusic000400", "Gymnopédie"},
    {5, "SGMusic000500", "Turning to Petals, the Cherry Blossoms Sing"},
    {6, "SGMusic000600", "World of Karmic Communion"},
    {7, "SGMusic000700", "The Painting Sings"},
    {8, "SGMusic000800", "The Brush Dances Like Breathing"},
    {9, "SGMusic000900", "The Night Flows Gently"},
    {10, "SGMusic001000", "Eternity Sealed Within a Moment"},
    {11, "SGMusic001100", "Daytime Paintings"},
    {12, "SGMusic001200", "The Musical Notes Begin to Walk"},
    {13, "SGMusic001300", "Graceful Scale"},
    {14, "SGMusic001400", "Lightly! Lightly!"},
    {15, "SGMusic001500", "Playful Painting"},
    {16, "SGMusic001600", "♪ Model"},
    {17, "SGMusic001700", "Crimson Truth"},
    {18, "SGMusic001800", "Flower of Transparent Lies"},
    {19, "SGMusic001900", "Light Within the Mental Sketch"},
    {20, "SGMusic002000", "Your Brush Plays the World"},
    {21, "SGMusic002100", "Beyond the Blue We Looked Up To"},
    {22, "SGMusic002200", "Looking Up at the Footsteps of Dreams"},
    {23, "SGMusic002300", "Rusted Dreams Scattering Away"},
    {24, "SGMusic002400", "Crossing of Seconds"},
    {25, "SGMusic002500", "Fools Appear Like Fools"},
    {26, "SGMusic002600", "Gentleman's Game"},
    {27, "SGMusic002700", "Heart Model"},
    {28, "SGMusic002800", "Everyday Life Like Schumann's Symphony No. 1"},
    {29, "SGMusic002900", "Moon Dancing in the Sky, Wings Dancing in the Sky"},
    {30, "SGMusic003000", "Turning to Petals, the World Sings"},
    {31, "SGMusic003100", "The Night Sky Will Play"},
    {32, "SGMusic003200", "ZYPRESSEN Turns to Cherry Blossoms"},
    {33, "SGMusic003300", "Thank You, Days Gone By"},
    {34, "SGMusic003400", "The Earth Where You Stand"},
    {35, "SGMusic003500", "Beneath This Sakura no Uta"},
    {36, "SGMusic003600", "Lunar Story of the Eye"},
    {37, "SGMusic003700", "Miracle of the Celestial Sphere"},
    {38, "SGMusic003800", "Colors Spiraling Upward"},
    {39, "SGMusic003900", "Transparent White Everyday"},
    {40, "SGMusic004000", "The World Resounded in Beautiful Tone"},
    {41, "SGMusic004100", "Spring Flower Sketch Touched by the Wind's Brush"},
    {42, "SGMusic004200", "Soaring Karmic Communion"},
}};

char ToAsciiLower(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch + ('a' - 'A'));
    }
    return ch;
}

bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0u; i < a.size(); ++i) {
        if (ToAsciiLower(a[i]) != ToAsciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

std::size_t InstructionDwordCount(std::uint32_t opcode) noexcept {
    switch (opcode) {
        case kOpPushInt:
        case kOpPushCodeAddr:
        case kOpPushVarRef:
        case kOpPushStrAddr:
        case kOpLoad:
        case kOpStore:
        case kOpStoreDestAfter:
        case kOpJumpIfFalse:
            return kTwoDwordInstruction;
        case kOpLineRecordVariant:
        case kOpLineRecord:
            return kThreeDwordInstruction;
        default:
            return kDefaultInstructionDwords;
    }
}

std::optional<std::uint32_t> ReadU32Le(const std::vector<std::uint8_t>& buf,
                                       std::size_t offset) noexcept {
    if (offset + kDwordBytes > buf.size()) {
        return std::nullopt;
    }
    std::uint32_t val = 0u;
    std::memcpy(&val, buf.data() + offset, kDwordBytes);
    return val;
}

struct ParsedSelectLabel {
    std::string menu_digits;
    int branch_index = 0;
};

std::optional<ParsedSelectLabel> ParseSelectLabel(std::string_view text) {
    if (text.size() <= kSelectPrefix.size() ||
        text.substr(0u, kSelectPrefix.size()) != kSelectPrefix) {
        return std::nullopt;
    }
    const std::string_view rest = text.substr(kSelectPrefix.size());
    const std::size_t dash = rest.find('-');
    if (dash == std::string_view::npos || dash == 0u || dash + 1u >= rest.size()) {
        return std::nullopt;
    }
    const std::string_view menu_part = rest.substr(0u, dash);
    const std::string_view branch_part = rest.substr(dash + 1u);
    auto all_ascii_digits = [](std::string_view sv) noexcept {
        for (const char ch : sv) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
        return !sv.empty();
    };
    if (!all_ascii_digits(menu_part) || !all_ascii_digits(branch_part)) {
        return std::nullopt;
    }
    ParsedSelectLabel out;
    out.menu_digits.assign(menu_part.begin(), menu_part.end());
    out.branch_index = std::stoi(std::string(branch_part));
    return out;
}

std::string UnescapeTsvField(std::string_view escaped) {
    std::string out;
    out.reserve(escaped.size());
    for (std::size_t i = 0u; i < escaped.size(); ++i) {
        if (escaped[i] == '\\' && i + 1u < escaped.size()) {
            const char next = escaped[i + 1u];
            if (next == '\\') {
                out.push_back('\\');
                ++i;
                continue;
            }
            if (next == 't') {
                out.push_back('\t');
                ++i;
                continue;
            }
            if (next == 'r') {
                out.push_back('\r');
                ++i;
                continue;
            }
            if (next == 'n') {
                out.push_back('\n');
                ++i;
                continue;
            }
        }
        out.push_back(escaped[i]);
    }
    return out;
}

std::string StripLeadingSpaces(std::string_view text) {
    std::size_t pos = 0u;
    while (pos < text.size()) {
        if (text[pos] == ' ') {
            ++pos;
            continue;
        }
        if (pos + kIdeographicSpaceBytes <= text.size() &&
            std::memcmp(text.data() + pos, kIdeographicSpaceUtf8,
                        kIdeographicSpaceBytes) == 0) {
            pos += kIdeographicSpaceBytes;
            continue;
        }
        break;
    }
    return std::string(text.substr(pos));
}

void LoadTsvIntoTable(const fs::path& tsv_path,
                      std::unordered_map<std::string, std::string>& table) {
    std::error_code ec;
    if (!fs::is_regular_file(tsv_path, ec)) {
        return;
    }
    std::ifstream ifs(tsv_path, std::ios::binary);
    if (!ifs) {
        return;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || line.find('\t', tab + 1u) != std::string::npos) {
            continue;
        }
        const std::string_view raw_jp(line.data(), tab);
        const std::string_view raw_en(line.data() + tab + 1u, line.size() - tab - 1u);
        if (raw_jp.empty() || raw_en.empty()) {
            continue;
        }
        std::string jp = UnescapeTsvField(raw_jp);
        std::string en = StripLeadingSpaces(UnescapeTsvField(raw_en));
        if (!jp.empty() && !en.empty()) {
            table.emplace(std::move(jp), std::move(en));
        }
        table.emplace(std::string(raw_jp), StripLeadingSpaces(raw_en));
    }
}

void LoadTranslatedJsonIntoTable(
    const fs::path& json_path,
    std::unordered_map<std::string, std::string>& table) {
    std::error_code ec;
    if (!fs::is_regular_file(json_path, ec)) {
        return;
    }
    const bj::value root = json_parse_file(json_path.u8string());
    if (!root.is_object()) {
        return;
    }
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings_v = kv.value().get_object().if_contains("strings");
        if (strings_v == nullptr || !strings_v->is_array()) {
            continue;
        }
        for (const auto& item_v : strings_v->get_array()) {
            if (!item_v.is_object()) {
                continue;
            }
            const auto& obj = item_v.get_object();
            const auto* jp_v = obj.if_contains("text");
            if (jp_v == nullptr || !jp_v->is_string()) {
                continue;
            }
            const bj::value* en_v = obj.if_contains("translation");
            if (en_v == nullptr || !en_v->is_string()) {
                en_v = obj.if_contains("en");
            }
            if (en_v == nullptr || !en_v->is_string()) {
                continue;
            }
            const std::string jp(jp_v->get_string());
            const std::string raw_en(en_v->get_string());
            if (jp.empty() || raw_en.empty()) {
                continue;
            }
            std::string shipped = StripLeadingSpaces(build_tsv::shipped_english(jp, raw_en));
            if (!shipped.empty()) {
                table.emplace(jp, std::move(shipped));
            }
        }
    }
}

}  // namespace

const std::array<SplashScreenSpec, 2>& SplashScreenCatalog() noexcept {
    return kSplashScreens;
}

const SplashScreenSpec* FindSplashScreenSpec(std::string_view entry_name) noexcept {
    for (const SplashScreenSpec& spec : kSplashScreens) {
        if (EqualsIgnoreAsciiCase(spec.entry_name, entry_name)) {
            return &spec;
        }
    }
    return nullptr;
}

const std::array<ConfigSettingRowSpec, kConfigLeftRowCount>&
ConfigLeftColumnCatalog() noexcept {
    return kConfigLeftRows;
}

const std::array<ConfigSettingRowSpec, kConfigRightVolumeRowCount>&
ConfigRightVolumeCatalog() noexcept {
    return kConfigRightVolumeRows;
}

const std::array<ConfigToggleStripSpec, kConfigToggleStripCount>&
ConfigToggleStripCatalog() noexcept {
    return kConfigToggleStrips;
}

const ConfigToggleStripSpec* FindConfigToggleStripSpec(
    std::string_view entry_name) noexcept {
    for (const ConfigToggleStripSpec& spec : kConfigToggleStrips) {
        if (EqualsIgnoreAsciiCase(spec.entry_name, entry_name)) {
            return &spec;
        }
    }
    return nullptr;
}

const std::array<ConfigVoicePortraitSpec, kConfigVoicePortraitCount>&
ConfigVoicePortraitCatalog() noexcept {
    return kConfigVoicePortraits;
}

const ConfigVoicePortraitSpec* FindConfigVoicePortraitSpec(
    std::string_view entry_name) noexcept {
    for (const ConfigVoicePortraitSpec& spec : kConfigVoicePortraits) {
        if (EqualsIgnoreAsciiCase(spec.entry_name, entry_name)) {
            return &spec;
        }
    }
    return nullptr;
}

const std::array<DialogPlateSpec, kDialogPlateCount>&
DialogPlateCatalog() noexcept {
    return kDialogPlates;
}

const DialogPlateSpec* FindDialogPlateSpec(std::string_view entry_name) noexcept {
    for (const DialogPlateSpec& spec : kDialogPlates) {
        if (EqualsIgnoreAsciiCase(spec.entry_name, entry_name)) {
            return &spec;
        }
    }
    return nullptr;
}

const std::array<MsgTooltipSpec, kMsgTooltipCount>& MsgTooltipCatalog() noexcept {
    return kMsgTooltips;
}

const MsgTooltipSpec* FindMsgTooltipSpec(std::string_view entry_name) noexcept {
    for (const MsgTooltipSpec& spec : kMsgTooltips) {
        if (EqualsIgnoreAsciiCase(spec.entry_name, entry_name)) {
            return &spec;
        }
    }
    return nullptr;
}

const std::array<MusicTrackSpec, kMusicTrackCount>& MusicTrackCatalog() noexcept {
    return kMusicTracks;
}

const MusicTrackSpec* FindMusicTrackSpec(std::string_view entry_name) noexcept {
    for (const MusicTrackSpec& spec : kMusicTracks) {
        if (EqualsIgnoreAsciiCase(spec.entry_name, entry_name)) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<std::string_view> AllLocalizedSysGrpEntryNames() {
    std::vector<std::string_view> names;
    names.reserve(2u + 1u + kConfigToggleStripCount + kConfigVoicePortraitCount +
                  kDialogPlateCount + kMsgTooltipCount + kMusicTrackCount);
    names.push_back(kTitleBgEntryName);
    names.push_back(kLockedCgThumbEntryName);
    names.push_back(kConfigBgEntryName);
    for (const auto& s : kConfigToggleStrips) {
        names.push_back(s.entry_name);
    }
    for (const auto& s : kConfigVoicePortraits) {
        names.push_back(s.entry_name);
    }
    for (const auto& s : kDialogPlates) {
        names.push_back(s.entry_name);
    }
    for (const auto& s : kMsgTooltips) {
        names.push_back(s.entry_name);
    }
    for (const auto& s : kMusicTracks) {
        names.push_back(s.entry_name);
    }
    return names;
}

std::map<std::string, std::string> ExtractChoiceCensusFromScriptBytes(
    const std::vector<std::uint8_t>& script_bytes) {
    std::map<std::string, std::string> found;
    if (script_bytes.size() < kCompiledScriptMagicLen ||
        std::memcmp(script_bytes.data(), kCompiledScriptMagic,
                    kCompiledScriptMagicLen) != 0) {
        return found;
    }

    const std::size_t hdr_extra =
        ReadU32Le(script_bytes, kScriptHeaderExtraOffset).value_or(0u);
    const std::size_t hdr_size =
        std::min(kScriptHeaderBaseBytes + hdr_extra, script_bytes.size());

    std::size_t boundary = hdr_size;
    if (script_bytes.size() >= kDwordBytes) {
        for (std::size_t i = script_bytes.size() - kDwordBytes + 1u; i-- > 0u;) {
            if (std::memcmp(script_bytes.data() + i, kEndOfCodeMarker,
                            kDwordBytes) == 0) {
                boundary = std::max(hdr_size, i + kDwordBytes);
                break;
            }
        }
    }

    const std::size_t code_size = boundary - hdr_size;
    const std::uint8_t* const text_ptr = script_bytes.data() + boundary;
    const std::size_t text_size = script_bytes.size() - boundary;

    std::unordered_map<std::int64_t, std::string> text_section;
    std::size_t chunk_start = 0u;
    for (std::size_t i = 0u; i <= text_size; ++i) {
        if (i == text_size || text_ptr[i] == 0x00u) {
            const std::size_t chunk_len = i - chunk_start;
            if (auto decoded = cp932_to_utf8_strict(text_ptr + chunk_start, chunk_len)) {
                text_section.emplace(static_cast<std::int64_t>(chunk_start),
                                     std::move(*decoded));
            }
            chunk_start = i + 1u;
        }
    }

    std::vector<std::string> pushed_strings;
    std::size_t code_pos = 0u;
    while (code_pos + kDwordBytes <= code_size) {
        const auto op = ReadU32Le(script_bytes, hdr_size + code_pos);
        if (!op) {
            break;
        }
        const std::size_t dword_count = InstructionDwordCount(*op);
        if (*op == kOpPushStrAddr && code_pos + kTwoDwordInstruction * kDwordBytes <= code_size) {
            const auto str_addr =
                ReadU32Le(script_bytes, hdr_size + code_pos + kDwordBytes);
            if (str_addr) {
                const std::int64_t text_offset =
                    static_cast<std::int64_t>(*str_addr) -
                    static_cast<std::int64_t>(code_size);
                const auto it = text_section.find(text_offset);
                if (it != text_section.end()) {
                    pushed_strings.push_back(it->second);
                }
            }
        }
        code_pos += dword_count * kDwordBytes;
    }

    for (std::size_t k = 0u; k + 1u < pushed_strings.size(); ++k) {
        const auto parsed = ParseSelectLabel(pushed_strings[k]);
        if (!parsed) {
            continue;
        }
        if (ParseSelectLabel(pushed_strings[k + 1u]).has_value()) {
            continue;
        }
        std::string canonical_name =
            std::string(kSelectPrefix) + parsed->menu_digits + "-" +
            std::to_string(parsed->branch_index);
        found[std::move(canonical_name)] = pushed_strings[k + 1u];
    }
    return found;
}

std::map<std::string, std::string> ScanChoiceOptionCensus(
    const fs::path& game_dir) {
    struct ScriptLocation {
        fs::path arc_path;
        std::size_t entry_index = 0u;
    };

    std::vector<fs::path> candidate_arcs;
    std::set<std::string> seen_arc_names;
    for (const std::string& arc_name : extract::SCRIPT_ARCS) {
        const fs::path p = game_dir / fs::u8path(arc_name);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            candidate_arcs.push_back(p);
            seen_arc_names.insert(arc_name);
        }
    }

    std::error_code dir_ec;
    if (fs::is_directory(game_dir, dir_ec)) {
        std::vector<fs::path> extra_script_arcs;
        for (const fs::directory_entry& de : fs::directory_iterator(game_dir, dir_ec)) {
            if (!de.is_regular_file()) {
                continue;
            }
            const std::string fname = de.path().filename().u8string();
            if (fname.rfind("data01", 0u) == 0u &&
                fname.size() > 4u &&
                EqualsIgnoreAsciiCase(
                    std::string_view(fname).substr(fname.size() - 4u), ".arc") &&
                seen_arc_names.find(fname) == seen_arc_names.end()) {
                extra_script_arcs.push_back(de.path());
            }
        }
        std::sort(extra_script_arcs.begin(), extra_script_arcs.end());
        for (fs::path& p : extra_script_arcs) {
            candidate_arcs.push_back(std::move(p));
        }
    }

    std::map<std::string, ScriptLocation> overlay;
    for (const fs::path& arc_path : candidate_arcs) {
        const std::string arc_utf8 = arc_path.u8string();
        std::vector<bgi::ArcEntry> entries;
        try {
            entries = bgi::list_arc(arc_utf8);
        } catch (...) {
            continue;
        }
        for (std::size_t idx = 0u; idx < entries.size(); ++idx) {
            overlay.emplace(entries[idx].name, ScriptLocation{arc_path, idx});
        }
    }

    std::map<std::string, std::string> census;
    for (const auto& [script_name, loc] : overlay) {
        (void)script_name;
        Bytes raw;
        try {
            raw = bgi::read_entry(loc.arc_path.u8string(), loc.entry_index, true);
        } catch (...) {
            continue;
        }
        for (auto& [opt_name, jp_text] : ExtractChoiceCensusFromScriptBytes(raw)) {
            census[opt_name] = std::move(jp_text);
        }
    }
    return census;
}

std::unordered_map<std::string, std::string> LoadChoiceTranslationTable(
    const fs::path& project_dir,
    const fs::path& game_dir) {
    std::unordered_map<std::string, std::string> table;
    if (!project_dir.empty()) {
        LoadTsvIntoTable(project_dir / "build" / "install" / "game" / "translations.tsv",
                         table);
        LoadTsvIntoTable(project_dir / "game" / "translations.tsv", table);
        LoadTsvIntoTable(project_dir / "translations.tsv", table);
    }
    if (!game_dir.empty()) {
        LoadTsvIntoTable(game_dir / "translations.tsv", table);
    }
    if (!project_dir.empty()) {
        LoadTranslatedJsonIntoTable(
            project_dir / "script_output" / "translated_text.json", table);
    }
    return table;
}

std::vector<ChoiceOptionItem> ResolveChoiceOptions(
    const std::vector<std::string>& choice_arc_entry_names,
    const std::map<std::string, std::string>& census_jp_by_name,
    const std::unordered_map<std::string, std::string>& en_by_jp) {
    std::vector<std::string> sorted_names;
    for (const std::string& name : choice_arc_entry_names) {
        if (ParseSelectLabel(name).has_value()) {
            sorted_names.push_back(name);
        }
    }
    std::sort(sorted_names.begin(), sorted_names.end());

    std::vector<ChoiceOptionItem> resolved;
    resolved.reserve(sorted_names.size());
    for (const std::string& name : sorted_names) {
        const auto jp_it = census_jp_by_name.find(name);
        if (jp_it == census_jp_by_name.end()) {
            throw std::runtime_error(
                "Choice option missing from script bytecode census: " + name);
        }
        const std::string& jp = jp_it->second;
        auto en_it = en_by_jp.find(jp);
        if (en_it == en_by_jp.end()) {
            const std::string stripped_jp = StripLeadingSpaces(jp);
            en_it = en_by_jp.find(stripped_jp);
        }
        if (en_it == en_by_jp.end() || en_it->second.empty()) {
            throw std::runtime_error(
                "Missing English translation for choice option " + name + ": " + jp);
        }
        resolved.push_back(ChoiceOptionItem{name, jp, en_it->second});
    }
    return resolved;
}

}  // namespace snu::menu
