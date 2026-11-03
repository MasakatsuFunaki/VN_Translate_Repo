// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The output-formatting helpers.  Every one of these exists because the tools'
// stdout is pinned by the committed logs -- so the expectations below are the
// exact forms those logs contain, not what looks tidy.
#include <gtest/gtest.h>

#include <string>

#include "common/util.h"
#include "patch/deploy.h"

using namespace ksh;

TEST(Format, hexu_pads_uppercase_to_the_field_width) {
    EXPECT_EQ(hexu(0x1AF, 8), "000001AF");
    EXPECT_EQ(hexu(0x1AF, 0), "1AF");
    EXPECT_EQ(hexu(0, 4), "0000");
    // The width is a minimum: a value too wide for it keeps every digit.
    EXPECT_EQ(hexu(0x123456, 4), "123456");
}

TEST(Format, hexl_is_lowercase_with_an_0x_prefix) {
    EXPECT_EQ(hexl(0x42896B), "0x42896b");
    EXPECT_EQ(hexl(0), "0x0");
}

// A NEGATIVE value pads the field INCLUDING the sign, so at width 8 the
// magnitude gets seven digits -- not the two's-complement form.
TEST(Format, hexu_signed_pads_the_sign_into_the_field) {
    EXPECT_EQ(hexu_signed(0x3C35, 8), "00003C35");
    EXPECT_EQ(hexu_signed(-5, 8), "-0000005");
    EXPECT_EQ(hexu_signed(-0x12345678LL, 8), "-12345678");
}

TEST(Format, hex_bytes_and_hex_lower) {
    const Bytes b{0x00, 0x1F, 0xAB, 0xFF};
    EXPECT_EQ(hex_bytes(b), "00 1F AB FF");
    EXPECT_EQ(hex_lower(b.data(), b.size()), "001fabff");
    EXPECT_EQ(hex_bytes(Bytes{}), "");
}

TEST(Format, ljust_pads_but_never_truncates) {
    EXPECT_EQ(ljust("ab", 4), "ab  ");
    EXPECT_EQ(ljust("abcdef", 4), "abcdef");
}

// The byte-string literal form 02_decrypt_devnames prints the decrypted
// device-name table with; the escaping rules below are what the log records.
TEST(Format, bytes_repr_escaping_and_quote_choice_are_pinned) {
    auto repr = [](const std::string& s) {
        return bytes_repr(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    };
    EXPECT_EQ(repr("abc"), "b'abc'");
    EXPECT_EQ(repr(std::string("\\\\.\\SICE")), "b'\\\\\\\\.\\\\SICE'");
    EXPECT_EQ(repr(std::string("a\0b", 3)), "b'a\\x00b'");
    EXPECT_EQ(repr("\t\n\r"), "b'\\t\\n\\r'");
    EXPECT_EQ(repr("\x7F\x80\xFF"), "b'\\x7f\\x80\\xff'");
    // A single quote in the data switches the delimiter to double quotes...
    EXPECT_EQ(repr("it's"), "b\"it's\"");
    // ...unless a double quote is present too, in which case the single quote
    // is escaped instead.
    EXPECT_EQ(repr("it's \"x\""), "b'it\\'s \"x\"'");
}

// A --game-dir with a trailing slash must not produce a doubled separator in the
// copy log or the DONE line.
TEST(PathJoin, does_not_double_or_invent_separators) {
    using patch::path_join;
    EXPECT_EQ(path_join(R"(C:\Games\KH)", "launch.exe"), R"(C:\Games\KH\launch.exe)");
    EXPECT_EQ(path_join(R"(C:\Games\KH\)", "launch.exe"), R"(C:\Games\KH\launch.exe)");
    EXPECT_EQ(path_join("C:/Games/KH/", "launch.exe"), "C:/Games/KH/launch.exe");
    EXPECT_EQ(path_join("C:", "launch.exe"), "C:launch.exe");
    EXPECT_EQ(path_join("", "launch.exe"), "launch.exe");
}

TEST(PathJoin, base_name_takes_either_separator) {
    EXPECT_EQ(patch::base_name(R"(C:\a\b\launch.exe)"), "launch.exe");
    EXPECT_EQ(patch::base_name("a/b/launch.exe"), "launch.exe");
    EXPECT_EQ(patch::base_name("launch.exe"), "launch.exe");
}

TEST(CanWrite, probes_the_directory_rather_than_trusting_acls) {
    EXPECT_TRUE(patch::can_write(testing::TempDir()));
    EXPECT_FALSE(patch::can_write(testing::TempDir() + "no_such_dir_here_42"));
    // A file is not a directory.
    EXPECT_FALSE(patch::can_write(std::string(KSH_PROJECT_DIR) + "\\README.md"));
}
