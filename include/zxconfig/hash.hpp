// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include <string>
#include <string_view>

namespace zxconfig {

/// @brief SHA-256 of @p bytes, as 64 lowercase hex digits.
///
/// Lives outside the cache module because it is general-purpose: the cache
/// uses it for manifest digests, and anything else that needs a content
/// fingerprint should not depend on the cache to get one.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

/// @brief Digest of a set of text parts.
///
/// Each part is hashed followed by one NUL byte, so a two-part digest
/// ("ab", "c") differs from a one-part digest ("ab\0c") and no boundary is
/// ambiguous.  Parts must arrive in a stable order when the digest is meant
/// to stand for a set of files: load order is the convention.
///
/// @param[in] parts  The parts to digest.
[[nodiscard]] std::string digest_parts(std::span<const std::string_view> parts);

}  // namespace zxconfig
