// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "imaging/wic_image_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace ama::imaging {

namespace {

using Microsoft::WRL::ComPtr;

// Joins the calling thread's COM apartment for the lifetime of the object
// (multithreaded if the thread has none yet).
class ScopedComApartment {
public:
    ScopedComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_ = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("CoInitializeEx failed");
        }
    }
    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;
    ~ScopedComApartment() {
        if (owns_) {
            CoUninitialize();
        }
    }

private:
    bool owns_ = false;
};

constexpr std::size_t kHresultTextCapacity = 16;

std::string HresultText(HRESULT hr) {
    char buf[kHresultTextCapacity];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

void Check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) {
        return;
    }
    throw std::runtime_error(std::string("WIC: ") + what + " failed (HRESULT " +
                             HresultText(hr) + ")");
}

ComPtr<IWICImagingFactory> CreateFactory() {
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)),
          "CoCreateInstance(CLSID_WICImagingFactory)");
    return factory;
}

UINT StrideOf(int width) {
    return static_cast<UINT>(width) * static_cast<UINT>(kBgraBytesPerPixel);
}

BgraImage DecodeWithFactory(IWICImagingFactory* factory,
                            const std::vector<std::uint8_t>& encoded) {
    if (encoded.empty()) {
        throw std::runtime_error("WIC: empty image buffer");
    }
    if (encoded.size() > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error("WIC: image buffer too large");
    }
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream), "CreateStream");
    Check(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),
                                       static_cast<DWORD>(encoded.size())),
          "InitializeFromMemory");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                           WICDecodeMetadataCacheOnDemand, &decoder),
          "CreateDecoderFromStream");
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame), "GetFrame");
    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter), "CreateFormatConverter");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom),
          "FormatConverter::Initialize");
    UINT width = 0;
    UINT height = 0;
    Check(converter->GetSize(&width, &height), "GetSize");
    BgraImage image = MakeBgraImage(static_cast<int>(width), static_cast<int>(height));
    if (image.pixels.empty()) {
        return image;
    }
    Check(converter->CopyPixels(nullptr, StrideOf(image.width),
                                static_cast<UINT>(image.pixels.size()), image.pixels.data()),
          "CopyPixels");
    return image;
}

std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.u8string());
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

}  // namespace

BgraImage DecodeImage(const std::vector<std::uint8_t>& encoded) {
    const ScopedComApartment apartment;
    const ComPtr<IWICImagingFactory> factory = CreateFactory();
    return DecodeWithFactory(factory.Get(), encoded);
}

std::vector<std::uint8_t> EncodePng(const BgraImage& image) {
    if (image.width <= 0 || image.height <= 0) {
        throw std::runtime_error("WIC: cannot encode an empty image as PNG");
    }
    const ScopedComApartment apartment;
    const ComPtr<IWICImagingFactory> factory = CreateFactory();

    ComPtr<IStream> memory;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &memory), "CreateStreamOnHGlobal");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder");
    Check(encoder->Initialize(memory.Get(), WICBitmapEncoderNoCache), "Encoder::Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    Check(frame->Initialize(nullptr), "Frame::Initialize");
    Check(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)),
          "SetSize");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(frame->SetPixelFormat(&format), "SetPixelFormat");
    if (format != GUID_WICPixelFormat32bppBGRA) {
        throw std::runtime_error("WIC: PNG encoder rejected 32bpp BGRA");
    }
    Check(frame->WritePixels(static_cast<UINT>(image.height), StrideOf(image.width),
                             static_cast<UINT>(image.pixels.size()),
                             const_cast<BYTE*>(image.pixels.data())),
          "WritePixels");
    Check(frame->Commit(), "Frame::Commit");
    Check(encoder->Commit(), "Encoder::Commit");

    STATSTG stat{};
    Check(memory->Stat(&stat, STATFLAG_NONAME), "Stat");
    const LARGE_INTEGER origin{};
    Check(memory->Seek(origin, STREAM_SEEK_SET, nullptr), "Seek");
    std::vector<std::uint8_t> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    Check(memory->Read(out.data(), static_cast<ULONG>(out.size()), &read), "Read");
    out.resize(read);
    return out;
}

void SavePng(const std::filesystem::path& path, const BgraImage& image) {
    const std::vector<std::uint8_t> png = EncodePng(image);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot create " + path.u8string());
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        throw std::runtime_error("write failed: " + path.u8string());
    }
}

BgraImage LoadPng(const std::filesystem::path& path) {
    return DecodeImage(ReadAllBytes(path));
}

}  // namespace ama::imaging
