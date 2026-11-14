// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// PIMPL-insulated Windows Imaging Component (WIC) 32-bpp straight-BGRA PNG
// encoder and decoder.
//
// Insulation: <windows.h>, <objbase.h>, and <wincodec.h> stay strictly inside
// wic_image_io.cpp behind WicImageIo::Impl. Each instance initializes COM on
// its calling thread safely (handling S_OK, S_FALSE, and RPC_E_CHANGED_MODE)
// and owns an IWICImagingFactory instance via RAII.
//
// Thread-compatible: separate WicImageIo instances may be used concurrently on
// separate threads.
#pragma once

#include "imaging/bgra_image.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <boost/core/span.hpp>

namespace adi::imaging {

class WicImageIo {
public:
    WicImageIo();
    WicImageIo(const WicImageIo&) = delete;
    WicImageIo& operator=(const WicImageIo&) = delete;
    WicImageIo(WicImageIo&& other) noexcept;
    WicImageIo& operator=(WicImageIo&& other) noexcept;
    ~WicImageIo();

    // Decodes an encoded image (PNG, BMP, JPEG) into straight-alpha 32-bpp BGRA.
    BgraImage DecodePng(boost::span<const std::uint8_t> encoded_bytes) const;

    // Encodes a non-empty `BgraImage` to lossless 32-bpp straight-BGRA PNG bytes.
    std::vector<std::uint8_t> EncodePng(const BgraImage& image) const;

    BgraImage LoadPngFile(const std::filesystem::path& path) const;
    void SavePngFile(const std::filesystem::path& path, const BgraImage& image) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience helpers constructing a short-lived WicImageIo instance.
BgraImage LoadPng(const std::filesystem::path& path);
void SavePng(const std::filesystem::path& path, const BgraImage& image);

}  // namespace adi::imaging
