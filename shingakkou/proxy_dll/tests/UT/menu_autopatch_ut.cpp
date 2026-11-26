// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for shingakkou menu_autopatch::MenuAutoPatcher
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
#include <utility>
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

constexpr std::size_t kDdp2HeaderSize = 32u;
constexpr std::size_t kDdp2EntrySize = 16u;
constexpr std::uint8_t kShsLiteral32Opcode = 0x1Fu;
constexpr std::uint32_t kPatchVersion = 1u;
constexpr std::uint32_t kArchiveSinSysd = 0u;
constexpr std::uint32_t kArchiveBootup = 1u;
constexpr std::uint32_t kMurmurM = 0x5BD1E995u;
constexpr int kMurmurR = 24;

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

std::uint32_t WireMurmur2(const Bytes& data) {
    std::uint32_t h = static_cast<std::uint32_t>(data.size());
    std::size_t i = 0u;
    while (i + 4u <= data.size()) {
        std::uint32_t k = static_cast<std::uint32_t>(data[i]) |
                          (static_cast<std::uint32_t>(data[i + 1u]) << 8) |
                          (static_cast<std::uint32_t>(data[i + 2u]) << 16) |
                          (static_cast<std::uint32_t>(data[i + 3u]) << 24);
        k *= kMurmurM;
        k ^= k >> kMurmurR;
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

Bytes WrapShsLiteral(const Bytes& raw) {
    Bytes out;
    out.reserve(5u + raw.size());
    out.push_back(kShsLiteral32Opcode);
    const auto n = static_cast<std::uint32_t>(raw.size());
    out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(n & 0xFFu));
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

Bytes MakeDdp2(const std::vector<Bytes>& entries) {
    Bytes out = {'D', 'D', 'P', '2'};
    AppendU32Le(out, static_cast<std::uint32_t>(entries.size()));
    out.resize(kDdp2HeaderSize, 0u);

    std::vector<Bytes> packed;
    packed.reserve(entries.size());
    for (const Bytes& e : entries) {
        packed.push_back(WrapShsLiteral(e));
    }

    std::uint32_t cursor =
        static_cast<std::uint32_t>(kDdp2HeaderSize + entries.size() * kDdp2EntrySize);
    for (std::size_t i = 0u; i < entries.size(); ++i) {
        AppendU32Le(out, cursor);
        AppendU32Le(out, static_cast<std::uint32_t>(entries[i].size()));
        AppendU32Le(out, static_cast<std::uint32_t>(packed[i].size()));
        AppendU32Le(out, 0u);
        cursor += static_cast<std::uint32_t>(packed[i].size());
    }
    for (const Bytes& p : packed) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

struct RecordSpec {
    std::uint32_t entry_index = 0u;
    Bytes orig_raw;
    Bytes new_raw;
};

Bytes MakeShinMenuBlob(const Bytes& orig_sin,
                       std::uint32_t sin_total,
                       const std::vector<RecordSpec>& sin_records,
                       const Bytes& orig_boot,
                       std::uint32_t boot_total,
                       const std::vector<RecordSpec>& boot_records) {
    Bytes out = {'S', 'H', 'I', 'N', 'M', 'E', 'N', 'U'};
    AppendU32Le(out, kPatchVersion);
    AppendU32Le(out, sin_total);
    AppendU32Le(out, static_cast<std::uint32_t>(orig_sin.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(sin_records.size()));
    AppendU32Le(out, boot_total);
    AppendU32Le(out, static_cast<std::uint32_t>(orig_boot.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(boot_records.size()));

    auto append_rec = [&](std::uint32_t arc_id, const RecordSpec& rec) {
        const Bytes orig_stored = WrapShsLiteral(rec.orig_raw);
        const std::uint32_t orig_hash = WireMurmur2(orig_stored);
        const std::uint32_t new_hash = WireMurmur2(rec.new_raw);
        AppendU32Le(out, arc_id);
        AppendU32Le(out, rec.entry_index);
        AppendU32Le(out, orig_hash);
        AppendU32Le(out, static_cast<std::uint32_t>(rec.new_raw.size()));
        AppendU32Le(out, new_hash);
        for (std::size_t k = 0u; k < rec.new_raw.size(); ++k) {
            const auto k32 = static_cast<std::uint32_t>(k);
            const auto hash_byte =
                static_cast<std::uint8_t>((orig_hash >> ((k32 & 3u) * 8u)) & 0xFFu);
            const auto stream_byte =
                static_cast<std::uint8_t>((k32 * 0x9Eu + 0x37u) & 0xFFu);
            const auto mask = static_cast<std::uint8_t>(
                orig_stored[k % orig_stored.size()] ^ hash_byte ^ stream_byte);
            out.push_back(static_cast<std::uint8_t>(rec.new_raw[k] ^ mask));
        }
    };

    for (const RecordSpec& r : sin_records) append_rec(kArchiveSinSysd, r);
    for (const RecordSpec& r : boot_records) append_rec(kArchiveBootup, r);
    return out;
}

void WriteBytes(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(f));
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(static_cast<bool>(f));
}

Bytes ReadBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0, std::ios::beg);
    Bytes out(static_cast<std::size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(out.data()), sz)) return {};
    return out;
}

}  // namespace

TEST(MenuAutoPatcherUt, SkipsWhenInputsAreMissingAndRejectsInvalidHeaders) {
    const ScopedTempDir tmp("shin_ut_missing_and_bad_hdr");
    const MenuAutoPatcher patcher(L"Local\\ShinUtMissingMutex");

    // Missing menu_patch.bin
    const PatchResult no_patch = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(no_patch.status, PatchStatus::kSkippedMissingInput);

    // Patch exists, but neither live nor .bak archives exist
    WriteBytes(tmp.path() / "menu_patch.bin", Bytes{'S', 'H', 'I', 'N'});
    const PatchResult no_archives = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(no_archives.status, PatchStatus::kSkippedMissingInput);

    // Archive exists, but menu_patch.bin is truncated / bad magic -> no .bak created
    const Bytes orig_sin = MakeDdp2({Bytes{0x00, 0x7F, 0x80, 0xFF}});
    WriteBytes(tmp.path() / "Data" / "sin_sysd.dat", orig_sin);
    const PatchResult bad_hdr = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(bad_hdr.status, PatchStatus::kFailed);
    EXPECT_FALSE(fs::exists(tmp.path() / "Data" / "sin_sysd.dat.bak"));
    EXPECT_EQ(ReadBytes(tmp.path() / "Data" / "sin_sysd.dat"), orig_sin);

    // Tampered Launch-1 patch must fail in memory before creating any .bak or .tmp
    Bytes bad_launch1 = MakeShinMenuBlob(orig_sin, 1u, {{0u, Bytes{0x00, 0x7F, 0x80, 0xFF}, Bytes{'X', 'Y'}}},
                                         {}, 0u, {});
    bad_launch1.back() ^= 0xFFu;
    WriteBytes(tmp.path() / "menu_patch.bin", bad_launch1);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kFailed);
    EXPECT_FALSE(fs::exists(tmp.path() / "Data" / "sin_sysd.dat.bak"));
    EXPECT_FALSE(fs::exists(tmp.path() / "Data" / "sin_sysd.dat.bak.tmp"));
    EXPECT_FALSE(fs::exists(tmp.path() / "Data" / "sin_sysd.dat.tmp"));
    EXPECT_EQ(ReadBytes(tmp.path() / "Data" / "sin_sysd.dat"), orig_sin);
}

TEST(MenuAutoPatcherUt, AppliesV1FastPathsLaunch2AndReAppliesSameAndDifferentSizeV2FromBak) {
    const ScopedTempDir tmp("shin_ut_v2_upgrade");
    const MenuAutoPatcher patcher(L"Local\\ShinUtV2UpgradeMutex");

    const Bytes orig_s0 = {0x00u, 0x7Fu, 0x80u, 0xFFu, 'O', 'R', 'I', 'G', '0'};
    const Bytes orig_s1 = {'K', 'E', 'E', 'P', '_', 'S', '1'};
    const Bytes orig_b0 = {0x80u, 0xFFu, 'B', 'O', 'O', 'T', '0'};

    const Bytes v1_s0 = {'V', '1', '_', 'S', 'Y', 'S', 'D', '_', '0', '1', '2', '3'};
    const Bytes v1_b0 = {'V', '1', '_', 'B', 'O', 'O', 'T', '_', 'A', 'B'};

    // v2_same has the EXACT same byte length as v1 so size-only checks would skip it!
    const Bytes v2_same_s0 = {'V', '2', '_', 'S', 'A', 'M', 'E', '_', '9', '8', '7', '6'};
    const Bytes v2_same_b0 = {'V', '2', '_', 'B', 'O', 'O', 'T', '_', 'X', 'Y'};

    // v3_resized changes length and exercises tail-byte remainders.
    const Bytes v3_s0 = {'V', '3', '_', 'L', 'O', 'N', 'G', '_', 'P', 'A', 'Y', 'L', 'O', 'A', 'D'};
    const Bytes v3_b0 = {'V', '3', 'B'};

    const Bytes orig_sin = MakeDdp2({orig_s0, orig_s1});
    const Bytes orig_boot = MakeDdp2({orig_b0});

    const fs::path sin_path = tmp.path() / "Data" / "sin_sysd.dat";
    const fs::path sin_bak = tmp.path() / "Data" / "sin_sysd.dat.bak";
    const fs::path boot_path = tmp.path() / "Bootup.dat";
    const fs::path boot_bak = tmp.path() / "Bootup.dat.bak";
    const fs::path patch_path = tmp.path() / "menu_patch.bin";
    const fs::path sentinel_path = tmp.path() / "menu_patch.applied";

    WriteBytes(sin_path, orig_sin);
    WriteBytes(boot_path, orig_boot);

    // Plant stale interrupted .bak.tmp files before Launch 1 to verify atomic .bak cleanup.
    WriteBytes(tmp.path() / "Data" / "sin_sysd.dat.bak.tmp", Bytes{'T', 'R', 'U', 'N', 'C'});
    WriteBytes(tmp.path() / "Bootup.dat.bak.tmp", Bytes{'T', 'R', 'U', 'N', 'C'});

    // Launch 1: apply v1
    WriteBytes(patch_path, MakeShinMenuBlob(orig_sin, 2u, {{0u, orig_s0, v1_s0}},
                                            orig_boot, 1u, {{0u, orig_b0, v1_b0}}));
    const PatchResult r1 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r1.status, PatchStatus::kApplied);
    EXPECT_EQ(r1.patched_sin_sysd_bytes, MakeDdp2({v1_s0, orig_s1}).size());
    EXPECT_EQ(r1.patched_bootup_bytes, MakeDdp2({v1_b0}).size());
    EXPECT_EQ(ReadBytes(sin_bak), orig_sin);
    EXPECT_EQ(ReadBytes(boot_bak), orig_boot);
    EXPECT_EQ(ReadBytes(sin_path), MakeDdp2({v1_s0, orig_s1}));
    EXPECT_EQ(ReadBytes(boot_path), MakeDdp2({v1_b0}));
    EXPECT_TRUE(fs::exists(sentinel_path));
    EXPECT_FALSE(fs::exists(tmp.path() / "Data" / "sin_sysd.dat.bak.tmp"));
    EXPECT_FALSE(fs::exists(tmp.path() / "Bootup.dat.bak.tmp"));

    // Launch 2: identical v1 hits fast path
    const PatchResult r2 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r2.status, PatchStatus::kAlreadyUpToDate);
    EXPECT_EQ(r2.patched_sin_sysd_bytes, r1.patched_sin_sysd_bytes);
    EXPECT_EQ(r2.patched_bootup_bytes, r1.patched_bootup_bytes);

    // Launch 3: same-size v2 upgrade MUST re-apply from .bak
    WriteBytes(patch_path, MakeShinMenuBlob(orig_sin, 2u, {{0u, orig_s0, v2_same_s0}},
                                            orig_boot, 1u, {{0u, orig_b0, v2_same_b0}}));
    const PatchResult r3 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r3.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(sin_bak), orig_sin);
    EXPECT_EQ(ReadBytes(boot_bak), orig_boot);
    EXPECT_EQ(ReadBytes(sin_path), MakeDdp2({v2_same_s0, orig_s1}));
    EXPECT_EQ(ReadBytes(boot_path), MakeDdp2({v2_same_b0}));

    // Launch 4: same v2 is now up to date
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kAlreadyUpToDate);

    // Launch 5: simulate legacy player (no sentinel file, already patched live + pristine .bak)
    fs::remove(sentinel_path);
    WriteBytes(patch_path, MakeShinMenuBlob(orig_sin, 2u, {{0u, orig_s0, v3_s0}},
                                            orig_boot, 1u, {{0u, orig_b0, v3_b0}}));
    const PatchResult r5 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r5.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(sin_path), MakeDdp2({v3_s0, orig_s1}));
    EXPECT_EQ(ReadBytes(boot_path), MakeDdp2({v3_b0}));
    EXPECT_EQ(ReadBytes(sin_bak), orig_sin);
    EXPECT_EQ(ReadBytes(boot_bak), orig_boot);
}

TEST(MenuAutoPatcherUt, RecoversMissingLiveArchivesFromBakAndRollsBackOnCorruptUpgrade) {
    const ScopedTempDir tmp("shin_ut_recovery_and_rollback");
    const MenuAutoPatcher patcher(L"Local\\ShinUtRecoveryMutex");

    const Bytes orig_s0 = {'S', '0', 0x00u, 0x80u, 0xFFu};
    const Bytes orig_b0 = {'B', '0', 0x7Fu, 0x80u};
    const Bytes v1_s0 = {'P', 'A', 'T', 'C', 'H', 'E', 'D', '_', 'S', '0'};
    const Bytes v1_b0 = {'P', 'A', 'T', 'C', 'H', 'E', 'D', '_', 'B', '0'};

    const Bytes orig_sin = MakeDdp2({orig_s0});
    const Bytes orig_boot = MakeDdp2({orig_b0});
    const Bytes v1_patch = MakeShinMenuBlob(orig_sin, 1u, {{0u, orig_s0, v1_s0}},
                                            orig_boot, 1u, {{0u, orig_b0, v1_b0}});

    const fs::path sin_path = tmp.path() / "Data" / "sin_sysd.dat";
    const fs::path sin_bak = tmp.path() / "Data" / "sin_sysd.dat.bak";
    const fs::path boot_path = tmp.path() / "Bootup.dat";
    const fs::path boot_bak = tmp.path() / "Bootup.dat.bak";
    const fs::path patch_path = tmp.path() / "menu_patch.bin";

    // Start with ONLY .bak files (both live archives missing!)
    WriteBytes(sin_bak, orig_sin);
    WriteBytes(boot_bak, orig_boot);
    WriteBytes(patch_path, v1_patch);

    const PatchResult rec_both = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(rec_both.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(sin_path), MakeDdp2({v1_s0}));
    EXPECT_EQ(ReadBytes(boot_path), MakeDdp2({v1_b0}));

    // Delete only Bootup.dat while sentinel exists -> must recover cleanly
    fs::remove(boot_path);
    const PatchResult rec_boot = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(rec_boot.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(boot_path), MakeDdp2({v1_b0}));

    // Valid -> Failing -> Valid seam: tampered v2 must fail without corrupting live v1
    Bytes tampered = v1_patch;
    tampered.back() ^= 0xFFu;
    WriteBytes(patch_path, tampered);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kFailed);
    EXPECT_EQ(ReadBytes(sin_path), MakeDdp2({v1_s0}));
    EXPECT_EQ(ReadBytes(boot_path), MakeDdp2({v1_b0}));
    EXPECT_FALSE(fs::exists(tmp.path() / "Data" / "sin_sysd.dat.tmp"));
    EXPECT_FALSE(fs::exists(tmp.path() / "Bootup.dat.tmp"));

    // Restore valid patch -> returns up-to-date
    WriteBytes(patch_path, v1_patch);
    EXPECT_EQ(patcher.Apply(tmp.path().wstring()).status, PatchStatus::kAlreadyUpToDate);
}

TEST(MenuAutoPatcherUt, PinnedWireVectorExact48ByteSentinelAndCorruptSentinelRejection) {
    // Independently precomputed canonical wire bytes (zero helper hash calls):
    // 1-entry DDP2 ("ABC" -> 5-byte {0x00, 0x7F, 0x80, 0xFF, 'Z'}), no Bootup.
    const Bytes pinned_orig_sin = {
        0x44u, 0x44u, 0x50u, 0x32u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x30u, 0x00u, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u,
        0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x1Fu, 0x00u, 0x00u, 0x00u, 0x03u, 0x41u, 0x42u, 0x43u,
    };
    const Bytes pinned_patch = {
        0x53u, 0x48u, 0x49u, 0x4Eu, 0x4Du, 0x45u, 0x4Eu, 0x55u,
        0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x38u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x45u, 0xCFu, 0xF0u, 0x74u,
        0x05u, 0x00u, 0x00u, 0x00u, 0xAEu, 0x2Bu, 0x1Cu, 0x01u,
        0x6Du, 0x65u, 0x03u, 0x9Au, 0xB3u,
    };
    const Bytes expected_patched_sin = {
        0x44u, 0x44u, 0x50u, 0x32u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x30u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u,
        0x0Au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x1Fu, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x7Fu, 0x80u,
        0xFFu, 0x5Au,
    };
    // Exact 48-byte SHINAPLD sentinel: magic + ver=1 + Murmur2(0x9EB7F3C9) +
    // FNV-1a-64(0x23EAD7F8B8C371B2) + patch_size(61) + sin_size(58) + boot_size(0).
    const Bytes expected_sentinel = {
        0x53u, 0x48u, 0x49u, 0x4Eu, 0x41u, 0x50u, 0x4Cu, 0x44u,
        0x01u, 0x00u, 0x00u, 0x00u, 0xC9u, 0xF3u, 0xB7u, 0x9Eu,
        0xB2u, 0x71u, 0xC3u, 0xB8u, 0xF8u, 0xD7u, 0xEAu, 0x23u,
        0x3Du, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x3Au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };

    Bytes out_sin;
    Bytes out_boot;
    ASSERT_TRUE(menu_autopatch::ApplyXorMenuPatchInMemory(
        pinned_orig_sin, {}, pinned_patch, out_sin, out_boot));
    EXPECT_EQ(out_sin, expected_patched_sin);
    EXPECT_TRUE(out_boot.empty());

    const ScopedTempDir tmp("shin_ut_pinned_sentinel");
    const MenuAutoPatcher patcher(L"Local\\ShinUtPinnedSentinelMutex");
    const fs::path sin_path = tmp.path() / "Data" / "sin_sysd.dat";
    const fs::path patch_path = tmp.path() / "menu_patch.bin";
    const fs::path sentinel_path = tmp.path() / "menu_patch.applied";

    WriteBytes(sin_path, pinned_orig_sin);
    WriteBytes(patch_path, pinned_patch);

    const PatchResult r1 = patcher.Apply(tmp.path().wstring());
    ASSERT_EQ(r1.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(sin_path), expected_patched_sin);
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
    const ScopedTempDir tmp("shin_ut_multichunk_hash");
    const MenuAutoPatcher patcher(L"Local\\ShinUtMultiChunkMutex");

    const Bytes orig_raw = {'A', 'B', 'C'};
    const Bytes orig_sin = MakeDdp2({orig_raw});

    // Build a 16387-byte (16384 + 3) menu_patch.bin crossing kHashChunkBytes with 3 tail bytes.
    constexpr std::size_t kBigNewLen = 16387u - 36u - 20u;
    Bytes big_new(kBigNewLen);
    for (std::size_t i = 0u; i < kBigNewLen; ++i) {
        big_new[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    const Bytes big_patch =
        MakeShinMenuBlob(orig_sin, 1u, {{0u, orig_raw, big_new}}, {}, 0u, {});
    ASSERT_EQ(big_patch.size(), 16387u);

    Bytes expected_sentinel = {'S', 'H', 'I', 'N', 'A', 'P', 'L', 'D'};
    AppendU32Le(expected_sentinel, 1u);
    AppendU32Le(expected_sentinel, 0xCD90814Eu);
    AppendU64Le(expected_sentinel, 0x899C4D0DB0A0C22BULL);
    AppendU64Le(expected_sentinel, 16387u);
    AppendU64Le(expected_sentinel, 16384u);
    AppendU64Le(expected_sentinel, 0u);
    ASSERT_EQ(expected_sentinel.size(), 48u);

    WriteBytes(tmp.path() / "Data" / "sin_sysd.dat", orig_sin);
    WriteBytes(tmp.path() / "menu_patch.bin", big_patch);

    const PatchResult r1 = patcher.Apply(tmp.path().wstring());
    ASSERT_EQ(r1.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadBytes(tmp.path() / "menu_patch.applied"), expected_sentinel);

    // Launch 2 exercises multi-chunk streaming hash_file_contents across 16384 + 3 bytes.
    const PatchResult r2 = patcher.Apply(tmp.path().wstring());
    EXPECT_EQ(r2.status, PatchStatus::kAlreadyUpToDate);
    EXPECT_EQ(r2.patched_sin_sysd_bytes, 16384u);
}

TEST(MenuAutoPatcherUt, Rejects32BitSizeAndOffsetWraparoundWithoutCrash) {
    Bytes out_sin;
    Bytes out_boot;

    // Case 1: DDP2 entry count = 0x10000001 wraps (32 + count * 16) to 48 on Win32.
    {
        Bytes evil_ddp = MakeDdp2({Bytes{'A', 'B', 'C'}});
        WriteU32Le(evil_ddp, 4u, 0x10000001u);
        Bytes patch = {'S', 'H', 'I', 'N', 'M', 'E', 'N', 'U'};
        AppendU32Le(patch, kPatchVersion);
        AppendU32Le(patch, 0x10000001u);
        AppendU32Le(patch, static_cast<std::uint32_t>(evil_ddp.size()));
        AppendU32Le(patch, 0u);
        AppendU32Le(patch, 0u);
        AppendU32Le(patch, 0u);
        AppendU32Le(patch, 0u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(
            evil_ddp, {}, patch, out_sin, out_boot));
    }

    // Case 2: sin_sysd_patch_count + bootup_patch_count wraps 32-bit size_t to 0.
    {
        const Bytes orig_sin = MakeDdp2({Bytes{'A', 'B', 'C'}});
        const Bytes orig_boot = MakeDdp2({Bytes{'D', 'E'}});
        Bytes patch = {'S', 'H', 'I', 'N', 'M', 'E', 'N', 'U'};
        AppendU32Le(patch, kPatchVersion);
        AppendU32Le(patch, 1u);
        AppendU32Le(patch, static_cast<std::uint32_t>(orig_sin.size()));
        AppendU32Le(patch, 0xFFFFFFFFu);
        AppendU32Le(patch, 1u);
        AppendU32Le(patch, static_cast<std::uint32_t>(orig_boot.size()));
        AppendU32Le(patch, 1u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(
            orig_sin, orig_boot, patch, out_sin, out_boot));
    }

    // Case 3: record new_unpacked_size = 0xFFFFFFF0 wraps (off + new_unpacked_size) on Win32.
    {
        const Bytes orig_raw = {'A', 'B', 'C'};
        const Bytes orig_sin = MakeDdp2({orig_raw});
        Bytes patch = MakeShinMenuBlob(
            orig_sin, 1u, {{0u, orig_raw, Bytes{'X', 'Y', 'Z'}}}, {}, 0u, {});
        // Record header starts at offset 36; new_unpacked_size is at +12 (offset 48).
        WriteU32Le(patch, 48u, 0xFFFFFFF0u);
        EXPECT_FALSE(menu_autopatch::ApplyXorMenuPatchInMemory(
            orig_sin, {}, patch, out_sin, out_boot));
    }
}
