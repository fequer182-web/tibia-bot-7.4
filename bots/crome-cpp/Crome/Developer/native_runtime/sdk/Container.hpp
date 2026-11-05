#pragma once
#include "Item.hpp"
#include <cstddef>
#include <string>
#include <vector>
namespace crome {
struct Container {
    std::size_t index{};
    std::string name;
    std::size_t capacity{};
    std::vector<Item> items;
    [[nodiscard]] bool full() const noexcept { return capacity && items.size() >= capacity; }
};
}
