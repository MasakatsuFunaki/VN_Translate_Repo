// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <system_error>

#include "common/util.h"

namespace oretsuba::apps {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBufferGrowthMultiplier = 2U;
constexpr std::size_t kPeMagicSecondByteOffset = 1U;
constexpr std::uint8_t kPeMagicByteM = static_cast<std::uint8_t>('M');
constexpr std::uint8_t kPeMagicByteZ = static_cast<std::uint8_t>('Z');

constexpr const char* kGameExecutableName = "ORE_TUBA.exe";
constexpr const char* kGameBackupExecutableName = "ORE_TUBA_orig.exe";

constexpr const char* kKnownGameInstallPath =
    R"(D:\Kits\my_downloadedGames\+new\dmm\Oretachi ni Tsubasa wa Nai Anniversary Edition\Oretachi ni Tsubasa wa Nai Anniversary Edition)";

std::string FormatOptionsDescription(const po::options_description& desc) {
    std::ostringstream stream;
    stream << desc;
    return stream.str();
}

}  // namespace

const std::vector<PipelineStepDescriptor>& PipelineSteps() {
    static const std::vector<PipelineStepDescriptor> kSteps = {
        {"01_extract", "Unpack SCRIPT.LPK and extract SOB0/SOB1/SOB2 strings"},
        {"02_translate", "Validate speaker gate and translate via gemini-4-argon"},
        {"03_build_tsv", "Build runtime translation_table.tsv with line wrapping"},
        {"deploy", "Unwrap ORE_TUBA.exe inner PE and deploy runtime artifacts"},
    };
    return kSteps;
}

std::string ExecutableDirectory() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buf.size()) {
            buf.resize(copied);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthMultiplier);
    }
    return fs::path(buf).parent_path().u8string();
}

std::string DefaultProjectDirectory() {
    const std::string exe = ExecutableDirectory();
    if (exe.empty()) {
        return {};
    }
    std::error_code ec;
    for (fs::path cur = fs::u8path(exe); !cur.empty(); cur = cur.parent_path()) {
        if (fs::is_directory(cur / "pipeline_cpp", ec) &&
            fs::is_regular_file(cur / "build.py", ec)) {
            return cur.string();
        }
        if (fs::is_directory(cur / "script_output", ec) &&
            fs::is_directory(cur / "bin", ec)) {
            return cur.string();
        }
        if (cur == cur.root_path()) {
            break;
        }
    }
    return {};
}

std::string DefaultGameDirectory() {
    std::error_code ec;
    const fs::path candidate = fs::u8path(kKnownGameInstallPath);
    if (fs::is_directory(candidate, ec) &&
        fs::is_regular_file(candidate / "SCRIPT.LPK", ec)) {
        return candidate.string();
    }
    return {};
}

po::options_description BuildCommonOptions(std::string& project_dir,
                                           std::string& game_dir) {
    auto* dir_val = po::value(&project_dir);
    const std::string found_project = DefaultProjectDirectory();
    if (found_project.empty()) {
        dir_val->required();
    }
    if (!found_project.empty()) {
        dir_val->default_value(found_project);
    }

    auto* game_val = po::value(&game_dir);
    const std::string found_game = DefaultGameDirectory();
    game_val->default_value(found_game);

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir_val, "Oretachi_ni_Tsubasa_wa_Nai project folder")
        ("game-dir", game_val, "game installation folder");
    return desc;
}

std::optional<int> ParseCommandLine(int argc,
                                    char** argv,
                                    const po::options_description& desc,
                                    po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(argc, argv).options(desc).run();
        const auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
        if (!stray.empty()) {
            const std::string help_text = FormatOptionsDescription(desc);
            std::fprintf(stderr, "[ERROR] unexpected argument '%s'\n\n%s\n",
                         stray.front().c_str(), help_text.c_str());
            return kExitCommandLineError;
        }

        po::store(parsed, vm);
        if (vm.count("help") > 0) {
            const std::string help_text = FormatOptionsDescription(desc);
            std::fputs((help_text + "\n").c_str(), stdout);
            return kExitSuccess;
        }
        po::notify(vm);
    } catch (const po::error& err) {
        const std::string help_text = FormatOptionsDescription(desc);
        std::fprintf(stderr, "[ERROR] %s\n\n%s\n", err.what(), help_text.c_str());
        return kExitCommandLineError;
    }
    return std::nullopt;
}

bool UnwrapInnerPeIfNeeded(const std::filesystem::path& game_dir) {
    if (game_dir.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path exe_path = game_dir / kGameExecutableName;
    if (!fs::is_regular_file(exe_path, ec)) {
        return false;
    }

    const auto raw_size = fs::file_size(exe_path, ec);
    if (ec) {
        return false;
    }
    const auto file_size = static_cast<std::size_t>(raw_size);
    if (file_size == kInnerPeSize) {
        return false;
    }

    const bool is_wrapper_size =
        (file_size == kSoftDenchiOuterSize) ||
        (file_size == kDmmOuterWrapperSize);
    if (!is_wrapper_size || file_size < kInnerPeOffset + kInnerPeSize) {
        return false;
    }

    const Bytes outer_bytes = ReadFileBytes(exe_path.u8string());
    if (outer_bytes.size() < kInnerPeOffset + kInnerPeSize) {
        return false;
    }
    if (outer_bytes[kInnerPeOffset] != kPeMagicByteM ||
        outer_bytes[kInnerPeOffset + kPeMagicSecondByteOffset] != kPeMagicByteZ) {
        return false;
    }

    const fs::path backup_path = game_dir / kGameBackupExecutableName;
    if (!fs::is_regular_file(backup_path, ec)) {
        WriteFileBytes(backup_path.u8string(), outer_bytes);
        LogInfo("Backed up outer wrapper to " + backup_path.u8string());
    }

    WriteFileBytes(
        exe_path.u8string(),
        outer_bytes.data() + kInnerPeOffset,
        kInnerPeSize);
    LogInfo("Unwrapped inner Lucifen PE (" +
            FormatWithCommas(static_cast<long long>(kInnerPeSize)) +
            " bytes) to " + exe_path.u8string());
    return true;
}

}  // namespace oretsuba::apps
