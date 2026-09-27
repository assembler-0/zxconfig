// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/text.hpp>

#include <gtest/gtest.h>

TEST(DecodeEscapes, passes_plain_text_through) {
    auto out = zxconfig::decode_escapes("hello, world 123 !?");
    EXPECT_TRUE(out.ok());
    EXPECT_EQ(out.value, "hello, world 123 !?");
}

TEST(DecodeEscapes, decodes_the_simple_escapes) {
    auto out = zxconfig::decode_escapes(R"(a\n\t\"\\b)");
    EXPECT_TRUE(out.ok());
    EXPECT_EQ(out.value, "a\n\t\"\\b");
}

TEST(DecodeEscapes, decodes_unicode) {
    auto out = zxconfig::decode_escapes(R"(\u{03C0})");
    EXPECT_TRUE(out.ok());
    EXPECT_EQ(out.value, "π");

    auto wide = zxconfig::decode_escapes(R"(\u{1F600})");
    EXPECT_TRUE(wide.ok());
    EXPECT_EQ(wide.value, "\xF0\x9F\x98\x80");
}

TEST(DecodeEscapes, rejects_invalid_unicode) {
    EXPECT_FALSE(zxconfig::decode_escapes(R"(\u{D800})").ok());   // surrogate
    EXPECT_FALSE(zxconfig::decode_escapes(R"(\u{0})").ok());      // zero
    EXPECT_FALSE(zxconfig::decode_escapes(R"(\u{110000})").ok()); // out of range
    EXPECT_FALSE(zxconfig::decode_escapes(R"(\u03C0)").ok());     // missing brace
    EXPECT_FALSE(zxconfig::decode_escapes(R"(\u{})").ok());       // empty
    EXPECT_FALSE(zxconfig::decode_escapes(R"(\u{03C0)").ok());    // unclosed
}

TEST(DecodeEscapes, keeps_unknown_escapes_as_written) {
    // A regex must reach `pattern` with its backslashes intact.
    auto out = zxconfig::decode_escapes(R"(^\d+\.\d+\.\d+$)");
    EXPECT_TRUE(out.ok());
    EXPECT_EQ(out.value, R"(^\d+\.\d+\.\d+$)");

    auto typo = zxconfig::decode_escapes("a\\qb");
    EXPECT_TRUE(typo.ok());
    EXPECT_EQ(typo.value, "a\\qb");
}

TEST(DecodeEscapes, rejects_a_trailing_escape) {
    EXPECT_FALSE(zxconfig::decode_escapes("trailing\\").ok());
}
