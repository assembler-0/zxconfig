// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/token.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace zxconfig {

/// @brief The declared type of a symbol, exactly as the language spells it.
enum class ValueType : std::uint8_t {
    Bool,
    Tristate,
    Int,
    Uint,
    Hex,
    String,
    Path,
    Enum,
    Set,
    List,
};

/// @brief The canonical spelling of @p type.  Matches the language keyword.
[[nodiscard]] constexpr const char* to_string(ValueType type) noexcept {
    switch (type) {
        case ValueType::Bool:     return "bool";
        case ValueType::Tristate: return "tristate";
        case ValueType::Int:      return "int";
        case ValueType::Uint:     return "uint";
        case ValueType::Hex:      return "hex";
        case ValueType::String:   return "string";
        case ValueType::Path:     return "path";
        case ValueType::Enum:     return "enum";
        case ValueType::Set:      return "set";
        case ValueType::List:     return "list";
    }
    return "string";
}

/// @brief Parse a type spelling.
/// @param[in] spelling  Exact match against the canonical spellings.
[[nodiscard]] constexpr std::optional<ValueType> parse_value_type(std::string_view spelling) noexcept {
    if (spelling == "bool")     return ValueType::Bool;
    if (spelling == "tristate") return ValueType::Tristate;
    if (spelling == "int")      return ValueType::Int;
    if (spelling == "uint")     return ValueType::Uint;
    if (spelling == "hex")      return ValueType::Hex;
    if (spelling == "string")   return ValueType::String;
    if (spelling == "path")     return ValueType::Path;
    if (spelling == "enum")     return ValueType::Enum;
    if (spelling == "set")      return ValueType::Set;
    if (spelling == "list")     return ValueType::List;
    return std::nullopt;
}

}  // namespace zxconfig
