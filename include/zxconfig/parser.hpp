// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/ast.hpp>
#include <zxconfig/error.hpp>
#include <zxconfig/source.hpp>

#include <string_view>
#include <vector>

namespace zxconfig {

/// @brief Outcome of parsing one configuration tree.
struct ParseResult {
    Document document{};
    std::vector<Diagnostic> diagnostics{};

    /// @brief True when no diagnostic has Error or Fatal severity.
    [[nodiscard]] bool ok() const noexcept;
};

/// @brief Parse the file @p file_id and everything it includes.
/// @param[in,out] arena    Arena holding the file to parse.  Includes are
///                         loaded into it, extending it.
/// @param[in] file_id      File to start from.
/// @param[in] root         Directory includes may not escape.  Empty disables
///                         the check; pass the project root in real runs.
[[nodiscard]] ParseResult parse_document(SourceArena& arena,
                                         std::uint32_t file_id,
                                         std::string_view root = {});

}  // namespace zxconfig
