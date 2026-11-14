// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests for the committed `amakano2pe/menu_patch.bin` ("AMK2MENU" v1)
// bundle and cross-component Producer (`GameAssetSource` +
// `CreateUiTextPayloadsRenderer` + `MenuPatchBuilder`) -> Consumer
// (`menu_patch::ParseMenuPatchContainer` + `menu_autopatch::EnsureMenuAssetsPatched`)
// pipeline.  Runs on CI without retail game archives.
#include "common/util.h"
#include "cs2/cs2_archive.h"
#include "hg3/hg3_codec.h"
#include "imaging/bgra_image.h"
#include "menu/core/fes_script_codec.h"
#include "menu/core/menu_patch_container.h"
#include "menu/domain_renderer.h"
#include "menu/ui_text_builders.h"
#include "menu_autopatch.h"
#include "menu_patch_format.h"

#ifndef OPENSSL_SUPPRESS_DEPRECATED
#  define OPENSSL_SUPPRESS_DEPRECATED
#endif
#include <gtest/gtest.h>
#include <openssl/blowfish.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kExpectedTotalRecords = 154;
constexpr std::size_t kExpectedHg3Records = 146;
constexpr std::size_t kExpectedNonGraphicRecords = 8;
constexpr std::size_t kExpectedStandaloneBackupRecords = 3;
constexpr std::size_t kExpectedSandboxRecordCount = 9;
constexpr std::size_t kSyntheticExeSizeBytes = 4096;
constexpr std::uint32_t kSyntheticFesReserved = 0x15U;
constexpr std::uint32_t kSyntheticFrameWidth = 16;
constexpr std::uint32_t kSyntheticFrameHeight = 16;
constexpr std::uint32_t kSyntheticFrameBpp = 32;
constexpr std::uint32_t kSyntheticImgMode = 1;
constexpr std::uint32_t kSyntheticKifSeed = 12345U;
constexpr int kShift8 = 8;
constexpr int kShift16 = 16;
constexpr int kShift24 = 24;
constexpr std::size_t kBlowfishKeyBytes = 4;
constexpr std::size_t kBlowfishHalfBlockBytes = 4;
constexpr std::size_t kBlowfishBlockBytes = 8;
constexpr std::size_t kBlowfishAlignMask = 7;
constexpr std::size_t kKifHeaderBytes = 8;
constexpr std::size_t kKifNameBytes = 64;
constexpr std::size_t kKifEntryBytes = 72;
constexpr std::string_view kSyntheticVCode2 = "NXT-76BPQ5YD";

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string ToLowerAscii(std::string_view input) {
    std::string out(input.size(), '\0');
    for (std::size_t i = 0; i < input.size(); ++i) {
        out[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(input[i])));
    }
    return out;
}

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto a = static_cast<unsigned char>(text[offset + i]);
        const auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto end_pos = in.tellg();
    if (end_pos <= 0) {
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end_pos));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void WriteBinaryFile(const fs::path& path,
                     const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "Failed to open for write: " << path.u8string();
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(out.good()) << "Failed writing: " << path.u8string();
}

std::vector<std::uint8_t> StringToBytes(std::string_view text) {
    const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
    return std::vector<std::uint8_t>(ptr, ptr + text.size());
}

std::string BytesToString(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::uint8_t> MakeDeterministicBytes(std::size_t size,
                                                 std::uint8_t seed) {
    std::vector<std::uint8_t> buf(size);
    for (std::size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<std::uint8_t>((i * 43U + seed) & 0xFFU);
    }
    return buf;
}

void PutLe32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kShift8));
    b.push_back(static_cast<std::uint8_t>(v >> kShift16));
    b.push_back(static_cast<std::uint8_t>(v >> kShift24));
}

void PutBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> kShift24);
    p[1] = static_cast<std::uint8_t>(v >> kShift16);
    p[2] = static_cast<std::uint8_t>(v >> kShift8);
    p[3] = static_cast<std::uint8_t>(v);
}

std::uint32_t GetBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << kShift24) |
           (static_cast<std::uint32_t>(p[1]) << kShift16) |
           (static_cast<std::uint32_t>(p[2]) << kShift8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& b, std::size_t i) {
    return static_cast<std::uint32_t>(b[i]) |
           (static_cast<std::uint32_t>(b[i + 1]) << kShift8) |
           (static_cast<std::uint32_t>(b[i + 2]) << kShift16) |
           (static_cast<std::uint32_t>(b[i + 3]) << kShift24);
}

void WriteLe32At(std::vector<std::uint8_t>& b, std::size_t i, std::uint32_t v) {
    b[i] = static_cast<std::uint8_t>(v);
    b[i + 1] = static_cast<std::uint8_t>(v >> kShift8);
    b[i + 2] = static_cast<std::uint8_t>(v >> kShift16);
    b[i + 3] = static_cast<std::uint8_t>(v >> kShift24);
}

// Builds a KIF v3 (.int) archive encrypted with `cipher` holding `entries`.
std::vector<std::uint8_t> BuildSyntheticKifArchive(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries,
    const ama::cs2::KifNameCipher& cipher, std::uint32_t seed) {
    BF_KEY key{};
    const std::uint32_t key_int = ama::cs2::mt_genrand(seed);
    std::uint8_t key_le[kBlowfishKeyBytes] = {
        static_cast<std::uint8_t>(key_int),
        static_cast<std::uint8_t>(key_int >> kShift8),
        static_cast<std::uint8_t>(key_int >> kShift16),
        static_cast<std::uint8_t>(key_int >> kShift24),
    };
    BF_set_key(&key, static_cast<int>(kBlowfishKeyBytes), key_le);

    const std::uint32_t total_count =
        static_cast<std::uint32_t>(entries.size() + 1U);
    std::uint32_t next_data_off =
        static_cast<std::uint32_t>(kKifHeaderBytes + total_count * kKifEntryBytes);

    std::vector<std::uint8_t> out = {'K', 'I', 'F', 0};
    PutLe32(out, total_count);

    // Entry 0: "__key__.dat"
    std::vector<std::uint8_t> key_name(kKifNameBytes, 0);
    constexpr char kKeyDat[] = "__key__.dat";
    std::memcpy(key_name.data(), kKeyDat, sizeof(kKeyDat) - 1);
    out.insert(out.end(), key_name.begin(), key_name.end());
    PutLe32(out, 0);
    PutLe32(out, seed);

    std::vector<std::uint8_t> all_data;
    for (std::size_t idx = 0; idx < entries.size(); ++idx) {
        const std::uint32_t entry_index = static_cast<std::uint32_t>(idx + 1U);
        const std::string obf_name = cipher.encode(entries[idx].first, entry_index);
        std::vector<std::uint8_t> stored = entries[idx].second;
        const std::size_t aligned = stored.size() & ~kBlowfishAlignMask;
        for (std::size_t i = 0; i + kBlowfishBlockBytes <= aligned;
             i += kBlowfishBlockBytes) {
            const std::uint32_t l = ReadLe32(stored, i);
            const std::uint32_t r = ReadLe32(stored, i + kBlowfishHalfBlockBytes);
            std::uint8_t block[kBlowfishBlockBytes];
            std::uint8_t enc_block[kBlowfishBlockBytes];
            PutBe32(block, l);
            PutBe32(block + kBlowfishHalfBlockBytes, r);
            BF_ecb_encrypt(block, enc_block, &key, BF_ENCRYPT);
            WriteLe32At(stored, i, GetBe32(enc_block));
            WriteLe32At(stored, i + kBlowfishHalfBlockBytes,
                        GetBe32(enc_block + kBlowfishHalfBlockBytes));
        }

        std::uint8_t plain_meta[kBlowfishBlockBytes];
        std::uint8_t enc_meta[kBlowfishBlockBytes];
        PutBe32(plain_meta, next_data_off);
        PutBe32(plain_meta + kBlowfishHalfBlockBytes,
                static_cast<std::uint32_t>(stored.size()));
        BF_ecb_encrypt(plain_meta, enc_meta, &key, BF_ENCRYPT);
        const std::uint32_t stored_off = GetBe32(enc_meta) - entry_index;
        const std::uint32_t stored_len =
            GetBe32(enc_meta + kBlowfishHalfBlockBytes);

        std::vector<std::uint8_t> name_buf(kKifNameBytes, 0);
        std::memcpy(name_buf.data(), obf_name.data(),
                    std::min(obf_name.size(), kKifNameBytes - 1));
        out.insert(out.end(), name_buf.begin(), name_buf.end());
        PutLe32(out, stored_off);
        PutLe32(out, stored_len);

        all_data.insert(all_data.end(), stored.begin(), stored.end());
        next_data_off += static_cast<std::uint32_t>(stored.size());
    }

    out.insert(out.end(), all_data.begin(), all_data.end());
    return out;
}

std::string BuildSyntheticStartupXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
           "<document>\r\n"
           "  <title>アマカノ2～Perfect Edition～</title>\r\n"
           "  <brand>あざらしそふと</brand>\r\n"
           "  <save>%USERPROFILE%\\%brand%\\%title%</save>\r\n"
           "  <imglib>%USERPROFILE%\\%brand%\\アマカノ2\\imglib</imglib>\r\n"
           "  <KEYCUSTOMIZE>\r\n"
           "    <name>決定</name><name>キャンセル</name><name>文章を進める</name>\r\n"
           "    <name>メッセージウィンドウ消去</name><name>カーソル↑移動</name>\r\n"
           "    <name>カーソル↓移動</name><name>カーソル←移動</name>\r\n"
           "    <name>カーソル→移動</name><name>スクロール↑</name>\r\n"
           "    <name>スクロール↓</name><name>１ページ↑</name><name>１ページ↓</name>\r\n"
           "    <name>強制スキップ</name><name>既読スキップ</name><name>オートモード</name>\r\n"
           "    <name>履歴ウィンドウ</name><name>次の選択肢までスキップ</name>\r\n"
           "    <name>前の選択肢に戻る</name><name>音声再生</name><name>セーブ画面</name>\r\n"
           "    <name>ロード画面</name><name>クイックセーブ</name><name>クイックロード</name>\r\n"
           "    <name>環境設定</name><name>オートスピードアップ</name>\r\n"
           "    <name>オートスピードダウン</name><name>スクリーンモード変更</name>\r\n"
           "    <name>ウィンドウ最小化</name><name>ゲーム終了</name>\r\n"
           "    <name>おあずけモード切り替え</name>\r\n"
           "  </KEYCUSTOMIZE>\r\n"
           "</document>\r\n";
}

std::string BuildSyntheticSoundconfXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
           "<soundconf>\r\n"
           "  <comment>ＢＧＭ</comment><comment>効果音</comment>\r\n"
           "  <comment>キャラクタボイス</comment><comment>スピーチ</comment>\r\n"
           "  <comment>システムボイス</comment><comment>システム効果音</comment>\r\n"
           "  <name>効果音</name><name>音声</name>\r\n"
           "  <name>システムボイス</name><name>システム効果音</name>\r\n"
           "  <voice>\r\n"
           "    <name>結灯</name><name>玲</name><name>ちとせ</name>\r\n"
           "    <name>紗雪</name><name>聖</name><name>こはる</name>\r\n"
           "    <name>穂波</name><name>奏</name><name>雪静</name><name>涙香</name>\r\n"
           "    <name>母さん</name><name>副部長</name><name>女将さん</name>\r\n"
           "    <name>寶泉路</name><name>淵上</name><name>マスター</name>\r\n"
           "    <name>放送</name><name>玲の父</name><name>結灯の父</name>\r\n"
           "    <name>迷子の女の子</name><name>プリント機</name>\r\n"
           "  </voice>\r\n"
           "  <sysvoice>\r\n"
           "    <name>結灯</name><name>玲</name><name>ちとせ</name>\r\n"
           "  </sysvoice>\r\n"
           "</soundconf>\r\n";
}

std::string BuildSyntheticConfigBasicFes() {
    std::string script;
    for (int i = 0; i < 6; ++i) {
        script += "str[0] str メッセージ速度のサンプルです。\r\n";
        script += "str[1] str オートモードの速度のサンプルです。\r\n";
    }
    script += "font_sample str \\pcテキストサンプル\r\n";
    return script;
}

std::string BuildSyntheticInputTxtFes() {
    return "strvar 10 和倉\r\n"
           "strvar 20 賢一\r\n"
           "strcmp g_TEMP_0 $str10 和倉\r\n"
           "strcmp g_TEMP_0 $str20 賢一\r\n"
           "strvar 11 苗字を入力\r\n"
           "strvar 11 名前を入力\r\n"
           "messagebox g_TEMP_0 2 確認 $str10 $str20 さんでお間違いないですか？\r\n";
}

std::string BuildSyntheticAdvXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
           "<adv>\r\n"
           "  <selectcomment>[選択肢]</selectcomment>\r\n"
           "</adv>\r\n";
}

}  // namespace

TEST(MenuPatcherCt, CommittedMenuPatchBinSatisfiesWireFormatCoverageAndZeroRawSignatures) {
    const fs::path patch_path = fs::u8path(AMA_PROJECT_DIR) / "menu_patch.bin";
    ASSERT_TRUE(fs::exists(patch_path)) << patch_path.u8string();

    const std::vector<std::uint8_t> raw_bundle = ReadBinaryFile(patch_path);
    ASSERT_GE(raw_bundle.size(), ama::menu::kMenuPatchHeaderSize);
    EXPECT_TRUE(ama::menu::VerifyNoRawAssetSignatures(raw_bundle));

    const ama::menu::MenuPatchContainerData producer_manifest =
        ama::menu::ParseMenuPatchContainer(raw_bundle);
    EXPECT_EQ(producer_manifest.format_version, ama::menu::kMenuPatchFormatVersion);
    EXPECT_EQ(producer_manifest.manifest_hash,
              ama::menu::ComputeManifestHash(producer_manifest.records));
    EXPECT_EQ(producer_manifest.records.size(), kExpectedTotalRecords);

    // Cross-verify with proxy_dll's runtime `menu_patch::ParseMenuPatchContainer`.
    menu_patch::MenuPatchManifest consumer_manifest;
    ASSERT_EQ(menu_patch::ParseMenuPatchContainer(
                  menu_patch::ByteView{raw_bundle.data(), raw_bundle.size()},
                  consumer_manifest),
              menu_patch::ParseStatus::kOk);
    EXPECT_EQ(consumer_manifest.header.format_version, menu_patch::kFormatVersion);
    EXPECT_EQ(consumer_manifest.header.record_count, kExpectedTotalRecords);
    EXPECT_EQ(consumer_manifest.header.manifest_hash,
              producer_manifest.manifest_hash);
    ASSERT_EQ(consumer_manifest.records.size(), producer_manifest.records.size());

    std::set<std::string> unique_targets;
    std::size_t hg3_records = 0;
    std::size_t non_graphic_records = 0;
    std::size_t standalone_records = 0;

    for (std::size_t i = 0; i < producer_manifest.records.size(); ++i) {
        const auto& rec = producer_manifest.records[i];
        const auto& consumer_rec = consumer_manifest.records[i];
        EXPECT_EQ(consumer_rec.source_rel_path, rec.source_rel_path);
        EXPECT_EQ(consumer_rec.target_rel_path, rec.target_rel_path);
        EXPECT_EQ(consumer_rec.flags, rec.flags);
        EXPECT_EQ(consumer_rec.orig_offset, rec.orig_offset);
        EXPECT_EQ(consumer_rec.orig_size, rec.orig_size);
        EXPECT_EQ(consumer_rec.orig_slice_hash, rec.orig_slice_hash);
        EXPECT_EQ(consumer_rec.new_raw_size, rec.new_raw_size);
        EXPECT_EQ(consumer_rec.new_raw_hash, rec.new_raw_hash);

        EXPECT_GE(rec.orig_size, ama::menu::kMinOrigSliceBytes);
        EXPECT_GT(rec.new_raw_size, 0U);
        EXPECT_EQ(rec.xor_payload.size(), rec.new_raw_size);
        EXPECT_NE(rec.orig_slice_hash, 0U);
        EXPECT_NE(rec.new_raw_hash, 0U);

        const std::string lower_target = ToLowerAscii(rec.target_rel_path);
        EXPECT_TRUE(unique_targets.insert(lower_target).second)
            << "Duplicate target_rel_path: " << rec.target_rel_path;

        if (rec.flags == ama::menu::kFlagBackupStandalone) {
            ++standalone_records;
            EXPECT_EQ(ToLowerAscii(rec.source_rel_path), lower_target);
        } else {
            EXPECT_EQ(rec.flags, ama::menu::kFlagLooseOverride);
            EXPECT_NE(ToLowerAscii(rec.source_rel_path), lower_target);
        }

        if (EndsWithIgnoreCase(rec.target_rel_path, ".hg3")) {
            ++hg3_records;
        } else {
            ++non_graphic_records;
        }
    }

    EXPECT_EQ(hg3_records, kExpectedHg3Records);
    EXPECT_EQ(non_graphic_records, kExpectedNonGraphicRecords);
    EXPECT_EQ(standalone_records, kExpectedStandaloneBackupRecords);

    // Representative targets across all 6 sprite domains and all 8 UI text/script payloads.
    const char* const kRequiredTargets[] = {
        "boot.hg3",
        "image\\sys_config.hg3",
        "image\\sys_config_basic.hg3",
        "image\\sys_config_sound.hg3",
        "image\\sys_config_etc.hg3",
        "image\\sys_log.hg3",
        "image\\sys_info.hg3",
        "image\\sl_base.hg3",
        "image\\sys_title.hg3",
        "image\\sys_title_naked.hg3",
        "image\\sys_warning_bg.hg3",
        "image\\sys_nameinput.hg3",
        "image\\conf.hg3",
        "image\\sys_mapselect.hg3",
        "image\\sys_school_map.hg3",
        "image\\mapkokuhaku.hg3",
        "image\\sys_graph_friend.hg3",
        "image\\sys_graph_lover.hg3",
        "image\\ex_cgmode.hg3",
        "image\\ex_charmode.hg3",
        "image\\ex_charmode_select.hg3",
        "image\\ex_music.hg3",
        "image\\ex_music_thumbnail.hg3",
        "image\\ex_replaymode.hg3",
        "image\\item_chat_01a.hg3",
        "image\\item_note_01p.hg3",
        "image\\item_tegami_03la.hg3",
        "language_.txt",
        "export\\lang.zt",
        "language.txt",
        "config\\startup.xml",
        "config\\soundconf.xml",
        "config\\adv.xml",
        "fes\\config_basic.fes",
        "fes\\inputtxt.fes",
    };
    for (const char* req : kRequiredTargets) {
        EXPECT_GT(unique_targets.count(req), 0U) << "Missing target: " << req;
    }
}

TEST(MenuPatcherCt,
     ProducerDomainRendererAndMenuPatchBuilderCrossVerifyThroughConsumerMenuAutoPatchWithIdempotenceAndTamperRollback) {
    const ScopedTempDir sandbox("ama_ct_producer_consumer_sandbox");
    const ama::cs2::KifNameCipher cipher{std::string(kSyntheticVCode2)};

    // Seed synthetic retail directory with cs2.exe, boot.hg3, standalone XMLs,
    // fes.int, and config.int.
    const std::vector<std::uint8_t> exe_bytes =
        MakeDeterministicBytes(kSyntheticExeSizeBytes, 0x3AU);
    WriteBinaryFile(sandbox.path() / "cs2.exe", exe_bytes);

    ama::hg3::Hg3Container orig_boot_hg3;
    ama::hg3::Hg3Frame frame;
    frame.frame_id = 0;
    frame.stdinfo.width = kSyntheticFrameWidth;
    frame.stdinfo.height = kSyntheticFrameHeight;
    frame.stdinfo.bpp = kSyntheticFrameBpp;
    frame.stdinfo.total_width = kSyntheticFrameWidth;
    frame.stdinfo.total_height = kSyntheticFrameHeight;
    frame.stdinfo.transparent_flag = 1;
    frame.imgmode = kSyntheticImgMode;
    frame.top_down_img0000 = true;
    frame.pixels = ama::imaging::MakeFilledBgraImage(
        kSyntheticFrameWidth, kSyntheticFrameHeight,
        ama::imaging::BgraColor{30, 60, 90, 255});
    orig_boot_hg3.frames.push_back(frame);
    const std::vector<std::uint8_t> orig_boot_bytes =
        ama::hg3::Hg3Codec::Encode(orig_boot_hg3);
    WriteBinaryFile(sandbox.path() / "boot.hg3", orig_boot_bytes);

    const std::vector<std::uint8_t> orig_startup_bytes =
        StringToBytes(BuildSyntheticStartupXml());
    const std::vector<std::uint8_t> orig_soundconf_bytes =
        StringToBytes(BuildSyntheticSoundconfXml());
    WriteBinaryFile(sandbox.path() / "config" / "startup.xml", orig_startup_bytes);
    WriteBinaryFile(sandbox.path() / "config" / "soundconf.xml",
                    orig_soundconf_bytes);

    const std::vector<std::uint8_t> packed_config_basic =
        ama::menu::FesScriptCodec::PackFesText(BuildSyntheticConfigBasicFes(),
                                               kSyntheticFesReserved);
    const std::vector<std::uint8_t> packed_inputtxt =
        ama::menu::FesScriptCodec::PackFesText(BuildSyntheticInputTxtFes(),
                                               kSyntheticFesReserved);
    const std::vector<std::uint8_t> fes_int_bytes = BuildSyntheticKifArchive(
        {
            {"config_basic.fes", packed_config_basic},
            {"inputtxt.fes", packed_inputtxt},
        },
        cipher, kSyntheticKifSeed);
    WriteBinaryFile(sandbox.path() / "fes.int", fes_int_bytes);

    const std::vector<std::uint8_t> config_int_bytes = BuildSyntheticKifArchive(
        {
            {"adv.xml", StringToBytes(BuildSyntheticAdvXml())},
        },
        cipher, kSyntheticKifSeed + 1U);
    WriteBinaryFile(sandbox.path() / "config.int", config_int_bytes);

    // Producer (`pipeline_cpp`): GameAssetSource + CreateUiTextPayloadsRenderer +
    // BuildHg3PatchEntry -> MenuPatchBuilder -> menu_patch.bin.
    const ama::menu::GameAssetSource asset_source(sandbox.path(),
                                                  std::string(kSyntheticVCode2));
    ama::menu::ExtractedAssetEntry boot_source_entry;
    ama::hg3::Hg3Container patched_boot_hg3 =
        asset_source.LoadHg3("boot.hg3", &boot_source_entry);
    ASSERT_FALSE(patched_boot_hg3.frames.empty());
    patched_boot_hg3.frames[0].pixels = ama::imaging::MakeFilledBgraImage(
        kSyntheticFrameWidth, kSyntheticFrameHeight,
        ama::imaging::BgraColor{180, 200, 220, 255});

    std::unique_ptr<ama::menu::IMenuDomainRenderer> ui_renderer =
        ama::menu::CreateUiTextPayloadsRenderer();
    std::vector<ama::menu::MenuPatchEntrySpec> specs =
        ui_renderer->RenderDomain(asset_source, ama::menu::DomainRenderOptions{});
    specs.push_back(ama::menu::GameAssetSource::BuildHg3PatchEntry(
        boot_source_entry, patched_boot_hg3));
    ASSERT_EQ(specs.size(), kExpectedSandboxRecordCount);

    ama::menu::MenuPatchBuilder builder;
    for (const auto& spec : specs) {
        builder.AddRecord(spec);
    }
    const std::vector<std::uint8_t> bundle_bytes = builder.BuildBytes();
    ASSERT_TRUE(ama::menu::VerifyNoRawAssetSignatures(bundle_bytes));
    builder.WriteToFile(sandbox.path() / "menu_patch.bin");

    // Consumer (`proxy_dll`): menu_autopatch::EnsureMenuAssetsPatched on Run 1.
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.path().wstring()),
              menu_autopatch::PatchOutcome::kApplied);

    // Verify standalone backups (.bak) and translated outputs.
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "boot.hg3.bak"), orig_boot_bytes);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "config" / "startup.xml.bak"),
              orig_startup_bytes);
    EXPECT_EQ(ReadBinaryFile(sandbox.path() / "config" / "soundconf.xml.bak"),
              orig_soundconf_bytes);

    const ama::hg3::Hg3Container extracted_boot =
        ama::hg3::Hg3Codec::Decode(ReadBinaryFile(sandbox.path() / "boot.hg3"));
    EXPECT_TRUE(ama::hg3::Hg3Codec::VerifyFrameMetadataParity(orig_boot_hg3,
                                                              extracted_boot));
    EXPECT_EQ(extracted_boot.frames[0].pixels.pixels,
              patched_boot_hg3.frames[0].pixels.pixels);

    const std::string patched_startup =
        BytesToString(ReadBinaryFile(sandbox.path() / "config" / "startup.xml"));
    EXPECT_NE(patched_startup.find("<title>アマカノ2～Perfect Edition～</title>"),
              std::string::npos);
    EXPECT_NE(patched_startup.find("<OPTTABLE>lang.zt</OPTTABLE>"),
              std::string::npos);
    EXPECT_NE(patched_startup.find("<name>Confirm</name>"), std::string::npos);

    const ama::menu::FesPayload extracted_inputtxt =
        ama::menu::FesScriptCodec::UnpackFes(
            ReadBinaryFile(sandbox.path() / "fes" / "inputtxt.fes"));
    EXPECT_NE(extracted_inputtxt.AsText().find("strcmp g_TEMP_0 $str10 和倉"),
              std::string::npos);
    EXPECT_NE(extracted_inputtxt.AsText().find("strcmp g_TEMP_0 $str20 賢一"),
              std::string::npos);
    EXPECT_NE(extracted_inputtxt.AsText().find("Enter Last Name"),
              std::string::npos);

    const std::string patched_adv =
        BytesToString(ReadBinaryFile(sandbox.path() / "config" / "adv.xml"));
    EXPECT_NE(patched_adv.find("<selectcomment>[Choice]</selectcomment>"),
              std::string::npos);

    // Verify binary "AMK2APLD" sentinel via consumer ParseAppliedSentinel.
    const std::vector<std::uint8_t> sentinel_bytes =
        ReadBinaryFile(sandbox.path() / "menu_patch.applied");
    menu_patch::AppliedSentinel sentinel;
    ASSERT_TRUE(menu_patch::ParseAppliedSentinel(
        menu_patch::ByteView{sentinel_bytes.data(), sentinel_bytes.size()},
        sentinel));
    EXPECT_EQ(sentinel.patch_file_size, bundle_bytes.size());
    EXPECT_EQ(sentinel.targets.size(), kExpectedSandboxRecordCount);

    // Run 2 fast-paths via sentinel without re-extracting.
    EXPECT_EQ(menu_autopatch::EnsureMenuAssetsPatched(sandbox.path().wstring()),
              menu_autopatch::PatchOutcome::kAlreadyApplied);

    // Tamper / rollback verification: corrupting cs2.exe slice in a fresh sandbox
    // causes EnsureMenuAssetsPatched to reject the mismatched records and withhold
    // menu_patch.applied.
    const ScopedTempDir tamper_sandbox("ama_ct_tamper_sandbox");
    std::vector<std::uint8_t> tampered_exe = exe_bytes;
    tampered_exe[0] ^= 0xFFU;
    WriteBinaryFile(tamper_sandbox.path() / "cs2.exe", tampered_exe);
    WriteBinaryFile(tamper_sandbox.path() / "boot.hg3", orig_boot_bytes);
    WriteBinaryFile(tamper_sandbox.path() / "config" / "startup.xml",
                    orig_startup_bytes);
    WriteBinaryFile(tamper_sandbox.path() / "config" / "soundconf.xml",
                    orig_soundconf_bytes);
    WriteBinaryFile(tamper_sandbox.path() / "fes.int", fes_int_bytes);
    WriteBinaryFile(tamper_sandbox.path() / "config.int", config_int_bytes);
    WriteBinaryFile(tamper_sandbox.path() / "menu_patch.bin", bundle_bytes);

    EXPECT_EQ(
        menu_autopatch::EnsureMenuAssetsPatched(tamper_sandbox.path().wstring()),
        menu_autopatch::PatchOutcome::kPartiallyApplied);
    EXPECT_FALSE(fs::exists(tamper_sandbox.path() / "menu_patch.applied"));
    EXPECT_FALSE(fs::exists(tamper_sandbox.path() / "language_.txt"));
    EXPECT_FALSE(fs::exists(tamper_sandbox.path() / "export" / "lang.zt"));
}
