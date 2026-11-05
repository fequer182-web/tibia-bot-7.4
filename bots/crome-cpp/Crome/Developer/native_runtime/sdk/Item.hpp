#pragma once
#include <cstdint>
namespace crome {
struct Item {
    std::uint32_t id{};
    std::uint32_t subtype{};
    std::uint32_t count{1};
};
}
