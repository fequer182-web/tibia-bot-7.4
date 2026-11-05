#pragma once
#include "../sdk/Container.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace crome {
struct LootRule {
    std::vector<std::uint32_t> itemIds;
    bool contains(std::uint32_t id) const { return std::find(itemIds.begin(), itemIds.end(), id) != itemIds.end(); }
};

class ContainerManager {
public:
    void setOpenContainers(std::vector<Container> c) { containers_ = std::move(c); }
    [[nodiscard]] const std::vector<Container>& containers() const noexcept { return containers_; }
    [[nodiscard]] const Container* firstWithEmptySlots() const noexcept {
        for (const auto& c : containers_) if (!c.full()) return &c;
        return nullptr;
    }
private:
    std::vector<Container> containers_;
};

class Looter {
public:
    explicit Looter(LootRule rule = {}) : rule_(std::move(rule)) {}
    [[nodiscard]] std::vector<Item> filter(const Container& corpse) const {
        std::vector<Item> out;
        for (const auto& i : corpse.items) if (rule_.contains(i.id)) out.push_back(i);
        return out;
    }
private:
    LootRule rule_;
};
}
