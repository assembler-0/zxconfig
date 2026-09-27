// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zxconfig {

/// @brief Outcome of decoding a string body.
struct DecodedText {
    std::string value;                    ///< decoded text; a prefix when failed
    std::optional<std::uint32_t> error;   ///< offset of the offending backslash

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

/// @brief Decode the body of a quoted string: escapes only, no quotes.
/// @param[in] raw  Bytes between the quotes.
[[nodiscard]] DecodedText decode_escapes(std::string_view raw);

}  // namespace zxconfig
