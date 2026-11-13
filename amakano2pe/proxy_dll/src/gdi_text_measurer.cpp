// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "gdi_text_measurer.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <unordered_map>

namespace gdi_text {

static_assert(kString8LayoutSizesOffset + sizeof(String8LayoutSizes) <=
                  kString8MinReadableBytes,
              "STRING8 layout size table must fit within kString8MinReadableBytes");
static_assert(kString8MeasureWeightOffset + sizeof(int) <=
                  kString8MinReadableBytes,
              "STRING8 MeasureGlyphs weight offset must fit within kString8MinReadableBytes");
static_assert(kString8RenderWeightOffset + sizeof(int) <=
                  kString8MinReadableBytes,
              "STRING8 Render weight offset must fit within kString8MinReadableBytes");

namespace {

constexpr std::size_t kInlineWideBufferSize = 128;
constexpr std::size_t kMaxSpanCacheEntries = 8192;
constexpr int kFallbackGlyphWidthDivisor = 2;
constexpr std::size_t kNullTerminatorChars = 1;
constexpr wchar_t kDefaultFontFace[] = L"\u30E1\u30A4\u30EA\u30AA";

class CriticalSection {
public:
    CriticalSection() noexcept { InitializeCriticalSection(&cs_); }
    ~CriticalSection() { DeleteCriticalSection(&cs_); }

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;

    CRITICAL_SECTION* native_handle() noexcept { return &cs_; }

private:
    CRITICAL_SECTION cs_{};
};

class ScopedLock {
public:
    explicit ScopedLock(CriticalSection& mutex) noexcept : mutex_(mutex) {
        EnterCriticalSection(mutex_.native_handle());
    }
    ~ScopedLock() { LeaveCriticalSection(mutex_.native_handle()); }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    CriticalSection& mutex_;
};

int SafeAbsInt(int value) noexcept {
    if (value == std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::max();
    }
    return std::abs(value);
}

bool IsValidDialogueHeight(int scaled_lf_height) noexcept {
    const int mag = SafeAbsInt(scaled_lf_height);
    return mag >= kMinDialogueFontHeightPx && mag <= kMaxDialogueFontHeightPx;
}

HFONT CreateCatSystem2Font(int negative_lf_height,
                           std::wstring_view face_name,
                           long font_weight) noexcept {
    LOGFONTW lf{};
    lf.lfHeight = negative_lf_height;
    lf.lfWeight = (font_weight > 0) ? font_weight : kDefaultFontWeight;
    lf.lfCharSet = SHIFTJIS_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

    const std::wstring_view effective_face =
        face_name.empty() ? std::wstring_view(kDefaultFontFace) : face_name;
    const std::size_t copy_chars = std::min<std::size_t>(
        effective_face.size(), LF_FACESIZE - kNullTerminatorChars);
    std::wmemcpy(lf.lfFaceName, effective_face.data(), copy_chars);
    lf.lfFaceName[copy_chars] = L'\0';

    return CreateFontIndirectW(&lf);
}

// REQUIRES: memory_dc != nullptr with the active HFONT selected.
bool MeasureUtf8Extent(HDC memory_dc,
                       std::string_view utf8_span,
                       SIZE& out_extent) {
    std::array<wchar_t, kInlineWideBufferSize> stack_wide{};
    const int src_bytes = static_cast<int>(utf8_span.size());
    int wide_len = MultiByteToWideChar(
        CP_UTF8, 0, utf8_span.data(), src_bytes, stack_wide.data(),
        static_cast<int>(stack_wide.size()));
    if (wide_len > 0) {
        return GetTextExtentPoint32W(memory_dc, stack_wide.data(), wide_len,
                                     &out_extent) != FALSE;
    }

    wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8_span.data(), src_bytes,
                                   nullptr, 0);
    if (wide_len <= 0) {
        return false;
    }
    std::wstring heap_wide(static_cast<std::size_t>(wide_len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, 0, utf8_span.data(), src_bytes, heap_wide.data(), wide_len);
    if (written != wide_len) {
        return false;
    }
    return GetTextExtentPoint32W(memory_dc, heap_wide.data(), wide_len,
                                 &out_extent) != FALSE;
}

}  // namespace

bool IsReadableMemoryRange(const void* ptr, std::size_t min_bytes) noexcept {
    if (ptr == nullptr) {
        return false;
    }
    if (min_bytes == 0) {
        return true;
    }
    auto current = reinterpret_cast<std::uintptr_t>(ptr);
    std::size_t remaining_needed = min_bytes;
    while (remaining_needed > 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi,
                         sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            return false;
        }
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        const auto region_base =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::size_t avail_in_region =
            mbi.RegionSize - static_cast<std::size_t>(current - region_base);
        if (avail_in_region == 0) {
            return false;
        }
        if (avail_in_region >= remaining_needed) {
            return true;
        }
        if (current >
            std::numeric_limits<std::uintptr_t>::max() - avail_in_region) {
            return false;
        }
        current += avail_in_region;
        remaining_needed -= avail_in_region;
    }
    return true;
}

namespace {

struct String8SiteTarget {
    const unsigned char* string8 = nullptr;
    std::size_t weight_offset = 0;
};

String8SiteTarget ResolveString8SiteTarget(std::uintptr_t caller_rva,
                                           const void* caller_esi,
                                           const void* caller_ebx,
                                           std::uintptr_t caller_ebp) noexcept {
    if (caller_rva == kMeasureGlyphsFullwidthBaseFontRetRva) {
        return {static_cast<const unsigned char*>(caller_esi),
                kString8MeasureWeightOffset};
    }
    if (caller_rva == kMeasureGlyphsHalfwidthBaseFontRetRva) {
        if (caller_ebp < kMeasureHalfwidthEbpFrameDisp) {
            return {};
        }
        const auto* const frame_slot = reinterpret_cast<const void*>(
            caller_ebp - kMeasureHalfwidthEbpFrameDisp);
        if (!IsReadableMemoryRange(frame_slot, sizeof(std::uintptr_t))) {
            return {};
        }
        std::uintptr_t raw_string8 = 0;
        std::memcpy(&raw_string8, frame_slot, sizeof(raw_string8));
        return {reinterpret_cast<const unsigned char*>(raw_string8),
                kString8MeasureWeightOffset};
    }
    if (caller_rva == kRenderFullwidthBaseFontRetRva) {
        return {static_cast<const unsigned char*>(caller_esi),
                kString8RenderWeightOffset};
    }
    if (caller_rva == kRenderHalfwidthBaseFontRetRva) {
        return {static_cast<const unsigned char*>(caller_ebx),
                kString8RenderWeightOffset};
    }
    return {};
}

}  // namespace

bool ResolveString8BaseFontObservation(
    std::uintptr_t module_base,
    std::uintptr_t caller_ret,
    const void* caller_esi,
    const void* caller_ebx,
    std::uintptr_t caller_ebp,
    String8BaseFontObservation& out) noexcept {
    if (module_base == 0 || caller_ret < module_base) {
        return false;
    }

    const String8SiteTarget target = ResolveString8SiteTarget(
        caller_ret - module_base, caller_esi, caller_ebx, caller_ebp);
    if (target.string8 == nullptr ||
        !IsReadableMemoryRange(target.string8, kString8MinReadableBytes)) {
        return false;
    }

    std::memcpy(out.layout_sizes.data(),
                target.string8 + kString8LayoutSizesOffset,
                sizeof(out.layout_sizes));
    int raw_weight = 0;
    std::memcpy(&raw_weight, target.string8 + target.weight_offset,
                sizeof(raw_weight));
    out.font_weight = (raw_weight > 0) ? static_cast<long>(raw_weight)
                                       : kDefaultFontWeight;
    return true;
}

int ScaleEngineFontHeight(int raw_height) noexcept {
    if (raw_height == std::numeric_limits<int>::min()) {
        return raw_height;
    }
    if (raw_height > 0) {
        return (raw_height * kFontScaleNumerator) / kFontScaleDenominator;
    }
    if (raw_height < 0) {
        return -(((-raw_height) * kFontScaleNumerator) / kFontScaleDenominator);
    }
    return 0;
}

bool IsMeswndDialogueLayoutSize(int raw_height) noexcept {
    const int mag = SafeAbsInt(raw_height);
    return std::any_of(kMeswndDialogueLayoutSizes.begin(),
                       kMeswndDialogueLayoutSizes.end(),
                       [mag](int candidate) { return mag == candidate; });
}

bool IsMeswndString8BaseLayout(const String8LayoutSizes& layout_sizes,
                               int raw_height) noexcept {
    return layout_sizes == kMeswndDialogueLayoutSizes &&
           SafeAbsInt(raw_height) == kMeswndPresetSizeNormal;
}

struct GdiTextWidthMeasurer::Impl {
    mutable CriticalSection mutex;
    HDC memory_dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ original_font = nullptr;
    int active_lf_height = kDefaultScaledDialogueLfHeight;
    long active_weight = kDefaultFontWeight;
    std::wstring active_face = kDefaultFontFace;
    mutable std::unordered_map<std::string, std::size_t> span_cache;

    Impl(int initial_lf_height, std::wstring_view face_name, long font_weight) {
        memory_dc = CreateCompatibleDC(nullptr);
        const int target_height = IsValidDialogueHeight(initial_lf_height)
                                      ? -SafeAbsInt(initial_lf_height)
                                      : kDefaultScaledDialogueLfHeight;
        const std::wstring_view target_face =
            face_name.empty() ? std::wstring_view(kDefaultFontFace) : face_name;
        const long target_weight =
            (font_weight > 0) ? font_weight : kDefaultFontWeight;

        active_lf_height = target_height;
        active_face.assign(target_face.data(), target_face.size());
        active_weight = target_weight;

        if (memory_dc == nullptr) {
            return;
        }
        font = CreateCatSystem2Font(active_lf_height, active_face, active_weight);
        if (font != nullptr) {
            original_font = SelectObject(memory_dc, font);
        }
    }

    ~Impl() {
        if (memory_dc != nullptr && original_font != nullptr) {
            SelectObject(memory_dc, original_font);
            original_font = nullptr;
        }
        if (font != nullptr) {
            DeleteObject(font);
            font = nullptr;
        }
        if (memory_dc != nullptr) {
            DeleteDC(memory_dc);
            memory_dc = nullptr;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool UpdateFont(int scaled_lf_height,
                    std::wstring_view face_name,
                    long font_weight) {
        if (!IsValidDialogueHeight(scaled_lf_height)) {
            return false;
        }
        const int normalized_height = -SafeAbsInt(scaled_lf_height);

        const ScopedLock lock(mutex);
        const std::wstring_view next_face =
            face_name.empty() ? std::wstring_view(active_face) : face_name;
        const long next_weight =
            (font_weight > 0) ? font_weight : active_weight;

        if (normalized_height == active_lf_height &&
            next_weight == active_weight && next_face == active_face &&
            font != nullptr) {
            return true;
        }
        if (memory_dc == nullptr) {
            return false;
        }

        const HFONT next_font =
            CreateCatSystem2Font(normalized_height, next_face, next_weight);
        if (next_font == nullptr) {
            return false;
        }

        const HGDIOBJ prev = SelectObject(memory_dc, next_font);
        if (original_font == nullptr) {
            original_font = prev;
        }
        if (font != nullptr) {
            DeleteObject(font);
        }
        font = next_font;
        active_lf_height = normalized_height;
        active_weight = next_weight;
        if (!face_name.empty()) {
            active_face.assign(face_name.data(), face_name.size());
        }
        span_cache.clear();
        return true;
    }

    int ActiveHeight() const noexcept {
        const ScopedLock lock(mutex);
        return active_lf_height;
    }

    long ActiveWeight() const noexcept {
        const ScopedLock lock(mutex);
        return active_weight;
    }

    bool Valid() const noexcept {
        const ScopedLock lock(mutex);
        return memory_dc != nullptr && font != nullptr;
    }

    std::size_t Measure(std::string_view utf8_span) const {
        if (utf8_span.empty()) {
            return 0;
        }

        const ScopedLock lock(mutex);
        const std::string cache_key(utf8_span);
        const auto cached = span_cache.find(cache_key);
        if (cached != span_cache.end()) {
            return cached->second;
        }

        const std::size_t fallback_px =
            utf8_span.size() *
            static_cast<std::size_t>(SafeAbsInt(active_lf_height)) /
            static_cast<std::size_t>(kFallbackGlyphWidthDivisor);
        if (memory_dc == nullptr || font == nullptr) {
            return fallback_px;
        }

        SIZE extent{};
        if (!MeasureUtf8Extent(memory_dc, utf8_span, extent)) {
            return fallback_px;
        }

        const auto width_px =
            extent.cx > 0 ? static_cast<std::size_t>(extent.cx) : 0u;
        if (span_cache.size() >= kMaxSpanCacheEntries) {
            span_cache.clear();
        }
        span_cache.emplace(cache_key, width_px);
        return width_px;
    }
};

GdiTextWidthMeasurer::GdiTextWidthMeasurer(int initial_lf_height,
                                           std::wstring_view face_name,
                                           long font_weight)
    : impl_(std::make_unique<Impl>(initial_lf_height, face_name, font_weight)) {}

GdiTextWidthMeasurer::~GdiTextWidthMeasurer() = default;

GdiTextWidthMeasurer::GdiTextWidthMeasurer(
    GdiTextWidthMeasurer&&) noexcept = default;

GdiTextWidthMeasurer& GdiTextWidthMeasurer::operator=(
    GdiTextWidthMeasurer&&) noexcept = default;

bool GdiTextWidthMeasurer::UpdateDialogueFont(int scaled_lf_height,
                                              std::wstring_view face_name,
                                              long font_weight) {
    return impl_->UpdateFont(scaled_lf_height, face_name, font_weight);
}

bool GdiTextWidthMeasurer::SetDialogueFontHeight(int scaled_lf_height) {
    return impl_->UpdateFont(scaled_lf_height, {}, 0);
}

int GdiTextWidthMeasurer::SyncFromString8BaseFont(
    int active_slot,
    const String8LayoutSizes* layout_sizes,
    int raw_height,
    std::wstring_view face_name,
    long font_weight) {
    const int scaled = ScaleEngineFontHeight(raw_height);
    if (active_slot != kPrimaryFontContextSlot || layout_sizes == nullptr ||
        !IsMeswndString8BaseLayout(*layout_sizes, raw_height)) {
        return scaled;
    }
    impl_->UpdateFont(scaled, face_name, font_weight);
    return scaled;
}

int GdiTextWidthMeasurer::ActiveFontHeight() const noexcept {
    return impl_->ActiveHeight();
}

long GdiTextWidthMeasurer::ActiveFontWeight() const noexcept {
    return impl_->ActiveWeight();
}

bool GdiTextWidthMeasurer::IsValid() const noexcept {
    return impl_->Valid();
}

std::size_t GdiTextWidthMeasurer::MeasureSpanWidth(
    std::string_view utf8_span) const {
    return impl_->Measure(utf8_span);
}

}  // namespace gdi_text
