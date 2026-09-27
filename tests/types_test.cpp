// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/types.hpp>

#include <gtest/gtest.h>

/// The contract that keeps the cache and the language on one vocabulary: the
/// spellings in types.hpp must be exactly the lexer's keyword spellings, and
/// the mapping must round-trip.
TEST(ValueType, spellings_are_the_language_keywords) {
    const std::pair<zxconfig::TokenKind, zxconfig::ValueType> pairs[] = {
        {zxconfig::TokenKind::KwBool,     zxconfig::ValueType::Bool},
        {zxconfig::TokenKind::KwTristate, zxconfig::ValueType::Tristate},
        {zxconfig::TokenKind::KwInt,      zxconfig::ValueType::Int},
        {zxconfig::TokenKind::KwUint,     zxconfig::ValueType::Uint},
        {zxconfig::TokenKind::KwHex,      zxconfig::ValueType::Hex},
        {zxconfig::TokenKind::KwString,   zxconfig::ValueType::String},
        {zxconfig::TokenKind::KwPath,     zxconfig::ValueType::Path},
        {zxconfig::TokenKind::KwEnum,     zxconfig::ValueType::Enum},
        {zxconfig::TokenKind::KwSet,      zxconfig::ValueType::Set},
        {zxconfig::TokenKind::KwList,     zxconfig::ValueType::List},
    };

    for (const auto& [kind, type] : pairs) {
        const auto spelling = zxconfig::keyword_spelling(kind);
        EXPECT_FALSE(spelling.empty()) << static_cast<unsigned>(kind);

        auto parsed = zxconfig::parse_value_type(spelling);
        ASSERT_TRUE(parsed.has_value()) << spelling;
        EXPECT_EQ(*parsed, type) << spelling;

        EXPECT_STREQ(zxconfig::to_string(type), spelling.data()) << spelling;
    }
}

TEST(ValueType, rejects_unknown_spellings) {
    EXPECT_FALSE(zxconfig::parse_value_type("").has_value());
    EXPECT_FALSE(zxconfig::parse_value_type("boolean").has_value());
    EXPECT_FALSE(zxconfig::parse_value_type("Bool").has_value());
    EXPECT_FALSE(zxconfig::parse_value_type("ident").has_value());
}
