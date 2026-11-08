// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/ast.hpp>
#include <zxconfig/cache.hpp>
#include <zxconfig/error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace zxconfig {

/// @brief A resolved value.
using EvalValue = std::variant<std::monostate,
                               bool,
                               std::int64_t,
                               std::string,
                               std::vector<std::string>>;

/// @brief Render @p value the way @p type is spelled in cache files and the
///        interface: y/m/n for tristates, 0x… for hex, comma-joined sets.
///        A monostate renders as <unset> regardless of type.
[[nodiscard]] std::string render_value(const EvalValue& value, ValueType type);

/// @brief Convert text from the cache or the interface into a typed value.
/// @param[in] text     The text.  Sets and lists are comma-separated.
/// @param[in] type     The declared type of the destination symbol.
/// @param[in] choices  The symbol's declared choices; only consulted for
///                     enum and set.
/// @retval TypeMismatch   when the text is not a legal literal of @p type
/// @retval UnknownChoice  when an enum or set item is not in @p choices
[[nodiscard]] Result<EvalValue> coerce(std::string_view text,
                                       ValueType type,
                                       const std::vector<std::string>& choices);

/// @brief Boolean strength of a value under tristate semantics.
[[nodiscard]] std::int64_t truth_level(const EvalValue& value);

/// @brief A candidate fix for an unsatisfied `implies`.
///
/// Several fixes on one implication are alternatives: applying any one of
/// them satisfies it (the spec's "user picks one if neither is set").
struct ImplicationFix {
    std::string symbol;
    std::string value;
    std::string description;
};

/// @brief An unsatisfied implication awaiting a decision.
struct Implication {
    std::string source;                ///< symbol whose implies is open
    std::string message;               ///< prompt text: the conflict's
                                       ///< resolve message, or a synthesized
                                       ///< question
    std::vector<ImplicationFix> fixes; ///< alternatives; empty = acknowledge only
    bool blocks_save{false};           ///< a conflict with the error policy
};

/// @brief The resolved configuration.
/// @note Not thread-safe.  The interface drives it from one thread.
class Runtime {
public:
    /// @param[in] document     Parsed configuration.  Must outlive the runtime.
    /// @param[in] diagnostics  Evaluation findings beyond value failures;
    ///                         may be null.  Expression failures are returned,
    ///                         not stored.
    explicit Runtime(const Document& document,
                     std::vector<Diagnostic>* diagnostics = nullptr);

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// @brief Resolve every symbol: defaults, then visibility.
    /// @warning Invalidates every previously returned value reference.
    [[nodiscard]] Result<void> evaluate();

    /// @brief Run the `validate` and `warn` clauses against current values.
    [[nodiscard]] std::vector<Diagnostic> run_validation() const;

    /// @brief Set a symbol to @p text, validating against its declaration.
    /// @param[in] name    Symbol name as registered (namespaces included).
    /// @param[in] text    The new value in its textual form.
    /// @param[in] claimed Type recorded by the cache, when applying one.
    ///                    Compared against the declaration; a mismatch is a
    ///                    recorded warning, and the declaration wins.
    [[nodiscard]] Result<void> set_user_value(std::string_view name,
                                              std::string_view text,
                                              std::optional<ValueType> claimed = std::nullopt);

    /// @brief Drop the user value; the symbol returns to its default.
    [[nodiscard]] Result<void> reset_symbol(std::string_view name);

    /// @brief Apply one cache assignment.  Unknown symbols are refused; the
    ///        reconciliation step should have filtered them already.
    [[nodiscard]] Result<void> apply_assignment(std::string_view name,
                                                std::string_view value,
                                                ValueType claimed);

    /// @brief The current value of @p name, or nullptr when undeclared.
    [[nodiscard]] const EvalValue* value(std::string_view name) const;

    /// @brief The value the declaration alone produces, or nullptr.
    [[nodiscard]] const EvalValue* default_value(std::string_view name) const;

    /// @brief Whether @p name was set by the user or by a cache assignment.
    [[nodiscard]] bool is_user_set(std::string_view name) const;

    /// @brief Whether @p name is visible, menu chain included.
    [[nodiscard]] bool is_visible(std::string_view name) const;

    /// @brief Whether @p name may be edited.  Computed symbols never are.
    [[nodiscard]] bool is_enabled(std::string_view name) const;

    /// @brief Resolved visibility of a menu node.
    [[nodiscard]] bool is_menu_visible(const Menu& menu) const;

    /// @brief The declaration of @p name, or nullptr when undeclared.
    ///
    /// An alias resolves to its target: with `alias A = B`, symbol("A")
    /// returns B's declaration, through a bounded chain.
    [[nodiscard]] const Symbol* symbol(std::string_view name) const;

    /// @brief Symbols with user values, in declaration-independent order.
    [[nodiscard]] std::vector<std::string> user_symbols() const;

    /// @brief Every declared symbol, sorted by name.  Deterministic order is
    ///        what keeps generated files free of spurious diffs.
    [[nodiscard]] std::vector<const Symbol*> declared_symbols() const;

    /// @brief Render the user-set symbols as a cache document.
    [[nodiscard]] CacheDocument to_cache(std::string_view manifest,
                                         std::string_view digest) const;

    /// @brief Forget memoized shell results, so the next evaluation re-runs
    ///        the probes.
    void invalidate_probes() noexcept;

    /// @brief Implications whose clauses are unsatisfied while their source
    ///        is on.  Computed from current values; the caller decides how to
    ///        surface them (prompt, warning, save refusal).
    [[nodiscard]] std::vector<Implication> unsatisfied_implications() const;

private:
    struct Scope {
        const Symbol* current{nullptr};
        std::vector<const Symbol*> stack;
    };

    [[nodiscard]] Result<EvalValue> evaluate_symbol(const Symbol& symbol, Scope& scope) const;
    [[nodiscard]] Result<EvalValue> evaluate_expr(const Expr& expr, Scope& scope) const;
    [[nodiscard]] Result<EvalValue> evaluate_call(const Expr& call, Scope& scope) const;
    [[nodiscard]] Result<EvalValue> run_probe(const std::string& kind,
                                              const std::string& argument) const;
    [[nodiscard]] Result<EvalValue> evaluate_default(const Symbol& symbol, Scope& scope) const;

    /// @brief Expand `${NAME}` inside a string value.
    ///
    /// Names resolve like identifiers — namespace-qualified first, then the
    /// alias chain — and substitute the target's canonical value.  Macro
    /// parameters are already gone by the time a string reaches here; what
    /// remains names symbols.  An unknown name is an Interpolation failure,
    /// an unset target is UndefinedValue.
    [[nodiscard]] Result<EvalValue> interpolate(std::string_view text,
                                                Scope& scope,
                                                SourceSpan span) const;
    void resolve_visibility(const Menu& menu, bool parent_visible);

    const Document& document_;
    std::vector<Diagnostic>* diagnostics_;

    std::unordered_map<std::string, EvalValue> user_values_;
    mutable std::unordered_map<std::string, EvalValue> values_;
    mutable std::unordered_map<std::string, EvalValue> default_values_;
    std::unordered_map<std::string, bool> visibility_;
    std::unordered_map<const Menu*, bool> menu_visibility_;
    std::unordered_set<std::string> user_set_;
    mutable std::unordered_map<std::string, EvalValue> probe_cache_;
};

}  // namespace zxconfig
