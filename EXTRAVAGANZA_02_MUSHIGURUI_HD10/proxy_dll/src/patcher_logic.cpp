// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "patcher_logic.h"

#include <algorithm>
#include <cstring>

namespace patcher_logic {

namespace {

constexpr std::size_t kDosHeaderMinBytes = 0x40;
constexpr std::uint16_t kDosMagicMz = 0x5A4Du;
constexpr std::size_t kDosLfanewOffset = 0x3C;

constexpr std::size_t kPeSignatureBytes = 4;
constexpr std::uint32_t kPeSignaturePe00 = 0x00004550u;

constexpr std::size_t kCoffFileHeaderBytes = 20;
constexpr std::size_t kCoffMachineOffset = 0;
constexpr std::uint16_t kImageFileMachineI386 = 0x014Cu;
constexpr std::size_t kCoffNumberOfSectionsOffset = 2;
constexpr std::size_t kCoffTimeDateStampOffset = 4;
constexpr std::size_t kCoffSizeOfOptionalHeaderOffset = 16;

constexpr std::size_t kOptionalHeaderMinBytes = 0x60;
constexpr std::uint16_t kPe32OptionalHeaderMagic = 0x010Bu;
constexpr std::size_t kOptMagicOffset = 0;
constexpr std::size_t kOptSizeOfImageOffset = 0x38;

constexpr std::size_t kPeSectionHeaderBytes = 40;
constexpr std::size_t kPeSectionVirtualSizeOffset = 8;
constexpr std::size_t kPeSectionVirtualAddressOffset = 12;
constexpr std::size_t kPeSectionSizeOfRawDataOffset = 16;
constexpr std::size_t kPeSectionCharacteristicsOffset = 36;

constexpr std::size_t kDistinctCodegenFamilyCount = 2;

constexpr int kDwordByteCount = 4;
constexpr int kMinPrintableBytesForAscii = 3;
constexpr std::uint8_t kPrintableAsciiMin = 0x20;
constexpr std::uint8_t kPrintableAsciiMax = 0x7E;

std::uint16_t ReadU16Le(const std::uint8_t* p) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint32_t ReadU32Le(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

bool RangeFitsInSection(SectionSpan span,
                        std::uint32_t rva,
                        std::size_t byte_len) noexcept {
    if (span.virtual_size == 0 || rva < span.rva) {
        return false;
    }
    const std::uint32_t offset = rva - span.rva;
    if (offset > span.virtual_size) {
        return false;
    }
    return byte_len <= static_cast<std::size_t>(span.virtual_size - offset);
}

std::size_t PrintMessageClusterSpanBytes(const EditionProfile& profile) noexcept {
    std::size_t max_end = kPrintMessageSentinelLen;
    for (const PatchDeltaSpec& patch : profile.patches) {
        const std::size_t end =
            static_cast<std::size_t>(patch.delta_from_sentinel) + patch.len;
        if (end > max_end) {
            max_end = end;
        }
    }
    return max_end;
}

std::size_t RenderCoreRequiredSpanBytes(const EditionProfile& profile) noexcept {
    if (profile.has_render_core_secondary_anchor) {
        return kRenderCoreSignatureSpan;
    }
    return profile.text_render_core_hook_len;
}

const EditionProfile& Preferred2019FamilyProfile(SectionSpan text_span,
                                                 std::uint32_t time_date_stamp) noexcept {
    if (time_date_stamp == kHd10Win7TimeDateStamp) {
        return kHd10Win7Profile;
    }
    if (time_date_stamp == kHd10TimeDateStamp) {
        return kHd10Profile;
    }
    if (text_span.virtual_size == kHd10Win7TextVirtualSize) {
        return kHd10Win7Profile;
    }
    return kHd10Profile;
}

template <typename Predicate>
std::optional<std::uint32_t> ScanUniqueRvaInSection(SectionSpan text_span,
                                                    std::size_t required_span_bytes,
                                                    Predicate matches_at_rva) noexcept {
    if (required_span_bytes == 0 ||
        text_span.virtual_size < required_span_bytes) {
        return std::nullopt;
    }

    const std::uint32_t max_offset =
        text_span.virtual_size - static_cast<std::uint32_t>(required_span_bytes);
    std::optional<std::uint32_t> found_rva;
    for (std::uint32_t offset = 0; offset <= max_offset; ++offset) {
        const std::uint32_t rva = text_span.rva + offset;
        if (!matches_at_rva(rva)) {
            continue;
        }
        if (found_rva.has_value()) {
            return std::nullopt;
        }
        found_rva = rva;
    }
    return found_rva;
}

std::optional<ResolvedTranslatorHooks> ScanTranslatorHooksForProfile(
    const EditionProfile& profile,
    const std::uint8_t* image_base,
    SectionSpan text_span) noexcept {
    const auto splitter_rva = ScanUniqueRvaInSection(
        text_span,
        profile.text_splitter_hook_len,
        [&](std::uint32_t rva) {
            return MatchesTextSplitterSignature(profile, image_base, text_span, rva);
        });
    if (!splitter_rva.has_value()) {
        return std::nullopt;
    }

    const auto render_core_rva = ScanUniqueRvaInSection(
        text_span,
        RenderCoreRequiredSpanBytes(profile),
        [&](std::uint32_t rva) {
            return MatchesTextRenderCoreSignature(profile, image_base, text_span, rva);
        });
    if (!render_core_rva.has_value()) {
        return std::nullopt;
    }

    const auto font_setup_rva = ScanUniqueRvaInSection(
        text_span,
        kFontSetupSignatureSpan,
        [&](std::uint32_t rva) {
            return MatchesFontSetupSignature(profile, image_base, text_span, rva);
        });
    if (!font_setup_rva.has_value()) {
        return std::nullopt;
    }

    return ResolvedTranslatorHooks{
        &profile,
        *splitter_rva,
        *render_core_rva,
        *font_setup_rva,
        true,
    };
}

}  // namespace

const EditionProfile& GetEditionProfile(EditionKind kind) noexcept {
    switch (kind) {
    case EditionKind::HD10:
        return kHd10Profile;
    case EditionKind::HD10_Win7:
        return kHd10Win7Profile;
    case EditionKind::CompleteEdition:
        return kCompleteEditionProfile;
    }
    return kHd10Profile;
}

bool BytesMatch(const unsigned char* addr,
                const unsigned char* expected,
                std::size_t len) {
    if (addr == nullptr || expected == nullptr) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    return std::memcmp(addr, expected, len) == 0;
}

void FillNops(unsigned char* dst, std::size_t len) {
    if (dst == nullptr || len == 0) {
        return;
    }
    std::memset(dst, kNopOpcode, len);
}

std::int32_t ComputeRel32(std::uintptr_t instr_start,
                          std::uintptr_t target) {
    const std::uint32_t post_ip =
        static_cast<std::uint32_t>(instr_start) +
        static_cast<std::uint32_t>(kRelBranchInstructionSize);
    const std::uint32_t diff =
        static_cast<std::uint32_t>(target) - post_ip;
    return static_cast<std::int32_t>(diff);
}

unsigned char* EmitRelOp32(unsigned char* out,
                           unsigned char opcode,
                           std::uintptr_t target) {
    out[0] = opcode;
    const std::int32_t rel =
        ComputeRel32(reinterpret_cast<std::uintptr_t>(out), target);
    std::memcpy(out + 1, &rel, sizeof(rel));
    return out + kRelBranchInstructionSize;
}

unsigned char* EmitBytes(unsigned char* out,
                         const unsigned char* src,
                         std::size_t len) {
    if (len > 0) {
        std::memcpy(out, src, len);
    }
    return out + len;
}

bool LooksLikeAscii(std::uint32_t val) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&val);
    int printable = 0;
    for (int i = 0; i < kDwordByteCount; ++i) {
        if (bytes[i] >= kPrintableAsciiMin && bytes[i] <= kPrintableAsciiMax) {
            ++printable;
        }
    }
    return printable >= kMinPrintableBytesForAscii;
}

bool IsValidSpec(const PatchSpec& s) {
    if (s.expected == nullptr) {
        return false;
    }
    if (s.len == 0) {
        return false;
    }
    if (s.replacement != nullptr && s.replacement == s.expected) {
        return false;
    }
    return true;
}

std::size_t BuildDetourTrampoline(std::uint8_t* trampoline_buf,
                                  std::uintptr_t trampoline_va,
                                  const std::uint8_t* hook_site_bytes,
                                  std::uintptr_t hook_site_va,
                                  std::size_t hook_len) {
    if (trampoline_buf == nullptr || hook_site_bytes == nullptr) {
        return 0;
    }
    if (hook_len < kRelBranchInstructionSize || hook_len > kMaxHookPrologueLen) {
        return 0;
    }

    std::memcpy(trampoline_buf, hook_site_bytes, hook_len);
    trampoline_buf[hook_len] = kRelJmpOpcode;
    const std::int32_t jmp_back_rel = ComputeRel32(
        trampoline_va + hook_len,
        hook_site_va + hook_len);
    std::memcpy(trampoline_buf + hook_len + 1, &jmp_back_rel, sizeof(jmp_back_rel));
    return hook_len + kRelBranchInstructionSize;
}

bool WriteDetourJump(std::uint8_t* hook_site_buf,
                     std::uintptr_t hook_site_va,
                     std::size_t hook_len,
                     std::uintptr_t detour_target_va) {
    if (hook_site_buf == nullptr) {
        return false;
    }
    if (hook_len < kRelBranchInstructionSize || hook_len > kMaxHookPrologueLen) {
        return false;
    }

    hook_site_buf[0] = kRelJmpOpcode;
    const std::int32_t detour_rel = ComputeRel32(hook_site_va, detour_target_va);
    std::memcpy(hook_site_buf + 1, &detour_rel, sizeof(detour_rel));
    FillNops(hook_site_buf + kRelBranchInstructionSize,
             hook_len - kRelBranchInstructionSize);
    return true;
}

std::optional<PeImageLayout> ParsePeImageLayout(const std::uint8_t* image_base,
                                                std::size_t max_header_bytes) {
    if (image_base == nullptr || max_header_bytes < kDosHeaderMinBytes) {
        return std::nullopt;
    }
    if (ReadU16Le(image_base) != kDosMagicMz) {
        return std::nullopt;
    }

    const std::uint32_t lfanew = ReadU32Le(image_base + kDosLfanewOffset);
    if (lfanew > max_header_bytes ||
        max_header_bytes - lfanew < kPeSignatureBytes + kCoffFileHeaderBytes) {
        return std::nullopt;
    }

    const std::uint8_t* nt_headers = image_base + lfanew;
    if (ReadU32Le(nt_headers) != kPeSignaturePe00) {
        return std::nullopt;
    }

    const std::uint8_t* coff_header = nt_headers + kPeSignatureBytes;
    if (ReadU16Le(coff_header + kCoffMachineOffset) != kImageFileMachineI386) {
        return std::nullopt;
    }

    const std::uint16_t num_sections =
        ReadU16Le(coff_header + kCoffNumberOfSectionsOffset);
    const std::uint32_t time_date_stamp =
        ReadU32Le(coff_header + kCoffTimeDateStampOffset);
    const std::uint16_t opt_header_size =
        ReadU16Le(coff_header + kCoffSizeOfOptionalHeaderOffset);
    if (num_sections == 0 || opt_header_size < kOptionalHeaderMinBytes) {
        return std::nullopt;
    }

    const std::size_t opt_offset = lfanew + kPeSignatureBytes + kCoffFileHeaderBytes;
    const std::size_t sections_bytes =
        static_cast<std::size_t>(num_sections) * kPeSectionHeaderBytes;
    if (opt_offset > max_header_bytes ||
        opt_header_size > max_header_bytes - opt_offset ||
        sections_bytes > max_header_bytes - opt_offset - opt_header_size) {
        return std::nullopt;
    }

    const std::uint8_t* opt_header = image_base + opt_offset;
    if (ReadU16Le(opt_header + kOptMagicOffset) != kPe32OptionalHeaderMagic) {
        return std::nullopt;
    }

    const std::uint32_t size_of_image = ReadU32Le(opt_header + kOptSizeOfImageOffset);
    if (size_of_image == 0) {
        return std::nullopt;
    }

    const std::uint8_t* section_table = opt_header + opt_header_size;
    for (std::uint16_t idx = 0; idx < num_sections; ++idx) {
        const std::uint8_t* sec = section_table + idx * kPeSectionHeaderBytes;
        const std::uint32_t virtual_size = ReadU32Le(sec + kPeSectionVirtualSizeOffset);
        const std::uint32_t virtual_address = ReadU32Le(sec + kPeSectionVirtualAddressOffset);
        const std::uint32_t size_of_raw_data = ReadU32Le(sec + kPeSectionSizeOfRawDataOffset);
        const std::uint32_t characteristics = ReadU32Le(sec + kPeSectionCharacteristicsOffset);
        if ((characteristics & kImageScnMemExecute) == 0) {
            continue;
        }
        if (virtual_address == 0 ||
            virtual_address >= size_of_image ||
            (virtual_size == 0 && size_of_raw_data == 0)) {
            continue;
        }
        const std::uint32_t mapped_span = std::min(
            std::max(virtual_size, size_of_raw_data),
            size_of_image - virtual_address);
        return PeImageLayout{
            SectionSpan{virtual_address, mapped_span},
            size_of_image,
            time_date_stamp,
        };
    }

    return std::nullopt;
}

std::optional<SectionSpan> ParsePeTextSectionBounds(const std::uint8_t* image_base,
                                                    std::size_t max_header_bytes) {
    const auto layout = ParsePeImageLayout(image_base, max_header_bytes);
    if (!layout.has_value()) {
        return std::nullopt;
    }
    return layout->text_section;
}

bool IsAddressInsideSection(std::uintptr_t addr,
                            std::uintptr_t image_base,
                            SectionSpan span) noexcept {
    if (span.virtual_size == 0 || addr < image_base) {
        return false;
    }
    const std::uintptr_t rva = addr - image_base;
    if (rva < span.rva) {
        return false;
    }
    return (rva - span.rva) < span.virtual_size;
}

bool IsBenignPackerSehException(std::uint32_t exception_code,
                                std::uintptr_t fault_addr,
                                std::uintptr_t image_base,
                                const PeImageLayout& pe_layout) noexcept {
    const bool is_packer_probe_code =
        (exception_code == kSehIntDivideByZero ||
         exception_code == kSehIllegalInstruction ||
         exception_code == kSehPrivInstruction);
    if (!is_packer_probe_code) {
        return false;
    }
    const SectionSpan full_image{0, pe_layout.size_of_image};
    if (!IsAddressInsideSection(fault_addr, image_base, full_image)) {
        return false;
    }
    return !IsAddressInsideSection(fault_addr, image_base, pe_layout.text_section);
}

bool MatchesPrintMessageCluster(const EditionProfile& profile,
                                const std::uint8_t* image_base,
                                SectionSpan text_span,
                                std::uint32_t sentinel_rva) noexcept {
    if (image_base == nullptr) {
        return false;
    }
    const std::size_t cluster_span = PrintMessageClusterSpanBytes(profile);
    if (!RangeFitsInSection(text_span, sentinel_rva, cluster_span)) {
        return false;
    }
    if (!BytesMatch(image_base + sentinel_rva,
                    profile.sentinel.data(),
                    profile.sentinel.size())) {
        return false;
    }
    for (const PatchDeltaSpec& patch : profile.patches) {
        const std::uint32_t site_rva = sentinel_rva + patch.delta_from_sentinel;
        if (!BytesMatch(image_base + site_rva, patch.expected, patch.len)) {
            return false;
        }
    }
    return true;
}

bool MatchesTextSplitterSignature(const EditionProfile& profile,
                                  const std::uint8_t* image_base,
                                  SectionSpan text_span,
                                  std::uint32_t candidate_rva) noexcept {
    if (image_base == nullptr) {
        return false;
    }
    if (!RangeFitsInSection(text_span, candidate_rva, profile.text_splitter_hook_len)) {
        return false;
    }
    return BytesMatch(image_base + candidate_rva,
                      profile.text_splitter_prologue.data(),
                      profile.text_splitter_hook_len);
}

bool MatchesTextRenderCoreSignature(const EditionProfile& profile,
                                    const std::uint8_t* image_base,
                                    SectionSpan text_span,
                                    std::uint32_t candidate_rva) noexcept {
    if (image_base == nullptr) {
        return false;
    }
    const std::size_t required_span = RenderCoreRequiredSpanBytes(profile);
    if (!RangeFitsInSection(text_span, candidate_rva, required_span)) {
        return false;
    }
    if (!BytesMatch(image_base + candidate_rva,
                    profile.text_render_core_prologue.data(),
                    profile.text_render_core_hook_len)) {
        return false;
    }
    if (!profile.has_render_core_secondary_anchor) {
        return true;
    }
    return BytesMatch(image_base + candidate_rva + kRenderCoreSecondaryAnchorOffset,
                      profile.render_core_secondary_anchor.data(),
                      profile.render_core_secondary_anchor.size());
}

bool MatchesFontSetupSignature(const EditionProfile& profile,
                               const std::uint8_t* image_base,
                               SectionSpan text_span,
                               std::uint32_t candidate_rva) noexcept {
    if (image_base == nullptr) {
        return false;
    }
    if (!RangeFitsInSection(text_span, candidate_rva, kFontSetupSignatureSpan)) {
        return false;
    }
    if (!BytesMatch(image_base + candidate_rva,
                    profile.font_setup_head5.data(),
                    profile.font_setup_head5.size())) {
        return false;
    }
    return BytesMatch(image_base + candidate_rva + kFontSetupTailOffset,
                      profile.font_setup_tail6.data(),
                      profile.font_setup_tail6.size());
}

std::array<PatchSpec, kPrintMessagePatchCount> BuildPrintMessagePatchSpecs(
    const EditionProfile& profile,
    std::uint32_t sentinel_rva) noexcept {
    std::array<PatchSpec, kPrintMessagePatchCount> specs{};
    for (std::size_t i = 0; i < kPrintMessagePatchCount; ++i) {
        const PatchDeltaSpec& delta = profile.patches[i];
        specs[i] = PatchSpec{
            sentinel_rva + delta.delta_from_sentinel,
            delta.expected,
            delta.replacement,
            delta.len,
        };
    }
    return specs;
}

std::optional<ResolvedPrintMessageTarget> ResolvePrintMessageTarget(
    const std::uint8_t* image_base,
    SectionSpan text_span,
    std::uint32_t time_date_stamp) noexcept {
    if (image_base == nullptr || text_span.virtual_size == 0) {
        return std::nullopt;
    }

    // Fast O(1) verification at each edition's exact expected sentinel RVA.
    for (const EditionProfile& profile : kEditionProfiles) {
        if (MatchesPrintMessageCluster(profile, image_base, text_span, profile.sentinel_rva)) {
            return ResolvedPrintMessageTarget{&profile, profile.sentinel_rva, false};
        }
    }

    // Fallback: unique `.text` scan across distinct pattern families
    // (2019 MSVC 14.16 family vs 2014 Complete Edition family).
    const EditionProfile& family_2019 =
        Preferred2019FamilyProfile(text_span, time_date_stamp);
    const std::array<const EditionProfile*, kDistinctCodegenFamilyCount> scan_families = {
        &family_2019,
        &kCompleteEditionProfile,
    };

    std::optional<ResolvedPrintMessageTarget> found;
    for (const EditionProfile* profile : scan_families) {
        const auto scanned_rva = ScanUniqueRvaInSection(
            text_span,
            PrintMessageClusterSpanBytes(*profile),
            [&](std::uint32_t rva) {
                return MatchesPrintMessageCluster(*profile, image_base, text_span, rva);
            });
        if (!scanned_rva.has_value()) {
            continue;
        }
        if (found.has_value()) {
            return std::nullopt;
        }
        found = ResolvedPrintMessageTarget{profile, *scanned_rva, true};
    }
    return found;
}

std::optional<ResolvedTranslatorHooks> ResolveTranslatorHooks(
    const std::uint8_t* image_base,
    SectionSpan text_span,
    std::uint32_t time_date_stamp) noexcept {
    if (image_base == nullptr || text_span.virtual_size == 0) {
        return std::nullopt;
    }

    // Fast O(1) verification at each edition's exact expected hook RVAs.
    for (const EditionProfile& profile : kEditionProfiles) {
        if (MatchesTextSplitterSignature(
                profile, image_base, text_span, profile.text_splitter_rva) &&
            MatchesTextRenderCoreSignature(
                profile, image_base, text_span, profile.text_render_core_rva) &&
            MatchesFontSetupSignature(
                profile, image_base, text_span, profile.font_setup_rva)) {
            return ResolvedTranslatorHooks{
                &profile,
                profile.text_splitter_rva,
                profile.text_render_core_rva,
                profile.font_setup_rva,
                false,
            };
        }
    }

    // Fallback: unique `.text` scan across distinct pattern families.
    const EditionProfile& family_2019 =
        Preferred2019FamilyProfile(text_span, time_date_stamp);
    const std::array<const EditionProfile*, kDistinctCodegenFamilyCount> scan_families = {
        &family_2019,
        &kCompleteEditionProfile,
    };

    std::optional<ResolvedTranslatorHooks> found;
    for (const EditionProfile* profile : scan_families) {
        const auto scanned =
            ScanTranslatorHooksForProfile(*profile, image_base, text_span);
        if (!scanned.has_value()) {
            continue;
        }
        if (found.has_value()) {
            return std::nullopt;
        }
        found = scanned;
    }
    return found;
}

std::optional<ResolvedEditionTargets> ResolveEditionTargets(
    const std::uint8_t* image_base,
    SectionSpan text_span,
    std::uint32_t time_date_stamp) noexcept {
    const auto pm = ResolvePrintMessageTarget(image_base, text_span, time_date_stamp);
    if (!pm.has_value()) {
        return std::nullopt;
    }
    const auto hooks = ResolveTranslatorHooks(image_base, text_span, time_date_stamp);
    if (!hooks.has_value() || hooks->profile->kind != pm->profile->kind) {
        return std::nullopt;
    }
    return ResolvedEditionTargets{
        pm->profile,
        pm->sentinel_rva,
        hooks->text_splitter_rva,
        hooks->text_render_core_rva,
        hooks->font_setup_rva,
        pm->used_scan_fallback || hooks->used_scan_fallback,
    };
}

bool ApplyPrintMessagePatchesInMemory(std::uint8_t* image_base,
                                      SectionSpan text_span,
                                      const EditionProfile& profile,
                                      std::uint32_t sentinel_rva) noexcept {
    if (image_base == nullptr) {
        return false;
    }
    if (!MatchesPrintMessageCluster(profile, image_base, text_span, sentinel_rva)) {
        return false;
    }

    const auto specs = BuildPrintMessagePatchSpecs(profile, sentinel_rva);
    for (const PatchSpec& spec : specs) {
        if (!IsValidSpec(spec)) {
            return false;
        }
        std::uint8_t* dst = image_base + spec.rva;
        if (spec.replacement == nullptr) {
            FillNops(dst, spec.len);
            continue;
        }
        std::memcpy(dst, spec.replacement, spec.len);
    }
    return true;
}

}  // namespace patcher_logic
