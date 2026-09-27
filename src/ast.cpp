// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/ast.hpp>

#include <algorithm>
#include <numeric>

namespace zxconfig {

std::vector<std::size_t> ordered_children(const Menu& menu) {
    std::vector<std::size_t> order(menu.children.size());
    std::iota(order.begin(), order.end(), std::size_t{0});

    std::ranges::stable_sort(order,
                             [&menu](std::size_t a, std::size_t b) {
                                 const auto key = [&menu](std::size_t index) -> std::int64_t {
                                     const auto* symbol = std::get_if<Symbol>(&menu.children[index]);
                                     return symbol != nullptr && symbol->meta.order.has_value()
                                                ? *symbol->meta.order : 0;
                                 };
                                 return key(a) < key(b);
                             });
    return order;
}

}  // namespace zxconfig
