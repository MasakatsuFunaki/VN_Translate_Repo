// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for FRATERNITE_HD menu_autopatch::MenuAutoPatcher
// (.applied sentinel v2 upgrades, missing-live .bak recovery, atomic .bak,
// and failure rollback).
#include "menu_autopatch.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using menu_autopatch::MenuAutoPatcher;
using menu_autopatch::PatchResult;
using menu_autopatch::PatchStatus;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
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

constexpr std::uint32_t kYpfVersion490 = 490u;
constexpr std::size_t kYpfHeaderSize = 32u;
constexpr std::size_t kYpfEntryPrefixSize = 5u;
constexpr std::size_t kYpfEntryFooterSize = 22u;
constexpr std::uint8_t kNameXorKey = 0xFFu;
constexpr std::uint32_t kMurmurM = 0x5BD1E995u;

void AppendU32Le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void AppendU64Le(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

void WriteU32Le(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v & 0xFFu);
    b[off + 1u] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    b[off + 2u] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    b[off + 3u] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

void WriteU64Le(Bytes& b, std::size_t off, std::uint64_t v) {
    for (std::size_t i = 0u; i < 8u; ++i) {
        b[off + i] = static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu);
    }
}

void AppendU32Be(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

std::uint32_t WireMurmur2(const Bytes& data) {
    std::uint32_t h = static_cast<std::uint32_t>(data.size());
    std::size_t i = 0u;
    while (i + 4u <= data.size()) {
        std::uint32_t k = static_cast<std::uint32_t>(data[i]) |
                          (static_cast<std::uint32_t>(data[i + 1u]) << 8) |
                          (static_cast<std::uint32_t>(data[i + 2u]) << 16) |
                          (static_cast<std::uint32_t>(data[i + 3u]) << 24);
        k *= kMurmurM;
        k ^= k >> 24;
        k *= kMurmurM;
        h *= kMurmurM;
        h ^= k;
        i += 4u;
    }
    const std::size_t rem = data.size() - i;
    if (rem == 3u) h ^= static_cast<std::uint32_t>(data[i + 2u]) << 16;
    if (rem >= 2u) h ^= static_cast<std::uint32_t>(data[i + 1u]) << 8;
    if (rem >= 1u) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurM;
    }
    h ^= h >> 13;
    h *= kMurmurM;
    h ^= h >> 15;
    return h;
}

Bytes MakeSyntheticPng(std::uint32_t w,
                       std::uint32_t h,
                       std::uint8_t color_type,
                       const Bytes& body) {
    Bytes out = {0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    AppendU32Be(out, 13u);
    out.insert(out.end(), {'I', 'H', 'D', 'R'});
    AppendU32Be(out, w);
    AppendU32Be(out, h);
    out.push_back(8u);
    out.push_back(color_type);
    out.push_back(0u);
    out.push_back(0u);
    out.push_back(0u);
    AppendU32Be(out, 0u);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

struct EntrySpec {
    std::string name;
    Bytes payload;
    bool share_prev = false;
};

Bytes BuildYpf(const std::vector<EntrySpec>& specs) {
    std::size_t idx_bytes = 0u;
    for (const auto& s : specs) {
        idx_bytes += kYpfEntryPrefixSize + s.name.size() + kYpfEntryFooterSize;
    }
    const std::uint64_t data_start = kYpfHeaderSize + idx_bytes;
    Bytes index;
    Bytes payloads;
    std::uint64_t last_off = data_start;

    for (std::size_t i = 0u; i < specs.size(); ++i) {
        const Bytes name_b(specs[i].name.begin(), specs[i].name.end());
        const Bytes& blob = specs[i].payload;
        std::uint64_t off = data_start + payloads.size();
        if (specs[i].share_prev && i > 0u) {
            off = last_off;
        } else {
            last_off = off;
            payloads.insert(payloads.end(), blob.begin(), blob.end());
            payloads.insert(payloads.end(), 4u, 0u);
        }

        AppendU32Le(index, WireMurmur2(name_b));
        index.push_back(static_cast<std::uint8_t>(name_b.size() ^ kNameXorKey));
        for (std::uint8_t b : name_b) {
            index.push_back(static_cast<std::uint8_t>(b ^ kNameXorKey));
        }
        index.push_back(2u);
        index.push_back(0u);
        AppendU32Le(index, static_cast<std::uint32_t>(blob.size()));
        AppendU32Le(index, static_cast<std::uint32_t>(blob.size()));
        AppendU64Le(index, off);
        AppendU32Le(index, WireMurmur2(blob));
    }

    Bytes out = {'Y', 'P', 'F', '\0'};
    AppendU32Le(out, kYpfVersion490);
    AppendU32Le(out, static_cast<std::uint32_t>(specs.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(data_start));
    out.insert(out.end(), 16u, 0u);
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), payloads.begin(), payloads.end());
    return out;
}

struct PatchEntrySpec {
    std::uint32_t entry_index = 0u;
    Bytes orig_png;
    Bytes new_png;
};

Bytes BuildFratMenuBlob(std::uint32_t total_entries,
                        std::uint32_t orig_size,
                        const std::vector<PatchEntrySpec>& recs) {
    Bytes out = {'F', 'R', 'A', 'T', 'M', 'E', 'N', 'U'};
    AppendU32Le(out, 1u);
    AppendU32Le(out, total_entries);
    AppendU32Le(out, orig_size);
    AppendU32Le(out, static_cast<std::uint32_t>(recs.size()));

    for (const auto& rec : recs) {
        const std::uint32_t orig_hash = WireMurmur2(rec.orig_png);
        const std::uint32_t new_hash = WireMurmur2(rec.new_png);
        AppendU32Le(out, rec.entry_index);
        AppendU32Le(out, orig_hash);
        AppendU32Le(out, static_cast<std::uint32_t>(rec.new_png.size()));
        AppendU32Le(out, new_hash);
        for (std::size_t k = 0u; k < rec.new_png.size(); ++k) {
            const auto k32 = static_cast<std::uint32_t>(k);
            const auto hb = static_cast<std::uint8_t>((orig_hash >> ((k32 & 3u) * 8u)) & 0xFFu);
            const auto sb = static_cast<std::uint8_t>((k32 * 0x9Eu + 0x37u) & 0xFFu);
            const auto mask = static_cast<std::uint8_t>(
                rec.orig_png[k % rec.orig_png.size()] ^ hb ^ sb);
            out.push_back(static_cast<std::uint8_t>(rec.new_png[k] ^ mask));
        }
    }
    return out;
}

void WriteBytes(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(static_cast<bool>(out));
}

Bytes ReadBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto sz = in.tellg();
    if (sz <= 0) return {};
    in.seekg(0, std::ios::beg);
    Bytes out(static_cast<std::size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(out.data()), sz)) return {};
    return out;
}

}  // namespace

TEST(MenuAutoPatcherUt, SkipsMissingInputsAndRejectsInvalidPatchHeader) {
    const ScopedTempDir tmp("frat_ut_missing_inputs");
    const MenuAutoPatcher patcher(L"Local\\FratUtMissingMutex");

    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kSkippedMissingInput);

    WriteBytes(tmp.path() / "cgsys_patch.bin", Bytes{'F', 'R', 'A', 'T'});
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kSkippedMissingInput);

    const Bytes orig_png = MakeSyntheticPng(64u, 32u, 6u, {0x00u, 0x7Fu, 0x80u, 0xFFu});
    const Bytes orig_ypf = BuildYpf({{"cgsys\\btn.png", orig_png, false}});
    WriteBytes(tmp.path() / "pac" / "cgsys.ypf", orig_ypf);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kFailed);
    EXPECT_FALSE(fs::exists(tmp.path() / "pac" / "cgsys.ypf.bak"));

    // Tampered Launch-1 patch must fail in memory before creating any .bak or .tmp
    Bytes bad_launch1 = BuildFratMenuBlob(
        1u, static_cast<std::uint32_t>(orig_ypf.size()),
        {{0u, orig_png, MakeSyntheticPng(64u, 32u, 6u, {0x11u, 0x22u})}});
    bad_launch1.back() ^= 0xFFu;
    WriteBytes(tmp.path() / "cgsys_patch.bin", bad_launch1);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kFailed);
    EXPECT_FALSE(fs::exists(tmp.path() / "pac" / "cgsys.ypf.bak"));
    EXPECT_FALSE(fs::exists(tmp.path() / "pac" / "cgsys.ypf.bak.tmp"));
    EXPECT_FALSE(fs::exists(tmp.path() / "pac" / "cgsys.ypf.tmp"));
    EXPECT_EQ(ReadBytes(tmp.path() / "pac" / "cgsys.ypf"), orig_ypf);
}

TEST(MenuAutoPatcherUt, AppliesV1FastPathsLaunch2AndReAppliesSameAndDifferentSizeV2FromBak) {
    const ScopedTempDir tmp("frat_ut_v2_upgrade");
    const MenuAutoPatcher patcher(L"Local\\FratUtV2UpgradeMutex");

    const Bytes orig_png = MakeSyntheticPng(1280u, 720u, 2u, {0x00u, 0x7Fu, 0x80u, 0xFFu, 0x11u});
    const Bytes v1_png = MakeSyntheticPng(1280u, 720u, 2u, Bytes(24u, 0x41u));
    // Exact same byte size as v1 to prove sentinel hash triggers re-patch where size-only skipped.
    const Bytes v2_same_png = MakeSyntheticPng(1280u, 720u, 2u, Bytes(24u, 0x82u));
    const Bytes v3_resized_png = MakeSyntheticPng(1280u, 720u, 2u, Bytes(37u, 0xC3u));

    const Bytes orig_ypf = BuildYpf({{"cgsys\\config\\back_easy.png", orig_png, false}});
    const Bytes v1_ypf = BuildYpf({{"cgsys\\config\\back_easy.png", v1_png, false}});
    const Bytes v2_ypf = BuildYpf({{"cgsys\\config\\back_easy.png", v2_same_png, false}});
    const Bytes v3_ypf = BuildYpf({{"cgsys\\config\\back_easy.png", v3_resized_png, false}});

    const fs::path ypf_path = tmp.path() / "pac" / "cgsys.ypf";
    const fs::path bak_path = tmp.path() / "pac" / "cgsys.ypf.bak";
    const fs::path bak_tmp = tmp.path() / "pac" / "cgsys.ypf.bak.tmp";
    const fs::path sentinel_path = tmp.path() / "pac" / "cgsys.ypf.applied";
    const fs::path patch_path = tmp.path() / "cgsys_patch.bin";

    WriteBytes(ypf_path, orig_ypf);
    // Plant stale interrupted .bak.tmp before Launch 1 to verify atomic .bak creation.
    WriteBytes(bak_tmp, Bytes{'B', 'A', 'D'});

    WriteBytes(patch_path, BuildFratMenuBlob(1u, static_cast<std::uint32_t>(orig_ypf.size()),
                                             {{0u, orig_png, v1_png}}));
    const PatchResult r1 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r1.status, PatchStatus::kApplied);
    EXPECT_EQ(r1.patched_archive_bytes, v1_ypf.size());
    EXPECT_EQ(ReadBytes(bak_path), orig_ypf);
    EXPECT_EQ(ReadBytes(ypf_path), v1_ypf);
    EXPECT_TRUE(fs::exists(sentinel_path));
    EXPECT_FALSE(fs::exists(bak_tmp));

    // Launch 2: identical v1 hits fast path
    const PatchResult r2 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r2.status, PatchStatus::kAlreadyUpToDate);
    EXPECT_EQ(r2.patched_archive_bytes, v1_ypf.size());

    // Launch 3: same-size v2 upgrade re-applies cleanly from .bak
    WriteBytes(patch_path, BuildFratMenuBlob(1u, static_cast<std::uint32_t>(orig_ypf.size()),
                                             {{0u, orig_png, v2_same_png}}));
    const PatchResult r3 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r3.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(bak_path), orig_ypf);
    EXPECT_EQ(ReadBytes(ypf_path), v2_ypf);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kAlreadyUpToDate);

    // Launch 4: legacy pre-sentinel player (no .applied file, live already patched, pristine .bak)
    fs::remove(sentinel_path);
    WriteBytes(patch_path, BuildFratMenuBlob(1u, static_cast<std::uint32_t>(orig_ypf.size()),
                                             {{0u, orig_png, v3_resized_png}}));
    const PatchResult r4 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r4.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(ypf_path), v3_ypf);
    EXPECT_EQ(ReadBytes(bak_path), orig_ypf);
}

TEST(MenuAutoPatcherUt, RecoversMissingLiveYpfFromBakAndIsolatesCorruptUpgrade) {
    const ScopedTempDir tmp("frat_ut_recovery");
    const MenuAutoPatcher patcher(L"Local\\FratUtRecoveryMutex");

    const Bytes orig_png = MakeSyntheticPng(240u, 48u, 6u, {0x00u, 0x7Fu, 0x80u, 0xFFu});
    const Bytes v1_png = MakeSyntheticPng(240u, 48u, 6u, Bytes(19u, 0x5Au));
    const Bytes orig_ypf = BuildYpf({{"cgsys\\config\\btn_bt3.png", orig_png, false}});
    const Bytes v1_ypf = BuildYpf({{"cgsys\\config\\btn_bt3.png", v1_png, false}});
    const Bytes v1_patch = BuildFratMenuBlob(1u, static_cast<std::uint32_t>(orig_ypf.size()),
                                             {{0u, orig_png, v1_png}});

    const fs::path ypf_path = tmp.path() / "pac" / "cgsys.ypf";
    const fs::path bak_path = tmp.path() / "pac" / "cgsys.ypf.bak";
    const fs::path patch_path = tmp.path() / "cgsys_patch.bin";

    // Live pac\cgsys.ypf absent, only .bak exists -> must recover patched live archive!
    WriteBytes(bak_path, orig_ypf);
    WriteBytes(patch_path, v1_patch);

    const PatchResult rec = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(rec.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(ypf_path), v1_ypf);
    EXPECT_EQ(ReadBytes(bak_path), orig_ypf);

    // Valid -> Failing -> Valid seam: tampered patch leaves v1 live archive intact
    Bytes tampered = v1_patch;
    tampered.back() ^= 0xFFu;
    WriteBytes(patch_path, tampered);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kFailed);
    EXPECT_EQ(ReadBytes(ypf_path), v1_ypf);
    EXPECT_FALSE(fs::exists(tmp.path() / "pac" / "cgsys.ypf.tmp"));

    WriteBytes(patch_path, v1_patch);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kAlreadyUpToDate);
}

TEST(MenuAutoPatcherUt, PinnedWireVectorExact40ByteSentinelAndCorruptSentinelRejection) {
    // Independently precomputed canonical wire bytes (zero helper hash calls):
    // 1-entry YPF v490 ("a.png", 1x1 RGB PNG 37B -> 38B).
    const Bytes pinned_orig_ypf = {
        0x59u, 0x50u, 0x46u, 0x00u, 0xEAu, 0x01u, 0x00u, 0x00u,
        0x01u, 0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0xE4u, 0xDFu, 0xA8u, 0xA2u, 0xFAu, 0x9Eu, 0xD1u, 0x8Fu,
        0x91u, 0x98u, 0x02u, 0x00u, 0x25u, 0x00u, 0x00u, 0x00u,
        0x25u, 0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x4Cu, 0x07u, 0xB3u, 0xFFu,
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
        0x00u, 0x00u, 0x00u, 0x0Du, 0x49u, 0x48u, 0x44u, 0x52u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u,
        0x08u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x7Fu, 0x80u, 0xFFu, 0x00u, 0x00u, 0x00u,
        0x00u,
    };
    const Bytes pinned_patch = {
        0x46u, 0x52u, 0x41u, 0x54u, 0x4Du, 0x45u, 0x4Eu, 0x55u,
        0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x69u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x4Cu, 0x07u, 0xB3u, 0xFFu,
        0x26u, 0x00u, 0x00u, 0x00u, 0x59u, 0xEBu, 0x5Eu, 0x54u,
        0x7Bu, 0xD2u, 0xC0u, 0xEEu, 0xE3u, 0x4Au, 0x58u, 0x76u,
        0x6Bu, 0xC2u, 0xD0u, 0xFEu, 0xD3u, 0x3Au, 0x68u, 0x86u,
        0x5Bu, 0xB2u, 0xE0u, 0x0Eu, 0xC3u, 0x2Au, 0x78u, 0x96u,
        0x4Bu, 0xA2u, 0xF0u, 0x1Eu, 0x33u, 0x1Au, 0x08u, 0xA6u,
        0xBBu, 0x83u, 0xDDu, 0x9Du, 0x98u, 0xD6u,
    };
    const Bytes expected_patched_ypf = {
        0x59u, 0x50u, 0x46u, 0x00u, 0xEAu, 0x01u, 0x00u, 0x00u,
        0x01u, 0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0xE4u, 0xDFu, 0xA8u, 0xA2u, 0xFAu, 0x9Eu, 0xD1u, 0x8Fu,
        0x91u, 0x98u, 0x02u, 0x00u, 0x26u, 0x00u, 0x00u, 0x00u,
        0x26u, 0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x59u, 0xEBu, 0x5Eu, 0x54u,
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
        0x00u, 0x00u, 0x00u, 0x0Du, 0x49u, 0x48u, 0x44u, 0x52u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u,
        0x08u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x00u, 0x00u,
        0x00u, 0x00u,
    };
    // Exact 40-byte FRATAPLD sentinel: magic + ver=1 + Murmur2(0x90D5C2FE) +
    // FNV-1a-64(0x10D31791F4EB0A26) + patch_size(78) + ypf_size(106).
    const Bytes expected_sentinel = {
        0x46u, 0x52u, 0x41u, 0x54u, 0x41u, 0x50u, 0x4Cu, 0x44u,
        0x01u, 0x00u, 0x00u, 0x00u, 0xFEu, 0xC2u, 0xD5u, 0x90u,
        0x26u, 0x0Au, 0xEBu, 0xF4u, 0x91u, 0x17u, 0xD3u, 0x10u,
        0x4Eu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x6Au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };

    Bytes out_ypf;
    ASSERT_TRUE(menu_autopatch::ApplyXorMenuPatchInMemory(
        pinned_orig_ypf, pinned_patch, out_ypf));
    EXPECT_EQ(out_ypf, expected_patched_ypf);

    const ScopedTempDir tmp("frat_ut_pinned_sentinel");
    const MenuAutoPatcher patcher(L"Local\\FratUtPinnedSentinelMutex");
    const fs::path ypf_path = tmp.path() / "pac" / "cgsys.ypf";
    const fs::path patch_path = tmp.path() / "cgsys_patch.bin";
    const fs::path sentinel_path = tmp.path() / "pac" / "cgsys.ypf.applied";

    WriteBytes(ypf_path, pinned_orig_ypf);
    WriteBytes(patch_path, pinned_patch);

    const PatchResult r1 = patcher.Apply(tmp.path().wstring());
    ASSERT_EQ(r1.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(ypf_path), expected_patched_ypf);
    EXPECT_EQ(ReadBytes(sentinel_path), expected_sentinel);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kAlreadyUpToDate);

    // Corrupt sentinel rejection: wrong magic, wrong version, truncated size, bit-flipped FNV64.
    {
        Bytes bad_magic = expected_sentinel;
        bad_magic[0] = 'X';
        WriteBytes(sentinel_path, bad_magic);
        EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kApplied);
        EXPECT_EQ(ReadBytes(sentinel_path), expected_sentinel);
    }
    {
        Bytes bad_ver = expected_sentinel;
        bad_ver[8] = 2u;
        WriteBytes(sentinel_path, bad_ver);
        EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kApplied);
        EXPECT_EQ(ReadBytes(sentinel_path), expected_sentinel);
    }
    {
        Bytes truncated(expected_sentinel.begin(), expected_sentinel.end() - 1);
        WriteBytes(sentinel_path, truncated);
        EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kApplied);
        EXPECT_EQ(ReadBytes(sentinel_path), expected_sentinel);
    }
    {
        Bytes bad_fnv = expected_sentinel;
        bad_fnv[16] ^= 0x01u;
        WriteBytes(sentinel_path, bad_fnv);
        EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kApplied);
        EXPECT_EQ(ReadBytes(sentinel_path), expected_sentinel);
    }
}

TEST(MenuAutoPatcherUt, StreamingMultiChunkHashBoundaryMatchesExactSentinel) {
    const ScopedTempDir tmp("frat_ut_multichunk_hash");
    const MenuAutoPatcher patcher(L"Local\\FratUtMultiChunkMutex");

    const Bytes orig_png = MakeSyntheticPng(1u, 1u, 2u, {0x00u, 0x7Fu, 0x80u, 0xFFu});
    const Bytes orig_ypf = BuildYpf({{"a.png", orig_png, false}});

    // Build a 16387-byte (16384 + 3) cgsys_patch.bin crossing kHashChunkBytes with 3 tail bytes.
    constexpr std::size_t kBigBodyLen = 16314u;
    Bytes big_body(kBigBodyLen);
    for (std::size_t i = 0u; i < kBigBodyLen; ++i) {
        big_body[i] = static_cast<std::uint8_t>((i * 41u + 7u) & 0xFFu);
    }
    const Bytes big_png = MakeSyntheticPng(1u, 1u, 2u, big_body);
    const Bytes big_patch = BuildFratMenuBlob(
        1u, static_cast<std::uint32_t>(orig_ypf.size()), {{0u, orig_png, big_png}});
    ASSERT_EQ(big_patch.size(), 16387u);

    Bytes expected_sentinel = {'F', 'R', 'A', 'T', 'A', 'P', 'L', 'D'};
    AppendU32Le(expected_sentinel, 1u);
    AppendU32Le(expected_sentinel, 0xAF514678u);
    AppendU64Le(expected_sentinel, 0xB6E6639728495634ULL);
    AppendU64Le(expected_sentinel, 16387u);
    AppendU64Le(expected_sentinel, 16415u);
    ASSERT_EQ(expected_sentinel.size(), 40u);

    WriteBytes(tmp.path() / "pac" / "cgsys.ypf", orig_ypf);
    WriteBytes(tmp.path() / "cgsys_patch.bin", big_patch);

    const PatchResult r1 = patcher.Apply(tmp.path().wstring());
    ASSERT_EQ(r1.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(tmp.path() / "pac" / "cgsys.ypf.applied"), expected_sentinel);

    // Launch 2 exercises multi-chunk streaming hash_file_contents across 16384 + 3 bytes.
    const PatchResult r2 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r2.status, PatchStatus::kAlreadyUpToDate);
    EXPECT_EQ(r2.patched_archive_bytes, 16415u);
}

TEST(MenuAutoPatcherUt, Rejects32BitAnd64BitWraparoundWithoutCrash) {
    const Bytes orig_png = MakeSyntheticPng(1u, 1u, 2u, {0x00u, 0x7Fu, 0x80u, 0xFFu});
    const Bytes new_png = MakeSyntheticPng(1u, 1u, 2u, {0x11u, 0x22u, 0x33u, 0x44u});
    const Bytes orig_ypf = BuildYpf({{"a.png", orig_png, false}});
    const Bytes valid_patch = BuildFratMenuBlob(
        1u, static_cast<std::uint32_t>(orig_ypf.size()), {{0u, orig_png, new_png}});
    Bytes out_ypf;

    // Case 1: YPF idx_size = 0xFFFFFFF0 wraps (idx_begin + idx_size) below idx_begin on Win32.
    {
        Bytes evil_ypf = orig_ypf;
        WriteU32Le(evil_ypf, 12u, 0xFFFFFFF0u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(evil_ypf, valid_patch, out_ypf));
    }

    // Case 2: YPF file_count = 0x10000000 must fail before reserving huge memory.
    {
        Bytes evil_ypf = orig_ypf;
        WriteU32Le(evil_ypf, 8u, 0x10000000u);
        Bytes evil_patch = valid_patch;
        WriteU32Le(evil_patch, 12u, 0x10000000u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(evil_ypf, evil_patch, out_ypf));
    }

    // Case 3: FRATMENU patch_entry_count = 0x10000000 must fail before reserving huge map.
    {
        Bytes evil_patch = valid_patch;
        WriteU32Le(evil_patch, 20u, 0x10000000u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(orig_ypf, evil_patch, out_ypf));
    }

    // Case 4: FRATMENU record new_raw_size = 0xFFFFFFF0 wraps (off + new_raw_size) on Win32.
    {
        Bytes evil_patch = valid_patch;
        // Record header starts at offset 24; new_raw_size is at +8 (offset 32).
        WriteU32Le(evil_patch, 32u, 0xFFFFFFF0u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(orig_ypf, evil_patch, out_ypf));
    }

    // Case 5: YPF entry data_offset = UINT64_MAX - 3 wraps (data_offset + packed_size) in 64-bit.
    {
        Bytes evil_ypf = orig_ypf;
        // Header (32) + prefix (5) + "a.png" (5) + footer offset +10 = offset 52.
        WriteU64Le(evil_ypf, 52u, std::numeric_limits<std::uint64_t>::max() - 3u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(evil_ypf, valid_patch, out_ypf));
    }
}
