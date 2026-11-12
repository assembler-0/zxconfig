// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/error.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace zxconfig {

/// @brief Lexical class of one token.
enum class TokenKind : std::uint16_t {
    End = 0,

    Id = 1,
    String,   ///< "..." with escapes resolved by the lexer
    Number,   ///< decimal integer
    Hex,      ///< 0x... integer

    KwZxconfig,
    KwOption,
    KwComputed,
    KwMenu,
    KwChoice,
    KwDeprecated,
    KwAlias,
    KwInclude,
    KwGenerate,
    KwMacro,
    KwExport,
    KwExportWithPrefix,
    KwPresetsDir,
    KwValidate,
    KwWarn,
    KwError,
    KwConflict,
    KwBool,
    KwTristate,
    KwInt,
    KwUint,
    KwHex,
    KwString,
    KwPath,
    KwEnum,
    KwSet,
    KwList,
    KwLabel,
    KwHelp,
    KwTags,
    KwRange,
    KwWhen,
    KwVisible,
    KwEnabled,
    KwImplies,
    KwDefault,
    KwOrder,
    KwPattern,
    KwChoices,
    KwResolve,
    KwMessage,
    KwFrom,
    KwFallback,
    KwMustExist,
    KwIsDir,
    KwNamespace,
    KwHeader,
    KwMakefile,
    KwJson,
    KwCmake,
    KwTemplate,
    KwAnd,
    KwOr,
    KwNot,

    LBrace,     ///< {
    RBrace,     ///< }
    LBracket,   ///< [
    RBracket,   ///< ]
    LParen,     ///< (
    RParen,     ///< )
    Colon,      ///< :
    Comma,      ///< ,
    Semicolon,  ///< ;
    Dot,        ///< .
    DotDot,     ///< ..
    Eq,         ///< =
    EqEq,       ///< ==
    NotEq,      ///< !=
    Lt,         ///< <
    LtEq,       ///< <=
    Gt,         ///< >
    GtEq,       ///< >=
    Plus,       ///< +
    Minus,      ///< -
    Star,       ///< *
    Slash,      ///< /
    Percent,    ///< %
    Amp,        ///< &
    AmpAmp,     ///< &&
    Pipe,       ///< |
    PipePipe,   ///< ||
    Caret,      ///< ^
    Tilde,      ///< ~
    Bang,       ///< !
    Question,   ///< ?
    Shl,        ///< <<
    Shr,        ///< >>
    Arrow,      ///< ->
    Dollar,     ///< $
};

/// @brief One token.
struct Token {
    TokenKind kind{TokenKind::End};
    SourceSpan span{};
    std::uint32_t text_offset{0};  ///< offset into the source, or into the string table
    std::uint32_t text_length{0};  ///< byte length of the spelling

    [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
    [[nodiscard]] bool is_keyword() const noexcept;
};

/// @brief One keyword: its spelling and the kind it classifies as.
struct Keyword {
    std::string_view spelling;
    TokenKind kind;
};

/// @brief Every keyword, sorted by spelling.
[[nodiscard]] std::span<const Keyword> keywords() noexcept;

/// @brief Stable spelling of a keyword, or an empty view for anything else.
[[nodiscard]] std::string_view keyword_spelling(TokenKind kind) noexcept;

/// @brief Stable name of a kind, for diagnostics.  Never null.
[[nodiscard]] const char* to_string(TokenKind kind) noexcept;

}  // namespace zxconfig
