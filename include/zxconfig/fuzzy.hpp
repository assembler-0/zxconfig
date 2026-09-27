// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

namespace zxconfig {

/// @brief Case-insensitive subsequence match with a quality score.
/// @param[in]  text    Candidate text.
/// @param[in]  pattern Query.  An empty pattern matches everything with 0.
/// @param[out] score   Match quality; untouched when the match fails.
/// @return True when @p pattern is a subsequence of @p text.
bool fuzzy_match(std::string_view text, std::string_view pattern, int& score);

}  // namespace zxconfig
