// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/lexer.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using zxconfig::TokenKind;

namespace {

zxconfig::LexResult scan(std::string_view text) {
    zxconfig::SourceArena arena;
    auto id = arena.add("t.zx", std::string{text});
    EXPECT_TRUE(id.has_value());
    return zxconfig::lex(arena, *id);
}

}  // namespace

TEST(Keywords, table_is_sorted_and_complete) {
    const auto table = zxconfig::keywords();
    EXPECT_TRUE(std::is_sorted(table.begin(), table.end(),
        [](const zxconfig::Keyword& a, const zxconfig::Keyword& b) {
            return a.spelling < b.spelling;
        }));

    std::vector<TokenKind> seen;
    for (const auto& keyword : table) {
        EXPECT_EQ(zxconfig::keyword_spelling(keyword.kind), keyword.spelling);
        seen.push_back(keyword.kind);
    }
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());

    for (auto kind = TokenKind::KwZxconfig; kind <= TokenKind::KwNot;
         kind = static_cast<TokenKind>(static_cast<std::uint16_t>(kind) + 1)) {
        EXPECT_FALSE(zxconfig::keyword_spelling(kind).empty())
            << "keyword kind " << static_cast<unsigned>(kind) << " has no table entry";
    }
}

TEST(Lexer, classifies_keywords_and_identifiers) {
    auto result = scan("option SMP : bool");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.tokens.size(), 5u);

    EXPECT_EQ(result.tokens[0].kind, TokenKind::KwOption);
    EXPECT_TRUE(result.tokens[0].is_keyword());
    EXPECT_EQ(result.tokens[1].kind, TokenKind::Id);
    EXPECT_EQ(result.spelling(result.tokens[1]), "SMP");
    EXPECT_EQ(result.tokens[2].kind, TokenKind::Colon);
    EXPECT_EQ(result.tokens[3].kind, TokenKind::KwBool);
    EXPECT_EQ(result.tokens[4].kind, TokenKind::End);
}

TEST(Lexer, spans_point_at_the_source) {
    auto result = scan("menu \"Memory\"");
    ASSERT_TRUE(result.ok());

    EXPECT_EQ(result.tokens[0].span.begin.offset, 0u);
    EXPECT_EQ(result.tokens[0].span.end.offset, 4u);
    EXPECT_EQ(result.tokens[1].span.begin.offset, 5u);
    EXPECT_EQ(result.spelling(result.tokens[1]), "Memory");
}

TEST(Lexer, decodes_string_escapes) {
    auto result = scan(R"("a\n\t\"\\b")");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.tokens[0].kind, TokenKind::String);
    EXPECT_EQ(result.spelling(result.tokens[0]), "a\n\t\"\\b");
}

TEST(Lexer, decodes_unicode_escapes) {
    auto result = scan(R"("\u{03C0}")");
    ASSERT_TRUE(result.ok()) << result.errors.front().diagnostic.message;
    EXPECT_EQ(result.spelling(result.tokens[0]), "π");
}

TEST(Lexer, rejects_a_bad_unicode_escape) {
    auto result = scan(R"("\u{D800}")");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.errors.front().diagnostic.code, zxconfig::ErrorCode::BadEscape);
}

TEST(Lexer, reads_numbers_and_hex) {
    auto result = scan("4096 0xFF 0x0");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.tokens[0].kind, TokenKind::Number);
    EXPECT_EQ(result.spelling(result.tokens[0]), "4096");
    EXPECT_EQ(result.tokens[1].kind, TokenKind::Hex);
    EXPECT_EQ(result.spelling(result.tokens[1]), "0xFF");
    EXPECT_EQ(result.tokens[2].kind, TokenKind::Hex);
}

TEST(Lexer, rejects_a_trailing_identifier_on_a_number) {
    auto result = scan("12abc");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.errors.front().diagnostic.code, zxconfig::ErrorCode::BadNumber);
}

TEST(Lexer, rejects_a_hex_literal_without_digits) {
    auto result = scan("0x ");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.errors.front().diagnostic.code, zxconfig::ErrorCode::BadNumber);
}

TEST(Lexer, forms_two_character_operators) {
    auto result = scan("== != <= >= && || << >> -> ..");
    ASSERT_TRUE(result.ok());
    const TokenKind expected[] = {
        TokenKind::EqEq, TokenKind::NotEq, TokenKind::LtEq, TokenKind::GtEq,
        TokenKind::AmpAmp, TokenKind::PipePipe, TokenKind::Shl, TokenKind::Shr,
        TokenKind::Arrow, TokenKind::DotDot, TokenKind::End,
    };
    ASSERT_EQ(result.tokens.size(), std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        EXPECT_EQ(result.tokens[i].kind, expected[i]);
    }
}

TEST(Lexer, discards_comments) {
    auto result = scan("option # trailing\nSMP");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.tokens[0].kind, TokenKind::KwOption);
    EXPECT_EQ(result.tokens[1].kind, TokenKind::Id);
    EXPECT_EQ(result.spelling(result.tokens[1]), "SMP");
}

TEST(Lexer, reports_an_unterminated_string) {
    auto result = scan("\"never closed");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.errors.front().diagnostic.code, zxconfig::ErrorCode::UnterminatedString);
    EXPECT_EQ(result.errors.front().diagnostic.span.begin.offset, 0u);
}

TEST(Lexer, reports_an_unexpected_character_and_continues) {
    auto result = scan("a @ b");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.errors.front().diagnostic.code, zxconfig::ErrorCode::UnexpectedCharacter);
    EXPECT_EQ(result.tokens.front().kind, TokenKind::Id);
    EXPECT_EQ(result.tokens.back().kind, TokenKind::End);
}

TEST(Lexer, lexes_a_declaration) {
    auto result = scan(
        "zxconfig 1.0\n"
        "option NR_CPUS : int = 4 {\n"
        "    range 1 .. 512\n"
        "    when SMP\n"
        "}\n");
    ASSERT_TRUE(result.ok()) << result.errors.front().diagnostic.message;

    EXPECT_EQ(result.tokens[0].kind, TokenKind::KwZxconfig);
    EXPECT_EQ(result.tokens[1].kind, TokenKind::Number);
    EXPECT_EQ(result.tokens[2].kind, TokenKind::Dot);
    EXPECT_EQ(result.tokens[3].kind, TokenKind::Number);
    EXPECT_EQ(result.tokens[4].kind, TokenKind::KwOption);
    EXPECT_EQ(result.tokens[8].kind, TokenKind::Eq);
    EXPECT_EQ(result.tokens[11].kind, TokenKind::KwRange);
    EXPECT_EQ(result.tokens[13].kind, TokenKind::DotDot);
}
