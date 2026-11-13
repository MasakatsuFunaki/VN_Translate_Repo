// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests validating committed repository fixtures and patch artifacts:
// analysys/ybn_samples/*.ybn and cgsys_patch.bin.
#include "common/util.h"
#include "extract/extractor.h"
#include "menu_patcher/menu_patcher.h"
#include "test_support.h"
#include "yuris/ypf_archive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace frat;
using namespace frat::yuris;

namespace {

constexpr int kExitOk = 0;
constexpr std::size_t kExpectedExtractedJsonBytes = 85674u;
constexpr std::size_t kExpectedYst00001PlainBytes = 532u;
constexpr std::size_t kExpectedTwoEntries = 2u;
constexpr std::size_t kPngSignatureSize = 8u;
constexpr std::size_t kPatchMagicSize = 8u;
constexpr std::size_t kPatchHeaderSize = 24u;
constexpr std::size_t kPatchEntryCountOffset = 20u;
constexpr std::size_t kExpectedTranslatedEntryCount = 507u;
constexpr int kShiftByte1 = 8;
constexpr int kShiftByte2 = 16;
constexpr int kShiftByte3 = 24;

std::filesystem::path project_root() {
    return std::filesystem::u8path(FRAT_PROJECT_DIR);
}

Bytes read_sample(const std::string& name) {
    const std::filesystem::path sample_path =
        project_root() / "analysys" / "ybn_samples" / std::filesystem::u8path(name);
    return read_file(sample_path.u8string());
}

bool contains_png_signature(const Bytes& blob) {
    constexpr std::uint8_t kSig[kPngSignatureSize] = {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    return std::search(blob.begin(), blob.end(), std::begin(kSig), std::end(kSig)) !=
           blob.end();
}

}  // namespace

TEST(FratArtifactsCt, EndToEnd_SyntheticArchive_OutputIsPinnedByDigest) {
    // A seven-entry archive built from the checked-in ybn samples; the digest
    // pins the whole of extracted_text.json (375 entries, 85,674 bytes) --
    // CRLF, indent=2, key order, the story-first ordering, global_seen dedup
    // and the CP932 PUA decode all at once.
    const Bytes yst_plain = read_sample("yst00001.ybn");
    const Bytes yst_enc = decrypt_ystb(yst_plain);  // XOR is an involution
    const Bytes cfg = read_sample("yscfg.ybn");
    const Bytes yse = read_sample("yse.ybn");
    const Bytes lst = read_sample("yst_list.ybn");

    const Bytes image = frat_test::build_ypf({
        {"ysbin\\yst00156.ybn", yst_enc}, {"ysbin\\yst00157.ybn", yst_enc},
        {"ysbin\\yst00001.ybn", yst_enc}, {"ysbin\\yscfg.ybn", cfg},
        {"ysbin\\yse.ybn", yse},          {"ysbin\\yst_list.ybn", lst},
        {"ysbin\\yst.ybn", cfg},
    });

    frat_test::ScratchDir dir("e2e");
    std::filesystem::create_directories(std::filesystem::u8path(dir / "pac"));
    write_file(dir / "pac\\bn.ypf", image);
    const std::string out = dir / "extracted_text.json";
    ASSERT_EQ(extract::run_extract(dir / "pac", out), kExitOk);

    const Bytes produced = read_file(out);
    EXPECT_EQ(produced.size(), kExpectedExtractedJsonBytes);
    EXPECT_EQ(frat_test::sha256_hex(produced.data(), produced.size()),
              "ed3795835191284252412fc9dea662f44eeb582ecb4f6c799eb704e90b9bb8f4");
}

TEST(FratArtifactsCt, ReadEntry_InflatesThenDecrypts) {
    const Bytes plain = read_sample("yst00001.ybn");
    ASSERT_EQ(plain.size(), kExpectedYst00001PlainBytes);
    const Bytes encrypted = decrypt_ystb(plain);
    const Bytes sidecar = read_sample("yscfg.ybn");

    const Bytes image = frat_test::build_ypf({{"ysbin\\yst00001.ybn", encrypted},
                                              {"ysbin\\yscfg.ybn", sidecar}});
    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), kExpectedTwoEntries);
    EXPECT_EQ(read_entry(image, entries[0]), plain);
    EXPECT_EQ(read_entry(image, entries[1]), sidecar);
}

TEST(FratArtifactsCt, TrackedXorPatch_HasValidHeaderEntryCountAndZeroPngSignatures) {
    const std::filesystem::path tracked_patch = project_root() / "cgsys_patch.bin";
    ASSERT_TRUE(std::filesystem::exists(tracked_patch));
    EXPECT_FALSE(std::filesystem::exists(project_root() / "pac" / "cgsys.ypf"));

    const Bytes patch_blob = read_file(tracked_patch.u8string());
    ASSERT_GE(patch_blob.size(), kPatchHeaderSize);
    EXPECT_EQ(std::memcmp(patch_blob.data(), "FRATMENU", kPatchMagicSize), 0);
    EXPECT_FALSE(contains_png_signature(patch_blob));

    const std::uint32_t patch_entry_count =
        static_cast<std::uint32_t>(patch_blob[kPatchEntryCountOffset]) |
        (static_cast<std::uint32_t>(patch_blob[kPatchEntryCountOffset + 1u]) << kShiftByte1) |
        (static_cast<std::uint32_t>(patch_blob[kPatchEntryCountOffset + 2u]) << kShiftByte2) |
        (static_cast<std::uint32_t>(patch_blob[kPatchEntryCountOffset + 3u]) << kShiftByte3);
    EXPECT_EQ(patch_entry_count, kExpectedTranslatedEntryCount);
}
