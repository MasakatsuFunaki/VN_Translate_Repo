// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room component tests for shingakkou's shipped menu auto-patcher
// driven strictly through MenuAutoPatcher / EnsureMenuArchivePatchedInDir.
#include "menu_autopatch.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using menu_autopatch::EnsureMenuArchivePatchedInDir;
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

std::uint32_t ReadU32Le(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1u]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2u]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3u]) << 24);
}

std::uint32_t ReadU32Be(const Bytes& b, std::size_t off) {
    return (static_cast<std::uint32_t>(b[off]) << 24) |
           (static_cast<std::uint32_t>(b[off + 1u]) << 16) |
           (static_cast<std::uint32_t>(b[off + 2u]) << 8) |
           static_cast<std::uint32_t>(b[off + 3u]);
}

std::uint64_t ReadU64Le(const Bytes& b, std::size_t off) {
    std::uint64_t v = 0u;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | b[off + static_cast<std::size_t>(i)];
    }
    return v;
}

void AppendU32Le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

std::uint32_t Murmur2(const Bytes& data) {
    constexpr std::uint32_t kM = 0x5BD1E995u;
    std::uint32_t h = static_cast<std::uint32_t>(data.size());
    std::size_t i = 0u;
    while (i + 4u <= data.size()) {
        std::uint32_t k = ReadU32Le(data, i);
        k *= kM;
        k ^= k >> 24;
        k *= kM;
        h *= kM;
        h ^= k;
        i += 4u;
    }
    const std::size_t rem = data.size() - i;
    if (rem == 3u) h ^= static_cast<std::uint32_t>(data[i + 2u]) << 16;
    if (rem >= 2u) h ^= static_cast<std::uint32_t>(data[i + 1u]) << 8;
    if (rem >= 1u) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kM;
    }
    h ^= h >> 13;
    h *= kM;
    h ^= h >> 15;
    return h;
}

Bytes ShsLiteral(const Bytes& raw) {
    Bytes out;
    out.reserve(5u + raw.size());
    out.push_back(0x1Fu);
    const auto n = static_cast<std::uint32_t>(raw.size());
    out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(n & 0xFFu));
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

Bytes BuildDdp2(const std::vector<Bytes>& raw_entries) {
    Bytes out = {'D', 'D', 'P', '2'};
    AppendU32Le(out, static_cast<std::uint32_t>(raw_entries.size()));
    out.resize(kDdp2HeaderSize, 0u);
    std::vector<Bytes> wrapped;
    wrapped.reserve(raw_entries.size());
    for (const Bytes& e : raw_entries) wrapped.push_back(ShsLiteral(e));

    std::uint32_t off =
        static_cast<std::uint32_t>(kDdp2HeaderSize + raw_entries.size() * kDdp2EntrySize);
    for (std::size_t i = 0u; i < raw_entries.size(); ++i) {
        AppendU32Le(out, off);
        AppendU32Le(out, static_cast<std::uint32_t>(raw_entries[i].size()));
        AppendU32Le(out, static_cast<std::uint32_t>(wrapped[i].size()));
        AppendU32Le(out, 0u);
        off += static_cast<std::uint32_t>(wrapped[i].size());
    }
    for (const Bytes& w : wrapped) out.insert(out.end(), w.begin(), w.end());
    return out;
}

struct PatchRec {
    std::uint32_t idx = 0u;
    Bytes orig_raw;
    Bytes repl_raw;
};

Bytes BuildShinMenu(const Bytes& orig_sin,
                    std::uint32_t sin_total,
                    const std::vector<PatchRec>& sin_recs,
                    const Bytes& orig_boot,
                    std::uint32_t boot_total,
                    const std::vector<PatchRec>& boot_recs) {
    Bytes out = {'S', 'H', 'I', 'N', 'M', 'E', 'N', 'U'};
    AppendU32Le(out, 1u);
    AppendU32Le(out, sin_total);
    AppendU32Le(out, static_cast<std::uint32_t>(orig_sin.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(sin_recs.size()));
    AppendU32Le(out, boot_total);
    AppendU32Le(out, static_cast<std::uint32_t>(orig_boot.size()));
    AppendU32Le(out, static_cast<std::uint32_t>(boot_recs.size()));

    auto emit = [&](std::uint32_t arc_id, const PatchRec& rec) {
        const Bytes orig_stored = ShsLiteral(rec.orig_raw);
        const std::uint32_t orig_hash = Murmur2(orig_stored);
        const std::uint32_t new_hash = Murmur2(rec.repl_raw);
        AppendU32Le(out, arc_id);
        AppendU32Le(out, rec.idx);
        AppendU32Le(out, orig_hash);
        AppendU32Le(out, static_cast<std::uint32_t>(rec.repl_raw.size()));
        AppendU32Le(out, new_hash);
        for (std::size_t k = 0u; k < rec.repl_raw.size(); ++k) {
            const auto k32 = static_cast<std::uint32_t>(k);
            const auto hb = static_cast<std::uint8_t>((orig_hash >> ((k32 & 3u) * 8u)) & 0xFFu);
            const auto sb = static_cast<std::uint8_t>((k32 * 0x9Eu + 0x37u) & 0xFFu);
            const auto mask = static_cast<std::uint8_t>(
                orig_stored[k % orig_stored.size()] ^ hb ^ sb);
            out.push_back(static_cast<std::uint8_t>(rec.repl_raw[k] ^ mask));
        }
    };
    for (const auto& r : sin_recs) emit(0u, r);
    for (const auto& r : boot_recs) emit(1u, r);
    return out;
}

// Independent DDP2 wire consumer: validates header, index table, and SHS literal framing.
void AssertValidDdp2Entries(const Bytes& archive, const std::vector<Bytes>& expected_raw) {
    ASSERT_GE(archive.size(), kDdp2HeaderSize);
    ASSERT_EQ(std::memcmp(archive.data(), "DDP2", 4u), 0);
    const std::uint32_t count = ReadU32Le(archive, 4u);
    ASSERT_EQ(count, expected_raw.size());
    for (std::size_t i = 0u; i < expected_raw.size(); ++i) {
        const std::size_t entry_off = kDdp2HeaderSize + i * kDdp2EntrySize;
        const std::uint32_t data_off = ReadU32Le(archive, entry_off);
        const std::uint32_t unpacked = ReadU32Le(archive, entry_off + 4u);
        const std::uint32_t packed = ReadU32Le(archive, entry_off + 8u);
        ASSERT_EQ(unpacked, expected_raw[i].size());
        ASSERT_EQ(packed, expected_raw[i].size() + 5u);
        ASSERT_LE(static_cast<std::size_t>(data_off) + packed, archive.size());
        EXPECT_EQ(archive[data_off], 0x1Fu);
        EXPECT_EQ(ReadU32Be(archive, static_cast<std::size_t>(data_off) + 1u),
                  static_cast<std::uint32_t>(expected_raw[i].size()));
        const Bytes payload(
            archive.begin() + static_cast<std::ptrdiff_t>(data_off + 5u),
            archive.begin() + static_cast<std::ptrdiff_t>(data_off + packed));
        EXPECT_EQ(payload, expected_raw[i]);
    }
}

void AssertValidSentinel(const Bytes& sent,
                         std::uint64_t expected_patch_size,
                         std::uint64_t expected_sin_size,
                         std::uint64_t expected_boot_size) {
    ASSERT_EQ(sent.size(), 48u);
    EXPECT_EQ(std::memcmp(sent.data(), "SHINAPLD", 8u), 0);
    EXPECT_EQ(ReadU32Le(sent, 8u), 1u);
    EXPECT_EQ(ReadU64Le(sent, 24u), expected_patch_size);
    EXPECT_EQ(ReadU64Le(sent, 32u), expected_sin_size);
    EXPECT_EQ(ReadU64Le(sent, 40u), expected_boot_size);
}

void WriteFile(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(static_cast<bool>(out));
}

Bytes ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto sz = in.tellg();
    if (sz <= 0) return {};
    in.seekg(0, std::ios::beg);
    Bytes out(static_cast<std::size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(out.data()), sz)) return {};
    return out;
}

bool HasAnyTempFiles(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".tmp") return true;
    }
    return false;
}

}  // namespace

TEST(ShingakkouMenuAutoPatchCt, MultiArchiveUpgradeShrinkingSetRecoveryAndFaultIsolation) {
    const ScopedTempDir sandbox("shin_menu_autopatch_ct");
    const fs::path game_dir = sandbox.path();
    const fs::path sin_path = game_dir / "Data" / "sin_sysd.dat";
    const fs::path sin_bak = game_dir / "Data" / "sin_sysd.dat.bak";
    const fs::path boot_path = game_dir / "Bootup.dat";
    const fs::path boot_bak = game_dir / "Bootup.dat.bak";
    const fs::path patch_path = game_dir / "menu_patch.bin";
    const fs::path sentinel_path = game_dir / "menu_patch.applied";

    // Multi-entry payloads with binary edge bytes (0x00, 0x7F, 0x80, 0xFF) and UTF-8/CP932 text.
    const Bytes s0_orig = {0x00u, 0x00u, 0x02u, 0x7Fu, 0x80u, 0xFFu, 'T', 'G', 'A', '0'};
    const Bytes s1_orig = {'1', ',', '"', 0x82u, 0xA0u, '"', '\r', '\n'};
    const Bytes s2_orig = {'U', 'N', 'T', 'O', 'U', 'C', 'H', 'E', 'D', 0x00u, 0xFFu};

    const Bytes b0_orig = {'D', 'D', 'W', 'u', 'H', 'X', 'B', 0x00u, 0x80u, 0xFFu};
    const Bytes b1_orig = {'I', 'N', 'I', 'T', '_', 'J', 'P', 0x7Fu};

    const Bytes s0_v1 = {0x00u, 0x00u, 0x02u, 0x7Fu, 0x80u, 0xFFu, 'E', 'N', '_', 'V', '1'};
    const Bytes s1_v1 = {'1', ',', '"', 'E', 'N', '_', 'V', '1', '"', '\r', '\n'};
    const Bytes b0_v1 = {'D', 'D', 'W', 'u', 'H', 'X', 'B', 0x00u, 'E', 'N', '1'};
    const Bytes b1_v1 = {'I', 'N', 'I', 'T', '_', 'E', 'N', '1'};

    // v2 drops s1 and b1 (shrinking set -> must revert cleanly from .bak!) and updates s0 & b0.
    const Bytes s0_v2 = {0x00u, 0x00u, 0x02u, 0x7Fu, 0x80u, 0xFFu, 'E', 'N', '_', 'V', '2'};
    const Bytes b0_v2 = {'D', 'D', 'W', 'u', 'H', 'X', 'B', 0x00u, 'E', 'N', '2'};

    const Bytes orig_sin = BuildDdp2({s0_orig, s1_orig, s2_orig});
    const Bytes orig_boot = BuildDdp2({b0_orig, b1_orig});
    WriteFile(sin_path, orig_sin);
    WriteFile(boot_path, orig_boot);

    // Pass 1: apply v1 patching 2 entries in sin_sysd.dat and 2 entries in Bootup.dat.
    const Bytes v1_blob = BuildShinMenu(
        orig_sin, 3u, {{0u, s0_orig, s0_v1}, {1u, s1_orig, s1_v1}},
        orig_boot, 2u, {{0u, b0_orig, b0_v1}, {1u, b1_orig, b1_v1}});
    WriteFile(patch_path, v1_blob);

    ASSERT_TRUE(EnsureMenuArchivePatchedInDir(game_dir.wstring()));
    EXPECT_EQ(ReadFile(sin_bak), orig_sin);
    EXPECT_EQ(ReadFile(boot_bak), orig_boot);
    const Bytes v1_sin_bytes = ReadFile(sin_path);
    const Bytes v1_boot_bytes = ReadFile(boot_path);
    AssertValidDdp2Entries(v1_sin_bytes, {s0_v1, s1_v1, s2_orig});
    AssertValidDdp2Entries(v1_boot_bytes, {b0_v1, b1_v1});
    AssertValidSentinel(ReadFile(sentinel_path), v1_blob.size(), v1_sin_bytes.size(), v1_boot_bytes.size());
    EXPECT_FALSE(HasAnyTempFiles(game_dir));

    // Pass 2: idempotent replay hits fast path.
    const MenuAutoPatcher patcher;
    const PatchResult replay = patcher.Apply(game_dir.wstring());
    EXPECT_EQ(replay.status, PatchStatus::kAlreadyUpToDate);
    AssertValidDdp2Entries(ReadFile(sin_path), {s0_v1, s1_v1, s2_orig});
    AssertValidDdp2Entries(ReadFile(boot_path), {b0_v1, b1_v1});

    // Pass 3: v2 upgrade with shrinking record set (drops s1 and b1).
    const Bytes v2_blob = BuildShinMenu(
        orig_sin, 3u, {{0u, s0_orig, s0_v2}},
        orig_boot, 2u, {{0u, b0_orig, b0_v2}});
    WriteFile(patch_path, v2_blob);
    const PatchResult v2_res = patcher.Apply(game_dir.wstring());
    EXPECT_EQ(v2_res.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadFile(sin_bak), orig_sin);
    EXPECT_EQ(ReadFile(boot_bak), orig_boot);
    const Bytes v2_sin_bytes = ReadFile(sin_path);
    const Bytes v2_boot_bytes = ReadFile(boot_path);
    AssertValidDdp2Entries(v2_sin_bytes, {s0_v2, s1_orig, s2_orig});
    AssertValidDdp2Entries(v2_boot_bytes, {b0_v2, b1_orig});
    AssertValidSentinel(ReadFile(sentinel_path), v2_blob.size(), v2_sin_bytes.size(), v2_boot_bytes.size());
    EXPECT_FALSE(HasAnyTempFiles(game_dir));

    // Pass 4: delete live sin_sysd.dat -> automatic recovery from .bak.
    fs::remove(sin_path);
    const PatchResult rec_res = patcher.Apply(game_dir.wstring());
    EXPECT_EQ(rec_res.status, PatchStatus::kApplied);
    AssertValidDdp2Entries(ReadFile(sin_path), {s0_v2, s1_orig, s2_orig});
    AssertValidDdp2Entries(ReadFile(boot_path), {b0_v2, b1_orig});
    AssertValidSentinel(ReadFile(sentinel_path), v2_blob.size(), v2_sin_bytes.size(), v2_boot_bytes.size());

    // Pass 5: mid-batch fault (record #2 in Bootup tampered) leaves v2 live archives untouched.
    Bytes bad_v3 = v1_blob;
    bad_v3.back() ^= 0xFFu;
    WriteFile(patch_path, bad_v3);
    EXPECT_EQ(patcher.Apply(game_dir.wstring()).status, PatchStatus::kFailed);
    AssertValidDdp2Entries(ReadFile(sin_path), {s0_v2, s1_orig, s2_orig});
    AssertValidDdp2Entries(ReadFile(boot_path), {b0_v2, b1_orig});
    EXPECT_EQ(ReadFile(sin_bak), orig_sin);
    EXPECT_EQ(ReadFile(boot_bak), orig_boot);
    EXPECT_FALSE(HasAnyTempFiles(game_dir));
}
