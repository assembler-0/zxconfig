// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/parser.hpp>
#include <zxconfig/lexer.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace zxconfig {
namespace {

constexpr std::uint32_t k_max_include_depth = 64;
constexpr std::uint32_t k_max_expansion_depth = 32;

bool is_type_token(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::KwBool: case TokenKind::KwTristate: case TokenKind::KwInt:
        case TokenKind::KwUint: case TokenKind::KwHex: case TokenKind::KwString:
        case TokenKind::KwPath: case TokenKind::KwEnum: case TokenKind::KwSet:
        case TokenKind::KwList:
            return true;
        default:
            return false;
    }
}

ValueType type_of(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::KwBool:     return ValueType::Bool;
        case TokenKind::KwTristate: return ValueType::Tristate;
        case TokenKind::KwInt:      return ValueType::Int;
        case TokenKind::KwUint:     return ValueType::Uint;
        case TokenKind::KwHex:      return ValueType::Hex;
        case TokenKind::KwString:   return ValueType::String;
        case TokenKind::KwPath:     return ValueType::Path;
        case TokenKind::KwEnum:     return ValueType::Enum;
        case TokenKind::KwSet:      return ValueType::Set;
        case TokenKind::KwList:     return ValueType::List;
        default:                    return ValueType::Bool;
    }
}

bool is_builtin(std::string_view name) noexcept {
    return name == "shell" || name == "env" || name == "is_defined"
        || name == "defined_and_true" || name == "abs" || name == "min"
        || name == "max" || name == "log2" || name == "contains" || name == "type_of";
}

std::size_t builtin_arity(std::string_view name) noexcept {
    return (name == "min" || name == "max" || name == "contains") ? 2 : 1;
}

/// @brief Binding strength of a binary operator.  Higher binds tighter.
int precedence(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::PipePipe: case TokenKind::KwOr: return 1;
        case TokenKind::AmpAmp:   case TokenKind::KwAnd: return 2;
        case TokenKind::Pipe:     return 3;
        case TokenKind::Caret:    return 4;
        case TokenKind::Amp:      return 5;
        case TokenKind::EqEq: case TokenKind::NotEq: return 6;
        case TokenKind::Lt: case TokenKind::LtEq:
        case TokenKind::Gt: case TokenKind::GtEq:    return 7;
        case TokenKind::Shl: case TokenKind::Shr:    return 8;
        case TokenKind::Plus: case TokenKind::Minus: return 9;
        case TokenKind::Star: case TokenKind::Slash:
        case TokenKind::Percent:                     return 10;
        default: return 0;
    }
}

bool is_statement_start(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::KwMenu: case TokenKind::KwOption: case TokenKind::KwComputed:
        case TokenKind::KwInclude: case TokenKind::KwGenerate: case TokenKind::KwValidate:
        case TokenKind::KwWarn: case TokenKind::KwError: case TokenKind::KwAlias: case TokenKind::KwDeprecated:
        case TokenKind::KwExport: case TokenKind::KwMacro:
        case TokenKind::KwExportWithPrefix: case TokenKind::KwPresetsDir:
        case TokenKind::RBrace: case TokenKind::End:
            return true;
        default:
            return false;
    }
}

bool is_metadata_start(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::KwLabel: case TokenKind::KwHelp: case TokenKind::KwTags:
        case TokenKind::KwRange: case TokenKind::KwWhen: case TokenKind::KwVisible:
        case TokenKind::KwEnabled: case TokenKind::KwImplies: case TokenKind::KwOrder:
        case TokenKind::KwPattern: case TokenKind::KwPath: case TokenKind::KwChoices:
        case TokenKind::KwChoice: case TokenKind::KwDefault: case TokenKind::KwConflict:
        case TokenKind::KwWarn: case TokenKind::KwError:
            return true;
        default:
            return false;
    }
}

Result<std::int64_t> parse_number(std::string_view spelling) {
    std::int64_t value = 0;
    const auto* first = spelling.data();
    const auto* last = spelling.data() + spelling.size();
    const auto parsed = std::from_chars(first, last, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::BadNumber, "'" + std::string{spelling} + "' is out of range"));
    }
    return value;
}

ExprPtr make_binary(SourceSpan span, TokenKind op, ExprPtr lhs, ExprPtr rhs) {
    auto node = std::make_unique<Expr>();
    node->kind = ExprKind::Binary;
    node->span = span;
    node->op = Token{op, span, 0, 0};
    node->args.push_back(std::move(lhs));
    node->args.push_back(std::move(rhs));
    return node;
}

/// @brief Fold an include condition into a child's own condition.
void fold_condition(const Expr& condition, ExprPtr& target) {
    auto combined = clone_expr(condition);
    if (target != nullptr) {
        combined = make_binary(condition.span, TokenKind::AmpAmp,
                               std::move(combined), std::move(target));
    }
    target = std::move(combined);
}

class Parser {
    using MacroRef = std::shared_ptr<const Macro>;
    using MacroScope = std::unordered_map<std::string, MacroRef>;

public:
    Parser(ParseResult& out, SourceArena& arena, std::string_view root,
           std::uint32_t root_file_id)
        : out_{out}, arena_{arena}, root_{root}, root_file_id_{root_file_id} {}

    /// @brief Record one gating edge of the include graph so the runtime can
    ///        deactivate a file whose every include edge is hidden.  The root
    ///        manifest (parent 0) is always active, so an edge from the root
    ///        carries no parent dependency.
    void record_include_gate(std::uint32_t file_id, const ExprPtr& condition) {
        IncludeGate gate;
        gate.file_id = file_id;
        gate.parent_file_id = file_id_ == root_file_id_ ? 0 : file_id_;
        if (condition != nullptr) gate.condition = clone_expr(*condition);
        out_.document.include_gates.push_back(std::move(gate));
    }

    /// @brief Parse one file into @p insert.  Returns false when the file
    ///        could not be lexed or has no version header.
    bool parse_file(std::uint32_t file_id, Menu& insert, std::uint32_t depth,
                    std::string* declared_namespace = nullptr) {
        if (depth > k_max_include_depth) {
            out_.diagnostics.push_back(Diagnostic::error(
                ErrorCode::CircularDependency,
                "include depth exceeds " + std::to_string(k_max_include_depth)));
            return false;
        }
        if (std::find(parsing_.begin(), parsing_.end(), file_id) != parsing_.end()) {
            out_.diagnostics.push_back(Diagnostic::warning(
                ErrorCode::AlreadyLoaded,
                "include cycle: '" + std::string{arena_.find(file_id)->path}
                    + "' is already being parsed; skipped"));
            return false;
        }

        parsing_.push_back(file_id);
        MacroScope inherited;
        if (!macro_scopes_.empty()) {
            for (const auto& [name, macro] : macro_scopes_.back()) {
                if (macro->exported) inherited.emplace(name, macro);
            }
        }
        macro_scopes_.push_back(std::move(inherited));

        struct Pop {
            Parser* p;
            LexResult saved_lexed;
            std::size_t saved_pos;
            std::uint32_t saved_file;
            std::string saved_ns;
            std::uint32_t saved_depth;

            ~Pop() {
                p->parsing_.pop_back();
                auto finished = std::move(p->macro_scopes_.back());
                p->macro_scopes_.pop_back();
                MacroScope exports;
                for (const auto& [name, macro] : finished) {
                    if (macro->exported) exports.emplace(name, macro);
                }
                p->file_macro_exports_[p->file_id_] = std::move(exports);
                if (!p->macro_scopes_.empty()) {
                    p->import_macros(p->file_macro_exports_.at(p->file_id_));
                }
                p->lexed_ = std::move(saved_lexed);
                p->toks_ = &p->lexed_.tokens;
                p->pos_ = saved_pos;
                p->file_id_ = saved_file;
                p->ns_ = std::move(saved_ns);
                p->depth_ = saved_depth;
            }
        } pop_guard{this, std::move(lexed_), pos_, file_id_, std::move(ns_), depth_};

        auto lexed = lex(arena_, file_id);
        bool ok = true;
        for (auto& failure : lexed.errors) {
            out_.diagnostics.push_back(failure.diagnostic);
            ok = false;
        }

        file_id_ = file_id;
        ns_.clear();
        depth_ = depth;
        lexed_ = std::move(lexed);
        toks_ = &lexed_.tokens;
        pos_ = 0;

        if (!parse_header()) return false;
        if (accept(TokenKind::KwNamespace)) {
            auto declared = parse_name("namespace");
            if (!declared) return false;
            ns_ = std::move(*declared);
            (void)accept(TokenKind::Semicolon);
        }
        out_.document.file_namespaces[file_id_] = ns_;
        if (declared_namespace != nullptr) *declared_namespace = ns_;
        parse_statements(insert, false);
        return ok;
    }

    /// @brief Build the symbol index and report duplicates.
    void link() {
        link_menu(*out_.document.root);
        resolve_targets();
    }

    /// @brief Canonicalize reference targets (alias, deprecation, conflict)
    ///        with the expression rule: the declaring file's namespace first,
    ///        then global.  Unknown targets are parse errors; a deprecation's
    ///        old name is a legacy spelling and is never validated.
    void resolve_targets() {
        const auto declared = [this](const std::string& name) {
            return out_.document.symbols.find(name) != out_.document.symbols.end();
        };
        const auto resolve = [&](std::string& name, SourceSpan span,
                                 std::string_view what) {
            // Unqualified names follow the expression rule: the declaring
            // file's namespace first, then global.  Qualified names must
            // exist exactly as written.
            if (name.find('.') == std::string::npos) {
                if (const auto file = out_.document.file_namespaces.find(span.begin.file_id);
                    file != out_.document.file_namespaces.end() && !file->second.empty()) {
                    auto qualified = file->second + "." + name;
                    if (declared(qualified)) {
                        name = std::move(qualified);
                        return;
                    }
                }
                if (declared(name)) return;
                fail(ErrorCode::UnknownSymbol,
                     "'" + name + "' is not declared (" + std::string{what} + ")", span);
                return;
            }
            if (declared(name)) return;
            fail(ErrorCode::UnknownSymbol,
                 "'" + name + "' is not declared (" + std::string{what} + ")", span);
        };

        for (auto& alias : out_.document.aliases) {
            resolve(alias.target, alias.span, "alias target");
        }
        for (auto& directive : out_.document.deprecated) {
            resolve(directive.new_name, directive.span, "deprecated replacement");
        }
        for (auto& [name, sym] : out_.document.symbols) {
            (void)name;
            for (auto& clause : sym->meta.conflicts) {
                resolve(clause.symbol, sym->span, "conflict target");
            }
        }
    }

private:
    void import_macros(const MacroScope& exports) {
        for (const auto& [name, macro] : exports) {
            const auto [it, inserted] = macro_scopes_.back().emplace(name, macro);
            if (!inserted && it->second != macro) {
                fail(ErrorCode::DuplicateSymbol,
                     "macro '" + name + "' is exported by multiple files", macro->span);
            }
        }
    }

    MacroRef find_macro(std::string_view name) const {
        const auto lookup = [](const MacroScope& scope, const std::string& key) -> MacroRef {
            const auto found = scope.find(key);
            return found == scope.end() ? nullptr : found->second;
        };
        const bool qualified = name.find('.') != std::string_view::npos;
        // Helpers and recursive calls resolve in their definition environment.
        // This does not change the namespace of generated symbol declarations.
        if (!expanding_macros_.empty()) {
            const auto& definition = expanding_macros_.back();
            const auto found = file_macro_definitions_.find(definition->span.begin.file_id);
            if (found != file_macro_definitions_.end()) {
                if (!qualified && !definition->namespace_.empty()) {
                    if (auto macro = lookup(found->second, definition->namespace_ + "." + std::string{name})) {
                        return macro;
                    }
                }
                if (auto macro = lookup(found->second, std::string{name})) return macro;
            }
        }
        if (!qualified && !ns_.empty()) {
            if (auto macro = lookup(macro_scopes_.back(), ns_ + "." + std::string{name})) return macro;
        }
        return lookup(macro_scopes_.back(), std::string{name});
    }

    std::optional<std::string> parse_name(std::string_view context) {
        if (!expect(TokenKind::Id, context)) return std::nullopt;
        std::string name{lexed_.spelling(previous())};
        while (accept(TokenKind::Dot)) {
            if (!expect(TokenKind::Id, context)) return std::nullopt;
            name += ".";
            name += lexed_.spelling(previous());
        }
        return name;
    }

    std::string local_name(std::string name) const {
        return ns_.empty() || name.find('.') != std::string::npos
            ? name : ns_ + "." + name;
    }

    [[nodiscard]] const Token& peek(std::size_t ahead = 0) const noexcept {
        const auto& tokens = *toks_;
        const auto index = pos_ + ahead;
        return index < tokens.size() ? tokens[index] : tokens.back();
    }

    const Token& advance() noexcept {
        const Token& token = peek();
        if (!peek().is(TokenKind::End)) ++pos_;
        return token;
    }

    /// @brief The token just consumed.  Only meaningful after expect()/accept().
    [[nodiscard]] const Token& previous() const noexcept {
        return (*toks_)[pos_ == 0 ? 0 : pos_ - 1];
    }

    [[nodiscard]] bool check(TokenKind kind) const noexcept { return peek().is(kind); }
    [[nodiscard]] bool accept(TokenKind kind) noexcept {
        if (!check(kind)) return false;
        advance();
        return true;
    }

    [[nodiscard]] SourceSpan span_from(SourceLocation begin) const noexcept {
        return {begin, peek().span.begin};
    }

    void fail(ErrorCode code, std::string message, SourceSpan span) {
        out_.diagnostics.push_back(Diagnostic::error(code, std::move(message), span));
    }

    /// @brief Consume the next token when it has @p kind.
    /// @return The consumed token, or nullopt after recording the failure.
    std::optional<Token> expect(TokenKind kind, std::string_view context) {
        if (check(kind)) return advance();
        fail(ErrorCode::ExpectedToken,
             std::string{context} + ": expected " + to_string(kind)
                 + ", found " + to_string(peek().kind),
             peek().span);
        return std::nullopt;
    }

    /// @brief Skip forward to a token that can start the next construct.
    template <typename Pred>
    void sync(Pred is_start) {
        do { advance(); } while (!is_start(peek().kind)
                                 && !check(TokenKind::RBrace)
                                 && !check(TokenKind::End));
    }

    void sync_statement() { sync(is_statement_start); }
    void sync_metadata() { sync(is_metadata_start); }

    bool parse_header() {
        if (!check(TokenKind::KwZxconfig)) {
            fail(ErrorCode::MissingVersion,
                 "every zxconfig file opens with a version header ('zxconfig 1.1')",
                 peek().span);
            return false;
        }
        const auto header_span = peek().span;
        advance();

        if (!check(TokenKind::Number)) {
            fail(ErrorCode::MissingVersion, "version header needs a major version",
                 peek().span);
            return false;
        }
        auto major = parse_number(lexed_.spelling(peek()));
        if (!major) {
            fail(ErrorCode::BadNumber, std::string{major.error().message}, peek().span);
            return false;
        }
        out_.document.version_major = *major;
        advance();

        out_.document.version_minor = 0;
        if (accept(TokenKind::Dot)) {
            if (!check(TokenKind::Number)) {
                fail(ErrorCode::MissingVersion, "version header needs a minor version",
                     peek().span);
                return false;
            }
            auto minor = parse_number(lexed_.spelling(peek()));
            if (!minor) {
                fail(ErrorCode::BadNumber, std::string{minor.error().message}, peek().span);
                return false;
            }
            out_.document.version_minor = *minor;
            advance();
        }

        if (out_.document.version_major != 1 || out_.document.version_minor != 1) {
            fail(ErrorCode::UnsupportedVersion,
                 "zxconfig " + std::to_string(out_.document.version_major)
                     + "." + std::to_string(out_.document.version_minor)
                     + " is not supported; migrate to zxconfig 1.1 and warn/error CONDITION : message clauses",
                 header_span);
            return false;
        }
        return true;
    }

    void parse_statements(Menu& into, bool in_menu) {
        for (;;) {
            if (in_menu && check(TokenKind::RBrace)) return;
            if (check(TokenKind::End)) {
                if (in_menu) {
                    fail(ErrorCode::UnbalancedDelimiter,
                         "menu opened at " + std::to_string(into.span.begin.offset)
                             + " is never closed",
                         peek().span);
                }
                return;
            }
            if (accept(TokenKind::Semicolon)) continue;

            switch (peek().kind) {
                case TokenKind::KwExportWithPrefix:
                case TokenKind::KwPresetsDir:
                    parse_project_directive(&into == out_.document.root.get() && !in_menu);
                    break;
                case TokenKind::KwMenu:       parse_menu(into); break;
                case TokenKind::KwOption:     parse_option(into); break;
                case TokenKind::KwComputed:   parse_computed(into); break;
                case TokenKind::KwInclude:    parse_include(into); break;
                case TokenKind::KwGenerate:   parse_generate(); break;
                case TokenKind::KwValidate:
                    fail(ErrorCode::UnsupportedConstruct,
                         "validate assertions were removed in 1.1; use error BAD_CONDITION : \"message\"", peek().span);
                    advance(); sync_statement(); break;
                case TokenKind::KwWarn:       parse_validate(true); break;
                case TokenKind::KwError:      parse_validate(false); break;
                case TokenKind::KwAlias:      parse_alias(); break;
                case TokenKind::KwDeprecated: parse_deprecated(); break;
                case TokenKind::KwExport:
                case TokenKind::KwMacro:      parse_macro(); break;
                case TokenKind::Id:
                    if (peek(1).is(TokenKind::LParen) || peek(1).is(TokenKind::Dot)) parse_invocation(into);
                    else {
                        fail(ErrorCode::UnexpectedToken,
                             std::string{"expected a declaration or macro call, found "}
                                 + to_string(peek().kind),
                             peek().span);
                        sync_statement();
                    }
                    break;
                default:
                    fail(ErrorCode::UnexpectedToken,
                         std::string{"expected a declaration, found "}
                             + to_string(peek().kind),
                         peek().span);
                    sync_statement();
                    break;
            }
        }
    }

    void parse_project_directive(bool at_root) {
        const auto directive = advance();
        const bool prefix = directive.kind == TokenKind::KwExportWithPrefix;
        const auto keyword = prefix ? "export_with_prefix" : "presets_dir";
        const auto value = expect(TokenKind::String, keyword);
        if (!value) { sync_statement(); return; }
        if (!at_root || depth_ != 0 || expansion_depth_ != 0) {
            fail(ErrorCode::UnsupportedConstruct,
                 std::string{keyword} + " is only allowed at the root manifest's top level", directive.span);
            return;
        }
        const std::string text{lexed_.spelling(*value)};
        if (prefix) {
            if (export_prefix_declared_) {
                fail(ErrorCode::DuplicateSymbol, "export_with_prefix may be declared only once", directive.span);
                return;
            }
            export_prefix_declared_ = true;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                if (!letter && !(i > 0 && c >= '0' && c <= '9')) {
                    fail(ErrorCode::TypeMismatch,
                         "export_with_prefix must be empty or match [A-Za-z_][A-Za-z0-9_]*", value->span);
                    return;
                }
            }
            out_.document.export_prefix = text;
        } else {
            if (text.find('\0') != std::string::npos || text.find("${") != std::string::npos) {
                fail(ErrorCode::TypeMismatch, "presets_dir requires a literal path without NUL or interpolation", value->span);
                return;
            }
            out_.document.preset_dirs.push_back(text);
        }
    }

    void parse_menu(Menu& parent) {
        const auto begin = peek().span.begin;
        advance();

        auto menu = std::make_unique<Menu>();
        menu->file_id = file_id_;
        menu->span.begin = begin;
        const auto label = expect(TokenKind::String, "menu");
        if (!label) { sync_statement(); return; }
        menu->label = std::string{lexed_.spelling(*label)};

        if (accept(TokenKind::KwVisible)) {
            menu->visible = parse_expression();
        }
        menu->span.end = peek().span.begin;

        if (!expect(TokenKind::LBrace, "menu")) { sync_statement(); return; }
        parse_statements(*menu, true);
        if (!accept(TokenKind::RBrace)) {
            fail(ErrorCode::ExpectedToken, "menu: expected '}' to close the block",
                 peek().span);
        }
        parent.children.emplace_back(std::move(menu));
    }

    void parse_option(Menu& parent) {
        const auto begin = peek().span.begin;
        advance();

        Symbol symbol;
        symbol.span.begin = begin;
        symbol.file_id = file_id_;
        symbol.namespace_ = ns_;

        if (!expect(TokenKind::Id, "option")) { sync_statement(); return; }
        symbol.raw_name = std::string{lexed_.spelling(previous())};
        if (symbol.raw_name.starts_with("HOST_")) {
            fail(ErrorCode::DuplicateSymbol, "HOST_ names are reserved for host builtins", previous().span);
        }
        symbol.name = ns_.empty() ? symbol.raw_name : ns_ + "." + symbol.raw_name;

        if (!parse_type(symbol)) { sync_statement(); return; }

        if (accept(TokenKind::Eq)) {
            auto value = parse_default_value(symbol.type);
            if (value != nullptr) {
                symbol.meta.defaults.emplace_back(std::move(value), nullptr);
            }
        }

        if (accept(TokenKind::LBrace)) {
            parse_metadata(symbol.meta);
            if (!accept(TokenKind::RBrace)) {
                fail(ErrorCode::ExpectedToken, "option: expected '}' to close metadata",
                     peek().span);
                sync_statement();
                return;
            }
        }
        symbol.span.end = peek().span.begin;
        parent.children.emplace_back(std::move(symbol));
    }

    void parse_computed(Menu& parent) {
        const auto begin = peek().span.begin;
        advance();

        Symbol symbol;
        symbol.span.begin = begin;
        symbol.file_id = file_id_;
        symbol.namespace_ = ns_;
        symbol.computed = true;

        if (!expect(TokenKind::Id, "computed")) { sync_statement(); return; }
        symbol.raw_name = std::string{lexed_.spelling(previous())};
        if (symbol.raw_name.starts_with("HOST_")) {
            fail(ErrorCode::DuplicateSymbol, "HOST_ names are reserved for host builtins", previous().span);
        }
        symbol.name = ns_.empty() ? symbol.raw_name : ns_ + "." + symbol.raw_name;

        if (!parse_type(symbol)) { sync_statement(); return; }

        if (accept(TokenKind::Eq)) {
            // Computed and configurable symbols share the expression grammar.
            auto value = parse_default_value(symbol.type);
            if (value != nullptr) {
                symbol.meta.defaults.emplace_back(std::move(value), nullptr);
            }
        } else if (accept(TokenKind::LBrace)) {
            parse_default_block(symbol.meta);
        } else {
            fail(ErrorCode::ExpectedToken,
                 "computed: expected '=' or a conditional block", peek().span);
            sync_statement();
            return;
        }
        symbol.span.end = peek().span.begin;
        parent.children.emplace_back(std::move(symbol));
    }

    bool parse_type(Symbol& symbol) {
        if (!expect(TokenKind::Colon, "option")) return false;
        if (!is_type_token(peek().kind)) {
            fail(ErrorCode::UnknownType,
                 std::string{"unknown type "} + to_string(peek().kind), peek().span);
            return false;
        }
        const auto kind = peek().kind;
        symbol.type = type_of(kind);
        advance();

        if (kind == TokenKind::KwList) {
            if (!expect(TokenKind::Lt, "list")) return false;
            if (!is_type_token(peek().kind)) {
                fail(ErrorCode::UnknownType,
                     std::string{"unknown element type "} + to_string(peek().kind),
                     peek().span);
                return false;
            }
            symbol.element = type_of(peek().kind);
            advance();
            if (!expect(TokenKind::Gt, "list")) return false;
        }
        return true;
    }

    void parse_include(Menu& parent) {        const auto begin = peek().span.begin;
        advance();

        if (!expect(TokenKind::String, "include")) { sync_statement(); return; }
        const auto path = std::string{lexed_.spelling(previous())};

        if (check(TokenKind::Id) && lexed_.spelling(peek()) == "as") {
            fail(ErrorCode::UnsupportedConstruct,
                 "include 'as' was removed; declare 'namespace NAME' after the included file's version header",
                 peek().span);
            advance();
            (void)parse_name("removed include alias");
        }

        IncludeDirective directive;
        directive.path = path;
        directive.span = {begin, peek().span.begin};

        if (accept(TokenKind::KwVisible)) {
            directive.condition = parse_expression();
        }

        // The condition has to exist before the file is parsed, because the
        // folded result is what the children carry.
        ExprPtr condition_copy =
            directive.condition != nullptr ? clone_expr(*directive.condition) : nullptr;

        const auto* current = arena_.find(file_id_);
        const auto base = std::filesystem::path{
            current->canonical.empty() ? current->path : current->canonical}
            .parent_path().string();

        const auto before = arena_.count();
        auto loaded = arena_.load(path, base, root_, file_id_, begin.offset);
        if (!loaded) {
            out_.diagnostics.push_back(loaded.error());
            out_.document.includes.push_back(std::move(directive));
            return;
        }
        directive.file_id = *loaded;
        directive.resolved = arena_.find(*loaded)->canonical;

        const bool already_loaded = arena_.count() == before;
        if (already_loaded) {
            if (const auto exports = file_macro_exports_.find(*loaded);
                exports != file_macro_exports_.end()) {
                import_macros(exports->second);
            }
            out_.diagnostics.push_back(Diagnostic::warning(
                ErrorCode::AlreadyLoaded,
                "'" + path + "' is included more than once; skipped",
                directive.span));
            record_include_gate(*loaded, condition_copy);
            out_.document.includes.push_back(std::move(directive));
            return;
        }

        std::string included_namespace;
        if (directive.condition == nullptr) {
            parse_file(*loaded, parent, depth_ + 1, &included_namespace);
        } else {
            // Parse into a scratch menu, fold the condition into every child,
            // then splice.  The scratch menu itself never reaches the tree.
            Menu scratch;
            if (parse_file(*loaded, scratch, depth_ + 1, &included_namespace)) {
                for (auto& child : scratch.children) {
                    if (auto* symbol = std::get_if<Symbol>(&child)) {
                        fold_condition(*condition_copy, symbol->meta.visible);
                    } else if (auto& menu = std::get<MenuPtr>(child)) {
                        fold_condition(*condition_copy, menu->visible);
                    }
                    parent.children.push_back(std::move(child));
                }
            }
        }
        directive.namespace_ = std::move(included_namespace);
        record_include_gate(*loaded, condition_copy);
        out_.document.includes.push_back(std::move(directive));
    }

    void parse_generate() {
        const auto begin = peek().span.begin;
        advance();

        GenerateDirective directive;
        directive.span.begin = begin;
        switch (peek().kind) {
            case TokenKind::KwHeader:   directive.backend = GenerateDirective::Backend::Header; break;
            case TokenKind::KwMakefile: directive.backend = GenerateDirective::Backend::Makefile; break;
            case TokenKind::KwJson:     directive.backend = GenerateDirective::Backend::Json; break;
            case TokenKind::KwCmake:    directive.backend = GenerateDirective::Backend::Cmake; break;
            case TokenKind::KwTemplate: directive.backend = GenerateDirective::Backend::Template; break;
            default:
                fail(ErrorCode::UnknownBackend,
                     std::string{"unknown generator backend "} + to_string(peek().kind),
                     peek().span);
                sync_statement();
                return;
        }
        advance();

        if (!expect(TokenKind::String, "generate")) { sync_statement(); return; }
        directive.output = std::string{lexed_.spelling(previous())};

        if (directive.backend == GenerateDirective::Backend::Template) {
            if (!accept(TokenKind::KwFrom)) {
                fail(ErrorCode::ExpectedToken,
                     "generate template: expected 'from'", peek().span);
                sync_statement();
                return;
            }
            if (!expect(TokenKind::String, "generate template")) { sync_statement(); return; }
            directive.template_path = std::string{lexed_.spelling(previous())};
        }

        // An explicit header prefix overrides the project-wide setting.
        // Keep explicit empty strings distinct from an inherited prefix.
        if (accept(TokenKind::LBrace)) {
            while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
                if (accept(TokenKind::Semicolon)) continue;
                if (check(TokenKind::Id) && lexed_.spelling(peek()) == "prefix") {
                    advance();
                    if (check(TokenKind::String)) {
                        directive.prefix = std::string{lexed_.spelling(peek())};
                        directive.prefix_override = true;
                        advance();
                    } else {
                        fail(ErrorCode::ExpectedToken, "prefix: expected a string",
                             peek().span);
                        advance();
                    }
                    continue;
                }
                fail(ErrorCode::UnexpectedToken,
                     std::string{"generate: unknown option "} + to_string(peek().kind),
                     peek().span);
                advance();
            }
            if (!accept(TokenKind::RBrace)) {
                fail(ErrorCode::ExpectedToken, "generate: expected '}' to close options",
                     peek().span);
            }
        }
        directive.span.end = peek().span.begin;
        out_.document.generates.push_back(std::move(directive));
    }

    void parse_validate(bool warning) {
        const auto begin = peek().span.begin;
        advance();

        ValidationClause clause;
        clause.warning = warning;
        clause.span.begin = begin;

        clause.condition = parse_expression();
        if (!clause.condition || !expect(TokenKind::Colon, warning ? "warn" : "error")
            || !expect(TokenKind::String, warning ? "warn" : "error")) {
            sync_statement();
            return;
        }
        clause.message = std::string{lexed_.spelling(previous())};
        clause.span.end = peek().span.begin;
        out_.document.validations.push_back(std::move(clause));
    }

    void parse_alias() {
        const auto begin = peek().span.begin;
        advance();

        AliasDirective alias;
        alias.span.begin = begin;

        auto alias_name = parse_name("alias");
        if (!alias_name) { sync_statement(); return; }
        alias.alias = local_name(std::move(*alias_name));

        if (!expect(TokenKind::Eq, "alias")) { sync_statement(); return; }
        auto target = parse_name("alias target");
        if (!target) { sync_statement(); return; }
        // Targets are references: kept as written and resolved with the
        // declaring file's namespace-first rule after the whole forest parses.
        alias.target = std::move(*target);

        alias.span.end = peek().span.begin;
        out_.document.aliases.push_back(std::move(alias));
    }

    void parse_deprecated() {
        const auto begin = peek().span.begin;
        advance();

        DeprecatedDirective directive;
        directive.span.begin = begin;

        auto old_name = parse_name("deprecated");
        if (!old_name) { sync_statement(); return; }
        // The old name is a legacy cache spelling: kept exactly as written.
        directive.old_name = std::move(*old_name);

        if (!expect(TokenKind::Arrow, "deprecated")) { sync_statement(); return; }
        auto new_name = parse_name("deprecated replacement");
        if (!new_name) { sync_statement(); return; }
        directive.new_name = std::move(*new_name);

        if (accept(TokenKind::LBrace)) {
            while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
                if (accept(TokenKind::KwMessage)) {
                    if (check(TokenKind::String)) {
                        directive.message = std::string{lexed_.spelling(peek())};
                        advance();
                    } else {
                        fail(ErrorCode::ExpectedToken, "message: expected a string",
                             peek().span);
                        advance();
                    }
                    continue;
                }
                fail(ErrorCode::UnexpectedToken,
                     std::string{"deprecated: expected 'message', found "}
                         + to_string(peek().kind),
                     peek().span);
                advance();
            }
            if (!accept(TokenKind::RBrace)) {
                fail(ErrorCode::ExpectedToken, "deprecated: expected '}'", peek().span);
            }
        }
        directive.span.end = peek().span.begin;
        out_.document.deprecated.push_back(std::move(directive));
    }

    /// @brief Raw source spelling of an argument token.
    std::string raw_arg_text(const Token& token) {
        const auto* file = arena_.find(file_id_);
        const auto begin = token.span.begin.offset;
        const auto end = token.span.end.offset;
        if (token.is(TokenKind::String) && end - begin >= 2) {
            return std::string{std::string_view{file->text}.substr(begin + 1, end - begin - 2)};
        }
        return std::string{std::string_view{file->text}.substr(begin, end - begin)};
    }

    /// @brief Replace `${param}` with the matching argument.
    static std::string expand_body(const Macro& macro, const std::vector<std::string>& args) {
        std::unordered_map<std::string_view, std::string_view> bindings;
        for (std::size_t i = 0; i < macro.params.size() && i < args.size(); ++i) {
            bindings.emplace(macro.params[i], args[i]);
        }

        std::string out;
        out.reserve(macro.body.size());
        std::size_t at = 0;
        while (at < macro.body.size()) {
            if (macro.body[at] == '$' && at + 1 < macro.body.size() && macro.body[at + 1] == '{') {
                const auto close = macro.body.find('}', at + 2);
                if (close == std::string::npos) {
                    out += macro.body.substr(at);
                    break;
                }
                const auto name = std::string_view{macro.body}.substr(at + 2, close - at - 2);
                if (const auto it = bindings.find(name); it != bindings.end()) {
                    out += it->second;
                } else {
                    out += macro.body.substr(at, close - at + 1);
                }
                at = close + 1;
                continue;
            }
            out.push_back(macro.body[at++]);
        }
        return out;
    }

    void parse_macro() {
        const auto begin = peek().span.begin;
        const bool exported = accept(TokenKind::KwExport);
        if (!expect(TokenKind::KwMacro, "macro")) { sync_statement(); return; }

        Macro macro;
        macro.exported = exported;
        macro.span.begin = begin;

        if (!expect(TokenKind::Id, "macro")) { sync_statement(); return; }
        macro.name = std::string{lexed_.spelling(previous())};
        macro.namespace_ = ns_;
        macro.name = local_name(std::move(macro.name));

        if (!expect(TokenKind::LParen, "macro")) { sync_statement(); return; }
        while (!check(TokenKind::RParen) && !check(TokenKind::End)) {
            if (accept(TokenKind::Comma)) continue;
            if (check(TokenKind::Id)) {
                macro.params.emplace_back(lexed_.spelling(peek()));
                advance();
                continue;
            }
            fail(ErrorCode::ExpectedToken, "macro parameters: expected a name", peek().span);
            advance();
        }
        if (!expect(TokenKind::RParen, "macro")) { sync_statement(); return; }
        if (!expect(TokenKind::LBrace, "macro")) { sync_statement(); return; }

        const auto body_open = previous().span.end.offset;
        std::uint32_t depth = 1;
        std::uint32_t body_close = body_open;
        while (depth > 0 && !check(TokenKind::End)) {
            if (check(TokenKind::LBrace)) {
                ++depth;
                advance();
            } else if (check(TokenKind::RBrace)) {
                body_close = peek().span.begin.offset;
                --depth;
                advance();
            } else {
                advance();
            }
        }
        if (depth != 0) {
            fail(ErrorCode::UnbalancedDelimiter,
                 "macro '" + macro.name + "' body is never closed", macro.span);
            return;
        }

        const auto* file = arena_.find(file_id_);
        macro.body = std::string{
            std::string_view{file->text}.substr(body_open, body_close - body_open)};
        macro.span.end = previous().span.begin;

        auto& scope = macro_scopes_.back();
        if (scope.contains(macro.name)) {
            fail(ErrorCode::DuplicateSymbol,
                 "macro '" + macro.name + "' is defined twice", macro.span);
            return;
        }
        out_.document.macros.push_back(macro);
        auto definition = std::make_shared<const Macro>(std::move(macro));
        scope.emplace(definition->name, definition);
        file_macro_definitions_[definition->span.begin.file_id].emplace(definition->name, definition);
    }

    void parse_invocation(Menu& into) {
        const auto name_token = peek();
        auto parsed_name = parse_name("macro call");
        if (!parsed_name) { sync_statement(); return; }
        const auto name = std::move(*parsed_name);

        if (!expect(TokenKind::LParen, "macro call")) { sync_statement(); return; }
        std::vector<std::string> args;
        while (!check(TokenKind::RParen) && !check(TokenKind::End)) {
            if (accept(TokenKind::Comma)) continue;
            const auto token = peek();
            if (token.is(TokenKind::Id)) {
                auto argument = parse_name("macro argument");
                if (!argument) { sync_statement(); return; }
                args.push_back(std::move(*argument));
                continue;
            }
            if (token.is(TokenKind::Number) || token.is(TokenKind::Hex) || token.is(TokenKind::String)) {
                args.push_back(raw_arg_text(token));
                advance();
                continue;
            }
            fail(ErrorCode::ExpectedToken, "macro call: expected an argument", token.span);
            advance();
        }
        if (!expect(TokenKind::RParen, "macro call")) { sync_statement(); return; }

        const auto macro = find_macro(name);
        if (macro == nullptr) {
            fail(ErrorCode::UndeclaredMacro,
                 "'" + std::string{name} + "' is not a macro in scope", name_token.span);
            return;
        }
        if (macro->params.size() != args.size()) {
            fail(ErrorCode::ArityMismatch,
                 "macro '" + macro->name + "' takes " + std::to_string(macro->params.size())
                     + " argument(s), got " + std::to_string(args.size()),
                 name_token.span);
            return;
        }

        expanding_macros_.push_back(macro);
        parse_expanded(expand_body(*macro, args),
                       std::string{name} + "()", into, name_token.span);
        expanding_macros_.pop_back();
    }

    /// @brief Parse expanded macro text as declarations into @p into.
    void parse_expanded(std::string text, std::string label,
                        Menu& into, SourceSpan call_site) {
        if (expansion_depth_ >= k_max_expansion_depth) {
            fail(ErrorCode::CircularDependency,
                 "macro expansion nested deeper than "
                     + std::to_string(k_max_expansion_depth),
                 call_site);
            return;
        }

        const auto* invoker = arena_.find(file_id_);
        auto admitted = arena_.add(invoker->path + "#macro " + label, std::move(text));
        if (!admitted) {
            out_.diagnostics.push_back(admitted.error());
            return;
        }
        // A synthetic expansion belongs to the invoking file's gate: when
        // the invoker is deactivated, so is everything the macro declared.
        record_include_gate(*admitted, nullptr);

        auto expanded = lex(arena_, *admitted);
        for (auto& failure : expanded.errors) {
            out_.diagnostics.push_back(failure.diagnostic);
        }

        auto saved_lexed = std::move(lexed_);
        const auto saved_tokens = toks_;
        (void)saved_tokens;
        const auto saved_pos = pos_;
        const auto saved_file = file_id_;

        lexed_ = std::move(expanded);
        toks_ = &lexed_.tokens;
        pos_ = 0;
        file_id_ = *admitted;
        out_.document.file_namespaces[file_id_] = ns_;
        ++expansion_depth_;

        parse_statements(into, false);

        --expansion_depth_;
        lexed_ = std::move(saved_lexed);
        toks_ = &lexed_.tokens;
        pos_ = saved_pos;
        file_id_ = saved_file;
    }

    void parse_metadata(Metadata& meta) {
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            if (accept(TokenKind::Semicolon)) continue;

            switch (peek().kind) {
                case TokenKind::KwLabel:
                    advance();
                    if (check(TokenKind::String)) {
                        meta.label = std::string{lexed_.spelling(peek())};
                        advance();
                    } else {
                        fail(ErrorCode::ExpectedToken, "label: expected a string", peek().span);
                        advance();
                    }
                    break;
                case TokenKind::KwHelp:
                    advance();
                    if (check(TokenKind::String)) {
                        meta.help = std::string{lexed_.spelling(peek())};
                        advance();
                    } else {
                        fail(ErrorCode::ExpectedToken, "help: expected a string", peek().span);
                        advance();
                    }
                    break;
                case TokenKind::KwTags:
                    advance();
                    parse_tag_list(meta);
                    break;
                case TokenKind::KwRange:
                    advance();
                    parse_range(meta);
                    break;
                case TokenKind::KwWhen:
                    advance();
                    meta.when = parse_expression();
                    break;
                case TokenKind::KwVisible:
                    advance();
                    meta.visible = parse_expression();
                    break;
                case TokenKind::KwEnabled:
                    advance();
                    meta.enabled = parse_expression();
                    break;
                case TokenKind::KwImplies:
                    advance();
                    if (auto condition = parse_expression()) {
                        meta.implies.push_back(std::move(condition));
                    }
                    break;
                case TokenKind::KwOrder:
                    advance();
                    parse_order(meta);
                    break;
                case TokenKind::KwPattern:
                    advance();
                    if (check(TokenKind::String)) {
                        meta.pattern = std::string{lexed_.spelling(peek())};
                        advance();
                    } else {
                        fail(ErrorCode::ExpectedToken, "pattern: expected a string", peek().span);
                        advance();
                    }
                    break;
                case TokenKind::KwPath:
                    advance();
                    parse_path_constraints(meta);
                    break;
                case TokenKind::KwChoices:
                    advance();
                    parse_choices(meta);
                    break;
                case TokenKind::KwChoice:
                    advance();
                    parse_choice_meta(meta);
                    break;
                case TokenKind::KwDefault:
                    advance();
                    if (!accept(TokenKind::LBrace)) {
                        fail(ErrorCode::ExpectedToken, "default: expected '{'", peek().span);
                        sync_metadata();
                        break;
                    }
                    parse_default_block(meta);
                    break;
                case TokenKind::KwConflict:
                    advance();
                    parse_conflict(meta);
                    break;
                case TokenKind::KwWarn:
                case TokenKind::KwError: {
                    const bool warning = peek().kind == TokenKind::KwWarn;
                    advance();
                    parse_metadata_warn(meta, warning);
                    break;
                }
                default:
                    fail(ErrorCode::UnexpectedToken,
                         std::string{"expected metadata, found "} + to_string(peek().kind),
                         peek().span);
                    sync_metadata();
                    break;
            }
        }
    }

    void parse_tag_list(Metadata& meta) {
        if (!expect(TokenKind::LBracket, "tags")) { sync_metadata(); return; }
        for (;;) {
            if (check(TokenKind::String) || check(TokenKind::Id)) {
                meta.tags.emplace_back(lexed_.spelling(peek()));
                advance();
            } else if (!check(TokenKind::RBracket)) {
                fail(ErrorCode::ExpectedToken, "tags: expected a name", peek().span);
                advance();
            }
            if (accept(TokenKind::Comma)) continue;
            break;
        }
        if (!expect(TokenKind::RBracket, "tags")) sync_metadata();
    }

    void parse_range(Metadata& meta) {
        auto lo = parse_expression();
        if (lo == nullptr) { sync_metadata(); return; }
        if (!expect(TokenKind::DotDot, "range")) { sync_metadata(); return; }
        auto hi = parse_expression();
        if (hi == nullptr) { sync_metadata(); return; }

        const auto* lo_lit = lo->is(ExprKind::IntLit) ? &lo->number : nullptr;
        const auto* hi_lit = hi->is(ExprKind::IntLit) ? &hi->number : nullptr;
        if (lo_lit == nullptr || hi_lit == nullptr) {
            fail(ErrorCode::TypeMismatch, "range bounds must be integer literals",
                 {lo->span.begin, hi->span.end});
            return;
        }
        meta.range = Range{*lo_lit, *hi_lit};
    }

    void parse_order(Metadata& meta) {
        auto value = parse_expression();
        if (value == nullptr) { sync_metadata(); return; }
        if (value->is(ExprKind::Unary) && value->args.size() == 1
            && value->op.is(TokenKind::Minus) && value->args.front()->is(ExprKind::IntLit)) {
            meta.order = -value->args.front()->number;
            return;
        }
        if (!value->is(ExprKind::IntLit)) {
            fail(ErrorCode::TypeMismatch, "order must be an integer", value->span);
            return;
        }
        meta.order = value->number;
    }

    void parse_path_constraints(Metadata& meta) {
        for (;;) {
            if (check(TokenKind::KwMustExist)) {
                meta.path_constraints.emplace_back("must_exist");
                advance();
            } else if (check(TokenKind::KwIsDir)) {
                meta.path_constraints.emplace_back("is_dir");
                advance();
            } else if (!check(TokenKind::Pipe) && !check(TokenKind::RBrace)) {
                fail(ErrorCode::ExpectedToken,
                     "path: expected must_exist or is_dir", peek().span);
                advance();
            }
            if (accept(TokenKind::Pipe)) continue;
            break;
        }
    }

    void parse_choices(Metadata& meta) {
        for (;;) {
            if (check(TokenKind::Id)) {
                meta.choices.emplace_back(lexed_.spelling(peek()));
                advance();
            } else if (!check(TokenKind::Pipe) && !check(TokenKind::RBrace)) {
                fail(ErrorCode::ExpectedToken, "choices: expected a name", peek().span);
                advance();
            }
            if (accept(TokenKind::Pipe)) continue;
            break;
        }
    }

    void parse_choice_meta(Metadata& meta) {
        if (!expect(TokenKind::Id, "choice")) { sync_metadata(); return; }

        ChoiceMeta choice;
        choice.name = std::string{lexed_.spelling(previous())};

        if (!expect(TokenKind::LBrace, "choice")) { sync_metadata(); return; }
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            if (accept(TokenKind::Semicolon)) continue;
            if (accept(TokenKind::KwLabel)) {
                if (check(TokenKind::String)) {
                    choice.label = std::string{lexed_.spelling(peek())};
                    advance();
                } else {
                    fail(ErrorCode::ExpectedToken, "choice label: expected a string",
                         peek().span);
                    advance();
                }
            } else if (accept(TokenKind::KwHelp)) {
                if (check(TokenKind::String)) {
                    choice.help = std::string{lexed_.spelling(peek())};
                    advance();
                } else {
                    fail(ErrorCode::ExpectedToken, "choice help: expected a string",
                         peek().span);
                    advance();
                }
            } else if (accept(TokenKind::KwVisible)) {
                choice.visible = parse_expression();
            } else {
                fail(ErrorCode::UnexpectedToken,
                     std::string{"choice: expected label, help, or visible, found "}
                         + to_string(peek().kind),
                     peek().span);
                advance();
            }
        }
        if (!accept(TokenKind::RBrace)) {
            fail(ErrorCode::ExpectedToken, "choice: expected '}'", peek().span);
        }
        meta.choice_meta.push_back(std::move(choice));
    }

    void parse_conflict(Metadata& meta) {
        auto target = parse_name("conflict");
        if (!target) { sync_metadata(); return; }

        ConflictClause clause;
        // References resolve like expressions: declaring-file namespace
        // first, then global (decided after the whole forest parses).
        clause.symbol = std::move(*target);

        if (accept(TokenKind::LBrace)) {
            while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
                if (accept(TokenKind::Semicolon)) continue;
                if (accept(TokenKind::KwResolve)) {
                    if (check(TokenKind::String)) {
                        clause.message = std::string{lexed_.spelling(peek())};
                        advance();
                    } else {
                        fail(ErrorCode::ExpectedToken, "resolve: expected a string",
                             peek().span);
                        advance();
                    }
                } else if (check(TokenKind::KwError)) {
                    // `error "..."` is the spec's hard-fail policy: the
                    // conflict blocks the save instead of prompting.
                    advance();
                    if (check(TokenKind::String)) {
                        clause.message = std::string{lexed_.spelling(peek())};
                        clause.policy = ConflictPolicy::Error;
                        advance();
                    } else {
                        fail(ErrorCode::ExpectedToken, "error: expected a string",
                             peek().span);
                        advance();
                    }
                } else {
                    fail(ErrorCode::UnexpectedToken,
                         std::string{"conflict: expected 'resolve' or 'error', found "}
                             + to_string(peek().kind),
                         peek().span);
                    advance();
                }
            }
            if (!accept(TokenKind::RBrace)) {
                fail(ErrorCode::ExpectedToken, "conflict: expected '}'", peek().span);
            }
        }
        meta.conflicts.push_back(std::move(clause));
    }

    void parse_metadata_warn(Metadata& meta, bool warning) {
        auto condition = parse_expression();
        if (condition == nullptr) { sync_metadata(); return; }
        if (!expect(TokenKind::Colon, "warn")) { sync_metadata(); return; }
        if (!expect(TokenKind::String, "warn")) { sync_metadata(); return; }

        meta.warnings.push_back(WarnClause{
            std::string{lexed_.spelling(previous())}, std::move(condition), warning});
    }

    void parse_default_block(Metadata& meta) {
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            auto value = parse_expression();
            if (value == nullptr) { sync_metadata(); continue; }
            ExprPtr condition;
            if (accept(TokenKind::KwWhen)) {
                condition = parse_expression();
            }
            meta.defaults.emplace_back(std::move(value), std::move(condition));
        }
        if (!accept(TokenKind::RBrace)) {
            fail(ErrorCode::ExpectedToken, "default: expected '}'", peek().span);
        }
    }

    /// @brief Every default accepts the same expression grammar.
    ExprPtr parse_default_value(ValueType) {
        return parse_expression();
    }

    ExprPtr parse_sequence(TokenKind open, TokenKind close) {
        const auto begin = peek().span.begin;
        if (!expect(open, "default")) { sync_statement(); return nullptr; }

        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::SequenceLit;
        node->op.kind = open;
        node->span.begin = begin;

        while (!check(close) && !check(TokenKind::End)) {
            auto item = parse_expression();
            if (item == nullptr) return nullptr;
            node->args.push_back(std::move(item));
            if (!accept(TokenKind::Comma)) break;
        }
        if (!expect(close, "default")) { sync_statement(); return nullptr; }
        node->span.end = peek().span.begin;
        return node;
    }

    ExprPtr parse_expression() { return parse_ternary(); }

    ExprPtr parse_ternary() {
        auto condition = parse_binary(1);
        if (condition == nullptr || !accept(TokenKind::Question)) return condition;

        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Ternary;
        node->span.begin = condition->span.begin;
        node->args.push_back(std::move(condition));

        node->args.push_back(parse_ternary());
        if (!expect(TokenKind::Colon, "ternary")) return nullptr;
        node->args.push_back(parse_ternary());

        if (node->args.back() != nullptr) node->span.end = node->args.back()->span.end;
        return node;
    }

    ExprPtr parse_binary(int min_precedence) {
        auto lhs = parse_unary();
        if (lhs == nullptr) return nullptr;

        for (;;) {
            const int prec = precedence(peek().kind);
            if (prec < min_precedence || prec == 0) return lhs;

            const auto op = peek();
            const auto begin = lhs->span.begin;
            advance();
            auto rhs = parse_binary(prec + 1);
            if (rhs == nullptr) return nullptr;
            // Read the span before moving rhs.  The order in which function
            // arguments are evaluated is not specified; reading rhs->span in
            // the same call as std::move(rhs) is therefore undefined.
            const auto end = rhs->span.end;
            lhs = make_binary({begin, end}, op.kind, std::move(lhs), std::move(rhs));
        }
    }

    ExprPtr parse_unary() {
        if (check(TokenKind::Bang) || check(TokenKind::Minus) || check(TokenKind::KwNot)
            || check(TokenKind::Tilde) || check(TokenKind::Plus)) {
            const auto op = peek();
            advance();
            auto operand = parse_unary();
            if (operand == nullptr) return nullptr;

            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->span = {op.span.begin, operand->span.end};
            node->op = op;
            node->args.push_back(std::move(operand));
            return node;
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        auto expr = parse_primary();
        if (expr == nullptr) return nullptr;

        while (accept(TokenKind::KwFallback)) {
            auto value = parse_expression();
            if (value == nullptr) return nullptr;

            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Fallback;
            node->span = {expr->span.begin, value->span.end};
            node->args.push_back(std::move(expr));
            node->args.push_back(std::move(value));
            expr = std::move(node);
        }
        return expr;
    }

    ExprPtr parse_primary() {
        const auto token = peek();
        switch (token.kind) {
            case TokenKind::Number: {
                advance();
                auto value = parse_number(lexed_.spelling(token));
                if (!value) {
                    fail(ErrorCode::BadNumber, std::string{value.error().message}, token.span);
                    return nullptr;
                }
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::IntLit;
                node->span = token.span;
                node->number = *value;
                return node;
            }
            case TokenKind::Hex: {
                advance();
                const auto spelling = lexed_.spelling(token).substr(2);
                std::int64_t value = 0;
                const auto* first = spelling.data();
                const auto* last = spelling.data() + spelling.size();
                const auto parsed = std::from_chars(first, last, value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != last) {
                    fail(ErrorCode::BadNumber,
                         "'" + std::string{lexed_.spelling(token)} + "' is out of range",
                         token.span);
                    return nullptr;
                }
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::IntLit;
                node->span = token.span;
                node->number = value;
                return node;
            }
            case TokenKind::String: {
                advance();
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::StringLit;
                node->span = token.span;
                node->text = std::string{lexed_.spelling(token)};
                return node;
            }
            case TokenKind::Id: {
                auto parsed_name = parse_name("expression");
                if (!parsed_name) return nullptr;
                const auto& name = *parsed_name;
                if (name == "true" || name == "false") {
                    auto node = std::make_unique<Expr>();
                    node->kind = ExprKind::BoolLit;
                    node->span = token.span;
                    node->number = name == "true" ? 1 : 0;
                    return node;
                }
                if (check(TokenKind::LParen)) return parse_call(token);
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Ident;
                node->span = {token.span.begin, previous().span.end};
                node->text = std::string{name};
                return node;
            }
            case TokenKind::LParen: {
                advance();
                auto inner = parse_expression();
                if (inner == nullptr) return nullptr;
                if (!expect(TokenKind::RParen, "grouped expression")) return nullptr;
                inner->span.end = peek(0).span.begin;
                return inner;
            }
            case TokenKind::Dollar: {
                advance();
                if (!expect(TokenKind::LBrace, "variable reference")) return nullptr;
                auto name = parse_name("variable reference");
                if (!name || !expect(TokenKind::RBrace, "variable reference")) return nullptr;
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Ident;
                node->op.kind = TokenKind::Dollar;
                node->span = {token.span.begin, previous().span.end};
                node->text = std::move(*name);
                return node;
            }
            case TokenKind::LBrace:
                return parse_sequence(TokenKind::LBrace, TokenKind::RBrace);
            case TokenKind::LBracket:
                return parse_sequence(TokenKind::LBracket, TokenKind::RBracket);
            default:
                fail(ErrorCode::UnexpectedToken,
                     std::string{"expected an expression, found "} + to_string(token.kind),
                     token.span);
                advance();
                return nullptr;
        }
    }

    ExprPtr parse_call(const Token& name_token) {
        const auto name = lexed_.spelling(name_token);
        if (!is_builtin(name)) {
            fail(ErrorCode::UnknownBuiltin,
                 "'" + std::string{name} + "' is not a builtin function", name_token.span);
            return nullptr;
        }
        advance();  // (

        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Call;
        node->span.begin = name_token.span.begin;
        node->text = std::string{name};

        if (!check(TokenKind::RParen)) {
            for (;;) {
                auto arg = parse_expression();
                if (arg == nullptr) return nullptr;
                node->args.push_back(std::move(arg));
                if (!accept(TokenKind::Comma)) break;
            }
        }
        if (!expect(TokenKind::RParen, "call")) return nullptr;
        node->span.end = peek(0).span.begin;

        if (node->args.size() != builtin_arity(name)) {
            fail(ErrorCode::ArityMismatch,
                 std::string{name} + " takes " + std::to_string(builtin_arity(name))
                     + " argument(s), got " + std::to_string(node->args.size()),
                 node->span);
            return nullptr;
        }
        return node;
    }

    void link_menu(Menu& menu) {
        ++stats_.menus;
        for (auto& child : menu.children) {
            if (auto* symbol = std::get_if<Symbol>(&child)) {
                ++stats_.symbols;
                const auto [it, inserted] =
                    out_.document.symbols.emplace(symbol->name, symbol);
                if (!inserted) {
                    fail(ErrorCode::DuplicateSymbol,
                         "'" + symbol->name + "' is declared more than once",
                         symbol->span);
                }
            } else if (auto& sub = std::get<MenuPtr>(child)) {
                link_menu(*sub);
            }
        }
    }

    ParseResult& out_;
    SourceArena& arena_;
    std::string root_;
    std::uint32_t root_file_id_{0};
    LexResult lexed_;
    const std::vector<Token>* toks_{nullptr};
    std::size_t pos_{0};
    std::uint32_t file_id_{0};
    std::string ns_;
    std::uint32_t depth_{0};
    std::uint32_t expansion_depth_{0};
    bool export_prefix_declared_{false};
    std::vector<std::uint32_t> parsing_;
    std::vector<MacroScope> macro_scopes_;
    std::unordered_map<std::uint32_t, MacroScope> file_macro_exports_;
    std::unordered_map<std::uint32_t, MacroScope> file_macro_definitions_;
    std::vector<MacroRef> expanding_macros_;
    DocumentStats stats_;
};

}  // namespace

bool ParseResult::ok() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) {
        return d.severity == Severity::Error || d.severity == Severity::Fatal;
    });
}

ExprPtr clone_expr(const Expr& expr) {
    auto copy = std::make_unique<Expr>();
    copy->kind = expr.kind;
    copy->span = expr.span;
    copy->op = expr.op;
    copy->text = expr.text;
    copy->number = expr.number;
    copy->args.reserve(expr.args.size());
    for (const auto& arg : expr.args) {
        copy->args.push_back(arg != nullptr ? clone_expr(*arg) : nullptr);
    }
    return copy;
}

DocumentStats document_stats(const Document& document) noexcept {
    std::size_t symbols = 0;
    std::size_t menus = 0;

    struct Walk {
        const Document& doc;
        std::size_t& symbols;
        std::size_t& menus;

        void menu(const Menu& m) const {
            ++menus;
            for (const auto& child : m.children) {
                if (std::holds_alternative<Symbol>(child)) ++symbols;
                else if (const auto& sub = std::get<MenuPtr>(child)) menu(*sub);
            }
        }
    } walk{document, symbols, menus};
    if (document.root != nullptr) walk.menu(*document.root);
    return {symbols, menus};
}

ParseResult parse_document(SourceArena& arena,
                           std::uint32_t file_id,
                           std::string_view root) {
    ParseResult result;
    if (const auto* file = arena.find(file_id)) {
        const auto path = file->canonical.empty() ? file->path : file->canonical;
        std::error_code ec;
        result.document.base_directory = std::filesystem::absolute(
            std::filesystem::path{path}.parent_path().empty() ? "." : std::filesystem::path{path}.parent_path(), ec).string();
    }
    Parser parser{result, arena, root, file_id};
    parser.parse_file(file_id, *result.document.root, 0);
    parser.link();
    return result;
}

}  // namespace zxconfig
