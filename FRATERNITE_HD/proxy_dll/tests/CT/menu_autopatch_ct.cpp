// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room component tests for FRATERNITE_HD's shipped menu auto-patcher
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

constexpr std::uint32_t kYpfVersion490 = 490u;
constexpr std::size_t kYpfHeaderSize = 32u;
constexpr std::size_t kYpfEntryPrefixSize = 5u;
constexpr std::size_t kYpfEntryFooterSize = 22u;
constexpr std::uint8_t kNameXorKey = 0xFFu;
constexpr std::uint32_t kMurmurM = 0x5BD1E995u;

std::uint32_t ReadU32Le(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1u]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2u]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3u]) << 24);
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

void AppendU64Le(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

void AppendU32Be(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

std::uint32_t Murmur2(const Bytes& data) {
    std::uint32_t h = static_cast<std::uint32_t>(data.size());
    std::size_t i = 0u;
    while (i + 4u <= data.size()) {
        std::uint32_t k = ReadU32Le(data, i);
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

Bytes MakePng(std::uint32_t w,
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

struct YpfEntrySpec {
    Bytes cp932_name;
    Bytes payload;
    bool share_prev = false;
};

Bytes BuildYpf(const std::vector<YpfEntrySpec>& specs) {
    std::size_t idx_bytes = 0u;
    for (const auto& s : specs) {
        idx_bytes += kYpfEntryPrefixSize + s.cp932_name.size() + kYpfEntryFooterSize;
    }
    const std::uint64_t data_start = kYpfHeaderSize + idx_bytes;
    Bytes index;
    Bytes payloads;
    std::uint64_t last_off = data_start;

    for (std::size_t i = 0u; i < specs.size(); ++i) {
        const Bytes& name_b = specs[i].cp932_name;
        const Bytes& blob = specs[i].payload;
        std::uint64_t off = data_start + payloads.size();
        if (specs[i].share_prev && i > 0u) {
            off = last_off;
        } else {
            last_off = off;
            payloads.insert(payloads.end(), blob.begin(), blob.end());
            payloads.insert(payloads.end(), 4u, 0u);
        }

        AppendU32Le(index, Murmur2(name_b));
        index.push_back(static_cast<std::uint8_t>(name_b.size() ^ kNameXorKey));
        for (std::uint8_t b : name_b) {
            index.push_back(static_cast<std::uint8_t>(b ^ kNameXorKey));
        }
        index.push_back(2u);
        index.push_back(0u);
        AppendU32Le(index, static_cast<std::uint32_t>(blob.size()));
        AppendU32Le(index, static_cast<std::uint32_t>(blob.size()));
        AppendU64Le(index, off);
        AppendU32Le(index, Murmur2(blob));
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

struct PatchRec {
    std::uint32_t idx = 0u;
    Bytes orig_png;
    Bytes repl_png;
};

Bytes BuildFratPatch(std::uint32_t total_entries,
                     std::uint32_t orig_size,
                     const std::vector<PatchRec>& recs) {
    Bytes out = {'F', 'R', 'A', 'T', 'M', 'E', 'N', 'U'};
    AppendU32Le(out, 1u);
    AppendU32Le(out, total_entries);
    AppendU32Le(out, orig_size);
    AppendU32Le(out, static_cast<std::uint32_t>(recs.size()));

    for (const auto& rec : recs) {
        const std::uint32_t orig_hash = Murmur2(rec.orig_png);
        const std::uint32_t new_hash = Murmur2(rec.repl_png);
        AppendU32Le(out, rec.idx);
        AppendU32Le(out, orig_hash);
        AppendU32Le(out, static_cast<std::uint32_t>(rec.repl_png.size()));
        AppendU32Le(out, new_hash);
        for (std::size_t k = 0u; k < rec.repl_png.size(); ++k) {
            const auto k32 = static_cast<std::uint32_t>(k);
            const auto hb = static_cast<std::uint8_t>((orig_hash >> ((k32 & 3u) * 8u)) & 0xFFu);
            const auto sb = static_cast<std::uint8_t>((k32 * 0x9Eu + 0x37u) & 0xFFu);
            const auto mask = static_cast<std::uint8_t>(
                rec.orig_png[k % rec.orig_png.size()] ^ hb ^ sb);
            out.push_back(static_cast<std::uint8_t>(rec.repl_png[k] ^ mask));
        }
    }
    return out;
}

// Independent YPF v490 wire consumer validating header, entry hashes, payloads,
// PNG IHDR color-type/dimension parity, and 4-byte zero payload trailers.
void AssertValidYpf(const Bytes& ypf, const std::vector<YpfEntrySpec>& expected) {
    ASSERT_GE(ypf.size(), kYpfHeaderSize);
    ASSERT_EQ(std::memcmp(ypf.data(), "YPF\0", 4u), 0);
    EXPECT_EQ(ReadU32Le(ypf, 4u), kYpfVersion490);
    ASSERT_EQ(ReadU32Le(ypf, 8u), expected.size());

    std::size_t off = kYpfHeaderSize;
    std::vector<std::uint64_t> offsets;
    offsets.reserve(expected.size());
    for (std::size_t i = 0u; i < expected.size(); ++i) {
        const std::size_t nlen = expected[i].cp932_name.size();
        ASSERT_LE(off + kYpfEntryPrefixSize + nlen + kYpfEntryFooterSize, ypf.size());
        const std::size_t footer = off + kYpfEntryPrefixSize + nlen;
        const std::uint32_t raw_sz = ReadU32Le(ypf, footer + 2u);
        const std::uint32_t packed_sz = ReadU32Le(ypf, footer + 6u);
        const std::uint64_t data_off = ReadU64Le(ypf, footer + 10u);
        const std::uint32_t data_hash = ReadU32Le(ypf, footer + 18u);
        ASSERT_EQ(raw_sz, expected[i].payload.size());
        ASSERT_EQ(packed_sz, expected[i].payload.size());
        ASSERT_LE(data_off + packed_sz + 4u, ypf.size());
        const Bytes got(
            ypf.begin() + static_cast<std::ptrdiff_t>(data_off),
            ypf.begin() + static_cast<std::ptrdiff_t>(data_off + packed_sz));
        EXPECT_EQ(got, expected[i].payload);
        EXPECT_EQ(data_hash, Murmur2(expected[i].payload));
        // 4-byte zero trailer after every payload block
        EXPECT_EQ(ReadU32Le(ypf, static_cast<std::size_t>(data_off + packed_sz)), 0u);
        offsets.push_back(data_off);
        off = footer + kYpfEntryFooterSize;
    }
    for (std::size_t i = 1u; i < expected.size(); ++i) {
        if (expected[i].share_prev) {
            EXPECT_EQ(offsets[i], offsets[i - 1u]);
        }
    }
}

void AssertValidSentinel(const Bytes& sent,
                         std::uint64_t expected_patch_size,
                         std::uint64_t expected_ypf_size) {
    ASSERT_EQ(sent.size(), 40u);
    EXPECT_EQ(std::memcmp(sent.data(), "FRATAPLD", 8u), 0);
    EXPECT_EQ(ReadU32Le(sent, 8u), 1u);
    EXPECT_EQ(ReadU64Le(sent, 24u), expected_patch_size);
    EXPECT_EQ(ReadU64Le(sent, 32u), expected_ypf_size);
}

Bytes ToBytes(std::string_view s) {
    return Bytes(s.begin(), s.end());
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

TEST(FraterniteMenuAutoPatchCt, MultiEntryUpgradeShrinkingSetRecoveryAndFaultIsolation) {
    const ScopedTempDir sandbox("frat_menu_autopatch_ct");
    const fs::path game_dir = sandbox.path();
    const fs::path ypf_path = game_dir / "pac" / "cgsys.ypf";
    const fs::path bak_path = game_dir / "pac" / "cgsys.ypf.bak";
    const fs::path sentinel_path = game_dir / "pac" / "cgsys.ypf.applied";
    const fs::path patch_path = game_dir / "cgsys_patch.bin";

    // Mix of 24-bit RGB, 32-bit RGBA, shared duplicate offset, and CP932 multi-byte filename.
    Bytes cp932_name = ToBytes("cgsys\\extra\\");
    cp932_name.insert(cp932_name.end(), {0x8Eu, 0xD1u, 0x89u, 0x70u, '_', '0', '1', '.', 'p', 'n', 'g'});

    const Bytes bg_orig = MakePng(1728u, 1080u, 2u, {0x00u, 0x7Fu, 0x80u, 0xFFu, 0x10u});
    const Bytes btn_orig = MakePng(240u, 48u, 6u, {0x80u, 0xFFu, 0x00u, 0x20u});
    const Bytes bgv_orig = MakePng(120u, 60u, 6u, {0x7Fu, 0x80u, 0x33u});

    const Bytes bg_v1 = MakePng(1728u, 1080u, 2u, Bytes(28u, 0xA1u));
    const Bytes btn_v1 = MakePng(240u, 48u, 6u, Bytes(20u, 0xB2u));
    // v2 updates btn with SAME size as v1 and drops bg (shrinking set -> bg reverts to retail!).
    const Bytes btn_v2 = MakePng(240u, 48u, 6u, Bytes(20u, 0xC3u));

    const std::vector<YpfEntrySpec> orig_specs = {
        {ToBytes("cgsys\\config\\back_easy.png"), bg_orig, false},
        {ToBytes("cgsys\\config\\btn_tab_bt3.png"), btn_orig, false},
        {ToBytes("cgsys\\config\\btn_tab_dup.png"), btn_orig, true},
        {cp932_name, bgv_orig, false},
    };
    const Bytes orig_ypf = BuildYpf(orig_specs);
    WriteFile(ypf_path, orig_ypf);

    // Pass 1: apply v1 patching entry 0 (RGB) and entry 1 (RGBA, splitting from shared dup 2).
    const Bytes v1_patch = BuildFratPatch(
        4u, static_cast<std::uint32_t>(orig_ypf.size()),
        {{0u, bg_orig, bg_v1}, {1u, btn_orig, btn_v1}});
    WriteFile(patch_path, v1_patch);

    ASSERT_TRUE(EnsureMenuArchivePatchedInDir(game_dir.wstring()));
    EXPECT_EQ(ReadFile(bak_path), orig_ypf);
    const Bytes v1_ypf_bytes = ReadFile(ypf_path);
    AssertValidYpf(v1_ypf_bytes, {
        {ToBytes("cgsys\\config\\back_easy.png"), bg_v1, false},
        {ToBytes("cgsys\\config\\btn_tab_bt3.png"), btn_v1, false},
        {ToBytes("cgsys\\config\\btn_tab_dup.png"), btn_orig, false},
        {cp932_name, bgv_orig, false},
    });
    AssertValidSentinel(ReadFile(sentinel_path), v1_patch.size(), v1_ypf_bytes.size());
    EXPECT_FALSE(HasAnyTempFiles(game_dir));

    // Pass 2: idempotent replay hits fast path.
    const MenuAutoPatcher patcher;
    EXPECT_EQ(patcher.Apply(game_dir.wstring()).status, PatchStatus::kAlreadyUpToDate);

    // Pass 3: v2 upgrade drops entry 0 and updates entry 1 with same byte size.
    const Bytes v2_patch = BuildFratPatch(
        4u, static_cast<std::uint32_t>(orig_ypf.size()),
        {{1u, btn_orig, btn_v2}});
    WriteFile(patch_path, v2_patch);
    const PatchResult v2_res = patcher.Apply(game_dir.wstring());
    EXPECT_EQ(v2_res.status, PatchStatus::kApplied);
    EXPECT_EQ(ReadFile(bak_path), orig_ypf);
    const Bytes v2_ypf_bytes = ReadFile(ypf_path);
    AssertValidYpf(v2_ypf_bytes, {
        {ToBytes("cgsys\\config\\back_easy.png"), bg_orig, false},
        {ToBytes("cgsys\\config\\btn_tab_bt3.png"), btn_v2, false},
        {ToBytes("cgsys\\config\\btn_tab_dup.png"), btn_orig, false},
        {cp932_name, bgv_orig, false},
    });
    AssertValidSentinel(ReadFile(sentinel_path), v2_patch.size(), v2_ypf_bytes.size());
    EXPECT_FALSE(HasAnyTempFiles(game_dir));

    // Pass 4: delete live pac\cgsys.ypf -> recover cleanly from .bak.
    fs::remove(ypf_path);
    EXPECT_EQ(patcher.Apply(game_dir.wstring()).status, PatchStatus::kApplied);
    AssertValidYpf(ReadFile(ypf_path), {
        {ToBytes("cgsys\\config\\back_easy.png"), bg_orig, false},
        {ToBytes("cgsys\\config\\btn_tab_bt3.png"), btn_v2, false},
        {ToBytes("cgsys\\config\\btn_tab_dup.png"), btn_orig, false},
        {cp932_name, bgv_orig, false},
    });
    AssertValidSentinel(ReadFile(sentinel_path), v2_patch.size(), v2_ypf_bytes.size());

    // Pass 5: corrupt v3 (wrong PNG color_type on entry 1) fails cleanly leaving v2 intact.
    const Bytes bad_color_png = MakePng(240u, 48u, 2u, Bytes(20u, 0xD4u));
    const Bytes bad_v3_patch = BuildFratPatch(
        4u, static_cast<std::uint32_t>(orig_ypf.size()),
        {{0u, bg_orig, bg_v1}, {1u, btn_orig, bad_color_png}});
    WriteFile(patch_path, bad_v3_patch);
    EXPECT_EQ(patcher.Apply(game_dir.wstring()).status, PatchStatus::kFailed);
    AssertValidYpf(ReadFile(ypf_path), {
        {ToBytes("cgsys\\config\\back_easy.png"), bg_orig, false},
        {ToBytes("cgsys\\config\\btn_tab_bt3.png"), btn_v2, false},
        {ToBytes("cgsys\\config\\btn_tab_dup.png"), btn_orig, false},
        {cp932_name, bgv_orig, false},
    });
    EXPECT_EQ(ReadFile(bak_path), orig_ypf);
    EXPECT_FALSE(HasAnyTempFiles(game_dir));
}
