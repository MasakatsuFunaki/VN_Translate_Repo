// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"

#include <filesystem>
#include <memory>

namespace spot_checker {

// Pure protocol interface synchronizing plain `script_output/*.json` artifacts
// with tracked `VNE1`/`VNE2` `.json.enc` encrypted files.
class ICryptSync {
public:
    virtual ~ICryptSync();

    ICryptSync(const ICryptSync&) = delete;
    ICryptSync& operator=(const ICryptSync&) = delete;

    // If `<game_root>/script_output/translated_text.json` is absent and
    // `translated_text.json.enc` is present, decrypts the game's `.json.enc`
    // files. Returns `ErrorCode::CryptSyncFailed` if the decryption tool is
    // missing, exits non-zero, or fails to materialize `translated_text.json`.
    virtual Status decrypt_if_needed(const std::filesystem::path& game_root) = 0;

    // If `<game_root>/script_output/translated_text.json.enc` is present
    // alongside `translated_text.json`, re-encrypts updated plain JSON files.
    // Returns `ErrorCode::CryptSyncFailed` if the encryption tool exits non-zero.
    virtual Status encrypt_if_tracked(
        const std::filesystem::path& game_root) = 0;

protected:
    ICryptSync() = default;
    ICryptSync(ICryptSync&&) noexcept = default;
    ICryptSync& operator=(ICryptSync&&) noexcept = default;
};

class NoOpCryptSync final : public ICryptSync {
public:
    NoOpCryptSync() = default;
    ~NoOpCryptSync() override;

    Status decrypt_if_needed(const std::filesystem::path& game_root) override;
    Status encrypt_if_tracked(const std::filesystem::path& game_root) override;
};

// Production implementation invoking `decrypt_translations.bat` and
// `encrypt_translations.bat` via `CreateProcessW` and verifying `exit_code == 0`.
// Searches script directory candidates in order:
//   1. `primary_scripts_dir` (when non-empty)
//   2. `game_root`
//   3. `game_root.parent_path()`
//   4. `fallback_scripts_dir` (when non-empty — e.g. `find_repo_root()`, so
//      staging `.json.enc` in `%TEMP%` still invokes `<repo_root>\decrypt_translations.bat`
//      with intact `%~dp0` key resolution).
class BatchProcessCryptSync final : public ICryptSync {
public:
    explicit BatchProcessCryptSync(
        std::filesystem::path primary_scripts_dir = {},
        std::filesystem::path fallback_scripts_dir = {});
    ~BatchProcessCryptSync() override;

    Status decrypt_if_needed(const std::filesystem::path& game_root) override;
    Status encrypt_if_tracked(const std::filesystem::path& game_root) override;

private:
    std::filesystem::path primary_scripts_dir_;
    std::filesystem::path fallback_scripts_dir_;
};

std::unique_ptr<ICryptSync> make_batch_crypt_sync(
    std::filesystem::path primary_scripts_dir = {},
    std::filesystem::path fallback_scripts_dir = {});

}  // namespace spot_checker
