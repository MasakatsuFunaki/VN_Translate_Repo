// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ama::menu {

inline constexpr std::size_t kFesHeaderBytes = 16;
inline constexpr std::size_t kKcsHeaderBytes = 52;

struct FesPayload {
    std::vector<std::uint8_t> body;
    std::uint32_t reserved = 0;

    std::string AsText() const;
    void SetText(std::string_view text);
};

struct KcsPayload {
    std::array<std::uint8_t, kKcsHeaderBytes> header{};
    std::vector<std::uint8_t> body;
};

class FesScriptCodec {
public:
    FesScriptCodec();
    ~FesScriptCodec();

    FesScriptCodec(FesScriptCodec&&) noexcept;
    FesScriptCodec& operator=(FesScriptCodec&&) noexcept;

    FesScriptCodec(const FesScriptCodec&) = delete;
    FesScriptCodec& operator=(const FesScriptCodec&) = delete;

    static FesPayload UnpackFes(const std::uint8_t* data, std::size_t size);
    static FesPayload UnpackFes(const std::vector<std::uint8_t>& bytes);
    static std::vector<std::uint8_t> PackFes(const FesPayload& payload);
    static std::vector<std::uint8_t> PackFes(const std::uint8_t* body, std::size_t size,
                                             std::uint32_t reserved = 0);
    static std::vector<std::uint8_t> PackFesText(std::string_view text,
                                                 std::uint32_t reserved = 0);

    static KcsPayload UnpackKcs(const std::uint8_t* data, std::size_t size);
    static KcsPayload UnpackKcs(const std::vector<std::uint8_t>& bytes);
    static std::vector<std::uint8_t> PackKcs(const KcsPayload& payload);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ama::menu
