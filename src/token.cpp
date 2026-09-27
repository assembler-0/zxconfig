// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/token.hpp>

namespace zxconfig {
namespace {

constexpr Keyword k_keywords[] = {
    {"alias",      TokenKind::KwAlias},
    {"and",        TokenKind::KwAnd},
    {"as",         TokenKind::KwAs},
    {"bool",       TokenKind::KwBool},
    {"choice",     TokenKind::KwChoice},
    {"choices",    TokenKind::KwChoices},
    {"cmake",      TokenKind::KwCmake},
    {"computed",   TokenKind::KwComputed},
    {"conflict",   TokenKind::KwConflict},
    {"default",    TokenKind::KwDefault},
    {"deprecated", TokenKind::KwDeprecated},
    {"enabled",    TokenKind::KwEnabled},
    {"enum",       TokenKind::KwEnum},
    {"export",     TokenKind::KwExport},
    {"fallback",   TokenKind::KwFallback},
    {"from",       TokenKind::KwFrom},
    {"generate",   TokenKind::KwGenerate},
    {"header",     TokenKind::KwHeader},
    {"help",       TokenKind::KwHelp},
    {"hex",        TokenKind::KwHex},
    {"implies",    TokenKind::KwImplies},
    {"include",    TokenKind::KwInclude},
    {"int",        TokenKind::KwInt},
    {"is_dir",     TokenKind::KwIsDir},
    {"json",       TokenKind::KwJson},
    {"label",      TokenKind::KwLabel},
    {"list",       TokenKind::KwList},
    {"macro",      TokenKind::KwMacro},
    {"makefile",   TokenKind::KwMakefile},
    {"menu",       TokenKind::KwMenu},
    {"message",    TokenKind::KwMessage},
    {"must_exist", TokenKind::KwMustExist},
    {"not",        TokenKind::KwNot},
    {"option",     TokenKind::KwOption},
    {"or",         TokenKind::KwOr},
    {"order",      TokenKind::KwOrder},
    {"path",       TokenKind::KwPath},
    {"pattern",    TokenKind::KwPattern},
    {"range",      TokenKind::KwRange},
    {"resolve",    TokenKind::KwResolve},
    {"set",        TokenKind::KwSet},
    {"string",     TokenKind::KwString},
    {"tags",       TokenKind::KwTags},
    {"template",   TokenKind::KwTemplate},
    {"tristate",   TokenKind::KwTristate},
    {"uint",       TokenKind::KwUint},
    {"validate",   TokenKind::KwValidate},
    {"visible",    TokenKind::KwVisible},
    {"warn",       TokenKind::KwWarn},
    {"when",       TokenKind::KwWhen},
    {"zxconfig",    TokenKind::KwZxconfig},
};

}  // namespace

std::span<const Keyword> keywords() noexcept { return k_keywords; }

bool Token::is_keyword() const noexcept {
    return kind >= TokenKind::KwZxconfig && kind <= TokenKind::KwNot;
}

std::string_view keyword_spelling(TokenKind kind) noexcept {
    for (const auto& keyword : k_keywords) {
        if (keyword.kind == kind) return keyword.spelling;
    }
    return {};
}

const char* to_string(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::End:      return "end of file";
        case TokenKind::Id:       return "identifier";
        case TokenKind::String:   return "string";
        case TokenKind::Number:   return "number";
        case TokenKind::Hex:      return "hexadecimal number";
        case TokenKind::LBrace:   return "'{'";
        case TokenKind::RBrace:   return "'}'";
        case TokenKind::LBracket: return "'['";
        case TokenKind::RBracket: return "']'";
        case TokenKind::LParen:   return "'('";
        case TokenKind::RParen:   return "')'";
        case TokenKind::Colon:    return "':'";
        case TokenKind::Comma:    return "','";
        case TokenKind::Semicolon: return "';'";
        case TokenKind::Dot:      return "'.'";
        case TokenKind::DotDot:   return "'..'";
        case TokenKind::Eq:       return "'='";
        case TokenKind::EqEq:     return "'=='";
        case TokenKind::NotEq:    return "'!='";
        case TokenKind::Lt:       return "'<'";
        case TokenKind::LtEq:     return "'<='";
        case TokenKind::Gt:       return "'>'";
        case TokenKind::GtEq:     return "'>='";
        case TokenKind::Plus:     return "'+'";
        case TokenKind::Minus:    return "'-'";
        case TokenKind::Star:     return "'*'";
        case TokenKind::Slash:    return "'/'";
        case TokenKind::Percent:  return "'%'";
        case TokenKind::Amp:      return "'&'";
        case TokenKind::AmpAmp:   return "'&&'";
        case TokenKind::Pipe:     return "'|'";
        case TokenKind::PipePipe: return "'||'";
        case TokenKind::Caret:    return "'^'";
        case TokenKind::Tilde:    return "'~'";
        case TokenKind::Bang:     return "'!'";
        case TokenKind::Question: return "'?'";
        case TokenKind::Shl:      return "'<<'";
        case TokenKind::Shr:      return "'>>'";
        case TokenKind::Arrow:    return "'->'";
        case TokenKind::Dollar:   return "'$'";
        case TokenKind::KwZxconfig: case TokenKind::KwOption: case TokenKind::KwComputed:
        case TokenKind::KwMenu: case TokenKind::KwChoice: case TokenKind::KwDeprecated:
        case TokenKind::KwAlias: case TokenKind::KwInclude: case TokenKind::KwGenerate:
        case TokenKind::KwMacro: case TokenKind::KwExport: case TokenKind::KwValidate:
        case TokenKind::KwWarn: case TokenKind::KwConflict: case TokenKind::KwBool:
        case TokenKind::KwTristate: case TokenKind::KwInt: case TokenKind::KwUint:
        case TokenKind::KwHex: case TokenKind::KwString: case TokenKind::KwPath:
        case TokenKind::KwEnum: case TokenKind::KwSet: case TokenKind::KwList:
        case TokenKind::KwLabel: case TokenKind::KwHelp: case TokenKind::KwTags:
        case TokenKind::KwRange: case TokenKind::KwWhen: case TokenKind::KwVisible:
        case TokenKind::KwEnabled: case TokenKind::KwImplies: case TokenKind::KwDefault:
        case TokenKind::KwOrder: case TokenKind::KwPattern: case TokenKind::KwChoices:
        case TokenKind::KwResolve: case TokenKind::KwMessage: case TokenKind::KwFrom:
        case TokenKind::KwAs: case TokenKind::KwFallback: case TokenKind::KwMustExist:
        case TokenKind::KwIsDir: case TokenKind::KwHeader: case TokenKind::KwMakefile:
        case TokenKind::KwJson: case TokenKind::KwCmake: case TokenKind::KwTemplate:
        case TokenKind::KwAnd: case TokenKind::KwOr: case TokenKind::KwNot:
            break;
    }
    const auto spelling = keyword_spelling(kind);
    return spelling.empty() ? "token" : spelling.data();
}

}  // namespace zxconfig
