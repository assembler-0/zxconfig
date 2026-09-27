// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/lexer.hpp>
#include <zxconfig/text.hpp>

#include <algorithm>

namespace zxconfig {
namespace {

TokenKind classify(std::string_view spelling) noexcept {
    const auto table = keywords();
    const auto found = std::lower_bound(
        table.begin(), table.end(), spelling,
        [](const Keyword& entry, std::string_view key) { return entry.spelling < key; });
    if (found != table.end() && found->spelling == spelling) return found->kind;
    return TokenKind::Id;
}

bool is_ident_start(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_ident_cont(char c) noexcept {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool is_hex(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

struct detail::Scanner {
    explicit Scanner(const SourceFile& file, std::string& strings)
        : file_{file}, text_{file.text}, strings_{strings} {}

    LexResult run() {
        while (!done()) {
            skip_whitespace_and_comments();
            if (done()) break;
            scan_one();
        }
        Token end;
        end.kind = TokenKind::End;
        end.span = SourceSpan::point(at());
        result_.tokens.push_back(end);
        return std::move(result_);
    }

private:
    bool done() const noexcept { return pos_ >= text_.size(); }

    char peek(std::size_t ahead = 0) const noexcept {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }

    SourceLocation at(std::size_t offset) const noexcept {
        return {file_.id, static_cast<std::uint32_t>(offset)};
    }

    SourceLocation at() const noexcept { return at(pos_); }

    void fail(ErrorCode code, std::string message, std::size_t begin, std::size_t end) {
        SourceSpan span;
        span.begin = at(begin);
        span.end = at(end);
        result_.errors.push_back(LexError{Diagnostic::error(code, std::move(message), span)});
    }

    void skip_whitespace_and_comments() {
        for (;;) {
            while (!done() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
                ++pos_;
            }
            if (peek() != '#') return;
            while (!done() && peek() != '\n') ++pos_;
        }
    }

    void scan_one() {
        const auto begin = pos_;
        const char c = peek();

        if (is_ident_start(c)) return scan_word(begin);
        if (is_digit(c)) return scan_number(begin);
        if (c == '"') return scan_string(begin);

        ++pos_;
        TokenKind kind = TokenKind::End;
        switch (c) {
            case '{': kind = TokenKind::LBrace; break;
            case '}': kind = TokenKind::RBrace; break;
            case '[': kind = TokenKind::LBracket; break;
            case ']': kind = TokenKind::RBracket; break;
            case '(': kind = TokenKind::LParen; break;
            case ')': kind = TokenKind::RParen; break;
            case ':': kind = TokenKind::Colon; break;
            case ',': kind = TokenKind::Comma; break;
            case ';': kind = TokenKind::Semicolon; break;
            case '~': kind = TokenKind::Tilde; break;
            case '?': kind = TokenKind::Question; break;
            case '$': kind = TokenKind::Dollar; break;
            case '+': kind = TokenKind::Plus; break;
            case '*': kind = TokenKind::Star; break;
            case '%': kind = TokenKind::Percent; break;
            case '^': kind = TokenKind::Caret; break;
            case '.':
                if (peek() == '.') { ++pos_; kind = TokenKind::DotDot; }
                else kind = TokenKind::Dot;
                break;
            case '=':
                if (peek() == '=') { ++pos_; kind = TokenKind::EqEq; }
                else kind = TokenKind::Eq;
                break;
            case '!':
                if (peek() == '=') { ++pos_; kind = TokenKind::NotEq; }
                else kind = TokenKind::Bang;
                break;
            case '<':
                if (peek() == '=') { ++pos_; kind = TokenKind::LtEq; }
                else if (peek() == '<') { ++pos_; kind = TokenKind::Shl; }
                else kind = TokenKind::Lt;
                break;
            case '>':
                if (peek() == '=') { ++pos_; kind = TokenKind::GtEq; }
                else if (peek() == '>') { ++pos_; kind = TokenKind::Shr; }
                else kind = TokenKind::Gt;
                break;
            case '&':
                if (peek() == '&') { ++pos_; kind = TokenKind::AmpAmp; }
                else kind = TokenKind::Amp;
                break;
            case '|':
                if (peek() == '|') { ++pos_; kind = TokenKind::PipePipe; }
                else kind = TokenKind::Pipe;
                break;
            case '-':
                if (peek() == '>') { ++pos_; kind = TokenKind::Arrow; }
                else kind = TokenKind::Minus;
                break;
            case '/': kind = TokenKind::Slash; break;
            default:
                fail(ErrorCode::UnexpectedCharacter,
                     std::string{"unexpected character '"} + c + "'", begin, pos_);
                return;
        }
        emit(kind, begin, text_.substr(begin, pos_ - begin));
    }

    void scan_word(std::size_t begin) {
        while (is_ident_cont(peek())) ++pos_;
        const auto spelling = text_.substr(begin, pos_ - begin);
        emit(classify(spelling), begin, spelling);
    }

    void scan_number(std::size_t begin) {
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            pos_ += 2;
            const auto digits = pos_;
            while (is_hex(peek())) ++pos_;
            if (pos_ == digits) {
                fail(ErrorCode::BadNumber, "hexadecimal literal has no digits", begin, pos_);
                return;
            }
            emit(TokenKind::Hex, begin, text_.substr(begin, pos_ - begin));
            return;
        }

        while (is_digit(peek())) ++pos_;
        if (is_ident_start(peek())) {
            while (is_ident_cont(peek())) ++pos_;
            fail(ErrorCode::BadNumber, "invalid digit in number", begin, pos_);
            return;
        }
        emit(TokenKind::Number, begin, text_.substr(begin, pos_ - begin));
    }

    void scan_string(std::size_t begin) {
        ++pos_;
        // Find the closing quote without interpreting escapes.  A backslash
        // consumes itself and the next byte, so \" cannot close the string.
        while (!done() && peek() != '"' && peek() != '\n') {
            pos_ += peek() == '\\' ? 2 : 1;
        }

        if (peek() != '"') {
            fail(ErrorCode::UnterminatedString, "unterminated string", begin, pos_);
            return;
        }
        const auto body = std::string_view{text_}.substr(begin + 1, pos_ - begin - 1);
        ++pos_;

        auto decoded = decode_escapes(body);
        if (!decoded.ok()) {
            const auto escape = begin + 1 + *decoded.error;
            fail(ErrorCode::BadEscape, "invalid string escape", escape, escape + 1);
            return;
        }

        strings_.push_back('\0');
        const auto written = static_cast<std::uint32_t>(strings_.size());
        strings_.append(decoded.value);

        Token token;
        token.kind = TokenKind::String;
        token.span = {at(begin), at(pos_)};
        token.text_offset = written;
        token.text_length = static_cast<std::uint32_t>(strings_.size() - written);
        result_.tokens.push_back(token);
    }

    void emit(TokenKind kind, std::size_t begin, std::string_view spelling) {
        Token token;
        token.kind = kind;
        token.span = {at(begin), at(pos_)};
        token.text_offset = static_cast<std::uint32_t>(begin);
        token.text_length = static_cast<std::uint32_t>(spelling.size());
        result_.tokens.push_back(token);
    }

    const SourceFile& file_;
    const std::string& text_;
    std::string& strings_;
    std::size_t pos_{0};
    LexResult result_;
};

std::string_view LexResult::spelling(const Token& token) const noexcept {
    const auto& buffer = token.kind == TokenKind::String ? strings_ : source_;
    if (static_cast<std::size_t>(token.text_offset) + token.text_length > buffer.size()) return {};
    return std::string_view{buffer}.substr(token.text_offset, token.text_length);
}

LexResult lex(const SourceArena& arena, std::uint32_t file_id) {
    const auto* file = arena.find(file_id);
    if (file == nullptr) {
        LexResult result;
        result.errors.push_back(LexError{Diagnostic::error(
            ErrorCode::Invariant, "lexer given an unknown file id")});
        return result;
    }

    LexResult scanned;
    scanned.source_ = file->text;
    detail::Scanner scanner{*file, scanned.strings_};
    auto produced = scanner.run();
    scanned.tokens = std::move(produced.tokens);
    scanned.errors = std::move(produced.errors);
    return scanned;
}

}  // namespace zxconfig
