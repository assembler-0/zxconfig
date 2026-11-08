// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/eval.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <sys/utsname.h>

namespace zxconfig {
namespace {

/// @brief Split on commas and trim each part.  Empty parts are dropped, so
///        "a,,b" is the same set as "a,b" and "" is the empty set.
std::vector<std::string> split_items(std::string_view text) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto part = text.substr(start, comma == std::string_view::npos
                                                ? text.size() - start
                                                : comma - start);
        std::size_t lo = 0;
        std::size_t hi = part.size();
        while (lo < hi && (part[lo] == ' ' || part[lo] == '\t')) ++lo;
        while (hi > lo && (part[hi - 1] == ' ' || part[hi - 1] == '\t')) --hi;
        if (hi > lo) items.emplace_back(part.substr(lo, hi - lo));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return items;
}

bool is_in(const std::vector<std::string>& choices, std::string_view item) {
    return std::find(choices.begin(), choices.end(), item) != choices.end();
}

bool is_any_choice(const Document& document, std::string_view name) {
    for (const auto& [symbol_name, sym] : document.symbols) {
        (void)symbol_name;
        if ((sym->type == ValueType::Enum || sym->type == ValueType::Set)
            && is_in(sym->meta.choices, name)) {
            return true;
        }
    }
    return false;
}

/// @brief Parse an integer literal of any integer-valued type.
Result<std::int64_t> parse_integer(std::string_view text, ValueType type) {
    if (text.empty()) {
        return std::unexpected(Diagnostic::error(ErrorCode::TypeMismatch, "empty value"));
    }
    int base = 10;
    if (type == ValueType::Hex) {
        if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
        base = 16;
    } else if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }

    std::int64_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::TypeMismatch,
            "'" + std::string{text} + "' is not a valid " + to_string(type)));
    }
    return value;
}

/// @brief Compare two scalars.  Numbers compare numerically, strings
///        lexicographically; mixed kinds are never equal.
std::expected<int, Diagnostic> compare(const EvalValue& lhs, const EvalValue& rhs) {
    const auto as_int = [](const EvalValue& v) -> std::optional<std::int64_t> {
        if (const auto* b = std::get_if<bool>(&v)) return *b ? 1 : 0;
        if (const auto* i = std::get_if<std::int64_t>(&v)) return *i;
        return std::nullopt;
    };
    if (const auto l = as_int(lhs)) {
        if (const auto r = as_int(rhs)) return *l < *r ? -1 : *l > *r ? 1 : 0;
        return std::unexpected(Diagnostic::error(
            ErrorCode::TypeMismatch, "cannot compare a number with a string"));
    }
    const auto* l = std::get_if<std::string>(&lhs);
    const auto* r = std::get_if<std::string>(&rhs);
    if (l != nullptr && r != nullptr) {
        return *l < *r ? -1 : *l > *r ? 1 : 0;
    }
    return std::unexpected(Diagnostic::error(
        ErrorCode::TypeMismatch, "cannot compare these operand types"));
}

std::optional<std::string> host_builtin(std::string_view name) {
    static const auto info = [] {
        struct utsname value{};
        (void)::uname(&value);
        return value;
    }();
    if (name == "HOST_OS") return std::string{info.sysname};
    if (name == "HOST_KERNEL") return std::string{info.release};
    if (name == "HOST_ARCH") return std::string{info.machine};
    if (name == "HOST_NODE") return std::string{info.nodename};
    return std::nullopt;
}

}  // namespace

std::string render_value(const EvalValue& value, ValueType type) {
    if (std::holds_alternative<std::monostate>(value)) return "<unset>";

    if (const auto* b = std::get_if<bool>(&value)) {
        if (type == ValueType::Tristate) return *b ? "y" : "n";
        return *b ? "true" : "false";
    }
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        if (type == ValueType::Tristate) {
            const auto level = std::clamp<std::int64_t>(*i, 0, 2);
            static constexpr const char* k_tristate[] = {"n", "m", "y"};
            return k_tristate[static_cast<std::size_t>(level)];
        }
        // Boolean logic yields tristate levels; a bool-typed symbol must
        // render as a boolean, not as "2".
        if (type == ValueType::Bool) return *i != 0 ? "true" : "false";
        if (type == ValueType::Hex) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "0x%llX",
                          static_cast<unsigned long long>(*i));
            return buffer;
        }
        return std::to_string(*i);
    }
    if (const auto* s = std::get_if<std::string>(&value)) return *s;
    if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
        std::string out;
        for (std::size_t i = 0; i < list->size(); ++i) {
            if (i != 0) out += ",";
            out += (*list)[i];
        }
        return out;
    }
    return "<unset>";
}

Result<EvalValue> coerce(std::string_view text,
                         ValueType type,
                         const std::vector<std::string>& choices) {
    switch (type) {
        case ValueType::Bool: {
            if (text == "true" || text == "y" || text == "yes" || text == "1"
                || text == "on") return EvalValue{true};
            if (text == "false" || text == "n" || text == "no" || text == "0"
                || text == "off") return EvalValue{false};
            return std::unexpected(Diagnostic::error(
                ErrorCode::TypeMismatch,
                "'" + std::string{text} + "' is not a boolean"));
        }
        case ValueType::Tristate: {
            if (text == "n" || text == "0") return EvalValue{std::int64_t{0}};
            if (text == "m" || text == "1") return EvalValue{std::int64_t{1}};
            if (text == "y" || text == "2") return EvalValue{std::int64_t{2}};
            return std::unexpected(Diagnostic::error(
                ErrorCode::TypeMismatch,
                "'" + std::string{text} + "' is not a tristate value (n, m, y)"));
        }
        case ValueType::Int:
        case ValueType::Uint:
        case ValueType::Hex: {
            auto value = parse_integer(text, type);
            if (!value) return std::unexpected(value.error());
            return EvalValue{*value};
        }
        case ValueType::String:
        case ValueType::Path:
            return EvalValue{std::string{text}};
        case ValueType::Enum: {
            if (!is_in(choices, text)) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::UnknownChoice,
                    "'" + std::string{text} + "' is not one of the declared choices"));
            }
            return EvalValue{std::string{text}};
        }
        case ValueType::Set: {
            std::vector<std::string> items = split_items(text);
            for (const auto& item : items) {
                if (!is_in(choices, item)) {
                    return std::unexpected(Diagnostic::error(
                        ErrorCode::UnknownChoice,
                        "'" + item + "' is not one of the declared choices"));
                }
            }
            return EvalValue{std::move(items)};
        }
        case ValueType::List:
            return EvalValue{split_items(text)};
    }
    return std::unexpected(Diagnostic::error(
        ErrorCode::Invariant, "unhandled value type"));
}

std::int64_t truth_level(const EvalValue& value) {
    if (const auto* b = std::get_if<bool>(&value)) return *b ? 2 : 0;
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return std::clamp<std::int64_t>(*i, 0, 2);
    }
    if (const auto* s = std::get_if<std::string>(&value)) return s->empty() ? 0 : 1;
    if (const auto* l = std::get_if<std::vector<std::string>>(&value)) {
        return l->empty() ? 0 : 1;
    }
    return 0;
}

Runtime::Runtime(const Document& document, std::vector<Diagnostic>* diagnostics)
    : document_{document}, diagnostics_{diagnostics} {}

const Symbol* Runtime::resolve_symbol(std::string_view name, SourceSpan span,
                                      const Scope& scope) const {
    if (name.find('.') == std::string_view::npos) {
        std::string_view ns;
        if (const auto file = document_.file_namespaces.find(span.begin.file_id);
            file != document_.file_namespaces.end()) {
            ns = file->second;
        } else if (scope.current != nullptr) {
            ns = scope.current->namespace_;
        }
        if (!ns.empty()) {
            if (const auto* found = symbol(std::string{ns} + "." + std::string{name})) {
                return found;
            }
        }
    }
    return symbol(name);
}

const Symbol* Runtime::symbol(std::string_view name) const {
    const auto direct = [this](std::string_view candidate) -> const Symbol* {
        const auto it = document_.symbols.find(std::string{candidate});
        return it == document_.symbols.end() ? nullptr : it->second;
    };
    if (const auto* found = direct(name)) return found;

    // An alias resolves to its target through a bounded chain; a cycle simply
    // runs out of hops and leaves the name unresolvable.
    std::string_view current = name;
    for (int hops = 0; hops < 8; ++hops) {
        const AliasDirective* alias = nullptr;
        for (const auto& candidate : document_.aliases) {
            if (candidate.alias == current) {
                alias = &candidate;
                break;
            }
        }
        if (alias == nullptr) break;
        current = alias->target;
        if (const auto* found = direct(current)) return found;
    }
    return nullptr;
}

const EvalValue* Runtime::value(std::string_view name) const {
    const auto* sym = symbol(name);
    if (sym == nullptr) return nullptr;
    const auto it = values_.find(sym->name);
    return it == values_.end() ? nullptr : &it->second;
}

const EvalValue* Runtime::default_value(std::string_view name) const {
    const auto* sym = symbol(name);
    if (sym == nullptr) return nullptr;
    const auto it = default_values_.find(sym->name);
    return it == default_values_.end() ? nullptr : &it->second;
}

bool Runtime::is_user_set(std::string_view name) const {
    const auto* sym = symbol(name);
    return sym != nullptr && user_set_.count(sym->name) != 0;
}

bool Runtime::is_visible(std::string_view name) const {
    const auto* sym = symbol(name);
    if (sym == nullptr) return false;
    const auto it = visibility_.find(sym->name);
    return it != visibility_.end() && it->second;
}

bool Runtime::is_enabled(std::string_view name) const {
    const auto* sym = symbol(name);
    if (sym == nullptr || sym->computed) return false;
    if (sym->meta.enabled == nullptr) return true;
    Scope scope{sym, {}};
    auto condition = evaluate_expr(*sym->meta.enabled, scope);
    return condition.has_value() && truth_level(*condition) != 0;
}

bool Runtime::is_menu_visible(const Menu& menu) const {
    const auto it = menu_visibility_.find(&menu);
    return it != menu_visibility_.end() && it->second;
}

std::vector<std::string> Runtime::user_symbols() const {
    std::vector<std::string> names;
    names.reserve(user_set_.size());
    for (const auto& name : user_set_) names.push_back(name);
    return names;
}

std::vector<const Symbol*> Runtime::declared_symbols() const {
    std::vector<const Symbol*> symbols;
    symbols.reserve(document_.symbols.size());
    for (const auto& [name, symbol] : document_.symbols) {
        (void)name;
        symbols.push_back(symbol);
    }
    std::sort(symbols.begin(), symbols.end(),
              [](const Symbol* a, const Symbol* b) { return a->name < b->name; });
    return symbols;
}

Result<EvalValue> Runtime::evaluate_symbol(const Symbol& symbol, Scope& scope) const {
    const auto name = symbol.name;
    if (const auto it = values_.find(name); it != values_.end()) return it->second;

    if (std::find(scope.stack.begin(), scope.stack.end(), &symbol) != scope.stack.end()) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::CircularDependency,
            "'" + name + "' is part of a value cycle"));
    }
    scope.stack.push_back(&symbol);
    struct Pop {
        std::vector<const Symbol*>& stack;
        ~Pop() { stack.pop_back(); }
    } pop{scope.stack};

    auto default_result = evaluate_default(symbol, scope);
    if (!default_result) return std::unexpected(default_result.error());
    default_values_[name] = *default_result;

    EvalValue value = *default_result;
    if (user_set_.count(name) != 0) {
        if (const auto it = user_values_.find(name); it != user_values_.end()) {
            value = it->second;
        }
    }
    values_[name] = std::move(value);
    return values_.at(name);
}

Result<EvalValue> Runtime::evaluate_default(const Symbol& symbol, Scope& scope) const {
    const Symbol* previous = scope.current;
    scope.current = &symbol;
    struct Restore {
        Scope& scope;
        const Symbol* previous;
        ~Restore() { scope.current = previous; }
    } restore{scope, previous};

    const auto& cases = symbol.meta.defaults;
    const Expr* chosen = nullptr;
    const Expr* fallback = nullptr;
    for (const auto& case_item : cases) {
        if (case_item.condition == nullptr) {
            fallback = case_item.value.get();
            continue;
        }
        auto condition = evaluate_expr(*case_item.condition, scope);
        if (!condition) return std::unexpected(condition.error());
        if (truth_level(*condition) != 0) {
            chosen = case_item.value.get();
            break;
        }
    }
    if (chosen == nullptr) chosen = fallback;
    if (chosen == nullptr) return EvalValue{std::monostate{}};
    return evaluate_expr(*chosen, scope);
}

Result<EvalValue> Runtime::evaluate_expr(const Expr& expr, Scope& scope) const {
    switch (expr.kind) {
        case ExprKind::BoolLit:
            return EvalValue{expr.number != 0};
        case ExprKind::IntLit:
            return EvalValue{expr.number};
        case ExprKind::StringLit:
            return interpolate(expr.text, scope, expr.span);
        case ExprKind::SequenceLit: {
            std::vector<std::string> items;
            items.reserve(expr.args.size());
            for (const auto& item : expr.args) {
                if (item == nullptr) continue;
                if (item->is(ExprKind::Ident) && item->op.kind != TokenKind::Dollar) {
                    items.push_back(item->text);
                } else if (item->is(ExprKind::StringLit)) {
                    // Interpolated like any string value; rendered back to
                    // plain text for the list.
                    auto value = interpolate(item->text, scope, item->span);
                    if (!value) return std::unexpected(value.error());
                    items.push_back(render_value(*value, ValueType::String));
                } else if (item->is(ExprKind::IntLit)) {
                    items.push_back(std::to_string(item->number));
                } else {
                    auto value = evaluate_expr(*item, scope);
                    if (!value) return std::unexpected(value.error());
                    if (std::holds_alternative<std::vector<std::string>>(*value)
                        || std::holds_alternative<std::monostate>(*value)) {
                        return std::unexpected(Diagnostic::error(
                            ErrorCode::TypeMismatch, "collection items must be scalar values", item->span));
                    }
                    items.push_back(render_value(*value, ValueType::String));
                }
            }
            return EvalValue{std::move(items)};
        }
        case ExprKind::Ident: {
            if (const auto* sym = resolve_symbol(expr.text, expr.span, scope)) {
                return evaluate_symbol(*sym, scope);
            }

            if (const auto host = host_builtin(expr.text)) return EvalValue{*host};

            if (expr.text == "n" || expr.text == "m" || expr.text == "y") {
                return EvalValue{expr.text == "n" ? std::int64_t{0}
                                  : expr.text == "m" ? std::int64_t{1}
                                                     : std::int64_t{2}};
            }
            if (is_any_choice(document_, expr.text)) return EvalValue{expr.text};

            return std::unexpected(Diagnostic::error(
                ErrorCode::UnknownSymbol,
                "'" + expr.text + "' is not declared", expr.span));
        }
        case ExprKind::Unary: {
            if (expr.args.empty() || expr.args.front() == nullptr) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::Invariant, "unary operator without an operand", expr.span));
            }
            auto operand = evaluate_expr(*expr.args.front(), scope);
            if (!operand) return std::unexpected(operand.error());

            switch (expr.op.kind) {
                case TokenKind::Bang:
                case TokenKind::KwNot:
                    return EvalValue{std::int64_t{2} - truth_level(*operand)};
                case TokenKind::Minus:
                case TokenKind::Plus:
                case TokenKind::Tilde: {
                    const auto* number = std::get_if<std::int64_t>(&*operand);
                    if (number == nullptr) {
                        return std::unexpected(Diagnostic::error(
                            ErrorCode::UndefinedValue,
                            "arithmetic on a value that has none", expr.span));
                    }
                    if (expr.op.kind == TokenKind::Minus) return EvalValue{-*number};
                    if (expr.op.kind == TokenKind::Plus) return EvalValue{*number};
                    return EvalValue{~*number};
                }
                default:
                    return std::unexpected(Diagnostic::error(
                        ErrorCode::Invariant, "unhandled unary operator", expr.span));
            }
        }
        case ExprKind::Binary: {
            if (expr.args.size() != 2 || expr.args[0] == nullptr || expr.args[1] == nullptr) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::Invariant, "binary operator without two operands", expr.span));
            }
            auto lhs = evaluate_expr(*expr.args[0], scope);
            if (!lhs) return std::unexpected(lhs.error());
            if ((expr.op.kind == TokenKind::AmpAmp || expr.op.kind == TokenKind::KwAnd)
                && truth_level(*lhs) == 0) return EvalValue{std::int64_t{0}};
            if ((expr.op.kind == TokenKind::PipePipe || expr.op.kind == TokenKind::KwOr)
                && truth_level(*lhs) == 2) return EvalValue{std::int64_t{2}};
            auto rhs = evaluate_expr(*expr.args[1], scope);
            if (!rhs) return std::unexpected(rhs.error());

            switch (expr.op.kind) {
                case TokenKind::AmpAmp:
                case TokenKind::KwAnd:
                    return EvalValue{std::min(truth_level(*lhs), truth_level(*rhs))};
                case TokenKind::PipePipe:
                case TokenKind::KwOr:
                    return EvalValue{std::max(truth_level(*lhs), truth_level(*rhs))};
                case TokenKind::EqEq:
                case TokenKind::NotEq: {
                    if (const auto* left = std::get_if<std::vector<std::string>>(&*lhs)) {
                        const auto* right = std::get_if<std::vector<std::string>>(&*rhs);
                        if (right == nullptr) {
                            return std::unexpected(Diagnostic::error(
                                ErrorCode::TypeMismatch, "a collection must be compared with a collection", expr.span));
                        }
                        const auto is_set = [&](const auto& self, const Expr& operand) -> bool {
                            if (operand.kind == ExprKind::SequenceLit) {
                                return operand.op.kind == TokenKind::LBrace;
                            }
                            if (operand.kind == ExprKind::Ternary && operand.args.size() == 3) {
                                auto condition = evaluate_expr(*operand.args[0], scope);
                                if (condition) {
                                    return self(self, *operand.args[truth_level(*condition) != 0 ? 1 : 2]);
                                }
                            }
                            const auto* declaration = operand.kind == ExprKind::Ident
                                ? resolve_symbol(operand.text, operand.span, scope) : nullptr;
                            return declaration != nullptr && declaration->type == ValueType::Set;
                        };
                        auto a = *left;
                        auto b = *right;
                        const bool left_set = is_set(is_set, *expr.args[0]);
                        const bool right_set = is_set(is_set, *expr.args[1]);
                        if (left_set != right_set) {
                            return std::unexpected(Diagnostic::error(
                                ErrorCode::TypeMismatch, "cannot compare a set with a list", expr.span));
                        }
                        if (left_set) {
                            std::sort(a.begin(), a.end());
                            std::sort(b.begin(), b.end());
                            a.erase(std::unique(a.begin(), a.end()), a.end());
                            b.erase(std::unique(b.begin(), b.end()), b.end());
                        }
                        const bool equal = a == b;
                        return EvalValue{expr.op.kind == TokenKind::EqEq ? equal : !equal};
                    }
                    auto ordering = compare(*lhs, *rhs);
                    if (!ordering) return std::unexpected(ordering.error());
                    const auto equal = *ordering == 0;
                    return EvalValue{expr.op.kind == TokenKind::EqEq ? equal : !equal};
                }
                case TokenKind::Lt: case TokenKind::LtEq:
                case TokenKind::Gt: case TokenKind::GtEq: {
                    auto ordering = compare(*lhs, *rhs);
                    if (!ordering) return std::unexpected(ordering.error());
                    bool result = false;
                    switch (expr.op.kind) {
                        case TokenKind::Lt:    result = *ordering < 0; break;
                        case TokenKind::LtEq:  result = *ordering <= 0; break;
                        case TokenKind::Gt:    result = *ordering > 0; break;
                        default:               result = *ordering >= 0; break;
                    }
                    return EvalValue{result};
                }
                case TokenKind::Plus: case TokenKind::Minus:
                case TokenKind::Star: case TokenKind::Slash:
                case TokenKind::Percent: case TokenKind::Amp:
                case TokenKind::Pipe: case TokenKind::Caret:
                case TokenKind::Shl: case TokenKind::Shr: {
                    const auto* l = std::get_if<std::int64_t>(&*lhs);
                    const auto* r = std::get_if<std::int64_t>(&*rhs);
                    if (l == nullptr || r == nullptr) {
                        return std::unexpected(Diagnostic::error(
                            ErrorCode::UndefinedValue,
                            "arithmetic on a value that has none", expr.span));
                    }
                    if ((expr.op.kind == TokenKind::Slash
                         || expr.op.kind == TokenKind::Percent) && *r == 0) {
                        return std::unexpected(Diagnostic::error(
                            ErrorCode::DivisionByZero, "division by zero", expr.span));
                    }
                    switch (expr.op.kind) {
                        case TokenKind::Plus:   return EvalValue{*l + *r};
                        case TokenKind::Minus:  return EvalValue{*l - *r};
                        case TokenKind::Star:   return EvalValue{*l * *r};
                        case TokenKind::Slash:  return EvalValue{*l / *r};
                        case TokenKind::Percent: return EvalValue{*l % *r};
                        case TokenKind::Amp:    return EvalValue{*l & *r};
                        case TokenKind::Pipe:   return EvalValue{*l | *r};
                        case TokenKind::Caret:  return EvalValue{*l ^ *r};
                        case TokenKind::Shl:    return EvalValue{*l << *r};
                        default:                return EvalValue{*l >> *r};
                    }
                }
                default:
                    return std::unexpected(Diagnostic::error(
                        ErrorCode::Invariant, "unhandled binary operator", expr.span));
            }
        }
        case ExprKind::Ternary: {
            if (expr.args.size() != 3) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::Invariant, "ternary without three operands", expr.span));
            }
            auto condition = evaluate_expr(*expr.args[0], scope);
            if (!condition) return std::unexpected(condition.error());
            return evaluate_expr(*expr.args[truth_level(*condition) != 0 ? 1 : 2], scope);
        }
        case ExprKind::Call:
            return evaluate_call(expr, scope);
        case ExprKind::Fallback: {
            if (expr.args.size() != 2 || expr.args[0] == nullptr || expr.args[1] == nullptr) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::Invariant, "fallback without two operands", expr.span));
            }
            // A failed probe is the one failure a fallback exists for; the
            // fallback value is used without comment.  Everything else the
            // primary produced propagates.
            return evaluate_expr(*expr.args[0], scope)
                .or_else([&](const Diagnostic&) {
                    return evaluate_expr(*expr.args[1], scope);
                });
        }
        case ExprKind::Invalid:
        default:
            return std::unexpected(Diagnostic::error(
                ErrorCode::Invariant, "invalid expression", expr.span));
    }
}

Result<EvalValue> Runtime::interpolate(std::string_view text,
                                       Scope& scope,
                                       SourceSpan span) const {
    const auto open_at = text.find("${");
    if (open_at == std::string_view::npos) return EvalValue{std::string{text}};

    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const auto open = text.find("${", at);
        if (open == std::string_view::npos) {
            out.append(text.substr(at));
            break;
        }
        out.append(text.substr(at, open - at));

        // A doubled dollar escapes expansion, including $${NAME}.
        if (open > at && text[open - 1] == '$') {
            out.pop_back();
            out += "${";
            at = open + 2;
            continue;
        }

        const auto close = text.find('}', open + 2);
        if (close == std::string_view::npos) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::Interpolation, "unclosed '${' in string", span));
        }
        const auto name = text.substr(open + 2, close - open - 2);

        // Resolution is the identifier rule: the referencing symbol's
        // namespace first, then the alias chain.
        const Symbol* target = resolve_symbol(name, span, scope);

        if (target == nullptr) {
            if (const auto host = host_builtin(name)) {
                out.append(*host);
                at = close + 1;
                continue;
            }
        }

        if (target == nullptr) {
            if (name == "n" || name == "m" || name == "y" || is_any_choice(document_, name)) {
                out.append(name);
                at = close + 1;
                continue;
            }
            return std::unexpected(Diagnostic::error(
                ErrorCode::Interpolation,
                "'${" + std::string{name} + "}' is not a declared symbol", span));
        }

        auto value = evaluate_symbol(*target, scope);
        if (!value) return std::unexpected(value.error());
        if (std::holds_alternative<std::monostate>(*value)) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::UndefinedValue,
                "'" + target->name + "' has no value to interpolate", span));
        }
        out.append(render_value(*value, target->type));
        at = close + 1;
    }
    return EvalValue{std::move(out)};
}

Result<std::string> Runtime::expand_text(std::string_view text,
                                         std::string_view context, SourceSpan span) const {
    Scope scope;
    if (!context.empty()) scope.current = symbol(context);
    auto expanded = interpolate(text, scope, span);
    if (!expanded) return std::unexpected(expanded.error());
    return render_value(*expanded, ValueType::String);
}

Result<EvalValue> Runtime::evaluate_call(const Expr& call, Scope& scope) const {
    if (call.args.empty() || call.args.front() == nullptr) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::Invariant, "call without arguments", call.span));
    }
    const auto& name = call.text;

    if (name == "type_of") {
        const auto& argument = *call.args.front();
        if (argument.kind == ExprKind::Ident) {
            if (const auto* declaration = resolve_symbol(argument.text, argument.span, scope)) {
                return EvalValue{std::string{to_string(declaration->type)}};
            }
        }
        auto value = evaluate_expr(argument, scope);
        if (!value) return std::unexpected(value.error());
        if (std::holds_alternative<bool>(*value)) return EvalValue{std::string{"bool"}};
        if (std::holds_alternative<std::int64_t>(*value)) return EvalValue{std::string{"int"}};
        if (std::holds_alternative<std::string>(*value)) return EvalValue{std::string{"string"}};
        return EvalValue{std::string{argument.op.kind == TokenKind::LBrace ? "set" : "list"}};
    }

    if (name == "contains") {
        auto collection = evaluate_expr(*call.args[0], scope);
        if (!collection) return std::unexpected(collection.error());
        auto item = evaluate_expr(*call.args[1], scope);
        if (!item) return std::unexpected(item.error());
        const auto* values = std::get_if<std::vector<std::string>>(&*collection);
        const auto* text = std::get_if<std::string>(&*item);
        if (values == nullptr || text == nullptr) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::TypeMismatch, "contains takes a collection and a string or enum choice", call.span));
        }
        return EvalValue{std::find(values->begin(), values->end(), *text) != values->end()};
    }

    if (name == "is_defined" || name == "defined_and_true") {
        const auto& argument = *call.args.front();
        if (!argument.is(ExprKind::Ident)) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::TypeMismatch, name + " takes a symbol name",
                argument.span.is_valid() ? argument.span : call.span));
        }
        const auto* sym = resolve_symbol(argument.text, argument.span, scope);
        const auto host = host_builtin(argument.text);
        const bool defined = sym != nullptr || host.has_value();
        if (name == "is_defined") return EvalValue{defined};
        if (!defined) return EvalValue{false};
        if (host && sym == nullptr) return EvalValue{!host->empty()};
        auto value = evaluate_symbol(*sym, scope);
        if (!value) return std::unexpected(value.error());
        return EvalValue{truth_level(*value) > 0};
    }

    if (name == "shell" || name == "env") {
        const auto& argument = *call.args.front();
        if (!argument.is(ExprKind::StringLit)) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::TypeMismatch, name + " takes a string literal", call.span));
        }
        auto expanded = interpolate(argument.text, scope, argument.span);
        if (!expanded) return std::unexpected(expanded.error());
        return run_probe(name, std::get<std::string>(*expanded));
    }

    // The remaining builtins are arithmetic; all operands must be integers.
    std::vector<std::int64_t> numbers;
    for (const auto& argument : call.args) {
        if (argument == nullptr) continue;
        auto value = evaluate_expr(*argument, scope);
        if (!value) return std::unexpected(value.error());
        const auto* number = std::get_if<std::int64_t>(&*value);
        if (number == nullptr) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::TypeMismatch, name + " takes integer arguments", call.span));
        }
        numbers.push_back(*number);
    }

    if (name == "abs") return EvalValue{numbers.front() < 0 ? -numbers.front()
                                                            : numbers.front()};
    if (name == "min") return EvalValue{*std::min_element(numbers.begin(), numbers.end())};
    if (name == "max") return EvalValue{*std::max_element(numbers.begin(), numbers.end())};
    if (name == "log2") {
        // Floor of the base-2 logarithm: the index of the highest set bit.
        // log2(0) is defined as 0 here; a zero-width mask is not an error the
        // user can act on.
        const auto value = numbers.front();
        std::int64_t result = 0;
        while ((value >> result) > 1) ++result;
        return EvalValue{result};
    }
    return std::unexpected(Diagnostic::error(
        ErrorCode::UnknownBuiltin, "'" + name + "' is not a builtin", call.span));
}

Result<EvalValue> Runtime::run_probe(const std::string& kind,
                                     const std::string& argument) const {
    const auto key = kind + ":" + argument;
    if (const auto it = probe_cache_.find(key); it != probe_cache_.end()) {
        return it->second;
    }

    if (kind == "env") {
        const char* value = ::getenv(argument.c_str());
        if (value == nullptr) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::ProbeFailed,
                "environment variable '" + argument + "' is not set"));
        }
        const EvalValue out{std::string{value}};
        probe_cache_.emplace(key, out);
        return out;
    }

    std::string output;
    FILE* process = ::popen(argument.c_str(), "r");
    if (process == nullptr) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::ProbeFailed, "cannot run '" + argument + "'"));
    }
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, process) != nullptr) {
        output += buffer;
    }
    const int status = ::pclose(process);
    if (status != 0) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::ProbeFailed,
            "'" + argument + "' exited with status " + std::to_string(status)));
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'
                               || output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    const EvalValue out{std::move(output)};
    probe_cache_.emplace(key, out);
    return out;
}

Result<void> Runtime::evaluate() {
    values_.clear();
    default_values_.clear();
    visibility_.clear();
    menu_visibility_.clear();

    Scope scope{nullptr, {}};
    for (const auto& [name, sym] : document_.symbols) {
        (void)name;
        auto value = evaluate_symbol(*sym, scope);
        if (!value) {
            return std::unexpected(Diagnostic::error(
                value.error().code, "while resolving '" + sym->name + "': "
                    + value.error().message, value.error().span));
        }
    }

    if (document_.root != nullptr) resolve_visibility(*document_.root, true);
    return {};
}

void Runtime::resolve_visibility(const Menu& menu, bool parent_visible) {
    bool visible = parent_visible;
    if (menu.visible != nullptr) {
        Scope scope{nullptr, {}};
        auto condition = evaluate_expr(*menu.visible, scope);
        visible = visible && condition.has_value() && truth_level(*condition) != 0;
    }
    menu_visibility_[&menu] = visible;

    for (const auto& child : menu.children) {
        if (const auto* symbol_node = std::get_if<Symbol>(&child)) {
            bool own = visible;
            Scope scope{symbol_node, {}};
            if (symbol_node->meta.when != nullptr) {
                auto condition = evaluate_expr(*symbol_node->meta.when, scope);
                own = own && condition.has_value() && truth_level(*condition) != 0;
            }
            if (symbol_node->meta.visible != nullptr) {
                auto condition = evaluate_expr(*symbol_node->meta.visible, scope);
                own = own && condition.has_value() && truth_level(*condition) != 0;
            }
            visibility_[symbol_node->name] = own;
        } else if (const auto& sub = std::get<MenuPtr>(child)) {
            resolve_visibility(*sub, visible);
        }
    }
}

std::vector<Diagnostic> Runtime::run_validation() const {
    std::vector<Diagnostic> findings;

    auto check = [&](const std::string& message,
                     const Expr& condition,
                     bool warning,
                     SourceSpan span) {
        Scope scope{nullptr, {}};
        auto result = evaluate_expr(condition, scope);
        if (!result) {
            auto failure = result.error();
            failure.severity = warning ? Severity::Warning : Severity::Error;
            findings.push_back(std::move(failure));
            return;
        }
        if (truth_level(*result) == 0) {
            auto expanded = expand_text(message, {}, span);
            findings.push_back(Diagnostic{
                ErrorCode::ConstraintViolated,
                warning ? Severity::Warning : Severity::Error,
                expanded ? *expanded : message, span});
        }
    };

    for (const auto& clause : document_.validations) {
        if (clause.condition == nullptr) continue;
        check(clause.message, *clause.condition, clause.warning, clause.span);
    }
    for (const auto& [name, sym] : document_.symbols) {
        (void)name;
        Scope scope{sym, {}};
        for (const auto& clause : sym->meta.warnings) {
            if (clause.condition == nullptr) continue;
            auto result = evaluate_expr(*clause.condition, scope);
            if (!result) {
                auto failure = result.error();
                failure.severity = Severity::Warning;
                findings.push_back(std::move(failure));
                continue;
            }
            if (truth_level(*result) != 0) {
                auto expanded = expand_text(clause.message, sym->name);
                findings.push_back(Diagnostic{
                    ErrorCode::ConstraintViolated, Severity::Warning,
                    expanded ? *expanded : clause.message, sym->span});
            }
        }
    }
    return findings;
}

Result<void> Runtime::set_user_value(std::string_view name,
                                     std::string_view text,
                                     std::optional<ValueType> claimed) {
    const auto* sym = symbol(name);
    if (sym == nullptr) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::UnknownSymbol, "'" + std::string{name} + "' is not declared"));
    }
    if (sym->computed) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::TypeMismatch, "'" + std::string{name} + "' is computed and read-only"));
    }

    if (claimed.has_value() && *claimed != sym->type) {
        if (diagnostics_ != nullptr) {
            diagnostics_->push_back(Diagnostic::warning(
                ErrorCode::TypeMismatch,
                "cache records '" + std::string{name} + "' as " + to_string(*claimed)
                    + " but the manifest declares " + to_string(sym->type)));
        }
    }

    auto value = coerce(text, sym->type, sym->meta.choices);
    if (!value) return std::unexpected(value.error());

    if ((sym->type == ValueType::Int || sym->type == ValueType::Uint)
        && sym->meta.range.has_value()) {
        const auto* number = std::get_if<std::int64_t>(&*value);
        if (number == nullptr
            || *number < sym->meta.range->lo || *number > sym->meta.range->hi) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::RangeViolated,
                "'" + std::string{text} + "' is outside the range "
                    + std::to_string(sym->meta.range->lo) + " .. "
                    + std::to_string(sym->meta.range->hi)));
        }
    }

    if (!sym->meta.pattern.empty()
        && (sym->type == ValueType::String || sym->type == ValueType::Path)) {
        try {
            const std::regex pattern{sym->meta.pattern};
            const std::string text_string{text};
            if (!std::regex_search(text_string, pattern)) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::PatternMismatch,
                    "'" + std::string{text} + "' does not match " + sym->meta.pattern));
            }
        } catch (const std::regex_error&) {
            if (diagnostics_ != nullptr) {
                diagnostics_->push_back(Diagnostic::warning(
                    ErrorCode::PatternMismatch,
                    "pattern '" + sym->meta.pattern + "' is not a valid regex; skipped"));
            }
        }
    }

    if (sym->type == ValueType::Path) {
        for (const auto& constraint : sym->meta.path_constraints) {
            const std::filesystem::path path{text};
            std::error_code ec;
            if (constraint == "must_exist" && !std::filesystem::exists(path, ec)) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::PathConstraint,
                    "'" + std::string{text} + "' does not exist"));
            }
            if (constraint == "is_dir" && !std::filesystem::is_directory(path, ec)) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::PathConstraint,
                    "'" + std::string{text} + "' is not a directory"));
            }
        }
    }

    // Store under the resolved name: an alias writes its target's slot, so
    // the value round-trips under one canonical symbol.
    const auto old_value = user_values_.find(sym->name);
    const bool was_set = user_set_.contains(sym->name);
    std::optional<EvalValue> previous;
    if (old_value != user_values_.end()) previous = old_value->second;

    user_values_[sym->name] = std::move(*value);
    user_set_.insert(sym->name);
    if (auto evaluated = evaluate(); !evaluated) {
        if (previous.has_value()) {
            user_values_[sym->name] = std::move(*previous);
        } else {
            user_values_.erase(sym->name);
        }
        if (was_set) user_set_.insert(sym->name);
        else user_set_.erase(sym->name);
        (void)evaluate();
        return std::unexpected(evaluated.error());
    }
    return {};
}

Result<void> Runtime::reset_symbol(std::string_view name) {
    const auto* sym = symbol(name);
    if (sym == nullptr) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::UnknownSymbol, "'" + std::string{name} + "' is not declared"));
    }
    if (sym->computed) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::TypeMismatch, "'" + std::string{name} + "' is computed and read-only"));
    }

    const auto old_value = user_values_.find(sym->name);
    const bool was_set = user_set_.contains(sym->name);
    std::optional<EvalValue> previous;
    if (old_value != user_values_.end()) previous = old_value->second;
    user_set_.erase(sym->name);
    user_values_.erase(sym->name);
    if (auto evaluated = evaluate(); !evaluated) {
        if (previous.has_value()) user_values_[sym->name] = std::move(*previous);
        if (was_set) user_set_.insert(sym->name);
        (void)evaluate();
        return std::unexpected(evaluated.error());
    }
    return {};
}

Result<void> Runtime::apply_assignment(std::string_view name,
                                       std::string_view value,
                                       ValueType claimed) {
    const auto* sym = symbol(name);
    if (sym == nullptr) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::UnknownSymbol, "'" + std::string{name} + "' is not declared"));
    }
    return set_user_value(name, value, claimed);
}

CacheDocument Runtime::to_cache(std::string_view manifest, std::string_view digest) const {
    CacheDocument document;
    document.manifest = std::string{manifest};
    document.manifest_digest = std::string{digest};
    for (const auto& name : user_symbols()) {
        const auto* sym = symbol(name);
        if (sym == nullptr) continue;
        const auto* value = this->value(name);
        if (value == nullptr) continue;
        document.assignments.push_back(Assignment{
            name, render_value(*value, sym->type), sym->type});
    }
    return document;
}

void Runtime::invalidate_probes() noexcept {
    probe_cache_.clear();
}

namespace {

/// @brief The fixes an implication clause can offer.  An identifier means
///        "make it true"; `A or B` means "make either true"; `X == lit`
///        means "set X to lit".  Anything else yields no fix and is reported
///        as a message only.
std::vector<ImplicationFix> fixes_for(const Expr& clause) {
    std::vector<ImplicationFix> fixes;

    const auto literal_text = [](const Expr& node) -> std::string {
        if (node.is(ExprKind::Ident)) return node.text;
        if (node.is(ExprKind::StringLit)) return node.text;
        if (node.is(ExprKind::IntLit)) return std::to_string(node.number);
        if (node.is(ExprKind::BoolLit)) return node.number != 0 ? "true" : "false";
        return {};
    };

    if (clause.is(ExprKind::Ident)) {
        fixes.push_back(ImplicationFix{clause.text, "true",
                                       clause.text + " = true"});
        return fixes;
    }
    if (clause.is(ExprKind::Binary)) {
        const bool either = clause.op.is(TokenKind::KwOr) || clause.op.is(TokenKind::PipePipe);
        if (either && clause.args.size() == 2
            && clause.args[0] != nullptr && clause.args[1] != nullptr
            && clause.args[0]->is(ExprKind::Ident) && clause.args[1]->is(ExprKind::Ident)) {
            for (const auto& target : clause.args) {
                fixes.push_back(ImplicationFix{target->text, "true",
                                               target->text + " = true"});
            }
            return fixes;
        }
        const bool equals = clause.op.is(TokenKind::EqEq);
        if (equals && clause.args.size() == 2
            && clause.args[0] != nullptr && clause.args[1] != nullptr) {
            const Expr* target = clause.args[0]->is(ExprKind::Ident) ? clause.args[0].get() : nullptr;
            const Expr* wanted = target != nullptr ? clause.args[1].get() : nullptr;
            if (target == nullptr && clause.args[1]->is(ExprKind::Ident)) {
                target = clause.args[1].get();
                wanted = clause.args[0].get();
            }
            if (target != nullptr && wanted != nullptr) {
                const auto text = literal_text(*wanted);
                if (!text.empty()) {
                    fixes.push_back(ImplicationFix{target->text, text,
                                                   target->text + " = " + text});
                }
            }
        }
    }
    return fixes;
}

}  // namespace

std::vector<Implication> Runtime::unsatisfied_implications() const {
    std::vector<Implication> out;

    for (const auto& [name, sym] : document_.symbols) {
        (void)name;
        const auto* current = this->value(sym->name);
        if (current == nullptr || truth_level(*current) == 0) continue;

        for (const auto& clause : sym->meta.implies) {
            if (clause == nullptr) continue;
            Scope scope{sym, {}};
            auto satisfied = evaluate_expr(*clause, scope);
            if (satisfied.has_value() && truth_level(*satisfied) != 0) continue;

            Implication implication;
            implication.source = sym->name;
            implication.fixes = fixes_for(*clause);
            for (auto& fix : implication.fixes) {
                if (const auto* target = resolve_symbol(fix.symbol, clause->span, scope)) {
                    fix.symbol = target->name;
                    fix.description = target->name + " = " + fix.value;
                }
            }

            // A conflict naming one of the fix targets supplies the prompt
            // text and, with the error policy, the save gate.
            for (const auto& conflict : sym->meta.conflicts) {
                bool concerns = implication.fixes.empty();
                for (const auto& fix : implication.fixes) {
                    if (fix.symbol == conflict.symbol) concerns = true;
                }
                if (!concerns) continue;
                implication.message = conflict.message;
                if (conflict.policy == ConflictPolicy::Error) {
                    implication.blocks_save = true;
                }
            }
            if (implication.message.empty()) {
                std::string question;
                for (const auto& fix : implication.fixes) {
                    if (!question.empty()) question += " or ";
                    question += fix.description;
                }
                implication.message = question.empty()
                    ? sym->name + " implies an unsatisfied condition"
                    : sym->name + " implies " + question + " — apply?";
            }
            out.push_back(std::move(implication));
        }

        // A conflict with the error policy and no matching implication still
        // gates the save: it is an unresolved contradiction.
        for (const auto& conflict : sym->meta.conflicts) {
            if (conflict.policy != ConflictPolicy::Error) continue;
            bool covered = false;
            for (const auto& open : out) {
                if (open.blocks_save && open.source == sym->name) covered = true;
            }
            if (!covered) {
                out.push_back(Implication{
                    sym->name,
                    conflict.message.empty()
                        ? sym->name + " conflicts with " + conflict.symbol
                        : conflict.message,
                    {}, true});
            }
        }
    }
    return out;
}

}  // namespace zxconfig
