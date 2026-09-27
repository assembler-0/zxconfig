// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/source.hpp>
#include <zxconfig/token.hpp>

#include <string>
#include <vector>

namespace zxconfig {

namespace detail { struct Scanner; }

/// @brief One lexical failure, with the span that caused it.
struct LexError {
    Diagnostic diagnostic;
};

/// @brief Outcome of lexing one file.
struct LexResult {
    std::vector<Token> tokens;
    std::vector<LexError> errors;

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }

    /// @brief Source this result was scanned from.
    [[nodiscard]] std::string_view source() const noexcept { return source_; }

    /// @brief The spelling of @p token.
    /// @param[in] token  A token produced by the scan that built this result.
    [[nodiscard]] std::string_view spelling(const Token& token) const noexcept;

private:
    friend LexResult lex(const SourceArena& arena, std::uint32_t file_id);
    friend struct detail::Scanner;

    std::string source_;   ///< raw text; identifier and number spellings index it
    std::string strings_;  ///< decoded string literals, NUL-separated
};

/// @brief Scan one loaded file into tokens.
/// @param[in] arena    Arena the file was loaded into.
/// @param[in] file_id  File to scan.
/// @return Tokens and every lexical error found.  The result owns its source
///         copy, so it outlives the arena.
/// @note String tokens carry the decoded text, with escapes resolved, and
///       their span covers the raw spelling including quotes.
[[nodiscard]] LexResult lex(const SourceArena& arena, std::uint32_t file_id);

}  // namespace zxconfig
