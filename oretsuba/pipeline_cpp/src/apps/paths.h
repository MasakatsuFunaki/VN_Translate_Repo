// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

namespace oretsuba::apps {

namespace po = boost::program_options;

constexpr int kExitSuccess = 0;
constexpr int kExitCommandLineError = 1;
constexpr int kExitGateOrValidationError = 2;

constexpr std::size_t kSoftDenchiOuterSize = 3004760U;
constexpr std::size_t kDmmOuterWrapperSize = 1381376U;
constexpr std::size_t kInnerPeOffset = 0x031BC8U;
constexpr std::size_t kInnerPeSize = 0x10D000U;

struct PipelineStepDescriptor {
    const char* name;
    const char* description;
};

const std::vector<PipelineStepDescriptor>& PipelineSteps();

std::string ExecutableDirectory();
std::string DefaultProjectDirectory();
std::string DefaultGameDirectory();

po::options_description BuildCommonOptions(std::string& project_dir,
                                           std::string& game_dir);

std::optional<int> ParseCommandLine(int argc,
                                    char** argv,
                                    const po::options_description& desc,
                                    po::variables_map& vm);

// Checks <game_dir>/ORE_TUBA.exe: if it is the outer SoftDenchi/DMM wrapper
// containing the 1,101,824-byte inner Lucifen PE at offset 0x031BC8..0x13EBC8
// starting with "MZ", backs up <game_dir>/ORE_TUBA_orig.exe and writes the
// inner PE to <game_dir>/ORE_TUBA.exe. Returns true if unwrapping occurred.
bool UnwrapInnerPeIfNeeded(const std::filesystem::path& game_dir);

}  // namespace oretsuba::apps
