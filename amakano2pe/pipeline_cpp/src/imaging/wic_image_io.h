// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Windows Imaging Component (WIC) codecs for BgraImage: decode any WIC image
// (JPEG, PNG, BMP) from memory, and encode / save / load PNG.
//
// <windows.h> and <wincodec.h> stay inside wic_image_io.cpp.  Every call
// joins (or creates) a COM apartment for its own duration, so callers need
// no COM setup.  All functions throw std::runtime_error on failure.
#pragma once

#include "imaging/bgra_image.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ama::imaging {

// Decodes `encoded` (JPEG, PNG, BMP, ...) to straight-alpha BGRA.
BgraImage DecodeImage(const std::vector<std::uint8_t>& encoded);

// Lossless 32-bit BGRA PNG.
std::vector<std::uint8_t> EncodePng(const BgraImage& image);

void SavePng(const std::filesystem::path& path, const BgraImage& image);
BgraImage LoadPng(const std::filesystem::path& path);

}  // namespace ama::imaging
