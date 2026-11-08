// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/ast.hpp>
#include <zxconfig/error.hpp>
#include <zxconfig/eval.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zxconfig {

/// @brief The macro name of one symbol under @p prefix.
/// @param[in] symbol  The symbol.  Its declaration spelling is preserved.
/// @param[in] prefix  Explicit prefix, e.g. "CONFIG_".  Empty by default.
[[nodiscard]] std::string macro_name(const Symbol& symbol, std::string_view prefix);

/// @brief The macro name of one choice or set member of @p symbol.
[[nodiscard]] std::string macro_name(const Symbol& symbol,
                                     std::string_view prefix,
                                     std::string_view item);

/// @brief Render one header directive to text, or refuse on a name clash.
/// @param[in] runtime    Resolved values to emit.  Borrows its document.
/// @param[in] directive  Which backend and where to write; only Header is
///                       accepted here.
/// @param[in] engine     Engine version for the file banner.
///
/// @retval UnknownBackend      when @p directive is not a header backend
/// @retval NamespaceCollision  when two symbols map to one macro name
[[nodiscard]] Result<std::string> render_header(const Runtime& runtime,
                                                const GenerateDirective& directive,
                                                std::string_view engine);

/// @brief Render and write one header directive to its output path.
/// @param[in] base       Directory relative outputs resolve against.
/// @retval WriteFailed  when the file cannot be created or written
[[nodiscard]] Result<void> generate_header(const Runtime& runtime,
                                           const GenerateDirective& directive,
                                           std::string_view engine,
                                           std::string_view base = {});

/// @brief Render one makefile directive to text.
[[nodiscard]] Result<std::string> render_makefile(const Runtime& runtime,
                                                  std::string_view engine);

/// @brief Render one json directive to text.  Keys are sorted.
[[nodiscard]] Result<std::string> render_json(const Runtime& runtime,
                                              std::string_view engine);

/// @brief Render one cmake directive to text.
[[nodiscard]] Result<std::string> render_cmake(const Runtime& runtime,
                                               std::string_view engine);

/// @brief Render a template body with `${SYMBOL}` interpolation.
/// @param[in] runtime         Resolved values to interpolate.
/// @param[in] template_text   The template file's contents.
[[nodiscard]] Result<std::string> render_template(const Runtime& runtime,
                                                  std::string_view template_text);

/// @brief Run every directive of @p document.  Each failure is returned in
///        @p failures with the directive that produced it; the call itself
///        keeps going so one bad output does not hide the rest.
///
/// @param[in]  document   The parsed configuration.
/// @param[in]  runtime    Resolved values to emit.
/// @param[in]  engine     Engine version for banners.
/// @param[in]  base       Directory relative outputs resolve against.
/// @param[out] failures   Collects one message per failed directive.
/// @return The number of directives that succeeded.
[[nodiscard]] std::size_t run_generators(const Document& document,
                                         const Runtime& runtime,
                                         std::string_view engine,
                                         std::string_view base,
                                         std::vector<std::string>& failures);

}  // namespace zxconfig
