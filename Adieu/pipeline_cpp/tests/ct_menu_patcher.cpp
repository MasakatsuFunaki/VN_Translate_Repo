// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box component tests for Adieu's menu patch pipeline (`MenuPatchPipeline`),
// domain renderers, contact-sheet QA generator, and committed `Adieu/menu_patch.bin`
// container invariants. Runs offline (`0` skips when `VN_GAME_DIR` is unset).

#include "g00/g00_codec.h"
#include "g00/gameexe_codec.h"
#include "imaging/bgra_image.h"
#include "imaging/contact_sheet.h"
#include "menu/core/menu_patch_container.h"
#include "menu/core/sprite_canvas.h"
#include "menu/domain_renderer.h"
#include "menu/gameexe_patch_rules.h"
#include "menu/render_cards_dialogs.h"
#include "menu/render_mess_overlays.h"
#include "menu/render_supplementary.h"
#include "menu/render_system_menus.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || defined(__wincodec_h__)
#error "Windows header leaked through public menu/g00/imaging headers"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace adi::menu {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMinExpectedCommittedRecords = 10u;
constexpr int kSyntheticScreenW = 640;
constexpr int kSyntheticScreenH = 480;
constexpr std::size_t kSyntheticRecollectCuts = 20u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        const std::string name =
            "adi_ct_menu_patcher_さよなら_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("failed to open file: " + p.u8string());
    }
    const std::streamoff sz = in.tellg();
    if (sz < 0) {
        throw std::runtime_error("failed to stat file: " + p.u8string());
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz), 0u);
    in.seekg(0, std::ios::beg);
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()), sz);
        if (!in || in.gcount() != sz) {
            throw std::runtime_error("short read on file: " + p.u8string());
        }
    }
    return out;
}

void WriteBinaryFile(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out)) << "failed opening " << p.u8string();
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(static_cast<bool>(out)) << "failed writing " << p.u8string();
}

g00::GameexeKey256 BuildSyntheticKey256() {
    g00::GameexeKey256 key{};
    std::copy(g00::kExpectedAdieuGameexeKeyPrefix.begin(),
              g00::kExpectedAdieuGameexeKeyPrefix.end(), key.begin());
    for (std::size_t i = g00::kGameexeKeyPrefixBytes; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>((i * 29u + 7u) & 0xFFu);
    }
    return key;
}

void WriteSyntheticSiglusExeWithKey(const fs::path& exe_path,
                                    const g00::GameexeKey256& key) {
    const std::size_t total_size =
        static_cast<std::size_t>(g00::kAdieuGameexeKeyFileOffset) + key.size() + 64u;
    std::vector<std::uint8_t> exe_bytes(total_size, 0xCCu);
    exe_bytes[0] = 'M';
    exe_bytes[1] = 'Z';
    std::copy(key.begin(), key.end(),
              exe_bytes.begin() +
                  static_cast<std::ptrdiff_t>(g00::kAdieuGameexeKeyFileOffset));
    WriteBinaryFile(exe_path, exe_bytes);
}

std::string BuildSyntheticJapaneseGameexeText() {
    return
        "#DISCMARK = \"sayonara.env\"\r\n"
        "#GAMENAME = \"さよならを教えて\"\r\n"
        "#DUMMY_CHECK_STR = \"（ゲーム名）のディスクを入れてください。\"\r\n"
        "#DUMMY_CHECK_OK_STR = \"お買い上げありがとうございます。\"\r\n"
        "#SYSCOMMENU.READ_SKIP = 1, 1, \"既読文章を早送り\"\r\n"
        "#SYSCOMMENU.UNREAD_SKIP = 1, 1, \"文章を早送り\"\r\n"
        "#SYSCOMMENU.AUTO_SKIP = 1, 1, \"自動早送り\"\r\n"
        "#SYSCOMMENU.AUTO_MODE = 1, 1, \"オートモード\"\r\n"
        "#SYSCOMMENU.HIDE_MWND = 1, 1, \"ウィンドウを消す\"\r\n"
        "#SYSCOMMENU.SAVE = 1, 1, \"セーブ\"\r\n"
        "#SYSCOMMENU.LOAD = 1, 1, \"ロード\"\r\n"
        "#SYSCOMMENU.RETURN_SEL = 1, 1, \"前の選択肢に戻る\"\r\n"
        "#SYSCOMMENU.CONFIG = 1, 1, \"環境設定\"\r\n"
        "#SYSCOMMENU.RETURN_MENU = 1, 1, \"タイトルに戻る\"\r\n"
        "#SYSCOMMENU.GAME_END = 1, 1, \"ゲームを終了する\"\r\n"
        "#SYSCOMMENU.CANCEL = 1, 1, \"キャンセル\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_SWITCH.000 = 0, 0, 0, \"ル汎用スイッチ０番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_SWITCH.001 = 0, 0, 0, \"ル汎用スイッチ１番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_SWITCH.002 = 0, 0, 0, \"ル汎用スイッチ２番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_SWITCH.003 = 0, 0, 0, \"ル汎用スイッチ３番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.000 = 0, 0, 0, \"ローカル汎用モード０番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.000.ITEM.000.STR = \"モード０\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.000.ITEM.001.STR = \"モード１\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.000.ITEM.002.STR = \"モード２\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.001 = 0, 0, 0, \"ローカル汎用モード１番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.001.ITEM.000.STR = \"モード０\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.001.ITEM.001.STR = \"モード１\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.001.ITEM.002.STR = \"モード２\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.002 = 0, 0, 0, \"ローカル汎用モード２番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.002.ITEM.000.STR = \"モード０\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.002.ITEM.001.STR = \"モード１\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.002.ITEM.002.STR = \"モード２\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.003 = 0, 0, 0, \"ローカル汎用モード３番\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.003.ITEM.000.STR = \"モード０\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.003.ITEM.001.STR = \"モード１\"\r\n"
        "#SYSCOMMENU.LOCAL_EXTRA_MODE.003.ITEM.002.STR = \"モード２\"\r\n"
        "#DIALOG.MESSAGE_CHRCOLOR.STR = \"文章を色分けする。\"\r\n"
        "#DIALOG.SLEEP.STR = \"動作を遅くする。\"\r\n"
        "#DIALOG.NO_WIPE_ANIME.STR = \"暗転アニメを無効にする。\"\r\n"
        "#DIALOG.SKIP_WIPE_ANIME.STR = \"暗転を飛ばす。\"\r\n"
        "#DIALOG.NO_MWND_ANIME.STR = \"開閉アニメを無効にする。\"\r\n"
        "#DIALOG.WHEEL_NEXT_MESSAGE.STR = \"ホイールで読み進める。\"\r\n"
        "#DIALOG.KOE_DONT_STOP.STR = \"声の再生を続ける。\"\r\n"
        "#DIALOG.SKIP_UNREAD_MESSAGE.STR = \"未読の文章も早送り。\"\r\n"
        "#DIALOG.PLAY_SILENT_SOUND.STR = \"雑音対策。\"\r\n"
        "#WARNINGINFO.RESTART_WARNING_STR = \"最初から始めてもよろしいですか？\"\r\n"
        "#WARNINGINFO.RETURNMENU_WARNING_STR = \"タイトルに戻ってもよろしいですか？\"\r\n"
        "#WARNINGINFO.RETURNSEL_WARNING_STR = \"前の選択肢に戻ってもよろしいですか？\"\r\n"
        "#WARNINGINFO.GAMEEND_WARNING_STR = \"終了してもよろしいですか？\"\r\n"
        "#SAVE.NO_DATA_STR = \"データがありません。\"\r\n"
        "#SAVEINFO.DLGWND_CAPTION_TITLE_STR = \"セーブ\"\r\n"
        "#SAVEINFO.DLGWND_DESIDE_BUTTON_STR = \"セーブする\"\r\n"
        "#SAVEINFO.DLGWND_DATALIST_NAMEHEADER_STR = \"セーブ\"\r\n"
        "#SAVEINFO.DLGWND_WARNING_CHKBOX_STR = \"上書き確認。\"\r\n"
        "#SAVEINFO.DLGWND_DBLCLICK_CHKBOX_STR = \"ダブルクリックでセーブ。\"\r\n"
        "#SAVEINFO.WARNING_STR = \"セーブしてもよろしいですか？\"\r\n"
        "#SAVEINFO.QUICK_WARNING_STR = \"クイックセーブ？\"\r\n"
        "#LOADINFO.DLGWND_CAPTION_TITLE_STR = \"ロード\"\r\n"
        "#LOADINFO.DLGWND_DESIDE_BUTTON_STR = \"ロードする\"\r\n"
        "#LOADINFO.DLGWND_DATALIST_NAMEHEADER_STR = \"ロード\"\r\n"
        "#LOADINFO.DLGWND_WARNING_CHKBOX_STR = \"ロード確認。\"\r\n"
        "#LOADINFO.DLGWND_DBLCLICK_CHKBOX_STR = \"ダブルクリックでロード。\"\r\n"
        "#LOADINFO.WARNING_STR = \"ロードしてもよろしいですか？\"\r\n"
        "#LOADINFO.QUICK_WARNING_STR = \"クイックロード？\"\r\n"
        "#CHRKOE.NOT_LOOK_NAME_STR = \"？？？\"\r\n"
        "#SAVE.CNT = 100\r\n"
        "#MWND.002.MOJI_CNT = 26, 16\r\n"
        "#MWND.002.MOJI_SIZE = 24\r\n"
        "#MWND.002.MOJI_SPACE = -1, 4\r\n"
        "#MWND.002.NAME_MOJI_SIZE = 18\r\n"
        "#MWND.002.TALK_MARGIN = 26, 36, 0, 36\r\n"
        "#SELBTN.000.MOJI_SIZE = 30, 0, 0, 0\r\n"
        "#SELBTN.001.MOJI_SIZE = 30, 0, 0, 0\r\n";
}

g00::G00Image BuildSyntheticType0Sprite(imaging::BgraColor bg,
                                        imaging::RectI marker_rect,
                                        imaging::BgraColor marker_color) {
    g00::G00Image img;
    img.type = g00::G00Type::kType0Bgr24;
    img.width = static_cast<std::uint16_t>(kSyntheticScreenW);
    img.height = static_cast<std::uint16_t>(kSyntheticScreenH);
    img.canvas = imaging::BgraImage(kSyntheticScreenW, kSyntheticScreenH, bg);
    for (int y = 0; y < kSyntheticScreenH; ++y) {
        for (int x = 0; x < kSyntheticScreenW; ++x) {
            imaging::BgraColor c = bg;
            c.b = static_cast<std::uint8_t>((c.b + ((x * 3 + y * 5) & 0x0F)) & 0xFF);
            c.g = static_cast<std::uint8_t>((c.g + ((x * 7 + y * 2) & 0x0F)) & 0xFF);
            c.r = static_cast<std::uint8_t>((c.r + ((x * 5 + y * 3) & 0x0F)) & 0xFF);
            img.canvas.SetPixel(x, y, c);
        }
    }
    if (!marker_rect.empty()) {
        img.canvas.FillRect(marker_rect, marker_color);
    }
    img.canvas_modified = true;
    return img;
}

g00::G00Image BuildSyntheticRecollectMultiCut(const imaging::BgraImage& base_bg,
                                              imaging::BgraColor bar_tint,
                                              imaging::BgraColor jp_stroke_color) {
    g00::G00Image img;
    img.type = g00::G00Type::kType2MultiCutBgra32;
    img.width = static_cast<std::uint16_t>(kSyntheticScreenW);
    img.height = static_cast<std::uint16_t>(kSyntheticScreenH);
    img.cuts.resize(kSyntheticRecollectCuts);

    for (std::size_t i = 0u; i < kSyntheticRecollectCuts; ++i) {
        const int y1 = 38 + static_cast<int>(i) * 20;
        const int y2 = y1 + 19;
        g00::G00Cut& cut = img.cuts[i];
        cut.is_empty = false;
        cut.descriptor = g00::G00CutDescriptor{280, y1, 639, y2, 0, 0};
        cut.cut_header.cut_type = 1u;
        cut.cut_header.part_count = 1u;
        cut.cut_header.bbox_x = 0;
        cut.cut_header.bbox_y = 0;
        cut.cut_header.bbox_w = 360;
        cut.cut_header.bbox_h = 20;
        cut.cut_header.cut_canvas_w = 360;
        cut.cut_header.cut_canvas_h = 20;
        cut.canvas = imaging::BgraImage(kSyntheticScreenW, kSyntheticScreenH);

        for (int y = y1; y <= y2; ++y) {
            for (int x = 280; x <= 639; ++x) {
                const imaging::BgraColor src = base_bg.PixelAt(x, y);
                imaging::BgraColor tinted{
                    static_cast<std::uint8_t>((src.b + bar_tint.b) / 2),
                    static_cast<std::uint8_t>((src.g + bar_tint.g) / 2),
                    static_cast<std::uint8_t>((src.r + bar_tint.r) / 2),
                    imaging::kOpaqueAlpha,
                };
                cut.canvas.SetPixel(x, y, tinted);
            }
        }
        // Synthetic 2px Japanese glyph stroke inside the strip's text region.
        cut.canvas.FillRect(imaging::RectI{395, y1 + 6, 30, 2}, jp_stroke_color);
        cut.modified = true;
    }
    return img;
}

class DuplicateAttenRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "duplicate_atten"; }
    void Apply(PatchBuildContext& ctx) override {
        g00::G00Image atten = ctx.LoadG00("ATTEN.g00");
        atten.MutableCanvas().SetPixel(10, 10, imaging::BgraColor{1, 2, 3, 255});
        ctx.SubmitPatchedG00("ATTEN.g00", atten);
    }
};

bool ContainsTargetPath(const MenuPatchManifest& manifest, std::string_view rel_path) {
    return std::any_of(manifest.records.begin(), manifest.records.end(),
                       [rel_path](const MenuPatchRecord& r) {
                           return r.target_rel_path == rel_path;
                       });
}

bool ContainsJapaneseCodePoint(std::string_view utf8) {
    std::size_t i = 0u;
    while (i < utf8.size()) {
        const auto b0 = static_cast<std::uint8_t>(utf8[i]);
        if (b0 < 0x80u) {
            ++i;
            continue;
        }
        std::uint32_t cp = 0u;
        if ((b0 & 0xE0u) == 0xC0u && i + 1u < utf8.size()) {
            cp = ((b0 & 0x1Fu) << 6u) |
                 (static_cast<std::uint8_t>(utf8[i + 1u]) & 0x3Fu);
            i += 2u;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2u < utf8.size()) {
            cp = ((b0 & 0x0Fu) << 12u) |
                 ((static_cast<std::uint8_t>(utf8[i + 1u]) & 0x3Fu) << 6u) |
                 (static_cast<std::uint8_t>(utf8[i + 2u]) & 0x3Fu);
            i += 3u;
        } else {
            i += 4u;
            continue;
        }
        if ((cp >= 0x3040u && cp <= 0x30FFu) ||
            (cp >= 0x4E00u && cp <= 0x9FFFu) ||
            (cp >= 0xFF01u && cp <= 0xFF9Fu)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(MenuPatcherCt, CommittedAdieuMenuPatchBinSatisfiesContainerAndSecurityInvariants) {
    const fs::path patch_path = fs::u8path(ADIEU_SOURCE_DIR) / "menu_patch.bin";
    ASSERT_TRUE(fs::is_regular_file(patch_path))
        << "Missing committed Adieu/menu_patch.bin at " << patch_path.u8string();

    const std::vector<std::uint8_t> blob = ReadBinaryFile(patch_path);
    ASSERT_GT(blob.size(), kMenuPatchHeaderBytes);

    std::string signature_violation;
    EXPECT_TRUE(VerifyNoRawAssetSignatures(blob, &signature_violation))
        << signature_violation;
    EXPECT_TRUE(signature_violation.empty());

    const MenuPatchManifest manifest = ParseMenuPatchContainer(blob);
    EXPECT_EQ(manifest.version, kMenuPatchVersion);
    EXPECT_NE(manifest.manifest_hash, 0u);
    EXPECT_GE(manifest.records.size(), kMinExpectedCommittedRecords);

    // Every domain renderer must contribute its key runtime records.
    EXPECT_TRUE(ContainsTargetPath(manifest, "Gameexe.dat"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\ATTEN.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\ME_02.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\ME_03.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\ME_04.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\MESS99.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\MESS98.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\MESS81.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\CM_B02.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\CM_B03.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\CM101.g00"));
    EXPECT_TRUE(ContainsTargetPath(manifest, "g00\\MUSIC1.g00"));

    std::set<std::string> seen_targets;
    for (const MenuPatchRecord& rec : manifest.records) {
        SCOPED_TRACE(rec.target_rel_path);
        EXPECT_TRUE(IsSafeRelativeAssetPath(rec.source_rel_path));
        EXPECT_TRUE(IsAllowedMenuTargetPath(rec.target_rel_path));
        EXPECT_TRUE(seen_targets.insert(rec.target_rel_path).second)
            << "Duplicate target_rel_path in menu_patch.bin: " << rec.target_rel_path;
        EXPECT_GE(rec.orig_size, kMinOrigSliceBytes);
        EXPECT_LE(rec.orig_size, kMaxAssetPayloadBytes);
        EXPECT_GT(rec.new_size, 0u);
        EXPECT_LE(rec.new_size, kMaxAssetPayloadBytes);
        EXPECT_EQ(rec.xor_payload.size(), static_cast<std::size_t>(rec.new_size));
        EXPECT_EQ(rec.flags, kFlagBackupStandalone);
    }
}

TEST(MenuPatcherCt,
     MenuPatchPipelineBuildsDeterministicMultiDomainBundleAndContactSheetsOffline) {
    ScopedTempDir sandbox;
    const fs::path game_dir = sandbox.path() / "GameData";
    const fs::path g00_dir = game_dir / "g00";
    const g00::GameexeKey256 key256 = BuildSyntheticKey256();
    WriteSyntheticSiglusExeWithKey(game_dir / "SiglusEngine.exe", key256);

    const std::u16string orig_gameexe_u16 =
        g00::GameexeUtf8ToUtf16(BuildSyntheticJapaneseGameexeText(), true);
    WriteBinaryFile(game_dir / "Gameexe.dat",
                    g00::EncodeGameexe(orig_gameexe_u16, key256));

    const g00::G00Image atten_orig = BuildSyntheticType0Sprite(
        imaging::BgraColor{10, 10, 12, 255}, imaging::RectI{200, 224, 240, 48},
        imaging::BgraColor{220, 220, 220, 255});
    WriteBinaryFile(g00_dir / "ATTEN.g00", g00::G00Codec::Encode(atten_orig));

    const g00::G00Image me01_orig = BuildSyntheticType0Sprite(
        imaging::BgraColor{18, 22, 28, 255}, imaging::RectI{}, {});
    WriteBinaryFile(g00_dir / "ME_01.g00", g00::G00Codec::Encode(me01_orig));
    WriteBinaryFile(g00_dir / "ME_02.g00",
                    g00::G00Codec::Encode(BuildSyntheticRecollectMultiCut(
                        me01_orig.canvas, imaging::BgraColor{18, 22, 28, 255},
                        imaging::BgraColor{90, 95, 100, 255})));
    WriteBinaryFile(g00_dir / "ME_03.g00",
                    g00::G00Codec::Encode(BuildSyntheticRecollectMultiCut(
                        me01_orig.canvas, imaging::BgraColor{24, 68, 56, 255},
                        imaging::BgraColor{210, 220, 210, 255})));
    WriteBinaryFile(g00_dir / "ME_04.g00",
                    g00::G00Codec::Encode(BuildSyntheticRecollectMultiCut(
                        me01_orig.canvas, imaging::BgraColor{12, 16, 120, 255},
                        imaging::BgraColor{180, 210, 245, 255})));

    auto make_pipeline = []() {
        MenuPatchPipeline p;
        p.RegisterRenderer(CreateGameexePatchRules());
        p.RegisterRenderer(CreateRenderCardsDialogs());
        p.RegisterRenderer(CreateRenderSystemMenus());
        return p;
    };

    const fs::path out1 = sandbox.path() / "out1.bin";
    const fs::path out2 = sandbox.path() / "out2.bin";
    const fs::path sheets_dir = sandbox.path() / "sheets";

    const MenuPatchBuildSummary sum1 =
        make_pipeline().Run(MenuPatchBuildOptions{game_dir, out1, sheets_dir});
    const MenuPatchBuildSummary sum2 =
        make_pipeline().Run(MenuPatchBuildOptions{game_dir, out2, {}});

    EXPECT_EQ(sum1.renderer_count, 3u);
    EXPECT_EQ(sum1.record_count, 5u);
    EXPECT_EQ(sum1.manifest_hash, sum2.manifest_hash);
    const std::vector<std::uint8_t> bytes1 = ReadBinaryFile(out1);
    const std::vector<std::uint8_t> bytes2 = ReadBinaryFile(out2);
    EXPECT_EQ(bytes1, bytes2);
    EXPECT_TRUE(VerifyNoRawAssetSignatures(bytes1));

    // Contact sheet PNGs were written and decode cleanly as non-empty images.
    EXPECT_FALSE(sum1.contact_sheet_pages.empty());
    for (const fs::path& sheet : sum1.contact_sheet_pages) {
        ASSERT_TRUE(fs::is_regular_file(sheet));
        const SpriteCanvas loaded = SpriteCanvas::LoadPng(sheet);
        EXPECT_GT(loaded.width(), 0);
        EXPECT_GT(loaded.height(), 0);
    }

    // Producer-to-consumer unmasking of all 5 records against pristine files.
    const MenuPatchManifest manifest = ParseMenuPatchContainer(bytes1);
    ASSERT_EQ(manifest.records.size(), 5u);
    for (const MenuPatchRecord& rec : manifest.records) {
        const std::vector<std::uint8_t> orig_bytes =
            ReadBinaryFile(game_dir / fs::u8path(rec.source_rel_path));
        const std::vector<std::uint8_t> unmasked =
            UnmaskMenuPatchRecord(rec, orig_bytes);

        if (rec.target_rel_path == "Gameexe.dat") {
            const g00::GameexeDocument patched_doc = g00::GameexeDocument::FromUtf16(
                g00::DecodeGameexe(unmasked, key256));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#DISCMARK"),
                      std::optional<std::string>("\"sayonara.env\""));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#MWND.002.MOJI_SIZE"),
                      std::optional<std::string>("20"));
            EXPECT_EQ(patched_doc.FindDirectiveRawValue("#SELBTN.000.MOJI_SIZE"),
                      std::optional<std::string>("22, 0, 0, 0"));
            EXPECT_FALSE(ContainsJapaneseCodePoint(patched_doc.ToUtf8(false)));
        } else {
            const g00::G00Image orig_img = g00::G00Codec::Decode(orig_bytes);
            const g00::G00Image patched_img = g00::G00Codec::Decode(unmasked);
            if (patched_img.type == g00::G00Type::kType2MultiCutBgra32) {
                std::set<std::size_t> all_cuts;
                for (std::size_t c = 0u; c < patched_img.cuts.size(); ++c) {
                    all_cuts.insert(c);
                    EXPECT_NE(patched_img.cuts[c].canvas, orig_img.cuts[c].canvas);
                }
                EXPECT_TRUE(g00::G00Codec::VerifyMetadataParity(orig_bytes, unmasked,
                                                                all_cuts));
            } else {
                EXPECT_NE(patched_img.canvas, orig_img.canvas);
                EXPECT_TRUE(
                    g00::G00Codec::VerifyMetadataParity(orig_bytes, unmasked, {}));
            }
        }
    }
}

TEST(MenuPatcherCt,
     DuplicateTargetAcrossRenderersFailsWithoutLeavingPartialPatchFile) {
    ScopedTempDir sandbox;
    const fs::path game_dir = sandbox.path() / "GameData";
    const g00::GameexeKey256 key256 = BuildSyntheticKey256();
    WriteSyntheticSiglusExeWithKey(game_dir / "SiglusEngine.exe", key256);
    WriteBinaryFile(
        game_dir / "Gameexe.dat",
        g00::EncodeGameexe(
            g00::GameexeUtf8ToUtf16(BuildSyntheticJapaneseGameexeText(), true),
            key256));
    WriteBinaryFile(game_dir / "g00" / "ATTEN.g00",
                    g00::G00Codec::Encode(BuildSyntheticType0Sprite(
                        imaging::BgraColor{8, 8, 8, 255},
                        imaging::RectI{200, 224, 240, 48},
                        imaging::BgraColor{200, 200, 200, 255})));

    MenuPatchPipeline pipeline;
    pipeline.RegisterRenderer(CreateRenderCardsDialogs());
    pipeline.RegisterRenderer(std::make_unique<DuplicateAttenRenderer>());

    const fs::path out_patch = sandbox.path() / "should_not_exist.bin";
    EXPECT_THROW(
        (void)pipeline.Run(MenuPatchBuildOptions{game_dir, out_patch, {}}),
        std::invalid_argument);
    EXPECT_FALSE(fs::exists(out_patch));
}

}  // namespace adi::menu
