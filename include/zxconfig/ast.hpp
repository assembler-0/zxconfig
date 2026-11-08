// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/error.hpp>
#include <zxconfig/source.hpp>
#include <zxconfig/token.hpp>
#include <zxconfig/types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace zxconfig {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

/// @brief Class of an expression node.
enum class ExprKind : std::uint8_t {
    Invalid,
    BoolLit,     ///< number holds 0 or 1
    IntLit,      ///< number holds the value; Hex literals land here too
    StringLit,   ///< text holds the decoded value
    Ident,       ///< text holds the name as written
    SequenceLit, ///< set or list literal; args hold the items
    Unary,       ///< op is the operator; args holds one operand
    Binary,      ///< op is the operator; args holds lhs, rhs
    Ternary,     ///< args holds condition, then, else
    Call,        ///< text is the builtin name; args holds the operands
    Fallback,    ///< args holds a probe call and its fallback value
};

/// @brief One node of an expression tree.
struct Expr {
    ExprKind kind{ExprKind::Invalid};
    SourceSpan span{};
    Token op{};
    std::string text{};
    std::int64_t number{0};
    std::vector<ExprPtr> args{};

    [[nodiscard]] bool is(ExprKind k) const noexcept { return kind == k; }
};

/// @brief Deep-copy an expression tree.
[[nodiscard]] ExprPtr clone_expr(const Expr& expr);

/// @brief What happens when two `implies` targets contradict each other.
enum class ConflictPolicy : std::uint8_t {
    Prompt,  ///< ask the operator; the default
    Error,   ///< refuse to save
};

/// @brief An inclusive numeric constraint.
struct Range {
    std::int64_t lo{0};
    std::int64_t hi{0};
};

/// @brief One arm of a conditional default.  A null condition is the fallback.
struct DefaultCase {
    ExprPtr value;
    ExprPtr condition;
};

/// @brief Presentation and behavior for one choice of an enum or set.
struct ChoiceMeta {
    std::string name;
    std::string label;
    std::string help;
    ExprPtr visible;
};

/// @brief A declared contradiction between a symbol and another value.
struct ConflictClause {
    std::string symbol;
    std::string message;
    ConflictPolicy policy{ConflictPolicy::Prompt};
};

/// @brief An advisory condition: reported when it fails, never blocking.
struct WarnClause {
    std::string message;
    ExprPtr condition;
};

/// @brief Everything an option or computed symbol carries besides its name.
struct Metadata {
    std::string label;
    std::string help;
    std::vector<std::string> tags;
    std::optional<std::int64_t> order;
    std::optional<Range> range;
    std::string pattern;
    std::vector<std::string> path_constraints;
    std::vector<std::string> choices;
    std::vector<ChoiceMeta> choice_meta;
    ExprPtr when;      ///< shorthand for visible, kept separate so the TUI can
                       ///< render the two forms differently
    ExprPtr visible;
    ExprPtr enabled;
    std::vector<ExprPtr> implies;
    std::vector<DefaultCase> defaults;
    std::vector<ConflictClause> conflicts;
    std::vector<WarnClause> warnings;
};

/// @brief A configurable or computed symbol.
struct Symbol {
    std::string name;        ///< namespaced final name
    std::string raw_name;    ///< as written in the source
    std::string namespace_;  ///< namespace declared by the defining file
    ValueType type{ValueType::Bool};
    ValueType element{ValueType::String};  ///< list element type
    bool computed{false};
    SourceSpan span{};
    std::uint32_t file_id{0};
    Metadata meta{};
};

struct Menu;
using MenuPtr = std::unique_ptr<Menu>;

/// @brief A nesting level of the configuration tree.
struct Menu {
    std::string label;
    ExprPtr visible;
    SourceSpan span{};
    std::uint32_t file_id{0};
    std::vector<std::variant<Symbol, MenuPtr>> children{};
};

/// @brief Record of one include directive, for the include-tree panel.
struct IncludeDirective {
    std::string path;          ///< as written
    std::string resolved;      ///< canonical path; empty when skipped
    std::uint32_t file_id{0};  ///< 0 when skipped
    std::string namespace_;
    ExprPtr condition;         ///< visible clause; null when unconditional
    SourceSpan span{};
};

/// @brief One output the engine is asked to produce.
struct GenerateDirective {
    enum class Backend : std::uint8_t { Header, Makefile, Json, Cmake, Template };
    Backend backend{Backend::Header};
    std::string output;
    std::string template_path;  ///< for Backend::Template
    std::string prefix;         ///< optional explicit prefix for Backend::Header
    SourceSpan span{};
};

/// @brief A stable public alias for an internal name.
struct AliasDirective {
    std::string alias;
    std::string target;
    SourceSpan span{};
};

/// @brief A renamed or removed symbol, for migrating old configurations.
struct DeprecatedDirective {
    std::string old_name;
    std::string new_name;
    std::string message;
    SourceSpan span{};
};

/// @brief A cross-symbol constraint.  Hard when not a warning.
struct ValidationClause {
    std::string message;
    ExprPtr condition;
    bool warning{false};
    SourceSpan span{};
};

/// @brief A macro definition captured for text-level expansion.
struct Macro {
    std::string name;
    std::vector<std::string> params;
    std::string body;
    bool exported{false};  ///< visible to files that include the defining file
    SourceSpan span{};
};

/// @brief The parsed configuration: a tree plus everything the tree does not
///        hold, and the symbol index the TUI and the evaluator look up by.
struct Document {
    std::int64_t version_major{0};
    std::int64_t version_minor{0};
    MenuPtr root{std::make_unique<Menu>()};
    std::vector<IncludeDirective> includes;
    std::vector<ValidationClause> validations;
    std::vector<GenerateDirective> generates;
    std::vector<AliasDirective> aliases;
    std::vector<DeprecatedDirective> deprecated;
    std::vector<Macro> macros;
    std::unordered_map<std::string, Symbol*> symbols;
    std::unordered_map<std::uint32_t, std::string> file_namespaces;
};

/// @brief Size summary of a parsed document.
struct DocumentStats {
    std::size_t symbols{0};
    std::size_t menus{0};
};

/// @brief Indices of @p menu's children in presentation order.
///
/// The spec's `order` overrides declaration order in the interface: each
/// symbol sorts by its order value (menus and unordered symbols sort as 0),
/// and ties keep declaration order.
[[nodiscard]] std::vector<std::size_t> ordered_children(const Menu& menu);

/// @brief Count the symbols and menus of @p document.
[[nodiscard]] DocumentStats document_stats(const Document& document) noexcept;

}  // namespace zxconfig
