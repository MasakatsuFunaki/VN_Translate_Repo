// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for the Nitroplus .pak v4 archive packer/reader and the .nps
// script rebuilder.
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/util.h"
#include "pak/nps_rebuilder.h"
#include "pak/pak_archive.h"

using namespace ksh;
using namespace ksh::pak;

namespace {

constexpr std::uint32_t kExpectedKishinMagicHash = 0xCC09DBB5u;
constexpr std::uint32_t kExpectedDa0100PathHash = 0x46EB80C6u;
constexpr std::uint32_t kExpectedSignedByte80Hash = 0xFFFFFF80u;
constexpr std::uint32_t kExpectedSignedByteFFHash = 0xFFFFFFFFu;
constexpr std::uint32_t kTestCipherKey = 0x11223344u;

constexpr int kBox0Index = 0;
constexpr int kBox1Index = 1;
constexpr int kBox4Index = 4;
constexpr int kBox5Index = 5;
constexpr int kBox8Index = 8;
constexpr int kBox9Index = 9;
constexpr int kBox10Index = 10;
constexpr int kBox12Index = 12;
constexpr int kBox13Index = 13;

std::vector<std::string> SplitCrLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find('\r', start);
        if (pos == std::string::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

}  // namespace

TEST(PakArchive, compute_pak_key_matches_system_dll_signed_byte_hash) {
    EXPECT_EQ(ComputePakKey("KishinHisyouDB"), kExpectedKishinMagicHash);
    EXPECT_EQ(ComputePakKey(R"(script\da0100.nps)"), kExpectedDa0100PathHash);
    EXPECT_EQ(ComputePakKey(""), 0u);

    const std::uint8_t byte_80 = 0x80u;
    const std::uint8_t byte_ff = 0xFFu;
    EXPECT_EQ(ComputePakKey(&byte_80, 1), kExpectedSignedByte80Hash);
    EXPECT_EQ(ComputePakKey(&byte_ff, 1), kExpectedSignedByteFFHash);

    // Two-byte sequence with high bit set exercises signed-char sign extension
    // before the 0x89 multiply-accumulate step.
    const std::uint8_t high_pair[] = {0x81u, 0x40u};
    const std::uint32_t expected_pair =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(0x81u))) *
            0x89u +
        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(0x40u)));
    EXPECT_EQ(ComputePakKey(high_pair, sizeof(high_pair)), expected_pair);
}

TEST(PakArchive, xor_word_stream_handles_aligned_and_unaligned_lengths) {
    EXPECT_TRUE(XorWordStream(Bytes{}, kTestCipherKey).empty());

    const Bytes one_byte{0xAAu};
    const Bytes enc_one = XorWordStream(one_byte, kTestCipherKey);
    ASSERT_EQ(enc_one.size(), 1u);
    EXPECT_EQ(enc_one[0], static_cast<std::uint8_t>(0xAAu ^ 0x44u));
    EXPECT_EQ(XorWordStream(enc_one, kTestCipherKey), one_byte);

    const Bytes aligned{0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u};
    const Bytes enc_aligned = XorWordStream(aligned, kTestCipherKey);
    const Bytes expected_aligned{
        static_cast<std::uint8_t>(0x01u ^ 0x44u), static_cast<std::uint8_t>(0x02u ^ 0x33u),
        static_cast<std::uint8_t>(0x03u ^ 0x22u), static_cast<std::uint8_t>(0x04u ^ 0x11u),
        static_cast<std::uint8_t>(0x05u ^ 0x44u), static_cast<std::uint8_t>(0x06u ^ 0x33u),
        static_cast<std::uint8_t>(0x07u ^ 0x22u), static_cast<std::uint8_t>(0x08u ^ 0x11u),
    };
    EXPECT_EQ(enc_aligned, expected_aligned);
    EXPECT_EQ(XorWordStream(enc_aligned, kTestCipherKey), aligned);

    const Bytes unaligned{0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u};
    const Bytes enc_unaligned = XorWordStream(unaligned, kTestCipherKey);
    ASSERT_EQ(enc_unaligned.size(), unaligned.size());
    EXPECT_EQ(enc_unaligned[4], static_cast<std::uint8_t>(0x50u ^ 0x44u));
    EXPECT_EQ(enc_unaligned[5], static_cast<std::uint8_t>(0x60u ^ 0x33u));
    EXPECT_EQ(enc_unaligned[6], static_cast<std::uint8_t>(0x70u ^ 0x22u));
    EXPECT_EQ(XorWordStream(enc_unaligned, kTestCipherKey), unaligned);
}

TEST(PakArchive, write_and_read_pak_archive_round_trip_compressed_and_uncompressed) {
    std::vector<PakEntry> entries;

    PakEntry e1;
    e1.relative_path = R"(script\da0100.nps)";
    e1.payload = Utf8ToCp932("<BOX type=\"0\">\r\xE3\x80\x80Hello Demonbane!<k>\r");
    e1.compress = true;
    entries.push_back(e1);

    PakEntry e2;
    e2.relative_path = R"(script\product.ini)";
    e2.payload = Utf8ToCp932("[製品情報]\rname=KishinHisho");
    e2.compress = false;
    entries.push_back(e2);

    PakEntry e3;
    e3.relative_path = R"(script\empty.nps)";
    e3.payload = {};
    e3.compress = true;
    entries.push_back(e3);

    const Bytes pak_bytes = WritePakArchive(entries, kDefaultPakMagicName);
    ASSERT_GE(pak_bytes.size(), 0x114u);

    const PakArchiveReader reader(pak_bytes);
    EXPECT_EQ(reader.Version(), kPakFormatVersion);
    EXPECT_EQ(reader.MagicName(), kDefaultPakMagicName);
    ASSERT_EQ(reader.Entries().size(), entries.size());
    EXPECT_NO_THROW(reader.VerifyAgainst(entries));
    EXPECT_NO_THROW(VerifyPakArchive(pak_bytes, entries));

    const PakEntry* found_ini = reader.FindEntry(R"(script\product.ini)");
    ASSERT_NE(found_ini, nullptr);
    EXPECT_FALSE(found_ini->compress);
    EXPECT_EQ(found_ini->payload, e2.payload);

    const PakEntry* found_nps = reader.FindEntry(R"(script\da0100.nps)");
    ASSERT_NE(found_nps, nullptr);
    EXPECT_TRUE(found_nps->compress);
    EXPECT_EQ(found_nps->payload, e1.payload);

    std::vector<PakEntry> tampered = entries;
    tampered[0].payload.push_back(0x00u);
    EXPECT_THROW(reader.VerifyAgainst(tampered), std::runtime_error);
}

TEST(NpsRebuilder, box_width_policy_assigns_expected_column_budgets) {
    const DefaultBoxWidthPolicy policy;
    EXPECT_EQ(policy.ColumnBudgetForBox(kBox0Index), kWideBoxColumnBudget);
    EXPECT_EQ(policy.ColumnBudgetForBox(kBox9Index), kWideBoxColumnBudget);
    EXPECT_EQ(policy.ColumnBudgetForBox(kBox12Index), kWideBoxColumnBudget);
    EXPECT_EQ(policy.ColumnBudgetForBox(kBox13Index), kWideBoxColumnBudget);

    EXPECT_EQ(policy.ColumnBudgetForBox(kBox1Index), kNarrowBoxColumnBudget);
    EXPECT_EQ(policy.ColumnBudgetForBox(kBox4Index), kNarrowBoxColumnBudget);

    EXPECT_EQ(policy.ColumnBudgetForBox(kBox5Index), kMediumBoxColumnBudget);
    EXPECT_EQ(policy.ColumnBudgetForBox(kBox8Index), kMediumBoxColumnBudget);
    EXPECT_EQ(policy.ColumnBudgetForBox(kBox10Index), kMediumBoxColumnBudget);

    EXPECT_TRUE(policy.IsFourLinePaginatedBox(kBox0Index));
    EXPECT_TRUE(policy.IsFourLinePaginatedBox(kBox8Index));
    EXPECT_FALSE(policy.IsFourLinePaginatedBox(kBox9Index));
    EXPECT_FALSE(policy.IsFourLinePaginatedBox(kBox12Index));
}

TEST(NpsRebuilder, token_aware_word_wrapping_respects_box_0_1_and_5_column_budgets) {
    const NpsRebuilder rebuilder;
    const std::string text =
        "The giant clockwork Deus Machina stood silently beneath the crimson sky of Arkham City "
        "waiting for its master to speak.";

    for (int box_type : {kBox0Index, kBox1Index, kBox5Index}) {
        NpsBlockMetadata meta;
        meta.box_type = box_type;
        meta.first_indent = "\xE3\x80\x80";
        meta.trailing_k = true;
        meta.k_clause_count = 1;
        meta.orig_k_clauses_clean = {"old"};
        meta.orig_clean_en = "old";
        meta.orig_raw_en = "\xE3\x80\x80old<k>";

        const NpsBlockResult res = rebuilder.ReconstructBlock(text, meta);
        EXPECT_TRUE(res.was_modified);
        EXPECT_TRUE(res.was_rewrapped);

        const int budget = rebuilder.WidthPolicy().ColumnBudgetForBox(box_type);
        const std::vector<std::string> lines = SplitCrLines(res.text);
        ASSERT_GT(lines.size(), 1u);
        for (const auto& line : lines) {
            if (line.empty()) {
                continue;
            }
            EXPECT_LE(ComputeVisibleColumns(line), budget)
                << "Line exceeded budget " << budget << " for box_type=" << box_type << ": "
                << line;
        }
    }
}

TEST(NpsRebuilder, ruby_attribute_equals_sign_is_protected_in_da0504_block_2) {
    const NpsRebuilder rebuilder;

    NpsBlockMetadata meta;
    meta.box_type = kBox9Index;
    meta.first_indent = "";
    meta.trailing_k = false;
    meta.k_clause_count = 2;
    meta.orig_k_clauses_clean = {
        "Recollection/Correspondence. = [Memoirs].",
        "Dreams/Drops of overflowed feelings.",
    };
    meta.ruby_list = {
        {"Recollection", "Play"},
        {"Correspondence", "Channeling"},
        {"=", "equals"},
    };
    meta.orig_clean_en =
        "Recollection/Correspondence. = [Memoirs]. Dreams/Drops of overflowed feelings.";
    meta.orig_raw_en =
        "<r text=\"Play\">Recollection</r>/<r text=\"Channeling\">Correspondence</r>. "
        "<r text=\"equals\">=</r> [Memoirs].<k>\rDreams/Drops of overflowed feelings.";

    const std::string modified_en =
        "Recollection/Correspondence. = [Memoirs]. Dreams/Drops of overflowing feelings.";
    const NpsBlockResult res = rebuilder.ReconstructBlock(modified_en, meta);
    EXPECT_TRUE(res.was_modified);
    EXPECT_TRUE(res.was_rewrapped);

    EXPECT_NE(res.text.find(R"(<r text="Play">Recollection</r>)"), std::string::npos);
    EXPECT_NE(res.text.find(R"(<r text="Channeling">Correspondence</r>)"), std::string::npos);
    EXPECT_NE(res.text.find(R"(<r text="equals">=</r>)"), std::string::npos);
    EXPECT_EQ(res.text.find(R"(<r text<r)"), std::string::npos);
}

TEST(NpsRebuilder, wait_tags_are_preserved_across_stripped_ruby_and_font_tags) {
    std::vector<NpsTextPair> ruby_list = {{"Deus Machina", "God Machine"}};
    std::vector<NpsTextPair> font_list = {{"Awaken", R"(<FONT COLOR="#FF0000">)"}};
    std::vector<NpsTextPair> wait_list = {
        {"Deus Machina.", R"(<WAIT TIME="15">)"},
        {"Awaken--now!", R"(<WAIT TIME="30">)"},
    };

    const std::string annotated = ApplyInlineAnnotations(
        "Behold the Deus Machina. Awaken--now!", ruby_list, font_list, wait_list);

    EXPECT_TRUE(ruby_list.empty());
    EXPECT_TRUE(font_list.empty());
    EXPECT_TRUE(wait_list.empty());
    EXPECT_EQ(annotated,
              "Behold the <r text=\"God Machine\">Deus Machina</r>.<WAIT TIME=\"15\"> "
              "<FONT COLOR=\"#FF0000\">Awaken</FONT>\xE2\x80\x95\xE2\x80\x95now!<WAIT TIME=\"30\">");
}

TEST(NpsRebuilder, boundary_font_tags_are_preserved_at_block_edges_and_before_mid_wait) {
    const NpsRebuilder rebuilder;

    NpsBlockMetadata meta;
    meta.box_type = kBox12Index;
    meta.first_indent = "\xE3\x80\x80\xE3\x80\x80\xE3\x80\x80";
    meta.trailing_k = false;
    meta.k_clause_count = 1;
    meta.orig_k_clauses_clean = {
        "Deus non est in suo caelo--Omnia rectus cum mundi. "
        "God's not in His heaven, All's right with the world.",
    };
    meta.font_list = {
        {"God's not in His heaven, All's right with the world.",
         R"(<FONT FACE="ＭＳ Ｐ明朝" SIZE="18" COLOR="#0000FF"><I>)"},
    };
    meta.wait_list = {
        {"ctus cum mundi.", R"(<wait time="5">)"},
        {"with the world.", R"(<wait time="99999999999">)"},
    };
    meta.orig_clean_en = meta.orig_k_clauses_clean.front();
    meta.orig_raw_en =
        "\xE3\x80\x80" "Deus non est in suo caelo" "\xE2\x80\x95\xE2\x80\x95"
        "Omnia rectus cum mundi.</FONT><wait time=\"5\">\r"
        "<FONT FACE=\"ＭＳ Ｐ明朝\" SIZE=\"18\" COLOR=\"#0000FF\"><I>\r"
        "\xE3\x80\x80" "God's not in His heaven, All's right with the world.</I></FONT>"
        "<wait time=\"99999999999\">";
    meta.boundary_suffix_tag = "</FONT>";

    const std::string modified_en =
        "Deus non est in suo caelo--Omnia rectus cum mundi. "
        "God's not in His heaven, all is right with the world.";
    // Adjust font_list target to match modified second sentence.
    meta.font_list[0].first = "God's not in His heaven, all is right with the world.";

    const NpsBlockResult res = rebuilder.ReconstructBlock(modified_en, meta);
    EXPECT_TRUE(res.was_modified);
    EXPECT_NE(res.text.find("</FONT><wait time=\"5\">"), std::string::npos);
    EXPECT_NE(res.text.find("</I></FONT><wait time=\"99999999999\">"), std::string::npos);
}

TEST(NpsRebuilder, typewriter_reconstruction_emits_per_char_keystroke_se_and_preserves_ruby) {
    const NpsRebuilder rebuilder;

    NpsBlockMetadata meta;
    meta.box_type = kBox9Index;
    meta.first_indent = "\xE3\x80\x80";
    meta.trailing_k = true;
    meta.is_typewriter = true;
    meta.has_complex_fx = true;
    meta.ruby_list = {{"restriction removal", "climax"}};
    meta.orig_clean_en = "Waiting for the restriction removal.";
    meta.orig_raw_en =
        "\xE3\x80\x80W<WAIT TIME=\"2\">"
        "<k>\r<SE ID=\"2\" SRC=\"se人体行動_手動タイプキャリッジ戻す飛翔\" MODE=\"normal\" TIME=\"\" LOOP=\"\" vol=\"\">";

    const std::string modified_en = "Awaiting the restriction removal.";
    const NpsBlockResult res = rebuilder.ReconstructBlock(modified_en, meta);
    EXPECT_TRUE(res.was_modified);
    EXPECT_TRUE(res.was_rewrapped);
    EXPECT_NE(res.text.find(R"(<r text="climax">r<WAIT TIME="2">)"), std::string::npos);
    EXPECT_NE(res.text.find(R"(l</r><WAIT TIME="2">)"), std::string::npos);
    EXPECT_NE(res.text.find(R"(<k>)" "\r" R"(<SE ID="2")"), std::string::npos);
}

TEST(NpsRebuilder, utf8_to_cp932_normalizes_demonbane_symbols_and_nec_kanji) {
    // U+2014 EM DASH -> 0x815C, U+301C WAVE DASH -> 0x8160, U+4F94 -> 0xED56.
    const std::string input = "\xE2\x80\x94\xE3\x80\x9C\xE4\xBE\x94";
    const Bytes cp932 = Utf8ToCp932(input);
    const Bytes expected{0x81u, 0x5Cu, 0x81u, 0x60u, 0xEDu, 0x56u};
    EXPECT_EQ(cp932, expected);
}
