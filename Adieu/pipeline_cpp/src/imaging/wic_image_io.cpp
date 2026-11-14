// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "imaging/wic_image_io.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace adi::imaging {
namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        Reset();
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
        return *this;
    }

    ~ComPtr() { Reset(); }

    void Reset() noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T** put() noexcept {
        Reset();
        return &ptr_;
    }

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }

private:
    T* ptr_ = nullptr;
};

class ScopedComApartment {
public:
    ScopedComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == S_OK || hr == S_FALSE) {
            should_uninit_ = true;
            return;
        }
        if (hr == RPC_E_CHANGED_MODE) {
            return;
        }
        std::ostringstream oss;
        oss << "CoInitializeEx failed with HRESULT 0x" << std::hex
            << static_cast<unsigned long>(hr);
        throw std::runtime_error(oss.str());
    }

    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;

    ~ScopedComApartment() {
        if (should_uninit_) {
            CoUninitialize();
        }
    }

private:
    bool should_uninit_ = false;
};

void CheckHResult(HRESULT hr, const char* step) {
    if (SUCCEEDED(hr)) {
        return;
    }
    std::ostringstream oss;
    oss << step << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    throw std::runtime_error(oss.str());
}

UINT CheckedRowStrideBytes(UINT width) {
    constexpr UINT kBytesPerPixel = static_cast<UINT>(kBgraBytesPerPixel);
    if (width == 0u || width > std::numeric_limits<UINT>::max() / kBytesPerPixel) {
        throw std::runtime_error("WIC image row stride overflow");
    }
    return width * kBytesPerPixel;
}

UINT CheckedBufferBytes(UINT stride, UINT height) {
    if (height == 0u || stride > std::numeric_limits<UINT>::max() / height) {
        throw std::runtime_error("WIC image byte buffer overflow");
    }
    return stride * height;
}

}  // namespace

struct WicImageIo::Impl {
    ScopedComApartment com;
    ComPtr<IWICImagingFactory> factory;

    Impl() {
        CheckHResult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory.put())),
                     "CoCreateInstance(CLSID_WICImagingFactory)");
    }
};

WicImageIo::WicImageIo() : impl_(std::make_unique<Impl>()) {}
WicImageIo::WicImageIo(WicImageIo&& other) noexcept = default;
WicImageIo& WicImageIo::operator=(WicImageIo&& other) noexcept = default;
WicImageIo::~WicImageIo() = default;

BgraImage WicImageIo::DecodePng(boost::span<const std::uint8_t> encoded_bytes) const {
    if (encoded_bytes.empty()) {
        throw std::runtime_error("WicImageIo::DecodePng received empty input buffer");
    }
    if (encoded_bytes.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        throw std::runtime_error("WicImageIo::DecodePng input exceeds WIC stream limit");
    }

    ComPtr<IWICStream> stream;
    CheckHResult(impl_->factory->CreateStream(stream.put()), "IWICImagingFactory::CreateStream");
    CheckHResult(stream->InitializeFromMemory(
                     const_cast<BYTE*>(reinterpret_cast<const BYTE*>(encoded_bytes.data())),
                     static_cast<DWORD>(encoded_bytes.size())),
                 "IWICStream::InitializeFromMemory");

    ComPtr<IWICBitmapDecoder> decoder;
    CheckHResult(impl_->factory->CreateDecoderFromStream(
                     stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()),
                 "IWICImagingFactory::CreateDecoderFromStream");

    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHResult(decoder->GetFrame(0u, frame.put()), "IWICBitmapDecoder::GetFrame");

    UINT width = 0u;
    UINT height = 0u;
    CheckHResult(frame->GetSize(&width, &height), "IWICBitmapFrameDecode::GetSize");
    if (width == 0u || height == 0u ||
        width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
        height > static_cast<UINT>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("WicImageIo::DecodePng invalid image dimensions");
    }

    ComPtr<IWICFormatConverter> converter;
    CheckHResult(impl_->factory->CreateFormatConverter(converter.put()),
                 "IWICImagingFactory::CreateFormatConverter");
    CheckHResult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom),
                 "IWICFormatConverter::Initialize(32bppBGRA)");

    const UINT stride = CheckedRowStrideBytes(width);
    const UINT buffer_bytes = CheckedBufferBytes(stride, height);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(buffer_bytes));
    CheckHResult(converter->CopyPixels(nullptr, stride, buffer_bytes, pixels.data()),
                 "IWICFormatConverter::CopyPixels");

    return BgraImage(static_cast<int>(width), static_cast<int>(height), std::move(pixels));
}

std::vector<std::uint8_t> WicImageIo::EncodePng(const BgraImage& image) const {
    if (image.empty()) {
        throw std::invalid_argument("WicImageIo::EncodePng cannot encode an empty BgraImage");
    }
    const UINT width = static_cast<UINT>(image.width());
    const UINT height = static_cast<UINT>(image.height());
    const UINT stride = CheckedRowStrideBytes(width);
    const UINT buffer_bytes = CheckedBufferBytes(stride, height);

    ComPtr<IStream> stream;
    CheckHResult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()), "CreateStreamOnHGlobal");

    ComPtr<IWICBitmapEncoder> encoder;
    CheckHResult(impl_->factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()),
                 "IWICImagingFactory::CreateEncoder(PNG)");
    CheckHResult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache),
                 "IWICBitmapEncoder::Initialize");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    CheckHResult(encoder->CreateNewFrame(frame.put(), props.put()),
                 "IWICBitmapEncoder::CreateNewFrame");
    CheckHResult(frame->Initialize(props.get()), "IWICBitmapFrameEncode::Initialize");
    CheckHResult(frame->SetSize(width, height), "IWICBitmapFrameEncode::SetSize");

    WICPixelFormatGUID target_format = GUID_WICPixelFormat32bppBGRA;
    CheckHResult(frame->SetPixelFormat(&target_format), "IWICBitmapFrameEncode::SetPixelFormat");

    if (std::memcmp(&target_format, &GUID_WICPixelFormat32bppBGRA, sizeof(GUID)) == 0) {
        CheckHResult(frame->WritePixels(height, stride, buffer_bytes,
                                        const_cast<BYTE*>(image.data())),
                     "IWICBitmapFrameEncode::WritePixels");
    } else {
        ComPtr<IWICBitmap> src_bitmap;
        CheckHResult(impl_->factory->CreateBitmapFromMemory(
                         width, height, GUID_WICPixelFormat32bppBGRA, stride, buffer_bytes,
                         const_cast<BYTE*>(image.data()), src_bitmap.put()),
                     "IWICImagingFactory::CreateBitmapFromMemory");
        CheckHResult(frame->WriteSource(src_bitmap.get(), nullptr),
                     "IWICBitmapFrameEncode::WriteSource");
    }

    CheckHResult(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    CheckHResult(encoder->Commit(), "IWICBitmapEncoder::Commit");

    STATSTG stat{};
    CheckHResult(stream->Stat(&stat, STATFLAG_NONAME), "IStream::Stat");
    if (stat.cbSize.QuadPart == 0u ||
        stat.cbSize.QuadPart > static_cast<ULONGLONG>(std::numeric_limits<ULONG>::max())) {
        throw std::runtime_error("WicImageIo::EncodePng unexpected PNG stream size");
    }

    LARGE_INTEGER zero{};
    CheckHResult(stream->Seek(zero, STREAM_SEEK_SET, nullptr), "IStream::Seek");

    const auto total_size = static_cast<ULONG>(stat.cbSize.QuadPart);
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(total_size));
    ULONG bytes_read = 0u;
    CheckHResult(stream->Read(encoded.data(), total_size, &bytes_read), "IStream::Read");
    if (bytes_read != total_size) {
        throw std::runtime_error("WicImageIo::EncodePng incomplete stream read");
    }
    return encoded;
}

BgraImage WicImageIo::LoadPngFile(const std::filesystem::path& path) const {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("WicImageIo::LoadPngFile failed to open: " + path.u8string());
    }
    const std::streamsize end_pos = ifs.tellg();
    if (end_pos <= 0) {
        throw std::runtime_error("WicImageIo::LoadPngFile empty or unreadable file: " +
                                 path.u8string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end_pos));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(bytes.data()), end_pos)) {
        throw std::runtime_error("WicImageIo::LoadPngFile read error: " + path.u8string());
    }
    return DecodePng(bytes);
}

void WicImageIo::SavePngFile(const std::filesystem::path& path, const BgraImage& image) const {
    const std::vector<std::uint8_t> png_bytes = EncodePng(image);
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("WicImageIo::SavePngFile failed to open: " + path.u8string());
    }
    ofs.write(reinterpret_cast<const char*>(png_bytes.data()),
              static_cast<std::streamsize>(png_bytes.size()));
    if (!ofs) {
        throw std::runtime_error("WicImageIo::SavePngFile write error: " + path.u8string());
    }
}

BgraImage LoadPng(const std::filesystem::path& path) {
    const WicImageIo io;
    return io.LoadPngFile(path);
}

void SavePng(const std::filesystem::path& path, const BgraImage& image) {
    const WicImageIo io;
    io.SavePngFile(path, image);
}

}  // namespace adi::imaging
