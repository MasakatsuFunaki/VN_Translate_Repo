// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"

#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oretsuba::apps {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBufferGrowthFactor = 2U;
constexpr int kMaxAncestorLevels = 8;
constexpr int kProgramNameArgCount = 1;
constexpr int kFirstUserArgIndex = 1;
constexpr std::size_t kPeMagicSecondByteOffset = 1U;
constexpr std::uint8_t kPeMagicByteM = static_cast<std::uint8_t>('M');
constexpr std::uint8_t kPeMagicByteZ = static_cast<std::uint8_t>('Z');

constexpr const char* kGameExecutableName = "ORE_TUBA.exe";
constexpr const char* kGameBackupExecutableName = "ORE_TUBA_orig.exe";

struct LocalFreeDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            ::LocalFree(ptr);
        }
    }
};

std::string WideArgToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed to convert command-line argument");
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), required, nullptr, nullptr);
    if (written != required) {
        throw std::runtime_error("WideCharToMultiByte produced unexpected UTF-8 byte count");
    }
    return utf8;
}

std::string FormatOptionsDescription(const po::options_description& desc) {
    std::ostringstream stream;
    stream << desc;
    return stream.str();
}

std::optional<int> ParseUtf8ArgumentList(const std::vector<std::string>& args,
                                         const po::options_description& desc,
                                         po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(args).options(desc).run();
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
        const DWORD copied = ::GetModuleFileNameW(
            nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buf.size()) {
            buf.resize(copied);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path().u8string();
}

std::string FindProjectDirectoryFrom(const std::filesystem::path& start_dir) {
    if (start_dir.empty()) {
        return {};
    }
    std::error_code ec;
    fs::path d = start_dir;
    for (int level = 0; level <= kMaxAncestorLevels && !d.empty(); ++level) {
        if (fs::is_directory(d / "pipeline_cpp", ec) &&
            fs::is_regular_file(d / "build.py", ec)) {
            return d.u8string();
        }
        if (d == d.root_path()) {
            break;
        }
        d = d.parent_path();
    }

    d = start_dir;
    for (int level = 0; level <= kMaxAncestorLevels && !d.empty(); ++level) {
        if (fs::is_directory(d / "bin", ec) &&
            (fs::is_directory(d / "game", ec) ||
             fs::is_directory(d / "script_output", ec))) {
            return d.u8string();
        }
        if (d == d.root_path()) {
            break;
        }
        d = d.parent_path();
    }
    return {};
}

std::string DefaultProjectDirectory() {
    const std::string exe = ExecutableDirectory();
    if (exe.empty()) {
        return {};
    }
    return FindProjectDirectoryFrom(fs::u8path(exe));
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

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir_val, "Oretachi_ni_Tsubasa_wa_Nai project folder")
        ("game-dir", po::value(&game_dir)->required(), "game installation folder");
    return desc;
}

po::options_description BuildProjectOnlyOptions(std::string& project_dir) {
    auto* dir_val = po::value(&project_dir);
    const std::string found_project = DefaultProjectDirectory();
    if (found_project.empty()) {
        dir_val->required();
    }
    if (!found_project.empty()) {
        dir_val->default_value(found_project);
    }

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir_val, "Oretachi_ni_Tsubasa_wa_Nai project folder");
    return desc;
}

std::vector<std::string> ProcessUtf8Args() {
    const wchar_t* cmd_line = ::GetCommandLineW();
    if (cmd_line == nullptr || *cmd_line == L'\0') {
        return {};
    }
    int argc = 0;
    wchar_t** raw_argv = ::CommandLineToArgvW(cmd_line, &argc);
    const std::unique_ptr<wchar_t*, LocalFreeDeleter> argv_guard(raw_argv);
    if (!argv_guard) {
        throw std::runtime_error("CommandLineToArgvW failed to parse process command line");
    }
    if (argc <= kProgramNameArgCount) {
        return {};
    }

    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<std::size_t>(argc - kProgramNameArgCount));
    for (int i = kFirstUserArgIndex; i < argc; ++i) {
        const wchar_t* arg = raw_argv[i];
        utf8_args.push_back(arg != nullptr ? WideArgToUtf8(arg) : std::string{});
    }
    return utf8_args;
}

std::vector<std::string> process_utf8_args() {
    return ProcessUtf8Args();
}

std::optional<int> ParseCommandLine(int argc,
                                    char** argv,
                                    const po::options_description& desc,
                                    po::variables_map& vm) {
    std::vector<std::string> args;
    if (argc > kProgramNameArgCount && argv != nullptr) {
        args.reserve(static_cast<std::size_t>(argc - kProgramNameArgCount));
        for (int i = kFirstUserArgIndex; i < argc; ++i) {
            args.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
    }
    return ParseUtf8ArgumentList(args, desc, vm);
}

std::optional<int> parse_command_line(int argc,
                                      char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    return ParseCommandLine(argc, argv, desc, vm);
}

std::optional<int> ParseProcessCommandLine(const po::options_description& desc,
                                           po::variables_map& vm) {
    return ParseUtf8ArgumentList(ProcessUtf8Args(), desc, vm);
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    return ParseProcessCommandLine(desc, vm);
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
