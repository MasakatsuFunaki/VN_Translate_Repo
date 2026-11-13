// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "translate/translate_core.h"

#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace po = boost::program_options;

constexpr const char* kScriptOutputSubdir = "script_output";
constexpr const char* kExtractedJsonFilename = "extracted_text.json";
constexpr const char* kTranslatedJsonFilename = "translated_text.json";
constexpr const char* kCacheJsonFilename = "translation_cache_gemini.json";
constexpr const char* kLastBatchSubdir = "last_gemini_translate";
constexpr const char* kLastBatchFilename = "last_translation.json";
constexpr const char* kToneMarkdownFilename = "TONE_OF_NOVEL.md";
constexpr const char* kBuildSubdir = "build";
constexpr const char* kInstallSubdir = "install";
constexpr const char* kProxySubdir = "proxy";
constexpr const char* kReleaseSubdir = "Release";
constexpr const char* kGameSubdir = "game";
constexpr const char* kTsvFilename = "translation_table.tsv";
constexpr const char* kProxyDllFilename = "winmm.dll";
constexpr int kUnlimitedBatches = 0;
constexpr int kExitRuntimeFailure = 1;

bool DeployArtifactsToGameDir(const fs::path& proj_path,
                              const fs::path& tsv_path,
                              const fs::path& game_dir) {
    if (const wchar_t* dist = _wgetenv(L"VN_DIST_BUILD");
        dist != nullptr && *dist != L'\0') {
        std::cout << "[DIST] VN_DIST_BUILD set -- skipping deploy to the game folder.\n";
        return true;
    }

    std::error_code ec;
    if (game_dir.empty() || !fs::exists(game_dir, ec) || ec) {
        std::fprintf(stderr,
                     "[ERROR] game directory not found: %s\n",
                     game_dir.u8string().c_str());
        return false;
    }

    std::vector<fs::path> dll_candidates;
    if (const std::string exe_dir = oretsuba::apps::ExecutableDirectory(); !exe_dir.empty()) {
        dll_candidates.push_back(fs::u8path(exe_dir) / ".." / kGameSubdir / kProxyDllFilename);
    }
    dll_candidates.push_back(proj_path / kGameSubdir / kProxyDllFilename);
    dll_candidates.push_back(
        proj_path / kBuildSubdir / kInstallSubdir / kGameSubdir / kProxyDllFilename);
    dll_candidates.push_back(
        proj_path / kBuildSubdir / kProxySubdir / kReleaseSubdir / kProxyDllFilename);

    fs::path src_dll;
    for (const fs::path& candidate : dll_candidates) {
        std::error_code probe_ec;
        if (fs::exists(candidate, probe_ec) && !probe_ec) {
            src_dll = candidate;
            break;
        }
    }
    if (src_dll.empty()) {
        std::fprintf(stderr,
                     "[ERROR] %s not found -- build the proxy half first (python build.py).\n",
                     kProxyDllFilename);
        return false;
    }

    if (oretsuba::apps::UnwrapInnerPeIfNeeded(game_dir)) {
        std::cout << "[Deploy] Unwrapped inner Lucifen PE to "
                  << (game_dir / "ORE_TUBA.exe").u8string() << "\n";
    }

    const fs::path dst_dll = game_dir / kProxyDllFilename;
    fs::copy_file(src_dll, dst_dll, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr,
                     "[ERROR] copying %s failed (%s) -- is the game running? Close it and retry.\n",
                     kProxyDllFilename,
                     ec.message().c_str());
        return false;
    }
    std::cout << "[Deploy] Copied " << dst_dll.u8string() << "\n";

    if (fs::exists(tsv_path, ec) && !ec) {
        const fs::path dst_tsv = game_dir / kTsvFilename;
        fs::copy_file(tsv_path, dst_tsv, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr,
                         "[ERROR] copying %s failed (%s)\n",
                         kTsvFilename,
                         ec.message().c_str());
            return false;
        }
        std::cout << "[Deploy] Copied " << dst_tsv.u8string() << "\n";
    }
    return true;
}

}  // namespace

int main() {
    try {
        oretsuba::SetupConsoleUtf8();

        std::string project_dir;
        std::string game_dir;
        po::options_description desc =
            oretsuba::apps::BuildCommonOptions(project_dir, game_dir);

        oretsuba::translate::TranslateOptions opts;
        desc.add_options()
            ("max-batches",
             po::value<int>(&opts.max_batches)->default_value(kUnlimitedBatches)->value_name("N"),
             "Maximum number of batches to run (0 = all)")
            ("batch",
             po::value<int>(&opts.batch_size)->default_value(oretsuba::translate::kDefaultBatchSize)->value_name("N"),
             "Batch size");

        po::variables_map vm;
        if (const auto rc = oretsuba::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path proj_path = fs::u8path(project_dir);
        const fs::path script_out = proj_path / kScriptOutputSubdir;
        const fs::path install_game_dir =
            proj_path / kBuildSubdir / kInstallSubdir / kGameSubdir;
        const fs::path staged_tsv = install_game_dir / kTsvFilename;

        opts.project_dir = project_dir;
        opts.extracted_file = (script_out / kExtractedJsonFilename).u8string();
        opts.translated_file = (script_out / kTranslatedJsonFilename).u8string();
        opts.cache_file = (script_out / kCacheJsonFilename).u8string();
        opts.last_batch_log_file =
            (script_out / kLastBatchSubdir / kLastBatchFilename).u8string();
        opts.tone_file = (proj_path / kToneMarkdownFilename).u8string();

        std::cout << "[1/4] Extracting SCRIPT.LPK\n";
        const oretsuba::extract::Extractor extractor;
        const int extract_rc = extractor.Run(game_dir, opts.extracted_file);
        if (extract_rc != oretsuba::apps::kExitSuccess) {
            return extract_rc;
        }

        std::cout << "[2/4] Translating\n";
        oretsuba::translate::TranslateCore core;
        const int translate_rc = core.Run(opts);
        if (translate_rc != oretsuba::apps::kExitSuccess) {
            return translate_rc;
        }

        std::cout << "[3/4] Building TSV\n";
        const oretsuba::build_tsv::TsvBuilder builder;
        const int tsv_rc =
            builder.Run(opts.translated_file, opts.cache_file, staged_tsv.u8string());
        if (tsv_rc != oretsuba::apps::kExitSuccess) {
            return tsv_rc;
        }

        std::cout << "[4/4] Unwrapping Inner PE & Deploying\n";
        if (!DeployArtifactsToGameDir(proj_path, staged_tsv, fs::u8path(game_dir))) {
            return kExitRuntimeFailure;
        }

        return oretsuba::apps::kExitSuccess;
    } catch (const std::exception& err) {
        std::fprintf(stderr, "[ERROR] %s\n", err.what());
        return kExitRuntimeFailure;
    }
}
