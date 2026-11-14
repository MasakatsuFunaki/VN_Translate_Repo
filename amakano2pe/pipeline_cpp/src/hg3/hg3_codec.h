// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "imaging/bgra_image.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ama::hg3 {

struct Hg3StdInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bpp = 32;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
    std::uint32_t total_width = 0;
    std::uint32_t total_height = 0;
    std::uint32_t transparent_flag = 1;
    std::int32_t canvas_x = 0;
    std::int32_t canvas_y = 0;

    bool operator==(const Hg3StdInfo& other) const noexcept {
        return width == other.width && height == other.height && bpp == other.bpp &&
               offset_x == other.offset_x && offset_y == other.offset_y &&
               total_width == other.total_width && total_height == other.total_height &&
               transparent_flag == other.transparent_flag && canvas_x == other.canvas_x &&
               canvas_y == other.canvas_y;
    }

    bool operator!=(const Hg3StdInfo& other) const noexcept { return !(*this == other); }
};

struct Hg3ExtraTag {
    std::string name;
    std::vector<std::uint8_t> payload;

    bool operator==(const Hg3ExtraTag& other) const noexcept {
        return name == other.name && payload == other.payload;
    }

    bool operator!=(const Hg3ExtraTag& other) const noexcept { return !(*this == other); }
};

struct Hg3Frame {
    std::uint32_t frame_id = 0;
    bool has_stdinfo = true;
    Hg3StdInfo stdinfo{};
    ama::imaging::BgraImage pixels;
    std::optional<std::uint32_t> cptype;
    std::optional<std::uint32_t> imgmode;
    bool was_jpeg_encoded = false;
    bool top_down_img0000 = false;
    std::uint32_t img0000_unknown = 0;
    std::vector<Hg3ExtraTag> extra_tags;
};

struct Hg3Container {
    std::vector<Hg3Frame> frames;

    Hg3Frame* FindFrame(std::uint32_t id) noexcept;
    const Hg3Frame* FindFrame(std::uint32_t id) const noexcept;
};

class Hg3Codec {
public:
    Hg3Codec();
    ~Hg3Codec();

    Hg3Codec(Hg3Codec&&) noexcept;
    Hg3Codec& operator=(Hg3Codec&&) noexcept;

    Hg3Codec(const Hg3Codec&) = delete;
    Hg3Codec& operator=(const Hg3Codec&) = delete;

    static Hg3Container Decode(const std::uint8_t* data, std::size_t size);
    static Hg3Container Decode(const std::vector<std::uint8_t>& bytes);
    static Hg3Container DecodeFile(const std::filesystem::path& path);

    static std::vector<std::uint8_t> Encode(const Hg3Container& container);
    static void EncodeToFile(const Hg3Container& container, const std::filesystem::path& path);

    static bool VerifyFrameMetadataParity(const Hg3Container& expected,
                                          const Hg3Container& actual) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ama::hg3
