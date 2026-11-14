// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests cross-verifying the pipeline_cpp MenuArchivePatcher producer
// against the real proxy_dll menu_autopatch consumer across clean-install
// sandboxes, .bak priority over legacy *_backup folders, .applied sentinel
// fast-path idempotence, mid-batch rollback atomicity, and committed
// extravaganza_patch.bin wire invariants.
#include "charts/charts.h"
#include "common/util.h"
#include "menus/menu_patcher.h"
#include "menu_autopatch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mn = exc::menus;
namespace fs = std::filesystem;
using namespace exc;

namespace {

constexpr std::uint32_t kByteMask = 0xFFu;
constexpr unsigned int kByte1Shift = 8u;
constexpr unsigned int kByte2Shift = 16u;
constexpr unsigned int kByte3Shift = 24u;

constexpr std::uint8_t kDwqHeaderPadByte = 0x20u;
constexpr std::size_t kDwqSignatureLength = 16u;
constexpr std::size_t kDwqPackTypeBlockOffset = 0x2Cu;
constexpr std::size_t kDwqPackTypeBlockLength = 20u;
constexpr std::size_t kPackBmpHeaderLen = 7u;
constexpr std::size_t kPackTypePrefixLen = 9u;
constexpr std::size_t kBmpMagicLen = 2u;
constexpr std::size_t kInt32ByteSize = 4u;

constexpr std::uint16_t kBmpPlanes = 1u;
constexpr std::uint16_t kBmp24BitDepth = 24u;
constexpr std::uint16_t kBmp8BitDepth = 8u;
constexpr std::uint32_t kMaskPaletteColorCount = 256u;
constexpr std::int32_t kInitialCropMargin = 1;

constexpr int kSprite0Width = 8;
constexpr int kSprite0Height = 6;
constexpr int kSprite1Width = 12;
constexpr int kSprite1Height = 8;
constexpr int kSprite2Width = 10;
constexpr int kSprite2Height = 6;
constexpr std::uint32_t kSyntheticArchiveEntryCount = 3u;
constexpr std::size_t kUntouchedFirstIndex = 0u;
constexpr std::size_t kPatchedMiddleIndex = 1u;
constexpr std::size_t kUntouchedLastIndex = 2u;
constexpr std::uint32_t kExpectedSyntheticModifiedGpkCount = 1u;

constexpr std::uint8_t kSpriteBaseRed = 30u;
constexpr std::uint8_t kSpriteBaseGreen = 90u;
constexpr std::uint8_t kSpriteBaseBlue = 160u;
constexpr std::uint8_t kSpritePatchedRed = 210u;
constexpr std::uint8_t kSpritePatchedGreen = 55u;
constexpr std::uint8_t kSpritePatchedBlue = 25u;
constexpr std::uint8_t kOpaqueAlpha = 255u;
constexpr std::uint8_t kSemiAlpha = 180u;

constexpr std::size_t kBasePayloadSeedMultiplier = 17u;
constexpr std::size_t kPatchedExtraBytesPerRecord = 24u;
constexpr std::size_t kMidBatchFaultIndex = 15u;
constexpr std::uint8_t kPoisonByteXor = 0x5Au;

constexpr std::size_t kSentinelExpectedBytes = 20u;
constexpr std::size_t kSentinelMagicBytes = 8u;
constexpr char kExpectedSentinelMagic[kSentinelMagicBytes] = {
    'E', 'X', 'C', 'E', 'S', 'N', 'T', '1'};

struct StandaloneSpec {
    const char* rel_path;
    const char* legacy_backup_dir;
    const char* filename;
};

constexpr std::array<StandaloneSpec, mn::kExpectedStandaloneFileCount> kStandaloneSpecs = {{
    {"spt\\01.spt",            "spt_backup",   "01.spt"},
    {"spt\\02.spt",            "spt_backup",   "02.spt"},
    {"spt\\03.spt",            "spt_backup",   "03.spt"},
    {"spt\\04.spt",            "spt_backup",   "04.spt"},
    {"spt\\05.spt",            "spt_backup",   "05.spt"},
    {"spt\\06.spt",            "spt_backup",   "06.spt"},
    {"spt\\062.spt",           "spt_backup",   "062.spt"},
    {"spt\\07.spt",            "spt_backup",   "07.spt"},
    {"spt\\08.spt",            "spt_backup",   "08.spt"},
    {"spt\\_exban.spt",        "spt_backup",   "_exban.spt"},
    {"spt\\_exgame.spt",       "spt_backup",   "_exgame.spt"},
    {"spt\\_meta.spt",         "spt_backup",   "_meta.spt"},
    {"spt\\_staff.spt",        "spt_backup",   "_staff.spt"},
    {"spt\\backscript.spt",    "spt_backup",   "backscript.spt"},
    {"spt\\omake.spt",         "spt_backup",   "omake.spt"},
    {"spt\\sys.spt",           "spt_backup",   "sys.spt"},
    {"spt\\charaname.xtx",     "xtx_backup",   "charaname.xtx"},
    {"chart\\chart1.fxf",      "chart_backup", "chart1.fxf"},
    {"chart\\chart2.fxf",      "chart_backup", "chart2.fxf"},
    {"chart\\chart3.fxf",      "chart_backup", "chart3.fxf"},
    {"chart\\chart4.fxf",      "chart_backup", "chart4.fxf"},
    {"chart\\chart5.fxf",      "chart_backup", "chart5.fxf"},
    {"chart\\chart6.fxf",      "chart_backup", "chart6.fxf"},
    {"chart\\chart7.fxf",      "chart_backup", "chart7.fxf"},
    {"chart\\chart8.fxf",      "chart_backup", "chart8.fxf"},
    {"init2\\mainControl.fxf", "init2_backup", "mainControl.fxf"},
    {"init2\\backlog.fxf",     "init2_backup", "backlog.fxf"},
}};

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        const std::string dir_name =
            std::string(prefix) + "_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(id);
        path_ = fs::temp_directory_path() / fs::u8path(dir_name);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void PutU16Le(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte1Shift) & kByteMask));
}

void PutU32Le(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte1Shift) & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte2Shift) & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte3Shift) & kByteMask));
}

void PutI32Le(Bytes& b, std::int32_t v) {
    PutU32Le(b, static_cast<std::uint32_t>(v));
}

Bytes MakeSyntheticDwq(const mn::RgbaImage& img, bool with_mask) {
    const std::size_t align = static_cast<std::size_t>(mn::kBmpRowAlignment);
    const std::size_t stride =
        (static_cast<std::size_t>(img.width) * mn::kBgrChannels + (align - 1u)) & ~(align - 1u);
    const std::size_t bmp_px_bytes = stride * static_cast<std::size_t>(img.height);
    const std::size_t bmp_total_bytes = mn::kBmpHeaderSize + bmp_px_bytes;

    Bytes dwq(mn::kDwqHeaderSize, kDwqHeaderPadByte);
    if (with_mask) {
        std::memcpy(dwq.data(), "BMP+MASK        ", kDwqSignatureLength);
        std::memcpy(dwq.data() + kDwqPackTypeBlockOffset, "    PACKTYPE=2A     ",
                    kDwqPackTypeBlockLength);
        const std::int32_t margin = kInitialCropMargin;
        std::memcpy(dwq.data() + mn::kDwqCropTopOffset, &margin, kInt32ByteSize);
        std::memcpy(dwq.data() + mn::kDwqCropLeftOffset, &margin, kInt32ByteSize);
        std::memcpy(dwq.data() + mn::kDwqCropRightOffset, &margin, kInt32ByteSize);
        std::memcpy(dwq.data() + mn::kDwqCropBottomOffset, &margin, kInt32ByteSize);
        const std::int32_t ds = static_cast<std::int32_t>(bmp_total_bytes);
        std::memcpy(dwq.data() + mn::kDwqDataSizeOffset, &ds, kInt32ByteSize);
    } else {
        std::memcpy(dwq.data(), "BMP             ", kDwqSignatureLength);
        std::memcpy(dwq.data() + kDwqPackTypeBlockOffset, "    PACKTYPE=0      ",
                    kDwqPackTypeBlockLength);
    }
    const std::int32_t w32 = img.width;
    const std::int32_t h32 = img.height;
    std::memcpy(dwq.data() + mn::kDwqWidthOffset, &w32, kInt32ByteSize);
    std::memcpy(dwq.data() + mn::kDwqHeightOffset, &h32, kInt32ByteSize);

    dwq.push_back('B');
    dwq.push_back('M');
    PutU32Le(dwq, static_cast<std::uint32_t>(bmp_total_bytes));
    PutU16Le(dwq, 0u);
    PutU16Le(dwq, 0u);
    PutU32Le(dwq, static_cast<std::uint32_t>(mn::kBmpHeaderSize));
    PutU32Le(dwq, static_cast<std::uint32_t>(mn::kBmpInfoHeaderSize));
    PutI32Le(dwq, img.width);
    PutI32Le(dwq, img.height);
    PutU16Le(dwq, kBmpPlanes);
    PutU16Le(dwq, kBmp24BitDepth);
    PutU32Le(dwq, 0u);
    PutU32Le(dwq, static_cast<std::uint32_t>(bmp_px_bytes));
    PutI32Le(dwq, 0);
    PutI32Le(dwq, 0);
    PutU32Le(dwq, 0u);
    PutU32Le(dwq, 0u);

    Bytes bgr_rows(bmp_px_bytes, 0u);
    for (int y = 0; y < img.height; ++y) {
        std::uint8_t* dst = bgr_rows.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* src =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * mn::kRgbaChannels;
        for (int x = 0; x < img.width; ++x) {
            dst[x * mn::kBgrChannels + mn::kBgrBlueIndex] =
                src[x * mn::kRgbaChannels + mn::kBlueChannel];
            dst[x * mn::kBgrChannels + mn::kBgrGreenIndex] =
                src[x * mn::kRgbaChannels + mn::kGreenChannel];
            dst[x * mn::kBgrChannels + mn::kBgrRedIndex] =
                src[x * mn::kRgbaChannels + mn::kRedChannel];
        }
    }
    dwq.insert(dwq.end(), bgr_rows.begin(), bgr_rows.end());

    if (with_mask) {
        const std::size_t total_px = static_cast<std::size_t>(img.width) * img.height;
        Bytes alpha(total_px, 0u);
        for (std::size_t i = 0; i < total_px; ++i) {
            alpha[i] = img.pixels[i * mn::kRgbaChannels + mn::kAlphaChannel];
        }
        const Bytes rle = mn::encode_rle_xor_mask(alpha.data(), img.width, img.height);

        dwq.push_back('B');
        dwq.push_back('M');
        PutU32Le(dwq, static_cast<std::uint32_t>(mn::kMaskHeaderTotalSize + total_px));
        PutU16Le(dwq, 0u);
        PutU16Le(dwq, 0u);
        PutU32Le(dwq, static_cast<std::uint32_t>(mn::kMaskHeaderTotalSize));
        PutU32Le(dwq, static_cast<std::uint32_t>(mn::kBmpInfoHeaderSize));
        PutI32Le(dwq, img.width);
        PutI32Le(dwq, img.height);
        PutU16Le(dwq, kBmpPlanes);
        PutU16Le(dwq, kBmp8BitDepth);
        PutU32Le(dwq, 0u);
        PutU32Le(dwq, static_cast<std::uint32_t>(total_px));
        PutI32Le(dwq, 0);
        PutI32Le(dwq, 0);
        PutU32Le(dwq, kMaskPaletteColorCount);
        PutU32Le(dwq, 0u);

        for (std::uint32_t i = 0; i < kMaskPaletteColorCount; ++i) {
            dwq.push_back(static_cast<std::uint8_t>(i));
            dwq.push_back(static_cast<std::uint8_t>(i));
            dwq.push_back(static_cast<std::uint8_t>(i));
            dwq.push_back(0u);
        }
        dwq.insert(dwq.end(), rle.begin(), rle.end());
    }

    return dwq;
}

mn::RgbaImage MakeSolidRgba(int width, int height,
                            std::uint8_t r, std::uint8_t g,
                            std::uint8_t b, std::uint8_t a) {
    mn::RgbaImage img;
    img.width = width;
    img.height = height;
    img.pixels.resize(static_cast<std::size_t>(width) * height * mn::kRgbaChannels);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        img.pixels[i * mn::kRgbaChannels + mn::kRedChannel] =
            static_cast<std::uint8_t>((r + i) & kByteMask);
        img.pixels[i * mn::kRgbaChannels + mn::kGreenChannel] = g;
        img.pixels[i * mn::kRgbaChannels + mn::kBlueChannel] = b;
        img.pixels[i * mn::kRgbaChannels + mn::kAlphaChannel] = a;
    }
    return img;
}

struct SyntheticSuiteAssets {
    std::array<Bytes, mn::kExpectedStandaloneFileCount> orig_standalone;
    std::array<Bytes, mn::kExpectedStandaloneFileCount> new_standalone;
    Bytes orig_gtb;
    Bytes orig_gpk;
    Bytes new_gtb;
    Bytes new_gpk;
    Bytes untouched_dwq_first;
    Bytes untouched_dwq_last;
    mn::RgbaImage patched_middle_rgba;
};

SyntheticSuiteAssets BuildSyntheticSuiteAssets() {
    SyntheticSuiteAssets assets;

    for (std::size_t i = 0; i < kStandaloneSpecs.size(); ++i) {
        const std::string_view rel = kStandaloneSpecs[i].rel_path;
        if (rel == "init2\\mainControl.fxf") {
            const std::string raw =
                "messagePrintSizeX 24\r\nmessageFontSize 24\r\nnameFontSize 24\r\n";
            const Bytes encoded = exc::charts::encrypt_fxf(Bytes(raw.begin(), raw.end()));
            assets.orig_standalone[i] = encoded;
            assets.new_standalone[i] = mn::patch_main_control_fxf(encoded);
            continue;
        }
        if (rel == "init2\\backlog.fxf") {
            const std::string raw = "fontSize 24\r\nprintSpaceY 30\r\n";
            const Bytes encoded = exc::charts::encrypt_fxf(Bytes(raw.begin(), raw.end()));
            assets.orig_standalone[i] = encoded;
            assets.new_standalone[i] = mn::patch_backlog_fxf(encoded);
            continue;
        }

        // Include Japanese UTF-8/CP932 bytes plus sign-bit boundary bytes
        // (0x00, 0x7F, 0x80, 0xFF) so signed-char truncation or NUL-termination
        // bugs fail immediately.
        const std::string jp_tag = "蟲愛でる少女:" + std::string(rel);
        Bytes orig(jp_tag.begin(), jp_tag.end());
        orig.push_back(0x00u);
        orig.push_back(0x7Fu);
        orig.push_back(0x80u);
        orig.push_back(0xFFu);
        for (std::size_t b = 0; b <= i; ++b) {
            orig.push_back(static_cast<std::uint8_t>(
                (i * kBasePayloadSeedMultiplier + b) & kByteMask));
        }

        const std::string en_tag = "The Girl Who Loved Insects:" + std::string(rel);
        Bytes patched(en_tag.begin(), en_tag.end());
        patched.push_back(0xFFu);
        patched.push_back(0x80u);
        patched.push_back(0x7Fu);
        patched.push_back(0x00u);
        for (std::size_t b = 0; b < i + kPatchedExtraBytesPerRecord; ++b) {
            patched.push_back(static_cast<std::uint8_t>(
                ((i + 1u) * kBasePayloadSeedMultiplier + b) & kByteMask));
        }

        assets.orig_standalone[i] = std::move(orig);
        assets.new_standalone[i] = std::move(patched);
    }

    const mn::RgbaImage img0 = MakeSolidRgba(
        kSprite0Width, kSprite0Height, kSpriteBaseRed, kSpriteBaseGreen, kSpriteBaseBlue, kOpaqueAlpha);
    const mn::RgbaImage img1_orig = MakeSolidRgba(
        kSprite1Width, kSprite1Height, kSpriteBaseRed, kSpriteBaseGreen, kSpriteBaseBlue, kSemiAlpha);
    const mn::RgbaImage img2 = MakeSolidRgba(
        kSprite2Width, kSprite2Height, kSpriteBaseRed, kSpriteBaseGreen, kSpriteBaseBlue, kOpaqueAlpha);

    assets.untouched_dwq_first = MakeSyntheticDwq(img0, false);
    const Bytes orig_dwq_middle = MakeSyntheticDwq(img1_orig, true);
    assets.untouched_dwq_last = MakeSyntheticDwq(img2, true);

    const std::array<std::string_view, kSyntheticArchiveEntryCount> names = {{
        "bg_frame", "btn_config", "btn_load"}};
    const std::array<const Bytes*, kSyntheticArchiveEntryCount> slices = {{
        &assets.untouched_dwq_first, &orig_dwq_middle, &assets.untouched_dwq_last}};

    std::vector<std::uint32_t> name_offsets;
    std::vector<std::uint32_t> data_offsets;
    Bytes name_blob;
    for (std::size_t i = 0; i < kSyntheticArchiveEntryCount; ++i) {
        name_offsets.push_back(static_cast<std::uint32_t>(name_blob.size()));
        name_blob.insert(name_blob.end(), names[i].begin(), names[i].end());
        name_blob.push_back(0u);

        data_offsets.push_back(static_cast<std::uint32_t>(assets.orig_gpk.size()));
        assets.orig_gpk.insert(assets.orig_gpk.end(), slices[i]->begin(), slices[i]->end());
    }

    PutU32Le(assets.orig_gtb, kSyntheticArchiveEntryCount);
    for (std::uint32_t off : name_offsets) {
        PutU32Le(assets.orig_gtb, off);
    }
    for (std::uint32_t off : data_offsets) {
        PutU32Le(assets.orig_gtb, off);
    }
    assets.orig_gtb.insert(assets.orig_gtb.end(), name_blob.begin(), name_blob.end());

    assets.patched_middle_rgba = MakeSolidRgba(
        kSprite1Width, kSprite1Height,
        kSpritePatchedRed, kSpritePatchedGreen, kSpritePatchedBlue, kOpaqueAlpha);
    mn::MenuArchivePatcher patcher =
        mn::MenuArchivePatcher::load_from_memory(assets.orig_gtb, assets.orig_gpk);
    patcher.replace_entry_rgba(kPatchedMiddleIndex, assets.patched_middle_rgba);
    auto [rebuilt_gtb, rebuilt_gpk] = patcher.serialize();
    assets.new_gtb = std::move(rebuilt_gtb);
    assets.new_gpk = std::move(rebuilt_gpk);

    return assets;
}

void WriteFileCreatingDirs(const fs::path& dst, const Bytes& data) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    write_file(dst.u8string(), data);
}

void PopulateCleanRetailInstall(const fs::path& root, const SyntheticSuiteAssets& assets) {
    for (std::size_t i = 0; i < kStandaloneSpecs.size(); ++i) {
        const fs::path dst = root / fs::u8path(kStandaloneSpecs[i].rel_path);
        WriteFileCreatingDirs(dst, assets.orig_standalone[i]);
    }
    WriteFileCreatingDirs(root / "dwq" / "sys0.gtb", assets.orig_gtb);
    WriteFileCreatingDirs(root / "dwq" / "sys0.gpk", assets.orig_gpk);
}

std::size_t CountFilesWithExtension(const fs::path& root, std::string_view ext) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().u8string();
        if (name.size() >= ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext.data(), ext.size()) == 0) {
            ++count;
        }
    }
    return count;
}

fs::path ResolvePatchBinPath() {
    std::error_code ec;
    const fs::path roots[] = {fs::current_path(ec), fs::u8path(__FILE__).parent_path()};
    for (const fs::path& base : roots) {
        for (fs::path cur = base; !cur.empty(); cur = cur.parent_path()) {
            const fs::path direct = cur / "extravaganza_patch.bin";
            if (fs::is_regular_file(direct, ec)) {
                return direct;
            }
            const fs::path nested = cur / "EXTRAVAGANZA_01_CE" / "extravaganza_patch.bin";
            if (fs::is_regular_file(nested, ec)) {
                return nested;
            }
            if (cur == cur.root_path()) {
                break;
            }
        }
    }
    return {};
}

bool ContainsBytes(const Bytes& haystack, const void* needle, std::size_t needle_len) {
    if (needle_len == 0 || haystack.size() < needle_len) {
        return false;
    }
    const auto* pat = static_cast<const std::uint8_t*>(needle);
    const std::size_t limit = haystack.size() - needle_len;
    for (std::size_t i = 0; i <= limit; ++i) {
        if (std::memcmp(haystack.data() + i, pat, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(MenuPatcherCt, ProducerToRealAutopatchConsumerCleanInstallIdempotenceAndRollback) {
    const SyntheticSuiteAssets assets = BuildSyntheticSuiteAssets();

    // Prepare producer staging tree with BOTH authoritative .bak pristine files
    // AND deliberately poisoned legacy *_backup directories so any regression in
    // .bak-first resolution causes the clean-install consumer to reject the patch.
    ScopedTempDir producer_dir("exc_ct_producer");
    for (std::size_t i = 0; i < kStandaloneSpecs.size(); ++i) {
        const fs::path live = producer_dir.path() / fs::u8path(kStandaloneSpecs[i].rel_path);
        const fs::path bak =
            producer_dir.path() / fs::u8path(std::string(kStandaloneSpecs[i].rel_path) + ".bak");
        WriteFileCreatingDirs(live, assets.new_standalone[i]);
        WriteFileCreatingDirs(bak, assets.orig_standalone[i]);

        Bytes poisoned = assets.new_standalone[i];
        poisoned.push_back(kPoisonByteXor);
        const fs::path legacy_poison =
            producer_dir.path() / kStandaloneSpecs[i].legacy_backup_dir /
            kStandaloneSpecs[i].filename;
        WriteFileCreatingDirs(legacy_poison, poisoned);
    }
    WriteFileCreatingDirs(producer_dir.path() / "dwq" / "sys0.gtb", assets.new_gtb);
    WriteFileCreatingDirs(producer_dir.path() / "dwq" / "sys0.gpk", assets.new_gpk);
    WriteFileCreatingDirs(producer_dir.path() / "dwq" / "sys0.gtb.bak", assets.orig_gtb);
    WriteFileCreatingDirs(producer_dir.path() / "dwq" / "sys0.gpk.bak", assets.orig_gpk);

    Bytes poisoned_gtb = assets.orig_gtb;
    poisoned_gtb.back() ^= kPoisonByteXor;
    WriteFileCreatingDirs(producer_dir.path() / "dwq_backup" / "sys0.gtb", poisoned_gtb);
    WriteFileCreatingDirs(producer_dir.path() / "dwq_backup" / "sys0.gpk", assets.new_gpk);

    const fs::path produced_patch_path = producer_dir.path() / "extravaganza_patch.bin";
    mn::MenuArchivePatcher::write_patch_bin_from_game_dir(
        producer_dir.path().u8string(), produced_patch_path.u8string());

    const Bytes produced_patch_bytes = read_file(produced_patch_path.u8string());
    const mn::ParsedPatchBin parsed = mn::parse_patch_bin(produced_patch_bytes);
    ASSERT_EQ(parsed.standalone_records.size(), mn::kExpectedStandaloneFileCount);
    ASSERT_EQ(parsed.gpk_records.size(), kExpectedSyntheticModifiedGpkCount);
    EXPECT_EQ(parsed.gpk_records[0].entry_index, kPatchedMiddleIndex);

    // Clean retail install sandbox: ZERO .bak files, ZERO *_backup folders.
    ScopedTempDir clean_dir("exc_ct_clean_install");
    PopulateCleanRetailInstall(clean_dir.path(), assets);
    write_file((clean_dir.path() / "extravaganza_patch.bin").u8string(), produced_patch_bytes);

    ASSERT_EQ(CountFilesWithExtension(clean_dir.path(), ".bak"), 0u);
    ASSERT_FALSE(fs::exists(clean_dir.path() / "extravaganza_patch.bin.applied"));

    // Run 1: Real proxy_dll consumer patches all 29 files, creates .bak backups,
    // and writes extravaganza_patch.bin.applied.
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(clean_dir.path().wstring()));

    const fs::path sentinel_path = clean_dir.path() / "extravaganza_patch.bin.applied";
    ASSERT_TRUE(fs::is_regular_file(sentinel_path));
    const Bytes sentinel_bytes = read_file(sentinel_path.u8string());
    ASSERT_EQ(sentinel_bytes.size(), kSentinelExpectedBytes);
    EXPECT_EQ(std::memcmp(sentinel_bytes.data(), kExpectedSentinelMagic, kSentinelMagicBytes), 0);
    EXPECT_EQ(CountFilesWithExtension(clean_dir.path(), ".tmp"), 0u);
    EXPECT_EQ(CountFilesWithExtension(clean_dir.path(), ".bak"),
              mn::kExpectedStandaloneFileCount + 2u);

    for (std::size_t i = 0; i < kStandaloneSpecs.size(); ++i) {
        const fs::path target = clean_dir.path() / fs::u8path(kStandaloneSpecs[i].rel_path);
        const fs::path bak =
            clean_dir.path() / fs::u8path(std::string(kStandaloneSpecs[i].rel_path) + ".bak");
        EXPECT_EQ(read_file(target.u8string()), assets.new_standalone[i])
            << kStandaloneSpecs[i].rel_path;
        EXPECT_EQ(read_file(bak.u8string()), assets.orig_standalone[i])
            << kStandaloneSpecs[i].rel_path;
    }

    EXPECT_EQ(read_file((clean_dir.path() / "dwq" / "sys0.gtb").u8string()), assets.new_gtb);
    EXPECT_EQ(read_file((clean_dir.path() / "dwq" / "sys0.gpk").u8string()), assets.new_gpk);
    EXPECT_EQ(read_file((clean_dir.path() / "dwq" / "sys0.gtb.bak").u8string()), assets.orig_gtb);
    EXPECT_EQ(read_file((clean_dir.path() / "dwq" / "sys0.gpk.bak").u8string()), assets.orig_gpk);

    const mn::MenuArchivePatcher consumer_arch = mn::MenuArchivePatcher::load(
        (clean_dir.path() / "dwq" / "sys0.gtb").u8string(),
        (clean_dir.path() / "dwq" / "sys0.gpk").u8string());
    ASSERT_EQ(consumer_arch.entry_count(), kSyntheticArchiveEntryCount);
    EXPECT_EQ(consumer_arch.raw_entry_bytes(kUntouchedFirstIndex), assets.untouched_dwq_first);
    EXPECT_EQ(consumer_arch.raw_entry_bytes(kUntouchedLastIndex), assets.untouched_dwq_last);
    const Bytes middle_dwq = consumer_arch.raw_entry_bytes(kPatchedMiddleIndex);
    const mn::RgbaImage middle_decoded =
        mn::decode_dwq_to_rgba(middle_dwq.data(), middle_dwq.size());
    EXPECT_EQ(middle_decoded.pixels, assets.patched_middle_rgba.pixels);

    // Run 2: Sentinel fast-path idempotence preserves .bak timestamps and outputs.
    const auto spt01_bak_mtime_before =
        fs::last_write_time(clean_dir.path() / "spt" / "01.spt.bak");
    const auto gpk_bak_mtime_before =
        fs::last_write_time(clean_dir.path() / "dwq" / "sys0.gpk.bak");
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(clean_dir.path().wstring()));
    EXPECT_EQ(fs::last_write_time(clean_dir.path() / "spt" / "01.spt.bak"),
              spt01_bak_mtime_before);
    EXPECT_EQ(fs::last_write_time(clean_dir.path() / "dwq" / "sys0.gpk.bak"),
              gpk_bak_mtime_before);
    EXPECT_EQ(read_file((clean_dir.path() / "spt" / "01.spt").u8string()),
              assets.new_standalone[0]);

    // Interleaved recovery: deleting .applied and reverting spt/01.spt re-patches
    // cleanly from .bak without overwriting .bak.
    fs::remove(sentinel_path);
    write_file((clean_dir.path() / "spt" / "01.spt").u8string(), assets.orig_standalone[0]);
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(clean_dir.path().wstring()));
    EXPECT_TRUE(fs::is_regular_file(sentinel_path));
    EXPECT_EQ(read_file((clean_dir.path() / "spt" / "01.spt").u8string()),
              assets.new_standalone[0]);
    EXPECT_EQ(read_file((clean_dir.path() / "spt" / "01.spt.bak").u8string()),
              assets.orig_standalone[0]);

    // Mid-batch rollback atomicity: if retail file #15 has corrupted bytes on a
    // clean install, items 0..14 staged before it roll back cleanly leaving zero
    // .tmp, .bak, or .applied files and leaving items 0..14 untouched.
    ScopedTempDir rollback_dir("exc_ct_rollback");
    PopulateCleanRetailInstall(rollback_dir.path(), assets);
    write_file((rollback_dir.path() / "extravaganza_patch.bin").u8string(), produced_patch_bytes);

    Bytes corrupted_mid = assets.orig_standalone[kMidBatchFaultIndex];
    corrupted_mid[0] ^= kPoisonByteXor;
    const fs::path fault_target =
        rollback_dir.path() / fs::u8path(kStandaloneSpecs[kMidBatchFaultIndex].rel_path);
    write_file(fault_target.u8string(), corrupted_mid);

    EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(rollback_dir.path().wstring()));
    EXPECT_EQ(CountFilesWithExtension(rollback_dir.path(), ".tmp"), 0u);
    EXPECT_EQ(CountFilesWithExtension(rollback_dir.path(), ".bak"), 0u);
    EXPECT_FALSE(fs::exists(rollback_dir.path() / "extravaganza_patch.bin.applied"));
    for (std::size_t i = 0; i < kMidBatchFaultIndex; ++i) {
        const fs::path untouched =
            rollback_dir.path() / fs::u8path(kStandaloneSpecs[i].rel_path);
        EXPECT_EQ(read_file(untouched.u8string()), assets.orig_standalone[i])
            << "Earlier batch item mutated despite mid-batch failure: "
            << kStandaloneSpecs[i].rel_path;
    }
}

TEST(MenuPatcherCt, CommittedPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path patch_bin_path = ResolvePatchBinPath();
    std::error_code ec;
    ASSERT_FALSE(patch_bin_path.empty());
    ASSERT_TRUE(fs::is_regular_file(patch_bin_path, ec))
        << "Missing extravaganza_patch.bin at " << patch_bin_path.u8string();

    const Bytes patch_bin = read_file(patch_bin_path.u8string());
    ASSERT_GE(patch_bin.size(), mn::kPatchHeaderSize);
    EXPECT_EQ(std::memcmp(patch_bin.data(), mn::kPatchMagic, mn::kPatchMagicSize), 0);

    constexpr std::uint8_t kPngHeader[] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    EXPECT_FALSE(ContainsBytes(patch_bin, "PACKBMP", kPackBmpHeaderLen));
    EXPECT_FALSE(ContainsBytes(patch_bin, "BMP+MASK        ", kDwqSignatureLength));
    EXPECT_FALSE(ContainsBytes(patch_bin, "PACKTYPE=", kPackTypePrefixLen));
    EXPECT_FALSE(ContainsBytes(patch_bin, kPngHeader, sizeof(kPngHeader)));

    const mn::ParsedPatchBin parsed = mn::parse_patch_bin(patch_bin);
    EXPECT_EQ(parsed.version, mn::kPatchFormatVersion);
    ASSERT_EQ(parsed.standalone_file_count, mn::kExpectedStandaloneFileCount);
    ASSERT_EQ(parsed.gpk_entry_count, mn::kExpectedModifiedGpkEntryCount);
    ASSERT_EQ(parsed.standalone_records.size(), mn::kExpectedStandaloneFileCount);
    ASSERT_EQ(parsed.gpk_records.size(), mn::kExpectedModifiedGpkEntryCount);

    constexpr std::uint32_t kExpectedPristine01SptSize = 1250316u;
    EXPECT_EQ(parsed.standalone_records[0].rel_path, "spt\\01.spt");
    EXPECT_EQ(parsed.standalone_records[0].orig_file_size, kExpectedPristine01SptSize);

    for (const mn::StandalonePatchRecord& rec : parsed.standalone_records) {
        EXPECT_FALSE(rec.rel_path.empty());
        EXPECT_GT(rec.orig_file_size, 0u);
        EXPECT_NE(rec.orig_data_hash, 0u);
        EXPECT_GT(rec.new_file_size, 0u);
        EXPECT_NE(rec.new_data_hash, 0u);
        EXPECT_EQ(rec.masked_payload.size(), rec.new_file_size);
        if (rec.masked_payload.size() >= kBmpMagicLen) {
            EXPECT_NE(std::memcmp(rec.masked_payload.data(), "BM", kBmpMagicLen), 0);
        }
    }
    for (const mn::GpkEntryPatchRecord& rec : parsed.gpk_records) {
        EXPECT_GT(rec.orig_size, 0u);
        EXPECT_NE(rec.orig_data_hash, 0u);
        EXPECT_GT(rec.new_size, 0u);
        EXPECT_NE(rec.new_data_hash, 0u);
        EXPECT_EQ(rec.masked_entry.size(), rec.new_size);
        if (rec.masked_entry.size() >= kBmpMagicLen) {
            EXPECT_NE(std::memcmp(rec.masked_entry.data(), "BM", kBmpMagicLen), 0);
        }
    }
}
