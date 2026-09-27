// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/fuzzy.hpp>

#include <optional>

namespace zxconfig {
namespace {

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool boundary(std::string_view text, std::size_t index) noexcept {
    if (index == 0) return true;
    const char previous = text[index - 1];
    return previous == '_' || previous == '.' || previous == ' '
        || (previous >= '0' && previous <= '9');
}

}  // namespace

bool fuzzy_match(std::string_view text, std::string_view pattern, int& score) {
    if (pattern.empty()) {
        score = 0;
        return true;
    }

    constexpr int k_first = 16;
    constexpr int k_consecutive = 8;
    constexpr int k_boundary = 6;
    constexpr int k_plain = 1;
    constexpr int k_gap_cap = 3;

    auto greedy_from = [&](std::size_t start) -> std::optional<int> {
        int total = 0;
        std::size_t at = start;
        std::size_t previous_hit = start;
        bool first = true;

        for (const char wanted : pattern) {
            const char w = lower(wanted);
            bool found = false;
            while (at < text.size()) {
                if (lower(text[at]) == w) {
                    if (first) {
                        total += k_first;
                        if (boundary(text, at)) total += k_boundary;
                        first = false;
                    } else if (at == previous_hit + 1) {
                        total += k_consecutive;
                    } else {
                        total -= static_cast<int>(std::min(at - previous_hit - 1,
                                                           std::size_t{k_gap_cap}));
                        total += boundary(text, at) ? k_boundary : k_plain;
                    }
                    if (wanted >= 'A' && wanted <= 'Z' && text[at] == wanted) total += 4;
                    previous_hit = at++;
                    found = true;
                    break;
                }
                ++at;
            }
            if (!found) return std::nullopt;
        }
        return total;
    };

    int best = 0;
    bool matched = false;
    for (std::size_t start = 0; start < text.size(); ++start) {
        if (lower(text[start]) != lower(pattern.front())) continue;
        auto score = greedy_from(start);
        if (score.has_value() && (!matched || *score > best)) {
            best = *score;
            matched = true;
        }
    }
    if (!matched) return false;

    score = best;
    return true;
}

}  // namespace zxconfig
