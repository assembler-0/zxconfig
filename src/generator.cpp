// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/generator.hpp>

#include <fstream>
#include <iterator>
#include <unordered_map>

namespace zxconfig {
namespace {

Result<void> check_export_names(const Runtime& runtime, std::string_view prefix = {}) {
    std::unordered_map<std::string, std::string> claimed;
    for (const auto* symbol : runtime.declared_symbols()) {
        const auto claim = [&](std::string name) -> Result<void> {
            const auto [it, inserted] = claimed.emplace(std::move(name), symbol->name);
            if (!inserted) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::NamespaceCollision,
                    "'" + symbol->name + "' and '" + it->second
                        + "' both export '" + it->first + "'", symbol->span));
            }
            return {};
        };
        if (auto result = claim(macro_name(*symbol, prefix)); !result) return result;
        if (symbol->type == ValueType::Tristate) {
            if (auto result = claim(macro_name(*symbol, prefix) + "_MODULE"); !result) return result;
        }
        if (symbol->type == ValueType::Enum || symbol->type == ValueType::Set) {
            for (const auto& item : symbol->meta.choices) {
                if (auto result = claim(macro_name(*symbol, prefix, item)); !result) return result;
            }
        }
    }
    return {};
}

/// @brief Quote @p value as a C string literal.
std::string c_string(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F || c >= 0x80) {
                    static constexpr char k_digits[] = "0123456789abcdef";
                    out += "\\x";
                    out.push_back(k_digits[c >> 4]);
                    out.push_back(k_digits[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

void not_set(std::string& out, const std::string& name) {
    out += "/* ";
    out += name;
    out += " is not set */\n";
}

void define(std::string& out, const std::string& name, std::string_view value) {
    out += "#define ";
    out += name;
    out += ' ';
    out += value;
    out += '\n';
}

/// @brief Whether the resolved value of @p symbol is "on" at @p level.
bool level_at(const EvalValue& value, std::int64_t level) {
    return truth_level(value) == level;
}

/// @brief Escape a value for use in a make assignment: `$` and `#` would
///        otherwise start a variable reference or a comment.
std::string make_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '$':  out += "$$"; break;
            case '#':  out += "\\#"; break;
            case '\n': out += ' '; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

/// @brief Quote @p value as a JSON string.
std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    static constexpr char k_digits[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(k_digits[c >> 4]);
                    out.push_back(k_digits[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

/// @brief Quote @p value as a CMake quoted argument.
std::string cmake_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::string macro_name(const Symbol& symbol, std::string_view prefix) {
    // Export names are intentionally flat.  The canonical namespace remains
    // in symbol.name for expressions, caches, and diagnostics; generated
    // interfaces use the declaration spelling and reject duplicate exports.
    return std::string{prefix} + symbol.raw_name;
}

std::string macro_name(const Symbol& symbol, std::string_view prefix, std::string_view item) {
    return macro_name(symbol, prefix) + "_" + std::string{item};
}

Result<std::string> render_header(const Runtime& runtime,
                                  const GenerateDirective& directive,
                                  std::string_view engine) {
    if (directive.backend != GenerateDirective::Backend::Header) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::UnknownBackend, "render_header only emits header directives",
            directive.span));
    }
    const std::string_view prefix = directive.prefix;

    const auto symbols = runtime.declared_symbols();

    if (auto checked = check_export_names(runtime, prefix); !checked) {
        return std::unexpected(checked.error());
    }

    std::string out;
    out += "/* generated by zxconfig ";
    out += engine;
    out += "; do not edit */\n";

    for (const auto* symbol : symbols) {
        const auto name = macro_name(*symbol, prefix);
        const auto* value = runtime.value(symbol->name);
        const EvalValue unset{std::monostate{}};
        const EvalValue& current = value != nullptr ? *value : unset;

        switch (symbol->type) {
            case ValueType::Bool: {
                if (level_at(current, 2)) define(out, name, "1");
                else not_set(out, name);
                break;
            }
            case ValueType::Tristate: {
                const auto level = truth_level(current);
                if (level == 2) {
                    define(out, name, "1");
                } else if (level == 1) {
                    define(out, name + "_MODULE", "1");
                } else {
                    not_set(out, name);
                }
                break;
            }
            case ValueType::Int:
            case ValueType::Uint: {
                if (std::holds_alternative<std::int64_t>(current)) {
                    define(out, name, render_value(current, ValueType::Int));
                } else {
                    not_set(out, name);
                }
                break;
            }
            case ValueType::Hex: {
                if (std::holds_alternative<std::int64_t>(current)) {
                    define(out, name, render_value(current, ValueType::Hex));
                } else {
                    not_set(out, name);
                }
                break;
            }
            case ValueType::String:
            case ValueType::Path: {
                if (std::holds_alternative<std::string>(current)) {
                    define(out, name, c_string(std::get<std::string>(current)));
                } else {
                    not_set(out, name);
                }
                break;
            }
            case ValueType::List: {
                if (const auto* items = std::get_if<std::vector<std::string>>(&current)) {
                    std::string joined;
                    for (std::size_t i = 0; i < items->size(); ++i) {
                        if (i != 0) joined += ' ';
                        joined += (*items)[i];
                    }
                    define(out, name, c_string(joined));
                } else {
                    not_set(out, name);
                }
                break;
            }
            case ValueType::Enum: {
                const auto* selected =
                    std::get_if<std::string>(&current);
                if (selected != nullptr) define(out, name, c_string(*selected));
                for (const auto& item : symbol->meta.choices) {
                    const auto choice_name = macro_name(*symbol, prefix, item);
                    const bool on = selected != nullptr && *selected == item;
                    define(out, choice_name, on ? "1" : "0");
                }
                if (selected != nullptr) {
                    out += "/* " + name + " = " + *selected + " */\n";
                }
                break;
            }
            case ValueType::Set: {
                const auto* members = std::get_if<std::vector<std::string>>(&current);
                for (const auto& item : symbol->meta.choices) {
                    const auto item_name = macro_name(*symbol, prefix, item);
                    const bool on = members != nullptr
                        && std::find(members->begin(), members->end(), item) != members->end();
                    define(out, item_name, on ? "1" : "0");
                }
                break;
            }
        }
    }
    return out;
}

Result<void> generate_header(const Runtime& runtime,
                             const GenerateDirective& directive,
                             std::string_view engine,
                             std::string_view base) {
    auto rendered = render_header(runtime, directive, engine);
    if (!rendered) return std::unexpected(rendered.error());

    std::filesystem::path output{directive.output};
    if (output.is_relative() && !base.empty()) {
        output = std::filesystem::path{std::string{base}} / output;
    }

    std::error_code ec;
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path(), ec);
        if (ec) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::WriteFailed,
                "cannot create '" + output.parent_path().string() + "': " + ec.message(),
                directive.span));
        }
    }

    std::ofstream out{output, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::WriteFailed,
            "cannot create '" + output.string() + "'", directive.span));
    }
    out.write(rendered->data(), static_cast<std::streamsize>(rendered->size()));
    if (!out) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::WriteFailed,
            "error writing '" + output.string() + "'", directive.span));
    }
    return {};
}

Result<std::string> render_makefile(const Runtime& runtime,
                                     std::string_view engine) {
    const auto symbols = runtime.declared_symbols();
    if (auto checked = check_export_names(runtime); !checked) return std::unexpected(checked.error());

    std::string out;
    out += "# generated by zxconfig ";
    out += engine;
    out += "; do not edit\n";

    for (const auto* symbol : symbols) {
        const auto name = macro_name(*symbol, "");
        const auto* value = runtime.value(symbol->name);
        const EvalValue unset{std::monostate{}};
        const EvalValue& current = value != nullptr ? *value : unset;

        switch (symbol->type) {
            case ValueType::Bool:
                if (level_at(current, 2)) out += name + "=y\n";
                else out += "# " + name + " is not set\n";
                break;
            case ValueType::Tristate: {
                const auto level = truth_level(current);
                if (level == 2) out += name + "=y\n";
                else if (level == 1) out += name + "=m\n";
                else out += "# " + name + " is not set\n";
                break;
            }
            case ValueType::Enum: {
                const auto* selected = std::get_if<std::string>(&current);
                if (selected != nullptr) {
                    out += name + "=" + make_escape(*selected) + "\n";
                } else {
                    out += "# " + name + " is not set\n";
                }
                break;
            }
            case ValueType::Set: {
                const auto* members = std::get_if<std::vector<std::string>>(&current);
                if (members != nullptr) {
                    std::string joined;
                    for (std::size_t i = 0; i < members->size(); ++i) {
                        if (i != 0) joined += ' ';
                        joined += make_escape((*members)[i]);
                    }
                    out += name + "=" + joined + "\n";
                } else {
                    out += "# " + name + " is not set\n";
                }
                break;
            }
            case ValueType::List: {
                const auto* items = std::get_if<std::vector<std::string>>(&current);
                if (items != nullptr) {
                    std::string joined;
                    for (std::size_t i = 0; i < items->size(); ++i) {
                        if (i != 0) joined += ' ';
                        joined += make_escape((*items)[i]);
                    }
                    out += name + "=" + joined + "\n";
                } else {
                    out += "# " + name + " is not set\n";
                }
                break;
            }
            case ValueType::Int:
            case ValueType::Uint:
            case ValueType::Hex: {
                if (std::holds_alternative<std::int64_t>(current)) {
                    out += name + "=" + render_value(current, symbol->type) + "\n";
                } else {
                    out += "# " + name + " is not set\n";
                }
                break;
            }
            case ValueType::String:
            case ValueType::Path: {
                if (const auto* text = std::get_if<std::string>(&current)) {
                    out += name + "=" + make_escape(*text) + "\n";
                } else {
                    out += "# " + name + " is not set\n";
                }
                break;
            }
        }
    }
    return out;
}

Result<std::string> render_json(const Runtime& runtime, std::string_view engine) {
    const auto symbols = runtime.declared_symbols();
    if (auto checked = check_export_names(runtime); !checked) return std::unexpected(checked.error());

    std::string out;
    out += "{\n";
    out += "  \"format\": 1,\n";
    out += "  \"generator\": " + json_escape(std::string{"zxconfig "} + std::string{engine}) + ",\n";
    out += "  \"symbols\": {\n";

    for (std::size_t i = 0; i < symbols.size(); ++i) {
        const auto* symbol = symbols[i];
        const auto* value = runtime.value(symbol->name);
        const EvalValue unset{std::monostate{}};
        const EvalValue& current = value != nullptr ? *value : unset;

        std::string rendered;
        switch (symbol->type) {
            case ValueType::Bool:
                rendered = std::holds_alternative<std::monostate>(current)
                    ? "null" : (truth_level(current) != 0 ? "true" : "false");
                break;
            case ValueType::Tristate: {
                if (std::holds_alternative<std::monostate>(current)) {
                    rendered = "null";
                } else {
                    rendered = json_escape(render_value(current, ValueType::Tristate));
                }
                break;
            }
            case ValueType::Int:
            case ValueType::Uint:
            case ValueType::Hex:
                rendered = std::holds_alternative<std::int64_t>(current)
                    ? render_value(current, ValueType::Int) : "null";
                break;
            case ValueType::String:
            case ValueType::Path:
            case ValueType::Enum: {
                const auto* text = std::get_if<std::string>(&current);
                rendered = text != nullptr ? json_escape(*text) : "null";
                break;
            }
            case ValueType::Set:
            case ValueType::List: {
                const auto* items = std::get_if<std::vector<std::string>>(&current);
                if (items == nullptr) {
                    rendered = "null";
                    break;
                }
                rendered = "[";
                for (std::size_t k = 0; k < items->size(); ++k) {
                    if (k != 0) rendered += ", ";
                    rendered += json_escape((*items)[k]);
                }
                rendered += "]";
                break;
            }
        }

        out += "    " + json_escape(symbol->raw_name) + ": {\"type\": "
             + json_escape(to_string(symbol->type)) + ", \"value\": " + rendered + "}";
        out += i + 1 < symbols.size() ? ",\n" : "\n";
    }
    out += "  }\n";
    out += "}\n";
    return out;
}

Result<std::string> render_cmake(const Runtime& runtime, std::string_view engine) {
    const auto symbols = runtime.declared_symbols();
    if (auto checked = check_export_names(runtime); !checked) return std::unexpected(checked.error());

    std::string out;
    out += "# generated by zxconfig ";
    out += engine;
    out += "; do not edit\n";

    for (const auto* symbol : symbols) {
        const auto name = macro_name(*symbol, "");
        const auto* value = runtime.value(symbol->name);
        const EvalValue unset{std::monostate{}};
        const EvalValue& current = value != nullptr ? *value : unset;

        switch (symbol->type) {
            case ValueType::Bool:
                out += "set(" + name + " " + (level_at(current, 2) ? "ON" : "OFF") + ")\n";
                break;
            case ValueType::Tristate: {
                const auto level = truth_level(current);
                if (level == 2) out += "set(" + name + " ON)\n";
                else if (level == 1) out += "set(" + name + " \"m\")\n";
                else out += "set(" + name + " OFF)\n";
                break;
            }
            case ValueType::Int:
            case ValueType::Uint:
            case ValueType::Hex:
                out += "set(" + name + " "
                     + (std::holds_alternative<std::int64_t>(current)
                            ? render_value(current, symbol->type) : "\"\"")
                     + ")\n";
                break;
            case ValueType::String:
            case ValueType::Path:
            case ValueType::Enum: {
                const auto* text = std::get_if<std::string>(&current);
                out += "set(" + name + " "
                     + (text != nullptr ? cmake_quote(*text) : "\"\"") + ")\n";
                break;
            }
            case ValueType::Set:
            case ValueType::List: {
                const auto* items = std::get_if<std::vector<std::string>>(&current);
                std::string joined;
                if (items != nullptr) {
                    for (std::size_t i = 0; i < items->size(); ++i) {
                        if (i != 0) joined += ' ';
                        joined += (*items)[i];
                    }
                }
                out += "set(" + name + " " + cmake_quote(joined) + ")\n";
                break;
            }
        }
    }
    return out;
}

Result<std::string> render_template(const Runtime& runtime, std::string_view template_text) {
    std::string out;
    out.reserve(template_text.size());
    std::size_t at = 0;
    while (at < template_text.size()) {
        if (template_text[at] == '$') {
            if (at + 1 < template_text.size() && template_text[at + 1] == '$') {
                out.push_back('$');
                at += 2;
                continue;
            }
            if (at + 1 < template_text.size() && template_text[at + 1] == '{') {
                const auto close = template_text.find('}', at + 2);
                if (close == std::string_view::npos) {
                    return std::unexpected(Diagnostic::error(
                        ErrorCode::Interpolation,
                        "unclosed '${' at byte " + std::to_string(at)));
                }
                auto value = runtime.expand_text(template_text.substr(at, close - at + 1));
                if (!value) return std::unexpected(value.error());
                out += *value;
                at = close + 1;
                continue;
            }
        }
        out.push_back(template_text[at++]);
    }
    return out;
}

std::size_t run_generators(const Document& document,
                           const Runtime& runtime,
                           std::string_view engine,
                           std::string_view base,
                           std::vector<std::string>& failures) {
    std::size_t ok = 0;
    for (const auto& directive : document.generates) {
        auto output_name = runtime.expand_text(directive.output, {}, directive.span);
        if (!output_name) {
            failures.push_back(directive.output + ": " + output_name.error().message);
            continue;
        }
        Result<std::string> rendered = std::unexpected(Diagnostic::error(
            ErrorCode::UnknownBackend, "no backend", directive.span));
        std::filesystem::path template_path;

        switch (directive.backend) {
            case GenerateDirective::Backend::Header:
                rendered = render_header(runtime, directive, engine);
                break;
            case GenerateDirective::Backend::Makefile:
                rendered = render_makefile(runtime, engine);
                break;
            case GenerateDirective::Backend::Json:
                rendered = render_json(runtime, engine);
                break;
            case GenerateDirective::Backend::Cmake:
                rendered = render_cmake(runtime, engine);
                break;
            case GenerateDirective::Backend::Template: {
                auto template_name = runtime.expand_text(directive.template_path, {}, directive.span);
                if (!template_name) {
                    failures.push_back(directive.output + ": " + template_name.error().message);
                    continue;
                }
                template_path = std::filesystem::path{*template_name};
                if (template_path.is_relative() && !base.empty()) {
                    template_path = std::filesystem::path{std::string{base}} / template_path;
                }
                std::ifstream in{template_path, std::ios::binary};
                if (!in) {
                    failures.push_back(directive.output + ": cannot read template '"
                                       + template_path.string() + "'");
                    continue;
                }
                const std::string body{std::istreambuf_iterator<char>{in}, {}};
                rendered = render_template(runtime, body);
                break;
            }
        }

        if (!rendered) {
            failures.push_back(directive.output + ": " + rendered.error().message);
            continue;
        }

        std::filesystem::path output{*output_name};
        if (output.is_relative() && !base.empty()) {
            output = std::filesystem::path{std::string{base}} / output;
        }
        std::error_code ec;
        if (!output.parent_path().empty()) {
            std::filesystem::create_directories(output.parent_path(), ec);
            if (ec) {
                failures.push_back(directive.output + ": cannot create '"
                                   + output.parent_path().string() + "': " + ec.message());
                continue;
            }
        }
        std::ofstream out{output, std::ios::binary | std::ios::trunc};
        if (!out) {
            failures.push_back(directive.output + ": cannot create output");
            continue;
        }
        out.write(rendered->data(), static_cast<std::streamsize>(rendered->size()));
        if (!out) {
            failures.push_back(directive.output + ": error writing output");
            continue;
        }
        ++ok;
    }
    return ok;
}

}  // namespace zxconfig
