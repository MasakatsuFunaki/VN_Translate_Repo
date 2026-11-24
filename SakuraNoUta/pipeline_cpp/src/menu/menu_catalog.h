// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Authoritative localization catalog for Sakura no Uta's UI sprites, startup
// splash screens, Extra Music track banners, and BGI bytecode choice plates.
//
// Thread-safe: all catalog tables are immutable static storage; choice-census
// functions operate exclusively on caller-supplied paths and local state.
#pragma once

#include "menu/bgra_image.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snu::menu {

inline constexpr std::string_view kSysGrpArcRelPath = "sysgrp.arc";
inline constexpr std::string_view kSplashArcRelPath = "data02501.arc";
inline constexpr std::string_view kChoiceArcRelPath = "data02502.arc";

inline constexpr std::string_view kSplash1EntryName = "att01";
inline constexpr std::string_view kSplash2EntryName = "att02";
inline constexpr std::string_view kTitleBgEntryName = "SGTitle990000";
inline constexpr std::string_view kLockedCgThumbEntryName = "SGCG800001";
inline constexpr std::string_view kConfigBgEntryName = "SGCnfgWnd990000";
inline constexpr std::string_view kMusicBgEntryName = "SGMusic990000";

inline constexpr std::string_view kLogoSubtitleEn =
    "Fluttering Above the Cherry Blossom Forest";
inline constexpr std::string_view kTitleRomajiCaptionEn = "Sakura no Uta";
inline constexpr std::string_view kConfigVerticalVoicesLabelEn =
    "Character Voices";

inline constexpr std::size_t kSplashParagraphCount = 3u;
inline constexpr std::size_t kConfigLeftRowCount = 9u;
inline constexpr std::size_t kConfigRightVolumeRowCount = 4u;
inline constexpr std::size_t kConfigToggleStripCount = 12u;
inline constexpr std::size_t kConfigVoicePortraitCount = 8u;
inline constexpr std::size_t kDialogPlateCount = 7u;
inline constexpr std::size_t kMsgTooltipCount = 10u;
inline constexpr std::size_t kMusicTrackCount = 42u;
inline constexpr std::size_t kRetailChoiceOptionCount = 34u;

struct SplashParagraphSpec {
    std::string_view text_en;
    RectI bounds{};
};

struct SplashScreenSpec {
    std::string_view entry_name;
    std::array<SplashParagraphSpec, kSplashParagraphCount> paragraphs{};
};

struct ConfigSettingRowSpec {
    std::string_view label_en;
    bool has_slider = false;
    std::string_view slider_min_en;
    std::string_view slider_max_en;
};

enum class ConfigToggleTheme {
    kLavenderInactiveCell0,
    kSkyBlueActiveCell0,
};

struct ConfigToggleStripSpec {
    std::string_view entry_name;
    std::string_view label_en;
    ConfigToggleTheme theme = ConfigToggleTheme::kLavenderInactiveCell0;
    std::string_view clean_donor_entry_name;
};

struct ConfigVoicePortraitSpec {
    std::string_view entry_name;
    std::string_view name_en;
    bool flat_border_wipe = false;
};

struct DialogPlateSpec {
    std::string_view entry_name;
    std::string_view prompt_en;
};

struct MsgTooltipSpec {
    std::string_view entry_name;
    std::string_view tooltip_en;
};

struct MusicTrackSpec {
    int track_number = 0;
    std::string_view entry_name;
    std::string_view title_en;
};

struct ChoiceOptionItem {
    std::string entry_name;
    std::string japanese_text;
    std::string english_text;
};

const std::array<SplashScreenSpec, 2>& SplashScreenCatalog() noexcept;
const SplashScreenSpec* FindSplashScreenSpec(std::string_view entry_name) noexcept;

const std::array<ConfigSettingRowSpec, kConfigLeftRowCount>&
ConfigLeftColumnCatalog() noexcept;

const std::array<ConfigSettingRowSpec, kConfigRightVolumeRowCount>&
ConfigRightVolumeCatalog() noexcept;

const std::array<ConfigToggleStripSpec, kConfigToggleStripCount>&
ConfigToggleStripCatalog() noexcept;
const ConfigToggleStripSpec* FindConfigToggleStripSpec(
    std::string_view entry_name) noexcept;

const std::array<ConfigVoicePortraitSpec, kConfigVoicePortraitCount>&
ConfigVoicePortraitCatalog() noexcept;
const ConfigVoicePortraitSpec* FindConfigVoicePortraitSpec(
    std::string_view entry_name) noexcept;

const std::array<DialogPlateSpec, kDialogPlateCount>&
DialogPlateCatalog() noexcept;
const DialogPlateSpec* FindDialogPlateSpec(std::string_view entry_name) noexcept;

const std::array<MsgTooltipSpec, kMsgTooltipCount>&
MsgTooltipCatalog() noexcept;
const MsgTooltipSpec* FindMsgTooltipSpec(std::string_view entry_name) noexcept;

const std::array<MusicTrackSpec, kMusicTrackCount>&
MusicTrackCatalog() noexcept;
const MusicTrackSpec* FindMusicTrackSpec(std::string_view entry_name) noexcept;

// Returns all localized sysgrp.arc entry names in deterministic catalog order.
std::vector<std::string_view> AllLocalizedSysGrpEntryNames();

// Disassembles a single BurikoCompiledScriptVer1.00 payload and extracts
// `selectNN-M -> Japanese option string` pairs using linear instruction layout.
std::map<std::string, std::string> ExtractChoiceCensusFromScriptBytes(
    const std::vector<std::uint8_t>& script_bytes);

// Scans `SCRIPT_ARCS` (in engine overlay priority order) under `game_dir` and
// maps every `selectNN-M` asset identifier to its Japanese option string.
std::map<std::string, std::string> ScanChoiceOptionCensus(
    const std::filesystem::path& game_dir);

// Loads JP -> EN lookup entries from `<project_dir>/build/install/game/translations.tsv`,
// `<project_dir>/translations.tsv`, `<game_dir>/translations.tsv`, and
// `<project_dir>/script_output/translated_text.json` (when present).
std::unordered_map<std::string, std::string> LoadChoiceTranslationTable(
    const std::filesystem::path& project_dir,
    const std::filesystem::path& game_dir);

// Resolves every `select*` entry present in `choice_arc_entries` against the
// bytecode census and English translation table. Throws `std::runtime_error`
// if a choice entry lacks either its script census mapping or English text.
std::vector<ChoiceOptionItem> ResolveChoiceOptions(
    const std::vector<std::string>& choice_arc_entry_names,
    const std::map<std::string, std::string>& census_jp_by_name,
    const std::unordered_map<std::string, std::string>& en_by_jp);

}  // namespace snu::menu
